/* [Quality] FarMeshes: simpler versions of the world's far meshes, made by the mod itself.
 *
 * The game has no working switch to simpler world models and draws its far meshes
 * at full detail, often under one pixel per triangle (Q14). Here each world mesh drawn
 * far enough away is read back from the GPU once, simplified in the background with meshoptimizer
 * into versions whose shape error is a fixed size in the model (2 / 4 / 8 / 16 units, cm in this
 * game), and each draw of it - the scene's and the shadow maps' - takes the coarsest version whose
 * error stays under FarMeshPixels on screen at the object's nearest distance (bounding sphere).
 * The versions are index lists over the game's own vertices: same vertex buffer, shaders and
 * textures. Until they exist the original is drawn.
 *   safe:  the mesh border and the texture / normal seams stay where they are;
 *   seams: collapses across the seams too (far fewer triangles; small texture shifts possible at
 *          the seams). The border stays in both.
 * A version never has fewer than FarMeshMinPercent of the triangles, so thin detail does not
 * vanish. A mesh that is ever drawn outside a known scene object (and outside the shadow maps) is
 * never swapped: passes that must match each other (a depth pre-pass and its colour pass) then
 * always draw the same triangles.
 *
 * Threads: draws (any thread) look meshes up under a shared lock; new meshes are queued; the
 * presenting thread reads them back through staging copies on the immediate context (mapped a few
 * frames later, never waiting); one background thread simplifies and creates the index buffer. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/meshoptimizer/src/meshoptimizer.h"

#define FM_N 16384
#define FM_LEVELS 4
#define FM_MIN_INDICES 1536             /* smaller meshes: little to gain */
#define FM_MAX_VERTS 65536
#define FM_FLIGHT 8                     /* read-backs in flight */
#define FM_QUEUE 4096
static const float k_err[FM_LEVELS] = {2.0f, 4.0f, 8.0f, 16.0f}; /* model units */

enum { FM_FREE = 0, FM_NEW, FM_READ_IB, FM_READ_VB, FM_WORK, FM_READY, FM_NONE, FM_DEAD };

typedef struct FMesh {
    ID3D11Buffer *ib, *vb; /* held while the entry lives (NULL for a placeholder of an unmarked mesh) */
    UINT ib_off, start, count, vb_off, vb_stride, pos_off;
    INT base;
    DXGI_FORMAT fmt;
    volatile LONG state;
    volatile LONG unmarked;
    volatile LONG64 last_frame;
    ID3D11Buffer* st; /* staging copy being read back */
    LONG64 st_frame;
    uint32_t lo, hi;
    uint32_t* idx; /* indices - lo, while simplifying */
    float* pos;    /* positions of vertices lo..hi */
    ID3D11Buffer* lod;
    UINT lvl_start[2][FM_LEVELS], lvl_count[2][FM_LEVELS];
} FMesh;

static FMesh* g_fm;
static SRWLOCK g_lock = SRWLOCK_INIT;
static FMesh* g_new[FM_QUEUE]; /* waiting for a read-back (under g_lock exclusive) */
static int g_new_head, g_new_n;
static FMesh* g_flight[FM_FLIGHT];
static FMesh* g_work[FM_QUEUE]; /* waiting for the worker */
static int g_work_head, g_work_n;
static CRITICAL_SECTION g_work_cs;
static HANDLE g_work_sem;
static volatile LONG64 g_frame;
static volatile float g_p11 = 1.73f;
static volatile LONG g_height = 900;
static int g_init;
/* statistics */
static volatile LONG64 st_swaps, st_tris_before, st_tris_after, st_unmarked, st_done, st_none, st_tris_in,
    st_work_us, st_read_bytes, st_failed, st_area_rejected;
static volatile LONG64 st_sh_swaps, st_sh_before, st_sh_after; /* the shadow-map share of the swaps */

static int fm_enabled(void) { return g_cfg.q_farmesh || g_cfg.q_fm_both; }

