/* Android/Wine telemetry: battery power and temperature, GPU clock/busy, CPU cluster
 * clocks, thermal zones, and the Linux view of every game thread (CPU time, wake-ups,
 * which core cluster it runs on). Read from /sys and /proc through Wine's unix path
 * mapping, so A/B tests get objective power numbers in the log.
 * Every source is optional: whatever SELinux does not allow is reported and skipped. */
#include "mcpu.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef WCHAR* (CDECL* wine_get_dos_file_name_t)(const char*);
static wine_get_dos_file_name_t p_dos;

typedef struct Src {
    WCHAR* path;
    double sum;
    LONG64 n;
} Src;

static Src s_bat_cur, s_bat_volt, s_bat_temp, s_gpu_clk, s_gpu_busy;
static WCHAR* s_bat_status;
static int s_gpu_clk_div; /* divisor to MHz */

#define MAX_CPU 16
static Src s_cpu[MAX_CPU];
static long long s_cpu_max[MAX_CPU];
static int s_ncpu;

#define MAX_TZ 6
static Src s_tz[MAX_TZ];
static char s_tz_name[MAX_TZ][32];
static int s_ntz;

static WCHAR* s_task_dir;
static SRWLOCK s_lock = SRWLOCK_INIT;

/* per-thread Linux view */
#define MAX_TASK 256
typedef struct Task {
    int tid;
    char comm[20];
    long long ticks, prev_ticks;
    long long ctx, prev_ctx;
    int seen;
    int hot;               /* sampled for core placement */
    LONG64 on_cluster[4];  /* placement samples per cluster */
    uint64_t allowed;      /* Linux CPU affinity (Cpus_allowed_list), 0 = unknown */
} Task;
static Task s_task[MAX_TASK];
static int s_ntask;
static int s_cluster_of_cpu[MAX_CPU];
static int s_nclusters;
static long long s_cluster_max[4];
static int s_cluster_first[4], s_cluster_last[4];

static WCHAR* to_dos(const char* unix_path)
{
    return p_dos ? p_dos(unix_path) : NULL;
}

static int read_text(const WCHAR* path, char* buf, int cap)
{
    if (!path)
        return -1;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)cap - 1, &got, NULL);
    CloseHandle(h);
    if (!ok)
        return -1;
    buf[got] = 0;
    return (int)got;
}

static int read_ll(const WCHAR* path, long long* v)
{
    char b[64];
    if (read_text(path, b, sizeof(b)) <= 0)
        return 0;
    char* end;
    *v = strtoll(b, &end, 10);
    return end != b;
}

/* Opens `unix_path`; keeps it only if it can be read now. */
static WCHAR* probe(const char* unix_path)
{
    WCHAR* p = to_dos(unix_path);
    long long v;
    if (p && read_ll(p, &v))
        return p;
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
    return NULL;
}

static void sample_src(Src* s, double scale)
{
    long long v;
    if (s->path && read_ll(s->path, &v)) {
        s->sum += (double)v * scale;
        s->n++;
    }
}

/* Average of the window's samples; returns 0 when there were none. (Values can be
 * negative: many devices report the discharge current as a negative number.) */
static int take_avg(Src* s, double* out)
{
    int ok = s->n > 0;
    if (ok)
        *out = s->sum / (double)s->n;
    s->sum = 0;
    s->n = 0;
    return ok;
}

/* ---- /proc/self/task */
static Task* task_get(int tid, int* fresh)
{
    *fresh = 0;
    for (int i = 0; i < s_ntask; i++)
        if (s_task[i].tid == tid)
            return &s_task[i];
    Task* t = NULL;
    if (s_ntask < MAX_TASK) {
        t = &s_task[s_ntask++];
    } else {
        for (int i = 0; i < s_ntask && !t; i++)
            if (!s_task[i].seen)
                t = &s_task[i]; /* reuse the slot of an exited thread */
    }
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));
    t->tid = tid;
    *fresh = 1;
    return t;
}

