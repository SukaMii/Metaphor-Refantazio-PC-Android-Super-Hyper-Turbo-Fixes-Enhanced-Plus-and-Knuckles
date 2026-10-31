/* Job-queue benchmark and correctness test for the semaphore replacement.
 *
 * Rebuilds the engine's bounded-buffer job queue (items + free-slot semaphores and a
 * queue lock) and runs it with
 *   (1) a faithful copy of the engine's semaphore:
 *       critical section, manual-reset "wake all" event, handshake event, timer churn
 *   (2) MetaphorCPU's replacement (jobsync.c)
 * One producer (the "main thread") submits jobs to 5 workers. Checks every job runs
 * exactly once and reports wall time, time the producer is blocked submitting, and CPU.
 * Usage: sem_test.exe [jobs=20000] [cores=0 (all)] */
#include "../src/mcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- engine original */
typedef struct OrigSem {
    LONG count, max;
    volatile LONG waiters;
    LONG pad;
    HANDLE ev_all, ev_ack;
    CRITICAL_SECTION cs;
} OrigSem;

static void orig_create(OrigSem* s, LONG init, LONG max)
{
    s->count = init;
    s->max = max;
    s->waiters = 0;
    s->ev_all = CreateEventA(NULL, TRUE, FALSE, NULL);
    s->ev_ack = CreateEventA(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSectionAndSpinCount(&s->cs, 1500);
}

static int orig_wait(OrigSem* s, int timeout_us)
{
    EnterCriticalSection(&s->cs);
    while (s->count < 1) {
        s->waiters++;
        DWORD ms = timeout_us ? (DWORD)((timeout_us + 999) / 1000) : INFINITE;
        LeaveCriticalSection(&s->cs);
        timeBeginPeriod(2);
        DWORD r = WaitForSingleObject(s->ev_all, ms);
        timeEndPeriod(2);
        if (r == WAIT_TIMEOUT) {
            EnterCriticalSection(&s->cs);
            LeaveCriticalSection(&s->cs);
            return -3;
        }
        if (InterlockedExchangeAdd(&s->waiters, -1) == 1) {
            ResetEvent(s->ev_all);
            SetEvent(s->ev_ack);
        }
        EnterCriticalSection(&s->cs);
    }
    s->count--;
    if (s->waiters > 0) {
        SetEvent(s->ev_all);
        WaitForSingleObject(s->ev_ack, INFINITE);
    }
    LeaveCriticalSection(&s->cs);
    return 1;
}

static int orig_post(OrigSem* s, int n)
{
    if (n < 0)
        return -2;
    EnterCriticalSection(&s->cs);
    LONG c = s->count + n;
    if (c > s->max) {
        LeaveCriticalSection(&s->cs);
        return -1;
    }
    s->count = c;
    if (s->waiters > 0) {
        SetEvent(s->ev_all);
        WaitForSingleObject(s->ev_ack, INFINITE);
    }
    LeaveCriticalSection(&s->cs);
    return 1;
}

/* ---------------------------------------------------------------- job queue */
typedef int (*wait_fn)(void*, int);
typedef int (*post_fn)(void*, int);

#define QSIZE 64
#define WORKERS 5
typedef struct Queue {
    void* items;  /* starts 0 */
    void* slots;  /* starts QSIZE */
    volatile LONG lock;
    int head, tail;
    int job[QSIZE];
    wait_fn wait;
    post_fn post;
    volatile LONG stop;
} Queue;

static volatile LONG* g_done; /* per-job execution counter */
static volatile uint64_t g_sink;

/* queue lock = the engine spinlock as patched by MetaphorCPU (spin -> yield -> WaitOnAddress) */
static void q_lock(volatile LONG* l) { mcpu_spin_lock(l); }
static void q_unlock(volatile LONG* l) { mcpu_spin_unlock(l); }

static void work(int n)
{
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < n; i++)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    g_sink += x;
}

static DWORD WINAPI worker(LPVOID p)
{
    Queue* q = (Queue*)p;
    for (;;) {
        q->wait(q->items, 0);
        q_lock(&q->lock);
        int j = q->job[q->head];
        q->head = (q->head + 1) % QSIZE;
        q_unlock(&q->lock);
        if (j < 0)
            return 0;            /* stop token */
        work(2000);              /* ~ a few microseconds of job work */
        InterlockedIncrement(&g_done[j]);
        q->post(q->slots, 1);    /* slot released after the job ran, as in the engine */
    }
}

static double push(Queue* q, int j)
{
    uint64_t t0 = qpc_now();
    q->wait(q->slots, 0);
    q_lock(&q->lock);
    q->job[q->tail] = j;
    q->tail = (q->tail + 1) % QSIZE;
    q_unlock(&q->lock);
    q->post(q->items, 1);
    return qpc_to_us(qpc_now() - t0);
}

