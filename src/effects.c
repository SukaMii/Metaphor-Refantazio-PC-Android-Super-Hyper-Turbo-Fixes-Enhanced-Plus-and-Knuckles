/* Post-processing options ([Quality] VolumetricFog, Glare, Smaa), found by shader.
 *
 * The game's shaders are identified by the FNV-1a 64 hash of their bytecode at creation (hashes
 * from the frame capture of 23 Oct 2026):
 *   - volumetric fog raymarch (quarter resolution): its target is cleared to 0 and the draw left
 *     out; the composite that follows then adds nothing (the fog's alpha is its opacity);
 *   - glare (R11G11B10F chain): the downsample and blur draws are left out, the combine's target is
 *     cleared to 0, so tone mapping adds no glare;
 *   - SMAA (Ultra in the game: edge threshold 0.05, diagonal search 16, orthogonal search 32):
 *     High / Medium are patched copies of the game's shaders made at creation and bound in place
 *     of the originals (edge threshold 0.1; diagonal search 8 / off; orthogonal search 16 / 8 in the
 *     vertex shader); Off leaves out the edge and weight draws and clears the weights, so the
 *     final blend becomes a plain copy (it samples the colour unchanged when the weights are 0).
 * Patched bytecode gets a new DXBC checksum (the Windows runtime checks it). */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <d3d11_3.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shaders_gen.h" /* the replacement shaders (tools/compile_shaders.py) */

enum { R_NONE, R_FOG, R_GLARE_DOWN, R_GLARE_BLUR, R_GLARE_COMBINE, R_SMAA_EDGES, R_SMAA_WEIGHTS, R_PAINT, R_N };
static const struct {
    uint64_t hash;
    int role;
} g_known[] = {
    {0xe772c2a84a5c2f2eull, R_FOG},          /* p009: volumetric fog raymarch */
    {0x87dfc3b058bc2699ull, R_GLARE_DOWN},   /* p037 (also used by bloom: only on R11G11B10F targets) */
    {0xc8e288fdea5aec70ull, R_GLARE_BLUR},   /* p038-p043 */
    {0xb28d164d1ee9722aull, R_GLARE_COMBINE}, /* p044 */
    {0x5b1af0481406dae1ull, R_SMAA_EDGES},   /* luma edge detection */
    {0xda00acd64a9e2b1full, R_SMAA_WEIGHTS}, /* blending weights */
    {0x822dcd902415fe5dull, R_SMAA_EDGES},   /* the same two, SMAA 2.8 as written */
    {0x9f3d5b38f7eff19eull, R_SMAA_WEIGHTS},
    {0xb3ebc8024b526721ull, R_PAINT},        /* p022: the painterly wobble */
};
static volatile LONG64 g_fx_seen[R_N], g_fx_skipped[R_N];

uint64_t fx_fnv64(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* ---------------------------------------------------------------- DXBC checksum */
/* MD5 over the container after the checksum field (offset 20), with DXBC's own final block:
 * the bit count goes first in the last block and (bits >> 2) | 1 into its last word. Checked
 * against the game's shaders (capture_test). */
static void md5_block(uint32_t st[4], const uint8_t* blk)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t M[16];
    memcpy(M, blk, 64);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)
            f = (b & c) | (~b & d), g = i;
        else if (i < 32)
            f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        else if (i < 48)
            f = b ^ c ^ d, g = (3 * i + 5) & 15;
        else
            f = c ^ (b | ~d), g = (7 * i) & 15;
        f += a + K[i] + M[g];
        a = d, d = c, c = b;
        b += (f << R[i]) | (f >> (32 - R[i]));
    }
    st[0] += a, st[1] += b, st[2] += c, st[3] += d;
}

void fx_dxbc_checksum(const uint8_t* dxbc, size_t size, uint32_t out[4])
{
    const uint8_t* p = dxbc + 20;
    size_t n = size - 20;
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint32_t bits = (uint32_t)(n * 8), tail = (bits >> 2) | 1;
    size_t full = n / 64, left = n % 64;
    for (size_t i = 0; i < full; i++)
        md5_block(st, p + i * 64);
    uint8_t blk[64];
    const uint8_t* rest = p + full * 64;
    if (left >= 56) {
        memset(blk, 0, 64);
        memcpy(blk, rest, left);
        blk[left] = 0x80;
        md5_block(st, blk);
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        memcpy(blk + 60, &tail, 4);
        md5_block(st, blk);
    } else {
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        memcpy(blk + 4, rest, left);
        blk[4 + left] = 0x80;
        memcpy(blk + 60, &tail, 4);
        md5_block(st, blk);
    }
    memcpy(out, st, 16);
}

/* replaces every occurrence of a dword sequence (preceded by `lead` when lead != 0); returns the
 * number of replacements */
static int patch_seq(uint32_t* w, size_t n, uint32_t lead, const uint32_t* from, const uint32_t* to, size_t len)
{
    int k = 0;
    for (size_t i = 1; i + len <= n; i++)
        if ((!lead || w[i - 1] == lead) && !memcmp(w + i, from, len * 4)) {
            memcpy(w + i, to, len * 4);
            k++;
        }
    return k;
}

/* ---------------------------------------------------------------- shaders */
#define SH_N 32768
static struct {
    void* sh;
    int role;
} g_role[SH_N]; /* shader -> role, for the known ones */
#define VAR_N 32
static struct {
    void* orig;
    void* var[3]; /* index = SMAA level: 1 medium, 2 high */
} g_var[VAR_N];
static int g_nvar;
/* [Quality] Painterly=0: the game's wobble shader -> the plain copy (src/shaders/painterly_off.hlsl) */
#define PAINT_N 8
static struct {
    void *orig, *plain;
} g_paint[PAINT_N];
static int g_npaint;
static volatile LONG64 g_paint_binds, g_paint_plain;
static SRWLOCK g_fx_lock = SRWLOCK_INIT;
static int g_vs_found, g_ps_patched;

static unsigned sh_hash(const void* p) { return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (SH_N - 1); }

static void set_role(void* sh, int role)
{
    unsigned k = sh_hash(sh);
    for (unsigned i = 0; i < SH_N; i++, k = (k + 1) & (SH_N - 1))
        if (!g_role[k].sh || g_role[k].sh == sh) {
            g_role[k].role = role;
            g_role[k].sh = sh;
            return;
        }
}

static int role_of(const void* sh)
{
    unsigned k = sh_hash(sh);
    for (unsigned i = 0; i < SH_N; i++, k = (k + 1) & (SH_N - 1)) {
        if (g_role[k].sh == sh)
            return g_role[k].role;
        if (!g_role[k].sh)
            return R_NONE;
    }
    return R_NONE;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateSh_t)(void*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);

/* a patched copy of `code`, created with the device's original Create*Shader */
static void* make_variant(void* dev, CreateSh_t create, const void* code, size_t len, int level, int vs, int role)
{
    uint8_t* b = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, len);
    if (!b)
        return NULL;
    memcpy(b, code, len);
    uint32_t* w = (uint32_t*)b;
    size_t n = len / 4;
    int k = 0;
    if (vs) { /* orthogonal search: float4(-2, 2, -2, 2) * steps (Ultra 32) */
        static const uint32_t u[4] = {0xC2800000, 0x42800000, 0xC2800000, 0x42800000};
        static const uint32_t h[4] = {0xC2000000, 0x42000000, 0xC2000000, 0x42000000}; /* 16 steps */
        static const uint32_t m[4] = {0xC1800000, 0x41800000, 0xC1800000, 0x41800000}; /* 8 steps */
        k = patch_seq(w, n, 0, u, level == 2 ? h : m, 4);
    } else if (role == R_SMAA_EDGES) { /* threshold 0.05 -> 0.1 */
        static const uint32_t f[2] = {0x3D4CCCCD, 0x3D4CCCCD}, t[2] = {0x3DCCCCCD, 0x3DCCCCCD};
        k = patch_seq(w, n, 0, f, t, 2);
    } else if (role == R_SMAA_WEIGHTS) { /* diagonal search, as compiled one way: scalar loop bounds 16, and 15 in the end checks */
        uint32_t f1 = 0x41800000, t1 = level == 2 ? 0x41000000 : 0x00000000; /* 8 / 0 (no diagonal search) */
        k = patch_seq(w, n, 0x00004001, &f1, &t1, 1);
        if (level == 2) {
            static const uint32_t f2[2] = {0x41700000, 0x41700000}, t2[2] = {0x40E00000, 0x40E00000}; /* 15 -> 7 */
            k += patch_seq(w, n, 0, f2, t2, 2);
        }
        /* or SMAA 2.8 as written: each diagonal search runs while its counter (from -1)
         * is below 15 (steps - 1), four loops: High 7 (8 steps), Medium -1 (no step: the distances stay -1, no
         * diagonal weights, as SMAA_DISABLE_DIAG_DETECTION) */
        if (!k) {
            uint32_t f3 = 0x41700000, t3 = level == 2 ? 0x40E00000 : 0xBF800000;
            k = patch_seq(w, n, 0x00004001, &f3, &t3, 1);
        }
    }
    void* out = NULL;
    if (k > 0 && len > 32 && !memcmp(b, "DXBC", 4)) {
        uint32_t sum[4];
        fx_dxbc_checksum(b, len, sum);
        memcpy(b + 4, sum, 16);
        if (FAILED(create(dev, b, len, NULL, &out)))
            out = NULL;
    }
    HeapFree(GetProcessHeap(), 0, b);
    LOGI("Effects: %s %s variant: %d constants patched, %s", vs ? "SMAA weights vertex shader" : (role == R_SMAA_EDGES ? "SMAA edge shader" : "SMAA weights shader"),
         level == 2 ? "High" : "Medium", k, out ? "created" : "NOT created");
    return out;
}

