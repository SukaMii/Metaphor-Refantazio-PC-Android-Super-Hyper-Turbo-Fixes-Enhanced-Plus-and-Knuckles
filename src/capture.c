/* One-frame capture of every render pass ([GpuProbe] Capture=1, PC diagnostic).
 *
 * Press F10 (or wait CaptureAfterSec): the next frames are recorded and one frame is written to
 * MetaphorCPU_capture_<time>\ next to the log:
 *   - pNNN_rtK.png: each colour target as it is after pass NNN (passes in GPU execution order;
 *     command lists of deferred contexts are spliced in where the immediate context executes
 *     them), shrunk to <= 960 px wide; float targets tone-mapped (x / (1 + x)), signed ones
 *     shown as 0.5 + 0.5 * x / max|x|;
 *   - final.png: the back buffer at Present;
 *   - ps_<hash>.dxbc: the pixel shaders of the frame (D3DDisassemble reads them);
 *   - capture.txt: per pass the targets (format, size, value range), draws, whether it is a
 *     full-screen draw, the pixel shader, and which earlier pass produced each input texture.
 * Timeline: the game records every draw on deferred contexts, some command lists a frame or more
 * before they run. Recording therefore spans up to 4 frames; the first frame (between two
 * Presents) whose executed command lists were all recorded is written, otherwise the one with the
 * fewest missing. Only passes with up to 128 draws are copied (post-processing, effects; the big
 * scene passes are listed without images), to bound the staging memory.
 * Copies are recorded into the context that drew the pass (CopySubresourceRegion to a staging
 * texture, allowed on deferred contexts) and read back on the presenting thread afterwards. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { CS_IDLE, CS_ARMED, CS_REC, CS_DONE };
#define CAP_MAX_DRAWS 128 /* passes with more draws are listed, not copied */
#define EF_N 4            /* execution frames recorded at most */
static volatile LONG g_cs = CS_IDLE;
static uint64_t g_cap_start_ms;
static int g_cap_count;

#define CAP_RT 4
#define CAP_SRV 8
/* [GpuProbe] CaptureDrawSteps: in the characters' pass (a colour target RGBA16F among 3+ targets, with depth), the
 * middle of the screen after every draw, to see which draw paints what */
#define STEP_SIZE 320
#define STEP_MAX 400
typedef struct CapStep {
    ID3D11Texture2D* st;
    void* ps;
    UINT verts;
} CapStep;

typedef struct CapPass {
    struct CapPass* next;
    void* ctx;
    int deferred;
    int nrt;
    ID3D11Resource* rt[CAP_RT];
    UINT rt_sub[CAP_RT], rt_w[CAP_RT], rt_h[CAP_RT];
    DXGI_FORMAT rt_fmt[CAP_RT];
    ID3D11Texture2D* staging[CAP_RT];
    ID3D11Resource* depth;
    int draws, fullscreen, first_seen;
    UINT verts;
    void* ps;
    int nsrv;
    ID3D11Resource* srv[CAP_SRV];
    UINT srv_slot[CAP_SRV];
    int stepping, nsteps;          /* CaptureDrawSteps: this pass is stepped; steps copied */
    UINT sx0, sy0, sw, sh;         /* the region */
    void* last_ps;                 /* the draw before the next copy */
    UINT last_verts;
    CapStep* steps;
} CapPass;

typedef struct CapCtx {
    void* ctx;
    CapPass* cur;
    CapPass *head, *tail; /* deferred: passes recorded since the last FinishCommandList */
    ID3D11ShaderResourceView* srv[16];
    void* ps;
} CapCtx;
#define CAP_CTX 64
static CapCtx g_cc[CAP_CTX];
static volatile LONG g_ncc;
static SRWLOCK g_cap_lock = SRWLOCK_INIT;

/* command lists finished while recording, and what they contain */
#define CAP_CL 512
static struct {
    void* cl;
    CapPass* head;
    int pre; /* recorded before the frame that is written */
} g_cl[CAP_CL];
static int g_ncl;
/* per execution frame (between two Presents): passes in execution order, lists run, lists unknown */
static CapPass *g_ef_head[EF_N], *g_ef_tail[EF_N];
static int g_ef_lists[EF_N], g_ef_missing[EF_N], g_ef_cur, g_ef_out = -1;
static ID3D11Texture2D* g_ef_final[EF_N];
static volatile LONG g_ps_seen; /* pixel shaders seen being created */
static int g_dropped;
static volatile LONG64 g_staging_bytes;
#define CAP_BUDGET (1536ull << 20)
static ID3D11DeviceContext* g_imm; /* the presenting device's immediate context, for the readback */
static UINT g_final_w, g_final_h;
static DXGI_FORMAT g_final_fmt;

/* pixel shaders: bytecode kept from creation, written for the shaders the frame uses */
#define PS_N 16384
static struct {
    void* ps;
    uint64_t hash;
    void* code;
    size_t len;
} g_ps[PS_N];
static SRWLOCK g_ps_lock = SRWLOCK_INIT;

static uint64_t fnv64(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

void capture_shader_created(void* ps, const void* code, size_t len)
{
    if (!g_cfg.capture || !ps || !code || !len)
        return;
    void* copy = HeapAlloc(GetProcessHeap(), 0, len);
    if (!copy)
        return;
    memcpy(copy, code, len);
    uint64_t h = fnv64(code, len);
    InterlockedIncrement(&g_ps_seen);
    unsigned k = (unsigned)(((uintptr_t)ps >> 4) * 2654435761u) & (PS_N - 1);
    AcquireSRWLockExclusive(&g_ps_lock);
    for (unsigned i = 0; i < PS_N; i++, k = (k + 1) & (PS_N - 1))
        if (!g_ps[k].ps || g_ps[k].ps == ps) {
            if (g_ps[k].code)
                HeapFree(GetProcessHeap(), 0, g_ps[k].code);
            g_ps[k].ps = ps, g_ps[k].hash = h, g_ps[k].code = copy, g_ps[k].len = len;
            copy = NULL;
            break;
        }
    ReleaseSRWLockExclusive(&g_ps_lock);
    if (copy)
        HeapFree(GetProcessHeap(), 0, copy);
}

static int ps_find(void* ps)
{
    unsigned k = (unsigned)(((uintptr_t)ps >> 4) * 2654435761u) & (PS_N - 1);
    for (unsigned i = 0; i < PS_N; i++, k = (k + 1) & (PS_N - 1)) {
        if (g_ps[k].ps == ps)
            return (int)k;
        if (!g_ps[k].ps)
            return -1;
    }
    return -1;
}

static CapCtx* cc_of(void* c)
{
    LONG n = g_ncc;
    for (LONG i = 0; i < n; i++)
        if (g_cc[i].ctx == c)
            return &g_cc[i];
    AcquireSRWLockExclusive(&g_cap_lock);
    CapCtx* r = NULL;
    for (LONG i = 0; i < g_ncc && !r; i++)
        if (g_cc[i].ctx == c)
            r = &g_cc[i];
    if (!r && g_ncc < CAP_CTX) {
        r = &g_cc[g_ncc];
        memset(r, 0, sizeof(*r));
        r->ctx = c;
        InterlockedIncrement(&g_ncc);
    }
    ReleaseSRWLockExclusive(&g_cap_lock);
    return r;
}

static int is_deferred(void* c)
{
    return ID3D11DeviceContext_GetType((ID3D11DeviceContext*)c) == D3D11_DEVICE_CONTEXT_DEFERRED;
}

static void free_pass(CapPass* p)
{
    for (int i = 0; i < p->nrt; i++) {
        if (p->staging[i])
            ID3D11Texture2D_Release(p->staging[i]);
        if (p->rt[i])
            ID3D11Resource_Release(p->rt[i]);
    }
    for (int i = 0; i < p->nsrv; i++)
        if (p->srv[i])
            ID3D11Resource_Release(p->srv[i]);
    if (p->depth)
        ID3D11Resource_Release(p->depth);
    for (int i = 0; i < p->nsteps; i++)
        if (p->steps[i].st)
            ID3D11Texture2D_Release(p->steps[i].st);
    if (p->steps)
        HeapFree(GetProcessHeap(), 0, p->steps);
    HeapFree(GetProcessHeap(), 0, p);
}

/* the region after the pass's last draw so far */
static void step_copy(CapPass* p, ID3D11DeviceContext* c)
{
    if (p->nsteps >= STEP_MAX || !p->steps)
        return;
    CapStep* s = &p->steps[p->nsteps++];
    s->ps = p->last_ps, s->verts = p->last_verts, s->st = NULL;
    UINT bytes = p->sw * p->sh * 8;
    if ((uint64_t)InterlockedExchangeAdd64(&g_staging_bytes, bytes) + bytes > CAP_BUDGET) {
        g_dropped++;
        return;
    }
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext_GetDevice(c, &dev);
    if (!dev)
        return;
    D3D11_TEXTURE2D_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.Width = p->sw, sd.Height = p->sh, sd.MipLevels = 1, sd.ArraySize = 1, sd.Format = p->rt_fmt[0];
    sd.SampleDesc.Count = 1, sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &s->st))) {
        D3D11_BOX box = {p->sx0, p->sy0, 0, p->sx0 + p->sw, p->sy0 + p->sh, 1};
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)s->st, 0, 0, 0, 0, p->rt[0], p->rt_sub[0], &box);
    } else {
        s->st = NULL;
    }
    ID3D11Device_Release(dev);
}

