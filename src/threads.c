/* Thread registry, thread creation / affinity / priority hooks, fast-core detection. */
#include "mcpu.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

HANDLE (WINAPI* o_CreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
uintptr_t (__cdecl* o_beginthreadex)(void*, unsigned, unsigned (__stdcall*)(void*), void*, unsigned, unsigned*);
DWORD_PTR (WINAPI* o_SetThreadAffinityMask)(HANDLE, DWORD_PTR);
BOOL (WINAPI* o_SetThreadPriority)(HANDLE, int);
BOOL (WINAPI* o_InitializeCriticalSectionAndSpinCount)(LPCRITICAL_SECTION, DWORD);
void* o_gfd_thread_create;

uint64_t g_fast_mask;
static uint64_t g_allowed_mask;
static DWORD g_main_tid;

typedef HRESULT (WINAPI* SetThreadDescription_t)(HANDLE, PCWSTR);
static SetThreadDescription_t pSetThreadDescription;

/* ------------------------------------------------------------------ registry */
#define MAX_THREADS 512
typedef struct ThreadInfo {
    DWORD tid;
    int kind;
    char name[40];
    void* start;
    void* inner;
    void* caller;
} ThreadInfo;

static ThreadInfo g_threads[MAX_THREADS];
static int g_thread_count;
static SRWLOCK g_reg_lock = SRWLOCK_INIT;

static ThreadInfo* find_locked(DWORD tid)
{
    for (int i = 0; i < g_thread_count; i++)
        if (g_threads[i].tid == tid)
            return &g_threads[i];
    return NULL;
}

void threads_register(DWORD tid, int kind, const char* name, void* start, void* caller)
{
    if (!tid)
        return;
    AcquireSRWLockExclusive(&g_reg_lock);
    ThreadInfo* t = find_locked(tid);
    if (!t) {
        if (g_thread_count < MAX_THREADS) {
            t = &g_threads[g_thread_count++];
        } else {
            t = &g_threads[MAX_THREADS - 1]; /* overwrite the last slot rather than fail */
        }
        memset(t, 0, sizeof(*t));
        t->tid = tid;
    }
    if (kind != TK_UNKNOWN)
        t->kind = kind;
    if (name && name[0]) {
        size_t i = 0;
        for (; i < sizeof(t->name) - 1 && name[i]; i++)
            t->name[i] = name[i];
        t->name[i] = 0;
    }
    if (start)
        t->start = start;
    if (caller)
        t->caller = caller;
    ReleaseSRWLockExclusive(&g_reg_lock);
}

static void registry_set_inner(DWORD tid, void* inner)
{
    AcquireSRWLockExclusive(&g_reg_lock);
    ThreadInfo* t = find_locked(tid);
    if (t)
        t->inner = inner;
    ReleaseSRWLockExclusive(&g_reg_lock);
}

int threads_lookup(DWORD tid, char* name, size_t cap, int* kind)
{
    int ok = 0;
    AcquireSRWLockShared(&g_reg_lock);
    ThreadInfo* t = find_locked(tid);
    if (t) {
        ok = 1;
        if (kind)
            *kind = t->kind;
        if (name && cap) {
            if (t->name[0]) {
                snprintf(name, cap, "%s", t->name);
            } else {
                char a[64], b[64];
                fmt_addr(t->start, a, sizeof(a));
                if (t->inner)
                    snprintf(name, cap, "start=%s fn=%s", a, fmt_addr(t->inner, b, sizeof(b)));
                else
                    snprintf(name, cap, "start=%s", a);
            }
        }
    }
    ReleaseSRWLockShared(&g_reg_lock);
    return ok;
}

/* If `arg` points at a readable {?, fn} pair whose fn lies in the game's .text,
 * remember fn: both the engine's thread object and the CRT invoker thunks use this layout. */
static void* guess_inner(void* arg)
{
    if (!arg)
        return NULL;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(arg, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return NULL;
    if ((uint8_t*)arg + 16 > (uint8_t*)mbi.BaseAddress + mbi.RegionSize)
        return NULL;
    void* fn = *(void**)((uint8_t*)arg + 8);
    uint8_t* gb = game_base();
    if (gb && (uint8_t*)fn > gb && (uint8_t*)fn < gb + game_size())
        return fn;
    return NULL;
}

/* ------------------------------------------------------------------ policies */
uint64_t threads_process_mask(void)
{
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc)
        proc = (DWORD_PTR)~0ull;
    return (uint64_t)proc;
}

static void apply_worker_policy(HANDLE h, int kind)
{
    if (g_cfg.worker_cores) {
        uint64_t m = g_cfg.worker_cores & g_allowed_mask;
        if (m)
            SetThreadAffinityMask(h, (DWORD_PTR)m);
    }
    if (kind == TK_JOB_WORKER && g_cfg.worker_priority != 99)
        SetThreadPriority(h, g_cfg.worker_priority);
}

static int policy_logged;

DWORD_PTR WINAPI hk_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    InterlockedIncrement64(&g_stats.affinity_requests);
    DWORD tid = GetThreadId(h);
    if (InterlockedIncrement((volatile LONG*)&policy_logged) <= 64) {
        char who[96], at[64];
        if (!threads_lookup(tid, who, sizeof(who), NULL))
            snprintf(who, sizeof(who), "?");
        LOGI("Game SetThreadAffinityMask(tid %lu [%s], 0x%llx) from %s -> %s", tid, who,
             (unsigned long long)mask, fmt_addr(__builtin_return_address(0), at, sizeof(at)),
             g_cfg.affinity_mode == 0 ? "passed through" : g_cfg.affinity_mode == 1 ? "ignored" : "remapped");
    }
    switch (g_cfg.affinity_mode) {
    case 0:
        return o_SetThreadAffinityMask(h, mask);
    case 2: {
        uint64_t m = (uint64_t)mask & g_allowed_mask;
        if (!m)
            m = g_allowed_mask;
        return o_SetThreadAffinityMask(h, (DWORD_PTR)m);
    }
    default:
        /* Leave scheduling to the OS. Report success with a plausible previous mask. */
        InterlockedIncrement64(&g_stats.affinity_blocked);
        return (DWORD_PTR)g_allowed_mask;
    }
}