static int fx_wanted(void)
{
    return g_cfg.q_fx_active;
}

/* ---------------------------------------------------------------- every pixel shader */
/* For the GPU timing by shader and the knock-out cycle. The game creates thousands of pixel-shader
 * objects at startup, many of them with the same bytecode: each object maps to an id (1..PS_MAX-1)
 * per distinct bytecode, with its FNV-64 hash and its size from the DXBC STAT chunk (instruction slots,
 * texture instructions, dynamic branches). The bytecode itself is kept for [GpuProbe] ShaderTiming,
 * to write the shaders that cost the most (up to PS_CODE_MAX bytes in all). */
#define PS_MAX 65536            /* distinct shaders */
#define PS_OBJ_N 262144         /* shader objects (open addressing, by address) */
#define PS_HASH_N 131072        /* distinct shaders by hash */
#define PS_CODE_MAX (256u << 20)
static struct {
    void* sh;
    int id;
} g_psobj[PS_OBJ_N];
static int g_pshash[PS_HASH_N]; /* -> id, 0 = empty */
static PsInfo g_ps[PS_MAX];
static volatile LONG g_nps = 1, g_npsobj, g_ps_full;
static size_t g_ps_code;

static void dxbc_stat(const uint8_t* code, size_t len, PsInfo* p)
{
    if (len < 32 || memcmp(code, "DXBC", 4))
        return;
    uint32_t n = *(const uint32_t*)(code + 28);
    for (uint32_t i = 0; i < n && 32 + 4 * (size_t)i + 4 <= len; i++) {
        uint32_t off = *(const uint32_t*)(code + 32 + 4 * i);
        if (off + 8 > len || memcmp(code + off, "STAT", 4))
            continue;
        uint32_t size = *(const uint32_t*)(code + off + 4);
        const uint32_t* s = (const uint32_t*)(code + off + 8);
        if (off + 8 + (size_t)size > len || size < 19 * 4)
            return;
        p->instr = s[0], p->flow = s[8];
        p->tex = s[14] + s[15] + s[16] + s[17] + s[18]; /* sample, load, compare, bias, gradient */
        return;
    }
}

/* a name in the bytecode (the RDEF chunk keeps the constant-buffer variable names) */
static int has_name(const void* code, size_t len, const char* name)
{
    size_t n = strlen(name);
    const uint8_t* b = (const uint8_t*)code;
    for (size_t i = 0; i + n <= len; i++)
        if (b[i] == (uint8_t)name[0] && !memcmp(b + i, name, n))
            return 1;
    return 0;
}

/* The SMAA pixel shaders by what they hold, for bytecode whose hash is not in g_known: the blending weights
 * bind the edges, area and search textures; the luma edge detection binds a
 * colour buffer, takes the luma (0.2126, 0.7152, 0.0722, more than once) and holds the
 * threshold 0.05 as one pair (what make_variant patches) */
static int smaa_by_content(const void* code, size_t len, uint64_t h)
{
    int role = R_NONE;
    if (has_name(code, len, "edgesBuffer") && has_name(code, len, "areaBuffer") && has_name(code, len, "searchBuffer")) {
        role = R_SMAA_WEIGHTS;
    } else if (len < 8192 && has_name(code, len, "colorBuffer")) {
        static const uint32_t thr[2] = {0x3D4CCCCD, 0x3D4CCCCD}, luma[3] = {0x3E59B3D0, 0x3F371759, 0x3D93DD98};
        const uint32_t* w = (const uint32_t*)code;
        size_t n = len / 4;
        int t = 0, l = 0;
        for (size_t i = 0; i < n; i++) {
            t += i + 2 <= n && !memcmp(w + i, thr, 8);
            l += i + 3 <= n && !memcmp(w + i, luma, 12);
        }
        if (t == 1 && l >= 2)
            role = R_SMAA_EDGES;
    }
    if (role)
        LOGI("Effects: SMAA %s shader recognised by its content (bytecode hash %016llx, %u bytes)",
             role == R_SMAA_EDGES ? "edge" : "blending weights", (unsigned long long)h, (unsigned)len);
    if (role && g_cfg.capture) { /* [GpuProbe] Capture=1: its bytecode too (MetaphorCPU_shaders_<time>), to study it */
        static char dir[MAX_PATH];
        if (!dir[0])
            output_dir(dir, sizeof(dir), "shaders");
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\ps_%016llx.dxbc", dir, (unsigned long long)h);
        HANDLE f = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, code, (DWORD)len, &w, NULL);
            CloseHandle(f);
        }
    }
    return role;
}

static unsigned ps_obj_slot(const void* sh) { return (unsigned)(((uintptr_t)sh >> 4) * 2654435761u) & (PS_OBJ_N - 1); }

/* the id of this bytecode, made on first sight (under g_fx_lock) */
static int ps_id_of_code(const void* code, size_t len)
{
    uint64_t h = fx_fnv64(code, len);
    unsigned k = (unsigned)(h >> 40) & (PS_HASH_N - 1);
    for (unsigned i = 0; i < PS_HASH_N; i++, k = (k + 1) & (PS_HASH_N - 1)) {
        int id = g_pshash[k];
        if (id && g_ps[id].hash == h)
            return id;
        if (!id) {
            if (g_nps >= PS_MAX)
                return 0;
            id = g_nps;
            PsInfo* p = &g_ps[id];
            p->hash = h;
            dxbc_stat((const uint8_t*)code, len, p);
            p->outline = has_name(code, len, "outlineColor"); /* the characters' ink outlines */
            p->hatch_slot = has_name(code, len, "toonShadowHatchingTexture")
                                ? dxbc_texture_slot((const uint8_t*)code, len, "toonShadowHatchingTexture")
                                : -1;
            static const char* const mat_names[3] = {"baseTexture", "toonShadowColorTexture", "toonParamsTexture"};
            for (int m = 0; m < 3; m++)
                p->mat_slot[m] = has_name(code, len, mat_names[m]) ? dxbc_texture_slot((const uint8_t*)code, len, mat_names[m])
                                                                   : -1;
            if (g_cfg.shader_timing && g_ps_code + len <= PS_CODE_MAX &&
                (p->code = HeapAlloc(GetProcessHeap(), 0, len)) != NULL) {
                memcpy(p->code, code, len);
                p->len = len, g_ps_code += len;
            }
            g_pshash[k] = id;
            MemoryBarrier();
            InterlockedIncrement(&g_nps); /* published: fx_ps_info may read it now */
            return id;
        }
    }
    return 0;
}

static void ps_register(void* sh, const void* code, size_t len)
{
    AcquireSRWLockExclusive(&g_fx_lock);
    int id = ps_id_of_code(code, len);
    unsigned k = ps_obj_slot(sh);
    for (unsigned i = 0; id && i < PS_OBJ_N; i++, k = (k + 1) & (PS_OBJ_N - 1))
        if (!g_psobj[k].sh || g_psobj[k].sh == sh) { /* the same address again: a released shader's */
            if (!g_psobj[k].sh)
                InterlockedIncrement(&g_npsobj);
            g_psobj[k].id = id;
            MemoryBarrier();
            g_psobj[k].sh = sh;
            id = -1;
            break;
        }
    if (id >= 0 && !g_ps_full++)
        LOGI("Effects: the pixel shader registry is full (%ld shaders, %ld objects): later ones are not timed",
             (long)g_nps - 1, (long)g_npsobj);
    ReleaseSRWLockExclusive(&g_fx_lock);
}

int fx_ps_id(const void* sh)
{
    if (!sh)
        return 0;
    unsigned k = ps_obj_slot(sh);
    for (unsigned i = 0; i < PS_OBJ_N; i++, k = (k + 1) & (PS_OBJ_N - 1)) {
        if (g_psobj[k].sh == sh)
            return g_psobj[k].id;
        if (!g_psobj[k].sh)
            return 0;
    }
    return 0;
}

const PsInfo* fx_ps_info(int id)
{
    return id > 0 && id < g_nps ? &g_ps[id] : NULL;
}

