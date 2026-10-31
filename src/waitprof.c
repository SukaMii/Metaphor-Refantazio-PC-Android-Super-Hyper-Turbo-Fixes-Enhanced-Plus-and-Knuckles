/* Main-thread wait profile ([WaitProfile] Enabled=1, diagnostic).
 *
 * The tablet log showed a frame of ~37 ms in which the main thread computes for ~12 ms
 * while neither the GPU nor any CPU thread is saturated: the frame is held up by threads
 * waiting for each other. This measures, per frame, how long the main thread is blocked
 * and where (game call site):
 *   sleeps             Sleep(n), SleepEx, the engine usleep (e.g. the job poll loop)
 *   event waits        WaitForSingleObject(Ex) (job semaphore, thread hand-offs)
 *   critical sections  EnterCriticalSection when the lock is taken (time until acquired)
 *   yields             Sleep(0) loops, including the sleeps adaptive yield turns them into
 * What remains of the frame after CPU time and these waits is time the thread was ready
 * but not running, or blocked inside other calls (D3D / DXVK). */
#include "mcpu.h"
#include <stdio.h>
#include <string.h>

static const char* const wp_name[WP_COUNT] = {"sleeps", "event waits", "critical sections", "yields"};
static SiteTable g_wp_sites[WP_COUNT]; /* main thread only: call site -> nanoseconds */
static LONG64 g_wp_prev_sites[WP_COUNT][SITE_SLOTS];
static volatile LONG64 g_wp_ns[WP_COUNT], g_wp_calls[WP_COUNT];
static double g_ns_per_tick;

/* Time inside D3D11 immediate-context calls (gpuprobe.c hooks them when the wait profile
 * is on). Part of it is CPU work already counted as CPU time, part is blocking: DXVK
 * waiting for its command-stream thread or for the GPU (e.g. a Map that reads back). */
static const char* const d3d_name[D3D_COUNT] = {"ExecuteCommandList", "Map(write)", "Map(read)", "Unmap",
                                                "UpdateSubresource", "Copy", "GetData", "Flush", "End"};
static SiteTable g_d3d_sites; /* call site -> nanoseconds, all methods */
static LONG64 g_d3d_prev_sites[SITE_SLOTS];
static volatile LONG64 g_d3d_ns[D3D_COUNT], g_d3d_calls[D3D_COUNT];

void waitprof_add_d3d(int method, void* site, uint64_t ticks)
{
    LONG64 ns = (LONG64)((double)ticks * g_ns_per_tick);
    InterlockedAdd64(&g_d3d_ns[method], ns);
    InterlockedIncrement64(&g_d3d_calls[method]);
    site_add(&g_d3d_sites, site, ns);
}

DWORD (WINAPI* o_WaitForSingleObject)(HANDLE, DWORD);
DWORD (WINAPI* o_WaitForSingleObjectEx)(HANDLE, DWORD, BOOL);
DWORD (WINAPI* o_SleepEx)(DWORD, BOOL);
void (WINAPI* o_EnterCriticalSection)(LPCRITICAL_SECTION);

void waitprof_init(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ns_per_tick = 1e9 / (double)f.QuadPart;
}

void waitprof_add(int cat, void* site, uint64_t ticks)
{
    if (GetCurrentThreadId() != threads_main_tid())
        return;
    LONG64 ns = (LONG64)((double)ticks * g_ns_per_tick);
    InterlockedAdd64(&g_wp_ns[cat], ns);
    InterlockedIncrement64(&g_wp_calls[cat]);
    site_add(&g_wp_sites[cat], site, ns);
}

DWORD WINAPI hk_WaitForSingleObject(HANDLE h, DWORD ms)
{
    void* ret = __builtin_return_address(0);
    uint64_t t0 = qpc_now();
    DWORD r = o_WaitForSingleObject(h, ms);
    DWORD e = GetLastError();
    waitprof_add(WP_EVENT, ret, qpc_now() - t0);
    SetLastError(e);
    return r;
}

DWORD WINAPI hk_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    void* ret = __builtin_return_address(0);
    uint64_t t0 = qpc_now();
    DWORD r = o_WaitForSingleObjectEx(h, ms, alertable);
    DWORD e = GetLastError();
    waitprof_add(WP_EVENT, ret, qpc_now() - t0);
    SetLastError(e);
    return r;
}

DWORD WINAPI hk_SleepEx(DWORD ms, BOOL alertable)
{
    void* ret = __builtin_return_address(0);
    uint64_t t0 = qpc_now();
    DWORD r = o_SleepEx(ms, alertable);
    waitprof_add(ms ? WP_SLEEP : WP_YIELD, ret, qpc_now() - t0);
    return r;
}

/* Uncontended: one TryEnter, the same atomic the original fast path does. Contended: the
 * original EnterCriticalSection (spin count, then block), timed. */
void WINAPI hk_EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (TryEnterCriticalSection(cs))
        return;
    void* ret = __builtin_return_address(0);
    uint64_t t0 = qpc_now();
    o_EnterCriticalSection(cs);
    waitprof_add(WP_CS, ret, qpc_now() - t0);
}

