/* [Quality] TemporalStabilise: the characters' pixels blended with the previous frame, just before
 * the game's SMAA (src/shaders/stabilise.hlsl has the method).
 *
 * It runs on the context that records the SMAA edge pass, before that pass's draw: the image (the edge shader's t0)
 * is copied, and one full-screen draw writes the image back (render target 0) and the history for the next frame
 * (target 1; two textures used in turn). The character flags, the depth and the game's view constants
 * (GFD_PSCONST_SYSTEM) come from the painterly pass (its t3, t2 and b0: the same objects every frame). The view
 * constants are copied on the GPU, at the pass, into one of two buffers used in turn with the history: the shader
 * gets this frame's view and the previous one, in step with the image whatever thread recorded what. The context's
 * state is saved and set back, and every call goes to the context's original methods: through the mod's hooks the
 * pass would count as the game's. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <stddef.h>
#include <string.h>
#include "shaders_gen.h"
#include "fxstate.h"

typedef struct {
    float weight, valid, depth_tol, motion_k;
    int w, h;
    float weight_world, motion_k_world;
    float mv_on, mv_view, world_cross, pad;
} StabConst;
#define SYS_BYTES 288 /* GFD_PSCONST_SYSTEM up to invProjParams (c17) */

static SRWLOCK g_tl = SRWLOCK_INIT;
static ID3D11ShaderResourceView *g_flags, *g_depth; /* the painterly pass's gbuffer1 and depth (referenced) */
static ID3D11Buffer* g_sys;                         /* and its GFD_PSCONST_SYSTEM (referenced) */
static struct {
    ID3D11Device* dev;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Buffer* cb;
    ID3D11RasterizerState* rs;
    ID3D11SamplerState* samp; /* linear, clamped: the history between texels */
    ID3D11Buffer* cam[2];     /* the view constants of the frame whose history is hist[i] */
    UINT cam_bytes;
    struct {
        ID3D11Resource* res; /* an image the game gave SMAA (referenced by the view) */
        ID3D11RenderTargetView* rtv;
        LONG64 used;
    } img[4];              /* the game may use more than one texture for it (it reuses its scratch targets) */
    UINT w, h;             /* the size and format of the history */
    DXGI_FORMAT fmt;
    ID3D11Texture2D* scratch; /* this frame's image, copied */
    ID3D11ShaderResourceView* scratch_srv;
    ID3D11Texture2D* hist[2];
    ID3D11ShaderResourceView* hist_srv[2];
    ID3D11RenderTargetView* hist_rtv[2];
    int cur, have;      /* hist[cur] is written next; `have`: hist[cur ^ 1] holds the previous frame */
    ULONGLONG last_ms;  /* the last pass, for the history's age */
    int failed;         /* the shaders could not be made: never again */
} T;
static volatile LONG64 g_t_runs, g_t_missing, g_t_resets, g_t_images, g_t_mv;
static volatile LONG g_t_off; /* F7 */

static void drop_targets(void)
{
    for (int i = 0; i < 4; i++)
        REL(T.img[i].rtv), T.img[i].res = NULL;
    REL(T.scratch_srv), REL(T.scratch);
    for (int i = 0; i < 2; i++)
        REL(T.hist_rtv[i]), REL(T.hist_srv[i]), REL(T.hist[i]);
    T.have = 0, T.w = T.h = 0;
}

static int make_shaders(ID3D11Device* dev)
{
    if (T.dev == dev && T.ps)
        return 1;
    if (T.failed)
        return 0;
    drop_targets();
    REL(T.cam[0]), REL(T.cam[1]), T.cam_bytes = 0;
    REL(T.vs), REL(T.ps), REL(T.cb), REL(T.rs), REL(T.samp), REL(T.dev);
    D3D11_BUFFER_DESC bd = {sizeof(StabConst), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    D3D11_SAMPLER_DESC sm;
    memset(&sm, 0, sizeof(sm));
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR, sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER, sm.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateVertexShader(dev, g_sh_fullscreen_vs, sizeof(g_sh_fullscreen_vs), NULL, &T.vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(dev, g_sh_stabilise, sizeof(g_sh_stabilise), NULL, &T.ps)) ||
        FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &T.cb)) ||
        FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &T.rs)) ||
        FAILED(ID3D11Device_CreateSamplerState(dev, &sm, &T.samp))) {
        REL(T.vs), REL(T.ps), REL(T.cb), REL(T.rs), REL(T.samp);
        T.failed = 1;
        LOGE("Temporal stabilisation: its shaders could not be made; off");
        return 0;
    }
    ID3D11Device_AddRef(dev);
    T.dev = dev;
    return 1;
}