void fx_ps_counts(long* shaders, long* objects)
{
    *shaders = (long)g_nps - 1, *objects = (long)g_npsobj;
}
/* Copies of the game's shaders made at creation (once per distinct bytecode, with the device's own CreatePixelShader)
 * and bound in their place by distance:
 * - [Quality] RimLight: the toon material shaders with every read of their rim light (matEdgeColor) reading 0;
 * - [Quality] InnerOutlines: the outline shaders without the lines drawn over the character's own body. Each outline
 *   shader looks up the outline id under the pixel: over its own character the line's opacity is the vertex
 *   colour's w x outlineColor.w (mul), elsewhere (the silhouette) outlineColor.w (movc). Only the mul reads 0. */
static volatile LONG g_rim_shaders, g_rim_failed, g_inner_shaders, g_inner_failed;
static void* alt_copy(void* dev, const void* code, size_t len, void* create, const char* var, int opcode, int comp,
                      int need)
{
    int slot, off, n = 0;
    size_t nl = 0;
    uint8_t* b = NULL;
    void* out = NULL;
    if (dxbc_cbuffer_var((const uint8_t*)code, len, "GFD_PSCONST_MATERIAL", var, &slot, &off) && !(off & 15))
        b = dxbc_patch_cb_reads((const uint8_t*)code, len, slot, off / 16, opcode, comp, 0.0f, &nl, &n);
    if (b && (!need || n == need) && FAILED(((CreateSh_t)create)(dev, b, nl, NULL, &out)))
        out = NULL;
    free(b);
    return out;
}

