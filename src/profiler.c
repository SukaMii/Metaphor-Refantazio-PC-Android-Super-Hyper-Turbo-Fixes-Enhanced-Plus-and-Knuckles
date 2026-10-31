/* Periodic per-thread CPU report. Shows whether the game is bound by one thread
 * (main / dxvk-cs) and how much time goes into spinning. */
#include "mcpu.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

volatile DWORD* g_engine_main_tid;

typedef LONG (NTAPI* NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef HRESULT (WINAPI* GetThreadDescription_t)(HANDLE, PWSTR*);

#define MAX_SAMPLES 512
typedef struct Sample {
    DWORD tid;
    ULONGLONG cpu;     /* kernel + user, 100 ns */
    ULONGLONG delta;
    LONG64 yields, ydelta;
    LONG64 escal, edelta;
    int prio;
    char name[72];
} Sample;

static Sample g_prev[MAX_SAMPLES];
static int g_prev_count;
static Stats g_prev_stats;
static LONG64 g_prev_sites_yield[SITE_SLOTS], g_prev_sites_esc[SITE_SLOTS], g_prev_sites_lock[SITE_SLOTS],
    g_prev_sites_sleepn[SITE_SLOTS];
static FrameTotals g_prev_frames;

static ULONGLONG ft64(FILETIME f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static int cmp_delta(const void* a, const void* b)
{
    const Sample* x = (const Sample*)a;
    const Sample* y = (const Sample*)b;
    return (x->delta < y->delta) - (x->delta > y->delta);
}

static void describe(DWORD tid, HANDLE h, char* out, size_t cap, NtQueryInformationThread_t nqit,
                     GetThreadDescription_t gtd)
{
    if (threads_lookup(tid, out, cap, NULL) && out[0])
        return;
    if (gtd) {
        PWSTR desc = NULL;
        if (SUCCEEDED(gtd(h, &desc)) && desc && desc[0]) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, out, (int)cap, NULL, NULL);
            out[cap - 1] = 0;
            LocalFree(desc);
            return;
        }
        if (desc)
            LocalFree(desc);
    }
    void* start = NULL;
    if (nqit && nqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), NULL) == 0 && start) {
        char a[64];
        snprintf(out, cap, "start=%s", fmt_addr(start, a, sizeof(a)));
        return;
    }
    snprintf(out, cap, "?");
}

static void report(double seconds, NtQueryInformationThread_t nqit, GetThreadDescription_t gtd, int quiet)
{
    static Sample cur[MAX_SAMPLES];
    int n = 0;
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || n >= MAX_SAMPLES)
                continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                Sample* s = &cur[n++];
                s->tid = te.th32ThreadID;
                s->cpu = ft64(k) + ft64(u);
                s->delta = s->cpu;
                s->yields = s->escal = 0;
                timing_thread_yields(s->tid, &s->yields, &s->escal);
                s->ydelta = s->yields;
                s->edelta = s->escal;
                for (int i = 0; i < g_prev_count; i++) {
                    if (g_prev[i].tid == s->tid) {
                        s->delta = s->cpu >= g_prev[i].cpu ? s->cpu - g_prev[i].cpu : 0;
                        s->ydelta = s->yields - g_prev[i].yields;
                        s->edelta = s->escal - g_prev[i].escal;
                        break;
                    }
                }
                s->prio = GetThreadPriority(h);
                describe(s->tid, h, s->name, sizeof(s->name), nqit, gtd);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    memcpy(g_prev, cur, sizeof(Sample) * n);
    g_prev_count = n;
    if (quiet) {
        g_prev_stats = g_stats;
        g_prev_frames = g_frames;
        InterlockedExchange64(&g_frames.window_max_us, 0);
        for (int s = 0; s < SITE_SLOTS; s++) {
            g_prev_sites_yield[s] = g_sites_yield.count[s];
            g_prev_sites_esc[s] = g_sites_escalated.count[s];
            g_prev_sites_lock[s] = g_sites_lock.count[s];
            g_prev_sites_sleepn[s] = g_sites_sleepn.count[s];
        }
        return;
    }

    ULONGLONG total = 0;
    for (int i = 0; i < n; i++)
        total += cur[i].delta;
    qsort(cur, n, sizeof(Sample), cmp_delta);

    double denom = seconds * 1e7; /* 100 ns units per core */
    DWORD engine_main = g_engine_main_tid ? *g_engine_main_tid : 0;
    if (!engine_main)
        engine_main = threads_main_tid(); /* earliest-created thread */
    LOGI("---- CPU report%s: %.1f s window, %d threads, process total %.0f%% of one core ----", ab_window_tag(),
         seconds, n, total * 100.0 / denom);
    int top = g_cfg.prof_top < n ? g_cfg.prof_top : n;
    LOGI("  %10s  %6s  %4s  %9s  %7s  %s", "tid", "cpu", "prio", "Sleep0/s", "esc/s", "thread");
    double main_pct = -1, busiest = 0;
    for (int i = 0; i < n; i++) {
        double pct = cur[i].delta * 100.0 / denom;
        if (pct > busiest)
            busiest = pct;
        if (engine_main && cur[i].tid == engine_main)
            main_pct = pct;
    }
    for (int i = 0; i < top; i++) {
        const Sample* s = &cur[i];
        LOGI("  tid %6lu  %5.1f%%  %4d  %9.0f  %7.0f  %s%s", s->tid, s->delta * 100.0 / denom, s->prio,
             s->ydelta / seconds, s->edelta / seconds, s->name,
             (engine_main && s->tid == engine_main) ? "  <engine main>" : "");
    }

    Stats now = g_stats;