static size_t fm_hash(ID3D11Buffer* ib, UINT off, UINT start, UINT count, INT base)
{
    uint64_t h = (uintptr_t)ib * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)start << 32 | count) * 0xC2B2AE3D27D4EB4Full;
    h ^= ((uint64_t)(uint32_t)base << 32 | off) * 0x165667B19E3779F9ull;
    return (size_t)(h >> 18) % FM_N;
}

/* caller holds g_lock (exclusive to insert); insert: a new slot is set up with the key */
static FMesh* fm_find(ID3D11Buffer* ib, UINT off, UINT start, UINT count, INT base, int insert)
{
    size_t h = fm_hash(ib, off, start, count, base);
    FMesh* dead = NULL;
    for (int k = 0; k < 128; k++, h = (h + 1) % FM_N) {
        FMesh* e = &g_fm[h];
        LONG s = e->state;
        if (s == FM_FREE) {
            if (!insert)
                return NULL;
            FMesh* n = dead ? dead : e;
            memset(n, 0, sizeof(*n));
            n->ib = ib, n->ib_off = off, n->start = start, n->count = count, n->base = base;
            return n;
        }
        if (s == FM_DEAD) {
            if (!dead)
                dead = e;
            continue;
        }
        if (e->ib == ib && e->ib_off == off && e->start == start && e->count == count && e->base == base)
            return e;
    }
    if (insert && dead) {
        memset(dead, 0, sizeof(*dead));
        dead->ib = ib, dead->ib_off = off, dead->start = start, dead->count = count, dead->base = base;
        return dead;
    }
    return NULL;
}

/* the camera's projection [1][1] (scene hook) and the back buffer height (Present) */
void farmesh_view(float p11)
{
    if (p11 > 0.05f && p11 < 100.0f)
        g_p11 = p11;
}

void farmesh_present(void* swapchain)
{
    static unsigned n;
    if (!swapchain || (n++ & 127))
        return;
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(IDXGISwapChain_GetDesc((IDXGISwapChain*)swapchain, &d)) && d.BufferDesc.Height >= 200)
        InterlockedExchange(&g_height, (LONG)d.BufferDesc.Height);
}

/* ---------------------------------------------------------------- draws (any thread) */
/* kind: 1 = a world object of the scene, 2 = a static shadow caster, 0 = an indexed draw outside
 * both (outside the shadow maps); near_cm = the object's nearest distance, scale = its world scale;
 * tpu = shadow-map texels per world unit at a caster (kind 2). A scene draw's version must stay
 * under FarMeshPixels on screen; a caster's under FarMeshShadowTexels in its shadow map when that
 * is set (else it is judged by the screen too). Returns 1 with a version to draw instead
 * (farmesh_done after the draw). */
static int fm_level_ok(int k, int kind, float near_cm, float scale, float tpu)
{
    if (kind == 2 && g_cfg.q_fm_shadow > 0)
        return tpu > 0 && k_err[k] * scale * tpu <= g_cfg.q_fm_shadow;
    float K = g_p11 * (float)g_height * 0.5f;
    return near_cm > 0 && k_err[k] * scale * K <= g_cfg.q_fm_px * near_cm;
}