static ULONGLONG proc_cpu_100ns(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

/* opaque wrappers so both implementations share one queue */
static int o_wait(void* s, int t) { return orig_wait((OrigSem*)s, t); }
static int o_post(void* s, int n) { return orig_post((OrigSem*)s, n); }

/* engine-struct-compatible object for the replacement: same 0x48-byte layout */
static void* new_sem(LONG init, LONG max)
{
    OrigSem* s = (OrigSem*)calloc(1, sizeof(OrigSem));
    orig_create(s, init, max); /* the replacement uses count/max/waiters only */
    return s;
}

static int run(const char* name, int fast, int jobs)
{
    Queue q;
    memset(&q, 0, sizeof(q));
    q.items = new_sem(0, QSIZE);
    q.slots = new_sem(QSIZE, QSIZE);
    q.wait = fast ? mcpu_sem_wait : o_wait;
    q.post = fast ? mcpu_sem_post : o_post;
    g_done = (volatile LONG*)calloc(jobs, sizeof(LONG));
    HANDLE th[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        th[i] = CreateThread(NULL, 0, worker, &q, 0, NULL);
    Sleep(50);

    Stats s0 = g_stats;
    ULONGLONG cpu0 = proc_cpu_100ns();
    uint64_t t0 = qpc_now();
    double blocked = 0, worst = 0;
    for (int j = 0; j < jobs; j++) {
        double us = push(&q, j);
        blocked += us;
        if (us > worst)
            worst = us;
        if ((j & 7) == 7)
            work(6000); /* the producer also does its own work between submissions */
    }
    for (int i = 0; i < WORKERS; i++)
        push(&q, -1);
    WaitForMultipleObjects(WORKERS, th, TRUE, 30000);
    double wall = qpc_to_us(qpc_now() - t0) / 1000.0;
    double cpu = (proc_cpu_100ns() - cpu0) / 1e4;

    int bad = 0;
    for (int j = 0; j < jobs; j++)
        if (g_done[j] != 1)
            bad++;
    printf("  %-22s wall %8.1f ms | producer blocked %7.1f ms (worst %6.0f us) | CPU %8.1f ms | %s\n", name,
           wall, blocked / 1000.0, worst, cpu, bad ? "JOBS LOST/DUPLICATED" : "all jobs ran exactly once");
    if (fast) {
        Stats s = g_stats;
        printf("  %-22s %lld waits, %lld slept, %lld posts, %lld woke a sleeper\n", "", s.sem_waits - s0.sem_waits,
               s.sem_sleeps - s0.sem_sleeps, s.sem_posts - s0.sem_posts, s.sem_wakes - s0.sem_wakes);
    }
    for (int i = 0; i < WORKERS; i++)
        CloseHandle(th[i]);
    free((void*)g_done);
    return bad;
}

int main(int argc, char** argv)
{
    int jobs = argc > 1 ? atoi(argv[1]) : 20000;
    int cores = argc > 2 ? atoi(argv[2]) : 0;
    config_defaults(&g_cfg);
    g_cfg.log_level = 0;
    g_cfg.sem_spin = argc > 3 ? atoi(argv[3]) : -1; /* polls before sleeping, -1 = auto */
    timing_init();
    spinlock_init();
    if (cores > 0) {
        DWORD_PTR proc, sys, mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        for (int i = 0, n = 0; i < 64 && n < cores; i++)
            if (proc >> i & 1) {
                mask |= (DWORD_PTR)1 << i;
                n++;
            }
        SetProcessAffinityMask(GetCurrentProcess(), mask);
    }
    if (!jobsync_init()) {
        printf("WaitOnAddress not available\n");
        return 2;
    }
    printf("auto/configured polls before sleeping: %d\n", g_cfg.sem_spin);
    printf("Job queue: %d jobs, %d workers, queue %d, %s\n\n", jobs, WORKERS, QSIZE,
           cores ? "restricted cores" : "all cores");
    int fail = 0;
    fail += run("engine semaphore", 0, jobs);
    fail += run("MetaphorCPU semaphore", 1, jobs);

    /* API contract: error codes and timeout */
    void* s = new_sem(0, 2);
    uint64_t t0 = qpc_now();
    int r = mcpu_sem_wait(s, 20000); /* 20 ms */
    double ms = qpc_to_us(qpc_now() - t0) / 1000.0;
    int ok = r == -3 && ms >= 18 && ms < 200;
    ok &= mcpu_sem_post(s, -1) == -2;
    ok &= mcpu_sem_post(s, 2) == 1 && mcpu_sem_post(s, 1) == -1; /* would exceed max 2 */
    ok &= mcpu_sem_wait(s, 0) == 1 && mcpu_sem_wait(s, 0) == 1 && *(volatile LONG*)s == 0;
    printf("\n  API contract (timeout -3 after %.0f ms, -2, -1, counts): %s\n", ms, ok ? "OK" : "FAIL");
    fail += !ok;
    printf("\n%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
