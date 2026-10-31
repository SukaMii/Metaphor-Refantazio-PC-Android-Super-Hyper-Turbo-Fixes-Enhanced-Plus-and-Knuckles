/* GPU probe ([GpuProbe] Enabled=1, diagnostic): hooks D3D11 device contexts to measure
 * the frame's render passes. For every pass (a render-target / depth-target binding)
 * it counts draws and indices; square targets of 512+ texels are treated as shadow-map
 * passes. It checks whether the shadow passes of a frame draw the identical set of
 * objects (= no per-cascade culling) and samples which game functions issue the shadow
 * draws (the code a culling patch has to go into).
 *
 * Every context that calls the hooked methods is tracked separately: the immediate
 * context(s) and deferred contexts (command lists recorded on worker threads). Device
 * creation, deferred-context creation, FinishCommandList and ExecuteCommandList are
 * hooked too. Completed passes from all contexts are merged into one per-frame list. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11_3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef void (STDMETHODCALLTYPE* OMSetRT_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                             ID3D11DepthStencilView*);
typedef void (STDMETHODCALLTYPE* OMSetRTUAV_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                               ID3D11DepthStencilView*, UINT, UINT,
                                               ID3D11UnorderedAccessView* const*, const UINT*);
typedef void (STDMETHODCALLTYPE* DrawIndexed_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE* Draw_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* DrawIndexedInst_t)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE* DrawInst_t)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* IASetIB_t)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
typedef void (STDMETHODCALLTYPE* IASetVB_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                            const UINT*);
typedef HRESULT (STDMETHODCALLTYPE* FinishCL_t)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
typedef void (STDMETHODCALLTYPE* ExecCL_t)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef HRESULT (STDMETHODCALLTYPE* CreateDC_t)(void*, UINT, void**);

#define SLOT(m) (offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void*))
#define SLOT1(m) (offsetof(ID3D11DeviceContext1Vtbl, m) / sizeof(void*))
#define NSLOTS0 (sizeof(ID3D11DeviceContextVtbl) / sizeof(void*))
#define NSLOTS (sizeof(ID3D11DeviceContext1Vtbl) / sizeof(void*)) /* slots >= NSLOTS0 only on ID3D11DeviceContext1 tables */
#define DSLOT(m) (offsetof(ID3D11Device3Vtbl, m) / sizeof(void*))

/* ---------------------------------------------------------------- method tables */
/* Originals are kept per table, and tables are re-checked every frame: the Windows
 * D3D11 runtime rewrites context entries at run time (DXVK does not). */
/* The Windows runtime gives every context object a method table of its own (the game uses 12
 * deferred contexts); DXVK shares one per context kind. */
#define MAX_VT 64
typedef struct HookedVt {
    void** vt;
    int writable; /* per-object tables of the Windows runtime live in writable heap memory */
    int dc1;      /* the table belongs to an ID3D11DeviceContext1 (has the 11.1 slots) */
    int deferred; /* the table of a deferred context */
    void* orig[NSLOTS];
} HookedVt;
static HookedVt g_vts[MAX_VT];
static volatile LONG g_nvt;
static SRWLOCK g_vt_lock = SRWLOCK_INIT;
static int g_active;
static uint8_t *g_text, *g_text_end;

static HookedVt* vt_of(void* self)
{
    void** vt = *(void***)self;
    LONG n = g_nvt;
    for (LONG i = 0; i < n; i++)
        if (g_vts[i].vt == vt)
            return &g_vts[i];
    return &g_vts[0];
}

static void* orig_of(void* self, size_t slot)
{
    return vt_of(self)->orig[slot];
}
#define O(type, self, slot) ((type)orig_of((self), (slot)))

static int repatch(HookedVt* h);
/* After a hooked call ran: the Windows runtime may have swapped entries meanwhile (its
 * per-object tables are writable). DXVK's tables are static: nothing to re-check per call. */
#define KEEP_HOOKED(self)                                                                                     \
    do {                                                                                                      \
        HookedVt* h_ = vt_of(self);                                                                           \
        if (h_->writable)                                                                                     \
            repatch(h_);                                                                                      \
    } while (0)

/* device tables: CreateDeferredContext, ...1, ...2, ...3 */
static void** g_dev_vt;
static void* g_dev_orig[4];
static const size_t g_dev_slots[4] = {DSLOT(CreateDeferredContext), DSLOT(CreateDeferredContext1),
                                      DSLOT(CreateDeferredContext2), DSLOT(CreateDeferredContext3)};

/* ---------------------------------------------------------------- passes */
typedef struct Pass {
    int w, h, shadow, deferred;
    LONG draws;
    LONG64 indices;
    uint64_t sum, xr; /* order-independent signature of the draw set */
} Pass;

#define MAX_CTX 64
typedef struct Ctx {
    void* ctx;
    int deferred;
    int has_pass;
    Pass cur;
    void *ib, *vb;
    uint64_t key;  /* GPU timing: label of the current pass */
    int last_kind; /* GPU timing: kind of the last timestamp written on this context */
    int last_cs;   /* [GpuProbe] ComputeCensus: the compute shader of the last compute timestamp */
    int work_marked; /* GPU timing: the current pass's first draw/clear has its timestamp */
    void* bound[9];  /* GPU timing: render targets + depth of the last bind (re-binds of the same set are not marked) */
    LONG pass_draws; /* GPU timing: draws of the current pass so far */
    int nrt;         /* render targets of the current pass (0 = depth only) */
    int ps_id;       /* GPU timing: the bound pixel shader (effects.c's id, 0 none) */
    int seg_ps;      /* ShaderTiming: the pixel shader of the running timing segment, -1 none */
    int ps_pending;  /* the pass's work mark came from a clear: it is named after its first draw's shader */
    LONG64 work_slot; /* that mark's timestamp slot, -1 none */
    /* render pass audit ([GpuProbe] Passes) */
    void* pa_att[9];          /* render targets 0..7, depth 8 */
    int pa_bytes[9];
    int pa_w, pa_h;
    char pa_clr[9], pa_disc[9]; /* cleared / discarded before the pass's first draw */
    int pa_open, pa_segs, pa_draws, pa_seg_draws, pa_full, pa_depth_used, pa_loaded_depth;
    uint64_t pa_key;
    double pa_load, pa_store, pa_avoid;
    int pa_blend, pa_ds_on, pa_vp_set;
    float pa_vp[4];
    void* pa_pend_clr[16];  /* clears of views not bound: DXVK defers them to the next pass binding them */
    void* pa_pend_disc[16];
    int pa_nclr, pa_ndisc;
    /* [Quality] far-object textures: the game's pixel-shader samplers and the bias each was bound with */
    void* q_orig[16];
    int q_sb[16]; /* sampler key each slot was bound with (bias + 32 * anisotropy clamp) */
    int q_n;   /* slots up to the last bound one */
    int q_all; /* the key every bound slot carries, -1 = mixed */
} Ctx;

/* ---------------------------------------------------------------- GPU time per pass */
/* [GpuProbe] Timing=1: a D3D11 timestamp is written at every render-target bind (pass
 * start), at the first Dispatch after it (compute), at FinishCommandList (end of a
 * recorded list) and at Present (end of frame). Read back a few frames later, sorted by
 * GPU time; the gap to the next timestamp is the GPU time of that pass. Gaps after a
 * list or frame end are idle time. */
/* K_BIND: render targets bound; the time until the pass's first draw/clear (K_PASS) runs is idle */
enum { K_PASS = 0, K_COMPUTE = 1, K_ENDLIST = 2, K_ENDFRAME = 3, K_BIND = 4 };
/* bits: width 0-13, height 14-27, format 28-35, targets 36-39, depth 40, cascade 41-43, first pixel shader 44-59,
 * kind 60-63 */
#define KEY(kind, w, h, fmt, nrt, dsv)                                                                       \
    ((uint64_t)(kind) << 60 | (uint64_t)((w) & 0x3FFF) | (uint64_t)((h) & 0x3FFF) << 14 |                   \
     (uint64_t)((fmt) & 0xFF) << 28 | (uint64_t)((nrt) & 0xF) << 36 | (uint64_t)((dsv) ? 1 : 0) << 40)
#define KIND(k) ((int)((k) >> 60))
#define AS_KIND(k, kind) (((k) & ~(0xFull << 60)) | (uint64_t)(kind) << 60)
#define CASCADE_OF(k) ((int)((k) >> 41 & 7) - 1) /* shadow cascade of a depth pass, -1 = none */
#define KEY_PS(k) ((int)((k) >> 44 & 0xFFFF))    /* the pass's first pixel shader (effects.c's id) */
#define WITH_PS(k, id) (((k) & ~(0xFFFFull << 44)) | (uint64_t)((id) & 0xFFFF) << 44)
#define PS_CONT 0x10000 /* a timestamp's shader: a shader change inside the pass (ShaderTiming), not a new pass */

/* draws per pass kind (recorded on the CPU when a pass ends) */
#define DK 512 /* pass kinds, told apart by their first pixel shader too */
static uint64_t g_dk_key[DK];
static LONG64 g_dk_draws[DK], g_dk_n[DK];
static SRWLOCK g_dk_lock = SRWLOCK_INIT;
#define TRING 8192
typedef struct TSlot {
    ID3D11Query* q;
    uint64_t key;
    LONG64 frame;
    int ps; /* the pixel shader of the segment this mark starts (ShaderTiming), else the pass's first */
    volatile LONG state; /* 0 free / claimed, 2 written, 3 dropped */
} TSlot;
static TSlot g_ts[TRING];
static volatile LONG64 g_thead, g_ttail;
static ID3D11Device* g_tdev;
static ID3D11DeviceContext* g_timm;
static ID3D11Query* g_disjoint;
static int g_disjoint_state; /* 0 idle, 1 begun, 2 ended (waiting for the result) */
static LONG64 g_disjoint_frame;
static volatile UINT64 g_ts_freq;
static volatile LONG64 t_ts_dropped, t_ts_disjoint;
static volatile LONG64 g_tframes; /* frames seen by the timing code (the probe's counter may be off) */
typedef struct TsEntry {
    UINT64 t;
    uint64_t key;
    uint32_t seq; /* recording order: breaks ties between equal timestamps */
    int ps;
} TsEntry;
#define TS_WIN 262144 /* a 10 s window with ShaderTiming's marks */
static TsEntry g_tsw[TS_WIN]; /* resolved timestamps of the current profiler window */
static int g_ntsw;
static SRWLOCK g_tsw_lock = SRWLOCK_INIT;
static Ctx g_ctx[MAX_CTX];
static volatile LONG g_nctx;
static SRWLOCK g_ctx_lock = SRWLOCK_INIT;

#define MAX_FRAME_PASSES 512
static Pass g_frame[MAX_FRAME_PASSES];
static int g_nframe;
static SRWLOCK g_frame_lock = SRWLOCK_INIT;

/* ---------------------------------------------------------------- totals for the profiler */
static volatile LONG64 t_frames, t_draws, t_deferred_draws, t_shadow_draws, t_shadow_passes, t_shadow_indices;
static volatile LONG64 t_cmp, t_same, t_finish, t_execute, t_contexts_seen;
static volatile LONG64 t_cascade_draws[4], t_cascade_idx[4];

/* ---------------------------------------------------------------- shadow-cull verification */
/* The shadow thread's deferred context (last render-target bind on this thread) and a ring
 * of occlusion queries: begun/ended there, read back on the immediate context at Present. */
static DWORD g_tls_ctx = TLS_OUT_OF_INDEXES;
#define QRING 8192
typedef struct QSlot {
    ID3D11Query* q;
    LONG64 frame;
} QSlot;
static QSlot g_qs[QRING];
static volatile LONG64 g_qhead, g_qtail;
static ID3D11Device* g_qdev;
static ID3D11DeviceContext* g_qimm;
static SRWLOCK g_snap_lock = SRWLOCK_INIT;
static char g_snap[600];

#define CHAINS 64
typedef struct Chain {
    void* f[3];
    LONG64 n;
} Chain;
static Chain g_chain[CHAINS];
static SRWLOCK g_chain_lock = SRWLOCK_INIT;
static SiteTable g_callers; /* direct caller of shadow-pass draw calls */

/* view -> target size cache (views are long-lived objects) */
#define VCACHE 1024
typedef struct VInfo {
    void* view;
    int w, h, fmt;
} VInfo;
static VInfo g_vcache[VCACHE];
static SRWLOCK g_vcache_lock = SRWLOCK_INIT;

static void view_info(void* view, int is_dsv, int* w, int* h, int* fmt)
{
    unsigned k = (unsigned)(((uintptr_t)view >> 4) * 2654435761u) & (VCACHE - 1);
    AcquireSRWLockShared(&g_vcache_lock);
    for (int i = 0; i < 8; i++) {
        VInfo* v = &g_vcache[(k + i) & (VCACHE - 1)];
        if (v->view == view) {
            *w = v->w, *h = v->h;
            if (fmt)
                *fmt = v->fmt;
            ReleaseSRWLockShared(&g_vcache_lock);
            return;
        }
    }
    ReleaseSRWLockShared(&g_vcache_lock);
    ID3D11Resource* res = NULL;
    int f = 0;
    UINT mip = 0;
    if (is_dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        ID3D11DepthStencilView_GetDesc((ID3D11DepthStencilView*)view, &dd);
        f = (int)dd.Format;
        if (dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
            mip = dd.Texture2D.MipSlice;
        else if (dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
            mip = dd.Texture2DArray.MipSlice;
        ID3D11DepthStencilView_GetResource((ID3D11DepthStencilView*)view, &res);
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        ID3D11RenderTargetView_GetDesc((ID3D11RenderTargetView*)view, &rd);
        f = (int)rd.Format;
        if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
            mip = rd.Texture2D.MipSlice;
        else if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
            mip = rd.Texture2DArray.MipSlice;
        ID3D11RenderTargetView_GetResource((ID3D11RenderTargetView*)view, &res);
    }
    if (fmt)
        *fmt = f;
    *w = *h = 0;
    if (res) {
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(res, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res, &td);
            *w = (int)(td.Width >> mip ? td.Width >> mip : 1);
            *h = (int)(td.Height >> mip ? td.Height >> mip : 1);
        }
        ID3D11Resource_Release(res);
    }
    AcquireSRWLockExclusive(&g_vcache_lock);
    VInfo* v = &g_vcache[k];
    v->view = view, v->w = *w, v->h = *h, v->fmt = f;
    ReleaseSRWLockExclusive(&g_vcache_lock);
}

/* Context state; the first call from a context registers it (and asks its type). */
static Ctx* ctx_of(ID3D11DeviceContext* c)
{
    LONG n = g_nctx;
    for (LONG i = 0; i < n; i++)
        if (g_ctx[i].ctx == c)
            return &g_ctx[i];
    AcquireSRWLockExclusive(&g_ctx_lock);
    Ctx* r = NULL;
    for (LONG i = 0; i < g_nctx && !r; i++)
        if (g_ctx[i].ctx == c)
            r = &g_ctx[i];
    if (!r && g_nctx < MAX_CTX) {
        r = &g_ctx[g_nctx];
        memset(r, 0, sizeof(*r));
        r->ctx = c;
        r->pa_ds_on = 1; /* D3D11 default state: depth test on */
        r->deferred = ID3D11DeviceContext_GetType(c) == D3D11_DEVICE_CONTEXT_DEFERRED;
        InterlockedIncrement(&g_nctx);
        InterlockedIncrement64(&t_contexts_seen);
    }
    ReleaseSRWLockExclusive(&g_ctx_lock);
    return r ? r : &g_ctx[MAX_CTX - 1];
}

static void flush_pass(Ctx* x)
{
    if (!x->has_pass || x->cur.draws == 0)
        return;
    AcquireSRWLockExclusive(&g_frame_lock);
    if (g_nframe < MAX_FRAME_PASSES)
        g_frame[g_nframe++] = x->cur;
    ReleaseSRWLockExclusive(&g_frame_lock);
    x->has_pass = 0;
}

static void begin_pass(Ctx* x, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    flush_pass(x);
    int w = 0, h = 0, fmt = 0, nrt = 0;
    for (UINT i = 0; rtvs && i < n && i < 8; i++)
        nrt += rtvs[i] != NULL;
    ID3D11RenderTargetView* rtv0 = NULL;
    for (UINT i = 0; rtvs && i < n && i < 8 && !rtv0; i++)
        rtv0 = rtvs[i];
    if (rtv0)
        view_info(rtv0, 0, &w, &h, &fmt);
    if (dsv) {
        int dfmt = 0;
        view_info(dsv, 1, &w, &h, &dfmt);
        if (!rtv0)
            fmt = dfmt;
    }
    memset(&x->cur, 0, sizeof(x->cur));
    x->cur.w = w, x->cur.h = h;
    x->cur.shadow = (w == h && w >= 512);
    x->cur.deferred = x->deferred;
    x->has_pass = 1;
    x->nrt = nrt;
    x->key = KEY(K_PASS, w, h, fmt, nrt, dsv != NULL);
    x->seg_ps = -1, x->ps_pending = 0, x->work_slot = -1;
    if (g_cfg.gpu_timing && !nrt && w == h && w >= 512) /* shadow map: which cascade (shadowcull.c knows) */
        x->key |= (uint64_t)((shadowcull_current_cascade() + 1) & 7) << 41;
}

/* GPU timing: the current pass ends; remember its draw count for its kind */
static void pass_done(Ctx* x)
{
    if (x->pass_draws <= 0)
        return;
    uint64_t k = AS_KIND(x->key, K_PASS);
    AcquireSRWLockExclusive(&g_dk_lock);
    for (int i = 0; i < DK; i++)
        if (g_dk_key[i] == k || !g_dk_n[i]) {
            g_dk_key[i] = k, g_dk_draws[i] += x->pass_draws, g_dk_n[i]++;
            break;
        }
    ReleaseSRWLockExclusive(&g_dk_lock);
    x->pass_draws = 0;
}

/* ---------------------------------------------------------------- GPU timing: writing */
/* returns the timestamp's slot (-1 if none was free) */
static LONG64 ts_mark_ps(ID3D11DeviceContext* c, Ctx* x, uint64_t key, int ps)
{
    if (!g_tdev) {
        ID3D11DeviceContext_GetDevice(c, &g_tdev);
        if (!g_tdev)
            return -1;
    }
    LONG64 h;
    do { /* claim a slot without lapping the reader */
        h = g_thead;
        if (h - g_ttail >= TRING) {
            InterlockedIncrement64(&t_ts_dropped);
            return -1;
        }
    } while (InterlockedCompareExchange64(&g_thead, h + 1, h) != h);
    TSlot* s = &g_ts[h % TRING];
    if (!s->q) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(ID3D11Device_CreateQuery(g_tdev, &d, &s->q))) {
            s->q = NULL;
            InterlockedExchange(&s->state, 3);
            return -1;
        }
    }
    s->key = key;
    s->ps = ps;
    s->frame = g_tframes;
    ID3D11DeviceContext_End(c, (ID3D11Asynchronous*)s->q);
    if (x)
        x->last_kind = KIND(key);
    MemoryBarrier();
    InterlockedExchange(&s->state, 2);
    return h;
}

