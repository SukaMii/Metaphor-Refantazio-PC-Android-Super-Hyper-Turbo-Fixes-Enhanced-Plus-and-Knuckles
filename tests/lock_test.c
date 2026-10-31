/* Benchmarks the engine's original spinlock / Sleep(0) waiting against the
 * MetaphorCPU replacements, under CPU oversubscription (more busy threads than
 * cores), which is the normal situation on a phone running Box64 + Wine.
 *
 * Usage: lock_test.exe [cores=2] [threads=8] [iters=100000]
 */
#include "../src/mcpu.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- exact behaviour of the engine's spinlock */
static void orig_lock(volatile LONG* l)
{
    for (;;) {
        for (int n = 1500; n > 0; n--)
            if (InterlockedCompareExchange(l, 1, 0) == 0)
                return;
        Sleep(0);
    }
}
static void orig_unlock(volatile LONG* l) { InterlockedExchange(l, 0); }

static void new_lock(volatile LONG* l) { mcpu_spin_lock(l); }
static void new_unlock(volatile LONG* l) { mcpu_spin_unlock(l); }

typedef struct Run {
    void (*lock)(volatile LONG*);
    void (*unlock)(volatile LONG*);
    volatile LONG lockword;
    volatile LONG64 counter;
    volatile LONG go;
    int iters;
    int cs_work;
    int out_work;
} Run;

static volatile uint64_t g_sink;

static void work(int n)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < n; i++)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    g_sink += x;
}

static DWORD WINAPI worker(LPVOID p)
{
    Run* r = (Run*)p;
    while (!r->go)
        YieldProcessor();
    for (int i = 0; i < r->iters; i++) {
        r->lock(&r->lockword);
        r->counter++;          /* protected only by the lock: detects broken mutual exclusion */
        work(r->cs_work);      /* critical section */
        r->unlock(&r->lockword);
        work(r->out_work);     /* work outside the lock */
    }
    return 0;
}