/* stat: "tid (comm) S f4 ... f14=utime f15=stime ... f20=num_threads ... f39=processor" */
static int parse_stat(const char* buf, char* comm, size_t comm_cap, long long* ticks, int* cpu, int* threads)
{
    const char* l = strchr(buf, '(');
    const char* r = strrchr(buf, ')');
    if (!l || !r || r < l)
        return 0;
    size_t n = (size_t)(r - l - 1);
    if (n >= comm_cap)
        n = comm_cap - 1;
    memcpy(comm, l + 1, n);
    comm[n] = 0;
    const char* p = r + 1; /* fields after ")" start at 3 (state letter) */
    int field = 3;
    long long utime = 0, stime = 0, proc = -1, nthr = 0;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char* tok = p;
        while (*p && *p != ' ')
            p++;
        if (field == 14)
            utime = strtoll(tok, NULL, 10);
        else if (field == 15)
            stime = strtoll(tok, NULL, 10);
        else if (field == 20)
            nthr = strtoll(tok, NULL, 10);
        else if (field == 39) {
            proc = strtoll(tok, NULL, 10);
            break;
        }
        field++;
    }
    *ticks = utime + stime;
    *cpu = (int)proc;
    if (threads)
        *threads = (int)nthr;
    return 1;
}

/* "0-5,7" -> bit mask */
static uint64_t parse_cpu_list(const char* s)
{
    uint64_t m = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s >= '0' && *s <= '9') {
        char* e;
        long a = strtol(s, &e, 10), b = a;
        if (*e == '-')
            b = strtol(e + 1, &e, 10);
        for (long i = a; i <= b && i < 64; i++)
            m |= (uint64_t)1 << i;
        s = e;
        if (*s != ',')
            break;
        s++;
    }
    return m;
}

static uint64_t parse_allowed(const char* status)
{
    const char* p = strstr(status, "Cpus_allowed_list:");
    return p ? parse_cpu_list(p + 18) : 0;
}

static void format_cpu_list(uint64_t m, char* out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 64 && n < cap;) {
        if (!(m >> i & 1)) {
            i++;
            continue;
        }
        int j = i;
        while (j + 1 < 64 && (m >> (j + 1) & 1))
            j++;
        n += (size_t)snprintf(out + n, cap - n, j > i ? "%s%d-%d" : "%s%d", n ? "," : "", i, j);
        i = j + 1;
    }
}

static int popcount64(uint64_t m)
{
    int n = 0;
    for (; m; m &= m - 1)
        n++;
    return n;
}

static long long parse_ctx(const char* buf)
{
    long long total = 0;
    const char* p = buf;
    while ((p = strstr(p, "ctxt_switches:")) != NULL) {
        p += 14;
        total += strtoll(p, NULL, 10);
    }
    return total;
}

/* The game's Linux pid. "/proc/self" cannot be used: Wine opens files from the
 * wineserver process, so "self" would be the wineserver. */
static int s_pid;
static int s_id_tries;
static uint64_t s_online;       /* CPUs the system has online (/sys/devices/system/cpu/online) */
static uint64_t s_allowed_seen; /* last allowed-core set reported */

static void task_path(int tid, const char* leaf, char* out, size_t cap)
{
    snprintf(out, cap, "/proc/%d/task/%d/%s", s_pid, tid, leaf);
}

static void lower(char* s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s += 32;
}

/* ---- which Linux process is the game */
typedef struct Cand {
    int pid, threads, name_match;
    long long ticks;
    char comm[20];
} Cand;

/* The game is the process whose CPU time equals what Windows reports for this process
 * (Wine reads the same kernel counters) and that has at least about as many threads as
 * Windows sees; a name match only breaks ties. Names alone fail under Winlator: the
 * explorer.exe that hosts the desktop carries the game's path on its command line. */
static int pick_game(const Cand* c, int n, long long ours, long long slack, int win_threads)
{
    long long tol = ours / 20 + slack;
    if (tol < 30)
        tol = 30;
    int best = -1;
    long long best_d = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].threads < win_threads * 3 / 4)
            continue;
        long long d = c[i].ticks > ours ? c[i].ticks - ours : ours - c[i].ticks;
        if (d > tol)
            continue;
        if (best < 0 || c[i].name_match > c[best].name_match || (c[i].name_match == c[best].name_match && d < best_d))
            best = i, best_d = d;
    }
    return best;
}