static LONG64 ts_mark(ID3D11DeviceContext* c, Ctx* x, uint64_t key)
{
    return ts_mark_ps(c, x, key, KEY_PS(key));
}

static void record_chain(void* direct)
{
    void* frames[12];
    USHORT n = RtlCaptureStackBackTrace(1, 12, frames, NULL);
    void* f[3] = {0};
    int k = 0;
    for (USHORT i = 0; i < n && k < 3; i++)
        if ((uint8_t*)frames[i] >= g_text && (uint8_t*)frames[i] < g_text_end)
            f[k++] = frames[i];
    if (!k && direct)
        f[0] = direct;
    unsigned hsh = (unsigned)((((uintptr_t)f[0] >> 2) ^ ((uintptr_t)f[1] >> 1) ^ (uintptr_t)f[2]) * 2654435761u) %
                   CHAINS;
    AcquireSRWLockExclusive(&g_chain_lock);
    for (int i = 0; i < CHAINS; i++) {
        Chain* c = &g_chain[(hsh + i) % CHAINS];
        if (!c->n || (c->f[0] == f[0] && c->f[1] == f[1] && c->f[2] == f[2])) {
            c->f[0] = f[0], c->f[1] = f[1], c->f[2] = f[2];
            c->n++;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_chain_lock);
}

/* ShaderTiming: draws and vertices per pass kind and pixel shader (statistics: one lock per draw, PC) */
#define SD_N 4096
typedef struct SdEntry {
    uint64_t key;
    int ps;
    LONG64 draws, verts;
} SdEntry;
static SdEntry g_sd[SD_N];
static SRWLOCK g_sd_lock = SRWLOCK_INIT;

static SdEntry* sd_find(uint64_t key, int ps, int create)
{
    unsigned h = (unsigned)(((key ^ (uint64_t)ps * 0x9E3779B97F4A7C15ull) * 0xC2B2AE3D27D4EB4Full) >> 40) & (SD_N - 1);
    for (int i = 0; i < SD_N; i++, h = (h + 1) & (SD_N - 1)) {
        SdEntry* e = &g_sd[h];
        if (e->draws && e->key == key && e->ps == ps)
            return e;
        if (!e->draws)
            return create ? (e->key = key, e->ps = ps, e) : NULL;
    }
    return NULL;
}

static void count_draw(ID3D11DeviceContext* c, UINT count, UINT start, INT base, void* caller)
{
    Ctx* x = ctx_of(c);
    if (!x->has_pass)
        begin_pass(x, 0, NULL, NULL);
    if (g_cfg.gpu_timing) {
        if (!x->work_marked) { /* the pass's GPU work starts here; it is named after this draw's pixel shader */
            x->key = WITH_PS(x->key, x->ps_id);
            ts_mark_ps(c, x, x->key, x->ps_id);
            x->work_marked = 1, x->seg_ps = x->ps_id;
        } else if (x->ps_pending) { /* the work mark came from a clear: named now (not run yet, not read yet) */
            x->key = WITH_PS(x->key, x->ps_id);
            LONG64 h = x->work_slot;
            if (h >= 0 && h >= g_ttail) {
                TSlot* s = &g_ts[h % TRING];
                s->key = WITH_PS(s->key, x->ps_id), s->ps = x->ps_id;
            }
            x->seg_ps = x->ps_id;
        } else if (g_cfg.shader_timing && x->ps_id != x->seg_ps) { /* PC: a timing segment per pixel shader */
            ts_mark_ps(c, x, x->key, x->ps_id | PS_CONT);
            x->seg_ps = x->ps_id;
        }
        x->ps_pending = 0;
        x->pass_draws++;
        if (g_cfg.shader_timing) {
            AcquireSRWLockExclusive(&g_sd_lock);
            SdEntry* e = sd_find(x->key, x->ps_id, 1);
            if (e)
                e->draws++, e->verts += count;
            ReleaseSRWLockExclusive(&g_sd_lock);
        }
    }
    if (!g_cfg.gpu_probe)
        return; /* timing only: no per-draw accounting */
    Pass* p = &x->cur;
    p->draws++;
    p->indices += count;
    uint64_t v = (uint64_t)(uintptr_t)x->ib * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)x->vb * 0xC2B2AE3D27D4EB4Full ^
                 ((uint64_t)start << 32 | count) * 0x165667B19E3779F9ull ^ (uint64_t)(int64_t)base;
    p->sum += v;
    p->xr ^= v;
    if (p->shadow) {
        site_add(&g_callers, caller, 1);
        if ((p->draws & 31) == 1)
            record_chain(caller);
    }
}

/* ---------------------------------------------------------------- render pass audit */
/* [GpuProbe] Passes=1: how DXVK turns the frame into Vulkan render passes, and what they
 * cost a tiled GPU (Adreno keeps the targets in tile memory: at the start of a render pass
 * every attachment is loaded from RAM unless it was cleared or discarded, at its end every
 * attachment is stored back). Modelled from the game's D3D11 calls, which are the same on PC:
 *   - a render pass starts at the first draw after the targets are bound;
 *   - clearing a bound target before that draw becomes its load op (no load); DXVK also
 *     defers clears of views that are not bound to the next render pass binding them;
 *   - copies, texture and partial buffer updates, compute dispatches, UAV clears, mip
 *     generation and resolves end the running render pass; the next draw starts another
 *     one that loads every attachment again (a "split");
 *   - every attachment is stored at the end of every render pass.
 * Loads that look avoidable are flagged: the pass's first draw is a full-screen triangle or
 * quad without blending (it overwrites the target anyway), or a depth target stays bound
 * while no draw of the pass uses depth or stencil. */
enum { SP_COPY, SP_UPDATE, SP_DISPATCH, SP_CLEARUAV, SP_OTHER, SP_N };
static const char* const sp_name[SP_N] = {"copies", "texture/partial buffer updates", "compute dispatches",
                                          "UAV clears", "mips/resolves"};

typedef struct PaKind {
    uint64_t key;
    LONG64 n, segs, draws, full, depth_unused;
    double load, store, avoid, depth_waste;
} PaKind;
#define PAK 96
static PaKind g_pak[PAK];
static SRWLOCK g_pa_lock = SRWLOCK_INIT;
static LONG64 pa_passes, pa_segs, pa_split[SP_N], pa_att_clear, pa_att_disc, pa_att_load, pa_att_load_split,
    pa_inpass_clears, pa_full, pa_depth_unused;
static double pa_load, pa_load_split, pa_store, pa_avoid, pa_depth_waste;
static char pa_snap[1400];
static int pa_snap_n, pa_snap_on;

static int fmt_bytes(int f)
{
    if (f >= 1 && f <= 4) return 16;
    if (f >= 5 && f <= 8) return 12;
    if (f >= 9 && f <= 22) return 8;
    if (f >= 23 && f <= 47) return 4;
    if (f >= 48 && f <= 59) return 2;
    if (f >= 60 && f <= 65) return 1;
    if (f >= 87 && f <= 93) return 4;
    return 4;
}

static int pending_take(void** list, int* n, void* v)
{
    for (int i = 0; i < *n; i++)
        if (list[i] == v) {
            list[i] = list[--*n];
            return 1;
        }
    return 0;
}

static void pending_add(void** list, int* n, void* v)
{
    for (int i = 0; i < *n; i++)
        if (list[i] == v)
            return;
    if (*n < 16)
        list[(*n)++] = v;
}

static void pa_seg_close(Ctx* x, int cause)
{
    if (!x->pa_open)
        return;
    for (int i = 0; i < 9; i++)
        if (x->pa_att[i])
            x->pa_store += x->pa_bytes[i];
    x->pa_open = 0;
    if (cause >= 0) {
        AcquireSRWLockExclusive(&g_pa_lock);
        pa_split[cause]++;
        ReleaseSRWLockExclusive(&g_pa_lock);
    }
}

/* end of a binding: account it */
static void pa_close(Ctx* x)
{
    pa_seg_close(x, -1);
    if (x->pa_draws > 0) {
        int depth_unused = x->pa_att[8] && !x->pa_depth_used;
        double dw = depth_unused ? x->pa_bytes[8] * (double)(x->pa_segs + x->pa_loaded_depth) : 0;
        AcquireSRWLockExclusive(&g_pa_lock);
        pa_passes++, pa_segs += x->pa_segs;
        pa_store += x->pa_store, pa_avoid += x->pa_avoid, pa_depth_waste += dw;
        pa_full += x->pa_full, pa_depth_unused += depth_unused;
        int k = 0;
        while (k < PAK && g_pak[k].n && g_pak[k].key != x->pa_key)
            k++;
        if (k < PAK) {
            PaKind* p = &g_pak[k];
            p->key = x->pa_key, p->n++, p->segs += x->pa_segs, p->draws += x->pa_draws, p->full += x->pa_full;
            p->depth_unused += depth_unused;
            p->load += x->pa_load, p->store += x->pa_store, p->avoid += x->pa_avoid, p->depth_waste += dw;
        }
        if (pa_snap_on && pa_snap_n < (int)sizeof(pa_snap) - 48) {
            int nrt = 0;
            for (int i = 0; i < 8; i++)
                nrt += x->pa_att[i] != NULL;
            char ld[10];
            int m = 0;
            for (int i = 0; i < 9; i++)
                if (x->pa_att[i])
                    ld[m++] = x->pa_clr[i] ? 'C' : x->pa_disc[i] ? 'D' : (i < 8 && x->pa_full) ? 'F' : 'L';
            /* F: loaded, but the first draw overwrites it all */
            ld[m] = 0;
            pa_snap_n += snprintf(pa_snap + pa_snap_n, sizeof(pa_snap) - pa_snap_n, " %dx%d%s%s:%s%s%s%d", x->pa_w,
                                  x->pa_h, nrt ? "" : "S", x->pa_att[8] ? "+D" : "", ld, depth_unused ? "u" : "",
                                  x->pa_segs > 1 ? "/" : ":", x->pa_segs > 1 ? x->pa_segs : x->pa_draws);
        }
        ReleaseSRWLockExclusive(&g_pa_lock);
    }
    memset(x->pa_att, 0, sizeof(x->pa_att));
    x->pa_segs = x->pa_draws = x->pa_seg_draws = x->pa_full = x->pa_depth_used = x->pa_loaded_depth = 0;
    x->pa_load = x->pa_store = x->pa_avoid = 0;
}

static void pa_bind(Ctx* x, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    void* b[9] = {0};
    for (UINT i = 0; rtvs && i < n && i < 8; i++)
        b[i] = rtvs[i];
    b[8] = dsv;
    if (x->pa_draws && memcmp(b, x->pa_att, sizeof(b)) == 0)
        return; /* the same targets again: DXVK keeps the running render pass */
    pa_close(x);
    x->pa_key = AS_KIND(x->key, K_PASS);
    x->pa_w = x->pa_h = 0;
    for (int i = 0; i < 9; i++) {
        void* v = i < 8 ? (rtvs && (UINT)i < n ? (void*)rtvs[i] : NULL) : (void*)dsv;
        x->pa_att[i] = v;
        x->pa_clr[i] = x->pa_disc[i] = 0;
        x->pa_bytes[i] = 0;
        if (!v)
            continue;
        int w = 0, h = 0, f = 0;
        view_info(v, i == 8, &w, &h, &f);
        x->pa_bytes[i] = w * h * fmt_bytes(f);
        if (!x->pa_w)
            x->pa_w = w, x->pa_h = h;
        /* clears / discards issued while the view was not bound */
        x->pa_clr[i] = (char)pending_take(x->pa_pend_clr, &x->pa_nclr, v);
        if (!x->pa_clr[i])
            x->pa_disc[i] = (char)pending_take(x->pa_pend_disc, &x->pa_ndisc, v);
    }
}

static void pa_clear(Ctx* x, void* v, int discard)
{
    for (int i = 0; i < 9; i++)
        if (x->pa_att[i] == v) {
            if (x->pa_open || x->pa_segs) { /* inside a render pass (or after one): an in-pass clear */
                AcquireSRWLockExclusive(&g_pa_lock);
                pa_inpass_clears++;
                ReleaseSRWLockExclusive(&g_pa_lock);
            } else if (discard) {
                x->pa_disc[i] = 1;
            } else {
                x->pa_clr[i] = 1, x->pa_disc[i] = 0;
            }
            return;
        }
    if (discard)
        pending_add(x->pa_pend_disc, &x->pa_ndisc, v);
    else
        pending_add(x->pa_pend_clr, &x->pa_nclr, v);
}

static void pa_draw(Ctx* x, UINT verts, UINT inst, int indexed)
{
    if (!x->pa_att[0] && !x->pa_att[8]) {
        int any = 0;
        for (int i = 1; i < 8; i++)
            any |= x->pa_att[i] != NULL;
        if (!any)
            return; /* nothing bound (stream-out / UAV-only draws) */
    }
    if (!x->pa_open) {
        x->pa_open = 1;
        x->pa_segs++;
        x->pa_seg_draws = 0;
        LONG64 c = 0, d = 0, l = 0, ls = 0;
        double lb = 0, lsb = 0;
        int first = x->pa_segs == 1;
        int full = 0;
        if (first) { /* the pass's first draw: does it overwrite the whole target? */
            full = !indexed && inst <= 1 && (verts == 3 || verts == 4 || verts == 6) && !x->pa_blend &&
                   x->pa_vp_set && x->pa_vp[0] <= 0 && x->pa_vp[1] <= 0 && x->pa_vp[2] >= (float)x->pa_w &&
                   x->pa_vp[3] >= (float)x->pa_h && !(x->pa_att[8] && x->pa_ds_on);
            x->pa_full = 0; /* set below if a target it overwrites was loaded */
        }
        for (int i = 0; i < 9; i++) {
            if (!x->pa_att[i])
                continue;
            if (!first) {
                ls++, lsb += x->pa_bytes[i];
            } else if (x->pa_clr[i]) {
                c++;
            } else if (x->pa_disc[i]) {
                d++;
            } else {
                l++, lb += x->pa_bytes[i];
                if (i < 8 && full)
                    x->pa_avoid += x->pa_bytes[i], x->pa_full = 1;
                if (i == 8)
                    x->pa_loaded_depth = 1;
            }
        }
        x->pa_load += lb + lsb;
        AcquireSRWLockExclusive(&g_pa_lock);
        pa_att_clear += c, pa_att_disc += d, pa_att_load += l, pa_att_load_split += ls;
        pa_load += lb, pa_load_split += lsb;
        ReleaseSRWLockExclusive(&g_pa_lock);
    }
    x->pa_draws++, x->pa_seg_draws++;
    if (x->pa_att[8] && x->pa_ds_on)
        x->pa_depth_used = 1;
}

static void pa_spill(ID3D11DeviceContext* c, int cause)
{
    if (g_cfg.gpu_passes)
        pa_seg_close(ctx_of(c), cause);
}

/* state a command list starts with / leaves behind (ExecuteCommandList and FinishCommandList
 * reset the context to defaults) */
static void pa_reset_state(Ctx* x)
{
    pa_close(x);
    x->pa_blend = 0, x->pa_ds_on = 1, x->pa_vp_set = 0;
}

/* ---------------------------------------------------------------- [Quality] textures */
/* TextureMipBias makes the game's samplers pick smaller mip levels: each sampler bound to a
 * pixel shader is swapped for a twin with the bias added. (Characters are recognised in the
 * engine, shadowcull.c: the game's input layouts carry no bone attributes.) */
static volatile LONG64 q_samp_swaps, q_twins_aniso, q_twins_lin, q_twins_other, q_twins_skip;

static unsigned ql_hash(const void* p)
{
    return (unsigned)(((uintptr_t)p >> 4) * 2654435761u);
}

#define ST_N 1024
/* a sampler key: texture LOD bias in quarter mip levels in the low 6 bits, the anisotropy clamp (0 = none) above */
#define QK(bias, aniso) ((bias) + 64 * (aniso))
typedef struct STwin {
    void* orig;
    void* twin; /* NULL: nothing to change for this sampler: use the original */
    int key;
} STwin;
static STwin g_st[ST_N];
static SRWLOCK g_st_lock = SRWLOCK_INIT;