int farmesh_pick(void* ctx, unsigned count, unsigned start, int base, int kind, float near_cm, float scale, float tpu,
                 FmPick* p)
{
    if (!g_init || count < FM_MIN_INDICES || kind > 2)
        return 0;
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    ID3D11Buffer* ib = NULL;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT off = 0;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &fmt, &off);
    if (!ib)
        return 0;
    if (fmt != DXGI_FORMAT_R16_UINT && fmt != DXGI_FORMAT_R32_UINT) {
        ID3D11Buffer_Release(ib);
        return 0;
    }
    LONG64 frame = g_frame;
    AcquireSRWLockShared(&g_lock);
    FMesh* e = fm_find(ib, off, start, count, base, 0);
    if (e) {
        if (e->last_frame != frame)
            e->last_frame = frame;
        if (kind == 0) {
            if (!e->unmarked) {
                InterlockedExchange(&e->unmarked, 1);
                InterlockedIncrement64(&st_unmarked);
            }
        } else if (g_cfg.q_farmesh && e->state == FM_READY && !e->unmarked && scale > 0) {
            /* across seams: in the shadow maps (only the outline counts there) and from FarMeshSeamsFrom on;
             * nearer, the versions with the seams kept (block and column edges keep their shading) */
            int m = g_cfg.q_farmesh == 2 && (kind == 2 || g_cfg.q_fm_seams_from <= 0 ||
                                             near_cm >= g_cfg.q_fm_seams_from * 100.0f)
                        ? 1
                        : 0;
            if (m && !e->lvl_count[1][0] && !e->lvl_count[1][1] && !e->lvl_count[1][2] && !e->lvl_count[1][3])
                m = 0; /* no version across seams (not made, or none useful): the one with the seams kept */
            for (int k = FM_LEVELS - 1; k >= 0; k--)
                if (e->lvl_count[m][k] && fm_level_ok(k, kind, near_cm, scale, tpu)) {
                    p->ib = ib, p->fmt = fmt, p->off = off, p->lod = e->lod;
                    p->start = e->lvl_start[m][k], p->count = e->lvl_count[m][k];
                    ID3D11Buffer_AddRef(e->lod);
                    ReleaseSRWLockShared(&g_lock);
                    st_swaps++, st_tris_before += count / 3, st_tris_after += p->count / 3; /* statistics */
                    if (kind == 2)
                        st_sh_swaps++, st_sh_before += count / 3, st_sh_after += p->count / 3;
                    return 1;
                }
        }
        ReleaseSRWLockShared(&g_lock);
        ID3D11Buffer_Release(ib);
        return 0;
    }
    ReleaseSRWLockShared(&g_lock);
    /* a new mesh: queued if it is a static world mesh with float positions at the start of buffer 0;
     * an unmarked one gets a placeholder that keeps it from ever being swapped */
    ID3D11Buffer* vb = NULL;
    ID3D11InputLayout* lay = NULL;
    UINT stride = 0, voff = 0;
    int ok = 0, pfmt = 0, poff = 0, pslot = -1, skinned = 0;
    /* scene objects from 15 m; casters whose shadow-map texels already allow the finest version */
    if ((kind == 1 && near_cm >= 15.0f * 100.0f) ||
        (kind == 2 && g_cfg.q_fm_shadow > 0 && scale > 0 && fm_level_ok(0, 2, near_cm, scale, tpu))) {
        ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &vb, &stride, &voff);
        ID3D11DeviceContext_IAGetInputLayout(c, &lay);
        ok = vb && lay && mesh_layout_pos(lay, &pfmt, &poff, &pslot, &skinned) && pslot == 0 && !skinned &&
             (pfmt == DXGI_FORMAT_R32G32B32_FLOAT || pfmt == DXGI_FORMAT_R32G32B32A32_FLOAT) && stride >= 12 &&
             (UINT)poff + 12 <= stride;
    }
    if (ok || kind == 0) {
        AcquireSRWLockExclusive(&g_lock);
        e = fm_find(ib, off, start, count, base, 0);
        if (!e && (kind == 0 || g_new_n < FM_QUEUE))
            e = fm_find(ib, off, start, count, base, 1);
        if (e && e->state == FM_FREE) {
            e->last_frame = frame;
            if (kind == 0) { /* placeholder: no references held */
                e->ib = ib, e->unmarked = 1, e->state = FM_NONE;
                st_unmarked++;
            } else {
                e->fmt = fmt, e->vb_off = voff, e->vb_stride = stride, e->pos_off = (UINT)poff;
                e->vb = vb;
                ID3D11Buffer_AddRef(ib);
                ID3D11Buffer_AddRef(vb);
                e->state = FM_NEW;
                g_new[(g_new_head + g_new_n++) % FM_QUEUE] = e;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (vb)
        ID3D11Buffer_Release(vb);
    if (lay)
        ID3D11InputLayout_Release(lay);
    ID3D11Buffer_Release(ib);
    return 0;
}

void farmesh_done(FmPick* p)
{
    if (p->lod)
        ID3D11Buffer_Release((ID3D11Buffer*)p->lod);
    if (p->ib)
        ID3D11Buffer_Release((ID3D11Buffer*)p->ib);
    p->lod = p->ib = NULL;
}

/* ---------------------------------------------------------------- background simplification */
/* surface area of a triangle list (model units squared) */
static double fm_area(const float* p, const uint32_t* ix, size_t n)
{
    double a = 0;
    for (size_t i = 0; i + 2 < n; i += 3) {
        const float *A = p + 3 * ix[i], *B = p + 3 * ix[i + 1], *C = p + 3 * ix[i + 2];
        double u0 = B[0] - A[0], u1 = B[1] - A[1], u2 = B[2] - A[2];
        double v0 = C[0] - A[0], v1 = C[1] - A[1], v2 = C[2] - A[2];
        double c0 = u1 * v2 - u2 * v1, c1 = u2 * v0 - u0 * v2, c2 = u0 * v1 - u1 * v0;
        a += 0.5 * sqrt(c0 * c0 + c1 * c1 + c2 * c2);
    }
    return a;
}

/* A version must keep at least this share of the surface: simplification can make whole small
 * pieces collapse (foliage cards, thin bushes: PC check of 24 Oct 2026, trees in Sunlumeo lost
 * their leaves across seams, desert bushes thinned with the seams kept) while their shape error
 * stays small; big meshes keep 80-100% of their area. */
#define FM_MIN_AREA 0.75

static void fm_simplify(FMesh* e)
{
    LARGE_INTEGER f0, f1, qf;
    QueryPerformanceCounter(&f0);
    size_t n = e->count, nv = (size_t)(e->hi - e->lo) + 1;
    int modes[2], nm = 0;
    if (g_cfg.q_fm_both || (g_cfg.q_farmesh == 2 && g_cfg.q_fm_seams_from > 0))
        modes[nm++] = 0, modes[nm++] = 1; /* switching, or seams far away with the seams kept nearer */
    else
        modes[nm++] = g_cfg.q_farmesh == 2 ? 1 : 0;
    uint32_t* out = (uint32_t*)malloc(sizeof(uint32_t) * n);
    uint32_t* all = (uint32_t*)malloc(sizeof(uint32_t) * n * FM_LEVELS * nm);
    size_t total = 0;
    size_t floor = (size_t)((double)n * g_cfg.q_fm_floor / 100.0) / 3 * 3;
    if (floor < 3)
        floor = 3;
    double a0 = fm_area(e->pos, e->idx, n);
    for (int mi = 0; out && all && mi < nm; mi++) {
        int m = modes[mi];
        unsigned opt = meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder | (m ? meshopt_SimplifyPermissive : 0);
        size_t prev = n;
        for (int k = 0; k < FM_LEVELS; k++) {
            float err = 0;
            size_t r = meshopt_simplify(out, e->idx, n, e->pos, nv, sizeof(float) * 3, floor, k_err[k], opt, &err);
            if (r < 3 || (double)r > 0.85 * (double)prev)
                continue; /* not enough fewer triangles than the finer version for a version of its own */
            if (fm_area(e->pos, out, r) < FM_MIN_AREA * a0) {
                st_area_rejected++;
                break; /* pieces vanished; the coarser versions would lose more */
            }
            e->lvl_start[m][k] = (UINT)total, e->lvl_count[m][k] = (UINT)r;
            for (size_t i = 0; i < r; i++)
                all[total + i] = out[i] + e->lo;
            total += r;
            prev = r;
            if (r <= floor + 3)
                break; /* at the floor: the coarser versions would be the same */
        }
    }
    ID3D11Buffer* lod = NULL;
    if (total) {
        ID3D11Device* dev = NULL;
        ID3D11Buffer_GetDevice(e->ib, &dev);
        int b16 = e->fmt == DXGI_FORMAT_R16_UINT;
        void* data = all;
        if (b16) { /* the game's own index size */
            uint16_t* s = (uint16_t*)all;
            for (size_t i = 0; i < total; i++)
                s[i] = (uint16_t)all[i];
        }
        D3D11_BUFFER_DESC d = {(UINT)(total * (b16 ? 2 : 4)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
        if (dev) {
            if (FAILED(ID3D11Device_CreateBuffer(dev, &d, &init, &lod)))
                lod = NULL;
            ID3D11Device_Release(dev);
        }
    }
    free(out);
    free(all);
    free(e->idx);
    free(e->pos);
    e->idx = NULL, e->pos = NULL;
    QueryPerformanceCounter(&f1);
    QueryPerformanceFrequency(&qf);
    InterlockedAdd64(&st_work_us, (LONG64)((f1.QuadPart - f0.QuadPart) * 1000000 / qf.QuadPart));
    InterlockedAdd64(&st_tris_in, (LONG64)(n / 3));
    if (lod) {
        e->lod = lod;
        MemoryBarrier();
        InterlockedExchange(&e->state, FM_READY);
        InterlockedIncrement64(&st_done);
    } else {
        InterlockedExchange(&e->state, FM_NONE);
        InterlockedIncrement64(&st_none);
    }
}

static DWORD WINAPI fm_worker(void* arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WaitForSingleObject(g_work_sem, INFINITE);
        FMesh* e = NULL;
        EnterCriticalSection(&g_work_cs);
        if (g_work_n) {
            e = g_work[g_work_head];
            g_work_head = (g_work_head + 1) % FM_QUEUE, g_work_n--;
        }
        LeaveCriticalSection(&g_work_cs);
        if (e)
            fm_simplify(e);
    }
    return 0;
}

static void fm_queue_work(FMesh* e)
{
    EnterCriticalSection(&g_work_cs);
    int ok = g_work_n < FM_QUEUE;
    if (ok)
        g_work[(g_work_head + g_work_n++) % FM_QUEUE] = e;
    LeaveCriticalSection(&g_work_cs);
    if (ok) {
        InterlockedExchange(&e->state, FM_WORK);
        ReleaseSemaphore(g_work_sem, 1, NULL);
    } else {
        free(e->idx), free(e->pos);
        e->idx = NULL, e->pos = NULL;
        InterlockedExchange(&e->state, FM_NONE);
    }
}

/* ---------------------------------------------------------------- read-back (presenting thread) */
static ID3D11Buffer* fm_copy(ID3D11DeviceContext* imm, ID3D11Buffer* src, UINT off, UINT bytes)
{
    D3D11_BUFFER_DESC sd;
    ID3D11Buffer_GetDesc(src, &sd);
    if (!bytes || off > sd.ByteWidth || bytes > sd.ByteWidth - off)
        return NULL;
    ID3D11Device* dev = NULL;
    ID3D11Buffer_GetDevice(src, &dev);
    if (!dev)
        return NULL;
    D3D11_BUFFER_DESC d = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer* st = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, NULL, &st)))
        st = NULL;
    ID3D11Device_Release(dev);
    if (st) {
        D3D11_BOX box = {off, 0, 0, off + bytes, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(imm, (ID3D11Resource*)st, 0, 0, 0, 0, (ID3D11Resource*)src, 0, &box);
        st_read_bytes += bytes;
    }
    return st;
}

static void fm_fail(FMesh* e)
{
    if (e->st)
        ID3D11Buffer_Release(e->st);
    e->st = NULL;
    free(e->idx), free(e->pos);
    e->idx = NULL, e->pos = NULL;
    InterlockedExchange(&e->state, FM_NONE);
    st_failed++;
}

/* a read-back that has arrived (mapped; the caller unmaps and releases it): indices -> start
 * reading the vertices; vertices -> to the worker. Returns 1 when the entry leaves the flight
 * list, -1 on failure. */
static int fm_arrived(ID3D11DeviceContext* imm, FMesh* e, const D3D11_MAPPED_SUBRESOURCE* mp)
{
    if (e->state == FM_READ_IB) {
        int b16 = e->fmt == DXGI_FORMAT_R16_UINT;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        e->idx = (uint32_t*)malloc(sizeof(uint32_t) * e->count);
        if (!e->idx)
            return -1;
        for (UINT i = 0; i < e->count; i++) {
            uint32_t v = b16 ? ((const uint16_t*)mp->pData)[i] : ((const uint32_t*)mp->pData)[i];
            e->idx[i] = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi < lo || hi - lo >= FM_MAX_VERTS || (int64_t)e->base + lo < 0)
            return -1;
        for (UINT i = 0; i < e->count; i++)
            e->idx[i] -= lo;
        e->lo = lo, e->hi = hi;
        UINT nv = hi - lo + 1;
        e->st = fm_copy(imm, e->vb, e->vb_off + (UINT)((int64_t)e->base + lo) * e->vb_stride, nv * e->vb_stride);
        if (!e->st)
            return -1;
        e->st_frame = g_frame;
        e->state = FM_READ_VB;
        return 0;
    }
    UINT nv = e->hi - e->lo + 1;
    e->pos = (float*)malloc(sizeof(float) * 3 * nv);
    if (!e->pos)
        return -1;
    for (UINT i = 0; i < nv; i++)
        memcpy(e->pos + 3 * i, (const uint8_t*)mp->pData + (size_t)i * e->vb_stride + e->pos_off, 12);
    fm_queue_work(e);
    return 1;
}

/* entries not drawn for a minute whose buffers only we still hold: let them go */
/* how many references the entries hold on each buffer (index and vertex buffers; many meshes
 * share one) - a buffer the game has let go has exactly that many left */
#define FM_RC_N (2 * FM_N)
typedef struct {
    void* b;
    ULONG n;
} FmRef;

static FmRef* fm_ref(FmRef* t, void* b)
{
    size_t h = ((uintptr_t)b >> 4) * 0x9E3779B97F4A7C15ull >> 40;
    for (int k = 0; k < FM_RC_N; k++, h++) {
        FmRef* r = &t[h % FM_RC_N];
        if (!r->b || r->b == b) {
            r->b = b;
            return r;
        }
    }
    return NULL;
}

/* entries not drawn for a minute whose index buffer only the mod still holds: let them go */
static void fm_collect(void)
{
    FmRef* refs = (FmRef*)calloc(FM_RC_N, sizeof(FmRef));
    if (!refs)
        return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < FM_N; i++) { /* placeholders (no vertex buffer) hold no references */
        FMesh* e = &g_fm[i];
        if (e->state != FM_FREE && e->state != FM_DEAD && e->vb) {
            FmRef* r = fm_ref(refs, e->ib);
            if (r)
                r->n++;
            if ((r = fm_ref(refs, e->vb)) != NULL)
                r->n++;
        }
    }
    for (int i = 0; i < FM_N; i++) {
        FMesh* e = &g_fm[i];
        LONG s = e->state;
        if ((s != FM_READY && s != FM_NONE) || g_frame - e->last_frame < 1800)
            continue;
        if (e->vb) {
            FmRef* r = fm_ref(refs, e->ib);
            ULONG rc = ID3D11Buffer_AddRef(e->ib) - 1;
            ID3D11Buffer_Release(e->ib);
            if (!r || rc > r->n) /* the game still has it */
                continue;
            r->n--;
            if ((r = fm_ref(refs, e->vb)) != NULL && r->n)
                r->n--;
            ID3D11Buffer_Release(e->ib);
            ID3D11Buffer_Release(e->vb);
        }
        if (e->lod)
            ID3D11Buffer_Release(e->lod);
        memset(e, 0, sizeof(*e));
        e->state = FM_DEAD;
    }
    ReleaseSRWLockExclusive(&g_lock);
    free(refs);
}

