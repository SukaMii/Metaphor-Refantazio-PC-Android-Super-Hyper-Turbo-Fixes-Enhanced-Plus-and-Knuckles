/* Checks whether high-resolution waitable timers work on this system and how
 * short a sleep can really be. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

typedef LONG (NTAPI* NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);

static double measure(HANDLE t, LONGLONG us)
{
    LARGE_INTEGER f, a, b, due;
    QueryPerformanceFrequency(&f);
    double sum = 0;
    for (int i = 0; i < 50; i++) {
        due.QuadPart = -us * 10;
        QueryPerformanceCounter(&a);
        SetWaitableTimerEx(t, &due, 0, NULL, NULL, NULL, 0);
        WaitForSingleObject(t, INFINITE);
        QueryPerformanceCounter(&b);
        sum += (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart;
    }
    return sum / 50;
}

int main(void)
{
    NtQueryTimerResolution_t q =
        (NtQueryTimerResolution_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    ULONG mn, mx, cur;
    if (q && q(&mn, &mx, &cur) == 0)
        printf("Timer resolution: current %.3f ms (min %.3f, max %.3f)\n", cur / 1e4, mx / 1e4, mn / 1e4);

    HANDLE hr = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    printf("High-resolution waitable timer: %s (error %lu)\n", hr ? "created" : "NOT available", hr ? 0 : GetLastError());
    HANDLE lr = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    LONGLONG req[] = {50, 100, 250, 500, 1000};
    for (int i = 0; i < 5; i++)
        printf("  request %5lld us -> high-res %7.0f us | normal %7.0f us\n", req[i], hr ? measure(hr, req[i]) : -1.0,
               measure(lr, req[i]));
    return 0;
}