static ID3D11SamplerState* q_twin(ID3D11SamplerState* s, int key)
{
    unsigned h = ql_hash(s) & (ST_N - 1);
    AcquireSRWLockShared(&g_st_lock);
    for (unsigned i = 0, k = h; i < ST_N && g_st[k].orig; i++, k = (k + 1) & (ST_N - 1))
        if (g_st[k].orig == s && g_st[k].key == key) {
            void* t = g_st[k].twin;
            ReleaseSRWLockShared(&g_st_lock);
            return t ? (ID3D11SamplerState*)t : s;
        }
    ReleaseSRWLockShared(&g_st_lock);
    D3D11_SAMPLER_DESC d;
    ID3D11SamplerState_GetDesc(s, &d);
    ID3D11SamplerState* tw = NULL;
    int bias = key & 63, aniso = key / 64, change = 0;
    if ((d.Filter & 0x180) == 0 && d.MaxLOD > 0) /* which filters the game uses (log) */
        InterlockedIncrement64(d.Filter == D3D11_FILTER_ANISOTROPIC ? &q_twins_aniso
                               : d.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR ? &q_twins_lin : &q_twins_other);
    else
        InterlockedIncrement64(&q_twins_skip);
    if (bias > 0 && (d.Filter & 0x180) == 0 && d.MaxLOD > 0) { /* plain filtering (no comparison / min-max) with mips */
        d.MipLODBias += 0.25f * (float)bias;
        if (d.MipLODBias > 15.99f)
            d.MipLODBias = 15.99f;
        change = 1;
    }
    if (aniso > 0 && (d.Filter & 0x55) == 0x55 && d.MaxAnisotropy > (UINT)aniso) { /* the anisotropic filters */
        d.MaxAnisotropy = (UINT)aniso;
        change = 1;
    }
    if (change) {
        ID3D11Device* dev = NULL;
        ID3D11DeviceChild_GetDevice((ID3D11DeviceChild*)s, &dev);
        if (dev) {
            if (FAILED(ID3D11Device_CreateSamplerState(dev, &d, &tw)))
                tw = NULL;
            ID3D11Device_Release(dev);
        }
    }
    int kept = 0;
    AcquireSRWLockExclusive(&g_st_lock);
    for (unsigned i = 0, k = h; i < ST_N; i++, k = (k + 1) & (ST_N - 1)) {
        if (g_st[k].orig == s && g_st[k].key == key) { /* another thread made it meanwhile */
            if (tw)
                ID3D11SamplerState_Release(tw);
            tw = (ID3D11SamplerState*)g_st[k].twin;
            kept = 1;
            break;
        }
        if (!g_st[k].orig) {
            g_st[k].orig = s, g_st[k].twin = tw, g_st[k].key = key;
            kept = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_st_lock);
    if (!kept && tw) { /* table full: leave this sampler as it is rather than leak twins */
        ID3D11SamplerState_Release(tw);
        tw = NULL;
    }
    return tw ? tw : s;
}

/* Report: what the game binds - anisotropy level of each sampler, looked up once */
#define SS_N 1024
static void* volatile g_ss_ptr[SS_N];
static volatile LONG g_ss_cls[SS_N]; /* MaxAnisotropy for the anisotropic filters, 0 other, -1 comparison */
static volatile LONG64 q_binds_aniso[5], q_binds_other, q_binds_cmp; /* aniso 1-2, 3-4, 5-8, 9-15, 16 */

static int sampler_class(ID3D11SamplerState* s)
{
    unsigned k = ql_hash(s) & (SS_N - 1);
    for (unsigned i = 0; i < 32; i++, k = (k + 1) & (SS_N - 1)) {
        void* cur = g_ss_ptr[k];
        if (cur == s)
            return (int)g_ss_cls[k];
        if (!cur) {
            D3D11_SAMPLER_DESC d;
            ID3D11SamplerState_GetDesc(s, &d);
            int cls = (d.Filter & 0x180) == 0x80 ? -1 : ((d.Filter & 0x55) == 0x55 ? (int)d.MaxAnisotropy : 0);
            g_ss_cls[k] = cls;
            if (InterlockedCompareExchangePointer((PVOID volatile*)&g_ss_ptr[k], s, NULL) != NULL)
                continue; /* lost the slot to another thread: try the next one */
            return cls;
        }
    }
    return 0;
}

static void count_binds(UINT n, ID3D11SamplerState* const* s)
{
    for (UINT i = 0; i < n; i++) {
        if (!s[i])
            continue;
        int cls = sampler_class(s[i]);
        if (cls < 0)
            q_binds_cmp++;
        else if (cls == 0)
            q_binds_other++;
        else
            q_binds_aniso[cls <= 2 ? 0 : cls <= 4 ? 1 : cls <= 8 ? 2 : cls < 16 ? 3 : 4]++;
    }
}

typedef void (STDMETHODCALLTYPE* PSSetSamp_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);

/* FarTextureMipBias: the scene hook (shadowcull.c) gives the draws of objects beyond FarDistance
 * an extra bias on the thread that records them. The engine may leave samplers bound from one
 * object to the next, so each draw re-binds the context's samplers when they carry a different
 * bias than the one wanted (the game's own samplers are remembered per context; command lists
 * and ClearState reset them, as D3D11 does). */
static DWORD g_tls_q = TLS_OUT_OF_INDEXES;
static int g_q_far_hooks; /* the draw hooks are installed (far textures, accounting, run-time switches) */
static volatile LONG64 q_far_draws, q_rebinds;
/* triangles per frame by class (world / characters per distance band), how many of them sit in
 * objects with under 4 pixels per triangle, and those drawn outside scene objects */
static volatile LONG64 q_geo_tris[GEO_CLASSES], q_geo_micro[GEO_CLASSES], q_geo_shadow, q_geo_other;

typedef struct QThread {
    int far_bias, cls;
    float area; /* on-screen pixels of the object at 720p */
    float dist; /* metres from the camera */
    int fm_kind; /* far meshes: 1 a world object of the scene, 2 a static shadow caster, 3 a character, 0 not known */
    float fm_near, fm_scale; /* its nearest distance (cm) and world scale */
    float fm_tpu;            /* a caster: shadow-map texels per world unit */
    float node_dist;         /* [Quality] OutlineDistance: the scene object's distance from the camera (m), -1 none */
    int node_char;           /* it is a character; else node_carried says if a character carries it (-1 not checked) */
    int node_carried;
    float node_c[3];         /* its centre, and the scene frame (for the carried check) */
    LONG64 node_seq;
    /* occlusion census: the scene object being drawn, its query once its first draw comes */
    int oc_on, oc_slot, oc_failed, oc_char, oc_bucket, oc_draws, oc_view;
    float oc_dist, oc_dpx;
    uint64_t oc_key;
    LONG64 oc_seq, oc_tris;
    void* oc_ctx;
    void* oc_prev_ctx; /* the previous object on this thread: the draw order */
    int oc_prev_bucket;
    float oc_prev_dist;
} QThread;

static QThread* q_thread(int create)
{
    if (g_tls_q == TLS_OUT_OF_INDEXES)
        return NULL;
    QThread* t = (QThread*)TlsGetValue(g_tls_q);
    if (!t && create) {
        t = (QThread*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(QThread));
        if (t)
            t->cls = -1, t->node_dist = -1;
        TlsSetValue(g_tls_q, t);
    }
    return t;
}

void gpuprobe_set_far(int bias)
{
    QThread* t = q_thread(bias != 0);
    if (t)
        t->far_bias = bias;
}

void gpuprobe_scene_node(int far_bias, int cls, float area_px, float dist_m)
{
    QThread* t = q_thread(1);
    if (t)
        t->far_bias = far_bias, t->cls = cls, t->area = area_px, t->dist = dist_m;
}

void gpuprobe_node_mark(float dist_m, int is_char, const float* c, LONG64 seq)
{
    QThread* t = q_thread(dist_m >= 0);
    if (!t)
        return;
    t->node_dist = dist_m, t->node_char = is_char, t->node_carried = -1, t->node_seq = seq;
    if (c)
        t->node_c[0] = c[0], t->node_c[1] = c[1], t->node_c[2] = c[2];
}

LONG64 gpuprobe_node_seq(void)
{
    QThread* t = q_thread(0);
    return t && t->node_dist >= 0 ? t->node_seq : 0;
}

float gpuprobe_node_raw_dist(void)
{
    QThread* t = q_thread(0);
    return t && t->node_dist >= 0 ? t->node_dist : -1;
}

/* -1 no scene object; -2 an object that is not a character (nor carried by one: [Quality] OutlineCarried) */
float gpuprobe_node_dist(int* carried)
{
    QThread* t = q_thread(0);
    *carried = 0;
    if (!t || t->node_dist < 0)
        return -1;
    if (t->node_char)
        return t->node_dist;
    if (!g_cfg.q_outline_carried)
        return -2;
    if (t->node_carried < 0) /* once per object: its draws share the answer */
        t->node_carried = shadowcull_carried(t->node_c, t->node_seq);
    *carried = t->node_carried;
    return t->node_carried ? t->node_dist : -2;
}

/* [Quality] FarMeshes: the object being drawn (scene hook, shadow caster hook) */
void gpuprobe_fm_mark(int kind, float near_cm, float scale, float tpu)
{
    QThread* t = q_thread(kind != 0);
    if (t)
        t->fm_kind = kind, t->fm_near = near_cm, t->fm_scale = scale, t->fm_tpu = tpu;
}

/* a simpler version of the mesh, if there is one for this draw (farmesh.c) */
static int fm_pick(ID3D11DeviceContext* c, UINT count, UINT start, INT base, FmPick* p)
{
    if (!(g_cfg.q_farmesh || g_cfg.q_fm_both) || count < 1536)
        return 0;
    QThread* t = q_thread(0);
    int kind = t ? t->fm_kind : 0;
    if (kind == 3 || (!kind && shadowcull_current_cascade() >= 0))
        return 0; /* a character; a shadow draw of an unknown caster (nothing has to match it) */
    return farmesh_pick(c, count, start, base, kind, t ? t->fm_near : 0, t ? t->fm_scale : 0, t ? t->fm_tpu : 0, p);
}

/* [GpuProbe] MeshCensus: the indexed draw, filed under its mesh while a census records */
static void census_q(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base)
{
    if (!g_cfg.mesh_census || !census_recording())
        return;
    QThread* t = q_thread(0);
    census_draw(c, count, inst, start, base, t ? t->cls : -1, t ? t->area : 0, t ? t->dist : 0,
                shadowcull_current_cascade() >= 0);
}

/* ---------------------------------------------------------------- occlusion census */
/* [GpuProbe] OcclusionCensus: every scene object of the mode 3 passes (the G-buffer, the character
 * passes) is drawn inside an occlusion query, begun at its first draw on that draw's context and ended
 * with the object. Read back on the immediate context two or more frames later. 0 samples = everything
 * drawn before it in the pass hid it. Each object's sphere is also placed against the view frustum: inside,
 * at the edge, outside (those must show nothing: a check of the test). Per object (its geometry and bucket)
 * the last 32 scene frames are kept: whether an object inside the view and hidden a few frames earlier is
 * still hidden says what a culling that waits for its queries would skip, and what it would wrongly leave
 * out. Measures only: nothing is skipped. */
#define OC_RING 32768
typedef struct OcSlot {
    ID3D11Query* q;
    uint64_t key;
    LONG64 seq, frame, tris;
    float dist, dpx;
    int is_char, bucket, view, px, h, nrt;
    volatile LONG state; /* 0 free / claimed, 1 begun, 2 ended, 3 dropped */
} OcSlot;
static OcSlot g_ocs[OC_RING];
static volatile LONG64 g_ochead, g_octail;
static ID3D11Device* volatile g_ocdev;
static ID3D11DeviceContext* volatile g_ocimm;
static volatile LONG64 oc_full, oc_nodraw, oc_pairs, oc_pairs_farther; /* statistics: plain increments */

static ID3D11Device* oc_device(ID3D11DeviceContext* c)
{
    ID3D11Device* d = g_ocdev;
    if (d)
        return d;
    ID3D11DeviceContext_GetDevice(c, &d);
    if (!d)
        return NULL;
    if (InterlockedCompareExchangePointer((void* volatile*)&g_ocdev, d, NULL) != NULL) {
        ID3D11Device_Release(d); /* another thread was first */
        return g_ocdev;
    }
    ID3D11DeviceContext* imm = NULL;
    ID3D11Device_GetImmediateContext(d, &imm);
    g_ocimm = imm;
    return d;
}

void gpuprobe_oc_node(int on, uint64_t key, float dist_m, float dpx, int is_char, int bucket, int view, LONG64 seq)
{
    QThread* t = q_thread(on);
    if (!t)
        return;
    if (on) {
        t->oc_on = 1, t->oc_slot = -1, t->oc_failed = 0, t->oc_draws = 0, t->oc_tris = 0;
        t->oc_key = key, t->oc_dist = dist_m, t->oc_dpx = dpx, t->oc_char = is_char, t->oc_bucket = bucket;
        t->oc_seq = seq, t->oc_view = view;
        return;
    }
    if (t->oc_slot >= 0) {
        OcSlot* s = &g_ocs[t->oc_slot];
        ID3D11DeviceContext_End((ID3D11DeviceContext*)t->oc_ctx, (ID3D11Asynchronous*)s->q);
        s->key = t->oc_key, s->seq = t->oc_seq, s->tris = t->oc_tris, s->dist = t->oc_dist, s->dpx = t->oc_dpx;
        s->is_char = t->oc_char, s->bucket = t->oc_bucket, s->view = t->oc_view, s->frame = g_tframes;
        MemoryBarrier();
        s->state = 2;
    } else if (t->oc_on && !t->oc_draws) {
        oc_nodraw++;
    }
    t->oc_on = 0, t->oc_slot = -1;
}

/* before a draw: the object's first draw begins its query */
static void oc_draw(ID3D11DeviceContext* c, UINT verts)
{
    if (!g_cfg.occl_census)
        return;
    QThread* t = q_thread(0);
    if (!t || !t->oc_on)
        return;
    t->oc_tris += verts / 3;
    if (t->oc_draws++ || t->oc_failed)
        return;
    if (c == t->oc_prev_ctx && t->oc_bucket == t->oc_prev_bucket) { /* draw order within a bucket */
        oc_pairs++;
        if (t->oc_dist >= t->oc_prev_dist)
            oc_pairs_farther++;
    }
    t->oc_prev_ctx = c, t->oc_prev_bucket = t->oc_bucket, t->oc_prev_dist = t->oc_dist;
    ID3D11Device* dev = oc_device(c);
    LONG64 h;
    do { /* claim a slot without lapping the reader */
        h = g_ochead;
        if (!dev || h - g_octail >= OC_RING) {
            oc_full++;
            t->oc_failed = 1;
            return;
        }
    } while (InterlockedCompareExchange64(&g_ochead, h + 1, h) != h);
    OcSlot* s = &g_ocs[h % OC_RING];
    s->state = 1;
    if (!s->q) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_OCCLUSION, 0};
        if (FAILED(ID3D11Device_CreateQuery(dev, &d, &s->q))) {
            s->q = NULL;
            s->state = 3;
            t->oc_failed = 1;
            return;
        }
    }
    Ctx* x = ctx_of(c);
    s->px = x->cur.w * x->cur.h, s->h = x->cur.h, s->nrt = x->nrt;
    ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous*)s->q);
    t->oc_slot = (int)(h % OC_RING), t->oc_ctx = c;
}

/* results (Present thread only) */
#define OC_CLS 6      /* world objects by distance band (GEO_BANDS), then characters */
#define OC_BUCKETS 64 /* the scene's render buckets (passes); the last one takes the rest */
#define OC_PRED 2     /* inside the view: hidden 3 frames earlier / hidden 4 frames in a row up to 3 frames earlier */
#define OC_VIEWS 4    /* inside the view, at its edge, outside, not known */
#define OC_KEYS 65536
typedef struct OcKey {
    uint64_t key;
    LONG64 seq;
    uint32_t hid, seen; /* bit i: hidden / measured i scene frames before seq */
} OcKey;
static OcKey g_ock[OC_KEYS];
static int g_ock_n;
static LONG64 oc_nodes[OC_CLS], oc_hnodes[OC_CLS], oc_tris[OC_CLS], oc_htris[OC_CLS], oc_unread;
static LONG64 oc_tracked, oc_ptris[OC_PRED], oc_pnodes[OC_PRED], oc_ppop[OC_PRED], oc_ppop_px[OC_PRED];
static LONG64 oc_b_nodes[OC_BUCKETS], oc_b_tris[OC_BUCKETS], oc_b_htris[OC_BUCKETS], oc_b_char[OC_BUCKETS];
static int oc_b_w[OC_BUCKETS], oc_b_h[OC_BUCKETS], oc_b_nrt[OC_BUCKETS];
static LONG64 oc_seq_max, oc_px, oc_samples;
static LONG64 oc_v_tris[OC_VIEWS], oc_v_htris[OC_VIEWS], oc_out_drew, oc_out_px; /* outside but drew: the check */

static OcKey* oc_key(uint64_t key)
{
    unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 48);
    for (int n = 0; n < 64; n++, i++) {
        OcKey* e = &g_ock[i & (OC_KEYS - 1)];
        if (e->key == key)
            return e;
        if (!e->key) {
            if (g_ock_n >= OC_KEYS / 2)
                return NULL; /* emptied at the next report */
            e->key = key;
            g_ock_n++;
            return e;
        }
    }
    return NULL;
}

static void oc_result(const OcSlot* s, int64_t samples)
{
    if (samples < 0) {
        oc_unread++;
        return;
    }
    int hidden = samples == 0;
    float d = s->dist;
    int k = s->is_char ? OC_CLS - 1 : d < 15 ? 0 : d < 30 ? 1 : d < 50 ? 2 : d < 100 ? 3 : 4;
    oc_nodes[k]++, oc_tris[k] += s->tris;
    if (hidden)
        oc_hnodes[k]++, oc_htris[k] += s->tris;
    int b = s->bucket >= 0 && s->bucket < OC_BUCKETS ? s->bucket : OC_BUCKETS - 1;
    oc_b_nodes[b]++, oc_b_tris[b] += s->tris, oc_b_char[b] += s->is_char;
    if (hidden)
        oc_b_htris[b] += s->tris;
    oc_b_w[b] = s->px && s->h ? s->px / s->h : 0, oc_b_h[b] = s->h, oc_b_nrt[b] = s->nrt;
    oc_samples += samples;
    int v = s->view >= 0 && s->view < OC_VIEWS - 1 ? s->view : OC_VIEWS - 1;
    oc_v_tris[v] += s->tris;
    if (hidden)
        oc_v_htris[v] += s->tris;
    else if (v == 2)
        oc_out_drew++, oc_out_px += samples;
    if (s->px > oc_px)
        oc_px = s->px;
    if (s->seq > oc_seq_max)
        oc_seq_max = s->seq;
    OcKey* e = oc_key(s->key);
    if (!e)
        return;
    if (e->seq == s->seq) { /* the same object again in this frame: hidden only if hidden every time */
        if (!hidden)
            e->hid &= ~1u;
        return;
    }
    LONG64 sh = s->seq - e->seq;
    if (!e->seq || sh < 0 || sh >= 32)
        e->hid = e->seen = 0;
    else
        e->hid <<= sh, e->seen <<= sh;
    /* a culling that reads its queries 3 frames late: skip if hidden then / hidden 4 frames in a row */
    int pred[OC_PRED] = {(e->seen & e->hid) >> 3 & 1, ((e->seen & e->hid) >> 3 & 0xF) == 0xF};
    if (e->seen >> 3 & 1)
        oc_tracked++;
    for (int p = 0; p < OC_PRED && s->view == 0; p++) /* a culling leaves the view's edge alone */
        if (pred[p]) {
            oc_pnodes[p]++, oc_ptris[p] += s->tris;
            if (!hidden)
                oc_ppop[p]++, oc_ppop_px[p] += samples; /* left out although it would show */
        }
    e->hid |= (uint32_t)hidden, e->seen |= 1, e->seq = s->seq;
}

static void oc_read(void)
{
    static int marks;
    if (!g_cfg.mesh_census && (GetAsyncKeyState(VK_F9) & 1)) /* the player names the places in the log */
        LOGI("Occlusion census: place mark %d (F9)", ++marks);
    if (!g_ocimm)
        return;
    while (g_octail < g_ochead) {
        OcSlot* s = &g_ocs[g_octail % OC_RING];
        LONG st = s->state;
        if (st == 0 || st == 1)
            break; /* still being recorded */
        if (st == 2) {
            if (s->frame + 2 > g_tframes)
                break;
            UINT64 n = 0;
            HRESULT hr = ID3D11DeviceContext_GetData(g_ocimm, (ID3D11Asynchronous*)s->q, &n, sizeof(n),
                                                     D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE && s->frame + 30 > g_tframes)
                break; /* not ready yet; a command list that never ran gives up after 30 frames */
            oc_result(s, hr == S_OK ? (int64_t)n : -1);
        }
        s->state = 0;
        InterlockedIncrement64(&g_octail);
    }
}

static int oc_pct(LONG64 a, LONG64 b)
{
    return b > 0 ? (int)(a * 100.0 / b + 0.5) : 0;
}

