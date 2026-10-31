/* Replacement for the engine's counting semaphore (job queues, file threads).
 *
 * Original: a critical section, a
 * manual-reset event and a handshake event. Every post that finds sleeping waiters
 * signals the manual-reset event (waking ALL of them) and then blocks, still holding
 * the critical section, until the last woken waiter acknowledges. With 5 workers per
 * job pool, one submitted job wakes 5 threads and stalls the submitting thread
 * (usually the main thread) for several thread wake-ups. Under Wine each event
 * operation is a round trip to the wineserver.
 *
 * This version keeps the engine's struct (count @0, max @4, waiters @8; the events
 * and critical section stay allocated but unused) and implements a futex-style
 * semaphore: post adds to the count and wakes exactly as many waiters as it added,
 * never blocking; wait takes a unit or sleeps in WaitOnAddress (a futex under Wine).
 * Startup option only ([JobSync] FastSemaphore): old- and new-style waiters cannot be
 * mixed, so it cannot be switched during an A/B session. */
#include "mcpu.h"

typedef struct GfdSem {
    volatile LONG count;
    LONG max;
    volatile LONG waiters;
    LONG pad;
    HANDLE ev_all;
    HANDLE ev_ack;
    CRITICAL_SECTION cs;
} GfdSem;

typedef BOOL (WINAPI* WaitOnAddress_t)(volatile VOID*, PVOID, SIZE_T, DWORD);
typedef VOID (WINAPI* WakeByAddress_t)(PVOID);
static WaitOnAddress_t pWait;
static WakeByAddress_t pWakeOne, pWakeAll;
static uint64_t g_qpf;

int jobsync_init(void)
{
    if (sizeof(GfdSem) != 0x48) /* must match the engine's allocation (create allocates 0x48) */
        return 0;
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    if (!kb)
        kb = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    if (!kb)
        return 0;
    pWait = (WaitOnAddress_t)GetProcAddress(kb, "WaitOnAddress");
    pWakeOne = (WakeByAddress_t)GetProcAddress(kb, "WakeByAddressSingle");
    pWakeAll = (WakeByAddress_t)GetProcAddress(kb, "WakeByAddressAll");
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (uint64_t)f.QuadPart;
    if (g_cfg.sem_spin < 0) {
        /* Polling before sleeping lets a worker that just finished a job pick up the
         * next one without a wake-up. Measured (tests\sem_test.c): a clear win with 4+
         * cores, harmful with 2 or fewer, where it steals time from the submitter. */
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        int cores = 0;
        for (; proc; proc &= proc - 1)
            cores++;
        g_cfg.sem_spin = cores >= 4 ? 256 : 32;
    }
    return pWait && pWakeOne && pWakeAll;
}

static int try_take(GfdSem* s)
{
    LONG c = s->count;
    while (c > 0) {
        LONG prev = InterlockedCompareExchange(&s->count, c - 1, c);
        if (prev == c)
            return 1;
        c = prev;
    }
    return 0;
}

/* timeout_us: 0 = infinite (engine convention). Returns 1, or -3 on timeout. */
int mcpu_sem_wait(void* sem, int timeout_us)
{
    GfdSem* s = (GfdSem*)sem;
    InterlockedIncrement64(&g_stats.sem_waits);
    for (int i = 0; i < g_cfg.sem_spin; i++) {
        if (try_take(s))
            return 1;
        YieldProcessor();
    }
    uint64_t deadline = 0;
    if (timeout_us > 0)
        deadline = qpc_now() + g_qpf * (uint64_t)timeout_us / 1000000ull;
    for (;;) {
        if (try_take(s))
            return 1;
        DWORD ms = 100; /* periodic re-check: a missed wake can never hang the game */
        if (deadline) {
            uint64_t now = qpc_now();
            if (now >= deadline)
                return -3;
            uint64_t left_ms = (deadline - now) * 1000ull / g_qpf + 1;
            if (left_ms < ms)
                ms = (DWORD)left_ms;
        }
        InterlockedIncrement(&s->waiters); /* full barrier, then re-check (pairs with post) */
        LONG seen = s->count;
        if (seen <= 0) {
            InterlockedIncrement64(&g_stats.sem_sleeps);
            pWait(&s->count, &seen, sizeof(LONG), ms);
        }
        InterlockedDecrement(&s->waiters);
    }
}

/* Returns 1, -1 if the count would exceed the maximum, -2 for a negative n. */
int mcpu_sem_post(void* sem, int n)
{
    GfdSem* s = (GfdSem*)sem;
    if (n < 0)
        return -2;
    LONG c = s->count;
    for (;;) {
        if (c + n > s->max)
            return -1;
        LONG prev = InterlockedCompareExchange(&s->count, c + n, c);
        if (prev == c)
            break;
        c = prev;
    }
    InterlockedIncrement64(&g_stats.sem_posts);
    if (n > 0 && s->waiters > 0) { /* CAS above is a full barrier */
        InterlockedIncrement64(&g_stats.sem_wakes);
        if (n == 1)
            pWakeOne((PVOID)&s->count);
        else
            pWakeAll((PVOID)&s->count);
    }
    return 1;
}