static ULONGLONG proc_cpu_100ns(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static void bench(const char* name, void (*lk)(volatile LONG*), void (*ul)(volatile LONG*), int threads,
                  int iters, int cs_work, int out_work)
{
    static Run r;
    r.lock = lk;
    r.unlock = ul;
    r.lockword = 0;
    r.counter = 0;
    r.go = 0;
    r.iters = iters;
    r.cs_work = cs_work;
    r.out_work = out_work;
    HANDLE h[64];
    for (int i = 0; i < threads; i++)
        h[i] = CreateThread(NULL, 0, worker, &r, 0, NULL);
    Sleep(50);
    ULONGLONG cpu0 = proc_cpu_100ns();
    uint64_t t0 = qpc_now();
    r.go = 1;
    WaitForMultipleObjects(threads, h, TRUE, INFINITE);
    double wall_ms = qpc_to_us(qpc_now() - t0) / 1000.0;
    double cpu_ms = (proc_cpu_100ns() - cpu0) / 1e4;
    for (int i = 0; i < threads; i++)
        CloseHandle(h[i]);
    LONG64 expect = (LONG64)threads * iters;
    printf("  %-24s wall %8.1f ms | CPU %8.1f ms | %6.0f ops/ms | %s\n", name, wall_ms, cpu_ms,
           expect / wall_ms, r.counter == expect ? "mutual exclusion OK" : "COUNTER MISMATCH");
}

/* ---- waiting for another thread with Sleep(0), as the engine's job waits do */
typedef struct Flag {
    volatile LONG done;
    int work_iters;
} Flag;

static DWORD WINAPI producer(LPVOID p)
{
    Flag* f = (Flag*)p;
    work(f->work_iters);
    InterlockedExchange(&f->done, 1);
    return 0;
}

static void wait_test(const char* name, VOID (WINAPI* sleep_fn)(DWORD), int waiter_prio, int rounds,
                      int work_iters)
{
    double worst = 0, total = 0;
    ULONGLONG cpu0 = proc_cpu_100ns();
    int timeouts = 0;
    SetThreadPriority(GetCurrentThread(), waiter_prio);
    for (int i = 0; i < rounds; i++) {
        Flag f = {0, work_iters};
        HANDLE h = CreateThread(NULL, 0, producer, &f, 0, NULL);
        uint64_t t0 = qpc_now();
        while (!f.done) {
            sleep_fn(0);
            if (qpc_to_us(qpc_now() - t0) > 3e6) { /* 3 s cap */
                timeouts++;
                break;
            }
        }
        double us = qpc_to_us(qpc_now() - t0);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        total += us;
        if (us > worst)
            worst = us;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    double cpu_ms = (proc_cpu_100ns() - cpu0) / 1e4;
    printf("  %-24s avg wait %8.0f us | worst %9.0f us | CPU %8.1f ms%s\n", name, total / rounds, worst, cpu_ms,
           timeouts ? " | TIMED OUT (starved)" : "");
}

static VOID WINAPI raw_sleep(DWORD ms) { Sleep(ms); }

int main(int argc, char** argv)
{
    int cores = argc > 1 ? atoi(argv[1]) : 2;
    int threads = argc > 2 ? atoi(argv[2]) : 8;
    int iters = argc > 3 ? atoi(argv[3]) : 100000;
    if (threads > 64)
        threads = 64;

    config_defaults(&g_cfg);
    g_cfg.log_level = 0;
    timing_init();
    spinlock_init();
    o_Sleep = Sleep;

    DWORD_PTR proc, sys;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    DWORD_PTR mask = 0;
    for (int i = 0, n = 0; i < 64 && n < cores; i++)
        if (proc >> i & 1) {
            mask |= (DWORD_PTR)1 << i;
            n++;
        }
    SetProcessAffinityMask(GetCurrentProcess(), mask);
    printf("Process restricted to %d core(s) (mask 0x%llx), %d threads x %d lock/unlock\n\n", cores,
           (unsigned long long)mask, threads, iters);

    printf("[0] Sleep precision (what an escalated Sleep(0) or engine usleep really costs)\n");
    {
        DWORD req[] = {50, 250, 1000};
        for (int k = 0; k < 3; k++) {
            double sum = 0, worst = 0;
            for (int i = 0; i < 50; i++) {
                uint64_t t0 = qpc_now();
                precise_sleep_us(req[k]);
                double us = qpc_to_us(qpc_now() - t0);
                sum += us;
                if (us > worst)
                    worst = us;
            }
            uint64_t t0 = qpc_now();
            for (int i = 0; i < 20; i++)
                Sleep((req[k] + 999) / 1000);
            double legacy = qpc_to_us(qpc_now() - t0) / 20;
            printf("  request %4lu us: precise avg %7.0f us (worst %6.0f) | Sleep(ceil ms) avg %7.0f us\n", req[k],
                   sum / 50, worst, legacy);
        }
        printf("\n");
    }

    printf("[1a] Spinlock, light contention (short critical section, long work outside)\n");
    bench("original 1500xCAS+Sleep0", orig_lock, orig_unlock, threads, iters, 40, 400);
    bench("MetaphorCPU lock", new_lock, new_unlock, threads, iters, 40, 400);
    printf("[1b] Spinlock, heavy contention (long critical section, little work outside)\n");
    bench("original 1500xCAS+Sleep0", orig_lock, orig_unlock, threads, iters / 4, 400, 40);
    bench("MetaphorCPU lock", new_lock, new_unlock, threads, iters / 4, 400, 40);
    Stats s = g_stats;
    printf("  replacement: %lld contended acquisitions, %lld blocked waits\n\n", s.lock_contended, s.lock_waits);

    /* one core: waiter and producer share it */
    SetProcessAffinityMask(GetCurrentProcess(), mask & (~mask + 1));
    printf("[2] Sleep(0) wait for a job on ONE core, equal priority (200 rounds, ~0.3 ms job)\n");
    wait_test("raw Sleep(0)", raw_sleep, THREAD_PRIORITY_NORMAL, 200, 30000);
    wait_test("adaptive Sleep(0)", hk_Sleep, THREAD_PRIORITY_NORMAL, 200, 30000);
    printf("\n[3] Same, waiter ABOVE_NORMAL (e.g. boosted main thread) -> priority inversion check\n");
    wait_test("raw Sleep(0)", raw_sleep, THREAD_PRIORITY_ABOVE_NORMAL, 3, 30000);
    wait_test("adaptive Sleep(0)", hk_Sleep, THREAD_PRIORITY_ABOVE_NORMAL, 200, 30000);
    s = g_stats;
    printf("  adaptive: %lld Sleep(0) calls, %lld escalated to %d us sleeps after %d us of spinning\n",
           s.sleep0_calls, s.sleep0_escalated, g_cfg.yield_sleep_us, g_cfg.yield_spin_us);
    return 0;
}
