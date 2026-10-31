/* Minimal "game" for testing MetaphorCPU_Launcher end to end. It imports the same
 * kind of APIs METAPHOR.exe does and sits next to mock_winmm (as winmm.dll), whose
 * exports only work after main() starts, like an ASI-loader proxy.
 * Exit code 0 = the injected DLL was active and hooked this EXE. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include "../src/mcpu.h"

__attribute__((noinline)) static void spin(int n)
{
    for (int i = 0; i < n; i++)
        Sleep(0);
}

static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    spin(1000);
    return 0;
}

int main(void)
{
    typedef void (*init_t)(void);
    init_t init = (init_t)GetProcAddress(GetModuleHandleA("winmm.dll"), "proxy_init");
    if (init)
        init(); /* the proxy becomes usable only now */
    printf("[mock] winmm proxy %s\n", init ? "initialised in main()" : "NOT found (test setup error)");

    HMODULE m = GetModuleHandleA("MetaphorCPU.dll");
    printf("[mock] MetaphorCPU.dll present when main() starts: %s\n", m ? "yes" : "no");

    for (int i = 0; i < 100; i++) {
        timeBeginPeriod(2);
        timeEndPeriod(2);
    }
    spin(5000);
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    typedef void (*GetStats_t)(Stats*);
    GetStats_t gs = m ? (GetStats_t)GetProcAddress(m, "MetaphorCPU_GetStats") : NULL;
    Stats s;
    ZeroMemory(&s, sizeof(s));
    if (gs)
        gs(&s);
    printf("[mock] hooks saw: timer-period calls %lld, Sleep(0) %lld, threads created %lld\n", s.period_suppressed,
           s.sleep0_calls, s.threads_created);
    int ok = m && init && s.period_suppressed >= 200 && s.sleep0_calls >= 6000 && s.threads_created >= 1;
    printf("[mock] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