static void oc_report(void)
{
    static LONG64 pf, pseq, pfull, pnd, ppairs, pfar;
    LONG64 f = g_tframes - pf, scene = oc_seq_max - pseq;
    pf = g_tframes, pseq = oc_seq_max;
    LONG64 full = oc_full - pfull, nd = oc_nodraw - pnd, pairs = oc_pairs - ppairs, farther = oc_pairs_farther - pfar;
    pfull = oc_full, pnd = oc_nodraw, ppairs = oc_pairs, pfar = oc_pairs_farther;
    if (f <= 0)
        return;
    LONG64 n = 0, hn = 0, t = 0, ht = 0;
    for (int k = 0; k < OC_CLS; k++)
        n += oc_nodes[k], hn += oc_hnodes[k], t += oc_tris[k], ht += oc_htris[k];
    if (!n && !g_cfg.occl_census) {
        ; /* Compare=occlusion: the census is off in this window */
    } else if (!n) {
        LOGI("  Occlusion census: no results yet (%lld objects without draws, %lld without a free query, %lld unread)",
             (long long)nd, (long long)full, (long long)oc_unread);
    } else {
        double fr = (double)f;
        LOGI("  Occlusion census: %.0f objects/frame, %.2fM triangles/frame | hidden (0 samples): %d%% of objects, "
             "%d%% of triangles (%.0fK/frame) | world by distance <15 / 15-30 / 30-50 / 50-100 / >100 m: hidden "
             "%d / %d / %d / %d / %d%% of %.0f / %.0f / %.0f / %.0f / %.0fK triangles | characters: hidden %d%% of "
             "%.0fK | by the view: inside %d%% of the triangles (hidden %d%%), at the edge %d%% (hidden %d%%), outside "
             "%d%% (hidden %d%%; %.1f objects/frame outside drew %.0f px), not known %d%% | %.2f samples passed per "
             "target pixel | %.2f scene frames per frame, %.0f objects without draws, %.0f without a free query, "
             "%lld unread",
             n / fr, t / 1e6 / fr, oc_pct(hn, n), oc_pct(ht, t), ht / 1e3 / fr, oc_pct(oc_htris[0], oc_tris[0]),
             oc_pct(oc_htris[1], oc_tris[1]), oc_pct(oc_htris[2], oc_tris[2]), oc_pct(oc_htris[3], oc_tris[3]),
             oc_pct(oc_htris[4], oc_tris[4]), oc_tris[0] / 1e3 / fr, oc_tris[1] / 1e3 / fr, oc_tris[2] / 1e3 / fr,
             oc_tris[3] / 1e3 / fr, oc_tris[4] / 1e3 / fr, oc_pct(oc_htris[5], oc_tris[5]), oc_tris[5] / 1e3 / fr,
             oc_pct(oc_v_tris[0], t), oc_pct(oc_v_htris[0], oc_v_tris[0]), oc_pct(oc_v_tris[1], t),
             oc_pct(oc_v_htris[1], oc_v_tris[1]), oc_pct(oc_v_tris[2], t), oc_pct(oc_v_htris[2], oc_v_tris[2]),
             oc_out_drew / fr, oc_out_px / fr, oc_pct(oc_v_tris[3], t),
             oc_px ? oc_samples / fr / (double)oc_px : 0.0, scene / fr, nd / fr, full / fr, (long long)oc_unread);
        LOGI("  Occlusion census over time, objects inside the view (a culling that reads its queries 3 frames late): "
             "skipping what was hidden then: %.0f objects/frame, %d%% of the triangles, %.1f of them objects/frame that show (%.0f px/frame, "
             "%.3f%% of the %lld-pixel target) | hidden 4 frames in a row: %.0f objects/frame, %d%% of the "
             "triangles, %.1f wrong objects/frame (%.0f px/frame) | %d%% of objects tracked | draw order: %d%% of "
             "neighbouring objects farther than the one before (front to back = 100%%)",
             oc_pnodes[0] / fr, oc_pct(oc_ptris[0], t), oc_ppop[0] / fr, oc_ppop_px[0] / fr,
             oc_px ? oc_ppop_px[0] * 100.0 / fr / (double)oc_px : 0.0, (long long)oc_px, oc_pnodes[1] / fr,
             oc_pct(oc_ptris[1], t), oc_ppop[1] / fr, oc_ppop_px[1] / fr, oc_pct(oc_tracked, n), oc_pct(farther, pairs));
        char line[700];
        int len = 0;
        line[0] = 0;
        for (int r = 0; r < 6; r++) { /* the buckets with the most triangles */
            int best = -1;
            for (int b = 0; b < OC_BUCKETS; b++)
                if (oc_b_tris[b] > 0 && (best < 0 || oc_b_tris[b] > oc_b_tris[best]))
                    best = b;
            if (best < 0)
                break;
            len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%s%d (%dx%d, %d targets, %s): %.0f objects, "
                            "%.0fK triangles, hidden %d%%", len ? " | " : "", best == OC_BUCKETS - 1 ? ">=" : "", best,
                            oc_b_w[best], oc_b_h[best], oc_b_nrt[best],
                            oc_b_char[best] * 2 > oc_b_nodes[best] ? "characters" : "world", oc_b_nodes[best] / fr,
                            oc_b_tris[best] / 1e3 / fr, oc_pct(oc_b_htris[best], oc_b_tris[best]));
            oc_b_tris[best] = -1; /* listed (the counters start afresh below) */
            if (len >= (int)sizeof(line) - 120)
                break;
        }
        LOGI("  Occlusion census by bucket (per frame): %s", line);
    }
    memset(oc_nodes, 0, sizeof(oc_nodes)), memset(oc_hnodes, 0, sizeof(oc_hnodes));
    memset(oc_tris, 0, sizeof(oc_tris)), memset(oc_htris, 0, sizeof(oc_htris));
    memset(oc_ptris, 0, sizeof(oc_ptris)), memset(oc_pnodes, 0, sizeof(oc_pnodes));
    memset(oc_ppop, 0, sizeof(oc_ppop)), memset(oc_ppop_px, 0, sizeof(oc_ppop_px));
    memset(oc_b_nodes, 0, sizeof(oc_b_nodes)), memset(oc_b_tris, 0, sizeof(oc_b_tris));
    memset(oc_b_htris, 0, sizeof(oc_b_htris)), memset(oc_b_char, 0, sizeof(oc_b_char));
    memset(oc_v_tris, 0, sizeof(oc_v_tris)), memset(oc_v_htris, 0, sizeof(oc_v_htris));
    oc_tracked = 0, oc_samples = 0, oc_unread = 0, oc_out_drew = 0, oc_out_px = 0;
    if (g_ock_n >= OC_KEYS / 2) { /* walked far: start the history afresh */
        memset(g_ock, 0, sizeof(g_ock));
        g_ock_n = 0;
    }
}

/* For tests\frame_test.c (occlusion mode): one scene object around a draw, as the scene hook marks it,
 * and the census counters */
__declspec(dllexport) void MetaphorCPU_TestOcclusionNode(void (*draw)(void*), void* arg, uint64_t key, float dist_m,
                                                        LONG64 seq)
{
    gpuprobe_oc_node(1, key, dist_m, 100.0f, 0, 1, 0, seq);
    draw(arg);
    gpuprobe_oc_node(0, 0, 0, 0, 0, 0, 0, 0);
}

/* the sampler key wanted now: global bias + this thread's object bias (far objects, characters; quarter mip
 * levels), the anisotropy clamp */
static int q_key_now(void)
{
    QThread* t = q_thread(0);
    return QK(4 * g_cfg.q_mip_bias + (t ? t->far_bias : 0), g_cfg.q_max_aniso);
}

static void q_bind(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s, int key)
{
    ID3D11SamplerState* t[16];
    for (UINT i = 0; i < n; i++)
        t[i] = (s[i] && key) ? q_twin(s[i], key) : s[i];
    O(PSSetSamp_t, c, SLOT(PSSetSamplers))(c, start, n, t);
}

static void q_forget(Ctx* x, ID3D11DeviceContext* c)
{
    if (x->ctx != c)
        return;
    memset(x->q_orig, 0, sizeof(x->q_orig));
    x->q_n = 0, x->q_all = 0;
}

static void STDMETHODCALLTYPE hk_PSSetSamp(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    int key = q_key_now();
    if (s && n > 0 && start + n <= 16) {
        if (g_cfg.q_report)
            count_binds(n, s);
        if (key) {
            q_bind(c, start, n, s, key);
            InterlockedExchangeAdd64(&q_samp_swaps, n);
        } else {
            O(PSSetSamp_t, c, SLOT(PSSetSamplers))(c, start, n, s);
        }
        Ctx* x = g_q_far_hooks ? ctx_of(c) : NULL;
        if (x && x->ctx == c) {
            for (UINT i = 0; i < n; i++)
                x->q_orig[start + i] = s[i], x->q_sb[start + i] = key;
            int hi = 0, all = key;
            for (int i = 0; i < 16; i++)
                if (x->q_orig[i]) {
                    hi = i + 1;
                    if (x->q_sb[i] != key)
                        all = -1;
                }
            x->q_n = hi, x->q_all = all;
        }
    } else {
        O(PSSetSamp_t, c, SLOT(PSSetSamplers))(c, start, n, s);
    }
    KEEP_HOOKED(c);
}

/* before a draw: count its triangles for the object being drawn; the samplers carry the key
 * wanted for it (far bias, anisotropy clamp, both switchable at run time) */
static void q_draw(ID3D11DeviceContext* c, UINT verts)
{
    if (!g_q_far_hooks)
        return;
    QThread* t = q_thread(0);
    if (g_cfg.q_report) { /* plain increments: statistics */
        LONG64 tris = verts / 3;
        if (t && t->cls >= 0 && t->cls < GEO_CLASSES) {
            q_geo_tris[t->cls] += tris;
            if (tris > 0 && t->area < 4.0f * (float)tris)
                q_geo_micro[t->cls] += tris;
        } else if (shadowcull_current_cascade() >= 0) {
            q_geo_shadow += tris;
        } else {
            q_geo_other += tris;
        }
    }
    int key = QK(4 * g_cfg.q_mip_bias + (t ? t->far_bias : 0), g_cfg.q_max_aniso);
    if (t && t->far_bias)
        q_far_draws++;
    Ctx* x = ctx_of(c);
    if (x->ctx != c || !x->q_n || x->q_all == key)
        return;
    q_bind(c, 0, (UINT)x->q_n, (ID3D11SamplerState* const*)x->q_orig, key);
    for (int i = 0; i < x->q_n; i++)
        x->q_sb[i] = key;
    x->q_all = key;
    q_rebinds++;
}

typedef void (STDMETHODCALLTYPE* PSSetSRV_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void (STDMETHODCALLTYPE* PSSetShader_t)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
                                                UINT);
static void STDMETHODCALLTYPE hk_PSSetSRV(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
{
    capture_srvs(c, start, n, (void* const*)v);
    O(PSSetSRV_t, c, SLOT(PSSetShaderResources))(c, start, n, v);
    KEEP_HOOKED(c);
}
/* temporal.c draws a pass of its own: through the hooks it would count as the game's (and be changed by them) */
void* gpuprobe_method(void* self, size_t slot)
{
    void** vt = *(void***)self;
    LONG n = g_nvt;
    for (LONG i = 0; i < n; i++)
        if (g_vts[i].vt == vt)
            return slot < NSLOTS && g_vts[i].orig[slot] ? g_vts[i].orig[slot] : vt[slot];
    return vt[slot];
}

/* effects.c ([Quality] RimLight): another copy of the bound shader, for the object about to be drawn */
void gpuprobe_bind_ps(void* ctx, void* ps)
{
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    O(PSSetShader_t, c, SLOT(PSSetShader))(c, (ID3D11PixelShader*)ps, NULL, 0);
}

static void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci,
                                             UINT nci)
{
    capture_ps(c, ps);
    if (g_cfg.gpu_timing)
        ctx_of(c)->ps_id = fx_ps_id(ps);
    ID3D11PixelShader* use = g_cfg.q_fx_active ? (ID3D11PixelShader*)effects_bind_ps(c, ps) : ps;
    O(PSSetShader_t, c, SLOT(PSSetShader))(c, use, ci, nci);
    KEEP_HOOKED(c);
}
typedef void (STDMETHODCALLTYPE* VSSetShader_t)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*,
                                                UINT);
static void STDMETHODCALLTYPE hk_VSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs,
                                             ID3D11ClassInstance* const* ci, UINT nci)
{
    ID3D11VertexShader* use = g_cfg.q_fx_active ? (ID3D11VertexShader*)effects_bind_vs(c, vs) : vs;
    O(VSSetShader_t, c, SLOT(VSSetShader))(c, use, ci, nci);
    KEEP_HOOKED(c);
}

typedef void (STDMETHODCALLTYPE* ClearState_t)(ID3D11DeviceContext*);
static void STDMETHODCALLTYPE hk_ClearState(ID3D11DeviceContext* c)
{
    if (g_cfg.q_mv_hooks)
        motion_flush(c);
    if (g_cfg.capture)
        capture_clear_state(c);
    if (g_cfg.q_fx_active)
        effects_reset(c);
    if (g_q_far_hooks)
        q_forget(ctx_of(c), c);
    if (g_cfg.gpu_timing)
        ctx_of(c)->ps_id = 0;
    O(ClearState_t, c, SLOT(ClearState))(c);
    KEEP_HOOKED(c);
}

/* ---------------------------------------------------------------- context hooks */
/* GPU timing: a new set of targets? (the same set bound again needs no mark: a timestamp makes
 * DXVK end the running render pass, which would split it on a tiled GPU) */
static int new_binding(Ctx* x, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    void* b[9] = {0};
    for (UINT i = 0; rtvs && i < n && i < 8; i++)
        b[i] = rtvs[i];
    b[8] = dsv;
    if (memcmp(b, x->bound, sizeof(b)) == 0)
        return 0;
    memcpy(x->bound, b, sizeof(b));
    return 1;
}

/* GPU timing: the same targets bound again get no mark (the render pass runs on): the running pass keeps its name */
typedef struct PassId {
    uint64_t key;
    int seg_ps, ps_pending;
    LONG64 work_slot;
} PassId;
static PassId pass_id(const Ctx* x)
{
    PassId p = {x->key, x->seg_ps, x->ps_pending, x->work_slot};
    return p;
}
static void keep_pass(Ctx* x, const PassId* p)
{
    if (g_cfg.gpu_timing && x->work_marked)
        x->key = p->key, x->seg_ps = p->seg_ps, x->ps_pending = p->ps_pending, x->work_slot = p->work_slot;
}

static void STDMETHODCALLTYPE hk_OMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                         ID3D11DepthStencilView* dsv)
{
    if (g_cfg.capture)
        capture_bind(c, n, (void* const*)rtvs, dsv);
    Ctx* x = ctx_of(c);
    if (g_cfg.gpu_timing)
        pass_done(x);
    PassId was = pass_id(x);
    begin_pass(x, n, rtvs, dsv);
    if (g_cfg.gpu_passes)
        pa_bind(x, n, rtvs, dsv);
    if (g_tls_ctx != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_tls_ctx, c);
    if (g_cfg.q_mv_hooks) /* [Quality] CharacterMotionVectors: leaving the characters' pass, its draws drawn again */
        motion_bind(c, n, (void* const*)rtvs, dsv);
    O(OMSetRT_t, c, SLOT(OMSetRenderTargets))(c, n, rtvs, dsv);
    if (g_cfg.gpu_timing && (n || dsv) && new_binding(x, n, rtvs, dsv)) {
        ts_mark(c, x, AS_KIND(x->key, K_BIND));
        x->work_marked = 0;
    }
    keep_pass(x, &was);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_OMSetRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                            ID3D11DepthStencilView* dsv, UINT us, UINT un,
                                            ID3D11UnorderedAccessView* const* uavs, const UINT* cnt)
{
    Ctx* x = NULL;
    PassId was = {0, -1, 0, -1};
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        if (g_cfg.capture)
            capture_bind(c, n, (void* const*)rtvs, dsv);
        x = ctx_of(c);
        if (g_cfg.gpu_timing)
            pass_done(x);
        was = pass_id(x);
        begin_pass(x, n, rtvs, dsv);
        if (g_cfg.gpu_passes)
            pa_bind(x, n, rtvs, dsv);
        if (g_tls_ctx != TLS_OUT_OF_INDEXES)
            TlsSetValue(g_tls_ctx, c);
    }
    if (g_cfg.q_mv_hooks && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        motion_bind(c, n, (void* const*)rtvs, dsv);
    O(OMSetRTUAV_t, c, SLOT(OMSetRenderTargetsAndUnorderedAccessViews))(c, n, rtvs, dsv, us, un, uavs, cnt);
    if (g_cfg.gpu_timing && x && (n || dsv) && new_binding(x, n, rtvs, dsv)) {
        ts_mark(c, x, AS_KIND(x->key, K_BIND));
        x->work_marked = 0;
    }
    if (x)
        keep_pass(x, &was);
    KEEP_HOOKED(c);
}

/* GPU timing: a clear is the pass's first GPU work too */
typedef void (STDMETHODCALLTYPE* ClearRTV_t)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void (STDMETHODCALLTYPE* ClearDSV_t)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
static void timing_work(ID3D11DeviceContext* c)
{
    Ctx* x = ctx_of(c);
    if (!x->work_marked && x->has_pass) {
        x->work_slot = ts_mark(c, x, x->key);
        x->work_marked = 1;
        x->ps_pending = 1; /* named after the first draw's pixel shader when it comes */
    }
}

static void STDMETHODCALLTYPE hk_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4])
{
    if (g_cfg.gpu_timing)
        timing_work(c);
    if (g_cfg.gpu_passes && v)
        pa_clear(ctx_of(c), v, 0);
    O(ClearRTV_t, c, SLOT(ClearRenderTargetView))(c, v, col);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_ClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s)
{
    if (g_cfg.gpu_timing)
        timing_work(c);
    if (g_cfg.gpu_passes && v)
        pa_clear(ctx_of(c), v, 0);
    O(ClearDSV_t, c, SLOT(ClearDepthStencilView))(c, v, f, d, s);
    KEEP_HOOKED(c);
}

/* GPU timing: compute work between passes gets its own segment */
typedef void (STDMETHODCALLTYPE* Dispatch_t)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* DispatchInd_t)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
/* ---------------------------------------------------------------- [GpuProbe] ComputeCensus */
/* The compute shaders the game dispatches: each by the hash of its bytecode with the resources its bytecode
 * declares (RDEF names), how often it runs, how many thread groups, and what it writes (its first UAV: a buffer, which
 * the vertex-buffer bind flag shows to be skinning output, or a texture). With [GpuProbe] Timing each compute shader
 * gets a timing row of its own: a timestamp where the compute shader changes. */