static void free_list(CapPass* p)
{
    while (p) {
        CapPass* n = p->next;
        free_pass(p);
        p = n;
    }
}

/* the pass's colour targets are copied out, in the context that drew them */
static void end_pass(CapCtx* x)
{
    CapPass* p = x->cur;
    if (!p)
        return;
    x->cur = NULL;
    if (g_cs != CS_REC || p->draws == 0) {
        free_pass(p); /* not part of the recording, or nothing drawn */
        return;
    }
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)x->ctx;
    if (p->stepping)
        step_copy(p, c); /* after the last draw */
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext_GetDevice(c, &dev);
    for (int i = 0; i < p->nrt && dev && p->draws <= CAP_MAX_DRAWS; i++) {
        if (i > 0 && !p->first_seen)
            continue; /* extra targets of an MRT pass: only the first time they are bound */
        D3D11_TEXTURE2D_DESC d;
        ZeroMemory(&d, sizeof(d));
        d.Width = p->rt_w[i], d.Height = p->rt_h[i], d.MipLevels = 1, d.ArraySize = 1;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D*)p->rt[i], &d);
        DXGI_FORMAT fmt = d.Format;
        UINT bytes = p->rt_w[i] * p->rt_h[i] * 8;
        if ((uint64_t)InterlockedExchangeAdd64(&g_staging_bytes, bytes) + bytes > CAP_BUDGET) {
            g_dropped++;
            continue;
        }
        D3D11_TEXTURE2D_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.Width = p->rt_w[i], sd.Height = p->rt_h[i], sd.MipLevels = 1, sd.ArraySize = 1, sd.Format = fmt;
        sd.SampleDesc.Count = 1, sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (d.SampleDesc.Count > 1 || FAILED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &p->staging[i]))) {
            p->staging[i] = NULL;
            continue;
        }
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)p->staging[i], 0, 0, 0, 0, p->rt[i], p->rt_sub[i],
                                                  NULL);
    }
    if (dev)
        ID3D11Device_Release(dev);
    if (p->deferred) {
        if (x->tail)
            x->tail->next = p;
        else
            x->head = p;
        x->tail = p;
    } else {
        AcquireSRWLockExclusive(&g_cap_lock);
        int f = g_ef_cur;
        if (g_ef_tail[f])
            g_ef_tail[f]->next = p;
        else
            g_ef_head[f] = p;
        g_ef_tail[f] = p;
        ReleaseSRWLockExclusive(&g_cap_lock);
    }
}

int capture_recording(void)
{
    return g_cs == CS_REC;
}

/* MRT targets already captured in this frame (only the first binding is copied) */
#define SEEN_N 256
static void* g_seen[SEEN_N];
static int g_nseen;
static SRWLOCK g_seen_lock = SRWLOCK_INIT;

static int first_time(void* res)
{
    int r = 1;
    AcquireSRWLockExclusive(&g_seen_lock);
    for (int i = 0; i < g_nseen; i++)
        if (g_seen[i] == res)
            r = 0;
    if (r && g_nseen < SEEN_N)
        g_seen[g_nseen++] = res;
    ReleaseSRWLockExclusive(&g_seen_lock);
    return r;
}

void capture_bind(void* ctx, UINT n, void* const* rtvs, void* dsv)
{
    if (!capture_recording())
        return;
    CapCtx* x = cc_of(ctx);
    if (!x)
        return;
    end_pass(x);
    CapPass* p = (CapPass*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CapPass));
    if (!p)
        return;
    p->ctx = ctx, p->deferred = is_deferred(ctx);
    for (UINT i = 0; i < n && i < 8 && rtvs; i++) {
        ID3D11RenderTargetView* v = (ID3D11RenderTargetView*)rtvs[i];
        if (!v || p->nrt >= CAP_RT)
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC vd;
        ID3D11RenderTargetView_GetDesc(v, &vd);
        if (vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D && vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
            continue;
        ID3D11Resource* r = NULL;
        ID3D11View_GetResource((ID3D11View*)v, &r);
        if (!r)
            continue;
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(r, &dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            ID3D11Resource_Release(r);
            continue;
        }
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D*)r, &td);
        UINT mip = vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? vd.Texture2D.MipSlice : vd.Texture2DArray.MipSlice;
        UINT slice = vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? 0 : vd.Texture2DArray.FirstArraySlice;
        int k = p->nrt++;
        p->rt[k] = r;
        p->rt_sub[k] = mip + slice * td.MipLevels;
        p->rt_w[k] = td.Width >> mip ? td.Width >> mip : 1;
        p->rt_h[k] = td.Height >> mip ? td.Height >> mip : 1;
        p->rt_fmt[k] = vd.Format;
    }
    if (dsv)
        ID3D11View_GetResource((ID3D11View*)dsv, &p->depth);
    p->first_seen = p->nrt > 1 ? first_time(p->rt[1]) : 1;
    x->cur = p;
}

void capture_srvs(void* ctx, UINT start, UINT n, void* const* srvs)
{
    if (!capture_recording())
        return;
    CapCtx* x = cc_of(ctx);
    for (UINT i = 0; x && i < n && start + i < 16; i++)
        x->srv[start + i] = srvs ? (ID3D11ShaderResourceView*)srvs[i] : NULL;
}

void capture_ps(void* ctx, void* ps)
{
    if (!capture_recording())
        return;
    CapCtx* x = cc_of(ctx);
    if (x)
        x->ps = ps;
}

void capture_draw(void* ctx, UINT verts, int nonindexed)
{
    if (!capture_recording())
        return;
    CapCtx* x = cc_of(ctx);
    CapPass* p = x ? x->cur : NULL;
    if (!p)
        return;
    if (p->stepping)
        step_copy(p, (ID3D11DeviceContext*)ctx); /* the region after the draw before this one */
    if (p->draws == 0 && g_cfg.capture_steps && p->nrt >= 3 && p->depth && p->rt_fmt[0] == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        p->rt_w[0] >= 64 && p->rt_h[0] >= 64) { /* the characters' pass: stepped from its second draw on */
        p->sw = p->rt_w[0] < STEP_SIZE ? p->rt_w[0] : STEP_SIZE, p->sh = p->rt_h[0] < STEP_SIZE ? p->rt_h[0] : STEP_SIZE;
        p->sx0 = (p->rt_w[0] - p->sw) / 2, p->sy0 = (p->rt_h[0] - p->sh) / 2;
        p->steps = (CapStep*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CapStep) * STEP_MAX);
        p->stepping = p->steps != NULL;
    }
    p->last_ps = x->ps, p->last_verts = verts;
    if (p->draws++ == 0) { /* the inputs and shader of the pass's first draw */
        p->ps = x->ps;
        p->fullscreen = nonindexed && (verts == 3 || verts == 4 || verts == 6);
        for (int s = 0; s < 16 && p->nsrv < CAP_SRV; s++)
            if (x->srv[s]) {
                ID3D11Resource* r = NULL;
                ID3D11View_GetResource((ID3D11View*)x->srv[s], &r);
                if (r)
                    p->srv[p->nsrv] = r, p->srv_slot[p->nsrv] = (UINT)s, p->nsrv++;
            }
    }
    p->verts += verts;
}