#define RATE(f) ((now.f - g_prev_stats.f) / seconds)
    LOGI("  per second: Sleep(0) %.0f (escalated %.0f), Sleep(n) %.0f, engine usleep %.0f, "
         "timer-period calls %.0f (%s)",
         RATE(sleep0_calls), RATE(sleep0_escalated), RATE(sleepn_calls), RATE(usleep_calls), RATE(period_calls),
         g_cfg.fix_timer_churn ? "suppressed" : "passed through");
    LOGI("  per second: spinlock contended %.0f, blocked waits %.0f | totals: threads %lld, "
         "affinity requests %lld (blocked %lld), priority requests %lld",
         RATE(lock_contended), RATE(lock_waits), now.threads_created, now.affinity_requests,
         now.affinity_blocked, now.priority_requests);
    if (g_cfg.fast_sem)
        LOGI("  job semaphore (fast): %.0f waits/s, %.0f went to sleep, %.0f posts/s, %.0f woke a sleeper",
             RATE(sem_waits), RATE(sem_sleeps), RATE(sem_posts), RATE(sem_wakes));
#undef RATE
    g_prev_stats = now;

    site_report("Sleep(0) call sites", &g_sites_yield, g_prev_sites_yield, seconds, 6);
    site_report("Sleep(0) escalated to real sleeps, by site", &g_sites_escalated, g_prev_sites_esc, seconds, 6);
    site_report("Contended engine spinlock, by caller", &g_sites_lock, g_prev_sites_lock, seconds, 6);
    site_report("Real sleeps (Sleep(n) / engine usleep), by caller", &g_sites_sleepn, g_prev_sites_sleepn, seconds, 6);

    /* ---- frames (from the Present hook) */
    FrameTotals f = g_frames;
    LONG64 worst_us = InterlockedExchange64(&g_frames.window_max_us, 0);
    LONG64 df = f.frames - g_prev_frames.frames;
    double dsum = f.sum_interval_ms - g_prev_frames.sum_interval_ms;
    double dsq = f.sum_sq_interval_ms - g_prev_frames.sum_sq_interval_ms;
    double dinside = f.sum_inside_ms - g_prev_frames.sum_inside_ms;
    LONG64 dvsync = f.vsync_frames - g_prev_frames.vsync_frames;
    LONG64 dstalls = f.stalls - g_prev_frames.stalls;
    LONG64 db[FRAME_BUCKETS];
    for (int i = 0; i < FRAME_BUCKETS; i++)
        db[i] = f.buckets[i] - g_prev_frames.buckets[i];
    FrameTotals prevf = g_prev_frames;
    g_prev_frames = f;
    double avg = 0, sd = 0, inside_share = 0;
    if (df > 0 && dsum > 0) {
        avg = dsum / df;
        double var = dsq / df - avg * avg;
        sd = var > 0 ? sqrt(var) : 0;
        inside_share = dinside / dsum;
        LOGI("  frames: %.1f fps | frame time avg %.1f ms, sd %.1f ms, worst %.1f ms | main thread inside Present "
             "%.1f ms/frame (%.0f%%) | V-Sync requested on %.0f%% of frames (interval %u)%s",
             1000.0 / avg, avg, sd, worst_us / 1000.0, dinside / df, inside_share * 100.0, dvsync * 100.0 / df,
             f.last_sync, dstalls ? " | long stalls excluded" : "");
        if (main_pct >= 0) {
            LOGI("  main thread CPU per frame: %.1f ms of %.1f ms", main_pct / 100.0 * seconds * 1000.0 / df, avg);
            waitprof_report(df, main_pct / 100.0 * seconds * 1000.0 / df, avg);
        }
        if (g_cfg.limiter_fps > 0) {
            double dlim = f.sum_limiter_ms - prevf.sum_limiter_ms;
            double dspin = f.sum_limiter_spin_ms - prevf.sum_limiter_spin_ms;
            LOGI("  MetaphorCPU limiter: waited %.1f ms/frame, of which %.2f ms spinning", dlim / df, dspin / df);
        }
        LOGI("  frame-time spread: <14ms %.0f%% | 14-18 %.0f%% | 18-25 %.0f%% | 25-31 %.0f%% | 31-36 %.0f%% | "
             "36-50 %.0f%% | 50-100 %.0f%% | >100 %.0f%%",
             db[0] * 100.0 / df, db[1] * 100.0 / df, db[2] * 100.0 / df, db[3] * 100.0 / df, db[4] * 100.0 / df,
             db[5] * 100.0 / df, db[6] * 100.0 / df, db[7] * 100.0 / df);
    }

    if (df >= 10 && avg > 0) {
        /* A cap shows up as a window average sitting on a standard value (30/40/60) while
         * individual frames may still jitter around it (Sleep-based limiters). */
        double fps = 1000.0 / avg;
        static const double caps[] = {30, 40, 60};
        int locked = 0;
        for (int i = 0; i < 3; i++)
            if (fabs(fps - caps[i]) < 0.6 && sd < 0.25 * avg)
                locked = 1;
        if (locked && g_cfg.limiter_fps > 0)
            LOGI("  verdict: frame rate held at %.1f fps by the MetaphorCPU limiter (frame time sd %.1f ms)", fps, sd);
        else if (locked)
            LOGI("  verdict: frame rate sits at %.1f fps, a standard cap value -> very likely a frame cap or V-Sync "
                 "is the limit, not CPU or GPU (%s)",
                 fps,
                 dvsync * 2 > df ? "the game itself presents with V-Sync on: try V-Sync off in the game's options"
                                 : "the game does not request V-Sync: check the in-game frame-rate limit, "
                                   "DXVK_FRAME_RATE / dxgi.maxFrameRate and the emulator's FPS limiter");
        else if (inside_share > 0.35)
            LOGI("  verdict: the main thread waits inside Present for %.0f%% of each frame -> GPU-bound "
                 "(or a frame limiter inside the graphics stack)",
                 inside_share * 100.0);
        else if (main_pct >= 85)
            LOGI("  verdict: main thread %.0f%% busy -> CPU-bound on the main thread", main_pct);
        else
            LOGI("  verdict: main thread %.0f%% busy, only %.0f%% of the frame inside Present -> time goes to other "
                 "waits (job sync, streaming, CPU<->GPU sync); see the Sleep(0) call sites",
                 main_pct, inside_share * 100.0);
    } else if (main_pct >= 0) {
        if (main_pct >= 85)
            LOGI("  verdict: main thread %.0f%% busy -> CPU-bound on the main thread", main_pct);
        else if (busiest < 85)
            LOGI("  verdict: no thread is saturated (main %.0f%%, busiest %.0f%%) -> frame rate is set by the GPU, "
                 "a frame cap/vsync, or loading; CPU changes will not raise FPS in this scene",
                 main_pct, busiest);
        else
            LOGI("  verdict: main %.0f%%, but another thread is at %.0f%% -> check the table for the bottleneck",
                 main_pct, busiest);
    }

    gpuprobe_report(seconds);
    shadowcull_report(seconds);
    telemetry_report(seconds);
    sampler_report();
    ab_after_window(); /* may switch A/B side for the next window */

    /* next window's sampling targets: main thread + busiest other threads */
    if (g_cfg.sampler_hz > 0) {
        DWORD tids[6];
        char names[6][72];
        int k = 0;
        for (int i = 0; i < n && k == 0; i++)
            if (cur[i].tid == engine_main) {
                tids[k] = cur[i].tid;
                memcpy(names[k], cur[i].name, sizeof(names[0]));
                k++;
            }
        for (int i = 0; i < n && k < 1 + g_cfg.sampler_threads && k < 6; i++) {
            if (cur[i].tid == engine_main || mcpu_is_own_thread(cur[i].tid) || cur[i].delta * 100.0 / denom < 3.0)
                continue;
            tids[k] = cur[i].tid;
            memcpy(names[k], cur[i].name, sizeof(names[0]));
            k++;
        }
        sampler_set_targets(tids, (const char(*)[72])names, k);
    }
}

static DWORD WINAPI profiler_thread(LPVOID unused)
{
    (void)unused;
    mcpu_register_own_thread(GetCurrentThreadId());
    NtQueryInformationThread_t nqit =
        (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    GetThreadDescription_t gtd =
        (GetThreadDescription_t)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    uint64_t last = qpc_now();
    report(0, nqit, gtd, 1); /* baseline */
    for (;;) {
        Sleep((DWORD)g_cfg.prof_interval_sec * 1000);
        uint64_t now = qpc_now();
        double secs = qpc_to_us(now - last) / 1e6;
        last = now;
        threads_apply_name_policy();
        report(secs, nqit, gtd, !g_cfg.prof_enabled);
    }
    return 0;
}

void profiler_start(void)
{
    HANDLE h = CreateThread(NULL, 0, profiler_thread, NULL, 0, NULL);
    if (h) {
        typedef HRESULT (WINAPI* STD)(HANDLE, PCWSTR);
        STD std = (STD)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetThreadDescription");
        if (std)
            std(h, L"MetaphorCPU-profiler");
        CloseHandle(h);
    }
}
