/* Call-redirect test for the shadow-culling hooks: a snippet shaped like the engine's
 * bucket executor (node in rbx, command stream in rcx, `call interpreter`) is redirected
 * through a stub in this EXE's int3 padding. The hook must receive (command, node) and
 * return normally into the snippet, and the snippet must still work before the patch.
 * Usage: hook_test.exe */
#include "../src/mcpu.h"
#include <stdio.h>
#include <string.h>

HMODULE g_self;
int g_is_wine;
Stats g_stats;
uint8_t* game_base(void) { return (uint8_t*)GetModuleHandleA(NULL); }
size_t game_size(void) { return 0; }
const char* fmt_addr(void* p, char* buf, size_t cap)
{
    snprintf(buf, cap, "%p", p);
    return buf;
}
uint64_t qpc_now(void) { return 0; }
void* gpuprobe_thread_ctx(int* shadow_pass) { (void)shadow_pass; return NULL; }
int gpuprobe_query_begin(void* ctx) { (void)ctx; return -1; }
void gpuprobe_query_end(void* ctx, int slot) { (void)ctx, (void)slot; }
void gpuprobe_set_far(int bias) { (void)bias; }
void gpuprobe_scene_node(int far_bias, int cls, float area_px, float dist_m) { (void)far_bias, (void)cls, (void)area_px, (void)dist_m; }
void gpuprobe_fm_mark(int kind, float near_cm, float scale, float tpu) { (void)kind, (void)near_cm, (void)scale, (void)tpu; }
void farmesh_view(float p11) { (void)p11; }
void gpuprobe_node_mark(float dist_m, int is_char, const float* c, LONG64 seq) { (void)dist_m, (void)is_char, (void)c, (void)seq; }
void gpuprobe_oc_node(int on, uint64_t key, float dist_m, float dpx, int is_char, int bucket, int view, LONG64 seq) { (void)on, (void)key, (void)dist_m, (void)dpx, (void)is_char, (void)bucket, (void)view, (void)seq; }

uint8_t* shadowcull_test_make_stub(const uint8_t* prefix, size_t plen, void* target);
int shadowcull_test_retarget(uint8_t* site, void* to);
extern int g_sc_force_near;

/* int3 padding like the game's (this compiler's own padding runs are shorter than 19 bytes) */
__asm__(".text\n.p2align 4\nmcpu_test_padding:\n.fill 48, 1, 0xcc\n");

static void* volatile seen_cmd;
static void* volatile seen_node;
static volatile int orig_calls, hook_calls;

__attribute__((noinline)) static void orig_interp(void* cmd)
{
    seen_cmd = cmd;
    orig_calls++;
}

__attribute__((noinline)) static void hook(void* cmd, void* node)
{
    seen_node = node;
    hook_calls++;
    orig_interp(cmd);
}

/* RWX memory within +-2 GB of this EXE */
static uint8_t* alloc_near(uint8_t* base)
{
    for (uintptr_t d = 0x10000; d < 0x70000000; d += 0x10000) {
        uint8_t* p = (uint8_t*)VirtualAlloc(base - d, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
            return p;
    }
    return NULL;
}

int main(void)
{
    config_defaults(&g_cfg);
    int fail = 0;
#define EXPECT(c, what)                                                                                       \
    do {                                                                                                      \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                   \
        if (!(c))                                                                                             \
            fail++;                                                                                           \
    } while (0)

    uint8_t* code = alloc_near(game_base());
    EXPECT(code != NULL, "executable memory near the EXE");
    if (!code)
        return 1;
    void* node = (void*)0x1111222233334444ull;
    void* cmd = (void*)0x5555666677778888ull;
    uint8_t* p = code;
    *p++ = 0x53;                                              /* push rbx */
    *p++ = 0x48, *p++ = 0xBB, memcpy(p, &node, 8), p += 8;    /* mov rbx, node */
    *p++ = 0x48, *p++ = 0xB9, memcpy(p, &cmd, 8), p += 8;     /* mov rcx, cmd */
    *p++ = 0x48, *p++ = 0x83, *p++ = 0xEC, *p++ = 0x20;       /* sub rsp, 0x20 */
    uint8_t* site = p;
    int32_t rel = (int32_t)((uint8_t*)orig_interp - (site + 5));
    *p++ = 0xE8, memcpy(p, &rel, 4), p += 4;                  /* call orig_interp */
    *p++ = 0x48, *p++ = 0x83, *p++ = 0xC4, *p++ = 0x20;       /* add rsp, 0x20 */
    *p++ = 0x5B;                                              /* pop rbx */
    *p++ = 0xC3;                                              /* ret */
    FlushInstructionCache(GetCurrentProcess(), code, p - code);
    void (*snippet)(void) = (void (*)(void))code;

    snippet();
    EXPECT(orig_calls == 1 && seen_cmd == cmd && hook_calls == 0, "snippet calls the original before patching");

    static const uint8_t mov_rdx_rbx[3] = {0x48, 0x89, 0xDA};
    uint8_t* stub = shadowcull_test_make_stub(mov_rdx_rbx, 3, (void*)hook);
    EXPECT(stub != NULL, "stub placed in int3 padding of the EXE");
    if (!stub)
        return 1;
    EXPECT(shadowcull_test_retarget(site, stub), "call retargeted to the stub");
    seen_cmd = seen_node = NULL;
    snippet();
    EXPECT(hook_calls == 1 && seen_node == node, "hook receives the node (rbx) in rdx");
    EXPECT(orig_calls == 2 && seen_cmd == cmd, "hook receives the command stream and runs the original");
    snippet();
    EXPECT(hook_calls == 2 && orig_calls == 3, "returns normally, repeatable");
    uint8_t* stub2 = shadowcull_test_make_stub(mov_rdx_rbx, 3, (void*)hook);
    EXPECT(stub2 && (stub2 >= stub + 17 || stub2 + 17 <= stub), "a second stub does not overlap the first");

    /* fallback: a page below the EXE */
    g_sc_force_near = 1;
    uint8_t* stub3 = shadowcull_test_make_stub(mov_rdx_rbx, 3, (void*)hook);
    EXPECT(stub3 != NULL && stub3 < game_base(), "fallback stub in a page below the EXE");
    if (stub3) {
        EXPECT(shadowcull_test_retarget(site, stub3), "call retargeted to the fallback stub");
        snippet();
        EXPECT(hook_calls == 3 && orig_calls == 4 && seen_node == node, "fallback stub passes the node too");
    }
    printf("\n%s (%d failure%s)\n", fail ? "FAIL" : "PASS", fail, fail == 1 ? "" : "s");
    return fail ? 1 : 0;
}