static void alt_variants(void* dev, void* sh, const void* code, size_t len, void* create)
{
    const PsInfo* p = fx_ps_info(fx_ps_id(sh));
    if (!p)
        return;
    int rim = g_cfg.q_rim_hooks && !p->rim && has_name(code, len, "matEdgeColor");
    int inner = g_cfg.q_inner_hooks && !p->inner && p->outline;
    if (!rim && !inner)
        return;
    /* inner lines: exactly the one multiplication (0x38 mul) of outlineColor.w (component 3) */
    void* out = rim ? alt_copy(dev, code, len, create, "matEdgeColor", -1, -1, 0)
                    : alt_copy(dev, code, len, create, "outlineColor", 0x38, 3, 1);
    AcquireSRWLockExclusive(&g_fx_lock);
    PsInfo* w = (PsInfo*)p;
    if (rim && !w->rim) {
        w->rim = out ? 1 : -1, w->norim = out;
        InterlockedIncrement(out ? &g_rim_shaders : &g_rim_failed);
        out = NULL;
    } else if (!rim && !w->inner) {
        w->inner = out ? 1 : -1, w->noinner = out;
        InterlockedIncrement(out ? &g_inner_shaders : &g_inner_failed);
        out = NULL;
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
    if (out) /* another thread made it first */
        IUnknown_Release((IUnknown*)out);
}

/* the copy wanted for the object being drawn: the game's shader within the option's distance (0: the game's
 * everywhere, <1: the copy everywhere). *by_dist: the choice depends on the object (checked again at each draw) */
static void* alt_choice(void* ps, int* by_dist)
{
    *by_dist = 0;
    const PsInfo* p = ps ? fx_ps_info(fx_ps_id(ps)) : NULL;
    if (!p || (p->rim != 1 && p->inner != 1))
        return ps;
    float lim = p->rim == 1 ? g_cfg.q_rim_dist : g_cfg.q_inner_dist;
    void* copy = p->rim == 1 ? p->norim : p->noinner;
    if (lim <= 0)
        return ps;
    if (lim < 1) /* off: everywhere */
        return copy;
    *by_dist = 1;
    float d = gpuprobe_node_raw_dist();
    return d >= 0 && d > lim ? copy : ps;
}

void effects_shader_created(void* dev, int vs, void* sh, const void* code, size_t len, void* create)
{
    if (!vs && sh && code && len >= 32 &&
        (g_cfg.gpu_timing || g_cfg.q_knock_n || g_cfg.q_outline_hooks || g_cfg.q_hatch_hooks || g_cfg.q_mv_hooks))
        ps_register(sh, code, len);
    if (!vs && sh && (g_cfg.q_rim_hooks || g_cfg.q_inner_hooks))
        alt_variants(dev, sh, code, len, create);
    if (!fx_wanted() || !sh || !code || len < 32)
        return;
    int role = R_NONE;
    if (vs) {
        static const uint32_t u[4] = {0xC2800000, 0x42800000, 0xC2800000, 0x42800000};
        const uint32_t* w = (const uint32_t*)code;
        for (size_t i = 0; i + 4 <= len / 4 && !role; i++)
            if (!memcmp(w + i, u, 16))
                role = R_SMAA_WEIGHTS;
        if (!role)
            return;
        InterlockedIncrement((volatile LONG*)&g_vs_found);
    } else {
        uint64_t h = fx_fnv64(code, len);
        for (unsigned i = 0; i < sizeof(g_known) / sizeof(g_known[0]); i++)
            if (g_known[i].hash == h)
                role = g_known[i].role;
        if (!role)
            role = smaa_by_content(code, len, h);
        if (!role)
            return;
    }
    AcquireSRWLockExclusive(&g_fx_lock);
    if (!vs)
        set_role(sh, role);
    if (role == R_PAINT && g_npaint < PAINT_N) { /* the plain copy, made now with the device's own CreatePixelShader */
        void* plain = NULL;
        if (FAILED(((CreateSh_t)create)(dev, g_sh_painterly_off, sizeof(g_sh_painterly_off), NULL, &plain)))
            plain = NULL;
        g_paint[g_npaint].orig = sh, g_paint[g_npaint].plain = plain, g_npaint++;
        LOGI("Effects: painterly wobble shader found; plain replacement %s", plain ? "created" : "NOT created");
    }
    int smaa = role == R_SMAA_EDGES || role == R_SMAA_WEIGHTS;
    if (smaa && g_nvar < VAR_N) { /* the High and Medium copies, made now; bound in place of the original */
        int v = g_nvar++;
        g_var[v].orig = sh;
        for (int level = 1; level <= 2; level++)
            g_var[v].var[level] = make_variant(dev, (CreateSh_t)create, code, len, level, vs, role);
        if (!vs)
            g_ps_patched++;
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
}

/* ---------------------------------------------------------------- per context */
#define FXC_N 64
typedef struct FxCtx {
    void* ctx;
    int role;
    int knock; /* the bound pixel shader is the one knocked out ([ABTest] Compare=knockout) */
    int outline; /* the bound pixel shader draws character outlines ([Quality] OutlineDistance) */
    void *game_ps, *bound_ps; /* RimLight / InnerOutlines: the game's pixel shader, and the one bound for it */
    int by_dist;              /* the choice depends on the object drawn: checked again at each draw */
    /* OutlineDepthOffset: our rasterizer state with the depth bias is bound (rs_on) in place of the game's (rs_orig,
     * referenced until it is bound back) */
    int line, redraw, cr_on; /* LineFix: a line shader bound; shifted copies wanted; conservative state bound */
    ID3D11RasterizerState *cr_orig, *cr_clone;
    int rs_on, rs_bias, rs_bucket, rs_fmt, rs_func; /* the bias, its distance bucket, the depth target and test */
    ID3D11RasterizerState *rs_orig, *rs_clone;
    void* ps; /* the game's pixel shader bound now */
} FxCtx;
static FxCtx g_fxc[FXC_N];
static volatile LONG g_nfxc;
static volatile LONG64 g_knocked; /* draws left out by the knock-out (statistics) */
static volatile LONG64 g_outline_draws, g_outline_skipped;
/* outline vertices by the character's distance: <15, 15-30, 30-50, 50-100, >=100 m, not a character (3D texts,
 * speech bubbles: always drawn), no scene object known; and of the first five, those of carried objects
 * (statistics) */
#define OL_BANDS 8
static volatile LONG64 g_outline_verts[OL_BANDS];

static FxCtx* ctx_fx(void* c)
{
    LONG n = g_nfxc;
    for (LONG i = 0; i < n; i++)
        if (g_fxc[i].ctx == c)
            return &g_fxc[i];
    AcquireSRWLockExclusive(&g_fx_lock);
    FxCtx* r = NULL;
    for (LONG i = 0; i < g_nfxc && !r; i++)
        if (g_fxc[i].ctx == c)
            r = &g_fxc[i];
    if (!r && g_nfxc < FXC_N) {
        r = &g_fxc[g_nfxc];
        r->ctx = c, r->role = R_NONE, r->knock = 0, r->outline = 0;
        InterlockedIncrement(&g_nfxc);
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
    return r;
}

static void* variant_of(void* sh)
{
    int level = g_cfg.q_smaa;
    if (level < 1 || level > 2)
        return sh;
    for (int i = 0; i < g_nvar; i++)
        if (g_var[i].orig == sh)
            return g_var[i].var[level] ? g_var[i].var[level] : sh;
    return sh;
}

void* effects_bind_ps(void* ctx, void* ps)
{
    int role = ps ? role_of(ps) : R_NONE;
    FxCtx* r = ctx_fx(ctx);
    if (r) {
        r->role = role, r->ps = ps;
        const PsInfo* p = (g_cfg.q_knock || g_cfg.q_outline_hooks) ? fx_ps_info(fx_ps_id(ps)) : NULL;
        r->knock = p && g_cfg.q_knock && p->hash == g_cfg.q_knock;
        r->line = 0;
        for (int i = 0; p && g_cfg.q_line_hooks && i < g_cfg.q_line_n && !r->line; i++)
            r->line = p->hash == g_cfg.q_line_ps[i];
        r->outline = p && p->outline;
    }
    if ((g_cfg.q_rim_hooks || g_cfg.q_inner_hooks) && r) { /* the copy for far objects */
        void* use = alt_choice(ps, &r->by_dist);
        r->game_ps = ps, r->bound_ps = use;
        if (use != ps)
            return use;
    }
    if (role == R_SMAA_EDGES || role == R_SMAA_WEIGHTS)
        return variant_of(ps);
    if (role == R_PAINT) {
        g_paint_binds++;
        for (int i = 0; !g_cfg.q_painterly && i < g_npaint; i++)
            if (g_paint[i].orig == ps && g_paint[i].plain) {
                g_paint_plain++;
                return g_paint[i].plain;
            }
    }
    return ps;
}

/* CharacterMotionVectors: the characters' pass also draws its meshes again for the subsurface ids (c1dad80b2f591622):
 * the same surfaces, no need to draw their motion twice */
int effects_bound_ps_skip(void* ctx)
{
    FxCtx* r = ctx_fx(ctx);
    const PsInfo* p = r && r->ps ? fx_ps_info(fx_ps_id(r->ps)) : NULL;
    return p && p->hash == 0xc1dad80b2f591622ull;
}

/* the bound pixel shader's bytecode hash (0: not known) */
uint64_t effects_bound_ps_hash(void* ctx)
{
    FxCtx* r = ctx_fx(ctx);
    const PsInfo* p = r && r->ps ? fx_ps_info(fx_ps_id(r->ps)) : NULL;
    return p ? p->hash : 0;
}

void* effects_bind_vs(void* ctx, void* vs)
{
    return (vs && g_nvar) ? variant_of(vs) : vs;
}

void effects_reset(void* ctx)
{
    FxCtx* r = ctx_fx(ctx);
    if (r) {
        r->role = R_NONE, r->knock = 0, r->outline = 0, r->game_ps = r->bound_ps = NULL, r->by_dist = 0;
        if (r->rs_on && r->rs_orig) /* the context's state is the default now: nothing to bind back */
            ID3D11RasterizerState_Release(r->rs_orig);
        r->rs_on = 0, r->rs_orig = r->rs_clone = NULL;
        if (r->cr_on && r->cr_orig)
            ID3D11RasterizerState_Release(r->cr_orig);
        r->cr_on = 0, r->cr_orig = r->cr_clone = NULL, r->line = 0, r->redraw = 0;
    }
}

/* ---------------------------------------------------------------- [Quality] OutlineDepthOffset
 * The characters' outlines are the mesh drawn again slightly enlarged, back faces only, black where it is not hidden
 * behind the character itself. In hair clumps and clothing folds the enlarged copy pokes through the surface by a
 * hair's breadth: broken lines of single pixels that change with every small movement. A depth bias moves the copy
 * back by a fraction of its distance: lines with a real depth gap behind them (the silhouette, an arm over the body)
 * stay, the near-coplanar ones are hidden. */
int fx_outline_bias(int fmt, int func, float a, float b, float k, float dist)
{
    int sign = (func == D3D11_COMPARISON_LESS || func == D3D11_COMPARISON_LESS_EQUAL)         ? 1
               : (func == D3D11_COMPARISON_GREATER || func == D3D11_COMPARISON_GREATER_EQUAL) ? -1
                                                                                                : 0;
    if (!sign || !(k > 0) || !(b != 0) || !(dist > 0))
        return 0;
    /* NDC = a + b / d: moving k d farther changes it by |b| k / d at the distance d (game units) */
    double z = (double)a + (double)b / dist, dz = fabs((double)b) * k / dist, unit;
    if (fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
        if (!(z > 0))
            return 0;
        int e;
        frexp(z, &e); /* z = m 2^e, m in [0.5, 1): a unit in the last place of 23 bits */
        unit = ldexp(1.0, e - 1 - 23);
    } else if (fmt == DXGI_FORMAT_D24_UNORM_S8_UINT) {
        unit = 1.0 / 16777216.0;
    } else if (fmt == DXGI_FORMAT_D16_UNORM) {
        unit = 1.0 / 65536.0;
    } else {
        return 0;
    }
    double bias = dz / unit + 0.5;
    return sign * (int)(bias > 1e9 ? 1e9 : bias);
}

#define RSC_N 128
static struct {
    D3D11_RASTERIZER_DESC od; /* the game's state (NULL bound: the default) */
    int bias;
    ID3D11RasterizerState* clone;
} g_rsc[RSC_N];
static int g_nrsc;

static ID3D11RasterizerState* rs_clone_of(ID3D11DeviceContext* c, ID3D11RasterizerState* orig, int bias)
{
    D3D11_RASTERIZER_DESC d = {D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    if (orig)
        ID3D11RasterizerState_GetDesc(orig, &d);
    ID3D11RasterizerState* out = NULL;
    AcquireSRWLockShared(&g_fx_lock);
    for (int i = 0; i < g_nrsc && !out; i++)
        if (g_rsc[i].bias == bias && !memcmp(&g_rsc[i].od, &d, sizeof(d)))
            out = g_rsc[i].clone;
    ReleaseSRWLockShared(&g_fx_lock);
    if (out)
        return out;
    D3D11_RASTERIZER_DESC nd = d;
    long long nb = (long long)d.DepthBias + bias;
    nd.DepthBias = (INT)(nb > 2000000000LL ? 2000000000LL : nb < -2000000000LL ? -2000000000LL : nb);
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext_GetDevice(c, &dev);
    if (!dev)
        return NULL;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &nd, &out)))
        out = NULL;
    ID3D11Device_Release(dev);
    if (!out)
        return NULL;
    AcquireSRWLockExclusive(&g_fx_lock);
    for (int i = 0; i < g_nrsc; i++)
        if (g_rsc[i].bias == bias && !memcmp(&g_rsc[i].od, &d, sizeof(d))) { /* another thread made it meanwhile */
            ID3D11RasterizerState_Release(out);
            out = g_rsc[i].clone;
            ReleaseSRWLockExclusive(&g_fx_lock);
            return out;
        }
    if (g_nrsc < RSC_N) {
        g_rsc[g_nrsc].od = d, g_rsc[g_nrsc].bias = bias, g_rsc[g_nrsc].clone = out;
        g_nrsc++;
    } else { /* the table is full: no offset (never seen: the game has a handful of states) */
        ID3D11RasterizerState_Release(out);
        out = NULL;
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
    return out;
}

/* the depth target and test bound now (the test 0 when off) */
static void ol_depth_state(ID3D11DeviceContext* c, int* fmt_out, int* func_out)
{
    ID3D11DepthStencilView* dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 0, NULL, &dsv);
    int fmt = 0;
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        ID3D11DepthStencilView_GetDesc(dsv, &dd);
        fmt = (int)dd.Format;
        ID3D11DepthStencilView_Release(dsv);
    }
    ID3D11DepthStencilState* ds = NULL;
    UINT ref = 0;
    ID3D11DeviceContext_OMGetDepthStencilState(c, &ds, &ref);
    int func = D3D11_COMPARISON_LESS, on = 1; /* NULL: the default state */
    if (ds) {
        D3D11_DEPTH_STENCIL_DESC sd;
        ID3D11DepthStencilState_GetDesc(ds, &sd);
        func = (int)sd.DepthFunc, on = sd.DepthEnable;
        ID3D11DepthStencilState_Release(ds);
    }
    *fmt_out = fmt, *func_out = on ? func : 0;
}

/* distance buckets of half an octave from 1 m: the bias is set for the bucket's middle, so the push stays within
 * +-19% of the share asked for at any distance (a fixed bias on a D24 target would grow with the distance squared) */
#define OLB_N 24
static int ol_bucket(float d_m)
{
    if (!(d_m > 1.0f))
        return 0;
    int i = (int)(2.0f * log2f(d_m));
    return i < OLB_N - 1 ? i : OLB_N - 1;
}

static int ol_bias_for(int fmt, int func, int bucket)
{
    float a = 0, b = 0;
    shadowcull_depth_proj(&a, &b);
    float mid_m = powf(2.0f, (bucket + 0.5f) * 0.5f);
    int bias = fx_outline_bias(fmt, func, a, b, g_cfg.q_ol_depth, mid_m * 100.0f /* game units: cm */);
    static volatile LONG logged;
    if (!logged && !InterlockedExchange(&logged, 1))
        LOGI("Outlines: depth target format %d, depth test %d (%s), camera depth = %.4g + %.4g / distance: "
             "OutlineDepthOffset %.2f%% = depth bias %d at %.1f m%s",
             fmt, func, !func ? "off" : func >= D3D11_COMPARISON_GREATER ? "reversed: near is 1" : "near is 0", a, b,
             100.0 * g_cfg.q_ol_depth, bias, mid_m, bias ? "" : " (not known: no offset)");
    return bias;
}