#define CS_N 512
static struct {
    void* sh;
    uint64_t hash;
    char names[200];
    char writes[80];
    volatile LONG64 dispatches, groups;
} g_cs[CS_N];
static volatile LONG g_ncs;
static SRWLOCK g_cs_lock = SRWLOCK_INIT;
static void* g_dev_create_cs;
typedef HRESULT(STDMETHODCALLTYPE* CreateCS_t)(void*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);

static HRESULT STDMETHODCALLTYPE hk_CreateCS(void* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl,
                                             ID3D11ComputeShader** cs)
{
    HRESULT hr = ((CreateCS_t)g_dev_create_cs)(d, code, len, cl, cs);
    if (SUCCEEDED(hr) && cs && *cs && code && len >= 32) {
        AcquireSRWLockExclusive(&g_cs_lock);
        LONG i = g_ncs;
        if (i < CS_N) {
            g_cs[i].sh = *cs, g_cs[i].hash = fx_fnv64(code, len);
            dxbc_bind_names((const uint8_t*)code, len, g_cs[i].names, sizeof(g_cs[i].names));
            InterlockedIncrement(&g_ncs);
        }
        ReleaseSRWLockExclusive(&g_cs_lock);
    }
    return hr;
}

/* the dispatch's compute shader (its id, 1.., 0 not known), counted with what it writes */
static int cs_note(ID3D11DeviceContext* c, UINT64 groups)
{
    ID3D11ComputeShader* sh = NULL;
    ID3D11DeviceContext_CSGetShader(c, &sh, NULL, NULL);
    if (!sh)
        return 0;
    int id = 0;
    LONG n = g_ncs;
    for (LONG i = 0; i < n && !id; i++)
        if (g_cs[i].sh == (void*)sh)
            id = (int)i + 1;
    ID3D11ComputeShader_Release(sh);
    if (!id)
        return 0;
    InterlockedIncrement64(&g_cs[id - 1].dispatches);
    InterlockedAdd64(&g_cs[id - 1].groups, (LONG64)groups);
    if (!g_cs[id - 1].writes[0]) { /* what its first UAV is, once */
        ID3D11UnorderedAccessView* u = NULL;
        ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, 1, &u);
        char w[80] = "nothing at u0";
        if (u) {
            ID3D11Resource* r = NULL;
            ID3D11UnorderedAccessView_GetResource(u, &r);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (r)
                ID3D11Resource_GetType(r, &dim);
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC bd;
                ID3D11Buffer_GetDesc((ID3D11Buffer*)r, &bd);
                snprintf(w, sizeof(w), "a buffer of %u KB%s%s", bd.ByteWidth / 1024,
                         (bd.BindFlags & D3D11_BIND_VERTEX_BUFFER) ? " (a vertex buffer: skinning output)" : "",
                         (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ? " (structured)" : "");
            } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc((ID3D11Texture2D*)r, &td);
                snprintf(w, sizeof(w), "a %ux%u texture (format %d)", td.Width, td.Height, (int)td.Format);
            } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
                snprintf(w, sizeof(w), "a 3D texture");
            }
            if (r)
                ID3D11Resource_Release(r);
            ID3D11UnorderedAccessView_Release(u);
        }
        memcpy(g_cs[id - 1].writes, w, sizeof(w));
    }
    return id;
}

static void cs_report(void)
{
    static LONG64 pd[CS_N], pg[CS_N], pf;
    LONG64 fr = g_frames.frames - pf;
    pf = g_frames.frames;
    LONG n = g_ncs;
    int order[CS_N], k = 0;
    LONG64 d[CS_N];
    for (LONG i = 0; i < n; i++) {
        d[i] = g_cs[i].dispatches - pd[i];
        if (d[i] > 0)
            order[k++] = (int)i;
    }
    for (int a = 1; a < k; a++) /* most dispatches first */
        for (int b = a; b > 0 && d[order[b]] > d[order[b - 1]]; b--) {
            int t = order[b];
            order[b] = order[b - 1], order[b - 1] = t;
        }
    double per = fr > 0 ? 1.0 / (double)fr : 0;
    LOGI("  Compute: %d compute shaders dispatched in this window (%ld created), by dispatches:", k, (long)n);
    for (int a = 0; a < k && a < 12; a++) {
        int i = order[a];
        LONG64 g = g_cs[i].groups - pg[i];
        LOGI("    CS %016llx: %.1f dispatches, %.0f thread groups a frame; writes %s | %s",
             (unsigned long long)g_cs[i].hash, (double)d[i] * per, (double)g * per, g_cs[i].writes, g_cs[i].names);
    }
    for (LONG i = 0; i < n; i++)
        pd[i] = g_cs[i].dispatches, pg[i] = g_cs[i].groups;
}

static void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    Ctx* cx = ctx_of(c);
    int cs = g_cfg.compute_census ? cs_note(c, (UINT64)x * y * z) : 0;
    if (g_cfg.gpu_timing && (cx->last_kind != K_COMPUTE || cx->last_cs != cs)) {
        ts_mark(c, cx, KEY(K_COMPUTE, cs, 0, 0, 0, 0));
        cx->last_cs = cs;
        cx->work_marked = 0; /* draws after it are pass work again */
    }
    pa_spill(c, SP_DISPATCH);
    O(Dispatch_t, c, SLOT(Dispatch))(c, x, y, z);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_DispatchInd(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off)
{
    Ctx* cx = ctx_of(c);
    int cs = g_cfg.compute_census ? cs_note(c, 0) : 0;
    if (g_cfg.gpu_timing && (cx->last_kind != K_COMPUTE || cx->last_cs != cs)) {
        ts_mark(c, cx, KEY(K_COMPUTE, cs, 0, 0, 0, 0));
        cx->last_cs = cs;
        cx->work_marked = 0;
    }
    pa_spill(c, SP_DISPATCH);
    O(DispatchInd_t, c, SLOT(DispatchIndirect))(c, b, off);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_IASetIB(ID3D11DeviceContext* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT off)
{
    ctx_of(c)->ib = b;
    O(IASetIB_t, c, SLOT(IASetIndexBuffer))(c, b, f, off);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_IASetVB(ID3D11DeviceContext* c, UINT start, UINT num, ID3D11Buffer* const* b,
                                         const UINT* strides, const UINT* offs)
{
    if (start == 0 && num && b)
        ctx_of(c)->vb = b[0];
    O(IASetVB_t, c, SLOT(IASetVertexBuffers))(c, start, num, b, strides, offs);
    KEEP_HOOKED(c);
}

/* [Quality] LineFix=redraw: the draw again with the viewport shifted by a third of a pixel (right, down, left, up), so a
 * line mesh thinner than a pixel fills its gaps: the copies overlap into one band at least a pixel wide for lines from
 * about a third of a pixel (half-pixel shifts left slivers between the copies); the viewports are set back afterwards */
typedef void (STDMETHODCALLTYPE* RSSetVPs_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
static const float k_line_off[8][2] = {{0.34f, 0}, {0, 0.34f}, {-0.34f, 0}, {0, -0.34f},  /* redraw: continuous */
                                       {0.68f, 0}, {0, 0.68f}, {-0.68f, 0}, {0, -0.68f}}; /* thick: about 2 px */
#define LINE_REDRAW(c, call)                                                                                       \
    do {                                                                                                           \
        int n_ = g_cfg.q_fx_active ? effects_redraws(c) : 0;                                                       \
        if (n_) {                                                                                                  \
            D3D11_VIEWPORT vp_[16], sh_[16];                                                                       \
            UINT nv_ = 16;                                                                                         \
            ID3D11DeviceContext_RSGetViewports(c, &nv_, vp_);                                                      \
            for (int k_ = 0; k_ < n_ && k_ < 8 && nv_; k_++) {                                                     \
                for (UINT v_ = 0; v_ < nv_; v_++)                                                                  \
                    sh_[v_] = vp_[v_], sh_[v_].TopLeftX += k_line_off[k_][0], sh_[v_].TopLeftY += k_line_off[k_][1]; \
                O(RSSetVPs_t, c, SLOT(RSSetViewports))(c, nv_, sh_);                                               \
                call;                                                                                              \
            }                                                                                                      \
            if (nv_)                                                                                               \
                O(RSSetVPs_t, c, SLOT(RSSetViewports))(c, nv_, vp_);                                               \
        }                                                                                                          \
    } while (0)

static void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    if (g_cfg.q_fx_active && effects_draw(c, count)) { /* [Quality] a post-processing effect switched off */
        KEEP_HOOKED(c);
        return;
    }
    if (g_cfg.gpu_probe || g_cfg.gpu_timing || g_cfg.gpu_passes)
        count_draw(c, count, start, base, __builtin_return_address(0));
    q_draw(c, count);
    oc_draw(c, count);
    census_q(c, count, 1, start, base);
    FmPick fp;
    if (fm_pick(c, count, start, base, &fp)) { /* the simpler version, then the game's index buffer back */
        O(IASetIB_t, c, SLOT(IASetIndexBuffer))(c, (ID3D11Buffer*)fp.lod, (DXGI_FORMAT)fp.fmt, 0);
        O(DrawIndexed_t, c, SLOT(DrawIndexed))(c, fp.count, fp.start, base);
        O(IASetIB_t, c, SLOT(IASetIndexBuffer))(c, (ID3D11Buffer*)fp.ib, (DXGI_FORMAT)fp.fmt, fp.off);
        farmesh_done(&fp);
        KEEP_HOOKED(c);
        return;
    }
    if (g_cfg.capture)
        capture_draw(c, count, 0);
    if (g_cfg.gpu_passes)
        pa_draw(ctx_of(c), count, 1, 1);
    O(DrawIndexed_t, c, SLOT(DrawIndexed))(c, count, start, base);
    LINE_REDRAW(c, O(DrawIndexed_t, c, SLOT(DrawIndexed))(c, count, start, base));
    if (g_cfg.q_mv_hooks)
        motion_draw(c, count, 1, start, base);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* c, UINT count, UINT start)
{
    if (g_cfg.q_fx_active && effects_draw(c, count)) { /* [Quality] a post-processing effect switched off */
        KEEP_HOOKED(c);
        return;
    }
    if (g_cfg.gpu_probe || g_cfg.gpu_timing || g_cfg.gpu_passes)
        count_draw(c, count, start, 0, __builtin_return_address(0));
    q_draw(c, count);
    oc_draw(c, count);
    if (g_cfg.capture)
        capture_draw(c, count, 1);
    if (g_cfg.gpu_passes)
        pa_draw(ctx_of(c), count, 1, 0);
    O(Draw_t, c, SLOT(Draw))(c, count, start);
    LINE_REDRAW(c, O(Draw_t, c, SLOT(Draw))(c, count, start));
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_DrawIndexedInst(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, INT base,
                                                 UINT sinst)
{
    if (g_cfg.q_fx_active && effects_draw(c, count)) { /* [Quality] a post-processing effect switched off */
        KEEP_HOOKED(c);
        return;
    }
    if (g_cfg.gpu_probe || g_cfg.gpu_timing || g_cfg.gpu_passes)
        count_draw(c, count * (inst ? inst : 1), start, base, __builtin_return_address(0));
    q_draw(c, count * (inst ? inst : 1));
    oc_draw(c, count * (inst ? inst : 1));
    census_q(c, count, inst, start, base);
    if (g_cfg.capture)
        capture_draw(c, count * (inst ? inst : 1), 0);
    if (g_cfg.gpu_passes)
        pa_draw(ctx_of(c), count, inst, 1);
    O(DrawIndexedInst_t, c, SLOT(DrawIndexedInstanced))(c, count, inst, start, base, sinst);
    LINE_REDRAW(c, O(DrawIndexedInst_t, c, SLOT(DrawIndexedInstanced))(c, count, inst, start, base, sinst));
    if (g_cfg.q_mv_hooks)
        motion_draw(c, count, inst, start, base);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_DrawInst(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start, UINT sinst)
{
    if (g_cfg.q_fx_active && effects_draw(c, count)) { /* [Quality] a post-processing effect switched off */
        KEEP_HOOKED(c);
        return;
    }
    if (g_cfg.gpu_probe || g_cfg.gpu_timing || g_cfg.gpu_passes)
        count_draw(c, count * (inst ? inst : 1), start, 0, __builtin_return_address(0));
    q_draw(c, count * (inst ? inst : 1));
    oc_draw(c, count * (inst ? inst : 1));
    if (g_cfg.capture)
        capture_draw(c, count, 1);
    if (g_cfg.gpu_passes)
        pa_draw(ctx_of(c), count, inst, 0);
    O(DrawInst_t, c, SLOT(DrawInstanced))(c, count, inst, start, sinst);
    LINE_REDRAW(c, O(DrawInst_t, c, SLOT(DrawInstanced))(c, count, inst, start, sinst));
    KEEP_HOOKED(c);
}

static HRESULT STDMETHODCALLTYPE hk_FinishCL(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** cl)
{
    if (g_cfg.q_mv_hooks)
        motion_flush(c);
    Ctx* x = ctx_of(c);
    flush_pass(x);
    if (g_cfg.gpu_passes)
        pa_reset_state(x);
    if (g_cfg.gpu_timing) {
        pass_done(x);
        ts_mark(c, x, KEY(K_ENDLIST, 0, 0, 0, 0, 0));
        memset(x->bound, 0, sizeof(x->bound)); /* a new list starts with nothing bound */
        if (!restore)
            x->ps_id = 0; /* nor a pixel shader */
    }
    InterlockedIncrement64(&t_finish);
    if (g_q_far_hooks && !restore)
        q_forget(x, c); /* the deferred context starts the next list with default state */
    if (g_cfg.capture)
        capture_end_pass(c);
    if (g_cfg.q_fx_active && !restore)
        effects_reset(c);
    HRESULT hr = O(FinishCL_t, c, SLOT(FinishCommandList))(c, restore, cl);
    if (g_cfg.capture && SUCCEEDED(hr) && cl)
        capture_finish_cl(c, *cl);
    KEEP_HOOKED(c);
    return hr;
}

/* Wait profile: time the main thread spends inside D3D calls (DXVK on Android), by method
 * and game call site. Other threads pass straight through. */
#define TIMED(method, call)                                                                                    \
    do {                                                                                                       \
        if (g_cfg.wait_profile && GetCurrentThreadId() == threads_main_tid()) {                                \
            void* ret_ = __builtin_return_address(0);                                                          \
            uint64_t t0_ = qpc_now();                                                                          \
            call;                                                                                              \
            waitprof_add_d3d((method), ret_, qpc_now() - t0_);                                                 \
        } else {                                                                                               \
            call;                                                                                              \
        }                                                                                                      \
    } while (0)

static void STDMETHODCALLTYPE hk_ExecCL(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore)
{
    InterlockedIncrement64(&t_execute);
    if (g_cfg.capture)
        capture_exec_cl(c, cl, restore);
    if (g_cfg.q_fx_active && !restore)
        effects_reset(c);
    if (g_cfg.gpu_passes)
        pa_close(ctx_of(c)); /* the list starts with its own bindings */
    TIMED(D3D_EXEC, O(ExecCL_t, c, SLOT(ExecuteCommandList))(c, cl, restore));
    if (g_cfg.gpu_passes && !restore)
        pa_reset_state(ctx_of(c));
    if (g_q_far_hooks && !restore)
        q_forget(ctx_of(c), c);
    if (g_cfg.gpu_timing && !restore)
        ctx_of(c)->ps_id = 0; /* the context's state is the default after the list */
    KEEP_HOOKED(c);
}

typedef HRESULT (STDMETHODCALLTYPE* Map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                           D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE* Unmap_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void (STDMETHODCALLTYPE* UpdateSub_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                              const void*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* CopyRes_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE* CopySubRegion_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                  ID3D11Resource*, UINT, const D3D11_BOX*);
typedef HRESULT (STDMETHODCALLTYPE* GetData_t)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* Flush_t)(ID3D11DeviceContext*);
typedef void (STDMETHODCALLTYPE* End_t)(ID3D11DeviceContext*, ID3D11Asynchronous*);

static HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type,
                                       UINT flags, D3D11_MAPPED_SUBRESOURCE* out)
{
    HRESULT hr;
    int read = type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE;
    TIMED(read ? D3D_MAP_READ : D3D_MAP_WRITE, hr = O(Map_t, c, SLOT(Map))(c, r, sub, type, flags, out));
    KEEP_HOOKED(c);
    return hr;
}

static void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
    TIMED(D3D_UNMAP, O(Unmap_t, c, SLOT(Unmap))(c, r, sub));
    KEEP_HOOKED(c);
}

/* DXVK replaces a whole buffer's storage instead of copying into it (no render pass split);
 * textures and partial buffer updates are copies, outside any render pass */
static void pa_update(ID3D11DeviceContext* c, ID3D11Resource* r, const D3D11_BOX* box)
{
    if (!g_cfg.gpu_passes || !r)
        return;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC bd;
        ID3D11Buffer_GetDesc((ID3D11Buffer*)r, &bd);
        if (!box || (box->left == 0 && box->right >= bd.ByteWidth))
            return;
    }
    pa_spill(c, SP_UPDATE);
}

static void STDMETHODCALLTYPE hk_UpdateSub(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                           const void* data, UINT rp, UINT dp)
{
    pa_update(c, r, box);
    TIMED(D3D_UPDATE, O(UpdateSub_t, c, SLOT(UpdateSubresource))(c, r, sub, box, data, rp, dp));
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_CopyRes(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s)
{
    pa_spill(c, SP_COPY);
    TIMED(D3D_COPY, O(CopyRes_t, c, SLOT(CopyResource))(c, d, s));
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_CopySubRegion(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y,
                                               UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* box)
{
    pa_spill(c, SP_COPY);
    TIMED(D3D_COPY, O(CopySubRegion_t, c, SLOT(CopySubresourceRegion))(c, d, ds, x, y, z, s, ss, box));
    KEEP_HOOKED(c);
}

static HRESULT STDMETHODCALLTYPE hk_GetData(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* data, UINT size,
                                           UINT flags)
{
    HRESULT hr;
    TIMED(D3D_GETDATA, hr = O(GetData_t, c, SLOT(GetData))(c, a, data, size, flags));
    KEEP_HOOKED(c);
    return hr;
}

static void STDMETHODCALLTYPE hk_Flush(ID3D11DeviceContext* c)
{
    TIMED(D3D_FLUSH, O(Flush_t, c, SLOT(Flush))(c));
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_End(ID3D11DeviceContext* c, ID3D11Asynchronous* a)
{
    TIMED(D3D_END, O(End_t, c, SLOT(End))(c, a));
    KEEP_HOOKED(c);
}

typedef void (STDMETHODCALLTYPE* OMSetBlend_t)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
typedef void (STDMETHODCALLTYPE* OMSetDS_t)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
typedef void (STDMETHODCALLTYPE* RSSetVP_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void (STDMETHODCALLTYPE* ClearUAVu_t)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT[4]);
typedef void (STDMETHODCALLTYPE* ClearUAVf_t)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT[4]);
typedef void (STDMETHODCALLTYPE* GenMips_t)(ID3D11DeviceContext*, ID3D11ShaderResourceView*);
typedef void (STDMETHODCALLTYPE* Resolve_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT,
                                            DXGI_FORMAT);
typedef void (STDMETHODCALLTYPE* Discard_t)(ID3D11DeviceContext1*, ID3D11View*);
typedef void (STDMETHODCALLTYPE* DiscardRes_t)(ID3D11DeviceContext1*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE* CopySub1_t)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                             ID3D11Resource*, UINT, const D3D11_BOX*, UINT);
typedef void (STDMETHODCALLTYPE* UpdateSub1_t)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                               const void*, UINT, UINT, UINT);

static void STDMETHODCALLTYPE hk_OMSetBlend(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m)
{
    Ctx* x = ctx_of(c);
    x->pa_blend = 0;
    if (b) {
        D3D11_BLEND_DESC d;
        ID3D11BlendState_GetDesc(b, &d);
        for (int i = 0; i < (d.IndependentBlendEnable ? 8 : 1); i++)
            if (d.RenderTarget[i].BlendEnable || (d.RenderTarget[i].RenderTargetWriteMask & 0xF) != 0xF)
                x->pa_blend = 1; /* blending or partial writes keep what is there */
    }
    O(OMSetBlend_t, c, SLOT(OMSetBlendState))(c, b, f, m);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_OMSetDS(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT ref)
{
    Ctx* x = ctx_of(c);
    x->pa_ds_on = 1; /* default state: depth test on */
    if (s) {
        D3D11_DEPTH_STENCIL_DESC d;
        ID3D11DepthStencilState_GetDesc(s, &d);
        x->pa_ds_on = d.DepthEnable || d.StencilEnable;
    }
    O(OMSetDS_t, c, SLOT(OMSetDepthStencilState))(c, s, ref);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_RSSetVP(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v)
{
    Ctx* x = ctx_of(c);
    if (n && v)
        x->pa_vp[0] = v->TopLeftX, x->pa_vp[1] = v->TopLeftY, x->pa_vp[2] = v->Width, x->pa_vp[3] = v->Height,
        x->pa_vp_set = 1;
    O(RSSetVP_t, c, SLOT(RSSetViewports))(c, n, v);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_ClearUAVu(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* u, const UINT v[4])
{
    pa_spill(c, SP_CLEARUAV);
    O(ClearUAVu_t, c, SLOT(ClearUnorderedAccessViewUint))(c, u, v);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_ClearUAVf(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* u, const FLOAT v[4])
{
    pa_spill(c, SP_CLEARUAV);
    O(ClearUAVf_t, c, SLOT(ClearUnorderedAccessViewFloat))(c, u, v);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_GenMips(ID3D11DeviceContext* c, ID3D11ShaderResourceView* v)
{
    pa_spill(c, SP_OTHER);
    O(GenMips_t, c, SLOT(GenerateMips))(c, v);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_Resolve(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss,
                                         DXGI_FORMAT f)
{
    pa_spill(c, SP_OTHER);
    O(Resolve_t, c, SLOT(ResolveSubresource))(c, d, ds, s, ss, f);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_DiscardView(ID3D11DeviceContext1* c, ID3D11View* v)
{
    if (g_cfg.gpu_passes && v)
        pa_clear(ctx_of((ID3D11DeviceContext*)c), v, 1);
    O(Discard_t, c, SLOT1(DiscardView))(c, v);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_DiscardRes(ID3D11DeviceContext1* c, ID3D11Resource* r)
{
    O(DiscardRes_t, c, SLOT1(DiscardResource))(c, r);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_CopySub1(ID3D11DeviceContext1* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z,
                                          ID3D11Resource* s, UINT ss, const D3D11_BOX* box, UINT flags)
{
    pa_spill((ID3D11DeviceContext*)c, SP_COPY);
    O(CopySub1_t, c, SLOT1(CopySubresourceRegion1))(c, d, ds, x, y, z, s, ss, box, flags);
    KEEP_HOOKED(c);
}

static void STDMETHODCALLTYPE hk_UpdateSub1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                            const void* data, UINT rp, UINT dp, UINT flags)
{
    pa_update((ID3D11DeviceContext*)c, r, box);
    O(UpdateSub1_t, c, SLOT1(UpdateSubresource1))(c, r, sub, box, data, rp, dp, flags);
    KEEP_HOOKED(c);
}

typedef struct HookSlot {
    size_t slot;
    void* hook;
} HookSlot;
static const HookSlot g_probe_hooks[] = {
    {SLOT(OMSetRenderTargets), (void*)hk_OMSetRT},
    {SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void*)hk_OMSetRTUAV},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(IASetIndexBuffer), (void*)hk_IASetIB},
    {SLOT(IASetVertexBuffers), (void*)hk_IASetVB},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
};
static const HookSlot g_timing_hooks[] = {
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(Map), (void*)hk_Map},
    {SLOT(Unmap), (void*)hk_Unmap},
    {SLOT(UpdateSubresource), (void*)hk_UpdateSub},
    {SLOT(CopyResource), (void*)hk_CopyRes},
    {SLOT(CopySubresourceRegion), (void*)hk_CopySubRegion},
    {SLOT(GetData), (void*)hk_GetData},
    {SLOT(Flush), (void*)hk_Flush},
    {SLOT(End), (void*)hk_End},
};
static const HookSlot g_pass_timing_hooks[] = {
    {SLOT(OMSetRenderTargets), (void*)hk_OMSetRT},
    {SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void*)hk_OMSetRTUAV},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(Dispatch), (void*)hk_Dispatch},
    {SLOT(DispatchIndirect), (void*)hk_DispatchInd},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(ClearRenderTargetView), (void*)hk_ClearRTV},
    {SLOT(ClearDepthStencilView), (void*)hk_ClearDSV},
    {SLOT(PSSetShader), (void*)hk_PSSetShader},
    {SLOT(ClearState), (void*)hk_ClearState},
};
static const HookSlot g_quality_hooks[] = {
    {SLOT(PSSetSamplers), (void*)hk_PSSetSamp},
};
static const HookSlot g_capture_hooks[] = {
    {SLOT(OMSetRenderTargets), (void*)hk_OMSetRT},
    {SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void*)hk_OMSetRTUAV},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(ClearState), (void*)hk_ClearState},
    {SLOT(PSSetShaderResources), (void*)hk_PSSetSRV},
    {SLOT(PSSetShader), (void*)hk_PSSetShader},
};
static const HookSlot g_effects_hooks[] = {
    {SLOT(PSSetShader), (void*)hk_PSSetShader},
    {SLOT(VSSetShader), (void*)hk_VSSetShader},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(ClearState), (void*)hk_ClearState},
};
/* [GpuProbe] ComputeCensus: the dispatches */
static const HookSlot g_compute_hooks[] = {
    {SLOT(Dispatch), (void*)hk_Dispatch},
    {SLOT(DispatchIndirect), (void*)hk_DispatchInd},
};
/* [Quality] CharacterMotionVectors: the targets (the characters' pass), its indexed draws, the end of a list */
static const HookSlot g_motion_hooks[] = {
    {SLOT(OMSetRenderTargets), (void*)hk_OMSetRT},
    {SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void*)hk_OMSetRTUAV},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(ClearState), (void*)hk_ClearState},
};
static const HookSlot g_quality_far_hooks[] = {
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(ClearState), (void*)hk_ClearState},
};
/* occlusion census: the draws, and the target binds (the pass size of each object) */
static const HookSlot g_occl_hooks[] = {
    {SLOT(OMSetRenderTargets), (void*)hk_OMSetRT},
    {SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void*)hk_OMSetRTUAV},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
};
static const HookSlot g_pass_audit_hooks[] = {
    {SLOT(OMSetRenderTargets), (void*)hk_OMSetRT},
    {SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void*)hk_OMSetRTUAV},
    {SLOT(FinishCommandList), (void*)hk_FinishCL},
    {SLOT(ExecuteCommandList), (void*)hk_ExecCL},
    {SLOT(Dispatch), (void*)hk_Dispatch},
    {SLOT(DispatchIndirect), (void*)hk_DispatchInd},
    {SLOT(DrawIndexed), (void*)hk_DrawIndexed},
    {SLOT(Draw), (void*)hk_Draw},
    {SLOT(DrawIndexedInstanced), (void*)hk_DrawIndexedInst},
    {SLOT(DrawInstanced), (void*)hk_DrawInst},
    {SLOT(ClearRenderTargetView), (void*)hk_ClearRTV},
    {SLOT(ClearDepthStencilView), (void*)hk_ClearDSV},
    {SLOT(CopyResource), (void*)hk_CopyRes},
    {SLOT(CopySubresourceRegion), (void*)hk_CopySubRegion},
    {SLOT(UpdateSubresource), (void*)hk_UpdateSub},
    {SLOT(OMSetBlendState), (void*)hk_OMSetBlend},
    {SLOT(OMSetDepthStencilState), (void*)hk_OMSetDS},
    {SLOT(RSSetViewports), (void*)hk_RSSetVP},
    {SLOT(ClearUnorderedAccessViewUint), (void*)hk_ClearUAVu},
    {SLOT(ClearUnorderedAccessViewFloat), (void*)hk_ClearUAVf},
    {SLOT(GenerateMips), (void*)hk_GenMips},
    {SLOT(ResolveSubresource), (void*)hk_Resolve},
    {SLOT1(DiscardView), (void*)hk_DiscardView},
    {SLOT1(DiscardResource), (void*)hk_DiscardRes},
    {SLOT1(CopySubresourceRegion1), (void*)hk_CopySub1},
    {SLOT1(UpdateSubresource1), (void*)hk_UpdateSub1},
};
/* active set: probe hooks ([GpuProbe]), pass timing ([GpuProbe] Timing), pass audit
 * ([GpuProbe] Passes) and/or D3D call timing ([WaitProfile]) */