/* a render target view of the image (the least recently used of 4 replaced) */
static ID3D11RenderTargetView* image_rtv(ID3D11Device* dev, ID3D11Resource* colour, DXGI_FORMAT view_fmt)
{
    int k = 0;
    for (int i = 0; i < 4; i++) {
        if (T.img[i].res == colour) {
            T.img[i].used = g_t_runs;
            return T.img[i].rtv;
        }
        if (T.img[i].used < T.img[k].used)
            k = i;
    }
    REL(T.img[k].rtv), T.img[k].res = NULL;
    D3D11_RENDER_TARGET_VIEW_DESC rv;
    memset(&rv, 0, sizeof(rv));
    rv.Format = view_fmt, rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, colour, &rv, &T.img[k].rtv)))
        return NULL;
    T.img[k].res = colour, T.img[k].used = g_t_runs;
    g_t_images++;
    return T.img[k].rtv;
}

/* the copy of the image and the history, for its size and format */
static int make_targets(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* cd, DXGI_FORMAT view_fmt)
{
    if (T.w == cd->Width && T.h == cd->Height && T.fmt == cd->Format && T.scratch)
        return 1;
    drop_targets();
    g_t_resets++;
    D3D11_TEXTURE2D_DESC d = {cd->Width, cd->Height, 1, 1, cd->Format, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof(sv));
    sv.Format = view_fmt, sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sv.Texture2D.MipLevels = 1;
    int ok = SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &T.scratch)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)T.scratch, &sv, &T.scratch_srv));
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &T.hist[i])) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)T.hist[i], NULL, &T.hist_srv[i])) &&
             SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)T.hist[i], NULL, &T.hist_rtv[i]));
    if (!ok) {
        drop_targets();
        LOGE("Temporal stabilisation: its textures for %ux%u (format %d) could not be made", cd->Width, cd->Height,
             (int)cd->Format);
        return 0;
    }
    T.w = cd->Width, T.h = cd->Height, T.fmt = cd->Format, T.cur = 0;
    LOGI("Temporal stabilisation: textures for the %ux%u image made (format %d)", cd->Width, cd->Height, (int)cd->Format);
    return 1;
}

/* the two copies of the view constants, the size of the game's buffer (CopyResource copies whole buffers) */
static int make_cams(ID3D11Device* dev, UINT bytes)
{
    if (T.cam_bytes == bytes && T.cam[0] && T.cam[1])
        return 1;
    REL(T.cam[0]), REL(T.cam[1]);
    T.cam_bytes = 0, T.have = 0;
    D3D11_BUFFER_DESC bd = {bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &T.cam[0])) ||
        FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &T.cam[1]))) {
        REL(T.cam[0]), REL(T.cam[1]);
        return 0;
    }
    T.cam_bytes = bytes;
    return 1;
}

static int tex_size(ID3D11ShaderResourceView* v, UINT* w, UINT* h)
{
    ID3D11Resource* r = NULL;
    ID3D11ShaderResourceView_GetResource(v, &r);
    if (!r)
        return 0;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    ID3D11Resource_GetType(r, &dim);
    int ok = dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    if (ok) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D*)r, &d);
        *w = d.Width, *h = d.Height;
    }
    ID3D11Resource_Release(r);
    return ok;
}