/* before an outline draw of a character d_m metres away: our state with the bias in place of the game's */
static void ol_bias_on(ID3D11DeviceContext* c, FxCtx* r, float d_m)
{
    int bucket = ol_bucket(d_m);
    ID3D11RasterizerState* cur = NULL;
    ID3D11DeviceContext_RSGetState(c, &cur);
    if (r->rs_on && cur == r->rs_clone) {
        if (cur)
            ID3D11RasterizerState_Release(cur);
        if (bucket == r->rs_bucket) /* still bound */
            return;
        /* another distance: the same game state with another bias */
        int bias = ol_bias_for(r->rs_fmt, r->rs_func, bucket);
        ID3D11RasterizerState* cl = bias ? rs_clone_of(c, r->rs_orig, bias) : NULL;
        ID3D11DeviceContext_RSSetState(c, cl ? cl : r->rs_orig);
        r->rs_bias = bias, r->rs_bucket = bucket, r->rs_clone = cl ? cl : r->rs_orig;
        return;
    }
    if (r->rs_on && r->rs_orig) /* the game bound another state meanwhile: that one is the game's now */
        ID3D11RasterizerState_Release(r->rs_orig);
    r->rs_on = 0, r->rs_orig = NULL;
    ol_depth_state(c, &r->rs_fmt, &r->rs_func);
    int bias = ol_bias_for(r->rs_fmt, r->rs_func, bucket);
    ID3D11RasterizerState* cl = bias ? rs_clone_of(c, cur, bias) : NULL;
    if (!cl) {
        if (cur)
            ID3D11RasterizerState_Release(cur);
        return;
    }
    ID3D11DeviceContext_RSSetState(c, cl);
    r->rs_on = 1, r->rs_bias = bias, r->rs_bucket = bucket, r->rs_orig = cur,
    r->rs_clone = cl; /* cur's reference kept until bound back */
}

/* ---------------------------------------------------------------- [Quality] LineFix
 * The characters' inner ink lines (lapels, stitches, folds, frog closures) are thin strips of geometry with a flat dark
 * material, drawn before the surface they lie on (Q35). Narrower than a pixel, they cover only some pixel centres and
 * break into dashes and single dots that change with every movement. Drawing them again shifted by half a pixel
 * (redraw) fills the gaps on any GPU; conservative rasterization (where the GPU has it) draws every pixel they touch. */
static volatile LONG64 g_line_draws, g_line_hidden, g_line_redrawn;
/* the line-mesh draws by the distance the scene hook gave their object: <5, 5-10, 10-20, 20-40, 40-100, >=100 m, not a
 * character (nor carried), no scene object */
#define LB_N 8
static volatile LONG64 g_line_band[LB_N];
static int g_cr_tier = -1; /* the GPU's conservative rasterization tier, -1 not asked yet */

static ID3D11RasterizerState* cr_clone_of(ID3D11DeviceContext* c, ID3D11RasterizerState* orig)
{
    D3D11_RASTERIZER_DESC d = {D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    if (orig)
        ID3D11RasterizerState_GetDesc(orig, &d);
    ID3D11RasterizerState* out = NULL;
    AcquireSRWLockShared(&g_fx_lock);
    for (int i = 0; i < g_nrsc && !out; i++)
        if (g_rsc[i].bias == INT_MIN && !memcmp(&g_rsc[i].od, &d, sizeof(d)))
            out = g_rsc[i].clone;
    ReleaseSRWLockShared(&g_fx_lock);
    if (out)
        return out;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext_GetDevice(c, &dev);
    if (!dev)
        return NULL;
    ID3D11Device3* d3 = NULL;
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, (void**)&d3)))
        d3 = NULL;
    if (g_cr_tier < 0) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2;
        memset(&o2, 0, sizeof(o2));
        g_cr_tier = d3 && SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2)))
                        ? (int)o2.ConservativeRasterizationTier
                        : 0;
        LOGI("Line fix: conservative rasterization %s (tier %d)", g_cr_tier > 0 ? "supported" : "not supported by this GPU",
             g_cr_tier);
    }
    if (d3 && g_cr_tier > 0) {
        D3D11_RASTERIZER_DESC2 d2 = {d.FillMode, d.CullMode, d.FrontCounterClockwise, d.DepthBias, d.DepthBiasClamp,
                                     d.SlopeScaledDepthBias, d.DepthClipEnable, d.ScissorEnable, FALSE, FALSE, 0,
                                     D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON};
        ID3D11RasterizerState2* s2 = NULL;
        if (SUCCEEDED(ID3D11Device3_CreateRasterizerState2(d3, &d2, &s2)))
            out = (ID3D11RasterizerState*)s2;
    }
    if (d3)
        ID3D11Device3_Release(d3);
    ID3D11Device_Release(dev);
    if (!out)
        return NULL;
    AcquireSRWLockExclusive(&g_fx_lock);
    if (g_nrsc < RSC_N) {
        g_rsc[g_nrsc].od = d, g_rsc[g_nrsc].bias = INT_MIN, g_rsc[g_nrsc].clone = out; /* INT_MIN: conservative */
        g_nrsc++;
    } else {
        ID3D11RasterizerState_Release(out);
        out = NULL;
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
    return out;
}

static void cr_on(ID3D11DeviceContext* c, FxCtx* r)
{
    ID3D11RasterizerState* cur = NULL;
    ID3D11DeviceContext_RSGetState(c, &cur);
    if (r->cr_on && cur == r->cr_clone) {
        if (cur)
            ID3D11RasterizerState_Release(cur);
        return;
    }
    if (r->cr_on && r->cr_orig)
        ID3D11RasterizerState_Release(r->cr_orig);
    r->cr_on = 0, r->cr_orig = NULL;
    ID3D11RasterizerState* cl = cr_clone_of(c, cur);
    if (!cl) {
        if (cur)
            ID3D11RasterizerState_Release(cur);
        return;
    }
    ID3D11DeviceContext_RSSetState(c, cl);
    r->cr_on = 1, r->cr_orig = cur, r->cr_clone = cl;
}

static void cr_off(ID3D11DeviceContext* c, FxCtx* r)
{
    ID3D11RasterizerState* cur = NULL;
    ID3D11DeviceContext_RSGetState(c, &cur);
    if (cur == r->cr_clone)
        ID3D11DeviceContext_RSSetState(c, r->cr_orig);
    if (cur)
        ID3D11RasterizerState_Release(cur);
    if (r->cr_orig)
        ID3D11RasterizerState_Release(r->cr_orig);
    r->cr_on = 0, r->cr_orig = r->cr_clone = NULL;
}

int effects_redraws(void* ctx)
{
    FxCtx* r = ctx_fx(ctx);
    if (!r || !r->redraw)
        return 0;
    int n = r->redraw;
    r->redraw = 0;
    g_line_redrawn += n;
    return n;
}

/* the first other draw: the game's state back */
static void ol_bias_off(ID3D11DeviceContext* c, FxCtx* r)
{
    ID3D11RasterizerState* cur = NULL;
    ID3D11DeviceContext_RSGetState(c, &cur);
    if (cur == r->rs_clone)
        ID3D11DeviceContext_RSSetState(c, r->rs_orig);
    if (cur)
        ID3D11RasterizerState_Release(cur);
    if (r->rs_orig)
        ID3D11RasterizerState_Release(r->rs_orig);
    r->rs_on = 0, r->rs_orig = r->rs_clone = NULL;
}

static DXGI_FORMAT rt0_format(ID3D11DeviceContext* c, ID3D11RenderTargetView** out)
{
    ID3D11RenderTargetView* v = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &v, NULL);
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    if (v) {
        D3D11_RENDER_TARGET_VIEW_DESC d;
        ID3D11RenderTargetView_GetDesc(v, &d);
        f = d.Format;
    }
    *out = v;
    return f;
}

/* The characters' material shaders, once each (the first 16): the textures they read (size, format, mip levels as the
 * shader sees them) and their samplers. Thin painted lines (hair strands, stitches) that break into single pixels at
 * every distance point to textures without mip levels or samplers that do not use them. */
