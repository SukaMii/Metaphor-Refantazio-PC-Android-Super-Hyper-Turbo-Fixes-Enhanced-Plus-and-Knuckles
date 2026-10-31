/* Lock-free call-site histograms: which game code locations spin on Sleep(0),
 * escalate to real sleeps, or hit a contended spinlock. */
#include "mcpu.h"
#include <stdio.h>
#include <string.h>

SiteTable g_sites_yield, g_sites_escalated, g_sites_lock, g_sites_sleepn;

void site_add(SiteTable* t, void* addr, LONG64 n)
{
    unsigned h = (unsigned)(((uintptr_t)addr >> 2) * 2654435761u) & (SITE_SLOTS - 1);
    for (int i = 0; i < 32; i++) {
        unsigned s = (h + i) & (SITE_SLOTS - 1);
        void* cur = t->addr[s];
        if (cur == addr) {
            InterlockedAdd64(&t->count[s], n);
            return;
        }
        if (!cur) {
            void* prev = InterlockedCompareExchangePointer((PVOID volatile*)&t->addr[s], addr, NULL);
            if (!prev || prev == addr) {
                InterlockedAdd64(&t->count[s], n);
                return;
            }
        }
    }
}

void site_report(const char* title, SiteTable* t, LONG64* prev, double seconds, int top)
{
    enum { MAXTOP = 8 };
    void* addr[MAXTOP];
    LONG64 delta[MAXTOP];
    int n = 0;
    LONG64 total = 0;
    if (top > MAXTOP)
        top = MAXTOP;
    for (int s = 0; s < SITE_SLOTS; s++) {
        void* a = t->addr[s];
        if (!a)
            continue;
        LONG64 c = t->count[s];
        LONG64 d = c - prev[s];
        prev[s] = c;
        if (d <= 0)
            continue;
        total += d;
        int pos = n < top ? n++ : top;
        if (pos == top) {
            if (d <= delta[top - 1])
                continue;
            pos = top - 1;
        }
        while (pos > 0 && delta[pos - 1] < d) {
            addr[pos] = addr[pos - 1];
            delta[pos] = delta[pos - 1];
            pos--;
        }
        addr[pos] = a;
        delta[pos] = d;
    }
    if (!n || seconds <= 0)
        return;
    LOGI("  %s (%.0f/s total):", title, total / seconds);
    for (int i = 0; i < n; i++) {
        char at[64], who[80] = "";
        uintptr_t a = (uintptr_t)addr[i];
        int idx = (int)(a >> 48); /* thread index packed by timing.c, 0 = none */
        void* site = (void*)(a & 0xFFFFFFFFFFFFull);
        if (idx) {
            DWORD tid = timing_index_tid(idx);
            char name[64];
            if (tid && threads_lookup(tid, name, sizeof(name), NULL))
                snprintf(who, sizeof(who), "  [%s]", name);
            else if (tid)
                snprintf(who, sizeof(who), "  [tid %lu]", tid);
        }
        LOGI("      %8.0f/s  %s%s", delta[i] / seconds, fmt_addr(site, at, sizeof(at)), who);
    }
}