void farmesh_frame_end(void* imm_)
{
    ID3D11DeviceContext* imm = (ID3D11DeviceContext*)imm_;
    InterlockedIncrement64(&g_frame);
    if (!g_init || !imm)
        return;
    /* 1. read-backs in flight */
    for (int i = 0; i < FM_FLIGHT; i++) {
        FMesh* e = g_flight[i];
        if (!e || g_frame - e->st_frame < 2)
            continue;
        D3D11_MAPPED_SUBRESOURCE mp;
        HRESULT hr = ID3D11DeviceContext_Map(imm, (ID3D11Resource*)e->st, 0, D3D11_MAP_READ,
                                             D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (g_frame - e->st_frame > 120)
                fm_fail(e), g_flight[i] = NULL;
            continue;
        }
        if (FAILED(hr)) {
            fm_fail(e), g_flight[i] = NULL;
            continue;
        }
        ID3D11Buffer* st = e->st;
        e->st = NULL;
        int r = fm_arrived(imm, e, &mp);
        ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)st, 0);
        ID3D11Buffer_Release(st);
        if (r < 0)
            fm_fail(e);
        if (r != 0)
            g_flight[i] = NULL;
    }
    /* 2. new read-backs: at most 2 per frame and 8 MB */
    UINT bytes = 0;
    for (int started = 0; started < 2 && bytes < (8u << 20);) {
        int slot = -1;
        for (int i = 0; i < FM_FLIGHT && slot < 0; i++)
            if (!g_flight[i])
                slot = i;
        if (slot < 0)
            break;
        AcquireSRWLockExclusive(&g_lock);
        FMesh* e = NULL;
        if (g_new_n) {
            e = g_new[g_new_head];
            g_new_head = (g_new_head + 1) % FM_QUEUE, g_new_n--;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!e)
            break;
        if (e->unmarked || !fm_enabled()) {
            e->state = FM_NONE;
            continue;
        }
        UINT isz = e->fmt == DXGI_FORMAT_R16_UINT ? 2 : 4;
        e->st = fm_copy(imm, e->ib, e->ib_off + e->start * isz, e->count * isz);
        if (!e->st) {
            fm_fail(e);
            continue;
        }
        e->st_frame = g_frame;
        e->state = FM_READ_IB;
        g_flight[slot] = e;
        bytes += e->count * isz;
        started++;
    }
    if ((g_frame % 600) == 0)
        fm_collect();
}

