/* Replacement for the engine's spinlock.
 *
 * Original: up to 1500 back-to-back `lock cmpxchg`
 * without PAUSE, then Sleep(0), forever. The lock word is a 32-bit int,
 * 0 = free, 1 = held; release is `xchg [lock], 0`.
 *
 * This version keeps the same lock word protocol (so any inlined copy of the
 * acquire or release elsewhere still interoperates) and does:
 *   1. test-and-test-and-set with PAUSE and capped exponential backoff,
 *   2. a couple of SwitchToThread() yields,
 *   3. WaitOnAddress() with a short timeout. Release wakes one waiter when the
 *      address' hash bucket has waiters. The timeout makes a missed wake
 *      harmless (e.g. if some inlined release does not call us). */
#include "mcpu.h"

typedef BOOL (WINAPI* WaitOnAddress_t)(volatile VOID*, PVOID, SIZE_T, DWORD);
typedef VOID (WINAPI* WakeByAddressSingle_t)(PVOID);

static WaitOnAddress_t pWaitOnAddress;
static WakeByAddressSingle_t pWakeByAddressSingle;

#define WAIT_BUCKETS 256
static volatile LONG g_waiters[WAIT_BUCKETS];

static inline unsigned bucket_of(volatile LONG* p)
{
    return (unsigned)((((uintptr_t)p >> 2) * 2654435761u) >> 24) & (WAIT_BUCKETS - 1);
}

void spinlock_init(void)
{
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    if (!kb)
        kb = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
    if (kb) {
        pWaitOnAddress = (WaitOnAddress_t)GetProcAddress(kb, "WaitOnAddress");
        pWakeByAddressSingle = (WakeByAddressSingle_t)GetProcAddress(kb, "WakeByAddressSingle");
    }
    if (!pWaitOnAddress || !pWakeByAddressSingle) {
        pWaitOnAddress = NULL;
        pWakeByAddressSingle = NULL;
    }
}

LONG mcpu_spin_lock(volatile LONG* l)
{
    if (InterlockedCompareExchange(l, 1, 0) == 0)
        return 0;

    InterlockedIncrement64(&g_stats.lock_contended);
    /* the patched function is entered by a jmp, so this is the game's call site */
    site_add(&g_sites_lock, __builtin_return_address(0), 1);
    if (!g_cfg.replace_spinlock) {
        /* A/B side A: behave exactly like the engine's own lock */
        for (;;) {
            for (int n = 1500; n > 0; n--)
                if (InterlockedCompareExchange(l, 1, 0) == 0)
                    return 0;
            hk_Sleep(0);
        }
    }
    const int iters = g_cfg.spin_iters;
    for (int round = 0;; round++) {
        unsigned backoff = 1;
        for (int i = 0; i < iters; i++) {
            if (*l == 0 && InterlockedCompareExchange(l, 1, 0) == 0)
                return 0;
            for (unsigned p = 0; p < backoff; p++)
                YieldProcessor();
            if (backoff < 16)
                backoff <<= 1;
        }
        if (round < g_cfg.spin_yields) {
            SwitchToThread();
            continue;
        }
        InterlockedIncrement64(&g_stats.lock_waits);
        if (!pWaitOnAddress) {
            precise_sleep_us(50);
            continue;
        }
        volatile LONG* w = &g_waiters[bucket_of(l)];
        InterlockedIncrement(w);
        LONG held = 1;
        if (*l == 1)
            pWaitOnAddress(l, &held, sizeof(LONG), (DWORD)g_cfg.spin_wait_ms);
        InterlockedDecrement(w);
    }
}

LONG mcpu_spin_unlock(volatile LONG* l)
{
    LONG old = InterlockedExchange(l, 0);
    if (pWakeByAddressSingle && g_waiters[bucket_of(l)] > 0)
        pWakeByAddressSingle((PVOID)l);
    return old;
}