/* before FinishCommandList: the open pass's copies go into this list */
void capture_end_pass(void* ctx)
{
    CapCtx* x = g_cs == CS_IDLE ? NULL : cc_of(ctx);
    if (x)
        end_pass(x);
}

/* after FinishCommandList: the passes recorded on this context belong to that list */
void capture_finish_cl(void* ctx, void* cl)
{
    if (g_cs == CS_IDLE)
        return;
    CapCtx* x = cc_of(ctx);
    if (!x)
        return;
    if (x->cur) {
        free_pass(x->cur);
        x->cur = NULL;
    }
    CapPass* head = x->head;
    x->head = x->tail = NULL;
    memset(x->srv, 0, sizeof(x->srv)); /* the next list starts with default state */
    x->ps = NULL;
    if (!head)
        return;
    CapPass* old = NULL;
    AcquireSRWLockExclusive(&g_cap_lock);
    if (cl && capture_recording()) {
        int i = 0;
        while (i < g_ncl && g_cl[i].cl != cl)
            i++;
        if (i < CAP_CL) {
            if (i == g_ncl)
                g_ncl++;
            old = g_cl[i].head;
            g_cl[i].cl = cl, g_cl[i].head = head, g_cl[i].pre = 0;
            head = NULL;
        }
    }
    ReleaseSRWLockExclusive(&g_cap_lock);
    free_list(head);
    free_list(old);
}

void capture_exec_cl(void* ctx, void* cl, int restore)
{
    LONG st = g_cs;
    if (st == CS_IDLE || st == CS_ARMED)
        return;
    CapCtx* x = cc_of(ctx);
    if (x) {
        end_pass(x); /* the immediate pass is interrupted by the list */
        if (!restore) {
            memset(x->srv, 0, sizeof(x->srv));
            x->ps = NULL;
        }
    }
    if (st != CS_REC)
        return;
    AcquireSRWLockExclusive(&g_cap_lock);
    int found = 0, f = g_ef_cur;
    for (int i = 0; i < g_ncl; i++)
        if (g_cl[i].cl == cl) {
            CapPass* h = g_cl[i].head;
            g_cl[i].head = NULL;
            g_cl[i].cl = NULL;
            found = 1;
            if (!h)
                break; /* a list without passes (copies, compute) */
            if (g_ef_tail[f])
                g_ef_tail[f]->next = h;
            else
                g_ef_head[f] = h;
            while (h->next)
                h = h->next;
            g_ef_tail[f] = h;
            break;
        }
    g_ef_lists[f]++;
    if (!found)
        g_ef_missing[f]++;
    ReleaseSRWLockExclusive(&g_cap_lock);
}

static int g_cmp_want;
static ID3D11Texture2D* g_cmp_tex;
static UINT g_cmp_w, g_cmp_h;
static DXGI_FORMAT g_cmp_fmt;

/* compare shots: the back buffer of this frame, full size */
static void cmp_copy(void* swapchain)
{
    ID3D11Texture2D* bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer((IDXGISwapChain*)swapchain, 0, &IID_ID3D11Texture2D, (void**)&bb)) || !bb)
        return;
    ID3D11Device* dev = NULL;
    ID3D11Texture2D_GetDevice(bb, &dev);
    if (dev) {
        ID3D11DeviceContext* imm = NULL;
        ID3D11Device_GetImmediateContext(dev, &imm);
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(bb, &d);
        d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
        d.MipLevels = 1, d.ArraySize = 1;
        if (imm && d.SampleDesc.Count == 1 && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_cmp_tex))) {
            ID3D11DeviceContext_CopyResource(imm, (ID3D11Resource*)g_cmp_tex, (ID3D11Resource*)bb);
            g_cmp_w = d.Width, g_cmp_h = d.Height, g_cmp_fmt = d.Format;
        }
        if (imm) {
            if (g_imm)
                ID3D11DeviceContext_Release(g_imm);
            g_imm = imm;
        }
        ID3D11Device_Release(dev);
    }
    ID3D11Texture2D_Release(bb);
}

void capture_present(void* swapchain)
{
    if (g_cmp_want) {
        g_cmp_want = 0;
        cmp_copy(swapchain);
    }
    if (!g_cfg.capture)
        return;
    LONG st = g_cs;
    if (st == CS_ARMED) {
        g_nseen = 0, g_ef_cur = 0, g_ef_out = -1;
        g_cs = CS_REC;
        return;
    }
    if (st != CS_REC)
        return;
    /* the end of one execution frame: the immediate pass and the back buffer */
    int f = g_ef_cur;
    ID3D11Texture2D* bb = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetBuffer((IDXGISwapChain*)swapchain, 0, &IID_ID3D11Texture2D, (void**)&bb)) && bb) {
        ID3D11Device* dev = NULL;
        ID3D11Texture2D_GetDevice(bb, &dev);
        if (dev) {
            ID3D11DeviceContext* imm = NULL;
            ID3D11Device_GetImmediateContext(dev, &imm);
            CapCtx* x = imm ? cc_of(imm) : NULL;
            if (x)
                end_pass(x);
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc(bb, &d);
            d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
            d.MipLevels = 1, d.ArraySize = 1;
            if (imm && d.SampleDesc.Count == 1 && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_ef_final[f]))) {
                ID3D11DeviceContext_CopyResource(imm, (ID3D11Resource*)g_ef_final[f], (ID3D11Resource*)bb);
                g_final_w = d.Width, g_final_h = d.Height, g_final_fmt = d.Format;
            }
            if (imm) {
                if (g_imm)
                    ID3D11DeviceContext_Release(g_imm);
                g_imm = imm; /* kept for the readback */
            }
            ID3D11Device_Release(dev);
        }
        ID3D11Texture2D_Release(bb);
    }
    if (g_ef_missing[f] == 0 && (g_ef_lists[f] > 0 || g_ef_head[f])) { /* every list of this frame was recorded */
        g_ef_out = f;
        g_cs = CS_DONE;
        return;
    }
    if (f + 1 < EF_N) {
        g_nseen = 0;
        g_ef_cur = f + 1;
        return;
    }
    int best = -1; /* none complete: the one with the most recorded lists */
    for (int i = 0; i < EF_N; i++)
        if ((g_ef_lists[i] > 0 || g_ef_head[i]) &&
            (best < 0 || g_ef_lists[i] - g_ef_missing[i] > g_ef_lists[best] - g_ef_missing[best]))
            best = i;
    g_ef_out = best;
    g_cs = CS_DONE;
}

/* ---------------------------------------------------------------- images */
static uint32_t g_crc[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc[n] = c;
    }
}