BOOL WINAPI hk_SetThreadPriority(HANDLE h, int prio)
{
    InterlockedIncrement64(&g_stats.priority_requests);
    DWORD tid = GetThreadId(h);
    int kind = TK_UNKNOWN;
    threads_lookup(tid, NULL, 0, &kind);
    int final = prio;
    if (kind == TK_JOB_WORKER && g_cfg.worker_priority != 99)
        final = g_cfg.worker_priority;
    if (tid == g_main_tid && g_cfg.main_priority != 99)
        final = g_cfg.main_priority;
    LOGV("Game SetThreadPriority(tid %lu, %d) -> %d", tid, prio, final);
    return o_SetThreadPriority(h, final);
}

BOOL WINAPI hk_InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InterlockedIncrement64(&g_stats.cs_inits);
    if (g_cfg.cs_spin_count >= 0)
        spin = (DWORD)g_cfg.cs_spin_count;
    return o_InitializeCriticalSectionAndSpinCount(cs, spin);
}

HANDLE WINAPI hk_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                              LPVOID arg, DWORD flags, LPDWORD tid_out)
{
    DWORD tid = 0;
    HANDLE h = o_CreateThread(sa, stack, start, arg, flags, &tid);
    if (h) {
        if (tid_out)
            *tid_out = tid;
        InterlockedIncrement64(&g_stats.threads_created);
        threads_register(tid, TK_WIN32, NULL, (void*)start, __builtin_return_address(0));
        void* inner = guess_inner(arg);
        if (inner)
            registry_set_inner(tid, inner);
        apply_worker_policy(h, TK_WIN32);
    }
    return h;
}

uintptr_t __cdecl hk_beginthreadex(void* sec, unsigned stack, unsigned (__stdcall* start)(void*), void* arg,
                                   unsigned flags, unsigned* tid_out)
{
    unsigned tid = 0;
    uintptr_t h = o_beginthreadex(sec, stack, start, arg, flags, &tid);
    if (h) {
        if (tid_out)
            *tid_out = tid;
        InterlockedIncrement64(&g_stats.threads_created);
        threads_register(tid, TK_CRT, NULL, (void*)start, __builtin_return_address(0));
        void* inner = guess_inner(arg);
        if (inner)
            registry_set_inner(tid, inner);
        apply_worker_policy((HANDLE)h, TK_CRT);
    }
    return h;
}

typedef void* (*gfd_thread_create_t)(void*, void*, int, int, int, const char*);