static HookSlot g_hooks[48];
static unsigned g_nhooks;
#define NHOOKS g_nhooks

static void add_hooks(const HookSlot* h, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned j = 0;
        while (j < g_nhooks && g_hooks[j].slot != h[i].slot)
            j++;
        if (j == g_nhooks && g_nhooks < sizeof(g_hooks) / sizeof(g_hooks[0]))
            g_hooks[g_nhooks++] = h[i];
    }
}

/* (Re)patch every hooked entry of `h` that is not ours; whatever is there is the new
 * original. Cheap when nothing changed (10 pointer compares). */
static int repatch(HookedVt* h)
{
    int n = 0;
    for (unsigned i = 0; i < NHOOKS; i++) {
        if (g_hooks[i].slot >= NSLOTS0 && !h->dc1)
            continue;
        void* cur = h->vt[g_hooks[i].slot];
        if (cur == g_hooks[i].hook)
            continue;
        h->orig[g_hooks[i].slot] = cur;
        void* p = g_hooks[i].hook;
        if (h->writable)
            InterlockedExchangePointer(&h->vt[g_hooks[i].slot], p);
        else
            patch_bytes(&h->vt[g_hooks[i].slot], &p, sizeof(p));
        n++;
    }
    return n;
}

static int is_writable(void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return 0;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
}

