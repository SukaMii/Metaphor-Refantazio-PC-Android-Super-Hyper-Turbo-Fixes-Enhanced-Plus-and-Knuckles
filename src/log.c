#include "mcpu.h"
#include <stdarg.h>
#include <stdio.h>

static HANDLE g_log = INVALID_HANDLE_VALUE, g_log_copy = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static int g_log_ready;

void log_open(const char* path)
{
    if (!g_log_ready) {
        InitializeCriticalSection(&g_log_cs);
        g_log_ready = 1;
    }
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
}

/* [General] LogCopy: every line also to `path`; a Linux path (/...) goes through Wine's own name conversion */
int log_copy_open(const char* path)
{
    if (path[0] == '/') {
        typedef WCHAR*(CDECL * dos_t)(const char*);
        dos_t dos = (dos_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name");
        WCHAR* w = dos ? dos(path) : NULL;
        if (w)
            g_log_copy = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        g_log_copy = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return g_log_copy != INVALID_HANDLE_VALUE;
}

/* <game folder>\<OutputFolder>\MetaphorCPU_<kind>_<HHMMSS>, created: where captures, compare shots
 * and mesh censuses go (OutputFolder may be absolute; empty = next to the game) */
void output_dir(char* out, size_t cap, const char* kind)
{
    const char* f = g_cfg.output_folder;
    size_t n;
    if (f[0] && (f[1] == ':' || (f[0] == '\\' && f[1] == '\\'))) {
        snprintf(out, cap, "%s", f);
    } else {
        GetModuleFileNameA(NULL, out, (DWORD)cap);
        char* sl = strrchr(out, '\\');
        if (sl)
            *sl = 0;
        if (f[0]) {
            n = strlen(out);
            snprintf(out + n, cap - n, "\\%s", f);
        }
    }
    CreateDirectoryA(out, NULL);
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    n = strlen(out);
    snprintf(out + n, cap - n, "\\MetaphorCPU_%s_%02d%02d%02d", kind, tm.wHour, tm.wMinute, tm.wSecond);
    CreateDirectoryA(out, NULL);
}

void log_msg(int level, const char* fmt, ...)
{
    if (level > g_cfg.log_level)
        return;
    char buf[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, level == 0 ? "ERROR " : "");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 3)
        n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;

    if (g_log_ready)
        EnterCriticalSection(&g_log_cs);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(g_log, buf, (DWORD)n, &w, NULL);
        if (g_log_copy != INVALID_HANDLE_VALUE)
            WriteFile(g_log_copy, buf, (DWORD)n, &w, NULL);
    } else {
        buf[n - 2] = '\n';
        buf[n - 1] = 0;
        fputs(buf, stdout);
    }
    OutputDebugStringA(buf);
    if (g_log_ready)
        LeaveCriticalSection(&g_log_cs);
}