#define CC_N 16
static void* g_cc_ps[CC_N];
static volatile LONG g_ncc;
static void char_census(ID3D11DeviceContext* c, void* ps, float d)
{
    AcquireSRWLockExclusive(&g_fx_lock);
    int seen = 0, n = g_ncc;
    for (int i = 0; i < n && !seen; i++)
        seen = g_cc_ps[i] == ps;
    if (!seen && n < CC_N)
        g_cc_ps[n] = ps, InterlockedIncrement(&g_ncc);
    ReleaseSRWLockExclusive(&g_fx_lock);
    if (seen || n >= CC_N)
        return;
    char line[1400];
    const PsInfo* p = fx_ps_info(fx_ps_id(ps));
    int at = snprintf(line, sizeof(line), "Character material %d: pixel shader %016llx (%d instructions, %d texture reads) "
                      "at %.0f m |", n + 1, p ? (unsigned long long)p->hash : 0ULL, p ? p->instr : 0, p ? p->tex : 0, d);
    ID3D11ShaderResourceView* v[16] = {0};
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, v);
    for (int i = 0; i < 16; i++) {
        if (!v[i])
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        ID3D11ShaderResourceView_GetDesc(v[i], &vd);
        ID3D11Resource* res = NULL;
        ID3D11ShaderResourceView_GetResource(v[i], &res);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (res)
            ID3D11Resource_GetType(res, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
            at < (int)sizeof(line) - 64) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res, &td);
            unsigned mips = vd.Texture2D.MipLevels == (UINT)-1 ? td.MipLevels - vd.Texture2D.MostDetailedMip
                                                              : vd.Texture2D.MipLevels;
            at += snprintf(line + at, sizeof(line) - at, " t%d %ux%u f%d mips %u", i, td.Width, td.Height, (int)vd.Format,
                           mips);
        }
        if (res)
            ID3D11Resource_Release(res);
        ID3D11ShaderResourceView_Release(v[i]);
    }
    ID3D11SamplerState* s[16] = {0};
    ID3D11DeviceContext_PSGetSamplers(c, 0, 16, s);
    for (int i = 0; i < 16; i++) {
        if (!s[i])
            continue;
        D3D11_SAMPLER_DESC sd;
        ID3D11SamplerState_GetDesc(s[i], &sd);
        if (at < (int)sizeof(line) - 80)
            at += snprintf(line + at, sizeof(line) - at, " | s%d filter 0x%x aniso %u lod %.1f..%.1f bias %+.2f", i,
                           (unsigned)sd.Filter, sd.MaxAnisotropy, sd.MinLOD, sd.MaxLOD > 1e6f ? 99.0f : sd.MaxLOD,
                           sd.MipLODBias);
        ID3D11SamplerState_Release(s[i]);
    }
    LOGI("%s", line);
}

/* [Quality] Hatching: a view of the toon shading's hatching texture that starts this many mip levels lower (blurred
 * strokes; 99 = its last level, the average tone without strokes), made once per texture view and level */
#define HV_N 32
static struct {
    ID3D11ShaderResourceView *orig, *view;
    int level;
} g_hv[HV_N];
static int g_nhv;
static volatile LONG64 g_hatch_swaps;

static ID3D11ShaderResourceView* hatch_view(ID3D11DeviceContext* c, ID3D11ShaderResourceView* v, int level)
{
    ID3D11ShaderResourceView* out = NULL;
    AcquireSRWLockShared(&g_fx_lock);
    for (int i = 0; i < g_nhv && !out; i++)
        if (g_hv[i].orig == v && g_hv[i].level == level)
            out = g_hv[i].view;
    ReleaseSRWLockShared(&g_fx_lock);
    if (out)
        return out;
    D3D11_SHADER_RESOURCE_VIEW_DESC d;
    ID3D11ShaderResourceView_GetDesc(v, &d);
    if (d.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return NULL;
    ID3D11Resource* res = NULL;
    ID3D11ShaderResourceView_GetResource(v, &res);
    if (!res)
        return NULL;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res, &td);
    UINT top = d.Texture2D.MostDetailedMip, n = d.Texture2D.MipLevels == (UINT)-1 ? td.MipLevels - top : d.Texture2D.MipLevels;
    UINT drop = (UINT)level >= n ? n - 1 : (UINT)level;
    d.Texture2D.MostDetailedMip = top + drop, d.Texture2D.MipLevels = n - drop;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext_GetDevice(c, &dev);
    if (dev && FAILED(ID3D11Device_CreateShaderResourceView(dev, res, &d, &out)))
        out = NULL;
    if (dev)
        ID3D11Device_Release(dev);
    ID3D11Resource_Release(res);
    if (!out)
        return NULL;
    AcquireSRWLockExclusive(&g_fx_lock);
    if (g_nhv < HV_N) {
        ID3D11ShaderResourceView_AddRef(v); /* the game's view stays alive while it is a key */
        g_hv[g_nhv].orig = v, g_hv[g_nhv].view = out, g_hv[g_nhv].level = level;
        g_nhv++;
    } else {
        ID3D11ShaderResourceView_Release(out);
        out = NULL;
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
    return out;
}

/* the view the game bound, or ours for another level: the game's */
static ID3D11ShaderResourceView* hatch_orig(ID3D11ShaderResourceView* v)
{
    ID3D11ShaderResourceView* o = v;
    AcquireSRWLockShared(&g_fx_lock);
    for (int i = 0; i < g_nhv; i++)
        if (g_hv[i].view == v)
            o = g_hv[i].orig;
    ReleaseSRWLockShared(&g_fx_lock);
    return o;
}

static void hatch_draw(ID3D11DeviceContext* c, int slot)
{
    ID3D11ShaderResourceView* v = NULL;
    ID3D11DeviceContext_PSGetShaderResources(c, (UINT)slot, 1, &v);
    if (!v)
        return;
    ID3D11ShaderResourceView* game = hatch_orig(v);
    ID3D11ShaderResourceView* want = g_cfg.q_hatch > 0 ? hatch_view(c, game, g_cfg.q_hatch) : game;
    if (want && want != v) {
        ID3D11DeviceContext_PSSetShaderResources(c, (UINT)slot, 1, &want);
        g_hatch_swaps++;
    }
    ID3D11ShaderResourceView_Release(v);
}

/* [GpuProbe] DumpCharacterTextures: the material textures (base colour, toon shadow colour, toon parameters) that
 * characters near the camera are drawn with, every mip level as its own DDS file, read back on the presenting thread
 * (the textures are loaded assets: copying them a frame later is the same) */
#define TD_N 64
static struct {
    ID3D11Resource* res;
    uint64_t ps;
    int kind, done;
    float d;
} g_td[TD_N];
static volatile LONG g_ntd, g_td_armed; /* armed by F8: the characters near the camera from then on */
static char g_td_dir[MAX_PATH];

static void tex_dump_note(ID3D11DeviceContext* c, const PsInfo* p, float d)
{
    for (int m = 0; m < 3; m++) {
        int slot = p->mat_slot[m];
        if (slot < 0 || slot > 15 || g_ntd >= TD_N)
            continue;
        ID3D11ShaderResourceView* v = NULL;
        ID3D11DeviceContext_PSGetShaderResources(c, (UINT)slot, 1, &v);
        if (!v)
            continue;
        ID3D11Resource* res = NULL;
        ID3D11ShaderResourceView_GetResource(v, &res);
        ID3D11ShaderResourceView_Release(v);
        if (!res)
            continue;
        AcquireSRWLockExclusive(&g_fx_lock);
        int seen = 0;
        for (LONG i = 0; i < g_ntd && !seen; i++)
            seen = g_td[i].res == res;
        if (!seen && g_ntd < TD_N) {
            g_td[g_ntd].res = res, g_td[g_ntd].ps = p->hash, g_td[g_ntd].kind = m, g_td[g_ntd].d = d, g_td[g_ntd].done = 0;
            res = NULL; /* the reference is kept for the readback */
            InterlockedIncrement(&g_ntd);
        }
        ReleaseSRWLockExclusive(&g_fx_lock);
        if (res)
            ID3D11Resource_Release(res);
    }
}

/* CompareShots=10: which pixel shaders draw the characters near the camera, with their draw counts */
#define NP_N 64
static struct {
    uint64_t hash;
    volatile LONG64 draws;
} g_np[NP_N];
static volatile LONG g_nnp;

static void near_ps_note(const PsInfo* p)
{
    LONG n = g_nnp;
    for (LONG i = 0; i < n; i++)
        if (g_np[i].hash == p->hash) {
            InterlockedIncrement64(&g_np[i].draws);
            return;
        }
    AcquireSRWLockExclusive(&g_fx_lock);
    int seen = 0;
    for (LONG i = 0; i < g_nnp && !seen; i++)
        seen = g_np[i].hash == p->hash;
    if (!seen && g_nnp < NP_N) {
        g_np[g_nnp].hash = p->hash, g_np[g_nnp].draws = 1;
        MemoryBarrier();
        InterlockedIncrement(&g_nnp);
    }
    ReleaseSRWLockExclusive(&g_fx_lock);
}

int fx_near_char_ps(uint64_t* out, int max)
{
    int n = 0;
    LONG64 last = -1;
    for (int k = 0; k < max; k++) { /* by draws, most first (a few dozen entries: a selection pass is enough) */
        LONG64 best = -1;
        int bi = -1;
        for (LONG i = 0; i < g_nnp; i++) {
            LONG64 dr = g_np[i].draws;
            int taken = 0;
            for (int j = 0; j < n && !taken; j++)
                taken = out[j] == g_np[i].hash;
            if (!taken && dr > best && (last < 0 || dr <= last))
                best = dr, bi = (int)i;
        }
        if (bi < 0)
            break;
        out[n++] = g_np[bi].hash, last = best;
    }
    return n;
}

/* bytes per 4x4 block (block-compressed) or per texel; 0 = not written */
static UINT td_unit(DXGI_FORMAT f, int* bc)
{
    *bc = 1;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81))
        return 8; /* BC1, BC4 */
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99))
        return 16; /* BC2, BC3, BC5, BC6H, BC7 */
    *bc = 0;
    if ((f >= 27 && f <= 32) || (f >= 87 && f <= 93))
        return 4; /* RGBA8, BGRA8 */
    if (f >= 9 && f <= 14)
        return 8; /* RGBA16 */
    if (f >= 48 && f <= 52)
        return 2; /* RG8 */
    if (f >= 60 && f <= 65)
        return 1; /* R8 */
    return 0;
}