static uint32_t crc_upd(uint32_t c, const uint8_t* b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = g_crc[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len)
{
    uint8_t h[8];
    put32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t c = crc_upd(0xFFFFFFFFu, (const uint8_t*)type, 4);
    c = crc_upd(c, data, len) ^ 0xFFFFFFFFu;
    put32(h, c);
    fwrite(h, 1, 4, f);
}

/* RGB PNG, zlib "stored" blocks (no compression library needed) */
static int write_png(const char* path, int w, int h, const uint8_t* rgb)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w), put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw = (size_t)h * (1 + 3 * (size_t)w);
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4;
    uint8_t* z = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, zlen);
    if (!z) {
        fclose(f);
        return 0;
    }
    size_t o = 0, done = 0;
    z[o++] = 0x78, z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    size_t row = 1 + 3 * (size_t)w;
    while (done < raw) {
        size_t n = raw - done > 65535 ? 65535 : raw - done;
        z[o++] = done + n == raw ? 1 : 0;
        z[o++] = (uint8_t)n, z[o++] = (uint8_t)(n >> 8), z[o++] = (uint8_t)~n, z[o++] = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++, done++) {
            size_t y = done / row, x = done % row;
            uint8_t v = x == 0 ? 0 : rgb[y * 3 * (size_t)w + x - 1];
            z[o++] = v;
            a = (a + v) % 65521u, b = (b + a) % 65521u;
        }
    }
    put32(z + o, (b << 16) | a), o += 4;
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    HeapFree(GetProcessHeap(), 0, z);
    fclose(f);
    return 1;
}

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, v;
    if (e == 0)
        return (s ? -1.0f : 1.0f) * ldexpf((float)m, -24);
    if (e == 31)
        v = s | 0x7F800000u | (m << 13);
    else
        v = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float uf_to_float(uint32_t bits, int mbits) /* unsigned 5-bit exponent floats (R11G11B10) */
{
    uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 0)
        return ldexpf((float)m, -14 - mbits);
    if (e == 31)
        return 65504.0f;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}

static const char* fmt_name(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8";
    case DXGI_FORMAT_R16_UNORM: return "R16";
    case DXGI_FORMAT_R16G16_UNORM: return "RG16";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16";
    default: return NULL;
    }
}

/* one texel as up to 4 floats; 0 = format not readable */
static int texel(DXGI_FORMAT f, const uint8_t* p, float* o)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        for (int i = 0; i < 4; i++)
            o[i] = p[i] / 255.0f;
        return 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        o[0] = p[2] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[0] / 255.0f, o[3] = p[3] / 255.0f;
        return 4;
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v;
        memcpy(&v, p, 4);
        o[0] = (v & 1023) / 1023.0f, o[1] = ((v >> 10) & 1023) / 1023.0f, o[2] = ((v >> 20) & 1023) / 1023.0f;
        o[3] = (v >> 30) / 3.0f;
        return 4;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int i = 0; i < 4; i++) {
            uint16_t h;
            memcpy(&h, p + 2 * i, 2);
            o[i] = half_to_float(h);
        }
        return 4;
    case DXGI_FORMAT_R11G11B10_FLOAT: {
        uint32_t v;
        memcpy(&v, p, 4);
        o[0] = uf_to_float(v & 2047, 6), o[1] = uf_to_float((v >> 11) & 2047, 6), o[2] = uf_to_float(v >> 22, 5);
        return 3;
    }
    case DXGI_FORMAT_R16G16_FLOAT:
        for (int i = 0; i < 2; i++) {
            uint16_t h;
            memcpy(&h, p + 2 * i, 2);
            o[i] = half_to_float(h);
        }
        return 2;
    case DXGI_FORMAT_R16_FLOAT: {
        uint16_t h;
        memcpy(&h, p, 2);
        o[0] = half_to_float(h);
        return 1;
    }
    case DXGI_FORMAT_R32_FLOAT:
        memcpy(o, p, 4);
        return 1;
    case DXGI_FORMAT_R32G32_FLOAT:
        memcpy(o, p, 8);
        return 2;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        memcpy(o, p, 16);
        return 4;
    case DXGI_FORMAT_R8_UNORM:
        o[0] = p[0] / 255.0f;
        return 1;
    case DXGI_FORMAT_R8G8_UNORM:
        o[0] = p[0] / 255.0f, o[1] = p[1] / 255.0f;
        return 2;
    case DXGI_FORMAT_R16_UNORM: {
        uint16_t v;
        memcpy(&v, p, 2);
        o[0] = v / 65535.0f;
        return 1;
    }
    case DXGI_FORMAT_R16G16_UNORM:
        for (int i = 0; i < 2; i++) {
            uint16_t v;
            memcpy(&v, p + 2 * i, 2);
            o[i] = v / 65535.0f;
        }
        return 2;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        for (int i = 0; i < 4; i++) {
            uint16_t v;
            memcpy(&v, p + 2 * i, 2);
            o[i] = v / 65535.0f;
        }
        return 4;
    default:
        return 0;
    }
}

static UINT texel_bytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R8G8_UNORM: return 2;
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 4;
    }
}

/* reads a mapped target into an RGB image (<= 960 px wide) and its per-channel value ranges */
static int convert(const D3D11_MAPPED_SUBRESOURCE* m, DXGI_FORMAT view, DXGI_FORMAT res, UINT w, UINT h, const char* path,
                   char* stats, size_t cap, UINT max_w)
{
    uint8_t zero[16] = {0};
    float t[4];
    DXGI_FORMAT f = texel(view, zero, t) ? view : res; /* the view's format, or the texture's if that is typeless */
    int nc = texel(f, (const uint8_t*)m->pData, t);
    if (!nc) {
        snprintf(stats, cap, "format %d not readable", (int)f);
        return 0;
    }
    UINT tb = texel_bytes(f), step = 1;
    while (max_w && w / step > max_w)
        step *= 2;
    UINT ow = w / step, oh = h / step;
    float mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    double sum[4] = {0};
    for (UINT y = 0; y < oh; y++)
        for (UINT x = 0; x < ow; x++) {
            texel(f, (const uint8_t*)m->pData + (size_t)y * step * m->RowPitch + (size_t)x * step * tb, t);
            for (int c = 0; c < nc; c++)
                if (isfinite(t[c])) {
                    mn[c] = fminf(mn[c], t[c]), mx[c] = fmaxf(mx[c], t[c]);
                    sum[c] += t[c];
                }
        }
    int n = 0;
    static const char ch[4] = {'R', 'G', 'B', 'A'};
    for (int c = 0; c < nc && n < (int)cap - 60; c++)
        n += snprintf(stats + n, cap - n, "%s%c %.3g..%.3g (mean %.3g)", c ? ", " : "", ch[c], mn[c], mx[c],
                      sum[c] / ((double)ow * oh));
    int is_float = f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT ||
                   f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT ||
                   f == DXGI_FORMAT_R32G32B32A32_FLOAT;
    int sgn = 0;
    float amax = 0;
    for (int c = 0; c < nc && c < 3; c++) {
        if (mn[c] < 0)
            sgn = 1;
        amax = fmaxf(amax, fmaxf(fabsf(mn[c]), fabsf(mx[c])));
    }
    uint8_t* rgb = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, (size_t)ow * oh * 3);
    if (!rgb)
        return 0;
    for (UINT y = 0; y < oh; y++)
        for (UINT x = 0; x < ow; x++) {
            texel(f, (const uint8_t*)m->pData + (size_t)y * step * m->RowPitch + (size_t)x * step * tb, t);
            uint8_t* o = rgb + ((size_t)y * ow + x) * 3;
            for (int c = 0; c < 3; c++) {
                float v = c < nc ? t[c] : (nc == 1 ? t[0] : 0.0f); /* single channel: grey */
                if (!isfinite(v))
                    v = 0;
                if (sgn && is_float)
                    v = 0.5f + 0.5f * (amax > 0 ? v / amax : 0);
                else if (is_float)
                    v = powf(v > 0 ? v / (1.0f + v) : 0.0f, 1.0f / 2.2f);
                o[c] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
            }
        }
    int ok = write_png(path, (int)ow, (int)oh, rgb);
    HeapFree(GetProcessHeap(), 0, rgb);
    return ok;
}

/* ---------------------------------------------------------------- writing the frame */
static ID3D11Resource* g_rid[1024]; /* resource ids for the index (R1, R2, ...) */
static int g_nrid;

static int rid(ID3D11Resource* r)
{
    for (int i = 0; i < g_nrid; i++)
        if (g_rid[i] == r)
            return i + 1;
    if (g_nrid < 1024)
        g_rid[g_nrid++] = r;
    return g_nrid;
}

