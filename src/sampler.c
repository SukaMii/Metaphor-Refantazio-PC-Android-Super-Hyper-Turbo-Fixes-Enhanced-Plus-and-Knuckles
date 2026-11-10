/* Opt-in sampling profiler ([Sampler] Hz). Periodically suspends the main thread and
 * the busiest other threads, records the instruction pointer and the nearest return
 * address inside METAPHOR.exe, then resumes. Shows what a busy thread is executing
 * (e.g. an unexplained wine_threadpool_worker, or where the main thread spends its
 * frame). While a thread is suspended only syscalls are made (context + stack copy),
 * so a suspended lock holder cannot deadlock the sampler. */
#include "mcpu.h"
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#define SLOTS 6
#define TOPN 6
#define MAX_SECTS 20

/* the game's sections: its own code against the protector's (.text1, .xcode) */
static struct {
    char name[9];
    uint8_t *start, *end;
} g_sect[MAX_SECTS];
static int g_nsect;

typedef struct Target {
    DWORD tid;
    HANDLE h;
    char name[72];
    SiteTable leaf;
    SiteTable game;
    SiteTable lib;  /* first non-system, non-game module on the stack (DXVK, steam_api64, ...) */
    LONG64 samples;
    LONG64 in_game; /* samples whose leaf is in METAPHOR.exe */
    LONG64 in_sect[MAX_SECTS];
} Target;

static Target g_t[SLOTS];
static int g_nt;
static SRWLOCK g_lock = SRWLOCK_INIT;
static uint8_t *g_text, *g_text_end;

/* loaded modules, classified once per profiler window (outside any suspension) */
#define MAX_MODS 256
typedef struct Mod {
    uint8_t* base;
    uint8_t* end;
    int skip; /* system/runtime/emulator/our own module: not interesting as a caller */
} Mod;
static Mod g_mods[MAX_MODS];
static int g_nmods;

static void refresh_modules(void)
{
    static const char* skip[] = {"ntdll", "kernel32", "kernelbase", "ucrtbase", "msvcrt", "win32u", "libarm64ec",
                                 "xtajit", "wow64", "metaphorcpu", "vcruntime", "msvcp", "user32", "gdi32",
                                 "combase", "rpcrt4", "sechost", "advapi32"};
    HMODULE mods[MAX_MODS];
    DWORD need = 0;
    g_nmods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
        return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > MAX_MODS)
        n = MAX_MODS;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi;
        char name[64];
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)))
            continue;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name)))
            name[0] = 0;
        for (char* c = name; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c += 32;
        Mod* m = &g_mods[g_nmods++];
        m->base = (uint8_t*)mi.lpBaseOfDll;
        m->end = m->base + mi.SizeOfImage;
        m->skip = (m->base == game_base());
        for (unsigned k = 0; k < sizeof(skip) / sizeof(skip[0]) && !m->skip; k++)
            if (strncmp(name, skip[k], strlen(skip[k])) == 0)
                m->skip = 1;
    }
}

static int interesting_module_address(uint64_t v)
{
    for (int i = 0; i < g_nmods; i++)
        if ((uint8_t*)v >= g_mods[i].base && (uint8_t*)v < g_mods[i].end)
            return !g_mods[i].skip;
    return 0;
}

#define MAX_OWN 8
static volatile DWORD g_own[MAX_OWN];
static volatile LONG g_nown;

void mcpu_register_own_thread(DWORD tid)
{
    LONG i = InterlockedIncrement(&g_nown) - 1;
    if (i < MAX_OWN)
        g_own[i] = tid;
}

int mcpu_is_own_thread(DWORD tid)
{
    for (LONG i = 0; i < g_nown && i < MAX_OWN; i++)
        if (g_own[i] == tid)
            return 1;
    return 0;
}

static void* func_start(void* pc)
{
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)pc, &base, NULL);
    if (f)
        return (void*)(base + f->BeginAddress);
    return (void*)((uintptr_t)pc & ~(uintptr_t)0xFF); /* no unwind info: 256-byte bucket */
}

/* Is `v` a plausible return address into the game's .text (preceded by a call)? */
static int game_return_address(uint64_t v)
{
    uint8_t* p = (uint8_t*)v;
    if (p < g_text + 8 || p >= g_text_end)
        return 0;
    if (p[-5] == 0xE8)
        return 1; /* call rel32 */
    if (p[-6] == 0xFF && p[-5] == 0x15)
        return 1; /* call [rip+disp32] */
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)
        return 1; /* call reg / [reg] */
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
        return 1; /* call [reg+disp8] */
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)
        return 1; /* call [reg+disp32] */
    return 0;
}

static void sample_one(Target* t)
{
    if (!t->h)
        return;
    CONTEXT ctx;
    static uint64_t stack[1024]; /* 8 KB; only the sampler thread uses it */
    SIZE_T got = 0;
    if (SuspendThread(t->h) == (DWORD)-1)
        return;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    BOOL ok = GetThreadContext(t->h, &ctx);
    if (ok) {
        /* the stack may be shallower than 8 KB: fall back to smaller copies */
        static const SIZE_T sizes[] = {sizeof(stack), 2048, 512};
        for (int i = 0; i < 3 && !got; i++)
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)ctx.Rsp, stack, sizes[i], &got))
                got = 0;
    }
    ResumeThread(t->h);
    if (!ok)
        return;

    /* everything below runs with the target already resumed */
    t->samples++;
    void* leaf = (void*)ctx.Rip;
    for (int s = 0; s < g_nsect; s++)
        if ((uint8_t*)leaf >= g_sect[s].start && (uint8_t*)leaf < g_sect[s].end) {
            t->in_sect[s]++;
            break;
        }
    site_add(&t->leaf, func_start(leaf), 1);
    int have_game = 0, have_lib = 0;
    if ((uint8_t*)leaf >= g_text && (uint8_t*)leaf < g_text_end) {
        t->in_game++;
        site_add(&t->game, func_start(leaf), 1);
        have_game = 1;
    } else if (interesting_module_address((uint64_t)leaf)) {
        site_add(&t->lib, func_start(leaf), 1);
        have_lib = 1;
    }
    for (SIZE_T i = 0; i < got / 8 && !(have_game && have_lib); i++) {
        if (!have_game && game_return_address(stack[i])) {
            site_add(&t->game, func_start((void*)stack[i]), 1);
            have_game = 1;
        } else if (!have_lib && !have_game && interesting_module_address(stack[i])) {
            /* a library frame below the first game frame: what the game is waiting in */
            site_add(&t->lib, func_start((void*)stack[i]), 1);
            have_lib = 1;
        }
    }
}

