/* Maps METAPHOR.exe from disk (read-only, section by section) and checks that every
 * signature the mod relies on resolves to exactly one location.
 * Usage: scan_test.exe <path-to-METAPHOR.exe> */
#include "../src/mcpu.h"
#include <stdio.h>
#include <string.h>

static uint8_t* map_image(const char* path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    uint8_t* file = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)sz.QuadPart, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    uint64_t off = 0;
    while (file && off < (uint64_t)sz.QuadPart) {
        DWORD chunk = (DWORD)min((uint64_t)64 << 20, (uint64_t)sz.QuadPart - off);
        if (!ReadFile(f, file + off, chunk, &got, NULL) || !got)
            break;
        off += got;
    }
    CloseHandle(f);
    if (!file || off != (uint64_t)sz.QuadPart)
        return NULL;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)file;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(file + dos->e_lfanew);
    uint8_t* img = (uint8_t*)VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD n = min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize);
        memcpy(img + sec[i].VirtualAddress, file + sec[i].PointerToRawData, n);
    }
    VirtualFree(file, 0, MEM_RELEASE);
    return img;
}

/* shadowcull.c's run-time dependencies, unused by resolve + self-test */
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

/* the supported build, told apart by the size of its code section (.ecode), and its sites' RVAs (another
 * build: only found or not) */
#define BUILD_APR2025 0x17bf000u
static size_t g_code;
#define X(apr2025) (g_code == BUILD_APR2025 ? (uint64_t)(apr2025) : 0)

static int check(const char* what, uint8_t* p, uint8_t* base, uint64_t expect)
{
    uint64_t rva = p ? (uint64_t)(p - base) : 0;
    int ok = p != NULL && (expect == 0 || rva == expect);
    printf("  %-34s %-4s rva=0x%llx%s\n", what, ok ? "OK" : "FAIL", (unsigned long long)rva,
           (p && expect && rva != expect) ? " (differs from this build's known address)" : "");
    return ok;
}

