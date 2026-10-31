#include "mcpu.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        mcpu_init(inst, NULL);
        break;
    case DLL_THREAD_DETACH:
        timing_thread_detach();
        break;
    }
    return TRUE;
}