void* hk_gfd_thread_create(void* entry, void* arg, int prio, int stack, int flags, const char* name)
{
    void* obj = ((gfd_thread_create_t)o_gfd_thread_create)(entry, arg, prio, stack, flags, name);
    if (obj) {
        HANDLE h = *(HANDLE*)obj;
        DWORD tid = GetThreadId(h);
        int kind = (name && strncmp(name, "jobWorker", 9) == 0) ? TK_JOB_WORKER : TK_ENGINE;
        threads_register(tid, kind, name, NULL, NULL);
        registry_set_inner(tid, entry);
        if (pSetThreadDescription && name) {
            WCHAR w[64];
            MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64);
            w[63] = 0;
            pSetThreadDescription(h, w);
        }
        apply_worker_policy(h, kind);
        char at[64];
        LOGI("Engine thread created: '%s' tid %lu prio %d stack 0x%x entry %s", name ? name : "?", tid, prio,
             stack, fmt_addr(entry, at, sizeof(at)));
    }
    return obj;
}

/* ------------------------------------------------------------------ main thread */
DWORD threads_find_main(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), best = 0;
    ULONGLONG best_time = ~0ull;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid)
                continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
                if (t < best_time) {
                    best_time = t;
                    best = te.th32ThreadID;
                }
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return best;
}

DWORD threads_main_tid(void)
{
    return g_main_tid;
}

void threads_apply_main_policy(DWORD main_tid)
{
    if (!main_tid)
        return;
    g_main_tid = main_tid;
    threads_register(main_tid, TK_MAIN, "main", NULL, NULL);
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, main_tid);
    if (!h) {
        LOGE("OpenThread(main %lu) failed: %lu", main_tid, GetLastError());
        return;
    }
    uint64_t mask = g_cfg.main_cores_auto ? g_fast_mask : g_cfg.main_cores;
    mask &= g_allowed_mask;
    if (mask && mask != g_allowed_mask) {
        DWORD_PTR prev = SetThreadAffinityMask(h, (DWORD_PTR)mask);
        LOGI("Main thread %lu affinity -> 0x%llx (%s)", main_tid, (unsigned long long)mask,
             prev ? "ok" : "failed");
    } else {
        LOGI("Main thread %lu affinity left to the OS%s", main_tid,
             mask ? " (fast mask covers every allowed core)" : "");
    }
    if (g_cfg.main_priority > 0 && g_cfg.main_priority != 99 && !g_cfg.adaptive_yield) {
        /* A higher-priority main thread spinning on raw Sleep(0) never yields to a
         * normal-priority lock holder on the same core (priority inversion). */
        LOGI("Main thread priority boost skipped: needs [Yield] AdaptiveYield=1");
    } else if (g_cfg.main_priority != 99) {
        BOOL ok = SetThreadPriority(h, g_cfg.main_priority);
        LOGI("Main thread priority -> %d (%s)", g_cfg.main_priority, ok ? "ok" : "failed");
    }
    CloseHandle(h);
}

/* Pin threads whose description starts with one of FastThreadNames (e.g. DXVK's
 * "dxvk-cs" submission thread) onto the fast cores. Runs from the profiler loop. */
#define MAX_PINNED 64
static DWORD g_pinned[MAX_PINNED];
static int g_pinned_count;

/* A/B side A: undo our placement (main thread + name-pinned threads back to all
 * cores, main thread back to normal priority). Runs on the profiler thread. */
void threads_reset_placement(void)
{
    DWORD_PTR all = (DWORD_PTR)g_allowed_mask;
    if (g_main_tid) {
        HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
        if (h) {
            SetThreadAffinityMask(h, (DWORD_PTR)threads_process_mask());
            SetThreadPriority(h, THREAD_PRIORITY_NORMAL);
            CloseHandle(h);
        }
    }
    for (int i = 0; i < g_pinned_count; i++) {
        HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, g_pinned[i]);
        if (h) {
            SetThreadAffinityMask(h, (DWORD_PTR)threads_process_mask());
            CloseHandle(h);
        }
    }
    g_pinned_count = 0; /* side B pins them again */
    (void)all;
}

typedef HRESULT (WINAPI* GetThreadDescription_t)(HANDLE, PWSTR*);