static void td_write(const char* path, DXGI_FORMAT fmt, UINT w, UINT h, const D3D11_MAPPED_SUBRESOURCE* m, UINT unit, int bc)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    UINT rows = bc ? (h + 3) / 4 : h, row_bytes = bc ? ((w + 3) / 4) * unit : w * unit;
    uint32_t hd[32 + 5];
    memset(hd, 0, sizeof(hd));
    hd[0] = 124, hd[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, hd[2] = h, hd[3] = w, hd[4] = row_bytes * rows, hd[6] = 1;
    hd[18] = 32, hd[19] = 0x4, hd[20] = 0x30315844; /* pixel format: FourCC "DX10" */
    hd[26] = 0x1000;                                /* caps: texture */
    hd[31] = (uint32_t)fmt, hd[32] = 3, hd[34] = 1;  /* DX10 header: format, 2D, one array slice */
    fwrite("DDS ", 1, 4, f);
    fwrite(hd, 4, 31 + 5, f);
    for (UINT y = 0; y < rows; y++)
        fwrite((const uint8_t*)m->pData + (size_t)y * m->RowPitch, 1, row_bytes, f);
    fclose(f);
}

void effects_present(void)
{
    if (!g_td_armed && (GetAsyncKeyState(VK_F8) & 1)) {
        g_td_armed = 1;
        LOGI("Texture dump: armed (F8): the material textures of characters within %.0f m are written from now on",
             g_cfg.dump_dist);
    }
    int n = 0;
    for (LONG i = 0; i < g_ntd && n < 4; i++) { /* a few per frame: each one stalls for its copy */
        if (g_td[i].done)
            continue;
        g_td[i].done = 1, n++;
        ID3D11Resource* res = g_td[i].res;
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        ID3D11Resource_GetType(res, &dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            continue;
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res, &d);
        int bc = 0;
        UINT unit = td_unit(d.Format, &bc);
        if (!unit || d.SampleDesc.Count != 1)
            continue;
        ID3D11Device* dev = NULL;
        ID3D11Resource_GetDevice(res, &dev);
        if (!dev)
            continue;
        ID3D11DeviceContext* imm = NULL;
        ID3D11Device_GetImmediateContext(dev, &imm);
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0;
        ID3D11Texture2D* st = NULL;
        if (imm && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st))) {
            ID3D11DeviceContext_CopyResource(imm, (ID3D11Resource*)st, res);
            if (!g_td_dir[0])
                output_dir(g_td_dir, sizeof(g_td_dir), "textures");
            static const char* const kind[3] = {"base", "shadowcolor", "params"};
            for (UINT mip = 0; mip < d.MipLevels; mip++) {
                D3D11_MAPPED_SUBRESOURCE m;
                if (FAILED(ID3D11DeviceContext_Map(imm, (ID3D11Resource*)st, mip, D3D11_MAP_READ, 0, &m)))
                    continue;
                UINT w = d.Width >> mip, h = d.Height >> mip;
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s\\tex%02ld_%s_mip%u_%ux%u.dds", g_td_dir, (long)i, kind[g_td[i].kind], mip,
                         w ? w : 1, h ? h : 1);
                td_write(path, d.Format, w ? w : 1, h ? h : 1, &m, unit, bc);
                ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)st, mip);
            }
            ID3D11Texture2D_Release(st);
            LOGI("Texture dump: tex%02ld %s of pixel shader %016llx (a character at %.1f m): %ux%u format %d, %u mip levels "
                 "-> %s",
                 (long)i, kind[g_td[i].kind], (unsigned long long)g_td[i].ps, g_td[i].d, d.Width, d.Height, (int)d.Format,
                 d.MipLevels, g_td_dir);
        }
        if (imm)
            ID3D11DeviceContext_Release(imm);
        ID3D11Device_Release(dev);
    }
}

/* before a draw: 1 = leave it out (its target cleared first where something reads it) */
int effects_draw(void* ctx, unsigned verts)
{
    FxCtx* r = ctx_fx(ctx);
    if (r && r->ps && g_cfg.q_hatch_hooks) { /* [Quality] Hatching: the toon shaders' hatching texture */
        const PsInfo* hp = fx_ps_info(fx_ps_id(r->ps));
        if (hp && hp->hatch_slot >= 0 && hp->hatch_slot < 16)
            hatch_draw((ID3D11DeviceContext*)ctx, hp->hatch_slot);
    }
    if (r && r->ps && !r->outline && (g_ncc < CC_N || (g_cfg.dump_tex && g_ntd < TD_N) || g_cfg.compare_shots == 10) &&
        g_cfg.q_outline_hooks) {
        int carried = 0; /* the characters' own materials */
        float d = gpuprobe_node_dist(&carried);
        if (d >= 0 && d < g_cfg.dump_dist && g_cfg.compare_shots == 10) { /* what draws the near characters, outlines aside */
            const PsInfo* np = fx_ps_info(fx_ps_id(r->ps));
            if (np)
                near_ps_note(np);
        }
        if (d >= 0 && !carried && g_ncc < CC_N)
            char_census((ID3D11DeviceContext*)ctx, r->ps, d);
        if (d >= 0 && !carried && g_cfg.dump_tex && g_td_armed && d < g_cfg.dump_dist) {
            const PsInfo* mp = fx_ps_info(fx_ps_id(r->ps));
            if (mp)
                tex_dump_note((ID3D11DeviceContext*)ctx, mp, d);
        }
    }
    if (r && r->by_dist && r->game_ps) { /* the object changed without a new bind: the right copy */
        void* want = alt_choice(r->game_ps, &r->by_dist);
        if (want != r->bound_ps) {
            gpuprobe_bind_ps(ctx, want);
            r->bound_ps = want;
        }
    }
    if (r && r->knock) { /* [ABTest] Compare=knockout: this shader's draws are left out */
        g_knocked++;
        return 1;
    }
    if (r && r->outline) { /* [Quality] OutlineDistance: the outlines of far characters are left out */
        g_outline_draws++;
        static volatile LONG blend_logged;
        if (!blend_logged && !InterlockedExchange(&blend_logged, 1)) { /* InnerOutlines works through the blend */
            ID3D11BlendState* bs = NULL;
            float bf[4];
            UINT mask = 0;
            ID3D11DeviceContext_OMGetBlendState((ID3D11DeviceContext*)ctx, &bs, bf, &mask);
            D3D11_BLEND_DESC d;
            memset(&d, 0, sizeof(d));
            if (bs)
                ID3D11BlendState_GetDesc(bs, &d);
            LOGI("Outlines: the outline pass's target 0 %s (source %d, destination %d, op %d; 5/6 = by the opacity)",
                 d.RenderTarget[0].BlendEnable ? "blends" : "does not blend", (int)d.RenderTarget[0].SrcBlend,
                 (int)d.RenderTarget[0].DestBlend, (int)d.RenderTarget[0].BlendOp);
            if (bs)
                ID3D11BlendState_Release(bs);
        }
        int carried = 0;
        float d = gpuprobe_node_dist(&carried); /* -2: not a character nor carried by one, -1: not known */
        int band = d == -2 ? 5 : d < 0 ? 6 : d < 15 ? 0 : d < 30 ? 1 : d < 50 ? 2 : d < 100 ? 3 : 4;
        g_outline_verts[band] += verts;
        if (carried)
            g_outline_verts[7] += verts;
        if (g_cfg.q_outline_dist > 0 && d >= 0 && d > g_cfg.q_outline_dist) {
            g_outline_skipped++;
            return 1;
        }
        if (g_cfg.q_ol_depth > 0 && d >= 0) /* characters and what they carry (not 3D texts, speech bubbles) */
            ol_bias_on((ID3D11DeviceContext*)ctx, r, d);
        else if (r->rs_on)
            ol_bias_off((ID3D11DeviceContext*)ctx, r);
    } else if (r && r->rs_on) {
        ol_bias_off((ID3D11DeviceContext*)ctx, r);
    }
    if (r && r->line) { /* LineFix: a line mesh of a character (not of a prop) */
        int carried = 0;
        float d = gpuprobe_node_dist(&carried);
        g_line_band[d == -2 ? 6 : d < 0 ? 7 : d < 5 ? 0 : d < 10 ? 1 : d < 20 ? 2 : d < 40 ? 3 : d < 100 ? 4 : 5]++;
        if (!g_cfg.q_line_mode) {
            if (r->cr_on)
                cr_off((ID3D11DeviceContext*)ctx, r);
        } else if (d >= 0 && (g_cfg.q_line_dist <= 0 || d <= g_cfg.q_line_dist)) {
            g_line_draws++;
            if (g_cfg.q_line_mode == 3) {
                g_line_hidden++;
                return 1;
            }
            if (g_cfg.q_line_mode == 1)
                r->redraw = g_cfg.q_line_redraws;
            else if (g_cfg.q_line_mode == 2)
                cr_on((ID3D11DeviceContext*)ctx, r);
        } else if (r->cr_on) {
            cr_off((ID3D11DeviceContext*)ctx, r);
        }
    } else if (r && r->cr_on) {
        cr_off((ID3D11DeviceContext*)ctx, r);
    }
    int role = r ? r->role : R_NONE;
    if (!role)
        return 0;
    g_fx_seen[role]++;
    if (role == R_PAINT && g_cfg.q_stab_hooks) /* TemporalStabilise: the character flags and the depth of this frame */
        temporal_inputs(ctx);
    else if (role == R_SMAA_EDGES && g_cfg.q_stab_hooks) /* the finished image, before SMAA */
        temporal_run(ctx);
    int skip = 0, clear = 0;
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    ID3D11RenderTargetView* v = NULL;
    switch (role) {
    case R_FOG:
        skip = clear = !g_cfg.q_fog;
        break;
    case R_GLARE_DOWN:
    case R_GLARE_BLUR:
    case R_GLARE_COMBINE:
        if (!g_cfg.q_glare && rt0_format(c, &v) == DXGI_FORMAT_R11G11B10_FLOAT)
            skip = 1, clear = role == R_GLARE_COMBINE;
        break;
    case R_SMAA_EDGES:
        skip = g_cfg.q_smaa == 0;
        break;
    case R_SMAA_WEIGHTS:
        skip = clear = g_cfg.q_smaa == 0;
        break;
    }
    if (clear) {
        if (!v)
            rt0_format(c, &v);
        static const float zero[4] = {0, 0, 0, 0};
        if (v)
            ID3D11DeviceContext_ClearRenderTargetView(c, v, zero);
    }
    if (v)
        ID3D11RenderTargetView_Release(v);
    if (skip)
        g_fx_skipped[role]++;
    return skip;
}

