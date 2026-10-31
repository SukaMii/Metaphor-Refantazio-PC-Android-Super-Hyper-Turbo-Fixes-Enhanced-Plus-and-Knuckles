/* The pipeline state the mod's own passes change (temporal.c, motion.c): saved before them and set back after, every
 * call through the context's original methods (gpuprobe_method): through the mod's hooks the passes would count as
 * the game's. Include after <d3d11.h> with COBJMACROS. */
#pragma once
#include <stddef.h>
#include <string.h>

#define SLOT(m) (offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void*))
#define M(type, c, m) ((type)gpuprobe_method((c), SLOT(m)))
typedef void(STDMETHODCALLTYPE* OMSetRT_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* SetSRV_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* SetCB_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* SetSh_t)(ID3D11DeviceContext*, void*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* Draw_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* DrawIndexed_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* SetVP_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(STDMETHODCALLTYPE* SetRS_t)(ID3D11DeviceContext*, ID3D11RasterizerState*);
typedef void(STDMETHODCALLTYPE* SetBlend_t)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
typedef void(STDMETHODCALLTYPE* SetDS_t)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
typedef void(STDMETHODCALLTYPE* SetIL_t)(ID3D11DeviceContext*, ID3D11InputLayout*);
typedef void(STDMETHODCALLTYPE* SetTopo_t)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE* SetVB_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void(STDMETHODCALLTYPE* SetIB_t)(ID3D11DeviceContext*, ID3D11Buffer*, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* CopyRes_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* ClearRTV_t)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* UpdateSub_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                             UINT, UINT);
typedef void(STDMETHODCALLTYPE* SetSamp_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);

static inline void fx_rel(IUnknown** p)
{
    if (*p)
        IUnknown_Release(*p);
    *p = NULL;
}
#define REL(p) fx_rel((IUnknown**)&(p))

typedef struct {
    ID3D11RenderTargetView* rtv[8];
    ID3D11DepthStencilView* dsv;
    ID3D11ShaderResourceView* srv[16];
    ID3D11Buffer* cb[3];
    ID3D11Buffer* vscb[2];
    ID3D11SamplerState* samp;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11GeometryShader* gs;
    ID3D11HullShader* hs;
    ID3D11DomainShader* ds;
    ID3D11RasterizerState* rs;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp;
    ID3D11BlendState* bs;
    FLOAT bf[4];
    UINT mask;
    ID3D11DepthStencilState* dss;
    UINT sref;
    ID3D11InputLayout* il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer* vb[2];
    UINT vb_stride[2], vb_off[2];
    ID3D11Buffer* ib;
    DXGI_FORMAT ib_fmt;
    UINT ib_off;
} FxState;

static void fx_save(ID3D11DeviceContext* c, FxState* s)
{
    memset(s, 0, sizeof(*s));
    ID3D11DeviceContext_OMGetRenderTargets(c, 8, s->rtv, &s->dsv);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, s->srv);
    ID3D11DeviceContext_PSGetConstantBuffers(c, 0, 3, s->cb);
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 2, s->vscb);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 1, &s->samp);
    ID3D11DeviceContext_VSGetShader(c, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &s->ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &s->gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &s->hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &s->ds, NULL, NULL);
    ID3D11DeviceContext_RSGetState(c, &s->rs);
    s->nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(c, &s->nvp, s->vp);
    ID3D11DeviceContext_OMGetBlendState(c, &s->bs, s->bf, &s->mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s->dss, &s->sref);
    ID3D11DeviceContext_IAGetInputLayout(c, &s->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s->topo);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 2, s->vb, s->vb_stride, s->vb_off);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &s->ib, &s->ib_fmt, &s->ib_off);
}

/* the targets first: a texture bound as a target goes back to the shader slots once it no longer is one */
static void fx_restore(ID3D11DeviceContext* c, FxState* s)
{
    M(OMSetRT_t, c, OMSetRenderTargets)(c, 8, s->rtv, s->dsv);
    M(SetSRV_t, c, PSSetShaderResources)(c, 0, 16, s->srv);
    M(SetCB_t, c, PSSetConstantBuffers)(c, 0, 3, s->cb);
    M(SetCB_t, c, VSSetConstantBuffers)(c, 0, 2, s->vscb);
    M(SetSamp_t, c, PSSetSamplers)(c, 0, 1, &s->samp);
    M(SetSh_t, c, VSSetShader)(c, s->vs, NULL, 0);
    M(SetSh_t, c, PSSetShader)(c, s->ps, NULL, 0);
    M(SetSh_t, c, GSSetShader)(c, s->gs, NULL, 0);
    M(SetSh_t, c, HSSetShader)(c, s->hs, NULL, 0);
    M(SetSh_t, c, DSSetShader)(c, s->ds, NULL, 0);
    M(SetRS_t, c, RSSetState)(c, s->rs);
    M(SetVP_t, c, RSSetViewports)(c, s->nvp, s->vp);
    M(SetBlend_t, c, OMSetBlendState)(c, s->bs, s->bf, s->mask);
    M(SetDS_t, c, OMSetDepthStencilState)(c, s->dss, s->sref);
    M(SetIL_t, c, IASetInputLayout)(c, s->il);
    M(SetTopo_t, c, IASetPrimitiveTopology)(c, s->topo);
    M(SetVB_t, c, IASetVertexBuffers)(c, 0, 2, s->vb, s->vb_stride, s->vb_off);
    M(SetIB_t, c, IASetIndexBuffer)(c, s->ib, s->ib_fmt, s->ib_off);
    for (int i = 0; i < 8; i++)
        REL(s->rtv[i]);
    for (int i = 0; i < 16; i++)
        REL(s->srv[i]);
    for (int i = 0; i < 3; i++)
        REL(s->cb[i]);
    for (int i = 0; i < 2; i++)
        REL(s->vscb[i]), REL(s->vb[i]);
    REL(s->dsv), REL(s->samp), REL(s->vs), REL(s->ps), REL(s->gs), REL(s->hs), REL(s->ds), REL(s->rs), REL(s->bs);
    REL(s->dss), REL(s->il), REL(s->ib);
}