int main(int argc, char** argv)
{
    const char* exe = argc > 1 ? argv[1] : "METAPHOR.exe";
    config_defaults(&g_cfg);
    g_cfg.log_level = 1;
    uint8_t* base = map_image(exe);
    if (!base) {
        printf("cannot map %s\n", exe);
        return 2;
    }
    uint8_t* code;
    if (!sig_text_range(base, &code, &g_code))
        g_code = 0;
    printf("Image: %s  (%s)\n", exe,
           g_code == BUILD_APR2025 ? "the April 2025 Steam build, with Denuvo"
                                   : "an unknown build: the sites are only searched");

    Targets t;
    int found = sig_resolve(base, &t, 1);
    printf("\nResolved %d/8 signatures\n", found);
    int pass = 1;
    pass &= check("spinlock acquire", t.spin_lock, base, X(0x10c2200));
    pass &= check("spinlock release", t.spin_unlock, base, X(0x10c2240));
    pass &= check("engine usleep", t.usleep, base, X(0x1094670));
    pass &= check("engine yield", t.yield, base, X(0x10946c0));
    pass &= check("main-thread id variable", (uint8_t*)t.main_tid, base, X(0x2f13e98));
    if (g_code == BUILD_APR2025) { /* inside the protected code there: threads unnamed, [Jobs] not available */
        printf("  %-34s %s\n", "thread create (not in this build)", t.thread_create ? "FAIL (found?)" : "OK");
        printf("  %-34s %s\n", "job pool init (not in this build)", t.job_init ? "FAIL (found?)" : "OK");
        pass &= !t.thread_create && !t.job_init;
    } else {
        pass &= check("engine thread create", t.thread_create, base, 0);
        pass &= check("job pool init", t.job_init, base, 0);
    }
    pass &= check("semaphore create (layout)", t.sem_create, base, X(0x10c22a0));
    pass &= check("semaphore wait", t.sem_wait, base, X(0x10c2360));
    pass &= check("semaphore post", t.sem_post, base, X(0x10c24b0));

    if (t.spin_unlock) {
        static const uint8_t rel[5] = {0x33, 0xC0, 0x87, 0x01, 0xC3};
        int ok = memcmp(t.spin_unlock, rel, 5) == 0;
        printf("  %-34s %s\n", "release bytes = xor/xchg/ret", ok ? "OK" : "FAIL");
        pass &= ok;
    }
    if (t.job_init) {
        printf("  %-34s pool0=%u pool1=%u (expected 5/5)\n", "job worker immediates", t.job_init[7], t.job_init[22]);
        pass &= t.job_init[7] == 5 && t.job_init[22] == 5;
    }

    printf("\nShadow culling sites:\n");
    ShadowSites s;
    int sc = shadowcull_resolve(base, &s, 1);
    pass &= sc;
    if (sc) {
        pass &= check("call cascade_begin (patched)", s.begin_site, base, X(0x11149b5));
        pass &= check("call execute_buckets", s.exec_site, base, X(0x11149c0));
        pass &= check("list A interpret call (patched)", s.exec_a_site, base, X(0x10f08b8));
        pass &= check("list B interpret call (patched)", s.exec_b_site, base, X(0x10f0908));
        pass &= check("cascade_begin", s.begin, base, X(0x109ff80));
        pass &= check("execute_buckets", s.exec, base, X(0x10f0860));
        pass &= check("command interpreter (mode 1)", s.interp, base, X(0x1119cb0));
        pass &= check("light matrix table", s.vp_table, base, X(0x2f11b38));
        pass &= check("handler: vertex buffer", (uint8_t*)s.h_vb, base, X(0x1117600));
        pass &= check("handler: material", (uint8_t*)s.h_material, base, X(0x1119920));
        pass &= check("handler: world matrix", (uint8_t*)s.h_world, base, X(0x1117500));
        pass &= check("handler: previous world", (uint8_t*)s.h_prev, base, X(0x1117560));
        pass &= check("handler: third matrix", (uint8_t*)s.h_mat3, base, X(0x1117530));
        pass &= check("handler: index buffer", (uint8_t*)s.h_ib, base, X(0x11176a0));
        pass &= check("handler: draw indexed", (uint8_t*)s.h_draw, base, X(0x11178d0));
        printf("  %-34s %s\n", "bone palette layout (animated)", s.anim_ok ? "OK" : "FAIL");
        pass &= s.anim_ok != 0;
        printf("  %-34s %s\n", "camera records (view-aware)", s.view_ok ? "OK" : "FAIL");
        pass &= s.view_ok != 0;
        {
            int m0 = 0, m2 = 0, m3 = 0;
            for (int i = 0; i < s.nscene; i++)
                m0 += s.scene_mode[i] == 0, m2 += s.scene_mode[i] == 2, m3 += s.scene_mode[i] == 3;
            int ok = s.nscene == 5 && m0 == 1 && m2 == 2 && m3 == 2;
            printf("  %-34s %s (mode 0: %d, mode 2: %d, mode 3: %d)\n", "scene-pass interpreter calls", ok ? "OK" : "FAIL",
                   m0, m2, m3);
            pass &= ok;
        }
        if (s.view_ok) {
            pass &= check("camera record base", s.cam_base, base, X(0x2f0d540));
            pass &= check("camera view index", (uint8_t*)s.cam_view_idx, base, X(0x2f12c60));
        }
        printf("  %-34s %s\n", "scene buckets (distance options)", s.scene_ok ? "OK" : "FAIL");
        pass &= s.scene_ok != 0;
        if (s.scene_ok) {
            pass &= check("scene bucket table (modes 2/3)", s.scene_tab, base, X(0x2f0d6a0));
            pass &= check("scene bucket table (mode 0)", s.scene_tab0, base, X(0x2f0d6b8));
        }
        pass &= check("character detail distance", (uint8_t*)s.char_lod, base, X(0x27aaa18));
        pass &= check("shadow distance (cascade setup)", s.shadow_far_site, base, X(0x1092dd8));
        int st = shadowcull_selftest(&s);
        printf("  %-34s %s\n", "culling self-test (synthetic mesh)", st ? "OK" : "FAIL");
        pass &= st;
    }
    printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