static void hook_context_vtable(void** vt, const char* what, int dc1)
{
    AcquireSRWLockExclusive(&g_vt_lock);
    for (LONG i = 0; i < g_nvt; i++)
        if (g_vts[i].vt == vt) {
            ReleaseSRWLockExclusive(&g_vt_lock);
            return;
        }
    if (g_nvt < MAX_VT) {
        HookedVt* h = &g_vts[g_nvt];
        h->vt = vt;
        h->writable = is_writable(vt);
        h->dc1 = dc1;
        h->deferred = strcmp(what, "deferred") == 0;
        memcpy(h->orig, vt, (dc1 ? NSLOTS : NSLOTS0) * sizeof(void*));
        /* entry already holding our hook (a table copied from one we patched): the real original
         * comes from a table of the same kind (immediate and deferred originals differ) */
        for (unsigned i = 0; i < NHOOKS; i++)
            if ((g_hooks[i].slot < NSLOTS0 || dc1) && vt[g_hooks[i].slot] == g_hooks[i].hook)
                for (LONG j = 0; j < g_nvt; j++)
                    if (g_vts[j].deferred == h->deferred) {
                        h->orig[g_hooks[i].slot] = g_vts[j].orig[g_hooks[i].slot];
                        break;
                    }
        repatch(h);
        InterlockedIncrement(&g_nvt);
        char at[64];
        LOGI("GPU probe: hooked %s context method table #%ld (DrawIndexed original %s)", what, g_nvt,
             fmt_addr(h->orig[SLOT(DrawIndexed)], at, sizeof(at)));
    } else {
        static int warned;
        if (!warned++)
            LOGI("GPU probe: WARNING - more than %d context method tables; further contexts are not hooked", MAX_VT);
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
}

/* ---------------------------------------------------------------- device hooks */
static int is_dc1(void* ctx)
{
    void* t = NULL;
    if (FAILED(IUnknown_QueryInterface((IUnknown*)ctx, &IID_ID3D11DeviceContext1, &t)) || !t)
        return 0;
    IUnknown_Release((IUnknown*)t);
    return 1;
}

static HRESULT create_dc(int which, void* dev, UINT flags, void** out)
{
    HRESULT hr = ((CreateDC_t)g_dev_orig[which])(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out)
        hook_context_vtable(*(void***)*out, "deferred", is_dc1(*out));
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateDC0(void* d, UINT f, void** o) { return create_dc(0, d, f, o); }
static HRESULT STDMETHODCALLTYPE hk_CreateDC1(void* d, UINT f, void** o) { return create_dc(1, d, f, o); }
static HRESULT STDMETHODCALLTYPE hk_CreateDC2(void* d, UINT f, void** o) { return create_dc(2, d, f, o); }
static HRESULT STDMETHODCALLTYPE hk_CreateDC3(void* d, UINT f, void** o) { return create_dc(3, d, f, o); }

typedef HRESULT (STDMETHODCALLTYPE* CreatePS_t)(void*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
typedef HRESULT (STDMETHODCALLTYPE* CreateVS_t)(void*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
static void *g_dev_create_ps, *g_dev_create_vs;
static HRESULT STDMETHODCALLTYPE hk_CreatePS(void* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl,
                                             ID3D11PixelShader** ps)
{
    HRESULT hr = ((CreatePS_t)g_dev_create_ps)(d, code, len, cl, ps);
    if (SUCCEEDED(hr) && ps && *ps) {
        capture_shader_created(*ps, code, len);
        effects_shader_created(d, 0, *ps, code, len, g_dev_create_ps);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateVS(void* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl,
                                             ID3D11VertexShader** vs)
{
    HRESULT hr = ((CreateVS_t)g_dev_create_vs)(d, code, len, cl, vs);
    if (SUCCEEDED(hr) && vs && *vs)
        effects_shader_created(d, 1, *vs, code, len, g_dev_create_vs);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE* CreateBuffer_t)(void*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*,
                                                    ID3D11Buffer**);
typedef HRESULT (STDMETHODCALLTYPE* CreateIL_t)(void*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T,
                                                ID3D11InputLayout**);
static void *g_dev_create_buf, *g_dev_create_il;
static ID3D11DeviceContext* g_census_imm;
static HRESULT STDMETHODCALLTYPE hk_CreateBuffer(void* d, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
                                                 ID3D11Buffer** b)
{
    HRESULT hr = ((CreateBuffer_t)g_dev_create_buf)(d, desc, init, b);
    if (SUCCEEDED(hr) && b && *b)
        census_buffer_created(*b, desc, init != NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateIL(void* d, const D3D11_INPUT_ELEMENT_DESC* e, UINT n, const void* code,
                                             SIZE_T len, ID3D11InputLayout** il)
{
    HRESULT hr = ((CreateIL_t)g_dev_create_il)(d, e, n, code, len, il);
    if (SUCCEEDED(hr) && il && *il)
        census_layout_created(*il, e, n);
    return hr;
}

static void hook_device(void* dev)
{
    void** vt = *(void***)dev;
    if (g_dev_vt == vt)
        return;
    if (g_dev_vt) {
        LOGI("GPU probe: a second device class appeared; deferred contexts of it are not tracked");
        return;
    }
    /* only patch the CreateDeferredContext1..3 slots if this device really is a Device1..3 */
    int levels = 1;
    void* tmp = NULL;
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown*)dev, &IID_ID3D11Device3, &tmp)) && tmp) {
        levels = 4;
        IUnknown_Release((IUnknown*)tmp);
    } else if (SUCCEEDED(IUnknown_QueryInterface((IUnknown*)dev, &IID_ID3D11Device2, &tmp)) && tmp) {
        levels = 3;
        IUnknown_Release((IUnknown*)tmp);
    } else if (SUCCEEDED(IUnknown_QueryInterface((IUnknown*)dev, &IID_ID3D11Device1, &tmp)) && tmp) {
        levels = 2;
        IUnknown_Release((IUnknown*)tmp);
    }
    void* hooks[4] = {(void*)hk_CreateDC0, (void*)hk_CreateDC1, (void*)hk_CreateDC2, (void*)hk_CreateDC3};
    g_dev_vt = vt;
    for (int i = 0; i < levels; i++) {
        g_dev_orig[i] = vt[g_dev_slots[i]];
        patch_bytes(&vt[g_dev_slots[i]], &hooks[i], sizeof(void*));
    }
    if (g_cfg.capture || g_cfg.q_fx_active || g_cfg.gpu_timing) { /* pixel shader bytecode: capture, effects, timing */
        void* h = (void*)hk_CreatePS;
        g_dev_create_ps = vt[DSLOT(CreatePixelShader)];
        patch_bytes(&vt[DSLOT(CreatePixelShader)], &h, sizeof(void*));
    }
    if (g_cfg.q_fx_active) { /* the SMAA vertex shader (orthogonal search) */
        void* h = (void*)hk_CreateVS;
        g_dev_create_vs = vt[DSLOT(CreateVertexShader)];
        patch_bytes(&vt[DSLOT(CreateVertexShader)], &h, sizeof(void*));
    }
    if (g_cfg.mesh_census) { /* buffer kinds of the meshes */
        void* h = (void*)hk_CreateBuffer;
        g_dev_create_buf = vt[DSLOT(CreateBuffer)];
        patch_bytes(&vt[DSLOT(CreateBuffer)], &h, sizeof(void*));
    }
    if (g_cfg.compute_census) { /* the compute shaders' bytecode: their names */
        void* h = (void*)hk_CreateCS;
        g_dev_create_cs = vt[DSLOT(CreateComputeShader)];
        patch_bytes(&vt[DSLOT(CreateComputeShader)], &h, sizeof(void*));
    }
    if (g_cfg.mesh_census || g_cfg.q_farmesh || g_cfg.q_fm_both || g_cfg.q_mv_hooks) { /* vertex formats of the meshes */
        void* h = (void*)hk_CreateIL;
        g_dev_create_il = vt[DSLOT(CreateInputLayout)];
        patch_bytes(&vt[DSLOT(CreateInputLayout)], &h, sizeof(void*));
    }
    LOGI("GPU probe: hooked deferred-context creation on the device (%d interface levels)", levels);
}

/* From the device-creation hook. Called for every device the game creates. */
void gpuprobe_on_device(void* dev, void* ctx)
{
    if (!g_cfg.gpu_probe && !g_cfg.wait_profile && !g_cfg.gpu_timing && !g_cfg.gpu_passes && !g_cfg.q_active &&
        !g_cfg.capture && !g_cfg.compare_shots)
        return;
    if (!g_active) {
        if (g_cfg.gpu_probe)
            add_hooks(g_probe_hooks, sizeof(g_probe_hooks) / sizeof(g_probe_hooks[0]));
        if (g_cfg.gpu_passes)
            add_hooks(g_pass_audit_hooks, sizeof(g_pass_audit_hooks) / sizeof(g_pass_audit_hooks[0]));
        if (g_cfg.q_active)
            add_hooks(g_quality_hooks, sizeof(g_quality_hooks) / sizeof(g_quality_hooks[0]));
        if (g_cfg.q_active)
            g_tls_q = TlsAlloc();
        if (g_cfg.q_farmesh || g_cfg.q_fm_both)
            farmesh_init();
        /* draw hooks: far textures, the triangle accounting, and the cycles that switch sampler keys */
        if (g_cfg.q_active && g_tls_q != TLS_OUT_OF_INDEXES &&
            (g_cfg.q_far_mip || g_cfg.q_char_tex_q || g_cfg.q_world_tex_q || g_cfg.compare_shots == 7 ||
             g_cfg.compare_shots == 12 || g_cfg.q_report || g_cfg.q_farmesh || g_cfg.q_fm_both ||
             (g_cfg.ab_enabled &&
              (g_cfg.ab_compare == AB_FAR || g_cfg.ab_compare == AB_GPU || g_cfg.ab_compare == AB_SAVINGS)))) {
            add_hooks(g_quality_far_hooks, sizeof(g_quality_far_hooks) / sizeof(g_quality_far_hooks[0]));
            g_q_far_hooks = 1;
        }
        if (g_cfg.occl_hooks) {
            add_hooks(g_occl_hooks, sizeof(g_occl_hooks) / sizeof(g_occl_hooks[0]));
            LOGI("Occlusion census: on (measures only: an occlusion query around every scene object; F9 marks a "
                 "place in the log)");
        }
        if (g_cfg.gpu_timing)
            add_hooks(g_pass_timing_hooks, sizeof(g_pass_timing_hooks) / sizeof(g_pass_timing_hooks[0]));
        if (g_cfg.shader_timing && g_is_wine)
            LOGI("GPU timing: ShaderTiming is for the PC. On a tiled GPU (DXVK on Android) a timestamp inside a pass "
                 "splits its render pass and the times mean little; use [ABTest] Compare=knockout there");
        if (g_cfg.capture)
            add_hooks(g_capture_hooks, sizeof(g_capture_hooks) / sizeof(g_capture_hooks[0]));
        if (g_cfg.q_fx_active)
            add_hooks(g_effects_hooks, sizeof(g_effects_hooks) / sizeof(g_effects_hooks[0]));
        if (g_cfg.q_mv_hooks)
            add_hooks(g_motion_hooks, sizeof(g_motion_hooks) / sizeof(g_motion_hooks[0]));
        if (g_cfg.compute_census)
            add_hooks(g_compute_hooks, sizeof(g_compute_hooks) / sizeof(g_compute_hooks[0]));
        if (g_cfg.wait_profile)
            add_hooks(g_timing_hooks, sizeof(g_timing_hooks) / sizeof(g_timing_hooks[0]));
        uint8_t* text;
        size_t len;
        if (sig_text_range(game_base(), &text, &len)) {
            g_text = text;
            g_text_end = text + len;
        }
        if (g_cfg.shadow_verify)
            g_tls_ctx = TlsAlloc();
        g_active = 1;
    }
    if (dev && (g_cfg.gpu_probe || g_cfg.gpu_timing || g_cfg.gpu_passes || g_cfg.q_active || g_cfg.capture ||
                g_cfg.compare_shots))
        hook_device(dev);
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    if (!c && dev)
        ID3D11Device_GetImmediateContext((ID3D11Device*)dev, &c);
    else if (c)
        ID3D11DeviceContext_AddRef(c);
    if (c) {
        hook_context_vtable(*(void***)c, "immediate", is_dc1(c));
        if (g_cfg.mesh_census || g_cfg.q_farmesh || g_cfg.q_fm_both) { /* meshes are read back through it */
            ID3D11DeviceContext_AddRef(c);
            if (g_census_imm)
                ID3D11DeviceContext_Release(g_census_imm);
            g_census_imm = c;
        }
        if (g_cfg.gpu_timing) { /* keep the newest device's immediate context for frame marks */
            if (g_timm)
                ID3D11DeviceContext_Release(g_timm);
            g_timm = c;
            if (g_tdev)
                ID3D11Device_Release(g_tdev);
            g_tdev = NULL;
            ID3D11DeviceContext_GetDevice(c, &g_tdev);
        } else {
            ID3D11DeviceContext_Release(c);
        }
    }
}

/* ---------------------------------------------------------------- GPU timing: frame end + readback */
/* Before Present: the end-of-frame mark must reach the GPU with this frame's work. */
void gpuprobe_before_present(void)
{
    if (!g_active || !g_cfg.gpu_timing || !g_timm || !g_tdev)
        return;
    ts_mark(g_timm, NULL, KEY(K_ENDFRAME, 0, 0, 0, 0, 0));
    for (LONG i = 0; i < g_nctx; i++) /* the next frame's first pass gets its mark even on the same targets */
        if (!g_ctx[i].deferred)
            memset(g_ctx[i].bound, 0, sizeof(g_ctx[i].bound)), g_ctx[i].work_marked = 0;
}

static void ts_frame_end(void)
{
    if (!g_timm || !g_tdev)
        return;

    /* timer frequency: a disjoint query spanning ~30 frames, repeated */
    if (!g_disjoint) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(ID3D11Device_CreateQuery(g_tdev, &d, &g_disjoint)))
            g_disjoint = NULL;
    }
    if (g_disjoint) {
        if (g_disjoint_state == 0) {
            ID3D11DeviceContext_Begin(g_timm, (ID3D11Asynchronous*)g_disjoint);
            g_disjoint_state = 1, g_disjoint_frame = g_tframes;
        } else if (g_disjoint_state == 1 && g_tframes - g_disjoint_frame >= 30) {
            ID3D11DeviceContext_End(g_timm, (ID3D11Asynchronous*)g_disjoint);
            g_disjoint_state = 2;
        } else if (g_disjoint_state == 2) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
            HRESULT hr = ID3D11DeviceContext_GetData(g_timm, (ID3D11Asynchronous*)g_disjoint, &dj, sizeof(dj),
                                                     D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK) {
                if (dj.Disjoint)
                    InterlockedIncrement64(&t_ts_disjoint);
                else if (dj.Frequency)
                    g_ts_freq = dj.Frequency;
                g_disjoint_state = 0;
            } else if (hr != S_FALSE) {
                g_disjoint_state = 0;
            }
        }
    }

    /* resolved timestamps, in the order they were written */
    for (int budget = 4096; budget > 0 && g_ttail < g_thead; budget--) {
        TSlot* s = &g_ts[g_ttail % TRING];
        LONG st = s->state;
        if (st == 0)
            break; /* claimed by a recording thread, End not issued yet */
        if (st == 2) {
            UINT64 v = 0;
            HRESULT hr = ID3D11DeviceContext_GetData(g_timm, (ID3D11Asynchronous*)s->q, &v, sizeof(v),
                                                     D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE && s->frame + 60 > g_tframes)
                break; /* not executed / finished yet; a list that never ran gives up after 60 frames */
            if (hr == S_OK) {
                AcquireSRWLockExclusive(&g_tsw_lock);
                if (g_ntsw < TS_WIN)
                    g_tsw[g_ntsw].t = v, g_tsw[g_ntsw].key = s->key, g_tsw[g_ntsw].seq = (uint32_t)g_ttail,
                    g_tsw[g_ntsw].ps = s->ps, g_ntsw++;
                ReleaseSRWLockExclusive(&g_tsw_lock);
            } else {
                InterlockedIncrement64(&t_ts_dropped);
            }
        }
        InterlockedExchange(&s->state, 0);
        InterlockedIncrement64(&g_ttail);
    }
}

static int ts_cmp(const void* a, const void* b)
{
    const TsEntry *p = (const TsEntry*)a, *q = (const TsEntry*)b;
    if (p->t != q->t)
        return p->t < q->t ? -1 : 1;
    return p->seq < q->seq ? -1 : p->seq > q->seq;
}

static const char* fmt_name(int f)
{
    switch (f) {
    case 2: return "RGBA32F";
    case 10: return "RGBA16F";
    case 16: return "RG32F";
    case 24: return "RGB10A2";
    case 26: return "R11G11B10F";
    case 28: return "RGBA8";
    case 29: return "RGBA8s";
    case 34: return "RG16F";
    case 35: return "RG16";
    case 40: return "D32";
    case 41: return "R32F";
    case 45: return "D24S8";
    case 49: return "RG8";
    case 54: return "R16F";
    case 55: return "D16";
    case 56: return "R16";
    case 61: return "R8";
    case 87: return "BGRA8";
    case 91: return "BGRA8s";
    case 20: return "D32S8";
    default: return NULL;
    }
}

static void key_label(uint64_t k, char* out, size_t cap)
{
    if (KIND(k) == K_COMPUTE) {
        int id = (int)(k & 0x3FFF);
        if (id > 0 && id <= g_ncs)
            snprintf(out, cap, "compute CS %016llx", (unsigned long long)g_cs[id - 1].hash);
        else
            snprintf(out, cap, "compute (Dispatch)");
        return;
    }
    int w = (int)(k & 0x3FFF), h = (int)(k >> 14 & 0x3FFF), f = (int)(k >> 28 & 0xFF), nrt = (int)(k >> 36 & 0xF);
    int dsv = (int)(k >> 40 & 1);
    const char* fn = fmt_name(f);
    char fb[16];
    if (!fn)
        snprintf(fb, sizeof(fb), "fmt%d", f), fn = fb;
    int cas = CASCADE_OF(k), n;
    if (nrt == 0 && cas >= 0)
        n = snprintf(out, cap, "%dx%d depth only (%s) = shadow map, cascade %d", w, h, fn, cas);
    else if (nrt == 0)
        n = snprintf(out, cap, "%dx%d depth only (%s)%s", w, h, fn, w == h && w >= 512 ? " = shadow map" : "");
    else
        n = snprintf(out, cap, "%dx%d %dx %s%s", w, h, nrt, fn, dsv ? " + depth" : "");
    const PsInfo* p = fx_ps_info(KEY_PS(k)); /* the pass's first pixel shader */
    if (p && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, ", first PS %016llx", (unsigned long long)p->hash);
}

typedef struct TsAgg {
    uint64_t key;
    int ps;
    double ms;
    LONG64 cnt; /* 0 = empty */
} TsAgg;

static TsAgg* agg_get(TsAgg* t, int cap, uint64_t key, int ps)
{
    unsigned h = (unsigned)(((key ^ (uint64_t)(unsigned)ps * 0x9E3779B97F4A7C15ull) * 0xC2B2AE3D27D4EB4Full) >> 40) &
                 (unsigned)(cap - 1);
    for (int i = 0; i < cap; i++, h = (h + 1) & (unsigned)(cap - 1)) {
        if (!t[h].cnt) {
            t[h].key = key, t[h].ps = ps, t[h].ms = 0;
            return &t[h];
        }
        if (t[h].key == key && t[h].ps == ps)
            return &t[h];
    }
    return NULL;
}

/* the biggest entry not listed yet (listed ones get cnt = -1) */
static TsAgg* agg_best(TsAgg* t, int cap)
{
    TsAgg* b = NULL;
    for (int i = 0; i < cap; i++)
        if (t[i].cnt > 0 && (!b || t[i].ms > b->ms))
            b = &t[i];
    return b;
}

/* ShaderTiming: the bytecode of the shaders that cost the most, written once each */
static void write_shader(const PsInfo* p)
{
    static char dir[MAX_PATH];
    static uint64_t done[256];
    static int ndone;
    if (!p || !p->code || ndone >= 256)
        return;
    for (int i = 0; i < ndone; i++)
        if (done[i] == p->hash)
            return;
    done[ndone++] = p->hash;
    if (!dir[0])
        output_dir(dir, sizeof(dir), "shaders");
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\ps_%016llx.dxbc", dir, (unsigned long long)p->hash);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, p->code, (DWORD)p->len, &wr, NULL);
        CloseHandle(f);
    }
    if (ndone == 1)
        LOGI("  GPU time by pixel shader: the costliest shaders' bytecode goes to %s", dir);
}

/* Profiler window: sort the window's timestamps by GPU time and attribute the gaps. */
static void ts_report(void)
{
    static TsEntry e[TS_WIN];
    AcquireSRWLockExclusive(&g_tsw_lock);
    int n = g_ntsw;
    memcpy(e, g_tsw, (size_t)n * sizeof(TsEntry));
    g_ntsw = 0;
    ReleaseSRWLockExclusive(&g_tsw_lock);
    UINT64 freq = g_ts_freq;
    if (n < 2 || !freq) {
        LOGI("  GPU time per pass: no data yet (%d timestamps, timer %s)", n, freq ? "ok" : "unknown");
        return;
    }
    qsort(e, (size_t)n, sizeof(TsEntry), ts_cmp);
    static int dumped;
    if (!dumped++ && n > 24) { /* once: the raw order, for checking the attribution */
        char line[900];
        int m = 0;
        for (int i = 0; i < 24 && m < (int)sizeof(line) - 40; i++)
            m += snprintf(line + m, sizeof(line) - m, " %c%lld@%.3f", "PCLFB????????????"[KIND(e[i].key)], (long long)e[i].seq,
                          (double)(e[i].t - e[0].t) * 1000.0 / (double)freq);
        LOGI("  GPU timestamps (kind P pass work / C compute / L list end / F frame end / B bind; recording order @ ms):%s", line);
    }
    enum { PK = 512, SK = 4096 };
    static TsAgg pass[PK], shd[SK];
    memset(pass, 0, sizeof(pass));
    if (g_cfg.shader_timing)
        memset(shd, 0, sizeof(shd));
    double idle = 0, busy = 0;
    LONG64 frames = 0;
    for (int i = 0; i + 1 < n; i++) {
        double d = (double)(e[i + 1].t - e[i].t) * 1000.0 / (double)freq;
        int kind = KIND(e[i].key);
        if (kind == K_ENDFRAME)
            frames++;
        if (d > 200.0) /* a gap over missing data: ignore */
            continue;
        if (kind == K_ENDLIST || kind == K_ENDFRAME || kind == K_BIND) {
            idle += d;
            continue;
        }
        busy += d;
        TsAgg* a = agg_get(pass, PK, e[i].key, 0);
        if (a) /* a shader change inside the pass is not another pass */
            a->ms += d, a->cnt += (e[i].ps & PS_CONT) ? 0 : 1, a->cnt += !a->cnt;
        if (g_cfg.shader_timing && kind == K_PASS && (a = agg_get(shd, SK, e[i].key, e[i].ps & 0xFFFF)) != NULL)
            a->ms += d, a->cnt++;
    }
    if (frames < 1)
        frames = 1;
    static LONG64 p_dropped;
    LONG64 dropped = t_ts_dropped - p_dropped;
    p_dropped = t_ts_dropped;
    long nps = 0, npo = 0;
    fx_ps_counts(&nps, &npo);
    LOGI("  GPU time per pass (%lld frames, timer %.1f MHz): busy %.1f ms/frame in passes, %.1f ms/frame between "
         "command lists / after the frame | %lld timestamps dropped | pixel shaders known: %ld (%ld objects)",
         frames, freq / 1e6, busy / frames, idle / frames, (long long)dropped, nps, npo);
    for (int r = 0; r < 20; r++) { /* biggest first */
        TsAgg* b = agg_best(pass, PK);
        if (!b || b->ms / frames < 0.05)
            break;
        char lab[128], dr[32] = "";
        key_label(b->key, lab, sizeof(lab));
        AcquireSRWLockShared(&g_dk_lock);
        for (int i = 0; i < DK && g_dk_n[i]; i++)
            if (g_dk_key[i] == b->key) {
                snprintf(dr, sizeof(dr), "%5.0f draws", (double)g_dk_draws[i] / g_dk_n[i]);
                break;
            }
        ReleaseSRWLockShared(&g_dk_lock);
        LOGI("    %6.2f ms/frame %5.1f%%  %4.1fx/frame %s  %s", b->ms / frames, busy ? b->ms * 100.0 / busy : 0.0,
             (double)b->cnt / frames, dr, lab);
        b->cnt = -1;
    }
    if (!g_cfg.shader_timing)
        return;
    /* PC: the passes split at every pixel-shader change; draws and vertices counted at recording */
    static SdEntry sd[SD_N];
    AcquireSRWLockExclusive(&g_sd_lock);
    memcpy(sd, g_sd, sizeof(sd));
    memset(g_sd, 0, sizeof(g_sd));
    ReleaseSRWLockExclusive(&g_sd_lock);
    LONG64 sf = g_tframes, rec;
    static LONG64 psf;
    rec = sf - psf > 0 ? sf - psf : 1;
    psf = sf;
    LOGI("  GPU time by pixel shader (ShaderTiming; per frame; instr = instruction slots, tex = texture instructions, "
         "br = dynamic branches):");
    for (int r = 0; r < 32; r++) {
        TsAgg* b = agg_best(shd, SK);
        if (!b || b->ms / frames < 0.02)
            break;
        char lab[128];
        key_label(b->key, lab, sizeof(lab));
        const PsInfo* p = fx_ps_info(b->ps);
        double draws = 0, verts = 0;
        for (int i = 0; i < SD_N; i++)
            if (sd[i].draws && sd[i].key == b->key && sd[i].ps == b->ps)
                draws = (double)sd[i].draws / rec, verts = (double)sd[i].verts / rec;
        if (p)
            LOGI("    %6.2f ms %5.1f%%  PS %016llx (instr %d, tex %d, br %d)  %5.0f draws %7.0fK verts  in %s",
                 b->ms / frames, busy ? b->ms * 100.0 / busy : 0.0, (unsigned long long)p->hash, p->instr, p->tex, p->flow,
                 draws, verts / 1e3, lab);
        else
            LOGI("    %6.2f ms %5.1f%%  %s  %5.0f draws %7.0fK verts  in %s", b->ms / frames,
                 busy ? b->ms * 100.0 / busy : 0.0, b->ps ? "PS not known" : "no PS (depth only)", draws, verts / 1e3, lab);
        write_shader(p);
        b->cnt = -1;
    }
}
/* ---------------------------------------------------------------- occlusion queries (shadow-cull verify) */
void* gpuprobe_thread_ctx(int* shadow_pass)
{
    if (g_tls_ctx == TLS_OUT_OF_INDEXES)
        return NULL;
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)TlsGetValue(g_tls_ctx);
    if (c && shadow_pass) {
        Ctx* x = ctx_of(c);
        *shadow_pass = x->has_pass && x->cur.shadow;
    }
    return c;
}

