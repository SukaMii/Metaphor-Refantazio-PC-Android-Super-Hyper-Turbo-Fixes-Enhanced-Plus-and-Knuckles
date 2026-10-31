/* In-session A/B test: loads MetaphorCPU.dll with [ABTest] on, runs a workload that
 * spins on Sleep(0) and calls timeBeginPeriod, then checks the log: windows tagged
 * [AB A] must show the game's own behaviour (no escalation, timer calls passed
 * through) and [AB B] windows the fixes (escalation, timer calls suppressed).
 * "pc": the PC profile with the [PC] timer/spinlock fixes and Compare=cpu - B suppresses the
 * timer calls but leaves Sleep(0) alone.
 * An INI with Compare=overall: the same checks on its cycle, [Q0 game-as-is] as A, [Q1 mod-defaults] and
 * [Q2 mod-steady-image] as B. Compare=features: [Qn no-cpu-fixes] as A, its other variants as B.
 * Usage: ab_test.exe <MetaphorCPU.dll> <ini> [pc] */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_short_sleeps, g_long_sleeps; /* game Sleep(1): ~1-2 ms with the timer fix, a 15.6 ms tick without */

__attribute__((noinline)) static void workload_step(void)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    do { /* ~3 ms Sleep(0) wait loop, like the engine's job waits */
        Sleep(0);
        QueryPerformanceCounter(&b);
    } while ((b.QuadPart - a.QuadPart) * 1000 < 3 * f.QuadPart);
    timeBeginPeriod(2);
    timeEndPeriod(2);
    QueryPerformanceCounter(&a);
    Sleep(1);
    QueryPerformanceCounter(&b);
    double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    if (ms < 5.0)
        g_short_sleeps++;
    else if (ms > 10.0)
        g_long_sleeps++;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: ab_test <MetaphorCPU.dll> <ini>\n");
        return 2;
    }
    SetEnvironmentVariableA("METAPHORCPU_INI", argv[2]);
    int pc = argc > 3 && strcmp(argv[3], "pc") == 0;
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        printf("[FAIL] cannot load DLL\n");
        return 1;
    }
    DWORD end = GetTickCount() + 14500;
    while (GetTickCount() < end)
        workload_step();
    Sleep(300);

    char logp[MAX_PATH];
    GetModuleFileNameA(dll, logp, MAX_PATH);
    strcpy(strrchr(logp, '.'), ".log");
    FILE* lf = fopen(logp, "r");
    if (!lf) {
        printf("[FAIL] no log\n");
        return 1;
    }
    char line[2048];
    int mode = -1, a_ok = 0, a_bad = 0, b_ok = 0, b_bad = 0;
    while (fgets(line, sizeof(line), lf)) {
        if (strstr(line, "CPU report")) {
            mode = -1;
            if (strstr(line, "[AB A]") || strstr(line, "[Q0 game-as-is]") || strstr(line, " no-cpu-fixes]"))
                mode = 0;
            else if (strstr(line, "[AB B]") || (strstr(line, "[Q") && !strstr(line, " transition]")))
                mode = 1;
            continue;
        }
        const char* p = strstr(line, "per second: Sleep(0)");
        if (!p || mode < 0)
            continue;
        const char* e = strstr(line, "(escalated ");
        double esc = e ? atof(e + 11) : -1;
        int suppressed = strstr(line, "(suppressed)") != NULL;
        if (mode == 0)
            (esc == 0 && !suppressed) ? a_ok++ : a_bad++;
        else if (pc) /* PC profile: timer fix on, Sleep(0) never touched */
            (esc == 0 && suppressed) ? b_ok++ : b_bad++;
        else
            (esc > 0 && suppressed) ? b_ok++ : b_bad++;
    }
    fclose(lf);
    int fail = 0;
    printf("  A windows (game as-is): %d correct, %d wrong\n", a_ok, a_bad);
    printf("  B windows (fixes on):   %d correct, %d wrong\n", b_ok, b_bad);
    if (a_ok < 2 || a_bad) {
        printf("  [FAIL] side A must leave Sleep(0) and the timer calls untouched\n");
        fail++;
    } else {
        printf("  [PASS] side A leaves Sleep(0) and the timer calls untouched\n");
    }
    if (b_ok < 2 || b_bad) {
        printf("  [FAIL] side B must escalate Sleep(0) storms and suppress timer churn\n");
        fail++;
    } else {
        printf("  [PASS] side B %s and suppresses timer churn\n",
               pc ? "leaves Sleep(0) alone" : "escalates Sleep(0) storms");
    }
    /* side B keeps a 1 ms resolution; side A must give it back (the game's own timer use) */
    printf("  Sleep(1) by the game: %d short (<5 ms), %d a full tick (>10 ms)\n", g_short_sleeps, g_long_sleeps);
    if (g_short_sleeps < 20 || g_long_sleeps < 20) {
        printf("  [FAIL] short sleeps in B and full timer ticks in A expected\n");
        fail++;
    } else {
        printf("  [PASS] the timer resolution follows the halves (1 ms in B, the game's own in A)\n");
    }
    printf("\n%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