/* ---------------------------------------------------------------- set-up and report */
void farmesh_init(void)
{
    if (g_init || !fm_enabled())
        return;
    g_fm = (FMesh*)calloc(FM_N, sizeof(FMesh));
    if (!g_fm)
        return;
    InitializeCriticalSection(&g_work_cs);
    g_work_sem = CreateSemaphoreA(NULL, 0, FM_QUEUE, NULL);
    HANDLE t = g_work_sem ? CreateThread(NULL, 0, fm_worker, NULL, 0, NULL) : NULL;
    if (!t)
        return;
    typedef HRESULT(WINAPI * STD)(HANDLE, PCWSTR);
    STD std = (STD)(void*)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetThreadDescription");
    if (std)
        std(t, L"MetaphorCPU-farmesh");
    CloseHandle(t);
    g_init = 1;
    char seams[64] = "simplified across texture seams";
    if (g_cfg.q_fm_seams_from > 0)
        snprintf(seams, sizeof(seams), "simplified across seams from %.0f m (seams kept nearer)", g_cfg.q_fm_seams_from);
    LOGI("Far meshes: %s, at most %.2f px of shape error, at least %d%% of the triangles kept%s",
         g_cfg.q_farmesh == 2 ? seams : g_cfg.q_farmesh ? "simplified, seams kept" : "off", g_cfg.q_fm_px,
         g_cfg.q_fm_floor, g_cfg.q_fm_both ? " (both kinds made, for switching)" : "");
}

