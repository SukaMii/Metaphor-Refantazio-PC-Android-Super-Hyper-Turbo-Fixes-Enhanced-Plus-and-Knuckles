/* [Quality] CharacterMotionVectors: how far each pixel of the characters moved since the previous frame,
 * for TemporalStabilise. The game draws no motion vectors: its character shaders write none (Luma patches them to).
 *
 * Here the opaque draws (no blending into the first target, depth tested) of the characters' pass (four targets, the
 * first RGBA16F, the fourth R32F, a depth buffer), from its first draw of a character itself on (not of what it carries),
 * are recorded as the game makes them (the particles' pass has the same targets but no character: tablet logs 27, 29
 * and 30, its draws drawn again cost 5-7 ms): the vertex and index buffers and ranges, and a GPU copy of the draw's transform
 * constants (GFD_VSCONST_TRANSFORM, its VS b1). When the pass ends they are drawn again on the same context with the
 * mod's shaders (src/shaders/motion_vs.hlsl, motion_ps.hlsl) into a motion target:
 *   - where the mesh is now: this draw's transform, as the game's vertex shaders compute it;
 *   - where it was: the same object's transform of the previous frame (the same buffers and index range) and, for the
 *     skinned meshes (vertex buffers the game's skinning writes: UAV-capable), a copy of the vertex buffer made after
 *     its previous frame's use. Copies alternate between two buffers per mesh, by frame.
 * Where the drawn distance matches the game's depth (within 2%: the game offsets its characters' depth a little), the
 * pixel gets its motion (pixels, back to where it was) and its distance then; the rest stays unknown (alpha 0). Frames are told apart by the scene hook's sequence (it counts the game's frame
 * slots), not by Present: the game records a frame while it presents the previous one. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <d3d11_1.h>
#include "shaders_gen.h"
#include "fxstate.h"

#define MV_MAX 512   /* draws recorded per frame */
#define KEY_N 2048   /* object -> transform slot, per frame */
#define SKIN_N 256   /* skinned vertex buffers with copies (the one drawn longest ago is reused when full) */
#define CROWD_HIST 64 /* frames: the most characters around in them decides MotionVectorSkinCrowd */
#define LAY_N 16     /* the mod's input layouts, by the position's format and offset */
#define MC_N 64      /* contexts */

typedef struct {
    void* ctx;
    ID3D11Buffer *vb, *ib, *pvb; /* referenced until the replay */
    UINT stride, off, ib_off, pos_off;
    DXGI_FORMAT ib_fmt, pos_fmt;
    UINT count, start;
    INT base;
    int slot, pslot; /* transform copies: this frame's, the previous frame's */
    int skin;        /* index into V.skin, -1: not skinned */
} Rec;

