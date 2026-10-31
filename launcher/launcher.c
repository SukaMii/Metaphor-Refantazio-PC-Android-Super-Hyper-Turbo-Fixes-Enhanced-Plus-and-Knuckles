/* MetaphorCPU launcher: starts METAPHOR.exe and injects MetaphorCPU.dll from this
 * folder, so the mod can be tested without copying anything into the game folder.
 *
 * Injection happens at the game's entry point: the process is started suspended,
 * the entry point is temporarily turned into a 2-byte self-loop (in memory only),
 * the loader finishes initialising every static DLL (ASI loader, ReShade, ...),
 * then the DLL is loaded with a remote LoadLibraryW and the original bytes are
 * restored before the game's own code runs.
 *
 * Usage: MetaphorCPU_Launcher.exe [--game <METAPHOR.exe>] [--dll <MetaphorCPU.dll>] [--wait] [-- game args]
 * Without --game: METAPHOR.exe in the current folder.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

static const wchar_t* DEFAULT_GAME = L"METAPHOR.exe";

typedef LONG (NTAPI* NtQueryInformationProcess_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef struct {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
} PBI;

static int read_entry_rva(const wchar_t* exe, DWORD* rva)
{
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    DWORD got = 0;
    int ok = ReadFile(f, &dos, sizeof(dos), &got, NULL) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
             SetFilePointer(f, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
             ReadFile(f, &nt, sizeof(nt), &got, NULL) && got == sizeof(nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
             nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    CloseHandle(f);
    if (ok)
        *rva = nt.OptionalHeader.AddressOfEntryPoint;
    return ok;
}

static int inject(HANDLE proc, const wchar_t* dll)
{
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dll, bytes, NULL))
        return 0;
    LPTHREAD_START_ROUTINE ll =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(proc, NULL, 0, ll, remote, 0, NULL);
    if (!t) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    WaitForSingleObject(t, 60000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return code != 0;
}

int main(void)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t game[MAX_PATH], dll[MAX_PATH], extra[2048] = L"";
    int wait = 0;
    wcscpy(game, DEFAULT_GAME);

    GetModuleFileNameW(NULL, dll, MAX_PATH);
    wchar_t* slash = wcsrchr(dll, L'\\');
    wcscpy(slash ? slash + 1 : dll, L"MetaphorCPU.dll");

    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--game") && i + 1 < argc)
            wcsncpy(game, argv[++i], MAX_PATH - 1);
        else if (!wcscmp(argv[i], L"--dll") && i + 1 < argc)
            wcsncpy(dll, argv[++i], MAX_PATH - 1);
        else if (!wcscmp(argv[i], L"--wait"))
            wait = 1;
        else if (!wcscmp(argv[i], L"--")) {
            for (int j = i + 1; j < argc; j++) {
                wcsncat(extra, L" \"", 2047 - wcslen(extra));
                wcsncat(extra, argv[j], 2047 - wcslen(extra));
                wcsncat(extra, L"\"", 2047 - wcslen(extra));
            }
            break;
        }
    }
    game[MAX_PATH - 1] = dll[MAX_PATH - 1] = 0;
    wchar_t full_dll[MAX_PATH];
    GetFullPathNameW(dll, MAX_PATH, full_dll, NULL);

    if (GetFileAttributesW(full_dll) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"DLL not found: %ls\n", full_dll);
        return 1;
    }
    DWORD ep_rva = 0;
    if (!read_entry_rva(game, &ep_rva)) {
        fwprintf(stderr, L"Cannot read game executable: %ls\n", game);
        return 1;
    }

    wchar_t dir[MAX_PATH];
    wcscpy(dir, game);
    slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;

    wchar_t cmd[4096];
    _snwprintf(cmd, 4096, L"\"%ls\"%ls", game, extra);
    cmd[4095] = 0;

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    wprintf(L"Started %ls (pid %lu)\n", game, pi.dwProcessId);

    NtQueryInformationProcess_t nqip =
        (NtQueryInformationProcess_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    PBI pbi = {0};
    ULONG_PTR image_base = 0;
    if (!nqip || nqip(pi.hProcess, 0, &pbi, sizeof(pbi), NULL) != 0 ||
        !ReadProcessMemory(pi.hProcess, (BYTE*)pbi.PebBaseAddress + 0x10, &image_base, sizeof(image_base), NULL)) {
        fwprintf(stderr, L"Cannot locate image base; injecting while suspended.\n");
        int ok = inject(pi.hProcess, full_dll);
        wprintf(L"Injection %ls\n", ok ? L"OK" : L"FAILED");
        ResumeThread(pi.hThread);
        return ok ? 0 : 2;
    }

    BYTE* ep = (BYTE*)image_base + ep_rva;
    BYTE orig[2], loop[2] = {0xEB, 0xFE};
    DWORD old_prot = 0;
    int patched = ReadProcessMemory(pi.hProcess, ep, orig, 2, NULL) &&
                  VirtualProtectEx(pi.hProcess, ep, 2, PAGE_EXECUTE_READWRITE, &old_prot) &&
                  WriteProcessMemory(pi.hProcess, ep, loop, 2, NULL);
    FlushInstructionCache(pi.hProcess, ep, 2);

    int reached = 0;
    ResumeThread(pi.hThread);
    if (patched) {
        for (int i = 0; i < 300; i++) { /* up to 15 s */
            Sleep(50);
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                fwprintf(stderr, L"Game exited during start-up.\n");
                return 3;
            }
            SuspendThread(pi.hThread);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(pi.hThread, &ctx) && ctx.Rip == (DWORD64)ep) {
                reached = 1;
                break;
            }
            ResumeThread(pi.hThread);
        }
        if (!reached)
            SuspendThread(pi.hThread);
        BYTE now[2];
        if (ReadProcessMemory(pi.hProcess, ep, now, 2, NULL) && now[0] == 0xEB && now[1] == 0xFE)
            WriteProcessMemory(pi.hProcess, ep, orig, 2, NULL);
        DWORD tmp;
        VirtualProtectEx(pi.hProcess, ep, 2, old_prot, &tmp);
        FlushInstructionCache(pi.hProcess, ep, 2);
    } else {
        SuspendThread(pi.hThread);
    }
    wprintf(L"Entry point %ls; injecting %ls\n", reached ? L"reached" : L"not confirmed", full_dll);

    int ok = inject(pi.hProcess, full_dll);
    wprintf(L"Injection %ls\n", ok ? L"OK" : L"FAILED (game continues unmodified)");
    ResumeThread(pi.hThread);

    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        wprintf(L"Game exited with code %lu\n", code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LocalFree(argv);
    return ok ? 0 : 2;
}