static void write_frame(ID3D11DeviceContext* imm, int ef)
{
    CapPass* out = ef >= 0 ? g_ef_head[ef] : NULL;
    ID3D11Texture2D* fin = ef >= 0 ? g_ef_final[ef] : NULL;
    char dir[MAX_PATH], path[MAX_PATH];
    output_dir(dir, sizeof(dir), "capture");
    snprintf(path, sizeof(path), "%s\\capture.txt", dir);
    FILE* ix = fopen(path, "w");
    if (!ix) {
        LOGI("Capture: cannot write %s", path);
        return;
    }
    crc_init();
    g_nrid = 0;
    fprintf(ix, "MetaphorCPU %s frame capture. Passes in GPU execution order; R<n> = a texture (same id = same "
                "texture). 'in' = textures the pass's first draw reads (pixel shader slots t0..t15), with the pass "
                "that last wrote them in this frame. Passes with more than %d draws are listed without images.\n"
                "Written: recorded frame %d of %d; command lists run in it %d, of them not recorded %d.\n\n",
            MCPU_VERSION, CAP_MAX_DRAWS, ef + 1, g_ef_cur + 1, ef >= 0 ? g_ef_lists[ef] : 0,
            ef >= 0 ? g_ef_missing[ef] : 0);
    int idx = 0, imgs = 0;
    static uint64_t shaders[4096];
    int nsh = 0;
    for (CapPass* p = out; p; p = p->next, idx++) {
        char line[2048];
        int n = snprintf(line, sizeof(line), "p%03d %s draws %d (%u %s)%s", idx, p->deferred ? "deferred " : "immediate",
                         p->draws, p->verts, p->fullscreen ? "vertices, full-screen" : "indices/vertices",
                         p->depth ? "" : " no depth");
        if (p->depth)
            n += snprintf(line + n, sizeof(line) - n, " depth R%d", rid(p->depth));
        int ps = p->ps ? ps_find(p->ps) : -1;
        if (ps >= 0) {
            n += snprintf(line + n, sizeof(line) - n, " | PS %016llx", (unsigned long long)g_ps[ps].hash);
            int seen = 0;
            for (int i = 0; i < nsh; i++)
                seen |= shaders[i] == g_ps[ps].hash;
            if (!seen && nsh < 4096) {
                shaders[nsh++] = g_ps[ps].hash;
                char sp[MAX_PATH];
                snprintf(sp, sizeof(sp), "%s\\ps_%016llx.dxbc", dir, (unsigned long long)g_ps[ps].hash);
                FILE* sf = fopen(sp, "wb");
                if (sf) {
                    fwrite(g_ps[ps].code, 1, g_ps[ps].len, sf);
                    fclose(sf);
                }
            }
        } else {
            n += snprintf(line + n, sizeof(line) - n, " | PS %s", p->ps ? "unknown" : "none");
        }
        fprintf(ix, "%s\n", line);
        for (int i = 0; i < p->nsrv; i++) { /* inputs: which earlier pass wrote them */
            int from = -1, j = 0;
            for (CapPass* q = out; q && q != p; q = q->next, j++) {
                for (int k = 0; k < q->nrt; k++)
                    if (q->rt[k] == p->srv[i])
                        from = j;
                if (q->depth == p->srv[i])
                    from = j + 100000;
            }
            D3D11_RESOURCE_DIMENSION dim;
            ID3D11Resource_GetType(p->srv[i], &dim);
            char what[96] = "";
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d;
                ID3D11Texture2D_GetDesc((ID3D11Texture2D*)p->srv[i], &d);
                const char* fn = fmt_name(d.Format);
                snprintf(what, sizeof(what), "%ux%u%s %s", d.Width, d.Height, d.ArraySize > 1 ? " array" : "",
                         fn ? fn : "?");
            } else {
                snprintf(what, sizeof(what), "%s", dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? "3D texture" : "buffer / other");
            }
            if (from >= 100000)
                fprintf(ix, "      in t%u: R%d %s (depth of p%03d)\n", p->srv_slot[i], rid(p->srv[i]), what, from - 100000);
            else if (from >= 0)
                fprintf(ix, "      in t%u: R%d %s (written by p%03d)\n", p->srv_slot[i], rid(p->srv[i]), what, from);
            else
                fprintf(ix, "      in t%u: R%d %s (not written this frame)\n", p->srv_slot[i], rid(p->srv[i]), what);
        }
        for (int k = 0; k < p->nrt; k++) {
            const char* fn = fmt_name(p->rt_fmt[k]);
            char stats[400] = "";
            char img[64] = "-";
            if (p->staging[k]) {
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(imm, (ID3D11Resource*)p->staging[k], 0, D3D11_MAP_READ, 0, &m))) {
                    D3D11_TEXTURE2D_DESC sd;
                    ID3D11Texture2D_GetDesc(p->staging[k], &sd);
                    snprintf(img, sizeof(img), "p%03d_rt%d.png", idx, k);
                    snprintf(path, sizeof(path), "%s\\%s", dir, img);
                    if (convert(&m, p->rt_fmt[k], sd.Format, p->rt_w[k], p->rt_h[k], path, stats, sizeof(stats),
                                g_cfg.capture_full ? 0 : 960))
                        imgs++;
                    else
                        snprintf(img, sizeof(img), "(not written)");
                    ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)p->staging[k], 0);
                }
            } else if (k > 0 && !p->first_seen) {
                snprintf(img, sizeof(img), "(as before)");
            } else if (p->draws > CAP_MAX_DRAWS) {
                snprintf(img, sizeof(img), "(not copied: %d draws)", p->draws);
            }
            fprintf(ix, "      rt%d: R%d %ux%u %s -> %s %s\n", k, rid(p->rt[k]), p->rt_w[k], p->rt_h[k],
                    fn ? fn : "?", img, stats);
        }
        if (p->stepping && p->nsteps) { /* the draws that changed the middle of the screen */
            fprintf(ix, "      steps: the middle %ux%u at (%u,%u) after each draw; the draws that changed it:\n", p->sw, p->sh,
                    p->sx0, p->sy0);
            size_t rb = (size_t)p->sw * 8, n_px = (size_t)p->sw * p->sh;
            uint8_t* prev = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rb * p->sh);
            int have_prev = 0;
            for (int s = 0; s < p->nsteps && prev; s++) {
                CapStep* st = &p->steps[s];
                D3D11_MAPPED_SUBRESOURCE m;
                if (!st->st || FAILED(ID3D11DeviceContext_Map(imm, (ID3D11Resource*)st->st, 0, D3D11_MAP_READ, 0, &m)))
                    continue;
                size_t changed = 0;
                UINT bx0 = p->sw, by0 = p->sh, bx1 = 0, by1 = 0;
                for (UINT y = 0; y < p->sh; y++) {
                    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (UINT xx = 0; xx < p->sw; xx++)
                        if (!have_prev || memcmp(row + xx * 8, prev + y * rb + xx * 8, 8)) {
                            changed++;
                            if (xx < bx0) bx0 = xx;
                            if (y < by0) by0 = y;
                            if (xx > bx1) bx1 = xx;
                            if (y > by1) by1 = y;
                        }
                    memcpy(prev + y * rb, row, rb);
                }
                if (changed) {
                    int ps = st->ps ? ps_find(st->ps) : -1;
                    uint64_t h = ps >= 0 ? g_ps[ps].hash : 0;
                    char sn[64], sstats[400];
                    snprintf(sn, sizeof(sn), "p%03d_d%03d_%08llx.png", idx, s, (unsigned long long)(h >> 32));
                    snprintf(path, sizeof(path), "%s\\%s", dir, sn);
                    convert(&m, p->rt_fmt[0], p->rt_fmt[0], p->sw, p->sh, path, sstats, sizeof(sstats), 0);
                    fprintf(ix, "        draw %3d: PS %016llx, %u indices -> %s: %zu of %zu texels changed in (%u,%u)-(%u,%u)\n",
                            s, (unsigned long long)h, st->verts, sn, changed, n_px, bx0, by0, bx1, by1);
                    int seen = 0;
                    for (int i = 0; i < nsh; i++)
                        seen |= shaders[i] == h;
                    if (!seen && ps >= 0 && nsh < 4096) {
                        shaders[nsh++] = h;
                        char sp[MAX_PATH];
                        snprintf(sp, sizeof(sp), "%s\\ps_%016llx.dxbc", dir, (unsigned long long)h);
                        FILE* sf = fopen(sp, "wb");
                        if (sf) {
                            fwrite(g_ps[ps].code, 1, g_ps[ps].len, sf);
                            fclose(sf);
                        }
                    }
                }
                have_prev = 1;
                ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)st->st, 0);
            }
            if (prev)
                HeapFree(GetProcessHeap(), 0, prev);
        }
    }
    if (fin) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(imm, (ID3D11Resource*)fin, 0, D3D11_MAP_READ, 0, &m))) {
            char stats[400];
            snprintf(path, sizeof(path), "%s\\final.png", dir);
            if (convert(&m, g_final_fmt, g_final_fmt, g_final_w, g_final_h, path, stats, sizeof(stats),
                        g_cfg.capture_full ? 0 : 960))
                imgs++;
            fprintf(ix, "\nfinal (back buffer at Present) %ux%u %s -> final.png %s\n", g_final_w, g_final_h,
                    fmt_name(g_final_fmt) ? fmt_name(g_final_fmt) : "?", stats);
            ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)fin, 0);
        }
    }
    fprintf(ix, "\n%d passes, %d images, %d pixel shaders written (%ld seen created); %d targets left out (memory "
                "budget)\n",
            idx, imgs, nsh, g_ps_seen, g_dropped);
    fclose(ix);
    LOGI("Capture: recorded frame %d of %d written: %d passes, %d images, %d pixel shaders (%ld seen created) -> %s "
         "(command lists %d, not recorded %d; %d targets over budget)",
         ef + 1, g_ef_cur + 1, idx, imgs, nsh, g_ps_seen, dir, ef >= 0 ? g_ef_lists[ef] : 0,
         ef >= 0 ? g_ef_missing[ef] : 0, g_dropped);
}

