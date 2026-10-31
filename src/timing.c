/* Timer-resolution churn removal, adaptive Sleep(0), precise sub-millisecond sleeps. */
#include "mcpu.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

VOID (WINAPI* o_Sleep)(DWORD) = NULL;
MMRESULT (WINAPI* o_timeBeginPeriod)(UINT) = NULL;
MMRESULT (WINAPI* o_timeEndPeriod)(UINT) = NULL;

typedef LONG (NTAPI* NtDelayExecution_t)(BOOLEAN, PLARGE_INTEGER);
static NtDelayExecution_t pNtDelayExecution;

typedef struct PerThread {
    uint64_t last_yield;
    uint64_t episode_start;
    int timer_tried;
    int registered;
    int index;          /* 1-based slot in g_pt_list, 0 if not registered */
    HANDLE hr_timer;
    DWORD tid;
    volatile LONG dead;
    volatile LONG64 yields;     /* written only by the owning thread */
    volatile LONG64 escalated;
} PerThread;

#define MAX_PT 512
static PerThread* volatile g_pt_list[MAX_PT];
static volatile LONG g_pt_count;

static DWORD g_tls = TLS_OUT_OF_INDEXES;
static uint64_t g_qpf;
static uint64_t g_window_ticks;
static uint64_t g_spin_ticks;
static volatile LONG g_period_set;

uint64_t qpc_now(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}

double qpc_to_us(uint64_t ticks)
{
    return (double)ticks * 1e6 / (double)g_qpf;
}