static SRWLOCK g_ml = SRWLOCK_INIT;
static struct {
    ID3D11Device* dev;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Buffer* cb;
    ID3D11RasterizerState* rs;
    ID3D11BlendState* bs_max; /* MotionVectorView: a rejected fragment never hides a matched one */
    struct {
        DXGI_FORMAT fmt;
        UINT off;
        ID3D11InputLayout* il;
    } lay[LAY_N];
    int nlay;
    ID3D11Buffer* xf[2][MV_MAX]; /* transform copies, by frame parity and slot */
    UINT xf_bytes;
    struct {
        uint64_t key;
        int slot;
    } keys[2][KEY_N];
    int nslot;     /* slots taken this frame */
    LONG64 seq;    /* the frame being recorded (scene sequence) */
    Rec rec[MV_MAX];
    int nrec;
    struct {
        ID3D11Buffer* vb; /* not referenced: compared only */
        ID3D11Buffer* copy[2];
        LONG64 seq[2]; /* the frame each copy holds */
        LONG64 used;   /* the last frame it was drawn in */
        UINT bytes;
    } skin[SKIN_N];
    int nskin;
    struct {
        LONG64 seq;
        int n;
    } crowd[CROWD_HIST]; /* the characters around, by frame */
    int crowd_most;      /* the most of them in the last CROWD_HIST frames */
    ID3D11Texture2D* mv;
    ID3D11RenderTargetView* mv_rtv;
    ID3D11ShaderResourceView* mv_srv;
    UINT w, h;
    struct {
        ID3D11Resource* res; /* referenced by the view */
        ID3D11ShaderResourceView* srv;
    } dsrv[4];
    LONG64 done;    /* replays so far: the stabiliser uses the target once per replay */
    uint64_t start_ps[4]; /* the pixel shaders that started the passes drawn again (the log: which passes) */
    LONG64 cleared; /* the frame the target was last cleared for (one clear a frame: the pass can end twice) */
    int failed;
} V;
static struct {
    void* ctx;
    int in;                    /* the characters' pass is bound */
    int chars;                 /* a character was drawn in it: from then on its draws are recorded */
    uint64_t start_ps;         /* the pixel shader of that first character draw (the log) */
    ID3D11DepthStencilView* dsv; /* its depth (not referenced) */
    D3D11_VIEWPORT vp;
} g_mc[MC_N];
static volatile LONG g_nmc;
static volatile LONG64 g_mv_draws, g_mv_replays, g_mv_skins, g_mv_noprev, g_mv_skin_bytes, g_mv_rigid, g_mv_reused;
/* draws left out: far, no scene object, a layout without a known position (or skinned in the vertex shader), the
 * subsurface-id redraw, not opaque (blended: the particles' pass has the same targets: tablet log 27, 28 Oct 2026, its
 * 142 draws drawn again cost 6.2 ms a frame), no room / no buffers */
static volatile LONG64 g_mv_far, g_mv_noobj, g_mv_lay, g_mv_sss, g_mv_transp, g_mv_nochar, g_mv_other;

static int mc_of(void* c)
{
    LONG n = g_nmc;
    for (LONG i = 0; i < n; i++)
        if (g_mc[i].ctx == c)
            return (int)i;
    AcquireSRWLockExclusive(&g_ml);
    int r = -1;
    for (LONG i = 0; i < g_nmc && r < 0; i++)
        if (g_mc[i].ctx == c)
            r = (int)i;
    if (r < 0 && g_nmc < MC_N) {
        r = (int)g_nmc;
        g_mc[r].ctx = c, g_mc[r].in = 0;
        InterlockedIncrement(&g_nmc);
    }
    ReleaseSRWLockExclusive(&g_ml);
    return r;
}

static DXGI_FORMAT view_format(ID3D11View* v, int rtv)
{
    if (!v)
        return DXGI_FORMAT_UNKNOWN;
    if (rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC d;
        ID3D11RenderTargetView_GetDesc((ID3D11RenderTargetView*)v, &d);
        return d.Format;
    }
    return DXGI_FORMAT_UNKNOWN;
}

static int mv_make_shaders(ID3D11Device* dev)
{
    if (V.dev == dev && V.ps)
        return 1;
    if (V.failed)
        return 0;
    REL(V.vs), REL(V.ps), REL(V.cb), REL(V.rs), REL(V.bs_max);
    D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    D3D11_BLEND_DESC bl;
    memset(&bl, 0, sizeof(bl));
    bl.RenderTarget[0].BlendEnable = TRUE, bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].SrcBlendAlpha = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    if (FAILED(ID3D11Device_CreateVertexShader(dev, g_sh_motion_vs, sizeof(g_sh_motion_vs), NULL, &V.vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(dev, g_sh_motion_ps, sizeof(g_sh_motion_ps), NULL, &V.ps)) ||
        FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &V.cb)) ||
        FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &V.rs)) ||
        FAILED(ID3D11Device_CreateBlendState(dev, &bl, &V.bs_max))) {
        REL(V.vs), REL(V.ps), REL(V.cb), REL(V.rs), REL(V.bs_max);
        V.failed = 1;
        LOGE("Motion vectors: their shaders could not be made; off");
        return 0;
    }
    V.dev = dev;
    return 1;
}