void effects_report(void)
{
    static LONG64 ps[R_N], pk[R_N], pb, pp;
    if (!g_cfg.q_fx_active)
        return;
    LONG64 s[R_N], k[R_N];
    for (int i = 0; i < R_N; i++)
        s[i] = g_fx_seen[i] - ps[i], k[i] = g_fx_skipped[i] - pk[i], ps[i] = g_fx_seen[i], pk[i] = g_fx_skipped[i];
    static const char* const smaa[4] = {"off", "Medium", "High", "Ultra (the game's)"};
    LOGI("  Effects: fog draws %lld (left out %lld), glare %lld (left out %lld), SMAA edges %lld (left out %lld), "
         "weights %lld (left out %lld) | SMAA %s; variants: %d pixel shaders, %d vertex shaders found | painterly "
         "wobble bound %lld times, %lld of them as the plain copy",
         s[R_FOG], k[R_FOG], s[R_GLARE_DOWN] + s[R_GLARE_BLUR] + s[R_GLARE_COMBINE],
         k[R_GLARE_DOWN] + k[R_GLARE_BLUR] + k[R_GLARE_COMBINE], s[R_SMAA_EDGES], k[R_SMAA_EDGES], s[R_SMAA_WEIGHTS],
         k[R_SMAA_WEIGHTS], smaa[g_cfg.q_smaa & 3], g_ps_patched, g_vs_found, (long long)(g_paint_binds - pb),
         (long long)(g_paint_plain - pp));
    pb = g_paint_binds, pp = g_paint_plain;
    static LONG64 pkn, pod, pos;
    LONG64 kn = g_knocked - pkn, od = g_outline_draws - pod, os = g_outline_skipped - pos;
    pkn = g_knocked, pod = g_outline_draws, pos = g_outline_skipped;
    if (g_cfg.q_outline_hooks) {
        char lim[48];
        if (g_cfg.q_outline_dist <= 0)
            snprintf(lim, sizeof(lim), "none left out: the game's");
        else if (g_cfg.q_outline_dist < 1)
            snprintf(lim, sizeof(lim), "all left out");
        else
            snprintf(lim, sizeof(lim), "left out beyond %.0f m", g_cfg.q_outline_dist);
        static LONG64 pv[OL_BANDS];
        LONG64 v[OL_BANDS], vt = 0;
        for (int i = 0; i < OL_BANDS; i++)
            v[i] = g_outline_verts[i] - pv[i], pv[i] = g_outline_verts[i], vt += i < 7 ? v[i] : 0;
        double f = vt ? 100.0 / (double)vt : 0;
        LOGI("  Outlines: %lld outline draws in this window, %lld left out (%s) | their vertices by the character's "
             "distance <15 / 15-30 / 30-50 / 50-100 / >100 m: %.0f / %.0f / %.0f / %.0f / %.0f%% (carried objects: %.0f%%), "
             "not characters (3D text, bubbles) %.0f%%, no scene object %.0f%%",
             (long long)od, (long long)os, lim, v[0] * f, v[1] * f, v[2] * f, v[3] * f, v[4] * f, v[7] * f, v[5] * f,
             v[6] * f);
    }
    if (g_cfg.q_rim_hooks)
        LOGI("  Rim light: %ld toon shaders with a rim-free copy (%ld not patched) | rim %s", (long)g_rim_shaders,
             (long)g_rim_failed, g_cfg.q_rim_dist <= 0 ? "as the game" : g_cfg.q_rim_dist < 1 ? "off everywhere" : "only near");
    if (g_cfg.q_line_hooks) {
        static LONG64 pld, plh, plr;
        LONG64 ld = g_line_draws - pld, lh = g_line_hidden - plh, lr = g_line_redrawn - plr;
        pld = g_line_draws, plh = g_line_hidden, plr = g_line_redrawn;
        static const char* const mode[4] = {"as the game", "drawn again shifted", "conservative rasterization", "left out"};
        static LONG64 pb[LB_N];
        LONG64 b[LB_N], bt = 0;
        for (int i = 0; i < LB_N; i++)
            b[i] = g_line_band[i] - pb[i], pb[i] = g_line_band[i], bt += b[i];
        double f = bt ? 100.0 / (double)bt : 0;
        LOGI("  Line fix: characters' line-mesh draws %lld in this window (%s, %d copies): %lld shifted copies, %lld left out "
             "| all draws of the line shaders %lld, by their object's distance <5 / 5-10 / 10-20 / 20-40 / 40-100 / >100 m: "
             "%.0f / %.0f / %.0f / %.0f / %.0f / %.0f%%, not characters (props) %.0f%%, no scene object %.0f%%",
             (long long)ld, mode[g_cfg.q_line_mode & 3], g_cfg.q_line_redraws, (long long)lr, (long long)lh, (long long)bt,
             b[0] * f, b[1] * f, b[2] * f, b[3] * f, b[4] * f, b[5] * f, b[6] * f, b[7] * f);
    }
    if (g_cfg.q_hatch_hooks) {
        static LONG64 phs;
        LONG64 hs = g_hatch_swaps - phs;
        phs = g_hatch_swaps;
        LOGI("  Hatching: texture %s in this window: %lld binds swapped, %d views made",
             g_cfg.q_hatch <= 0 ? "as the game" : g_cfg.q_hatch >= 99 ? "flat (its average)" : "from lower mip levels",
             (long long)hs, g_nhv);
    }
    if (g_cfg.q_stab_hooks)
        temporal_report();
    if (g_cfg.q_inner_hooks)
        LOGI("  Inner outlines: %ld outline shaders with a copy without them (%ld not patched) | inner lines %s",
             (long)g_inner_shaders, (long)g_inner_failed,
             g_cfg.q_inner_dist <= 0 ? "as the game" : g_cfg.q_inner_dist < 1 ? "off everywhere" : "only near");
    if (g_cfg.q_knock_n)
        LOGI("  Knock-out: pixel shader %016llx left out: %lld draws in this window | pixel shaders known: %ld (%ld objects)",
             (unsigned long long)g_cfg.q_knock, (long long)kn, (long)g_nps - 1, (long)g_npsobj);
}