void farmesh_report(void)
{
    if (!g_init)
        return;
    static LONG64 p_frame, p_swaps, p_before, p_after, p_us, p_tris, p_ssw, p_sb, p_sa;
    LONG64 fr = g_frame - p_frame, sw = st_swaps - p_swaps, tb = st_tris_before - p_before, ta = st_tris_after - p_after;
    LONG64 us = st_work_us - p_us, ti = st_tris_in - p_tris;
    LONG64 ssw = st_sh_swaps - p_ssw, sb = st_sh_before - p_sb, sa = st_sh_after - p_sa;
    p_frame = g_frame, p_swaps = st_swaps, p_before = st_tris_before, p_after = st_tris_after, p_us = st_work_us,
    p_tris = st_tris_in, p_ssw = st_sh_swaps, p_sb = st_sh_before, p_sa = st_sh_after;
    if (fr <= 0)
        fr = 1;
    int n_ready = 0, n_none = 0, n_pending = 0, n_unmarked = 0;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < FM_N; i++) {
        LONG s = g_fm[i].state;
        if (s == FM_READY) n_ready++;
        else if (s == FM_NONE) n_none++;
        else if (s >= FM_NEW && s <= FM_WORK) n_pending++;
        if (s != FM_FREE && s != FM_DEAD && g_fm[i].unmarked) n_unmarked++;
    }
    ReleaseSRWLockShared(&g_lock);
    LOGI("  Far meshes (%s): %d meshes with simpler versions, %d pending, %d without (%d also drawn outside known "
         "objects) | this window: %.0f draws/frame swapped, their triangles %.0f k -> %.0f k per frame; of them in "
         "shadow maps (%s): %.0f draws, %.0f k -> %.0f k | background: %.2f M triangles simplified in %.1f s; read "
         "back %.1f MB in all, %lld failed; %lld versions left out (pieces vanished)",
         g_cfg.q_farmesh == 2 ? (g_cfg.q_fm_seams_from > 0 ? "seams far, safe near" : "seams")
                              : g_cfg.q_farmesh ? "safe" : "off",
         n_ready, n_pending, n_none, n_unmarked,
         (double)sw / fr, tb / 1e3 / fr, ta / 1e3 / fr,
         g_cfg.q_fm_shadow > 0 ? (g_cfg.q_fm_shadow < 0.75f ? "by texel 0.5" : "by texel 1") : "by screen",
         (double)ssw / fr, sb / 1e3 / fr, sa / 1e3 / fr, ti / 1e6, us / 1e6, st_read_bytes / 1048576.0,
         (long long)st_failed, (long long)st_area_rejected);
}

/* for the test: is the mesh at this index range simplified, and its version counts */
int farmesh_test_state(void* ib, unsigned off, unsigned start, unsigned count, int base, unsigned* counts)
{
    AcquireSRWLockShared(&g_lock);
    FMesh* e = g_fm ? fm_find((ID3D11Buffer*)ib, off, start, count, base, 0) : NULL;
    int s = e ? (int)e->state : -1;
    if (e && counts)
        for (int m = 0; m < 2; m++)
            for (int k = 0; k < FM_LEVELS; k++)
                counts[m * FM_LEVELS + k] = e->lvl_count[m][k];
    if (e && e->unmarked)
        s = 100 + s;
    ReleaseSRWLockShared(&g_lock);
    return s;
}
