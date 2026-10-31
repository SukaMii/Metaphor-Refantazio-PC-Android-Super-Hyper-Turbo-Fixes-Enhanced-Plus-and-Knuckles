/* Globals normally defined by core.c, for tests that link individual modules. */
#include "../src/mcpu.h"
#include <stdio.h>

HMODULE g_self;
int g_is_wine;
Stats g_stats;

uint8_t* game_base(void) { return NULL; }
size_t game_size(void) { return 0; }

const char* fmt_addr(void* p, char* buf, size_t cap)
{
    snprintf(buf, cap, "%p", p);
    return buf;
}

DWORD threads_main_tid(void) { return 0; }

int threads_lookup(DWORD tid, char* name, size_t cap, int* kind)
{
    (void)tid;
    (void)name;
    (void)cap;
    (void)kind;
    return 0;
}