/* the painterly pass's draw: its depth (t2), gbuffer1 (t3) and GFD_PSCONST_SYSTEM (b0) */
void temporal_inputs(void* ctx)
{
    ID3D11ShaderResourceView* v[2] = {NULL, NULL};
    ID3D11Buffer* sys = NULL;
    ID3D11DeviceContext_PSGetShaderResources((ID3D11DeviceContext*)ctx, 2, 2, v);
    ID3D11DeviceContext_PSGetConstantBuffers((ID3D11DeviceContext*)ctx, 0, 1, &sys);
    AcquireSRWLockExclusive(&g_tl);
    if (v[0] != g_depth || v[1] != g_flags) {
        REL(g_depth), REL(g_flags);
        g_depth = v[0], g_flags = v[1];
        v[0] = v[1] = NULL;
    }
    if (sys != g_sys) {
        REL(g_sys);
        g_sys = sys, sys = NULL;
    }
    ReleaseSRWLockExclusive(&g_tl);
    REL(v[0]), REL(v[1]), REL(sys);
}

float temporal_weight(void)
{
    return g_t_off ? 0 : g_cfg.q_stab;
}

static float world_weight(void)
{
    return g_t_off ? 0 : g_cfg.q_stab_world;
}

/* before the SMAA edge pass's draw */
void temporal_run(void* ctx)
{
    if (GetAsyncKeyState(VK_F7) & 1) {
        g_t_off = !g_t_off;
        LOGI("Temporal stabilisation: %s (F7)", g_t_off ? "off" : "on");
    }
    float k = temporal_weight(), kw = world_weight();
    if (!(k > 0) && !(kw > 0))
        return;
    ID3D11DeviceContext* c = (ID3D11DeviceContext*)ctx;
    ID3D11ShaderResourceView *src = NULL, *flags = NULL, *depth = NULL;
    ID3D11Buffer* sys = NULL;
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &src);
    AcquireSRWLockShared(&g_tl);
    if (g_flags && g_depth && g_sys) {
        flags = g_flags, depth = g_depth, sys = g_sys;
        ID3D11ShaderResourceView_AddRef(flags), ID3D11ShaderResourceView_AddRef(depth), ID3D11Buffer_AddRef(sys);
    }
    ReleaseSRWLockShared(&g_tl);
    D3D11_BUFFER_DESC sysd;
    memset(&sysd, 0, sizeof(sysd));
    if (sys)
        ID3D11Buffer_GetDesc(sys, &sysd);
    ID3D11Resource* colour = NULL;
    D3D11_TEXTURE2D_DESC cd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    UINT fw = 0, fh = 0, dw = 0, dh = 0;
    int ok = src && flags && depth && sys && sysd.ByteWidth >= SYS_BYTES && (sysd.BindFlags & D3D11_BIND_CONSTANT_BUFFER);
    if (ok) {
        ID3D11ShaderResourceView_GetDesc(src, &sd);
        ID3D11ShaderResourceView_GetResource(src, &colour);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (colour)
            ID3D11Resource_GetType(colour, &dim);
        ok = dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        if (ok) {
            ID3D11Texture2D_GetDesc((ID3D11Texture2D*)colour, &cd);
            ok = cd.SampleDesc.Count == 1 && cd.ArraySize == 1 && (cd.BindFlags & D3D11_BIND_RENDER_TARGET) &&
                 tex_size(flags, &fw, &fh) && tex_size(depth, &dw, &dh) && fw == cd.Width && fh == cd.Height &&
                 dw == cd.Width && dh == cd.Height;
        }
    }
    if (!ok)
        g_t_missing++;
    ID3D11Device* dev = NULL;
    if (ok)
        ID3D11DeviceContext_GetDevice(c, &dev);
    AcquireSRWLockExclusive(&g_tl); /* one pass at a time (one a frame) */
    ID3D11RenderTargetView* out = NULL;
    if (ok && dev && make_shaders(dev) && make_targets(dev, &cd, sd.Format) && make_cams(dev, sysd.ByteWidth) &&
        (out = image_rtv(dev, colour, sd.Format))) {
        ULONGLONG now = GetTickCount64();
        ID3D11ShaderResourceView* mv = g_cfg.q_mv ? (ID3D11ShaderResourceView*)motion_view(T.w, T.h) : NULL;
        StabConst k_ = {k > 0.95f ? 0.95f : k, T.have && now - T.last_ms < 250 ? 1.0f : 0.0f, 0.05f, 0.15f, (int)T.w,
                        (int)T.h, kw > 0.95f ? 0.95f : kw, 0.05f, mv ? 1.0f : 0.0f, g_cfg.q_mv_view ? 1.0f : 0.0f,
                        g_cfg.q_stab_world_taps == 4 ? 1.0f : 0.0f, 0};
        g_t_mv += mv != NULL;
        FxState s;
        fx_save(c, &s);
        M(CopyRes_t, c, CopyResource)(c, (ID3D11Resource*)T.scratch, colour);
        M(CopyRes_t, c, CopyResource)(c, (ID3D11Resource*)T.cam[T.cur], (ID3D11Resource*)sys); /* this frame's view */
        ID3D11ShaderResourceView* in[16] = {T.scratch_srv, T.hist_srv[T.cur ^ 1], flags, depth, mv};
        M(SetSRV_t, c, PSSetShaderResources)(c, 0, 16, in); /* nothing else bound reads the targets */
        ID3D11RenderTargetView* rt[2] = {out, T.hist_rtv[T.cur]};
        M(OMSetRT_t, c, OMSetRenderTargets)(c, 2, rt, NULL);
        D3D11_VIEWPORT vp = {0, 0, (float)T.w, (float)T.h, 0, 1};
        M(SetVP_t, c, RSSetViewports)(c, 1, &vp);
        M(SetRS_t, c, RSSetState)(c, T.rs);
        M(SetBlend_t, c, OMSetBlendState)(c, NULL, NULL, 0xffffffffu);
        M(SetDS_t, c, OMSetDepthStencilState)(c, NULL, 0);
        M(SetIL_t, c, IASetInputLayout)(c, NULL);
        M(SetTopo_t, c, IASetPrimitiveTopology)(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        M(SetSh_t, c, VSSetShader)(c, T.vs, NULL, 0);
        M(SetSh_t, c, GSSetShader)(c, NULL, NULL, 0);
        M(SetSh_t, c, HSSetShader)(c, NULL, NULL, 0);
        M(SetSh_t, c, DSSetShader)(c, NULL, NULL, 0);
        M(SetSh_t, c, PSSetShader)(c, T.ps, NULL, 0);
        M(UpdateSub_t, c, UpdateSubresource)(c, (ID3D11Resource*)T.cb, 0, NULL, &k_, 0, 0);
        ID3D11Buffer* cbs[3] = {T.cb, T.cam[T.cur], T.cam[T.cur ^ 1]};
        M(SetCB_t, c, PSSetConstantBuffers)(c, 0, 3, cbs);
        M(SetSamp_t, c, PSSetSamplers)(c, 0, 1, &T.samp);
        M(Draw_t, c, Draw)(c, 3, 0);
        fx_restore(c, &s);
        REL(mv);
        T.cur ^= 1, T.have = 1, T.last_ms = now;
        g_t_runs++;
    }
    ReleaseSRWLockExclusive(&g_tl);
    REL(dev), REL(colour), REL(src), REL(flags), REL(depth), REL(sys);
}

void temporal_report(void)
{
    static LONG64 pr, pm, ps, pi, pv;
    LONG64 r = g_t_runs - pr, m = g_t_missing - pm, s = g_t_resets - ps, i = g_t_images - pi, v = g_t_mv - pv;
    pr = g_t_runs, pm = g_t_missing, ps = g_t_resets, pi = g_t_images, pv = g_t_mv;
    LOGI("  Temporal stabilisation: %s (history: characters %.0f%%, the rest %.0f%%): %lld passes in this window, %lld "
         "frames without its inputs (the painterly pass's flags, depth and view, or another size) | new: %lld histories "
         "(%ux%u), %lld image textures | with the characters' motion vectors: %lld passes",
         temporal_weight() > 0 || world_weight() > 0 ? "on" : g_t_off ? "off (F7)" : "off", 100.0 * g_cfg.q_stab,
         100.0 * g_cfg.q_stab_world, (long long)r, (long long)m, (long long)s, T.w, T.h, (long long)i, (long long)v);
    if (g_cfg.q_mv)
        motion_report();
}
