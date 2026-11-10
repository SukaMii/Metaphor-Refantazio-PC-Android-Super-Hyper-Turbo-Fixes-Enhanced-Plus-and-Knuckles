/* Integration test: loads the real MetaphorCPU.dll into this process. The DLL hooks
 * this EXE's import table exactly as it would hook METAPHOR.exe, so the hooks can
 * be exercised without starting the game.
 * Usage: host_test.exe <path-to-MetaphorCPU.dll> <path-to-test.ini> */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "../src/mcpu.h"

typedef void (*GetStats_t)(Stats*);
static int g_fail;

static void expect(int cond, const char* what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        g_fail++;
}

static DWORD WINAPI dummy(LPVOID p)
{
    (void)p;
    Sleep(10);
    return 0;
}

/* noinline: otherwise the compiler may load the Sleep IAT slot once for the whole
 * of main(), i.e. before the DLL installed its hook */
__attribute__((noinline)) static void sleep0_loop(int n)
{
    for (int i = 0; i < n; i++)
        Sleep(0);
}

__attribute__((noinline)) static double sleep1_avg_ms(int n)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < n; i++)
        Sleep(1);
    QueryPerformanceCounter(&b);
    return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / n;
}

static ULONGLONG thread_cpu_100ns(void)
{
    FILETIME c, e, k, u;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: host_test <MetaphorCPU.dll> <ini> [pc]\n");
        return 2;
    }
    /* "pc": the INI selects the PC profile; every CPU-side fix must stay off */
    int deck = argc > 3 && strcmp(argv[3], "deck") == 0;
    /* "deck": the Steam Deck profile, which has the PC profile's CPU side */
    int pc = (argc > 3 && strcmp(argv[3], "pc") == 0) || deck;
    SetEnvironmentVariableA("METAPHORCPU_INI", argv[2]);

    /* baseline: raw Sleep(0) loop cost before hooks */
    ULONGLONG c0 = thread_cpu_100ns();
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    sleep0_loop(20000);
    QueryPerformanceCounter(&b);
    double raw_ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    double raw_cpu = (thread_cpu_100ns() - c0) / 1e4;

    HMODULE dll = LoadLibraryA(argv[1]);
    expect(dll != NULL, "DLL loads and initialises");
    if (!dll)
        return 1;
    GetStats_t get = (GetStats_t)GetProcAddress(dll, "MetaphorCPU_GetStats");
    expect(get != NULL, "MetaphorCPU_GetStats exported");
    if (!get)
        return 1;
    Stats s0, s1;

    /* the game sleeps before it ever asks for a timer resolution (the engine's own requests
     * sit inside its usleep, which the mod replaces): on Windows a Sleep(1) then lasts a
     * whole 15.6 ms tick unless the mod requests the resolution itself */
    double s1ms = sleep1_avg_ms(20);
    printf("        Sleep(1) by the game: %.2f ms on average\n", s1ms);
    if (!pc)
        expect(s1ms < 5.0, "game Sleep(1) lasts ~1 ms: the mod set the timer resolution at the first sleep");

    get(&s0);
    for (int i = 0; i < 100; i++) {
        timeBeginPeriod(2);
        timeEndPeriod(2);
    }
    get(&s1);
    if (pc)
        expect(s1.period_suppressed - s0.period_suppressed == 0, "PC profile: timer calls passed through");
    else
        expect(s1.period_suppressed - s0.period_suppressed == 200, "timeBeginPeriod/timeEndPeriod churn suppressed (200 calls)");

    get(&s0);
    c0 = thread_cpu_100ns();
    QueryPerformanceCounter(&a);
    sleep0_loop(20000);
    QueryPerformanceCounter(&b);
    double hk_ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    double hk_cpu = (thread_cpu_100ns() - c0) / 1e4;
    get(&s1);
    expect(s1.sleep0_calls - s0.sleep0_calls == 20000, "Sleep(0) routed through hook");
    if (pc)
        expect(s1.sleep0_escalated - s0.sleep0_escalated == 0, "PC profile: Sleep(0) left as it is");
    else
        expect(s1.sleep0_escalated - s0.sleep0_escalated > 0, "Sleep(0) storm escalates to real sleeps");
    printf("        20000 x Sleep(0): raw %.1f ms wall / %.1f ms CPU  ->  hooked %.1f ms wall / %.1f ms CPU\n", raw_ms,
           raw_cpu, hk_ms, hk_cpu);

    DWORD_PTR proc, sys;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    get(&s0);
    DWORD_PTR r = SetThreadAffinityMask(GetCurrentThread(), 1); /* hooked: should be ignored (Mode=1) */
    get(&s1);
    typedef DWORD_PTR (WINAPI* STAM)(HANDLE, DWORD_PTR);
    STAM real = (STAM)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadAffinityMask");
    DWORD_PTR actual = real(GetCurrentThread(), proc); /* returns the mask actually in force */
    if (pc) {
        expect(r != 0 && s1.affinity_blocked - s0.affinity_blocked == 0, "PC profile: SetThreadAffinityMask passed through");
    } else {
        expect(r != 0 && s1.affinity_blocked - s0.affinity_blocked == 1, "game SetThreadAffinityMask intercepted");
        expect(actual != 1, "hard pinning to core 0 was not applied");
    }

    get(&s0);
    HANDLE t = CreateThread(NULL, 0, dummy, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CRITICAL_SECTION cs;
    InitializeCriticalSectionAndSpinCount(&cs, 1500);
    DeleteCriticalSection(&cs);
    get(&s1);
    expect(s1.threads_created - s0.threads_created == 1, "CreateThread tracked");
    expect(s1.cs_inits - s0.cs_inits == 1, "InitializeCriticalSectionAndSpinCount tracked");

    printf("  waiting 2.5 s for a profiler report...\n");
    Sleep(2500);
    char logp[MAX_PATH];
    GetModuleFileNameA(dll, logp, MAX_PATH);
    strcpy(strrchr(logp, '.'), ".log");
    HANDLE lf = CreateFileA(logp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    static char buf[1 << 16];
    DWORD got = 0;
    if (lf != INVALID_HANDLE_VALUE) {
        ReadFile(lf, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(lf);
    }
    buf[got] = 0;
    expect(strstr(buf, "CPU report") != NULL, "profiler wrote a CPU report to the log");
    if (pc) {
        if (deck)
            expect(strstr(buf, "Profile: Steam Deck (set in the INI) - the picture changes as on Android") != NULL,
                   "Steam Deck profile reported");
        else
            expect(strstr(buf, "Profile: PC (set in the INI)") != NULL, "PC profile reported");
        expect(strstr(buf, "Timer: resolution set") == NULL, "PC profile: timer resolution left to the game");
    } else {
        expect(strstr(buf, "Profile: Android (set in the INI)") != NULL, "Android profile reported");
        expect(strstr(buf, "Timer: resolution set to 1 ms at the game's first sleep") != NULL,
               "timer resolution requested at the first sleep");
        expect(strstr(buf, "Fast core mask") != NULL, "fast-core detection ran");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
