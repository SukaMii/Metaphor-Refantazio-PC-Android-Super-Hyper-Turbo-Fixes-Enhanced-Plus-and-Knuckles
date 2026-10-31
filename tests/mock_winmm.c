/* Stand-in for a winmm.dll proxy such as Ultimate ASI Loader: its exports forward
 * through pointers that are only filled in once the game's own code starts
 * (proxy_init, called from mock_game's main). Calling them earlier jumps to NULL,
 * which is exactly how MetaphorCPU 0.1 crashed when injected at the entry point. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned (__stdcall* period_fn)(unsigned);
static period_fn g_begin, g_end;

__declspec(dllexport) void proxy_init(void)
{
    WCHAR path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\winmm.dll");
    HMODULE real = LoadLibraryW(path);
    g_begin = (period_fn)GetProcAddress(real, "timeBeginPeriod");
    g_end = (period_fn)GetProcAddress(real, "timeEndPeriod");
}

__declspec(dllexport) unsigned __stdcall timeBeginPeriod(unsigned p) { return g_begin(p); }
__declspec(dllexport) unsigned __stdcall timeEndPeriod(unsigned p) { return g_end(p); }