static void reset_all(void)
{
    AcquireSRWLockExclusive(&g_cap_lock);
    for (int f = 0; f < EF_N; f++) {
        free_list(g_ef_head[f]);
        g_ef_head[f] = g_ef_tail[f] = NULL;
        g_ef_lists[f] = g_ef_missing[f] = 0;
        if (g_ef_final[f])
            ID3D11Texture2D_Release(g_ef_final[f]);
        g_ef_final[f] = NULL;
    }
    for (int i = 0; i < g_ncl; i++)
        free_list(g_cl[i].head);
    g_ncl = 0;
    ReleaseSRWLockExclusive(&g_cap_lock);
    g_dropped = 0;
    g_staging_bytes = 0;
}

void capture_clear_state(void* ctx)
{
    if (!capture_recording())
        return;
    CapCtx* x = cc_of(ctx);
    if (x) {
        end_pass(x);
        memset(x->srv, 0, sizeof(x->srv));
        x->ps = NULL;
    }
}

/* ---------------------------------------------------------------- compare shots */
/* [GpuProbe] CompareShots: F10 (F11 when Capture is on too) saves the final image, full size,
 * with each variant in turn; each is held for a few frames first (the game records its command
 * lists ahead). The INI's values come back afterwards. 1 = the post-processing options,
 * 2 = the far meshes (the game's, simpler with seams kept, simpler across seams). */