void threads_apply_name_policy(void)
{
    if (!g_cfg.fast_thread_names[0] || !g_fast_mask || g_fast_mask == g_allowed_mask)
        return;
    GetThreadDescription_t gtd =
        (GetThreadDescription_t)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    if (!gtd)
        return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || g_pinned_count >= MAX_PINNED)
                continue;
            int done = 0;
            for (int i = 0; i < g_pinned_count; i++)
                if (g_pinned[i] == te.th32ThreadID)
                    done = 1;
            if (done)
                continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION, FALSE,
                                  te.th32ThreadID);
            if (!h)
                continue;
            PWSTR desc = NULL;
            if (SUCCEEDED(gtd(h, &desc)) && desc && desc[0]) {
                char name[64];
                WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name), NULL, NULL);
                name[63] = 0;
                char list[256];
                snprintf(list, sizeof(list), "%s", g_cfg.fast_thread_names);
                for (char* tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
                    while (*tok == ' ')
                        tok++;
                    if (*tok && strncmp(name, tok, strlen(tok)) == 0) {
                        DWORD_PTR prev = SetThreadAffinityMask(h, (DWORD_PTR)g_fast_mask);
                        LOGI("Pinned thread %lu '%s' to fast cores 0x%llx (%s)", te.th32ThreadID, name,
                             (unsigned long long)g_fast_mask, prev ? "ok" : "failed");
                        g_pinned[g_pinned_count++] = te.th32ThreadID;
                        break;
                    }
                }
            }
            if (desc)
                LocalFree(desc);
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* ------------------------------------------------------------------ fast cores */
static uint64_t bench_once(uint32_t iters)
{
    volatile uint64_t sink;
    uint64_t x = 88172645463325252ull, acc = 0;
    uint64_t t0 = qpc_now();
    for (uint32_t i = 0; i < iters; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        acc += x * 2654435761u;
    }
    sink = acc;
    (void)sink;
    return qpc_now() - t0;
}

uint64_t detect_fast_cores(uint64_t allowed)
{
    HANDLE th = GetCurrentThread();
    double best_us[64];
    double fastest = 1e30;
    uint32_t iters = 200000;
    int calibrated = 0, measured = 0;
    DWORD_PTR original = 0;

    for (int i = 0; i < 64; i++) {
        best_us[i] = -1;
        if (!(allowed >> i & 1))
            continue;
        DWORD_PTR prev = SetThreadAffinityMask(th, (DWORD_PTR)1 << i);
        if (!prev)
            continue;
        if (!original)
            original = prev;
        SwitchToThread();
        if (!calibrated) {
            double us = qpc_to_us(bench_once(iters));
            if (us > 1.0)
                iters = (uint32_t)(iters * (1000.0 / us)); /* ~1 ms per run */
            if (iters < 20000)
                iters = 20000;
            calibrated = 1;
        }
        bench_once(iters * 3); /* warm-up: let DVFS raise the clock */
        double b = 1e30;
        for (int r = 0; r < 3; r++) {
            double us = qpc_to_us(bench_once(iters));
            if (us < b)
                b = us;
        }
        best_us[i] = b;
        if (b < fastest)
            fastest = b;
        measured++;
    }
    SetThreadAffinityMask(th, original ? original : (DWORD_PTR)allowed);

    uint64_t fast = 0;
    char line[512];
    int n = 0;
    for (int i = 0; i < 64; i++) {
        if (best_us[i] < 0)
            continue;
        double rel = fastest / best_us[i];
        if (best_us[i] <= fastest * 1.40)
            fast |= 1ull << i;
        if (n < (int)sizeof(line) - 24)
            n += snprintf(line + n, sizeof(line) - n, " c%d=%.0f%%", i, rel * 100.0);
    }
    line[n] = 0;
    LOGI("Core speed (relative to fastest):%s", measured ? line : " n/a");
    if (!fast)
        fast = allowed;
    LOGI("Fast core mask: 0x%llx (allowed 0x%llx)", (unsigned long long)fast, (unsigned long long)allowed);
    return fast;
}

void threads_init(HMODULE game)
{
    (void)game;
    pSetThreadDescription =
        (SetThreadDescription_t)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetThreadDescription");
    uint64_t proc = threads_process_mask();
    g_allowed_mask = g_cfg.allowed_cores ? (proc & g_cfg.allowed_cores) : proc;
    if (!g_allowed_mask)
        g_allowed_mask = proc;
    g_fast_mask = g_allowed_mask;
    if (!o_SetThreadAffinityMask)
        o_SetThreadAffinityMask = SetThreadAffinityMask;
    if (!o_SetThreadPriority)
        o_SetThreadPriority = SetThreadPriority;
}