static PerThread* pt_get(void)
{
    if (g_tls == TLS_OUT_OF_INDEXES)
        return NULL;
    PerThread* p = (PerThread*)TlsGetValue(g_tls);
    if (!p) {
        p = (PerThread*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
        if (p) {
            p->tid = GetCurrentThreadId();
            LONG idx = InterlockedIncrement(&g_pt_count) - 1;
            if (idx < MAX_PT) {
                g_pt_list[idx] = p;
                p->registered = 1;
                p->index = (int)idx + 1;
            }
            TlsSetValue(g_tls, p);
        }
    }
    return p;
}

void timing_thread_detach(void)
{
    if (g_tls == TLS_OUT_OF_INDEXES)
        return;
    PerThread* p = (PerThread*)TlsGetValue(g_tls);
    if (!p)
        return;
    if (p->hr_timer)
        CloseHandle(p->hr_timer);
    p->hr_timer = NULL;
    TlsSetValue(g_tls, NULL);
    if (p->registered)
        p->dead = 1; /* the profiler may still read it; keep the memory */
    else
        HeapFree(GetProcessHeap(), 0, p);
}

int timing_thread_index(void)
{
    PerThread* p = pt_get();
    return p ? p->index : 0;
}

DWORD timing_index_tid(int index)
{
    if (index < 1 || index > MAX_PT || index > g_pt_count)
        return 0;
    PerThread* p = g_pt_list[index - 1];
    return p ? p->tid : 0;
}

/* Site key carrying the calling thread: return address in the low 48 bits, the
 * thread's 1-based index above (decoded by site_report). */
static void* thread_site(void* ret, PerThread* pt)
{
    uintptr_t idx = pt ? (uintptr_t)pt->index : 0;
    return (void*)(((uintptr_t)ret & 0xFFFFFFFFFFFFull) | (idx << 48));
}

int timing_thread_yields(DWORD tid, LONG64* yields, LONG64* escalated)
{
    LONG n = g_pt_count;
    if (n > MAX_PT)
        n = MAX_PT;
    for (LONG i = 0; i < n; i++) {
        PerThread* p = g_pt_list[i];
        if (p && p->tid == tid && !p->dead) {
            *yields = p->yields;
            *escalated = p->escalated;
            return 1;
        }
    }
    return 0;
}

static void raw_sleep0(void)
{
    if (o_Sleep)
        o_Sleep(0);
    else
        Sleep(0);
}

void precise_sleep_us(DWORD us)
{
    if (us == 0) {
        raw_sleep0();
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;

    /* Wine: NtDelayExecution is a select() with microsecond precision. */
    if (!g_is_wine) {
        PerThread* pt = pt_get();
        if (pt && !pt->timer_tried) {
            pt->timer_tried = 1;
            pt->hr_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
        }
        if (pt && pt->hr_timer && SetWaitableTimerEx(pt->hr_timer, &due, 0, NULL, NULL, NULL, 0)) {
            WaitForSingleObject(pt->hr_timer, INFINITE);
            return;
        }
    }
    if (pNtDelayExecution)
        pNtDelayExecution(FALSE, &due);
    else if (o_Sleep)
        o_Sleep((us + 999) / 1000);
    else
        Sleep((us + 999) / 1000);
}

static void sleep_impl(DWORD ms, void* ret);
static void ensure_period(void);

VOID WINAPI hk_Sleep(DWORD ms)
{
    void* ret = __builtin_return_address(0);
    if (!g_cfg.wait_profile) {
        sleep_impl(ms, ret);
        return;
    }
    uint64_t t0 = qpc_now();
    sleep_impl(ms, ret);
    DWORD e = GetLastError();
    waitprof_add(ms ? WP_SLEEP : WP_YIELD, ret, qpc_now() - t0);
    SetLastError(e);
}

static void sleep_impl(DWORD ms, void* ret)
{
    if (ms != 0) {
        InterlockedIncrement64(&g_stats.sleepn_calls);
        DWORD e = GetLastError();
        site_add(&g_sites_sleepn, thread_site(ret, pt_get()), 1);
        if (g_cfg.fix_timer_churn && !g_period_set)
            ensure_period();
        SetLastError(e);
        o_Sleep(ms);
        return;
    }
    InterlockedIncrement64(&g_stats.sleep0_calls);
    DWORD err = GetLastError();
    PerThread* pt = pt_get();
    if (pt) {
        pt->yields++;
        if ((pt->yields & 15) == 0)
            site_add(&g_sites_yield, thread_site(ret, pt), 16); /* sampled: keeps the hot path cheap */
    }
    if (!g_cfg.adaptive_yield || !pt) {
        o_Sleep(0);
        SetLastError(err);
        return;
    }
    uint64_t now = qpc_now();
    if (now - pt->last_yield > g_window_ticks)
        pt->episode_start = now; /* new waiting episode */
    if (now - pt->episode_start >= g_spin_ticks) {
        /* the thread has been spinning on Sleep(0) for a while: stop burning the core */
        InterlockedIncrement64(&g_stats.sleep0_escalated);
        pt->escalated++;
        site_add(&g_sites_escalated, thread_site(ret, pt), 1);
        precise_sleep_us((DWORD)g_cfg.yield_sleep_us);
    } else {
        if (g_cfg.yield_mode == 1) {
            if (!SwitchToThread())
                YieldProcessor();
        } else {
            o_Sleep(0);
        }
    }
    pt->last_yield = qpc_now();
    SetLastError(err);
}

/* The global resolution is requested on the game's first own call, never during
 * DLL initialisation: when injected at the entry point, winmm may still be an
 * uninitialised proxy (e.g. Ultimate ASI Loader) at that moment. */
static void ensure_period(void)
{
    if (g_period_set || g_cfg.timer_period_ms <= 0)
        return;
    MMRESULT(WINAPI * begin)(UINT) = o_timeBeginPeriod;
    if (!begin) { /* the game does not import it: winmm is initialised by now (the game is running) */
        HMODULE w = GetModuleHandleA("winmm.dll");
        begin = w ? (MMRESULT(WINAPI*)(UINT))(void*)GetProcAddress(w, "timeBeginPeriod") : NULL;
    }
    if (!begin || InterlockedExchange(&g_period_set, 1))
        return;
    MMRESULT r = begin((UINT)g_cfg.timer_period_ms);
    LOGI("Timer: resolution set to %d ms at the game's first sleep (result %u); per-call changes suppressed",
         g_cfg.timer_period_ms, r);
}

/* A/B: back to the engine's own timer use (side A) - give the permanent resolution back;
 * the next sleep with the fix on requests it again */
void timing_period_release(void)
{
    if (!g_period_set)
        return;
    MMRESULT(WINAPI * end)(UINT) = o_timeEndPeriod;
    if (!end) {
        HMODULE w = GetModuleHandleA("winmm.dll");
        end = w ? (MMRESULT(WINAPI*)(UINT))(void*)GetProcAddress(w, "timeEndPeriod") : NULL;
    }
    if (end && g_cfg.timer_period_ms > 0)
        end((UINT)g_cfg.timer_period_ms);
    InterlockedExchange(&g_period_set, 0);
}

MMRESULT WINAPI hk_timeBeginPeriod(UINT p)
{
    InterlockedIncrement64(&g_stats.period_calls);
    if (g_cfg.fix_timer_churn) {
        ensure_period();
        InterlockedIncrement64(&g_stats.period_suppressed);
        return 0; /* TIMERR_NOERROR */
    }
    return o_timeBeginPeriod ? o_timeBeginPeriod(p) : 0;
}

MMRESULT WINAPI hk_timeEndPeriod(UINT p)
{
    InterlockedIncrement64(&g_stats.period_calls);
    if (g_cfg.fix_timer_churn) {
        InterlockedIncrement64(&g_stats.period_suppressed);
        return 0;
    }
    return o_timeEndPeriod ? o_timeEndPeriod(p) : 0;
}

/* Replacement for the engine's usleep(us). The original wraps every call in
 * timeBeginPeriod(2)/timeEndPeriod(2) and rounds up to whole milliseconds. */
static void usleep_impl(int us, void* ret)
{
    if (!g_cfg.fix_timer_churn && !g_cfg.precise_usleep) {
        /* A/B side A: the engine's original behaviour */
        DWORD n = us <= 0 ? 0 : ((DWORD)us + 999) / 1000;
        hk_timeBeginPeriod(2);
        if (n)
            o_Sleep(n);
        else
            sleep_impl(0, ret);
        hk_timeEndPeriod(2);
        return;
    }
    if (us <= 0) {
        sleep_impl(0, ret);
        return;
    }
    /* The engine's usleep was the game's only caller of timeBeginPeriod: with it replaced,
     * the resolution must be requested here, or every Sleep(1) on Windows lasts a whole
     * 15.6 ms tick (Wine's Sleep is ~1 ms either way). */
    if (!g_period_set)
        ensure_period();
    if (g_cfg.precise_usleep)
        precise_sleep_us((DWORD)us);
    else
        o_Sleep(((DWORD)us + 999) / 1000);
}

void mcpu_usleep(int us)
{
    /* entered by a jmp from the patched engine function: this is the game's call site */
    void* ret = __builtin_return_address(0);
    InterlockedIncrement64(&g_stats.usleep_calls);
    site_add(&g_sites_sleepn, thread_site(ret, pt_get()), 1);
    if (!g_cfg.wait_profile) {
        usleep_impl(us, ret);
        return;
    }
    uint64_t t0 = qpc_now();
    usleep_impl(us, ret);
    waitprof_add(WP_SLEEP, ret, qpc_now() - t0);
}

typedef struct {
    ULONG Version, ControlMask, StateMask;
} PowerThrottling;

/* Only touches kernel32/ntdll: safe to run inside DllMain at the entry point. */
void timing_init(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (uint64_t)f.QuadPart;
    g_window_ticks = g_qpf * (uint64_t)(g_cfg.yield_window_us > 0 ? g_cfg.yield_window_us : 30) / 1000000ull;
    /* Wine sleeps via select() (tens of microseconds); Windows bottoms out near 0.5 ms
     * even with high-resolution timers, so only escalate long waits there. */
    if (g_cfg.yield_spin_us < 0)
        g_cfg.yield_spin_us = g_is_wine ? 250 : 2000;
    g_spin_ticks = g_qpf * (uint64_t)g_cfg.yield_spin_us / 1000000ull;
    if (g_cfg.adaptive_yield)
        LOGI("Adaptive yield: Sleep(0) loops escalate after %d us to %d us sleeps (%s)", g_cfg.yield_spin_us,
             g_cfg.yield_sleep_us, g_cfg.yield_mode == 1 ? "SwitchToThread before that" : "Sleep(0) before that");
    if (g_tls == TLS_OUT_OF_INDEXES)
        g_tls = TlsAlloc();

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    pNtDelayExecution = (NtDelayExecution_t)GetProcAddress(ntdll, "NtDelayExecution");
    if (!o_Sleep)
        o_Sleep = Sleep;

    /* Windows 11: keep honouring the timer resolution and opt out of EcoQoS
     * (stops the game being parked on E-cores when the window is not focused). */
    typedef BOOL (WINAPI* SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);
    SetProcessInformation_t spi =
        (SetProcessInformation_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (spi && !g_is_wine) {
        PowerThrottling pt = {1, 0x1 | 0x4, 0}; /* EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, state off */
        BOOL ok = spi(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pt, sizeof(pt));
        LOGI("Power throttling opt-out: %s", ok ? "ok" : "not supported");
    }
}