/* After the profiler's frame lines. frames, CPU and frame time are for this window. */
void waitprof_report(LONG64 frames, double cpu_ms, double frame_ms)
{
    static LONG64 prev_ns[WP_COUNT], prev_calls[WP_COUNT];
    if (!g_cfg.wait_profile)
        return;
    double ms[WP_COUNT], calls[WP_COUNT], waits = 0;
    for (int c = 0; c < WP_COUNT; c++) {
        LONG64 ns = g_wp_ns[c], n = g_wp_calls[c];
        ms[c] = frames > 0 ? (double)(ns - prev_ns[c]) / 1e6 / frames : 0;
        calls[c] = frames > 0 ? (double)(n - prev_calls[c]) / frames : 0;
        prev_ns[c] = ns, prev_calls[c] = n;
        waits += ms[c];
    }
    if (frames <= 0) {
        for (int c = 0; c < WP_COUNT; c++) /* keep the site deltas in step */
            for (int s = 0; s < SITE_SLOTS; s++)
                g_wp_prev_sites[c][s] = g_wp_sites[c].count[s];
        return;
    }
    double other = frame_ms - cpu_ms - waits;
    LOGI("  main thread per frame (%.1f ms): CPU %.1f | %s %.1f (%.1fx) | %s %.1f (%.1fx) | %s %.1f (%.1fx) | %s %.1f "
         "(%.1fx) | other %.1f ms (ready but not running, or inside D3D/driver calls)",
         frame_ms, cpu_ms, wp_name[0], ms[0], calls[0], wp_name[1], ms[1], calls[1], wp_name[2], ms[2], calls[2],
         wp_name[3], ms[3], calls[3], other > 0 ? other : 0.0);
    /* where: the 3 biggest call sites of each kind */
    for (int c = 0; c < WP_COUNT; c++) {
        void* top_a[3] = {0};
        LONG64 top_d[3] = {0};
        for (int s = 0; s < SITE_SLOTS; s++) {
            void* a = g_wp_sites[c].addr[s];
            if (!a)
                continue;
            LONG64 cur = g_wp_sites[c].count[s], d = cur - g_wp_prev_sites[c][s];
            g_wp_prev_sites[c][s] = cur;
            for (int i = 0; i < 3; i++)
                if (d > top_d[i]) {
                    for (int j = 2; j > i; j--)
                        top_a[j] = top_a[j - 1], top_d[j] = top_d[j - 1];
                    top_a[i] = a, top_d[i] = d;
                    break;
                }
        }
        if (!top_a[0] || (double)top_d[0] / 1e6 / frames < 0.05)
            continue;
        char line[320];
        int n = snprintf(line, sizeof(line), "    main thread %s at:", wp_name[c]);
        for (int i = 0; i < 3 && top_a[i] && n < (int)sizeof(line) - 80; i++) {
            char at[64];
            n += snprintf(line + n, sizeof(line) - n, " %s %.2f ms", fmt_addr(top_a[i], at, sizeof(at)),
                          (double)top_d[i] / 1e6 / frames);
        }
        LOGI("%s", line);
    }

    /* D3D calls on the main thread */
    static LONG64 prev_d3d_ns[D3D_COUNT], prev_d3d_calls[D3D_COUNT];
    char line[600];
    int n = 0;
    double total = 0;
    for (int m = 0; m < D3D_COUNT; m++) {
        LONG64 ns = g_d3d_ns[m], c = g_d3d_calls[m];
        double ms = (double)(ns - prev_d3d_ns[m]) / 1e6 / frames, calls = (double)(c - prev_d3d_calls[m]) / frames;
        prev_d3d_ns[m] = ns, prev_d3d_calls[m] = c;
        total += ms;
        if (calls > 0 && n < (int)sizeof(line) - 60)
            n += snprintf(line + n, sizeof(line) - n, " | %s %.2f (%.1fx)", d3d_name[m], ms, calls);
    }
    void* top_a[4] = {0};
    LONG64 top_d[4] = {0};
    for (int s = 0; s < SITE_SLOTS; s++) {
        void* a = g_d3d_sites.addr[s];
        if (!a)
            continue;
        LONG64 cur = g_d3d_sites.count[s], d = cur - g_d3d_prev_sites[s];
        g_d3d_prev_sites[s] = cur;
        for (int i = 0; i < 4; i++)
            if (d > top_d[i]) {
                for (int j = 3; j > i; j--)
                    top_a[j] = top_a[j - 1], top_d[j] = top_d[j - 1];
                top_a[i] = a, top_d[i] = d;
                break;
            }
    }
    if (total <= 0)
        return;
    LOGI("  main thread inside D3D calls per frame: %.1f ms%s", total, line);
    n = snprintf(line, sizeof(line), "    main thread D3D calls at:");
    for (int i = 0; i < 4 && top_a[i] && n < (int)sizeof(line) - 80; i++) {
        char at[64];
        n += snprintf(line + n, sizeof(line) - n, " %s %.2f ms", fmt_addr(top_a[i], at, sizeof(at)),
                      (double)top_d[i] / 1e6 / frames);
    }
    LOGI("%s", line);
}