typedef struct {
    const char* name;
    int fog, glare, smaa, farmesh; /* -1: as set */
    float fmshadow;                /* -1: as set */
    float seams_from;              /* -1: as set */
    float outline;                 /* [Quality] OutlineDistance, -1: as set */
    int painterly;                 /* [Quality] Painterly, -1: as set */
    float rim;                     /* [Quality] RimLight distance (0 the game's, 0.001 none), -1: as set */
    float inner;                   /* [Quality] InnerOutlines distance (0 the game's, 0.001 none), -1: as set */
    int ctex;                      /* [Quality] CharacterTextureBias in quarter mip levels, -1: as set */
    float oldepth;                 /* [Quality] OutlineDepthOffset as a fraction of the distance, -1: as set */
    int charshadows;               /* [Quality] CharacterShadows, -1: as set */
    int hatch;                     /* [Quality] Hatching (0 the game's, 99 flat), -1: as set */
    int shadows;                   /* [Quality] Shadows, -1: as set */
    int knock_set;                 /* CompareShots=10: the draws of the pixel shader knock left out (0: as set) */
    uint64_t knock;
    int linemode, lineredraws;     /* [Quality] LineFix mode (0 the game's .. 3 left out) and copies, -1: as set */
    int wtex;                      /* [Quality] WorldTextureBias in quarter mip levels, -1: as set */
} CmpVariant;
static const CmpVariant g_cmp_fx[] = {{"game", 1, 1, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},        {"fog-off", 0, 1, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"glare-off", 1, 0, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},   {"fog-and-glare-off", 0, 0, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"smaa-high", 1, 1, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},   {"smaa-medium", 1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"smaa-off", 1, 1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=2: the game's meshes, seams kept, across seams from 150 m, across seams everywhere */
static const CmpVariant g_cmp_fm[] = {{"farmesh-off", -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"farmesh-safe", -1, -1, -1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"farmesh-seams-from-150m", -1, -1, -1, 2, -1, 150.0f, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"farmesh-seams-everywhere", -1, -1, -1, 2, -1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=3: the far meshes as set; their shadow casters by the screen / 0.5 / 1 texel */
static const CmpVariant g_cmp_fms[] = {{"shadows-by-screen", -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                       {"shadows-0.5-texel", -1, -1, -1, -1, 0.5f, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                       {"shadows-1-texel", -1, -1, -1, -1, 1.0f, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=4: the characters' ink outlines all drawn / left out beyond 50 m / beyond 30 m / all left out */
static const CmpVariant g_cmp_ol[] = {{"outlines-game", -1, -1, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"outlines-off-beyond-50m", -1, -1, -1, -1, -1, -1, 50, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"outlines-off-beyond-30m", -1, -1, -1, -1, -1, -1, 30, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                      {"outlines-all-off", -1, -1, -1, -1, -1, -1, 0.001f, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=5: the game / the painterly wobble and edges off / the characters' outlines off / both off */
static const CmpVariant g_cmp_paint[] = {{"game", -1, -1, -1, -1, -1, -1, 0, 1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                         {"painterly-off", -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                         {"character-outlines-off", -1, -1, -1, -1, -1, -1, 0.001f, 1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                         {"both-off", -1, -1, -1, -1, -1, -1, 0.001f, 0, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=6: shimmer - the game / the toon rim light off / rim only within 20 m / outlines and rim within 20 m /
 * the same with the painterly pass off */
static const CmpVariant g_cmp_rim[] = {{"game", -1, -1, -1, -1, -1, -1, 0, 1, 0, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                       {"rim-off", -1, -1, -1, -1, -1, -1, 0, 1, 0.001f, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                       {"rim-within-20m", -1, -1, -1, -1, -1, -1, 0, 1, 20, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                       {"outlines-and-rim-within-20m", -1, -1, -1, -1, -1, -1, 20, 1, 20, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                       {"outlines-and-rim-within-20m-painterly-off", -1, -1, -1, -1, -1, -1, 20, 0, 20, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=7: thin dark lines on characters - the Android defaults (SMAA High, outlines within 40 m, no rim) /
 * the outline lines over the character's own body (hair, sleeves, folds) off / those only within 15 m / the characters'
 * textures 0.5 and 1 mip level softer (painted lines) / inner lines within 15 m and textures 0.5 softer */
static const CmpVariant g_cmp_thin[] = {{"android-defaults", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                        {"inner-lines-off", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0.001f, 0, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                        {"inner-lines-within-15m", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 15, 0, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                        {"character-textures-0.5-softer", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 2, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                        {"character-textures-1-softer", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 4, -1, -1, -1, -1, 0, 0, -1, -1, -1},
                                        {"inner-lines-within-15m-textures-0.5-softer", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 15, 2, -1, -1, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=8: the thin broken lines in hair and clothing - the Android defaults / all character outlines off (which
 * lines the outline pass draws) / the outlines pushed back by 0.3%, 1% and 3% of their distance / outlines and the
 * characters' shadows off (what is left comes from the characters' own shading) */
static const CmpVariant g_cmp_lines[] = {
    {"android-defaults", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0, 1, -1, -1, 0, 0, -1, -1, -1},
    {"outlines-all-off", -1, -1, 2, -1, -1, -1, 0.001f, -1, 0.001f, 0, 0, 0, 1, -1, -1, 0, 0, -1, -1, -1},
    {"outline-depth-offset-0.3pct", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0.003f, 1, -1, -1, 0, 0, -1, -1, -1},
    {"outline-depth-offset-1pct", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0.01f, 1, -1, -1, 0, 0, -1, -1, -1},
    {"outline-depth-offset-3pct", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0.03f, 1, -1, -1, 0, 0, -1, -1, -1},
    {"outlines-and-character-shadows-off", -1, -1, 2, -1, -1, -1, 0.001f, -1, 0.001f, 0, 0, 0, 0, -1, -1, 0, 0, -1, -1, -1}};
/* CompareShots=9: the speckles in hair and clothing - the Android defaults / every shadow map empty / the toon
 * hatching flat (its average tone, no strokes) / hatching 2 mip levels softer / shadows off and hatching flat / that
 * with the outlines off too (what is left is the textures and the toon shading) */
static const CmpVariant g_cmp_speck[] = {
    {"android-defaults", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0, 1, 0, 1, 0, 0, -1, -1, -1},
    {"shadows-off", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, -1},
    {"hatching-flat", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0, 1, 99, 1, 0, 0, -1, -1, -1},
    {"hatching-2-levels-softer", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0, 1, 2, 1, 0, 0, -1, -1, -1},
    {"shadows-off-hatching-flat", -1, -1, 2, -1, -1, -1, 40, -1, 0.001f, 0, 0, 0, 1, 99, 0, 0, 0, -1, -1, -1},
    {"shadows-hatching-outlines-off", -1, -1, 2, -1, -1, -1, 0.001f, -1, 0.001f, 0, 0, 0, 1, 99, 0, 0, 0, -1, -1, -1}};
/* CompareShots=11: the characters' ink line meshes - as the game / drawn again shifted, 4 copies (continuous) / 8 copies
 * (thick, about 2 px) / left out */
static const CmpVariant g_cmp_line[] = {
    {"lines-game", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, -1},
    {"lines-redraw-4", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 1, 4, -1},
    {"lines-thick-8", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 1, 8, -1},
    {"lines-left-out", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 3, -1, -1}};
/* CompareShots=10: knock-out - the frame as set, then without the draws of one pixel shader at a time, for each of
 * the pixel shaders that drew the characters near the camera (most draws first; built when F11 is pressed) */
#define KO_N 16
static CmpVariant g_cmp_ko[KO_N];
static char g_cmp_ko_name[KO_N][24];
/* CompareShots=12: the world's textures as the game's / 0.5 / 1 mip level smaller (WorldTextureBias) */
static const CmpVariant g_cmp_wtex[] = {
    {"world-textures-game", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, 0},
    {"world-textures-0.5", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, 2},
    {"world-textures-1.0", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, -1, -1, 4}};
/* CompareShots=13: every change of RESULTS.md, the variants of [ABTest] Compare=features (ab.c switches them; held longer
 * for the stabilisation's history to settle) */
#define FEAT_N 32
#define FEAT_HOLD 20
static CmpVariant g_cmp_feat[FEAT_N];
static const CmpVariant* g_cmp = g_cmp_fx;
static int CMP_N = (int)(sizeof(g_cmp_fx) / sizeof(g_cmp_fx[0]));
#define CMP_HOLD 6
static int g_cmp_step = -1, g_cmp_frames, g_cmp_saved[4], g_cmp_count;
static float g_cmp_saved_fms, g_cmp_saved_sf, g_cmp_saved_ol;
static int g_cmp_saved_paint;
static float g_cmp_saved_rim, g_cmp_saved_inner, g_cmp_saved_old;
static int g_cmp_saved_ctex, g_cmp_saved_cs, g_cmp_saved_hatch, g_cmp_saved_sh;
static uint64_t g_cmp_saved_knock;
static int g_cmp_saved_lm, g_cmp_saved_lr;
static int g_cmp_saved_wtex;
static char g_cmp_dir[MAX_PATH];

static void cmp_apply(int i)
{
    const CmpVariant* v = &g_cmp[i];
    if (g_cfg.compare_shots == 13) {
        ab_shot_apply(i);
        return;
    }
    if (v->fog >= 0)
        g_cfg.q_fog = v->fog;
    if (v->glare >= 0)
        g_cfg.q_glare = v->glare;
    if (v->smaa >= 0)
        g_cfg.q_smaa = v->smaa;
    if (v->farmesh >= 0)
        g_cfg.q_farmesh = v->farmesh;
    if (v->fmshadow >= 0)
        g_cfg.q_fm_shadow = v->fmshadow;
    if (v->seams_from >= 0)
        g_cfg.q_fm_seams_from = v->seams_from;
    if (v->outline >= 0)
        g_cfg.q_outline_dist = v->outline;
    if (v->painterly >= 0)
        g_cfg.q_painterly = v->painterly;
    if (v->rim >= 0)
        g_cfg.q_rim_dist = v->rim;
    if (v->inner >= 0)
        g_cfg.q_inner_dist = v->inner;
    if (v->ctex >= 0)
        g_cfg.q_char_tex_q = v->ctex;
    if (v->oldepth >= 0)
        g_cfg.q_ol_depth = v->oldepth;
    if (v->charshadows >= 0)
        g_cfg.q_char_shadows = v->charshadows;
    if (v->hatch >= 0)
        g_cfg.q_hatch = v->hatch;
    if (v->shadows >= 0)
        g_cfg.q_shadows = v->shadows;
    if (v->knock_set)
        g_cfg.q_knock = v->knock;
    if (v->linemode >= 0)
        g_cfg.q_line_mode = v->linemode;
    if (v->lineredraws >= 0)
        g_cfg.q_line_redraws = v->lineredraws;
    if (v->wtex >= 0)
        g_cfg.q_world_tex_q = v->wtex;
}

static void compare_tick(void)
{
    if (g_cmp_step < 0) {
        static uint64_t t0;
        if (!t0)
            t0 = GetTickCount64();
        int key = (GetAsyncKeyState(g_cfg.capture ? VK_F11 : VK_F10) & 1) != 0;
        int timed = !g_cfg.capture && g_cfg.capture_after_sec > 0 && !g_cmp_count &&
                    GetTickCount64() - t0 > (uint64_t)g_cfg.capture_after_sec * 1000;
        if (!key && !timed)
            return;
        if (g_cfg.compare_shots == 13 && !ab_shot_count()) {
            LOGI("Compare shots: CompareShots=13 needs [ABTest] Enabled=1 and Compare=features");
            g_cmp_count++;
            return;
        }
        output_dir(g_cmp_dir, sizeof(g_cmp_dir), "compare");
        g_cmp_saved[0] = g_cfg.q_fog, g_cmp_saved[1] = g_cfg.q_glare, g_cmp_saved[2] = g_cfg.q_smaa;
        g_cmp_saved[3] = g_cfg.q_farmesh, g_cmp_saved_fms = g_cfg.q_fm_shadow, g_cmp_saved_sf = g_cfg.q_fm_seams_from;
        g_cmp_saved_ol = g_cfg.q_outline_dist, g_cmp_saved_paint = g_cfg.q_painterly, g_cmp_saved_rim = g_cfg.q_rim_dist;
        g_cmp_saved_inner = g_cfg.q_inner_dist, g_cmp_saved_ctex = g_cfg.q_char_tex_q;
        g_cmp_saved_old = g_cfg.q_ol_depth, g_cmp_saved_cs = g_cfg.q_char_shadows;
        g_cmp_saved_hatch = g_cfg.q_hatch, g_cmp_saved_sh = g_cfg.q_shadows;
        g_cmp_saved_knock = g_cfg.q_knock;
        g_cmp_saved_lm = g_cfg.q_line_mode, g_cmp_saved_lr = g_cfg.q_line_redraws;
        g_cmp_saved_wtex = g_cfg.q_world_tex_q;
        if (g_cfg.compare_shots == 13) {
            CMP_N = ab_shot_count();
            if (CMP_N > FEAT_N)
                CMP_N = FEAT_N;
            for (int i = 0; i < CMP_N; i++)
                g_cmp_feat[i].name = ab_shot_name(i);
            g_cmp = g_cmp_feat;
        } else if (g_cfg.compare_shots == 12) {
            g_cmp = g_cmp_wtex, CMP_N = (int)(sizeof(g_cmp_wtex) / sizeof(g_cmp_wtex[0]));
        } else if (g_cfg.compare_shots == 11) {
            g_cmp = g_cmp_line, CMP_N = (int)(sizeof(g_cmp_line) / sizeof(g_cmp_line[0]));
        } else if (g_cfg.compare_shots == 10) {
            uint64_t ps[KO_N - 1];
            int n = fx_near_char_ps(ps, KO_N - 1);
            static const CmpVariant as_set = {"", -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 0, -1, -1, -1};
            for (int i = 0; i <= n; i++) {
                g_cmp_ko[i] = as_set;
                g_cmp_ko[i].knock = i ? ps[i - 1] : 0;
                if (i)
                    snprintf(g_cmp_ko_name[i], sizeof(g_cmp_ko_name[i]), "no-%016llx", (unsigned long long)ps[i - 1]);
                else
                    snprintf(g_cmp_ko_name[i], sizeof(g_cmp_ko_name[i]), "as-set");
                g_cmp_ko[i].name = g_cmp_ko_name[i];
            }
            g_cmp = g_cmp_ko, CMP_N = n + 1;
            LOGI("Compare shots (knock-out): %d pixel shaders drew the characters within %.0f m; one image without each", n,
                 g_cfg.dump_dist);
        } else if (g_cfg.compare_shots == 9)
            g_cmp = g_cmp_speck, CMP_N = (int)(sizeof(g_cmp_speck) / sizeof(g_cmp_speck[0]));
        else if (g_cfg.compare_shots == 8)
            g_cmp = g_cmp_lines, CMP_N = (int)(sizeof(g_cmp_lines) / sizeof(g_cmp_lines[0]));
        else if (g_cfg.compare_shots == 7)
            g_cmp = g_cmp_thin, CMP_N = (int)(sizeof(g_cmp_thin) / sizeof(g_cmp_thin[0]));
        else if (g_cfg.compare_shots == 6)
            g_cmp = g_cmp_rim, CMP_N = (int)(sizeof(g_cmp_rim) / sizeof(g_cmp_rim[0]));
        else if (g_cfg.compare_shots == 5)
            g_cmp = g_cmp_paint, CMP_N = (int)(sizeof(g_cmp_paint) / sizeof(g_cmp_paint[0]));
        else if (g_cfg.compare_shots == 4)
            g_cmp = g_cmp_ol, CMP_N = (int)(sizeof(g_cmp_ol) / sizeof(g_cmp_ol[0]));
        else if (g_cfg.compare_shots == 2)
            g_cmp = g_cmp_fm, CMP_N = (int)(sizeof(g_cmp_fm) / sizeof(g_cmp_fm[0]));
        else if (g_cfg.compare_shots == 3)
            g_cmp = g_cmp_fms, CMP_N = (int)(sizeof(g_cmp_fms) / sizeof(g_cmp_fms[0]));
        else
            g_cmp = g_cmp_fx, CMP_N = (int)(sizeof(g_cmp_fx) / sizeof(g_cmp_fx[0]));
        crc_init();
        g_cmp_step = 0, g_cmp_frames = 0;
        cmp_apply(0);
        LOGI("Compare shots: started -> %s", g_cmp_dir);
        return;
    }
    if (g_cmp_tex) { /* this variant's frame was copied at the last Present */
        D3D11_MAPPED_SUBRESOURCE m;
        if (g_imm && SUCCEEDED(ID3D11DeviceContext_Map(g_imm, (ID3D11Resource*)g_cmp_tex, 0, D3D11_MAP_READ, 0, &m))) {
            char path[MAX_PATH], stats[400];
            snprintf(path, sizeof(path), "%s\\%d_%s.png", g_cmp_dir, g_cmp_step + 1, g_cmp[g_cmp_step].name);
            convert(&m, g_cmp_fmt, g_cmp_fmt, g_cmp_w, g_cmp_h, path, stats, sizeof(stats), 0);
            ID3D11DeviceContext_Unmap(g_imm, (ID3D11Resource*)g_cmp_tex, 0);
        }
        ID3D11Texture2D_Release(g_cmp_tex);
        g_cmp_tex = NULL;
        if (++g_cmp_step >= CMP_N) {
            g_cfg.q_fog = g_cmp_saved[0], g_cfg.q_glare = g_cmp_saved[1], g_cfg.q_smaa = g_cmp_saved[2];
            g_cfg.q_farmesh = g_cmp_saved[3], g_cfg.q_fm_shadow = g_cmp_saved_fms, g_cfg.q_fm_seams_from = g_cmp_saved_sf;
            g_cfg.q_outline_dist = g_cmp_saved_ol, g_cfg.q_painterly = g_cmp_saved_paint, g_cfg.q_rim_dist = g_cmp_saved_rim;
            g_cfg.q_inner_dist = g_cmp_saved_inner, g_cfg.q_char_tex_q = g_cmp_saved_ctex;
            g_cfg.q_ol_depth = g_cmp_saved_old, g_cfg.q_char_shadows = g_cmp_saved_cs;
            g_cfg.q_hatch = g_cmp_saved_hatch, g_cfg.q_shadows = g_cmp_saved_sh;
            g_cfg.q_knock = g_cmp_saved_knock;
            g_cfg.q_line_mode = g_cmp_saved_lm, g_cfg.q_line_redraws = g_cmp_saved_lr;
            g_cfg.q_world_tex_q = g_cmp_saved_wtex;
            if (g_cfg.compare_shots == 13)
                ab_shot_apply(0);
            g_cmp_step = -1;
            g_cmp_count++;
            LOGI("Compare shots: %d images written to %s", CMP_N, g_cmp_dir);
            return;
        }
        g_cmp_frames = 0;
        cmp_apply(g_cmp_step);
        return;
    }
    if (++g_cmp_frames >= (g_cfg.compare_shots == 13 ? FEAT_HOLD : CMP_HOLD))
        g_cmp_want = 1; /* the next Present copies the back buffer */
}

/* on the presenting thread, after Present */
void capture_frame_end(void)
{
    ID3D11DeviceContext* imm = g_imm;
    if (g_cfg.compare_shots)
        compare_tick();
    if (!g_cfg.capture)
        return;
    LONG st = g_cs;
    if (st == CS_IDLE) {
        if (!g_cap_start_ms)
            g_cap_start_ms = GetTickCount64();
        int key = (GetAsyncKeyState(VK_F10) & 1) != 0;
        int timed = g_cfg.capture_after_sec > 0 && !g_cap_count &&
                    GetTickCount64() - g_cap_start_ms > (uint64_t)g_cfg.capture_after_sec * 1000;
        if (key || timed) {
            reset_all();
            g_cs = CS_ARMED;
            LOGI("Capture: armed (%s); the next frames are recorded", key ? "F10" : "CaptureAfterSec");
        }
        return;
    }
    if (st == CS_DONE) { /* passes still open in other contexts are dropped when they end */
        if (imm)
            write_frame(imm, g_ef_out);
        else
            LOGI("Capture: no immediate context for the readback");
        reset_all();
        g_cap_count++;
        g_cs = CS_IDLE;
    }
}