static DWORD WINAPI sampler_thread(LPVOID unused)
{
    (void)unused;
    mcpu_register_own_thread(GetCurrentThreadId());
    DWORD period = 1000u / (DWORD)g_cfg.sampler_hz;
    if (!period)
        period = 1;
    for (;;) {
        Sleep(period);
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_nt; i++)
            sample_one(&g_t[i]);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return 0;
}

void sampler_start(void)
{
    if (g_cfg.sampler_hz <= 0)
        return;
    uint8_t* text;
    size_t len;
    if (sig_text_range(game_base(), &text, &len)) {
        g_text = text;
        g_text_end = text + len;
    }
    uint8_t* base = (uint8_t*)game_base();
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && g_nsect < MAX_SECTS; i++) {
        memcpy(g_sect[g_nsect].name, sec[i].Name, 8);
        g_sect[g_nsect].name[8] = 0;
        g_sect[g_nsect].start = base + sec[i].VirtualAddress;
        g_sect[g_nsect].end = base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
        g_nsect++;
    }
    HANDLE h = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_HIGHEST); /* sample on time, it is very short */
        CloseHandle(h);
    }
    LOGI("Sampler: %d Hz on the main thread + %d busiest threads (diagnostic; small overhead)", g_cfg.sampler_hz,
         g_cfg.sampler_threads);
}

void sampler_set_targets(const DWORD* tids, const char (*names)[72], int n)
{
    if (g_cfg.sampler_hz <= 0)
        return;
    AcquireSRWLockExclusive(&g_lock);
    refresh_modules();
    for (int i = 0; i < g_nt; i++)
        if (g_t[i].h)
            CloseHandle(g_t[i].h);
    memset(g_t, 0, sizeof(g_t));
    g_nt = 0;
    for (int i = 0; i < n && g_nt < SLOTS; i++) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
        if (!h)
            continue;
        Target* t = &g_t[g_nt++];
        t->tid = tids[i];
        t->h = h;
        snprintf(t->name, sizeof(t->name), "%s", names[i]);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static void print_top(const char* label, SiteTable* st, LONG64 total)
{
    void* addr[TOPN];
    LONG64 cnt[TOPN];
    int n = 0;
    for (int s = 0; s < SITE_SLOTS; s++) {
        if (!st->addr[s] || st->count[s] <= 0)
            continue;
        LONG64 c = st->count[s];
        int pos = n < TOPN ? n++ : TOPN;
        if (pos == TOPN) {
            if (c <= cnt[TOPN - 1])
                continue;
            pos = TOPN - 1;
        }
        while (pos > 0 && cnt[pos - 1] < c) {
            addr[pos] = addr[pos - 1];
            cnt[pos] = cnt[pos - 1];
            pos--;
        }
        addr[pos] = st->addr[s];
        cnt[pos] = c;
    }
    char line[640];
    int k = snprintf(line, sizeof(line), "      %s:", label);
    for (int i = 0; i < n && k < (int)sizeof(line) - 80; i++) {
        char at[64];
        k += snprintf(line + k, sizeof(line) - k, " %s %.0f%%%s", fmt_addr(addr[i], at, sizeof(at)),
                      cnt[i] * 100.0 / total, i + 1 < n ? "," : "");
    }
    if (n)
        LOGI("%s", line);
}

void sampler_report(void)
{
    if (g_cfg.sampler_hz <= 0)
        return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_nt; i++) {
        Target* t = &g_t[i];
        if (!t->samples)
            continue;
        LOGI("  samples: tid %lu '%s' (%lld samples, %.0f%% directly in game code)", t->tid, t->name, t->samples,
             t->in_game * 100.0 / t->samples);
        char where[240];
        int w = snprintf(where, sizeof(where), "      where:");
        LONG64 inside = 0;
        for (int s = 0; s < g_nsect; s++) {
            inside += t->in_sect[s];
            if (t->in_sect[s] && w < (int)sizeof(where) - 40)
                w += snprintf(where + w, sizeof(where) - w, " %s %.0f%%,", g_sect[s].name, t->in_sect[s] * 100.0 / t->samples);
        }
        snprintf(where + w, sizeof(where) - w, " outside the game %.0f%%", (t->samples - inside) * 100.0 / t->samples);
        LOGI("%s", where);
        memset(t->in_sect, 0, sizeof(t->in_sect));
        print_top("executing", &t->leaf, t->samples);
        print_top("game function on the stack", &t->game, t->samples);
        print_top("library it is in (DXVK, steam, ...)", &t->lib, t->samples);
        memset(&t->leaf, 0, sizeof(t->leaf));
        memset(&t->game, 0, sizeof(t->game));
        memset(&t->lib, 0, sizeof(t->lib));
        t->samples = t->in_game = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
}
