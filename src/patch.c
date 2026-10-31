/* Minimal code patching: absolute jumps, a trampoline for a known prologue, and IAT hooks. */
#include "mcpu.h"
#include <string.h>

int patch_bytes(void* dst, const void* src, size_t n)
{
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}

/* jmp qword ptr [rip+0]; dq target  (14 bytes). Remaining room is filled with int3. */
int patch_abs_jmp(void* at, void* to, size_t room)
{
    uint8_t buf[32];
    if (room < 14 || room > sizeof(buf))
        return 0;
    memset(buf, 0xCC, room);
    buf[0] = 0xFF;
    buf[1] = 0x25;
    memset(buf + 2, 0, 4);
    memcpy(buf + 6, &to, 8);
    return patch_bytes(at, buf, room);
}

/* Copy `prologue_len` position-independent bytes and append a jump back. */
void* make_trampoline(const uint8_t* orig, size_t prologue_len)
{
    uint8_t* t = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
        return NULL;
    memcpy(t, orig, prologue_len);
    const uint8_t* back = orig + prologue_len;
    t[prologue_len + 0] = 0xFF;
    t[prologue_len + 1] = 0x25;
    memset(t + prologue_len + 2, 0, 4);
    memcpy(t + prologue_len + 6, &back, 8);
    DWORD old;
    VirtualProtect(t, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 64);
    return t;
}

/* Replace every IAT slot of `mod` that imports `func` (by name, any DLL). */
int iat_hook(HMODULE mod, const char* func, void* hook, void** orig_out)
{
    uint8_t* base = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress)
        return 0;
    int hits = 0;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; imp++) {
        if (!imp->OriginalFirstThunk)
            continue;
        IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA64* slots = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) != 0)
                continue;
            void* cur = (void*)slots->u1.Function;
            if (cur == hook)
                continue;
            if (orig_out && hits == 0)
                *orig_out = cur; /* chain to whatever the slot pointed at (system or another hook) */
            uint64_t v = (uint64_t)hook;
            if (patch_bytes(&slots->u1.Function, &v, sizeof(v)))
                hits++;
        }
    }
    return hits;
}