/* Called by one thread at a time (the shadow stage), Begin and End back to back around one draw. */
int gpuprobe_query_begin(void* ctx)
{
    LONG64 h = g_qhead;
    if (h - g_qtail >= QRING)
        return -1;
    QSlot* s = &g_qs[h % QRING];
    if (!s->q) {
        if (!g_qdev) {
            ID3D11DeviceContext_GetDevice((ID3D11DeviceContext*)ctx, &g_qdev);
            if (!g_qdev)
                return -1;
            ID3D11Device_GetImmediateContext(g_qdev, &g_qimm);
        }
        D3D11_QUERY_DESC d = {D3D11_QUERY_OCCLUSION, 0};
        if (FAILED(ID3D11Device_CreateQuery(g_qdev, &d, &s->q)))
            return -1;
    }
    ID3D11DeviceContext_Begin((ID3D11DeviceContext*)ctx, (ID3D11Asynchronous*)s->q);
    return (int)(h % QRING);
}

void gpuprobe_query_end(void* ctx, int slot)
{
    QSlot* s = &g_qs[slot];
    ID3D11DeviceContext_End((ID3D11DeviceContext*)ctx, (ID3D11Asynchronous*)s->q);
    s->frame = t_frames;
    MemoryBarrier();
    InterlockedIncrement64(&g_qhead);
}

/* Present thread: results of queries recorded two or more frames ago, in order. */
static void read_queries(void)
{
    if (!g_qimm)
        return;
    LONG64 head = g_qhead;
    MemoryBarrier();
    while (g_qtail < head) {
        QSlot* s = &g_qs[g_qtail % QRING];
        if (s->frame + 2 > t_frames)
            break;
        UINT64 n = 0;
        HRESULT hr = ID3D11DeviceContext_GetData(g_qimm, (ID3D11Asynchronous*)s->q, &n, sizeof(n),
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE && s->frame + 30 > t_frames)
            break; /* not ready yet; a command list that never ran gives up after 30 frames */
        shadowcull_query_result((int)(g_qtail % QRING), hr == S_OK ? (int64_t)n : -1);
        InterlockedIncrement64(&g_qtail);
    }
}

/* ---------------------------------------------------------------- frame end (Present) */
void gpuprobe_frame_end(void)
{
    if (!g_active || !g_nvt)
        return;
    static int repatched_logged;
    for (LONG i = 0; i < g_nvt; i++)
        if (repatch(&g_vts[i]) && !repatched_logged++)
            LOGI("GPU probe: the runtime rewrote hooked context entries; re-hooking them every frame");
    g_tframes++;
    if (g_cfg.mesh_census)
        census_frame_end(g_census_imm);
    if (g_cfg.q_farmesh || g_cfg.q_fm_both)
        farmesh_frame_end(g_census_imm);
    if (g_cfg.gpu_timing)
        ts_frame_end();
    if (g_cfg.occl_hooks)
        oc_read();
    if (g_cfg.gpu_passes) {
        for (LONG i = 0; i < g_nctx; i++) /* the presenting thread drives the immediate context */
            if (!g_ctx[i].deferred)
                pa_close(&g_ctx[i]);
        AcquireSRWLockExclusive(&g_pa_lock);
        if (pa_snap_on) /* one frame's sequence per profiler window */
            pa_snap_on = 0;
        else if (!pa_snap_n)
            pa_snap_on = 1;
        ReleaseSRWLockExclusive(&g_pa_lock);
    }
    if (!g_cfg.gpu_probe)
        return; /* timing hooks only (wait profile / pass timing / pass audit) */

    /* the presenting thread drives the immediate context(s): close their open pass */
    for (LONG i = 0; i < g_nctx; i++)
        if (!g_ctx[i].deferred)
            flush_pass(&g_ctx[i]);

    AcquireSRWLockExclusive(&g_frame_lock);
    LONG64 draws = 0, ddraws = 0, sdraws = 0, sidx = 0;
    int nshadow = 0, first[4], nsizes = 0;
    for (int i = 0; i < g_nframe; i++) {
        Pass* p = &g_frame[i];
        draws += p->draws;
        if (p->deferred)
            ddraws += p->draws;
        if (!p->shadow)
            continue;
        sdraws += p->draws;
        sidx += p->indices;
        /* compare with the first shadow pass of the same size (the cascades share one size) */
        int j = 0;
        while (j < nsizes && g_frame[first[j]].w != p->w)
            j++;
        if (j == nsizes) {
            if (nsizes < 4)
                first[nsizes++] = i;
        } else {
            t_cmp++;
            Pass* f = &g_frame[first[j]];
            if (f->draws == p->draws && f->sum == p->sum && f->xr == p->xr)
                t_same++;
        }
        nshadow++;
    }
    /* per-cascade numbers: the shadow passes of the most common size, in frame order */
    int best = -1, bestn = 0;
    for (int j = 0; j < nsizes; j++) {
        int n = 0;
        for (int i = first[j]; i < g_nframe; i++)
            n += g_frame[i].shadow && g_frame[i].w == g_frame[first[j]].w;
        if (n > bestn)
            best = j, bestn = n;
    }
    for (int i = best >= 0 ? first[best] : g_nframe, c = 0; i < g_nframe && c < 4; i++)
        if (g_frame[i].shadow && g_frame[i].w == g_frame[first[best]].w) {
            t_cascade_draws[c] += g_frame[i].draws;
            t_cascade_idx[c] += g_frame[i].indices;
            c++;
        }
    t_frames++;
    t_draws += draws;
    t_deferred_draws += ddraws;
    t_shadow_draws += sdraws;
    t_shadow_passes += nshadow;
    t_shadow_indices += sidx;

    if ((t_frames & 63) == 1) { /* keep a readable picture of one frame's passes */
        char s[600];
        int n = 0;
        s[0] = 0;
        for (int i = 0; i < g_nframe && n < (int)sizeof(s) - 40; i++) {
            Pass* p = &g_frame[i];
            if (p->draws < 5)
                continue;
            n += snprintf(s + n, sizeof(s) - n, "%s%dx%d%s%s:%ld", n ? " " : "", p->w, p->h, p->shadow ? "S" : "",
                          p->deferred ? "d" : "", p->draws);
        }
        AcquireSRWLockExclusive(&g_snap_lock);
        memcpy(g_snap, s, (size_t)n + 1);
        ReleaseSRWLockExclusive(&g_snap_lock);
    }
    g_nframe = 0;
    ReleaseSRWLockExclusive(&g_frame_lock);
    read_queries();
}

/* [Quality] per window: the texture option */
static void q_report(void)
{
    static LONG64 pf, pss, pfd, prb;
    LONG64 f = g_tframes - pf, ss = q_samp_swaps - pss, fd = q_far_draws - pfd, rb = q_rebinds - prb;
    pf = g_tframes, pss = q_samp_swaps, pfd = q_far_draws, prb = q_rebinds;
    if (f <= 0 || !g_cfg.q_report)
        return;
    LOGI("  Quality: samplers biased %.0f/frame | sampler kinds seen: anisotropic %lld, trilinear %lld, other filters "
         "%lld, left alone (comparison / no mips) %lld",
         (double)ss / f, (long long)q_twins_aniso, (long long)q_twins_lin, (long long)q_twins_other,
         (long long)q_twins_skip);
    if (g_q_far_hooks)
        LOGI("  Quality: draws with a texture bias (far objects, characters) %.0f/frame, sampler re-binds %.0f/frame", (double)fd / f,
             (double)rb / f);
    static LONG64 pa[5], po, pc;
    LONG64 a[5], o = q_binds_other - po, cm = q_binds_cmp - pc;
    for (int i = 0; i < 5; i++)
        a[i] = q_binds_aniso[i] - pa[i], pa[i] = q_binds_aniso[i];
    po = q_binds_other, pc = q_binds_cmp;
    LOGI("  Quality: pixel-shader sampler binds per frame - anisotropic 2x %.0f, 4x %.0f, 8x %.0f, 9-15x %.0f, 16x %.0f | "
         "other filters %.0f, comparison %.0f | anisotropy clamp %d",
         (double)a[0] / f, (double)a[1] / f, (double)a[2] / f, (double)a[3] / f, (double)a[4] / f, (double)o / f,
         (double)cm / f, g_cfg.q_max_aniso);
    if (!g_q_far_hooks)
        return;
    static LONG64 pt[GEO_CLASSES], pm[GEO_CLASSES], ps, pot;
    LONG64 t[GEO_CLASSES], m[GEO_CLASSES], sh = q_geo_shadow - ps, ot = q_geo_other - pot;
    double tw = 0, tc = 0, mw = 0, mc = 0;
    for (int i = 0; i < GEO_CLASSES; i++) {
        t[i] = q_geo_tris[i] - pt[i], m[i] = q_geo_micro[i] - pm[i], pt[i] = q_geo_tris[i], pm[i] = q_geo_micro[i];
        if (i < GEO_BANDS)
            tw += (double)t[i], mw += (double)m[i];
        else
            tc += (double)t[i], mc += (double)m[i];
    }
    ps = q_geo_shadow, pot = q_geo_other;
    LOGI("  Quality: scene triangles per frame (thousands) by distance <15 / 15-30 / 30-50 / 50-100 / >100 m - world "
         "%.0f / %.0f / %.0f / %.0f / %.0f, characters %.0f / %.0f / %.0f / %.0f / %.0f | in objects with < 4 px per "
         "triangle: world %.0f%%, characters %.0f%% | shadow maps %.0f, other draws %.0f",
         t[0] / 1e3 / f, t[1] / 1e3 / f, t[2] / 1e3 / f, t[3] / 1e3 / f, t[4] / 1e3 / f, t[5] / 1e3 / f, t[6] / 1e3 / f,
         t[7] / 1e3 / f, t[8] / 1e3 / f, t[9] / 1e3 / f, tw > 0 ? mw * 100.0 / tw : 0.0, tc > 0 ? mc * 100.0 / tc : 0.0,
         (double)sh / 1e3 / f, (double)ot / 1e3 / f);
}

static int cmp_pak(const void* a, const void* b)
{
    const PaKind *p = (const PaKind*)a, *q = (const PaKind*)b;
    double x = p->load + p->store, y = q->load + q->store;
    return (x < y) - (x > y);
}

static void pa_report(void)
{
    static LONG64 p_frames;
    static PaKind k[PAK];
    AcquireSRWLockExclusive(&g_pa_lock);
    LONG64 f = g_tframes - p_frames;
    p_frames = g_tframes;
    memcpy(k, g_pak, sizeof(k));
    memset(g_pak, 0, sizeof(g_pak));
    LONG64 passes = pa_passes, segs = pa_segs, sp[SP_N], ac = pa_att_clear, ad = pa_att_disc, al = pa_att_load,
           als = pa_att_load_split, ic = pa_inpass_clears, full = pa_full, du = pa_depth_unused;
    double load = pa_load, lsplit = pa_load_split, store = pa_store, avoid = pa_avoid, dw = pa_depth_waste;
    for (int i = 0; i < SP_N; i++)
        sp[i] = pa_split[i], pa_split[i] = 0;
    pa_passes = pa_segs = pa_att_clear = pa_att_disc = pa_att_load = pa_att_load_split = pa_inpass_clears = 0;
    pa_full = pa_depth_unused = 0;
    pa_load = pa_load_split = pa_store = pa_avoid = pa_depth_waste = 0;
    char snap[sizeof(pa_snap)];
    memcpy(snap, pa_snap, sizeof(snap));
    pa_snap[0] = 0, pa_snap_n = 0, pa_snap_on = 0;
    ReleaseSRWLockExclusive(&g_pa_lock);
    if (f <= 0 || passes <= 0)
        return;
    double F = (double)f, MB = 1048576.0;
    char splits[200] = "";
    int n = 0;
    for (int i = 0; i < SP_N; i++)
        if (sp[i])
            n += snprintf(splits + n, sizeof(splits) - n, "%s%.1f by %s", n ? ", " : "", sp[i] / F, sp_name[i]);
    LOGI("  Render passes: %.0f per frame (target bindings with draws) -> %.0f render pass instances (%.1f splits%s%s%s); "
         "%.1f clears inside passes",
         passes / F, segs / F, (segs - passes) / F, n ? ": " : "", splits, "", ic / F);
    LOGI("  Render pass attachments per frame (tiled GPU): %.1f cleared, %.1f discarded, %.1f loaded at pass start "
         "(%.1f MB), %.1f reloaded after splits (%.1f MB); stored %.1f MB | looks avoidable: %.1f MB of loads in %.1f "
         "passes that overwrite the whole target, %.1f MB load+store of depth bound but unused in %.1f passes",
         ac / F, ad / F, al / F, load / MB / F, als / F, lsplit / MB / F, store / MB / F, avoid / MB / F, full / F,
         dw / MB / F, du / F);
    qsort(k, PAK, sizeof(k[0]), cmp_pak);
    for (int i = 0; i < 12 && k[i].n; i++) {
        char lab[96];
        key_label(k[i].key, lab, sizeof(lab));
        LOGI("    %6.1f MB load + %6.1f MB store /frame  %4.1fx/frame  %5.1f render passes  %6.1f draws  full-screen "
             "first draw %3.0f%%  depth unused %3.0f%%  %s",
             k[i].load / MB / F, k[i].store / MB / F, k[i].n / F, k[i].segs / F, (double)k[i].draws / k[i].n,
             k[i].full * 100.0 / k[i].n, k[i].depth_unused * 100.0 / k[i].n, lab);
    }
    if (snap[0])
        LOGI("  Render passes of one frame (WxH, S = depth only, +D depth; per attachment C cleared / D discarded / L "
             "loaded / F loaded but overwritten; u = depth unused; :draws or /render passes when split):%s",
             snap);
}

void gpuprobe_report(double seconds)
{
    static LONG64 p_frames, p_draws, p_ddraws, p_sdraws, p_spasses, p_sidx, p_cmp, p_same, p_cas[4], p_cidx[4];
    static LONG64 p_fin, p_exe;
    static LONG64 prev_callers[SITE_SLOTS];
    if (g_active && g_cfg.gpu_timing)
        ts_report();
    if (g_active && g_cfg.compute_census)
        cs_report();
    if (g_active && g_cfg.gpu_passes)
        pa_report();
    if (g_active && g_cfg.q_active) {
        q_report();
        effects_report();
        farmesh_report();
        adaptive_report();
        if (g_cfg.occl_hooks)
            oc_report();
    }
    if (!g_active || !g_cfg.gpu_probe)
        return;
    AcquireSRWLockShared(&g_frame_lock); /* consistent snapshot of the totals */
    LONG64 f = t_frames - p_frames, d = t_draws - p_draws, dd = t_deferred_draws - p_ddraws;
    LONG64 sd = t_shadow_draws - p_sdraws, sp = t_shadow_passes - p_spasses, si = t_shadow_indices - p_sidx;
    LONG64 cmp = t_cmp - p_cmp, same = t_same - p_same, cas[4], cidx[4];
    for (int i = 0; i < 4; i++) {
        cas[i] = t_cascade_draws[i] - p_cas[i], p_cas[i] = t_cascade_draws[i];
        cidx[i] = t_cascade_idx[i] - p_cidx[i], p_cidx[i] = t_cascade_idx[i];
    }
    p_frames = t_frames, p_draws = t_draws, p_ddraws = t_deferred_draws, p_sdraws = t_shadow_draws;
    p_spasses = t_shadow_passes, p_sidx = t_shadow_indices, p_cmp = t_cmp, p_same = t_same;
    ReleaseSRWLockShared(&g_frame_lock);
    LONG64 fin = t_finish - p_fin, exe = t_execute - p_exe;
    p_fin = t_finish, p_exe = t_execute;
    if (f <= 0)
        return;
    LOGI("  GPU probe: %.0f draws/frame (%.0f%% recorded on deferred contexts; %.1f command lists finished, %.1f "
         "executed per frame; %ld contexts seen)",
         (double)d / f, d ? dd * 100.0 / d : 0.0, (double)fin / f, (double)exe / f, g_nctx);
    LOGI("  GPU probe: %.1f shadow-map passes/frame with %.0f draws (%.0f%% of all draws), %.2fM indices",
         (double)sp / f, (double)sd / f, d ? sd * 100.0 / d : 0.0, si / 1e6 / f);
    LOGI("  GPU probe: cascades (shadow passes of the common size, frame order): draws %.0f / %.0f / %.0f / %.0f, "
         "indices %.2fM / %.2fM / %.2fM / %.2fM | passes drawing the identical object set as the first pass of their "
         "size: %.0f%% of %lld comparisons",
         (double)cas[0] / f, (double)cas[1] / f, (double)cas[2] / f, (double)cas[3] / f, cidx[0] / 1e6 / f,
         cidx[1] / 1e6 / f, cidx[2] / 1e6 / f, cidx[3] / 1e6 / f, cmp ? same * 100.0 / cmp : 0.0, cmp);
    AcquireSRWLockShared(&g_snap_lock);
    LOGI("  GPU probe: one frame's passes (WxH, S = shadow map, d = deferred, :draws): %s", g_snap);
    ReleaseSRWLockShared(&g_snap_lock);
    site_report("GPU probe: callers of shadow-pass draw calls", &g_callers, prev_callers, seconds, 6);

    Chain snap[CHAINS];
    AcquireSRWLockShared(&g_chain_lock);
    memcpy(snap, g_chain, sizeof(snap));
    ReleaseSRWLockShared(&g_chain_lock);
    for (int r = 0; r < 4; r++) {
        int best = -1;
        for (int i = 0; i < CHAINS; i++)
            if (snap[i].n > 0 && (best < 0 || snap[i].n > snap[best].n))
                best = i;
        if (best < 0)
            break;
        char a[64], b[64], c[64];
        Chain* ch = &snap[best];
        LOGI("  GPU probe: shadow draw call chain %lld x: %s <- %s <- %s", ch->n, fmt_addr(ch->f[0], a, sizeof(a)),
             ch->f[1] ? fmt_addr(ch->f[1], b, sizeof(b)) : "-", ch->f[2] ? fmt_addr(ch->f[2], c, sizeof(c)) : "-");
        ch->n = 0;
    }
}
