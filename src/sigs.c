/* Byte-signature resolution of the engine functions we patch.
 * Works on any image laid out at its virtual addresses (the live game module,
 * or a file mapped section-by-section by tests/scan_test.c). */
#include "mcpu.h"
#include <stdio.h>
#include <string.h>

/* gfd spinlock acquire (1500 x lock cmpxchg, then Sleep(0)) followed by the
 * 5-byte release `xor eax,eax; xchg [rcx],eax; ret` and int3 padding. */
static const char SIG_SPINLOCK[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 BF 01 00 00 00 BA DC 05 00 00 33 C0 F0 0F B1 3B "
    "74 10 FF CA 85 D2 7F F2 33 C9 FF 15 ?? ?? ?? ?? EB E3 48 8B 5C 24 30 48 83 C4 20 5F C3 "
    "CC CC CC CC CC CC 33 C0 87 01 C3 CC CC CC CC CC CC CC CC CC CC CC";

/* gfd usleep: timeBeginPeriod(2); Sleep((us + 999) / 1000); timeEndPeriod(2) */
static const char SIG_USLEEP[] =
    "40 53 48 83 EC 20 8B D9 B9 02 00 00 00 FF 15 ?? ?? ?? ?? 33 C9 B8 D3 4D 62 10 F7 EB C1 FA "
    "06 8B C2 C1 E8 1F 03 D0 69 C2 E8 03 00 00 3B D8 0F 95 C1 03 CA FF 15 ?? ?? ?? ?? B9 02 00 "
    "00 00 48 83 C4 20 5B 48 FF 25";

/* gfd yield: if (GetCurrentThreadId() == mainTid && TEB->FiberData) fiberYield(); Sleep(0) */
static const char SIG_YIELD[] =
    "48 83 EC 28 FF 15 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 75 13 65 48 8B 04 25 20 00 00 00 48 85 C0 "
    "74 05 E8 ?? ?? ?? ?? 33 C9 48 83 C4 28 48 FF 25";

/* gfd thread create(entry, arg, prio, stackSize, flags, name) -> CreateThread + SetThreadPriority.
 * Only a jump into the protected code is left there: not found, and left alone. */
static const char SIG_THREAD_CREATE[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 4C 8B F1 "
    "49 63 F1 B9 20 00 00 00 41 8B D8 48 8B EA E8";

/* job system init: four pools created with (queue, threads) = (0x2000,5) (0x2800,5) (0x20,1) (0x2000,1) */
static const char SIG_JOB_INIT[] =
    "41 54 48 83 EC 60 BA ?? 00 00 00 B9 00 20 00 00 E8 ?? ?? ?? ?? BA ?? 00 00 00 48 89 05 ?? "
    "?? ?? ?? B9 00 28 00 00 E8";

/* semaphore create: allocates 0x48 bytes, count @0 = initial, max @4, waiters @8 = 0 */
static const char SIG_SEM_CREATE[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B D9 8B FA B9 48 00 00 00 E8 ?? ?? ?? ?? 45 33 C9 45 33 "
    "C0 33 C9 48 8B F0 89 18 41 8D 51 01 89 78 04 C7 40 08 00 00 00 00";

/* semaphore wait(sem, timeout_us): EnterCS; if count >= 1 take; else ++waiters, wait manual-reset event */
static const char SIG_SEM_WAIT[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B F1 44 8B FA 48 83 C1 20 "
    "FF 15 ?? ?? ?? ?? 8B 06 83 F8 01 0F 8D ?? ?? ?? ?? FF 46 08 45 85 FF 75 07 BB FF FF FF FF";

/* semaphore post(sem, n): n < 0 -> -2; count + n > max -> -1; wake all + wait for handshake */
static const char SIG_SEM_POST[] =
    "48 89 5C 24 10 56 48 83 EC 20 8B F2 48 8B D9 85 D2 79 10 B8 FE FF FF FF 48 8B 5C 24 38 48 83 C4 20 5E "
    "C3 48 83 C1 20 48 89 7C 24 30 FF 15 ?? ?? ?? ?? 8B 0B 03 CE 3B 4B 04";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "48 8B ?? .." into bytes + mask. Returns length. */
static size_t parse_pattern(const char* p, uint8_t* bytes, uint8_t* mask, size_t cap)
{
    size_t n = 0;
    while (*p && n < cap) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (p[0] == '?') {
            bytes[n] = 0;
            mask[n] = 0;
            p += (p[1] == '?') ? 2 : 1;
        } else {
            int hi = hexval(p[0]), lo = hexval(p[1]);
            if (hi < 0 || lo < 0)
                return 0;
            bytes[n] = (uint8_t)(hi << 4 | lo);
            mask[n] = 1;
            p += 2;
        }
        n++;
    }
    return n;
}

uint8_t* sig_find(uint8_t* start, size_t len, const char* pattern, int* count)
{
    uint8_t bytes[256], mask[256];
    size_t n = parse_pattern(pattern, bytes, mask, sizeof(bytes));
    uint8_t* first = NULL;
    int found = 0;
    if (!n || len < n)
        goto done;
    for (size_t i = 0; i + n <= len; i++) {
        if (start[i] != bytes[0])
            continue;
        size_t j = 1;
        while (j < n && (!mask[j] || start[i + j] == bytes[j]))
            j++;
        if (j == n) {
            if (!first)
                first = start + i;
            found++;
        }
    }
done:
    if (count)
        *count = found;
    return first;
}

int sig_text_range(uint8_t* base, uint8_t** start, size_t* len)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    /* the game's code: .ecode (Denuvo's own code is in .text1 and .xcode: the whole name is compared),
     * or .text */
    static const char* const names[2] = {".text", ".ecode"};
    for (int n = 0; n < 2; n++)
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            char nm[9] = {0};
            memcpy(nm, sec[i].Name, 8);
            if (strcmp(nm, names[n]) == 0) {
                *start = base + sec[i].VirtualAddress;
                *len = sec[i].Misc.VirtualSize;
                return 1;
            }
        }
    return 0;
}

static uint8_t* find_one(uint8_t* text, size_t len, const char* name, const char* sig, uint8_t* base,
                         int verbose)
{
    int count = 0;
    uint8_t* p = sig_find(text, len, sig, &count);
    if (verbose)
        LOGI("  sig %-14s %s (matches=%d) rva=0x%llx", name, count == 1 ? "OK  " : "FAIL", count,
             p ? (unsigned long long)(p - base) : 0ull);
    return count == 1 ? p : NULL;
}

int sig_resolve(uint8_t* base, Targets* t, int verbose)
{
    uint8_t* text;
    size_t len;
    memset(t, 0, sizeof(*t));
    if (!sig_text_range(base, &text, &len))
        return 0;

    t->spin_lock = find_one(text, len, "spinlock", SIG_SPINLOCK, base, verbose);
    if (t->spin_lock)
        t->spin_unlock = t->spin_lock + 0x40;
    t->usleep = find_one(text, len, "usleep", SIG_USLEEP, base, verbose);
    t->yield = find_one(text, len, "yield", SIG_YIELD, base, verbose);
    if (t->yield) {
        int32_t disp;
        memcpy(&disp, t->yield + 12, 4);
        t->main_tid = (volatile DWORD*)(t->yield + 16 + disp);
    }
    t->thread_create = find_one(text, len, "thread_create", SIG_THREAD_CREATE, base, verbose);
    t->job_init = find_one(text, len, "job_init", SIG_JOB_INIT, base, verbose);
    t->sem_create = find_one(text, len, "sem_create", SIG_SEM_CREATE, base, verbose);
    t->sem_wait = find_one(text, len, "sem_wait", SIG_SEM_WAIT, base, verbose);
    t->sem_post = find_one(text, len, "sem_post", SIG_SEM_POST, base, verbose);

    return (t->spin_lock != NULL) + (t->usleep != NULL) + (t->yield != NULL) + (t->thread_create != NULL) +
           (t->job_init != NULL) + (t->sem_create != NULL) + (t->sem_wait != NULL) + (t->sem_post != NULL);
}