/* this process's CPU time in Linux clock ticks (USER_HZ = 100) */
static long long own_ticks(void)
{
    FILETIME a, b, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u))
        return -1;
    ULARGE_INTEGER kk, uu;
    kk.LowPart = k.dwLowDateTime, kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime, uu.HighPart = u.dwHighDateTime;
    return (long long)((kk.QuadPart + uu.QuadPart) / 100000);
}

static int windows_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int n = 0;
    DWORD me = GetCurrentProcessId();
    if (Thread32First(snap, &te))
        do {
            n += te.th32OwnerProcessID == me;
        } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return n;
}

#define MAX_CAND 160
static Cand s_cand[MAX_CAND];

static int list_processes(Cand* c, int cap)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    const char* base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    char want[64];
    snprintf(want, sizeof(want), "%s", base);
    lower(want);

    WCHAR* proc = to_dos("/proc");
    if (!proc)
        return 0;
    WCHAR pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", proc);
    pattern[MAX_PATH - 1] = 0;
    HeapFree(GetProcessHeap(), 0, proc);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do {
        int pid = _wtoi(fd.cFileName);
        if (pid <= 0 || n >= cap)
            continue;
        char up[64], buf[1024], low[20];
        snprintf(up, sizeof(up), "/proc/%d/stat", pid);
        WCHAR* p = to_dos(up);
        int cpu;
        Cand* x = &c[n];
        if (read_text(p, buf, sizeof(buf)) > 0 &&
            parse_stat(buf, x->comm, sizeof(x->comm), &x->ticks, &cpu, &x->threads)) {
            x->pid = pid;
            snprintf(low, sizeof(low), "%s", x->comm);
            lower(low);
            x->name_match = strlen(low) >= 4 && strncmp(want, low, strlen(low)) == 0; /* comm: first 15 chars */
            n++;
        }
        if (p)
            HeapFree(GetProcessHeap(), 0, p);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* Privacy: the log may be shared. Other processes are named only when they are the emulator's own: Windows
 * programs under Wine and the Wine / translation-layer processes. Anything else (on desktop Linux every
 * program of the user is visible) is only counted. */
static const char* log_name(const char* comm)
{
    char low[20];
    snprintf(low, sizeof(low), "%s", comm);
    lower(low);
    static const char* const keep[] = {".exe", "wine", "box64", "box86", "fex", "proot", "winedevice", "wineserver"};
    for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
        if (strstr(low, keep[i]))
            return comm;
    return "(another program)";
}

static int cmp_cand(const void* a, const void* b)
{
    long long x = ((const Cand*)a)->ticks, y = ((const Cand*)b)->ticks;
    return (x < y) - (x > y);
}

/* Tries to find the game's Linux process; 1 when done (found, or given up). Needs a few
 * seconds of game CPU time so the fingerprint is distinctive. */
static int identify_game(void)
{
    long long t0 = own_ticks();
    if (t0 < 200 && ++s_id_tries < 12) /* ~2 min without CPU time from Windows: try anyway, then log */
        return 0;
    int wt = windows_threads();
    int n = list_processes(s_cand, MAX_CAND);
    long long t1 = own_ticks();
    int i = pick_game(s_cand, n, (t0 + t1) / 2, (t1 - t0) / 2 + 2, wt);
    if (i >= 0) {
        s_pid = s_cand[i].pid;
        char up[48];
        snprintf(up, sizeof(up), "/proc/%d/task", s_pid);
        s_task_dir = to_dos(up);
        LOGI("Telemetry: game process is Linux pid %d ('%s', %d threads, CPU %.1f s; Windows reports %.1f s and %d "
             "threads)",
             s_pid, s_cand[i].comm, s_cand[i].threads, s_cand[i].ticks / 100.0, t1 / 100.0, wt);
        return 1;
    }
    if (++s_id_tries < 6 && n > 0)
        return 0;
    qsort(s_cand, n, sizeof(s_cand[0]), cmp_cand);
    char line[600];
    int k = 0;
    for (int j = 0; j < n && j < 8 && k < (int)sizeof(line) - 60; j++)
        k += snprintf(line + k, sizeof(line) - k, "%s%d '%s' %d thr %.1f s", j ? ", " : "", s_cand[j].pid,
                      log_name(s_cand[j].comm), s_cand[j].threads, s_cand[j].ticks / 100.0);
    LOGI("Telemetry: game process not identified (Windows: %.1f s CPU, %d threads; %d Linux processes visible: %s); "
         "per-thread Linux view disabled",
         t1 / 100.0, wt, n, k ? line : "none");
    return 1;
}

static void read_task(Task* t, int with_ctx)
{
    char up[96], buf[1024];
    task_path(t->tid, "stat", up, sizeof(up));
    WCHAR* p = to_dos(up);
    int cpu = -1;
    long long ticks;
    if (read_text(p, buf, sizeof(buf)) > 0 && parse_stat(buf, t->comm, sizeof(t->comm), &ticks, &cpu, NULL)) {
        t->ticks = ticks;
        t->seen = 1;
        if (t->hot && cpu >= 0 && cpu < MAX_CPU)
            t->on_cluster[s_cluster_of_cpu[cpu]]++;
    }
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
    if (with_ctx) {
        /* status runs to ~1.5 KB; Cpus_allowed_list and the context-switch counters are at its end */
        char st[4096];
        task_path(t->tid, "status", up, sizeof(up));
        p = to_dos(up);
        if (read_text(p, st, sizeof(st)) > 0) {
            t->ctx = parse_ctx(st);
            t->allowed = parse_allowed(st);
        }
        if (p)
            HeapFree(GetProcessHeap(), 0, p);
    }
}

static void scan_tasks(void)
{
    if (!s_task_dir)
        return;
    WCHAR pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", s_task_dir);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    for (int i = 0; i < s_ntask; i++)
        s_task[i].seen = 0;
    do {
        int tid = _wtoi(fd.cFileName);
        if (tid <= 0)
            continue;
        int fresh;
        Task* t = task_get(tid, &fresh);
        if (!t)
            continue;
        read_task(t, 1); /* sets seen only if its stat file could be parsed */
        if (fresh) { /* count only what happens from now on */
            t->prev_ticks = t->ticks;
            t->prev_ctx = t->ctx;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static int cmp_task(const void* a, const void* b)
{
    const Task* x = *(const Task* const*)a;
    const Task* y = *(const Task* const*)b;
    long long dx = x->ticks - x->prev_ticks, dy = y->ticks - y->prev_ticks;
    return (dx < dy) - (dx > dy);
}

/* ---- sampling thread */
static DWORD WINAPI telemetry_thread(LPVOID unused)
{
    (void)unused;
    mcpu_register_own_thread(GetCurrentThreadId());
    for (;;) {
        Sleep((DWORD)g_cfg.telemetry_sample_ms);
        AcquireSRWLockExclusive(&s_lock);
        sample_src(&s_bat_cur, 1.0);
        sample_src(&s_bat_volt, 1.0);
        sample_src(&s_bat_temp, 1.0);
        sample_src(&s_gpu_clk, 1.0 / (s_gpu_clk_div ? s_gpu_clk_div : 1));
        sample_src(&s_gpu_busy, 1.0);
        for (int i = 0; i < s_ncpu; i++)
            sample_src(&s_cpu[i], 1.0);
        for (int i = 0; i < s_ntz; i++)
            sample_src(&s_tz[i], 1.0);
        for (int i = 0; i < s_ntask; i++)
            if (s_task[i].hot)
                read_task(&s_task[i], 0); /* core placement of the busiest threads */
        ReleaseSRWLockExclusive(&s_lock);
    }
    return 0;
}

static void setup_clusters(void)
{
    char up[96];
    for (int i = 0; i < s_ncpu; i++) {
        snprintf(up, sizeof(up), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        WCHAR* p = to_dos(up);
        if (!p || !read_ll(p, &s_cpu_max[i]))
            s_cpu_max[i] = 0;
        if (p)
            HeapFree(GetProcessHeap(), 0, p);
    }
    s_nclusters = 0;
    for (int i = 0; i < s_ncpu; i++) {
        int c = -1;
        for (int k = 0; k < s_nclusters; k++)
            if (s_cluster_max[k] == s_cpu_max[i])
                c = k;
        if (c < 0 && s_nclusters < 4) {
            c = s_nclusters++;
            s_cluster_max[c] = s_cpu_max[i];
            s_cluster_first[c] = i;
        }
        if (c < 0)
            c = s_nclusters - 1;
        s_cluster_of_cpu[i] = c;
        s_cluster_last[c] = i;
    }
}

void telemetry_start(void)
{
    if (!g_cfg.telemetry || !g_is_wine)
        return;
    p_dos = (wine_get_dos_file_name_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name");
    if (!p_dos) {
        LOGI("Telemetry: wine_get_dos_file_name not available; skipped");
        return;
    }

    s_bat_cur.path = probe("/sys/class/power_supply/battery/current_now");
    s_bat_volt.path = probe("/sys/class/power_supply/battery/voltage_now");
    s_bat_temp.path = probe("/sys/class/power_supply/battery/temp");
    s_bat_status = to_dos("/sys/class/power_supply/battery/status");

    static const struct { const char* path; int div; } gpu_clk[] = {
        {"/sys/class/kgsl/kgsl-3d0/gpuclk", 1000000},
        {"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", 1000000},
        {"/sys/kernel/gpu/gpu_clock", 1},
    };
    for (unsigned i = 0; i < sizeof(gpu_clk) / sizeof(gpu_clk[0]) && !s_gpu_clk.path; i++) {
        s_gpu_clk.path = probe(gpu_clk[i].path);
        s_gpu_clk_div = gpu_clk[i].div;
    }
    s_gpu_busy.path = probe("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
    if (!s_gpu_busy.path)
        s_gpu_busy.path = probe("/sys/kernel/gpu/gpu_busy");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s_ncpu = si.dwNumberOfProcessors < MAX_CPU ? (int)si.dwNumberOfProcessors : MAX_CPU;
    int cpu_ok = 0;
    for (int i = 0; i < s_ncpu; i++) {
        char up[96];
        snprintf(up, sizeof(up), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        s_cpu[i].path = probe(up);
        cpu_ok += s_cpu[i].path != NULL;
    }
    setup_clusters();

    for (int z = 0; z < 120 && s_ntz < MAX_TZ; z++) {
        char up[96], type[64];
        snprintf(up, sizeof(up), "/sys/class/thermal/thermal_zone%d/type", z);
        WCHAR* tp = to_dos(up);
        int got = read_text(tp, type, sizeof(type));
        if (tp)
            HeapFree(GetProcessHeap(), 0, tp);
        if (got <= 0)
            continue;
        type[strcspn(type, "\r\n")] = 0;
        if (!strstr(type, "skin") && !strstr(type, "shell") && !strstr(type, "gpu") && !strstr(type, "cpu") &&
            !strstr(type, "soc"))
            continue;
        /* only one zone per kind keeps the line readable */
        int dup = 0;
        for (int k = 0; k < s_ntz; k++)
            if (strncmp(s_tz_name[k], type, 3) == 0)
                dup = 1;
        if (dup)
            continue;
        snprintf(up, sizeof(up), "/sys/class/thermal/thermal_zone%d/temp", z);
        WCHAR* p = probe(up);
        if (p) {
            s_tz[s_ntz].path = p;
            snprintf(s_tz_name[s_ntz], sizeof(s_tz_name[0]), "%s", type);
            s_ntz++;
        }
    }

    {
        char buf[64];
        WCHAR* p = to_dos("/sys/devices/system/cpu/online");
        if (read_text(p, buf, sizeof(buf)) > 0)
            s_online = parse_cpu_list(buf);
        if (p)
            HeapFree(GetProcessHeap(), 0, p);
    }

    char cl[160];
    int n = 0;
    for (int c = 0; c < s_nclusters; c++)
        n += snprintf(cl + n, sizeof(cl) - n, "%scpu%d-%d max %.2f GHz", c ? ", " : "", s_cluster_first[c],
                      s_cluster_last[c], s_cluster_max[c] / 1e6);
    char online[64] = "?";
    if (s_online)
        format_cpu_list(s_online, online, sizeof(online));
    LOGI("Telemetry: battery current %s, voltage %s, temp %s | GPU clock %s, busy %s | CPU clocks %d/%d | "
         "thermal zones %d | CPUs online %s | game process: found by its CPU time at the first report",
         s_bat_cur.path ? "ok" : "no", s_bat_volt.path ? "ok" : "no", s_bat_temp.path ? "ok" : "no",
         s_gpu_clk.path ? "ok" : "no", s_gpu_busy.path ? "ok" : "no", cpu_ok, s_ncpu, s_ntz, online);
    if (s_nclusters)
        LOGI("Telemetry: clusters %s", cl);

    HANDLE h = CreateThread(NULL, 0, telemetry_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* ---- the load besides the game: the other processes the emulator's sandbox shows, and the whole
 * system's CPU time (/proc/stat; where Android denies it, each core's idle residency from cpuidle) */
#define MAX_OTHER 160
static struct {
    int pid;
    long long ticks;
} s_prev_proc[MAX_OTHER];
static int s_nprev_proc;
static int s_sys_mode;            /* 0 not probed, 1 /proc/stat, 2 cpuidle, -1 neither */
static WCHAR* s_stat_path;
static long long s_sys_busy, s_sys_total; /* /proc/stat, ticks */
#define MAX_IDLE_STATES 8
static WCHAR* s_idle_path[MAX_CPU][MAX_IDLE_STATES];
static long long s_idle_us[MAX_CPU];
static uint64_t s_idle_wall_us;

/* the first line of /proc/stat: busy and total ticks of all cores */
static int parse_proc_stat(const char* buf, long long* busy, long long* total)
{
    if (strncmp(buf, "cpu ", 4))
        return 0;
    long long v[10] = {0};
    char* p = (char*)buf + 4;
    for (int i = 0; i < 10; i++)
        v[i] = strtoll(p, &p, 10);
    *total = 0;
    for (int i = 0; i < 8; i++) /* user nice system idle iowait irq softirq steal */
        *total += v[i];
    *busy = *total - v[3] - v[4];
    return *total > 0;
}

static int proc_stat(long long* busy, long long* total)
{
    char buf[512];
    return read_text(s_stat_path, buf, sizeof(buf)) > 0 && parse_proc_stat(buf, busy, total);
}

static long long cpu_idle_us(int cpu)
{
    long long sum = 0, v;
    for (int s = 0; s < MAX_IDLE_STATES && s_idle_path[cpu][s]; s++)
        if (read_ll(s_idle_path[cpu][s], &v))
            sum += v;
    return sum;
}

static uint64_t now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / (double)f.QuadPart * 1e6);
}

static void sys_probe(void)
{
    s_stat_path = to_dos("/proc/stat");
    if (proc_stat(&s_sys_busy, &s_sys_total)) {
        s_sys_mode = 1;
        return;
    }
    int found = 0;
    for (int c = 0; c < s_ncpu; c++)
        for (int s = 0; s < MAX_IDLE_STATES; s++) {
            char up[96];
            snprintf(up, sizeof(up), "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time", c, s);
            if (!(s_idle_path[c][s] = probe(up)))
                break;
            found++;
        }
    s_sys_mode = found ? 2 : -1;
    for (int c = 0; c < s_ncpu; c++)
        s_idle_us[c] = cpu_idle_us(c);
    s_idle_wall_us = now_us();
}

/* busy cores over the window x 100 (% of one core), -1 if not known */
static double sys_busy_pct(void)
{
    if (s_sys_mode == 1) {
        long long b, t;
        if (!proc_stat(&b, &t) || t <= s_sys_total)
            return -1;
        double r = (double)(b - s_sys_busy) / (double)(t - s_sys_total) * s_ncpu * 100.0;
        s_sys_busy = b, s_sys_total = t;
        return r;
    }
    if (s_sys_mode == 2) {
        uint64_t w = now_us();
        double wall = (double)(w - s_idle_wall_us), busy = 0;
        for (int c = 0; c < s_ncpu; c++) {
            long long i = cpu_idle_us(c);
            if (wall > 0 && (!s_online || (s_online >> c & 1))) {
                double idle = (double)(i - s_idle_us[c]) / wall;
                busy += idle < 1.0 ? 1.0 - idle : 0.0;
            }
            s_idle_us[c] = i;
        }
        s_idle_wall_us = w;
        return wall > 0 ? busy * 100.0 : -1;
    }
    return -1;
}

static void report_other_load(double seconds, double game_pct)
{
    if (!s_sys_mode)
        sys_probe();
    double sys = sys_busy_pct();
    Cand* c = (Cand*)HeapAlloc(GetProcessHeap(), 0, sizeof(Cand) * MAX_OTHER);
    if (!c)
        return;
    int n = list_processes(c, MAX_OTHER);
    double other = 0, top[3] = {0};
    int ti[3] = {-1, -1, -1};
    for (int i = 0; i < n; i++) {
        long long prev = -1;
        for (int j = 0; j < s_nprev_proc; j++)
            if (s_prev_proc[j].pid == c[i].pid)
                prev = s_prev_proc[j].ticks;
        if (c[i].pid == s_pid || prev < 0 || c[i].ticks < prev)
            continue;
        double pct = (c[i].ticks - prev) * 100.0 / (seconds * 100.0);
        other += pct;
        for (int k = 0; k < 3; k++)
            if (pct > top[k]) {
                for (int m = 2; m > k; m--)
                    top[m] = top[m - 1], ti[m] = ti[m - 1];
                top[k] = pct, ti[k] = i;
                break;
            }
    }
    char line[400];
    int k = snprintf(line, sizeof(line), "  besides the game: other processes in the emulator %.0f%% of a core", other);
    for (int j = 0; j < 3 && ti[j] >= 0 && top[j] >= 1.0; j++)
        k += snprintf(line + k, sizeof(line) - (size_t)k, "%s%s %.0f%%", j ? ", " : " (", log_name(c[ti[j]].comm), top[j]);
    if (ti[0] >= 0 && top[0] >= 1.0)
        k += snprintf(line + k, sizeof(line) - (size_t)k, ")");
    if (sys >= 0)
        k += snprintf(line + k, sizeof(line) - (size_t)k, " | whole system %.0f%% of a core (%s)", sys,
                      s_sys_mode == 1 ? "/proc/stat" : "core idle times");
    if (sys >= 0 && game_pct >= 0)
        k += snprintf(line + k, sizeof(line) - (size_t)k, ", outside the game ~%.0f%%", sys - game_pct > 0 ? sys - game_pct : 0);
    LOGI("%s", line);
    s_nprev_proc = n < MAX_OTHER ? n : MAX_OTHER;
    for (int i = 0; i < s_nprev_proc; i++)
        s_prev_proc[i].pid = c[i].pid, s_prev_proc[i].ticks = c[i].ticks;
    HeapFree(GetProcessHeap(), 0, c);
}

void telemetry_report(double seconds)
{
    if (!p_dos)
        return;
    AcquireSRWLockExclusive(&s_lock);

    double cur = 0, volt = 0, temp = 0, gclk = 0, gbusy = 0;
    int has_cur = take_avg(&s_bat_cur, &cur), has_volt = take_avg(&s_bat_volt, &volt);
    int has_temp = take_avg(&s_bat_temp, &temp);
    int has_gclk = take_avg(&s_gpu_clk, &gclk), has_gbusy = take_avg(&s_gpu_busy, &gbusy);
    char status[32] = "";
    if (s_bat_status && read_text(s_bat_status, status, sizeof(status)) > 0)
        status[strcspn(status, "\r\n")] = 0;

    char line[512];
    int n = 0;
    if (has_cur && has_volt && volt > 0) {
        double ua = cur < 0 ? -cur : cur;
        if (ua < 20000)
            ua *= 1000; /* reported in mA */
        double uv = volt < 100000 ? volt * 1000 : volt;
        n += snprintf(line + n, sizeof(line) - n, "battery %.2f W%s", ua * uv / 1e12,
                      (strstr(status, "Charging") || strstr(status, "Full")) ? " (CHARGING: not comparable)" : "");
    }
    if (has_temp)
        n += snprintf(line + n, sizeof(line) - n, "%sbattery %.1f C", n ? " | " : "", temp / 10.0);
    if (has_gclk)
        n += snprintf(line + n, sizeof(line) - n, "%sGPU %.0f MHz", n ? " | " : "", gclk);
    if (has_gbusy)
        n += snprintf(line + n, sizeof(line) - n, "%s busy %.0f%%", n ? "," : "GPU", gbusy);
    for (int c = 0; c < s_nclusters; c++) {
        double sum = 0;
        int k = 0;
        for (int i = s_cluster_first[c]; i <= s_cluster_last[c]; i++) {
            double a;
            if (take_avg(&s_cpu[i], &a)) {
                sum += a;
                k++;
            }
        }
        if (k)
            n += snprintf(line + n, sizeof(line) - n, "%scpu%d-%d %.2f GHz", n ? " | " : "", s_cluster_first[c],
                          s_cluster_last[c], sum / k / 1e6);
    }
    for (int z = 0; z < s_ntz; z++) {
        double t;
        if (take_avg(&s_tz[z], &t))
            n += snprintf(line + n, sizeof(line) - n, "%s%s %.1f C", n ? " | " : "", s_tz_name[z],
                          t > 1000 ? t / 1000.0 : t);
    }
    if (n)
        LOGI("  device: %s", line);

    /* Linux view of the threads */
    if (!s_pid && s_id_tries >= 0 && identify_game()) {
        if (!s_pid)
            s_id_tries = -1; /* given up */
        scan_tasks();
        for (int i = 0; i < s_ntask; i++) /* the window starts now */
            s_task[i].prev_ticks = s_task[i].ticks, s_task[i].prev_ctx = s_task[i].ctx;
    }
    if (!s_pid) {
        report_other_load(seconds, -1);
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    scan_tasks();
    Task* order[MAX_TASK];
    int m = 0;
    long long total_ctx = 0, total_ticks = 0;
    uint64_t allowed = 0;
    for (int i = 0; i < s_ntask; i++)
        if (s_task[i].seen) {
            order[m++] = &s_task[i];
            total_ctx += s_task[i].ctx - s_task[i].prev_ctx;
            total_ticks += s_task[i].ticks - s_task[i].prev_ticks;
            allowed |= s_task[i].allowed;
        }
    qsort(order, m, sizeof(order[0]), cmp_task);
    char al[64] = "?", on[64] = "?";
    if (allowed)
        format_cpu_list(allowed, al, sizeof(al));
    if (s_online)
        format_cpu_list(s_online, on, sizeof(on));
    LOGI("  linux threads: %d, total %.0f%% of a core, %.0f wake-ups/s (context switches) | game may run on cores %s "
         "of %s",
         m, total_ticks * 100.0 / (seconds * 100.0), total_ctx / seconds, al, on);
    /* the emulator can confine the game to a few cores (Winlator: Processor Affinity) without
     * Windows seeing it: Wine still reports every CPU */
    if (allowed && s_online && (allowed & s_online) != s_online && allowed != s_allowed_seen)
        LOGI("  WARNING: the game can only use %d of the %d cores (%s of %s): the emulator limits its CPU affinity "
             "(Winlator / GameNative: tick every core under Processor Affinity). Results are not comparable with "
             "all cores.",
             popcount64(allowed & s_online), popcount64(s_online), al, on);
    if (allowed)
        s_allowed_seen = allowed;
    report_other_load(seconds, total_ticks * 100.0 / (seconds * 100.0));
    for (int i = 0; i < m && i < 10; i++) {
        Task* t = order[i];
        char place[96] = "";
        LONG64 tot = 0;
        for (int c = 0; c < s_nclusters; c++)
            tot += t->on_cluster[c];
        if (tot) {
            int k = 0;
            for (int c = 0; c < s_nclusters; c++)
                k += snprintf(place + k, sizeof(place) - k, "%scpu%d-%d %.0f%%", c ? " " : "", s_cluster_first[c],
                              s_cluster_last[c], t->on_cluster[c] * 100.0 / tot);
        }
        LOGI("    %-16s %5.1f%%  %6.0f wake-ups/s  %s", t->comm, (t->ticks - t->prev_ticks) * 100.0 / (seconds * 100.0),
             (t->ctx - t->prev_ctx) / seconds, place);
    }
    /* reset window, and choose which threads get placement samples next window */
    for (int i = 0; i < s_ntask; i++) {
        s_task[i].prev_ticks = s_task[i].ticks;
        s_task[i].prev_ctx = s_task[i].ctx;
        s_task[i].hot = 0;
        memset(s_task[i].on_cluster, 0, sizeof(s_task[i].on_cluster));
    }
    for (int i = 0; i < m && i < 8; i++)
        order[i]->hot = 1;
    ReleaseSRWLockExclusive(&s_lock);
}