static ID3D11InputLayout* layout_for(DXGI_FORMAT fmt, UINT off)
{
    for (int i = 0; i < V.nlay; i++)
        if (V.lay[i].fmt == fmt && V.lay[i].off == off)
            return V.lay[i].il;
    if (V.nlay >= LAY_N)
        return NULL;
    D3D11_INPUT_ELEMENT_DESC e[2] = {{"POSITION", 0, fmt, 0, off, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                     {"TEXCOORD", 7, fmt, 1, off, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11InputLayout* il = NULL;
    if (FAILED(ID3D11Device_CreateInputLayout(V.dev, e, 2, g_sh_motion_vs, sizeof(g_sh_motion_vs), &il)))
        il = NULL;
    V.lay[V.nlay].fmt = fmt, V.lay[V.nlay].off = off, V.lay[V.nlay].il = il, V.nlay++;
    return il;
}

static uint64_t rec_key(const Rec* r)
{
    uint64_t k = (uint64_t)(uintptr_t)r->vb * 0x9E3779B97F4A7C15ull;
    k ^= ((uint64_t)r->off << 32 | r->start) * 0xC2B2AE3D27D4EB4Full;
    k ^= ((uint64_t)r->count << 32 | (uint32_t)r->base) * 0x165667B19E3779F9ull;
    k ^= (uint64_t)(uintptr_t)r->ib * 0x27D4EB2F165667C5ull;
    return k ? k : 1;
}

static int key_find(int parity, uint64_t k)
{
    unsigned h = (unsigned)(k >> 7) & (KEY_N - 1);
    for (int i = 0; i < KEY_N; i++, h = (h + 1) & (KEY_N - 1)) {
        if (V.keys[parity][h].key == k)
            return V.keys[parity][h].slot;
        if (!V.keys[parity][h].key)
            return -1;
    }
    return -1;
}

static void key_put(int parity, uint64_t k, int slot)
{
    unsigned h = (unsigned)(k >> 7) & (KEY_N - 1);
    for (int i = 0; i < KEY_N; i++, h = (h + 1) & (KEY_N - 1))
        if (!V.keys[parity][h].key || V.keys[parity][h].key == k) {
            V.keys[parity][h].key = k, V.keys[parity][h].slot = slot;
            return;
        }
}

/* the copy of a skinned vertex buffer (made after its use in frame `seq`), -1 if none can be made */
static int skin_of(ID3D11Buffer* vb, UINT bytes)
{
    int old = -1;
    for (int i = 0; i < V.nskin; i++) {
        if (V.skin[i].vb == vb && V.skin[i].bytes == bytes) {
            V.skin[i].used = V.seq;
            return i;
        }
        if (V.skin[i].used < V.seq && (old < 0 || V.skin[i].used < V.skin[old].used))
            old = i;
    }
    /* full: the entry drawn longest ago is reused (one of this frame's is not: its draws are not drawn again yet). The
     * game makes new buffers for the characters of each place and fight: the table filled up in the Q48 PC run (29 Oct
     * 2026), and in the fight that followed no character had its limbs' motion. */
    int i = V.nskin;
    if (i >= SKIN_N) {
        if (old < 0)
            return -1;
        i = old;
        if (V.skin[i].bytes != bytes)
            REL(V.skin[i].copy[0]), REL(V.skin[i].copy[1]);
        g_mv_reused++;
    } else {
        V.nskin++;
    }
    D3D11_BUFFER_DESC cd = {bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    for (int k = 0; k < 2; k++)
        if (!V.skin[i].copy[k] && FAILED(ID3D11Device_CreateBuffer(V.dev, &cd, NULL, &V.skin[i].copy[k]))) {
            REL(V.skin[i].copy[0]), REL(V.skin[i].copy[1]);
            V.skin[i].vb = NULL, V.skin[i].bytes = 0, V.skin[i].used = -9;
            return -1;
        }
    /* its copies hold nothing of this buffer yet */
    V.skin[i].vb = vb, V.skin[i].bytes = bytes, V.skin[i].seq[0] = V.skin[i].seq[1] = -9, V.skin[i].used = V.seq;
    return i;
}

/* [Quality] MotionVectorSkinCrowd: the most characters around in the last CROWD_HIST frames. One frame's count swings
 * with the camera in a town (5-18 in the Q48 PC log, 29 Oct 2026); a fight's stayed at 0-6. */
static int crowd_note(LONG64 seq, int n)
{
    int k = (int)(seq & (CROWD_HIST - 1)); /* a power of two */
    V.crowd[k].seq = seq, V.crowd[k].n = n;
    int most = -1;
    for (int i = 0; i < CROWD_HIST; i++)
        if (V.crowd[i].seq > seq - CROWD_HIST && V.crowd[i].seq <= seq && V.crowd[i].n > most)
            most = V.crowd[i].n;
    return most;
}

/* a view of the depth buffer for the pixel shader's test (cached by texture) */
static ID3D11ShaderResourceView* depth_view(ID3D11DepthStencilView* dsv)
{
    ID3D11Resource* r = NULL;
    ID3D11DepthStencilView_GetResource(dsv, &r);
    if (!r)
        return NULL;
    ID3D11ShaderResourceView* out = NULL;
    for (int i = 0; i < 4 && !out; i++)
        if (V.dsrv[i].res == r)
            out = V.dsrv[i].srv;
    if (!out) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        ID3D11DepthStencilView_GetDesc(dsv, &dd);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.Format = dd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT    ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                    : dd.Format == DXGI_FORMAT_D32_FLOAT           ? DXGI_FORMAT_R32_FLOAT
                    : dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                    : dd.Format == DXGI_FORMAT_D16_UNORM           ? DXGI_FORMAT_R16_UNORM
                                                                   : DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
        if (sd.Format != DXGI_FORMAT_UNKNOWN && dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D &&
            SUCCEEDED(ID3D11Device_CreateShaderResourceView(V.dev, r, &sd, &out))) {
            REL(V.dsrv[3].srv), REL(V.dsrv[3].res);
            memmove(&V.dsrv[1], &V.dsrv[0], 3 * sizeof(V.dsrv[0]));
            V.dsrv[0].res = r, V.dsrv[0].srv = out;
            r = NULL; /* the cache keeps the reference */
        } else {
            out = NULL;
        }
    }
    REL(r);
    return out;
}

static int make_target(UINT w, UINT h)
{
    if (V.mv && V.w == w && V.h == h)
        return 1;
    REL(V.mv_srv), REL(V.mv_rtv), REL(V.mv);
    V.w = V.h = 0;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(V.dev, &d, NULL, &V.mv)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(V.dev, (ID3D11Resource*)V.mv, NULL, &V.mv_rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(V.dev, (ID3D11Resource*)V.mv, NULL, &V.mv_srv))) {
        REL(V.mv_srv), REL(V.mv_rtv), REL(V.mv);
        return 0;
    }
    V.w = w, V.h = h;
    LOGI("Motion vectors: target for the %ux%u characters' pass made", w, h);
    return 1;
}

/* the recorded draws of this context drawn again into the motion target (the lock is held) */
static void replay(ID3D11DeviceContext* c, int mi)
{
    int n = 0;
    for (int i = 0; i < V.nrec; i++)
        n += V.rec[i].ctx == c;
    if (!n)
        return;
    D3D11_VIEWPORT vp = g_mc[mi].vp;
    ID3D11ShaderResourceView* dsrv = g_mc[mi].dsv ? depth_view(g_mc[mi].dsv) : NULL;
    int ok = dsrv && vp.Width >= 1 && vp.Height >= 1 &&
             make_target((UINT)(vp.TopLeftX + vp.Width + 0.5f), (UINT)(vp.TopLeftY + vp.Height + 0.5f));
    int parity = (int)(V.seq & 1);
    if (ok) {
        FxState s;
        fx_save(c, &s);
        static const float zero[4] = {0, 0, 0, 0};
        if (V.cleared != V.seq) {
            M(ClearRTV_t, c, ClearRenderTargetView)(c, V.mv_rtv, zero);
            V.cleared = V.seq;
        }
        M(SetSRV_t, c, PSSetShaderResources)(c, 0, 16, (ID3D11ShaderResourceView* const[16]){NULL});
        M(OMSetRT_t, c, OMSetRenderTargets)(c, 1, &V.mv_rtv, NULL);
        M(SetVP_t, c, RSSetViewports)(c, 1, &vp);
        M(SetRS_t, c, RSSetState)(c, V.rs);
        M(SetBlend_t, c, OMSetBlendState)(c, g_cfg.q_mv_view ? V.bs_max : NULL, NULL, 0xffffffffu);
        M(SetDS_t, c, OMSetDepthStencilState)(c, NULL, 0);
        M(SetTopo_t, c, IASetPrimitiveTopology)(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        M(SetSh_t, c, VSSetShader)(c, V.vs, NULL, 0);
        M(SetSh_t, c, GSSetShader)(c, NULL, NULL, 0);
        M(SetSh_t, c, HSSetShader)(c, NULL, NULL, 0);
        M(SetSh_t, c, DSSetShader)(c, NULL, NULL, 0);
        M(SetSh_t, c, PSSetShader)(c, V.ps, NULL, 0);
        float a = 0, b = 0;
        shadowcull_depth_proj(&a, &b);
        float k[8] = {vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, b != 0 ? 0.02f : 1.0f, a, b,
                      g_cfg.q_mv_view ? 1.0f : 0.0f};
        M(UpdateSub_t, c, UpdateSubresource)(c, (ID3D11Resource*)V.cb, 0, NULL, k, 0, 0);
        M(SetCB_t, c, PSSetConstantBuffers)(c, 0, 1, &V.cb);
        M(SetSRV_t, c, PSSetShaderResources)(c, 0, 1, &dsrv);
        for (int i = 0; i < V.nrec; i++) {
            Rec* r = &V.rec[i];
            if (r->ctx != c || r->pslot < 0)
                continue;
            ID3D11InputLayout* il = layout_for(r->pos_fmt, r->pos_off);
            if (!il)
                continue;
            ID3D11Buffer* vbs[2] = {r->vb, r->pvb ? r->pvb : r->vb};
            UINT st[2] = {r->stride, r->stride}, of[2] = {r->off, r->off};
            ID3D11Buffer* cbs[2] = {V.xf[parity][r->slot], V.xf[parity ^ 1][r->pslot]};
            M(SetIL_t, c, IASetInputLayout)(c, il);
            M(SetVB_t, c, IASetVertexBuffers)(c, 0, 2, vbs, st, of);
            M(SetIB_t, c, IASetIndexBuffer)(c, r->ib, r->ib_fmt, r->ib_off);
            M(SetCB_t, c, VSSetConstantBuffers)(c, 0, 2, cbs);
            M(DrawIndexed_t, c, DrawIndexed)(c, r->count, r->start, r->base);
            g_mv_draws++;
        }
        /* this frame's skinned positions, for the next frame (after the draws that read the previous ones) */
        for (int i = 0; i < V.nrec; i++) {
            Rec* r = &V.rec[i];
            if (r->ctx != c || r->skin < 0 || V.skin[r->skin].seq[parity] == V.seq)
                continue;
            M(CopyRes_t, c, CopyResource)(c, (ID3D11Resource*)V.skin[r->skin].copy[parity], (ID3D11Resource*)r->vb);
            V.skin[r->skin].seq[parity] = V.seq;
            g_mv_skins++, g_mv_skin_bytes += V.skin[r->skin].bytes;
        }
        fx_restore(c, &s);
        V.done++;
        g_mv_replays++;
        uint64_t sp = g_mc[mi].start_ps;
        int seen = 0;
        for (int i = 0; i < 4; i++)
            seen |= V.start_ps[i] == sp;
        if (!seen) {
            memmove(&V.start_ps[1], &V.start_ps[0], 3 * sizeof(V.start_ps[0]));
            V.start_ps[0] = sp;
        }
    }
    int w = 0; /* the other contexts' records stay */
    for (int i = 0; i < V.nrec; i++) {
        Rec* r = &V.rec[i];
        if (r->ctx == c) {
            REL(r->vb), REL(r->ib), REL(r->pvb);
        } else {
            V.rec[w++] = *r;
        }
    }
    V.nrec = w;
}

/* the targets bound: is it the characters' pass? Leaving it, its draws are drawn again */
void motion_bind(void* ctx, unsigned n, void* const* rtvs, void* dsv)
{
    int mi = mc_of(ctx);
    if (mi < 0)
        return;
    int in = n >= 4 && rtvs && rtvs[0] && rtvs[3] && dsv &&
             view_format((ID3D11View*)rtvs[0], 1) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
             view_format((ID3D11View*)rtvs[3], 1) == DXGI_FORMAT_R32_FLOAT;
    if (g_mc[mi].in && !in) {
        AcquireSRWLockExclusive(&g_ml);
        replay((ID3D11DeviceContext*)ctx, mi);
        ReleaseSRWLockExclusive(&g_ml);
    }
    if (in && !g_mc[mi].in)
        g_mc[mi].vp.Width = 0, g_mc[mi].chars = 0, g_mc[mi].start_ps = 0; /* taken at the pass's first draw */
    g_mc[mi].in = in;
    if (in)
        g_mc[mi].dsv = (ID3D11DepthStencilView*)dsv;
}

/* the context's command list is finished or its state cleared: a pass still open ends here */
void motion_flush(void* ctx)
{
    int mi = mc_of(ctx);
    if (mi < 0 || !g_mc[mi].in)
        return;
    AcquireSRWLockExclusive(&g_ml);
    replay((ID3D11DeviceContext*)ctx, mi);
    ReleaseSRWLockExclusive(&g_ml);
    g_mc[mi].in = 0;
}

/* after an indexed draw of the game */
void motion_draw(void* ctx, unsigned count, unsigned inst, unsigned start, int base)
{
    int mi = mc_of(ctx);
    if (!g_cfg.q_mv || mi < 0 || !g_mc[mi].in || inst > 1 || count < 3)
        return;
    int carried = 0;
    float d = gpuprobe_node_dist(&carried);
    LONG64 seq = gpuprobe_node_seq();
    if (d == -1 || seq <= 0) {
        g_mv_noobj++;
        return;
    }
    if (d >= 0 && !carried) /* a character itself (a skinned mesh): not what it carries, which can be an effect near
                             * it in the particles' pass (tablet log 30: OutlineCarried counts objects in its sphere) */
        g_mc[mi].chars = 1, g_mc[mi].start_ps = g_mc[mi].start_ps ? g_mc[mi].start_ps : effects_bound_ps_hash(ctx);
    else if (!g_mc[mi].chars) { /* no character drawn in this pass yet: the particles' pass, the sky's */
        g_mv_nochar++;
        return;
    }
    if (g_cfg.q_mv_dist > 0 && d > g_cfg.q_mv_dist) {
        g_mv_far++;
        return;
    }
    if (effects_bound_ps_skip(ctx)) {
        g_mv_sss++;
        return;
    }
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    { /* opaque only: blended into the first target (particles, effects) or not depth tested, it is no surface the depth
       * buffer holds. Depth writes may be off: the characters are drawn after a depth pre-pass (tablet log 28, 0.48.3
       * required them and kept 6 draws of ~80) */
        ID3D11BlendState* bs = NULL;
        ID3D11DepthStencilState* dss = NULL;
        FLOAT bf[4];
        UINT mask = 0, ref = 0;
        ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
        ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &ref);
        int skip = 0;
        if (bs) { /* blending that only overwrites (one, zero) is no blending */
            D3D11_BLEND_DESC bd;
            ID3D11BlendState_GetDesc(bs, &bd);
            const D3D11_RENDER_TARGET_BLEND_DESC* t = &bd.RenderTarget[0];
            skip |= t->BlendEnable && !(t->SrcBlend == D3D11_BLEND_ONE && t->DestBlend == D3D11_BLEND_ZERO &&
                                        t->BlendOp == D3D11_BLEND_OP_ADD);
        }
        if (dss) { /* NULL: the default state, which tests depth */
            D3D11_DEPTH_STENCIL_DESC dd;
            ID3D11DepthStencilState_GetDesc(dss, &dd);
            skip |= !dd.DepthEnable;
        }
        REL(bs), REL(dss);
        if (skip) {
            g_mv_transp++;
            return;
        }
    }
    ID3D11InputLayout* lay = NULL;
    ID3D11DeviceContext_IAGetInputLayout(c, &lay);
    int pfmt = 0, poff = 0, pslot = -1, skinned_layout = 0;
    int ok = lay && mesh_layout_pos(lay, &pfmt, &poff, &pslot, &skinned_layout) && pslot >= 0 && pslot < 16 &&
             !skinned_layout;
    REL(lay);
    if (!ok) {
        g_mv_lay++;
        return;
    }
    Rec r;
    memset(&r, 0, sizeof(r));
    r.ctx = ctx, r.pos_fmt = (DXGI_FORMAT)pfmt, r.pos_off = (UINT)poff, r.count = count, r.start = start, r.base = base;
    ID3D11DeviceContext_IAGetVertexBuffers(c, (UINT)pslot, 1, &r.vb, &r.stride, &r.off);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &r.ib, &r.ib_fmt, &r.ib_off);
    ID3D11Buffer* cb = NULL;
    ID3D11DeviceContext_VSGetConstantBuffers(c, 1, 1, &cb);
    D3D11_BUFFER_DESC cbd, vbd;
    memset(&cbd, 0, sizeof(cbd)), memset(&vbd, 0, sizeof(vbd));
    if (cb)
        ID3D11Buffer_GetDesc(cb, &cbd);
    if (r.vb)
        ID3D11Buffer_GetDesc(r.vb, &vbd);
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext_GetDevice(c, &dev);
    AcquireSRWLockExclusive(&g_ml);
    ok = r.vb && r.ib && cb && cbd.ByteWidth >= 128 && dev && mv_make_shaders(dev) && V.nrec < MV_MAX;
    if (ok && seq != V.seq) { /* a new frame: its slots and keys start afresh, the last frame's become the previous */
        int p = (int)(seq & 1);
        if (seq != V.seq + 1) /* frames missed: nothing to match */
            memset(V.keys[p ^ 1], 0, sizeof(V.keys[p ^ 1]));
        memset(V.keys[p], 0, sizeof(V.keys[p]));
        V.seq = seq, V.nslot = 0;
        V.crowd_most = crowd_note(seq, shadowcull_crowd());
    }
    if (ok && V.xf_bytes != cbd.ByteWidth) { /* the copies take the size of the game's buffer (CopyResource) */
        for (int p = 0; p < 2; p++)
            for (int i = 0; i < MV_MAX; i++)
                REL(V.xf[p][i]);
        memset(V.keys, 0, sizeof(V.keys));
        V.xf_bytes = cbd.ByteWidth, V.nslot = 0;
    }
    int parity = (int)(V.seq & 1);
    if (ok && V.nslot < MV_MAX) {
        r.slot = V.nslot;
        if (!V.xf[parity][r.slot]) {
            D3D11_BUFFER_DESC xd = {cbd.ByteWidth, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (FAILED(ID3D11Device_CreateBuffer(dev, &xd, NULL, &V.xf[parity][r.slot])))
                ok = 0;
        }
    } else {
        ok = 0;
    }
    if (ok) {
        V.nslot++;
        uint64_t key = rec_key(&r);
        key_put(parity, key, r.slot);
        r.pslot = key_find(parity ^ 1, key);
        if (r.pslot >= 0 && !V.xf[parity ^ 1][r.pslot])
            r.pslot = -1;
        if (r.pslot < 0)
            g_mv_noprev++;
        /* skinned (written by the game's skinning: UAV-capable); beyond MotionVectorSkinDistance its transform's
         * motion alone: the same buffer for both positions, no copy */
        int skinned = (vbd.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        int crowd = V.crowd_most; /* a fight (few characters): every character keeps its limbs' motion */
        if (skinned && g_cfg.q_mv_skin_dist > 0 && d > g_cfg.q_mv_skin_dist && (g_cfg.q_mv_crowd <= 0 || crowd > g_cfg.q_mv_crowd))
            skinned = 0, g_mv_rigid++;
        r.skin = skinned ? skin_of(r.vb, vbd.ByteWidth) : -1;
        if (r.skin >= 0) {
            if (V.skin[r.skin].seq[parity ^ 1] == V.seq - 1)
                r.pvb = V.skin[r.skin].copy[parity ^ 1], ID3D11Buffer_AddRef(r.pvb);
            else
                r.pslot = -1; /* no previous positions yet */
        }
        M(CopyRes_t, c, CopyResource)(c, (ID3D11Resource*)V.xf[parity][r.slot], (ID3D11Resource*)cb);
        V.rec[V.nrec++] = r;
        r.vb = r.ib = r.pvb = NULL;
        if (!g_mc[mi].vp.Width) {
            UINT nv = 1;
            ID3D11DeviceContext_RSGetViewports(c, &nv, &g_mc[mi].vp);
        }
    }
    ReleaseSRWLockExclusive(&g_ml);
    if (!ok)
        g_mv_other++;
    REL(r.vb), REL(r.ib), REL(r.pvb), REL(cb), REL(dev);
}

/* for the stabiliser: the motion target, if a replay filled it since the last call and it has this size */
void* motion_view(unsigned w, unsigned h)
{
    static LONG64 used;
    ID3D11ShaderResourceView* v = NULL;
    AcquireSRWLockShared(&g_ml);
    if (V.mv_srv && V.done != used && V.w == w && V.h == h)
        v = V.mv_srv, ID3D11ShaderResourceView_AddRef(v);
    LONG64 d = V.done;
    ReleaseSRWLockShared(&g_ml);
    used = d;
    return v;
}

void motion_report(void)
{
    static LONG64 pd, pr, pk, pn, pf, po, pl, pq, pt, pc, px, pb, pg;
    LONG64 bytes = g_mv_skin_bytes - pb, rigid = g_mv_rigid - pg;
    pb = g_mv_skin_bytes, pg = g_mv_rigid;
    LONG64 d = g_mv_draws - pd, r = g_mv_replays - pr, k = g_mv_skins - pk, n = g_mv_noprev - pn, f = g_mv_far - pf,
           o = g_mv_noobj - po, l = g_mv_lay - pl, q = g_mv_sss - pq, t = g_mv_transp - pt, c = g_mv_nochar - pc,
           x = g_mv_other - px;
    pd = g_mv_draws, pr = g_mv_replays, pk = g_mv_skins, pn = g_mv_noprev, pf = g_mv_far, po = g_mv_noobj;
    pl = g_mv_lay, pq = g_mv_sss, pt = g_mv_transp, pc = g_mv_nochar, px = g_mv_other;
    float a = 0, b = 0;
    shadowcull_depth_proj(&a, &b);
    LOGI("  Motion vectors: %lld passes drawn again in this window, %lld draws (%.0f per pass), %lld skinned buffers "
         "copied (%.1f MB a pass), %lld skinned draws beyond %.0f m without their own motion (%d characters within %.0f "
         "m, the most in the last %d frames %d: the skin distance %s) | draws left out: far %lld, no scene object %lld, before any character in their pass %lld, layout "
         "%lld, subsurface ids %lld, not opaque %lld, other %lld; first seen (no previous frame) %lld | %d skinned "
         "meshes (%lld reused), %d layouts, target %ux%u, depth mapping a %.4g b %.4g | passes started by a character drawn with PS "
         "%016llx %016llx %016llx",
         (long long)r, (long long)d, r ? (double)d / (double)r : 0.0, (long long)k,
         r ? (double)bytes / (double)r / 1048576.0 : 0.0, (long long)rigid, g_cfg.q_mv_skin_dist, shadowcull_crowd(),
         g_cfg.q_mv_dist, CROWD_HIST, V.crowd_most,
         g_cfg.q_mv_skin_dist <= 0 ? "off" : g_cfg.q_mv_crowd <= 0 || V.crowd_most > g_cfg.q_mv_crowd ? "applies"
                                                                                                    : "off (a fight)",
         (long long)f, (long long)o,
         (long long)c, (long long)l, (long long)q, (long long)t, (long long)x, (long long)n, V.nskin, (long long)g_mv_reused, V.nlay, V.w, V.h,
         a, b, (unsigned long long)V.start_ps[0], (unsigned long long)V.start_ps[1], (unsigned long long)V.start_ps[2]);
}
