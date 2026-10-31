/* Post-processing options (effects.c) on the game's own SMAA shaders (tests\data, from a frame
 * capture): the DXBC checksum, the High / Medium copies made at creation (accepted by the D3D11
 * runtime, which checks the checksum; the right constants patched), and the shader swap. */
#define COBJMACROS
#include "../src/effects.c"
#include "../src/temporal.c" /* TemporalStabilise: its counters */
#include "../src/motion.c"   /* CharacterMotionVectors: its target, set by the tests */
#include <stddef.h>
#include <stdio.h>

static int fails;

/* [Quality] OutlineDistance: the scene object's distance, set by the test (gpuprobe.c in the mod) */
static float s_node_dist = -1;
static int s_carried;
float gpuprobe_node_dist(int* carried) { *carried = s_carried; return s_node_dist; }
float gpuprobe_node_raw_dist(void) { return s_node_dist; }
static void* s_rebound; /* RimLight: what the draw-time check bound */
void gpuprobe_bind_ps(void* ctx, void* ps) { (void)ctx, s_rebound = ps; }
/* OutlineDepthOffset: the camera's depth mapping (reversed, infinite far plane, near plane 10 units) */
static float s_proj_a = 0, s_proj_b = 10;
void shadowcull_depth_proj(float* a, float* b) { *a = s_proj_a, *b = s_proj_b; }
/* TemporalStabilise: no hooks in the test, the context's own methods */
void* gpuprobe_method(void* ctx, size_t slot) { return (*(void***)ctx)[slot]; }
/* CharacterMotionVectors: the frame of the object drawn (the scene hook's sequence), the position in a layout */
static LONG64 s_seq;
LONG64 gpuprobe_node_seq(void) { return s_seq; }
static int s_crowd = 100; /* MotionVectorSkinCrowd: the characters around (the scene hook's count) */
int shadowcull_crowd(void) { return s_crowd; }
static void* s_mv_layout;
int mesh_layout_pos(void* lay, int* fmt, int* off, int* slot, int* skinned)
{
    if (!lay || lay != s_mv_layout)
        return 0;
    *fmt = DXGI_FORMAT_R32G32B32_FLOAT, *off = 0, *slot = 0, *skinned = 0;
    return 1;
}
#define EXPECT(c, what)                                                                                         \
    do {                                                                                                        \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                     \
        if (!(c))                                                                                               \
            fails++;                                                                                            \
    } while (0)

static uint8_t* load(const char* dir, const char* name, size_t* len)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    static uint8_t buf[4][65536];
    static int next;
    uint8_t* b = buf[next++ & 3];
    *len = fread(b, 1, 65536, f);
    fclose(f);
    return b;
}

/* half floats for the RGBA16F images of the TemporalStabilise test (normal values and 0 only) */
static uint16_t to_half(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    int e = (int)((u >> 23) & 255) - 127 + 15;
    return e <= 0 ? 0 : (uint16_t)(((u >> 16) & 0x8000) | (e << 10) | ((u >> 13) & 0x3ff));
}

static float from_half(uint16_t h)
{
    int e = (h >> 10) & 31;
    uint32_t u = e ? (((uint32_t)(h & 0x8000) << 16) | ((uint32_t)(e - 15 + 127) << 23) | ((uint32_t)(h & 0x3ff) << 13)) : 0;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int count_dw(const uint8_t* b, size_t len, uint32_t v)
{
    int k = 0;
    for (size_t i = 0; i + 4 <= len; i += 4)
        k += *(const uint32_t*)(b + i) == v;
    return k;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "tests\\data";
    size_t lw = 0, le = 0, lb = 0;
    uint8_t* w = load(dir, "ps_da00acd64a9e2b1f.dxbc", &lw); /* blending weights */
    uint8_t* e = load(dir, "ps_5b1af0481406dae1.dxbc", &le); /* edge detection */
    uint8_t* bl = load(dir, "ps_6a27f376d7d79b8f.dxbc", &lb); /* neighbourhood blending */
    if (!w || !e || !bl) {
        printf("[FAIL] test shaders not found in %s\n", dir);
        return 1;
    }
    /* the checksum reproduces the game's */
    uint32_t sum[4];
    int ok = 1;
    uint8_t* all[3] = {w, e, bl};
    size_t lens[3] = {lw, le, lb};
    for (int i = 0; i < 3; i++) {
        fx_dxbc_checksum(all[i], lens[i], sum);
        ok &= !memcmp(sum, all[i] + 4, 16);
    }
    EXPECT(ok, "DXBC checksum of the game's shaders reproduced");
    EXPECT(fx_fnv64(w, lw) == 0xda00acd64a9e2b1full && fx_fnv64(e, le) == 0x5b1af0481406dae1ull,
           "the shaders hash as in the capture");
    /* a hash not in the list: the SMAA shaders recognised by their content, the final blend not */
    EXPECT(smaa_by_content(w, lw, 0) == R_SMAA_WEIGHTS && smaa_by_content(e, le, 0) == R_SMAA_EDGES &&
               smaa_by_content(bl, lb, 0) == R_NONE,
           "SMAA weights and edge shaders recognised by their content, the blend not");

    /* a real device: the patched copies must be accepted */
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        printf("[FAIL] D3D11CreateDevice(WARP) 0x%08lx\n", hr);
        return 1;
    }
    void* create = (*(void***)dev)[offsetof(ID3D11DeviceVtbl, CreatePixelShader) / sizeof(void*)];
    g_cfg.q_fx_active = 1;
    g_cfg.gpu_timing = 1; /* every pixel shader registered with its size */
    ID3D11PixelShader *pw = NULL, *pe = NULL, *pb = NULL;
    ID3D11Device_CreatePixelShader(dev, w, lw, NULL, &pw);
    ID3D11Device_CreatePixelShader(dev, e, le, NULL, &pe);
    ID3D11Device_CreatePixelShader(dev, bl, lb, NULL, &pb);
    EXPECT(pw && pe && pb, "the game's shaders created");
    effects_shader_created(dev, 0, pw, w, lw, create);
    effects_shader_created(dev, 0, pe, e, le, create);
    effects_shader_created(dev, 0, pb, bl, lb, create);
    EXPECT(g_nvar == 2 && g_var[0].var[1] && g_var[0].var[2] && g_var[1].var[1] && g_var[1].var[2],
           "High and Medium copies of the weight and edge shaders created (checksum accepted)");
    EXPECT(role_of(pw) == R_SMAA_WEIGHTS && role_of(pe) == R_SMAA_EDGES && role_of(pb) == R_NONE,
           "roles: weights, edges; the final blend left alone");

    /* the constants: weights High = 4 scalar 16 -> 8 and the 15s -> 7, Medium = 16 -> 0; edges 0.05 -> 0.1 */
    uint8_t c[16384];
    memcpy(c, w, lw);
    static const uint32_t f1 = 0x41800000, t8 = 0x41000000;
    int k1 = patch_seq((uint32_t*)c, lw / 4, 0x00004001, &f1, &t8, 1);
    EXPECT(k1 == 4, "4 diagonal loop bounds in the weight shader");
    EXPECT(count_dw(w, lw, 0x41800000) == 8, "the area-texture 16s are vector constants, not touched");
    memcpy(c, e, le);
    static const uint32_t fe[2] = {0x3D4CCCCD, 0x3D4CCCCD}, te[2] = {0x3DCCCCCD, 0x3DCCCCCD};
    int ke = patch_seq((uint32_t*)c, le / 4, 0, fe, te, 2);
    EXPECT(ke == 1, "one edge threshold pair in the edge shader");

    /* the SMAA 2.8 shaders, when they are in tests\data: known by
     * hash, the weights' four diagonal bounds (15) patched to 7 / -1, both copies accepted by the runtime */
    {
        size_t lw2 = 0, le2 = 0;
        uint8_t* w2 = load(dir, "ps_9f3d5b38f7eff19e.dxbc", &lw2);
        uint8_t* e2 = w2 ? load(dir, "ps_822dcd902415fe5d.dxbc", &le2) : NULL;
        if (w2 && e2) {
            memcpy(c, w2, lw2);
            static const uint32_t f15 = 0x41700000, t7 = 0x40E00000;
            int k15 = patch_seq((uint32_t*)c, lw2 / 4, 0x00004001, &f15, &t7, 1);
            memcpy(c, w2, lw2);
            int k16 = patch_seq((uint32_t*)c, lw2 / 4, 0x00004001, &f1, &t8, 1);
            EXPECT(k15 == 4 && k16 == 0, "SMAA 2.8 weights: 4 diagonal bounds of 15, no scalar 16");
            void* v[2][3] = {{0}};
            for (int lv = 1; lv <= 2; lv++) {
                v[0][lv] = make_variant(dev, (CreateSh_t)create, w2, lw2, lv, 0, R_SMAA_WEIGHTS);
                v[1][lv] = make_variant(dev, (CreateSh_t)create, e2, le2, lv, 0, R_SMAA_EDGES);
            }
            EXPECT(fx_fnv64(w2, lw2) == 0x9f3d5b38f7eff19eull && fx_fnv64(e2, le2) == 0x822dcd902415fe5dull &&
                       v[0][1] && v[0][2] && v[1][1] && v[1][2],
                   "SMAA 2.8 shaders: High and Medium copies of the weights and edges created");
            for (int i = 0; i < 2; i++)
                for (int lv = 1; lv <= 2; lv++)
                    if (v[i][lv])
                        ID3D11PixelShader_Release((ID3D11PixelShader*)v[i][lv]);
        } else {
            printf("  (the SMAA 2.8 shaders are not in %s: not checked)\n", dir);
        }
    }

    /* the swap: Ultra keeps the original, High / Medium bind the copies, Off leaves the shaders alone */
    void* cx = (void*)ctx;
    g_cfg.q_smaa = 3;
    EXPECT(effects_bind_ps(cx, pw) == pw, "Ultra: the game's shader");
    g_cfg.q_smaa = 2;
    EXPECT(effects_bind_ps(cx, pw) == g_var[0].var[2], "High: the High copy");
    g_cfg.q_smaa = 1;
    EXPECT(effects_bind_ps(cx, pe) == g_var[1].var[1], "Medium: the Medium edge copy");
    g_cfg.q_smaa = 0;
    EXPECT(effects_bind_ps(cx, pw) == pw && effects_draw(cx, 3) == 1, "Off: the weight draw left out");
    effects_bind_ps(cx, pb);
    EXPECT(effects_draw(cx, 3) == 0, "the final blend still drawn");

    /* the registry: the STAT chunk as the disassembler counts it (instruction slots; texture instructions;
     * dynamic branches) */
    const PsInfo *iw = fx_ps_info(fx_ps_id(pw)), *ie = fx_ps_info(fx_ps_id(pe)), *ib = fx_ps_info(fx_ps_id(pb));
    EXPECT(iw && ie && ib && iw != ie && ie != ib, "every pixel shader gets an id of its own");
    EXPECT(iw && iw->hash == 0xda00acd64a9e2b1full && iw->instr == 260 && iw->tex == 39 && iw->flow == 17,
           "weights: 260 instruction slots, 39 texture instructions, 17 dynamic branches");
    EXPECT(ie && ie->instr == 52 && ie->tex == 7 && ie->flow == 0, "edges: 52 slots, 7 texture instructions");
    EXPECT(ib && ib->instr == 19 && ib->tex == 5 && ib->flow == 1, "blend: 19 slots, 5 texture instructions, 1 branch");
    EXPECT(fx_ps_id(NULL) == 0 && fx_ps_id((void*)&fails) == 0, "no id for no shader or an unknown one");
    /* the knock-out: the draws of the named shader are left out, the others drawn */
    g_cfg.q_smaa = 1;
    g_cfg.q_knock = ib ? ib->hash : 1;
    effects_bind_ps(cx, pb);
    int kb = effects_draw(cx, 3);
    effects_bind_ps(cx, pe);
    int ke2 = effects_draw(cx, 3);
    effects_reset(cx);
    EXPECT(kb == 1 && ke2 == 0 && effects_draw(cx, 3) == 0, "knock-out: the blend's draws left out, the edges' drawn");
    g_cfg.q_knock = 0;

    /* the game makes thousands of shader objects, many with the same bytecode: 10,000 objects of the blend's
     * code share its id; 6,000 distinct shaders (more than the 4,096 of 0.38.0) all get ids */
    long ns0 = 0, no0 = 0, ns1 = 0, no1 = 0;
    fx_ps_counts(&ns0, &no0);
    static uint8_t objs[16000];
    int same = 1;
    for (int i = 0; i < 10000; i++) {
        ps_register(&objs[i], bl, lb);
        same &= fx_ps_id(&objs[i]) == fx_ps_id(pb);
    }
    uint8_t* v = (uint8_t*)malloc(lb);
    int distinct = 1, last = 0;
    for (int i = 0; i < 6000 && v; i++) {
        memcpy(v, bl, lb);
        int mark = i + 1; /* 0 would leave the blend's own bytes */
        memcpy(v + lb - 8, &mark, sizeof(mark)); /* another bytecode (the STAT chunk is earlier) */
        ps_register(&objs[10000 + i], v, lb);
        int id = fx_ps_id(&objs[10000 + i]);
        distinct &= id > last;
        last = id;
    }
    free(v);
    fx_ps_counts(&ns1, &no1);
    printf("        registry: %ld -> %ld shaders, %ld -> %ld objects\n", ns0, ns1, no0, no1);
    EXPECT(same, "10,000 objects of one bytecode share its id");
    EXPECT(distinct && ns1 - ns0 == 6000 && no1 - no0 == 16000, "6,000 more distinct shaders, 16,000 more objects mapped");

    /* outlines: a shader with the game's outline parameter is recognised; its draws are left out for objects
     * beyond OutlineDistance, kept nearer, when the distance is not known, and when the option is off */
    typedef HRESULT(WINAPI * D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                           ID3D10Blob**, ID3D10Blob**);
    HMODULE dcl = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t compile = dcl ? (D3DCompile_t)(void*)GetProcAddress(dcl, "D3DCompile") : NULL;
    static const char osrc[] = "cbuffer GFD_PSCONST_MATERIAL : register(b2) { float4 outlineColor; };"
                               "float4 main() : SV_Target { return outlineColor; }";
    ID3D10Blob *oc = NULL, *oerr = NULL;
    ID3D11PixelShader* po = NULL;
    if (compile && SUCCEEDED(compile(osrc, sizeof(osrc) - 1, "o", NULL, NULL, "main", "ps_4_0", 0, 0, &oc, &oerr))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(oc), ID3D10Blob_GetBufferSize(oc), NULL, &po);
        g_cfg.q_outline_hooks = 1;
        effects_shader_created(dev, 0, po, ID3D10Blob_GetBufferPointer(oc), ID3D10Blob_GetBufferSize(oc), create);
    }
    const PsInfo* io = fx_ps_info(fx_ps_id(po));
    EXPECT(io && io->outline && iw && !iw->outline && !ib->outline, "the outline shader recognised, the SMAA ones not");
    g_cfg.q_smaa = 1, g_cfg.q_outline_dist = 50;
    int r[7];
    effects_bind_ps(cx, po);
    s_node_dist = 80, r[0] = effects_draw(cx, 3);
    s_node_dist = 20, r[1] = effects_draw(cx, 3);
    s_node_dist = -1, r[2] = effects_draw(cx, 3);
    s_node_dist = 80, g_cfg.q_outline_dist = 0, r[3] = effects_draw(cx, 3);
    g_cfg.q_outline_dist = 0.001f, s_node_dist = 0.5f, r[4] = effects_draw(cx, 3); /* "all" */
    s_node_dist = -2, g_cfg.q_outline_dist = 50, r[6] = effects_draw(cx, 3); /* not a character: 3D text, bubbles */
    effects_bind_ps(cx, pb);
    g_cfg.q_outline_dist = 50, s_node_dist = 80, r[5] = effects_draw(cx, 3);
    printf("        outline draws left out: beyond 50 m %d, at 20 m %d, distance unknown %d, option off %d, 'all' %d, "
           "another shader beyond 50 m %d\n", r[0], r[1], r[2], r[3], r[4], r[5]);
    EXPECT(r[6] == 0, "outline-shader draws of other objects (3D text, speech bubbles) kept beyond the distance");
    EXPECT(r[0] == 1 && r[1] == 0 && r[2] == 0 && r[3] == 0 && r[4] == 1 && r[5] == 0,
           "outlines left out beyond the distance only, other shaders drawn");
    s_carried = 1, s_node_dist = 80, g_cfg.q_outline_dist = 50;
    effects_bind_ps(cx, po);
    int rc = effects_draw(cx, 3);
    s_carried = 0;
    EXPECT(rc == 1, "a carried object's outline (bag, weapon) left out with its character");
    g_cfg.q_outline_dist = 0;

    /* the painterly wobble: the game's shader gets a plain copy (our DXBC, accepted by the runtime), bound in its
     * place only with Painterly=0 */
    size_t lp = 0;
    uint8_t* pw2 = load(dir, "ps_b3ebc8024b526721.dxbc", &lp);
    ID3D11PixelShader* ppaint = NULL;
    if (pw2)
        ID3D11Device_CreatePixelShader(dev, pw2, lp, NULL, &ppaint);
    if (ppaint)
        effects_shader_created(dev, 0, ppaint, pw2, lp, create);
    EXPECT(ppaint && g_npaint == 1 && g_paint[0].plain, "painterly wobble recognised, its plain copy created");
    g_cfg.q_painterly = 1;
    void* b1 = effects_bind_ps(cx, ppaint);
    g_cfg.q_painterly = 0;
    void* b0 = effects_bind_ps(cx, ppaint);
    EXPECT(b1 == ppaint && b0 == g_paint[0].plain && effects_draw(cx, 3) == 0,
           "Painterly=1 binds the game's shader, 0 the plain copy (drawn, not left out)");
    g_cfg.q_painterly = 1;

    /* the rim-light patch: every read of matEdgeColor reads 0. A shader in the game's style, rendered on WARP
     * before and after: base + edge colour x strength x factor, then the base alone */
    static const char rsrc[] = "cbuffer GFD_PSCONST_MATERIAL : register(b1) { float4 matBaseColor; float4 matEdgeColor; "
                               "float edgeFactor; };"
                               "float4 main(float4 p : SV_Position) : SV_Target {"
                               " return matBaseColor + matEdgeColor * matEdgeColor.w * edgeFactor; }";
    static const char vsrc[] = "float4 main(uint id : SV_VertexID) : SV_Position {"
                               " float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
    ID3D10Blob *rc0 = NULL, *vc0 = NULL;
    int rslot = -1, roff = -1, nrep = 0;
    size_t rlen = 0;
    uint8_t* rpat = NULL;
    float px[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    if (compile && SUCCEEDED(compile(rsrc, sizeof(rsrc) - 1, "r", NULL, NULL, "main", "ps_5_0", 0, 0, &rc0, &oerr)) &&
        SUCCEEDED(compile(vsrc, sizeof(vsrc) - 1, "v", NULL, NULL, "main", "vs_5_0", 0, 0, &vc0, &oerr))) {
        const uint8_t* rb = (const uint8_t*)ID3D10Blob_GetBufferPointer(rc0);
        size_t rbl = ID3D10Blob_GetBufferSize(rc0);
        dxbc_cbuffer_var(rb, rbl, "GFD_PSCONST_MATERIAL", "matEdgeColor", &rslot, &roff);
        rpat = dxbc_zero_cb_reads(rb, rbl, rslot, roff / 16, &rlen, &nrep);
        ID3D11PixelShader *ps0 = NULL, *ps1 = NULL;
        ID3D11VertexShader* vs0 = NULL;
        ID3D11Device_CreatePixelShader(dev, rb, rbl, NULL, &ps0);
        if (rpat)
            ID3D11Device_CreatePixelShader(dev, rpat, rlen, NULL, &ps1);
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vc0), ID3D10Blob_GetBufferSize(vc0), NULL, &vs0);
        D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ID3D11Texture2D *rt = NULL, *st = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
        td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        ID3D11RenderTargetView* rtv = NULL;
        if (rt)
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)rt, NULL, &rtv);
        static const float cbd[12] = {0.25f, 0.5f, 0.75f, 1, 1, 1, 1, 0.5f, 2, 0, 0, 0}; /* base, edge (w 0.5), factor 2 */
        D3D11_BUFFER_DESC bd = {sizeof(cbd), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {cbd, 0, 0};
        ID3D11Buffer* cb = NULL;
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &cb);
        D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
        for (int k = 0; k < 2 && ps0 && ps1 && vs0 && rtv && st && cb; k++) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs0, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, k ? ps1 : ps0, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &cb);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st, (ID3D11Resource*)rt);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
                memcpy(px[k], m.pData, sizeof(px[k]));
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st, 0);
            }
        }
    }
    printf("        rim patch: matEdgeColor in cb%d at %d, %d reads replaced; pixel before %.2f %.2f %.2f, after %.2f %.2f %.2f\n",
           rslot, roff, nrep, px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2]);
    EXPECT(rslot == 1 && roff == 16 && nrep >= 1, "RDEF read: matEdgeColor in cb1 at byte 16; its reads replaced");
    EXPECT(px[0][0] > 1.2f && px[0][0] < 1.3f && px[1][0] > 0.24f && px[1][0] < 0.26f && px[1][2] > 0.74f && px[1][2] < 0.76f,
           "rendered on WARP: base + rim before, the base alone after (the patched DXBC runs)");
    free(rpat);
    /* the game's toon shader: patched and accepted by the runtime */
    size_t lt = 0, ltp = 0;
    int ts = -1, tof = -1, tn = 0;
    uint8_t* toon = load(dir, "ps_c76d6b4ed1ee9785.dxbc", &lt);
    uint8_t* tpat = NULL;
    ID3D11PixelShader* tps = NULL;
    if (toon && dxbc_cbuffer_var(toon, lt, "GFD_PSCONST_MATERIAL", "matEdgeColor", &ts, &tof))
        tpat = dxbc_zero_cb_reads(toon, lt, ts, tof / 16, &ltp, &tn);
    if (tpat)
        ID3D11Device_CreatePixelShader(dev, tpat, ltp, NULL, &tps);
    printf("        the game's toon shader c76d6b4e: matEdgeColor in cb%d at %d, %d reads replaced, %zu -> %zu bytes\n", ts, tof,
           tn, lt, ltp);
    EXPECT(ts == 1 && tof == 48 && tn >= 2 && tps, "the game's toon shader patched (colour and strength reads) and accepted");
    free(tpat);
    /* RimLight=20: the game's toon shader gets its rim-free copy at creation; near objects draw with the game's,
     * far ones with the copy, also when the object changes without a new bind */
    ID3D11PixelShader* tgame = NULL;
    if (toon)
        ID3D11Device_CreatePixelShader(dev, toon, lt, NULL, &tgame);
    g_cfg.q_rim_hooks = 1, g_cfg.q_rim_dist = 20;
    if (tgame)
        effects_shader_created(dev, 0, tgame, toon, lt, create);
    const PsInfo* it = fx_ps_info(fx_ps_id(tgame));
    s_node_dist = 10;
    void* near_ps = effects_bind_ps(cx, tgame);
    s_node_dist = 35, s_rebound = NULL;
    effects_draw(cx, 3);
    void* moved = s_rebound;
    void* far_ps = effects_bind_ps(cx, tgame);
    g_cfg.q_rim_dist = 0.001f, s_node_dist = -1;
    void* off_ps = effects_bind_ps(cx, tgame);
    printf("        RimLight: copy made %d; near -> %s, far -> %s, moved away unbound -> %s, off -> %s\n", it ? it->rim : 0,
           near_ps == tgame ? "game's" : "copy", far_ps == tgame ? "game's" : "copy",
           moved == (it ? it->norim : NULL) ? "copy" : "none", off_ps == tgame ? "game's" : "copy");
    EXPECT(it && it->rim == 1 && near_ps == tgame && far_ps == it->norim && moved == it->norim && off_ps == it->norim,
           "rim: the game's shader near, the rim-free copy beyond 20 m (re-bound at the draw), and everywhere with off");
    g_cfg.q_rim_hooks = 0, g_cfg.q_rim_dist = 0;

    /* InnerOutlines: in an outline shader only the multiplication of outlineColor.w (the opacity of the lines over the
     * character's own body) reads 0. A shader in the game's style on WARP: over its own outline id the opacity is
     * x/4 x outlineColor.w before and 0 after; over another id (the silhouette) outlineColor.w both times */
    static const char isrc[] = "cbuffer U : register(b0) { uint underID; };"
                               "cbuffer GFD_PSCONST_MATERIAL : register(b1) { float outlineBloomIntensity; float lerpBlendRate;"
                               " float _reserve; uint outlineID; float4 outlineColor; };"
                               "float4 main(float4 p : SV_Position) : SV_Target {"
                               " float inner = p.x * 0.25 * outlineColor.w;"
                               " return float4(outlineColor.rgb, underID == outlineID ? inner : outlineColor.w); }";
    ID3D10Blob *ic0 = NULL, *vc1 = NULL;
    int islot = -1, ioff = -1, inrep = 0;
    size_t ilen = 0;
    uint8_t* ipat = NULL;
    float ipx[4][4]; /* inner before, after; silhouette before, after */
    for (int i = 0; i < 4; i++)
        ipx[i][0] = ipx[i][1] = ipx[i][2] = ipx[i][3] = -1;
    if (compile && SUCCEEDED(compile(isrc, sizeof(isrc) - 1, "i", NULL, NULL, "main", "ps_5_0", 0, 0, &ic0, &oerr)) &&
        SUCCEEDED(compile(vsrc, sizeof(vsrc) - 1, "v", NULL, NULL, "main", "vs_5_0", 0, 0, &vc1, &oerr))) {
        const uint8_t* ib0 = (const uint8_t*)ID3D10Blob_GetBufferPointer(ic0);
        size_t ibl = ID3D10Blob_GetBufferSize(ic0);
        dxbc_cbuffer_var(ib0, ibl, "GFD_PSCONST_MATERIAL", "outlineColor", &islot, &ioff);
        ipat = dxbc_patch_cb_reads(ib0, ibl, islot, ioff / 16, 0x38, 3, 0.0f, &ilen, &inrep);
        ID3D11PixelShader *ps0 = NULL, *ps1 = NULL;
        ID3D11VertexShader* vs0 = NULL;
        ID3D11Device_CreatePixelShader(dev, ib0, ibl, NULL, &ps0);
        if (ipat)
            ID3D11Device_CreatePixelShader(dev, ipat, ilen, NULL, &ps1);
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vc1), ID3D10Blob_GetBufferSize(vc1), NULL, &vs0);
        D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ID3D11Texture2D *rt = NULL, *st = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
        td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        ID3D11RenderTargetView* rtv = NULL;
        if (rt)
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)rt, NULL, &rtv);
        static const uint32_t mat[8] = {0, 0, 0, 7, 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3F4CCCCD}; /* id 7; 0.1 0.2 0.3 0.8 */
        static const uint32_t under[2][4] = {{7, 0, 0, 0}, {3, 0, 0, 0}}; /* the same id (inner), another (silhouette) */
        D3D11_BUFFER_DESC bd = {sizeof(mat), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {mat, 0, 0};
        ID3D11Buffer *cbm = NULL, *cbu[2] = {NULL, NULL};
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &cbm);
        bd.ByteWidth = 16;
        for (int u = 0; u < 2; u++) {
            sd.pSysMem = under[u];
            ID3D11Device_CreateBuffer(dev, &bd, &sd, &cbu[u]);
        }
        D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
        for (int k = 0; k < 4 && ps0 && ps1 && vs0 && rtv && st && cbm && cbu[0] && cbu[1]; k++) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs0, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, (k & 1) ? ps1 : ps0, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbu[k >> 1]);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &cbm);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st, (ID3D11Resource*)rt);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
                memcpy(ipx[k], m.pData, sizeof(ipx[k]));
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st, 0);
            }
        }
    }
    printf("        inner-line patch: outlineColor in cb%d at %d, %d read replaced; opacity over its own body %.2f -> %.2f, "
           "over another %.2f -> %.2f; colour kept %.2f %.2f %.2f\n",
           islot, ioff, inrep, ipx[0][3], ipx[1][3], ipx[2][3], ipx[3][3], ipx[1][0], ipx[1][1], ipx[1][2]);
    EXPECT(islot == 1 && ioff == 16 && inrep == 1, "RDEF read: outlineColor in cb1 at byte 16; only the one mul of .w replaced");
    EXPECT(ipx[0][3] > 0.09f && ipx[0][3] < 0.11f && ipx[1][3] == 0 && ipx[2][3] > 0.79f && ipx[2][3] < 0.81f &&
               ipx[3][3] > 0.79f && ipx[3][3] < 0.81f && ipx[1][0] > 0.09f && ipx[1][0] < 0.11f && ipx[1][2] > 0.29f,
           "rendered on WARP: the inner line's opacity 0 after, the silhouette's and the colour unchanged");
    free(ipat);
    /* the game's outline shader 44782049: the one mul patched, the movc of the silhouette left reading cb1[1].w;
     * accepted by the runtime */
    size_t lo = 0, lop = 0;
    int os = -1, oof = -1, on = 0;
    uint8_t* ogame = load(dir, "ps_447820495b495377.dxbc", &lo);
    uint8_t* opat = NULL;
    ID3D11PixelShader* ops = NULL;
    if (ogame && dxbc_cbuffer_var(ogame, lo, "GFD_PSCONST_MATERIAL", "outlineColor", &os, &oof))
        opat = dxbc_patch_cb_reads(ogame, lo, os, oof / 16, 0x38, 3, 0.0f, &lop, &on);
    if (opat)
        ID3D11Device_CreatePixelShader(dev, opat, lop, NULL, &ops);
    typedef HRESULT(WINAPI * D3DDisassemble_t)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3D10Blob**);
    D3DDisassemble_t disasm = dcl ? (D3DDisassemble_t)(void*)GetProcAddress(dcl, "D3DDisassemble") : NULL;
    ID3D10Blob* dis = NULL;
    int mul0 = 0, movc_kept = 0;
    if (opat && disasm && SUCCEEDED(disasm(opat, lop, 0, NULL, &dis))) {
        const char* txt = (const char*)ID3D10Blob_GetBufferPointer(dis);
        mul0 = strstr(txt, "mul r0.x, v5.w, l(0.000000)") != NULL;
        movc_kept = strstr(txt, "movc r0.x, r2.x, r0.x, cb1[1].w") != NULL;
    }
    printf("        the game's outline shader 44782049: outlineColor in cb%d at %d, %d read replaced; disassembly: inner "
           "opacity %s, silhouette %s\n", os, oof, on, mul0 ? "v5.w x 0" : "?", movc_kept ? "cb1[1].w" : "?");
    EXPECT(os == 1 && oof == 16 && on == 1 && ops && mul0 && movc_kept,
           "the game's outline shader: inner opacity reads 0, the silhouette's still outlineColor.w; accepted");
    free(opat);
    /* InnerOutlines=15: the copy made at creation; near characters draw with the game's shader, far ones with the
     * copy (also when the object changes without a new bind), every one with off. Their outlines beyond
     * OutlineDistance are still left out with the copy bound */
    ID3D11PixelShader* og = NULL;
    if (ogame)
        ID3D11Device_CreatePixelShader(dev, ogame, lo, NULL, &og);
    g_cfg.q_outline_hooks = 1, g_cfg.q_inner_hooks = 1, g_cfg.q_inner_dist = 15;
    if (og)
        effects_shader_created(dev, 0, og, ogame, lo, create);
    const PsInfo* ig = fx_ps_info(fx_ps_id(og));
    s_node_dist = 10;
    void* inear = effects_bind_ps(cx, og);
    s_node_dist = 25, s_rebound = NULL;
    effects_draw(cx, 3);
    void* imoved = s_rebound;
    void* ifar = effects_bind_ps(cx, og);
    g_cfg.q_outline_dist = 20;
    int ileft = effects_draw(cx, 3);
    g_cfg.q_outline_dist = 0, g_cfg.q_inner_dist = 0.001f, s_node_dist = 5;
    void* ioff_ps = effects_bind_ps(cx, og);
    g_cfg.q_inner_dist = 0;
    void* igame = effects_bind_ps(cx, og);
    printf("        InnerOutlines: copy made %d; near -> %s, far -> %s, moved away unbound -> %s, off -> %s, on -> %s; "
           "beyond OutlineDistance left out %d\n", ig ? ig->inner : 0, inear == og ? "game's" : "copy",
           ifar == og ? "game's" : "copy", imoved == (ig ? ig->noinner : NULL) ? "copy" : "none",
           ioff_ps == og ? "game's" : "copy", igame == og ? "game's" : "copy", ileft);
    EXPECT(ig && ig->inner == 1 && ig->outline && inear == og && ifar == ig->noinner && imoved == ig->noinner &&
               ioff_ps == ig->noinner && igame == og,
           "inner lines: the game's shader near, the copy beyond 15 m (re-bound at the draw), everywhere with off");
    EXPECT(ileft == 1, "the copy bound: the outline still left out beyond OutlineDistance");
    g_cfg.q_inner_hooks = 0, g_cfg.q_outline_hooks = 0;

    /* OutlineDepthOffset: the bias for a push of k x the distance. Reversed float depth (z = 10 / d): 0.3% at 10 m
     * is 3e-5 in z, 32212 units of 2^-30; it stays 0.3% of the distance at 5 m. D24 with the usual mapping: 1678 units
     * of 2^-24 for 1% at 10 m. Unknown formats, no depth test or no camera yet: none */
    int b32 = fx_outline_bias(DXGI_FORMAT_D32_FLOAT, D3D11_COMPARISON_GREATER_EQUAL, 0, 10, 0.003f, 1000.0f);
    int b24 = fx_outline_bias(DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_COMPARISON_LESS, 1.0001f, -10.001f, 0.01f, 1000.0f);
    double z5 = 10.0 / 500, dz5 = -b32 * ldexp(1.0, -29), push5 = dz5 * 500 * 500 / 10; /* z 0.02 = 0.64 x 2^-5 */
    printf("        depth bias: D32 reversed 0.3%% -> %d (at 5 m it moves %.2f cm = %.2f%%), D24 1%% -> %d; R16 %d, "
           "always %d, no camera %d\n", b32, push5, push5 / 5, b24,
           fx_outline_bias(DXGI_FORMAT_R16_FLOAT, D3D11_COMPARISON_LESS, 0, 10, 0.01f, 1000.0f),
           fx_outline_bias(DXGI_FORMAT_D32_FLOAT, D3D11_COMPARISON_ALWAYS, 0, 10, 0.01f, 1000.0f),
           fx_outline_bias(DXGI_FORMAT_D32_FLOAT, D3D11_COMPARISON_GREATER, 0, 0, 0.01f, 1000.0f));
    (void)z5;
    /* D24 (the game's: standard depth, near plane 20 cm): the bias set per distance keeps the push 1% of it */
    int b24n = fx_outline_bias(DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_COMPARISON_LESS_EQUAL, 1, -20, 0.01f, 300.0f);
    int b24f = fx_outline_bias(DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_COMPARISON_LESS_EQUAL, 1, -20, 0.01f, 3000.0f);
    double pn = b24n / 16777216.0 * 300 * 300 / 20, pf = b24f / 16777216.0 * 3000 * 3000 / 20;
    printf("        D24, near plane 20 cm, 1%%: at 3 m bias %d moves %.2f cm, at 30 m bias %d moves %.1f cm\n", b24n, pn, b24f,
           pf);
    EXPECT(pn > 2.95 && pn < 3.05 && pf > 29.5 && pf < 30.5, "D24: the push is 1% of the distance at 3 m and at 30 m");
    EXPECT(b32 == -32212 && push5 > 1.49 && push5 < 1.51 && b24 > 1670 && b24 < 1685 &&
               !fx_outline_bias(DXGI_FORMAT_R16_FLOAT, D3D11_COMPARISON_LESS, 0, 10, 0.01f, 1000.0f) &&
               !fx_outline_bias(DXGI_FORMAT_D32_FLOAT, D3D11_COMPARISON_ALWAYS, 0, 10, 0.01f, 1000.0f) &&
               !fx_outline_bias(DXGI_FORMAT_D32_FLOAT, D3D11_COMPARISON_GREATER, 0, 0, 0.01f, 1000.0f),
           "depth bias: reversed float scales with the distance, D24 at 10 m, none when not known");
    /* on WARP, reversed float depth: the character's surface at z 0.02 (5 m), then an outline 0.1% nearer (a hair's
     * breadth: pokes through) and one 1% nearer (a real gap); with 0.3% offset the first is hidden, the second drawn.
     * The game's rasterizer state is bound back at the next draw that is not an outline */
    static const char zvs[] = "cbuffer Z : register(b0) { float z; };"
                              "float4 main(uint id : SV_VertexID) : SV_Position {"
                              " float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), z, 1); }";
    static const char rps[] = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
    ID3D10Blob *zb = NULL, *rb2 = NULL;
    float got[3][2]; /* [case][offset off/on]: green = the outline drawn */
    int restored = 0, cases_ok = 0;
    for (int i = 0; i < 3; i++)
        got[i][0] = got[i][1] = -1;
    if (compile && po && SUCCEEDED(compile(zvs, sizeof(zvs) - 1, "z", NULL, NULL, "main", "vs_5_0", 0, 0, &zb, &oerr)) &&
        SUCCEEDED(compile(rps, sizeof(rps) - 1, "r", NULL, NULL, "main", "ps_5_0", 0, 0, &rb2, &oerr))) {
        ID3D11VertexShader* zv = NULL;
        ID3D11PixelShader* red = NULL;
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(zb), ID3D10Blob_GetBufferSize(zb), NULL, &zv);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(rb2), ID3D10Blob_GetBufferSize(rb2), NULL, &red);
        D3D11_TEXTURE2D_DESC td = {4, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ID3D11Texture2D *rt = NULL, *st = NULL, *dt = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
        td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        td.Format = DXGI_FORMAT_D32_FLOAT, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_DEPTH_STENCIL,
        td.CPUAccessFlags = 0;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &dt);
        ID3D11RenderTargetView* rtv = NULL;
        ID3D11DepthStencilView* dsv = NULL;
        if (rt)
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)rt, NULL, &rtv);
        if (dt)
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)dt, NULL, &dsv);
        D3D11_DEPTH_STENCIL_DESC dd = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_GREATER_EQUAL, FALSE, 0, 0,
                                       {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS},
                                       {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS}};
        ID3D11DepthStencilState* dss = NULL;
        ID3D11Device_CreateDepthStencilState(dev, &dd, &dss);
        D3D11_RASTERIZER_DESC gd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
        ID3D11RasterizerState* game_rs = NULL;
        ID3D11Device_CreateRasterizerState(dev, &gd, &game_rs);
        static const float green[4] = {0, 1, 0, 1};
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {green, 0, 0};
        ID3D11Buffer *ocb = NULL, *zcb[3] = {NULL, NULL, NULL};
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &ocb);
        static const float zs[3][4] = {{0.02f}, {0.02f * 1.001f}, {0.02f * 1.01f}}; /* surface, hair's breadth, gap */
        for (int i = 0; i < 3; i++) {
            sd.pSysMem = zs[i];
            ID3D11Device_CreateBuffer(dev, &bd, &sd, &zcb[i]);
        }
        D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
        g_cfg.q_outline_hooks = 1, g_cfg.q_outline_dist = 0;
        s_node_dist = 5; /* a character */
        for (int c2 = 1; c2 <= 2 && zv && red && rtv && dsv && st && dss && game_rs && ocb && zcb[2]; c2++)
            for (int on = 0; on < 2; on++) {
                static const float black[4] = {0, 0, 0, 0};
                ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
                ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, dss, 0);
                ID3D11DeviceContext_RSSetState(ctx, game_rs);
                ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
                ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
                ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11DeviceContext_VSSetShader(ctx, zv, NULL, 0);
                g_cfg.q_ol_depth = on ? 0.003f : 0;
                /* the character's surface */
                ID3D11DeviceContext_PSSetShader(ctx, red, NULL, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &zcb[0]);
                effects_bind_ps(cx, red);
                effects_draw(cx, 3);
                ID3D11DeviceContext_Draw(ctx, 3, 0);
                /* its outline, as the hooks do: the bind, the check before the draw */
                ID3D11DeviceContext_PSSetShader(ctx, po, NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(ctx, 2, 1, &ocb);
                ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &zcb[c2]);
                effects_bind_ps(cx, po);
                effects_draw(cx, 3);
                ID3D11DeviceContext_Draw(ctx, 3, 0);
                /* the next draw of something else: the game's state back */
                ID3D11DeviceContext_PSSetShader(ctx, red, NULL, 0);
                effects_bind_ps(cx, red);
                effects_draw(cx, 3);
                ID3D11RasterizerState* now = NULL;
                ID3D11DeviceContext_RSGetState(ctx, &now);
                restored += now == game_rs;
                if (now)
                    ID3D11RasterizerState_Release(now);
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st, (ID3D11Resource*)rt);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
                    got[c2][on] = ((const float*)m.pData)[1]; /* green: the outline drawn */
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st, 0);
                }
                cases_ok++;
            }
        g_cfg.q_ol_depth = 0, g_cfg.q_outline_hooks = 0;
        effects_reset(cx);
    }
    printf("        on WARP (reversed float depth, 5 m): outline 0.1%% nearer drawn %s / with 0.3%% offset %s; 1%% nearer "
           "%s / %s; the game's state bound back %d of 4\n", got[1][0] > 0.5f ? "yes" : "no", got[1][1] > 0.5f ? "yes" : "no",
           got[2][0] > 0.5f ? "yes" : "no", got[2][1] > 0.5f ? "yes" : "no", restored);
    EXPECT(cases_ok == 4 && got[1][0] > 0.5f && got[1][1] < 0.5f && got[2][0] > 0.5f && got[2][1] > 0.5f,
           "OutlineDepthOffset: the line poking through by a hair's breadth hidden, the one with a real gap kept");
    EXPECT(restored == 4, "the game's rasterizer state bound back at the next draw that is not an outline");

    /* Hatching: a toon shader's hatching texture (found by name in its resource list) is swapped for a view that starts
     * 2 mip levels lower, or at its last level with flat; with the option off the game's view is bound back */
    static const char hsrc[] = "Texture2D baseTexture : register(t0); Texture2D toonShadowHatchingTexture : register(t13);"
                               "SamplerState s : register(s12);"
                               "float4 main(float4 p : SV_Position) : SV_Target {"
                               " return baseTexture.Sample(s, p.xy) * toonShadowHatchingTexture.Sample(s, p.yx); }";
    ID3D10Blob* hb = NULL;
    int hslot = -1, top[3] = {-1, -1, -1};
    if (compile && SUCCEEDED(compile(hsrc, sizeof(hsrc) - 1, "h", NULL, NULL, "main", "ps_5_0", 0, 0, &hb, &oerr))) {
        const uint8_t* hc = (const uint8_t*)ID3D10Blob_GetBufferPointer(hb);
        size_t hl = ID3D10Blob_GetBufferSize(hb);
        hslot = dxbc_texture_slot(hc, hl, "toonShadowHatchingTexture");
        ID3D11PixelShader* hps = NULL;
        ID3D11Device_CreatePixelShader(dev, hc, hl, NULL, &hps);
        g_cfg.q_hatch_hooks = 1;
        if (hps)
            effects_shader_created(dev, 0, hps, hc, hl, create);
        D3D11_TEXTURE2D_DESC td = {8, 8, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D* ht = NULL;
        ID3D11ShaderResourceView* hv = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &ht);
        if (ht)
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)ht, NULL, &hv);
        static const int levels[3] = {2, 99, 0};
        for (int k = 0; k < 3 && hps && hv; k++) {
            ID3D11DeviceContext_PSSetShaderResources(ctx, 13, 1, &hv); /* the game binds its own view */
            if (k == 2) { /* off: ours from the frame before is still bound */
                ID3D11ShaderResourceView* ours = hatch_view(ctx, hv, 2);
                ID3D11DeviceContext_PSSetShaderResources(ctx, 13, 1, &ours);
            }
            g_cfg.q_hatch = levels[k];
            effects_bind_ps(cx, hps);
            effects_draw(cx, 3);
            ID3D11ShaderResourceView* now = NULL;
            ID3D11DeviceContext_PSGetShaderResources(ctx, 13, 1, &now);
            if (now) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd;
                ID3D11ShaderResourceView_GetDesc(now, &vd);
                top[k] = now == hv ? -2 : (int)vd.Texture2D.MostDetailedMip; /* -2: the game's own view */
                ID3D11ShaderResourceView_Release(now);
            }
        }
        g_cfg.q_hatch = 0, g_cfg.q_hatch_hooks = 0;
    }
    printf("        Hatching: the texture's slot t%d; bound with 2 -> from mip %d, flat -> from mip %d, off -> %s\n", hslot, top[0],
           top[1], top[2] == -2 ? "the game's view" : "ours");
    EXPECT(hslot == 13 && top[0] == 2 && top[1] == 3 && top[2] == -2,
           "Hatching: the view from 2 levels lower, flat from the last level, off the game's own again");

    /* DumpCharacterTextures: a character 2 m away drawn with a material whose baseTexture (t0) has 3 mip levels: each
     * level written as a DDS file (DX10 header) with the texture's bytes */
    static const char msrc[] = "Texture2D baseTexture : register(t0); SamplerState s : register(s12);"
                               "float4 main(float4 p : SV_Position) : SV_Target { return baseTexture.Sample(s, p.xy); }";
    ID3D10Blob* mb = NULL;
    int dump_ok = 0, dump_files = 0;
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    strncat(tmp, "mcpu_fx_dump", sizeof(tmp) - strlen(tmp) - 1);
    CreateDirectoryA(tmp, NULL);
    if (compile && SUCCEEDED(compile(msrc, sizeof(msrc) - 1, "m", NULL, NULL, "main", "ps_5_0", 0, 0, &mb, &oerr))) {
        const uint8_t* mc = (const uint8_t*)ID3D10Blob_GetBufferPointer(mb);
        size_t ml = ID3D10Blob_GetBufferSize(mb);
        ID3D11PixelShader* mps = NULL;
        ID3D11Device_CreatePixelShader(dev, mc, ml, NULL, &mps);
        g_cfg.q_outline_hooks = 1, g_cfg.dump_tex = 1, g_cfg.dump_dist = 8, g_td_armed = 1; /* as after F8 */
        snprintf(g_cfg.output_folder, sizeof(g_cfg.output_folder), "%s", tmp);
        if (mps)
            effects_shader_created(dev, 0, mps, mc, ml, create);
        static uint32_t px[16 + 4 + 1];
        for (int i = 0; i < 21; i++)
            px[i] = 0x01020304u * (uint32_t)(i + 1);
        D3D11_SUBRESOURCE_DATA md[3] = {{px, 16, 0}, {px + 16, 8, 0}, {px + 20, 4, 0}};
        D3D11_TEXTURE2D_DESC td = {4, 4, 3, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D* mt = NULL;
        ID3D11ShaderResourceView* mv = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, md, &mt);
        if (mt)
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)mt, NULL, &mv);
        if (mps && mv) {
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &mv);
            s_node_dist = 2, s_carried = 0;
            effects_bind_ps(cx, mps);
            effects_draw(cx, 3);
            effects_present();
            char path[MAX_PATH];
            for (int mip = 0; mip < 3; mip++) {
                snprintf(path, sizeof(path), "%s\\tex00_base_mip%d_%dx%d.dds", g_td_dir, mip, 4 >> mip, 4 >> mip);
                FILE* fd = fopen(path, "rb");
                if (!fd)
                    continue;
                uint8_t buf[256];
                size_t got = fread(buf, 1, sizeof(buf), fd);
                fclose(fd);
                DeleteFileA(path);
                size_t bytes = (size_t)(4 >> mip) * (4 >> mip) * 4;
                const uint32_t* want = mip == 0 ? px : mip == 1 ? px + 16 : px + 20;
                dump_files++;
                dump_ok += got == 4 + 124 + 20 + bytes && !memcmp(buf, "DDS ", 4) && *(const uint32_t*)(buf + 128) == 28 &&
                           !memcmp(buf + 148, want, bytes);
            }
            RemoveDirectoryA(g_td_dir);
        }
        g_cfg.q_outline_hooks = 0, g_cfg.dump_tex = 0, s_node_dist = -1;
    }
    RemoveDirectoryA(tmp);
    printf("        DumpCharacterTextures: %d of 3 mip files written, %d with the DX10 header and the texture's bytes\n",
           dump_files, dump_ok);
    EXPECT(dump_files == 3 && dump_ok == 3, "texture dump: every mip level of a near character's base texture as DDS");

    /* CompareShots=10: the pixel shaders that drew the near characters, most draws first */
    static PsInfo kp[3];
    kp[0].hash = 0xAAull, kp[1].hash = 0xBBull, kp[2].hash = 0xCCull;
    for (int i = 0; i < 2; i++)
        near_ps_note(&kp[0]);
    for (int i = 0; i < 5; i++)
        near_ps_note(&kp[1]);
    near_ps_note(&kp[2]);
    uint64_t order[4] = {0, 0, 0, 0};
    int no = fx_near_char_ps(order, 4);
    printf("        near characters' pixel shaders: %d, by draws %llx %llx %llx\n", no, (unsigned long long)order[0],
           (unsigned long long)order[1], (unsigned long long)order[2]);
    EXPECT(no == 3 && order[0] == 0xBB && order[1] == 0xAA && order[2] == 0xCC,
           "knock-out shots: the near characters' pixel shaders, most draws first");

    /* LineFix: a line mesh 0.3 px wide drawn diagonally across a 64x64 target breaks into dashes (rows with no pixel);
     * drawn again shifted by half a pixel (2 copies, as the draw hooks do) or conservatively rasterized it is
     * continuous; left out it draws nothing. A draw of the same shader on a prop (not a character) is left alone */
    static const char lvs[] = "float4 main(uint id : SV_VertexID) : SV_Position {"
                              " float2 a = float2(-0.83, -0.9), b = float2(0.71, 0.86), n = normalize(float2(-1.76, 1.54)) * 0.0047;"
                              " float2 p[6] = {a - n, b - n, b + n, a - n, b + n, a + n};"
                              " return float4(p[id], 0.5, 1); }";
    static const char lps[] = "float4 main() : SV_Target { return float4(1, 1, 1, 1); }";
    ID3D10Blob *lvb = NULL, *lpb = NULL;
    int gaps[7] = {-1, -1, -1, -1, -1, -1, -1}, lit[7] = {0, 0, 0, 0, 0, 0, 0};
    if (compile && SUCCEEDED(compile(lvs, sizeof(lvs) - 1, "lv", NULL, NULL, "main", "vs_5_0", 0, 0, &lvb, &oerr)) &&
        SUCCEEDED(compile(lps, sizeof(lps) - 1, "lp", NULL, NULL, "main", "ps_5_0", 0, 0, &lpb, &oerr))) {
        ID3D11VertexShader* lv = NULL;
        ID3D11PixelShader* lp = NULL;
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(lvb), ID3D10Blob_GetBufferSize(lvb), NULL, &lv);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(lpb), ID3D10Blob_GetBufferSize(lpb), NULL, &lp);
        g_cfg.q_outline_hooks = 1;
        if (lp)
            effects_shader_created(dev, 0, lp, ID3D10Blob_GetBufferPointer(lpb), ID3D10Blob_GetBufferSize(lpb), create);
        const PsInfo* li = fx_ps_info(fx_ps_id(lp));
        g_cfg.q_line_hooks = 1, g_cfg.q_line_n = 1, g_cfg.q_line_ps[0] = li ? li->hash : 0, g_cfg.q_line_dist = 0;
        D3D11_TEXTURE2D_DESC td = {64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ID3D11Texture2D *rt = NULL, *st = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
        td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        ID3D11RenderTargetView* rtv = NULL;
        if (rt)
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)rt, NULL, &rtv);
        D3D11_RASTERIZER_DESC nd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
        ID3D11RasterizerState* nocull = NULL;
        ID3D11Device_CreateRasterizerState(dev, &nd, &nocull);
        /* modes: game, redraw 2, redraw 4, conservative, left out, redraw 2 on a prop (no character), thick (8) */
        static const int modes[7] = {0, 1, 1, 2, 3, 1, 1}, copies[7] = {0, 2, 4, 0, 0, 2, 8};
        for (int k = 0; k < 7 && lv && lp && rtv && st && nocull; k++) {
            static const float black[4] = {0, 0, 0, 0};
            D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_RSSetState(ctx, nocull);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, lv, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, lp, NULL, 0);
            g_cfg.q_line_mode = modes[k], g_cfg.q_line_redraws = copies[k];
            s_node_dist = k == 5 ? -2 : 3, s_carried = 0; /* a character 3 m away; a prop */
            effects_bind_ps(cx, lp);
            if (!effects_draw(cx, 6))
                ID3D11DeviceContext_Draw(ctx, 6, 0);
            int nr = effects_redraws(cx); /* the draw hooks' shifted copies (gpuprobe.c LINE_REDRAW) */
            for (int j = 0; j < nr && j < 8; j++) {
                static const float off[8][2] = {{0.34f, 0}, {0, 0.34f}, {-0.34f, 0}, {0, -0.34f},
                                                {0.68f, 0}, {0, 0.68f}, {-0.68f, 0}, {0, -0.68f}};
                D3D11_VIEWPORT sv = vp;
                sv.TopLeftX += off[j][0], sv.TopLeftY += off[j][1];
                ID3D11DeviceContext_RSSetViewports(ctx, 1, &sv);
                ID3D11DeviceContext_Draw(ctx, 6, 0);
            }
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            effects_bind_ps(cx, pb); /* the next draw: anything else (the conservative state goes back) */
            effects_draw(cx, 3);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st, (ID3D11Resource*)rt);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
                gaps[k] = 0;
                for (int y = 8; y < 56; y++) { /* the line's middle rows */
                    int any = 0;
                    for (int x = 0; x < 64; x++) {
                        int on = ((const uint8_t*)m.pData)[(size_t)y * m.RowPitch + x * 4] > 0;
                        any |= on, lit[k] += on;
                    }
                    gaps[k] += !any;
                }
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st, 0);
            }
        }
        g_cfg.q_line_hooks = 0, g_cfg.q_line_mode = 0, g_cfg.q_outline_hooks = 0, s_node_dist = -1;
        effects_reset(cx);
    }
    printf("        LineFix on a 0.3 px line: rows with a gap / pixels lit - game %d/%d, redraw 2: %d/%d, redraw 4: %d/%d, "
           "conservative %d/%d (tier %d), left out %d/%d, a prop with redraw %d/%d\n", gaps[0], lit[0], gaps[1], lit[1],
           gaps[2], lit[2], gaps[3], lit[3], g_cr_tier, gaps[4], lit[4], gaps[5], lit[5]);
    printf("        LineFix thick (8 copies): rows with a gap %d, pixels lit %d (%.1f per row)\n", gaps[6], lit[6], lit[6] / 48.0);
    EXPECT(gaps[6] == 0 && lit[6] >= 80 && lit[6] <= 130, "LineFix thick: continuous and about 2 px wide");
    EXPECT(gaps[0] > 5 && gaps[1] < gaps[0] && gaps[2] == 0 && lit[2] <= 52 && lit[4] == 0 && gaps[5] == gaps[0] &&
               lit[5] == lit[0] &&
               (g_cr_tier > 0 ? gaps[3] == 0 : gaps[3] == gaps[0]),
           "LineFix: the sub-pixel line breaks as the game draws it; 4 shifted copies make it continuous and still about a "
           "pixel wide (2 fewer gaps); conservative where the GPU has it; left out draws nothing; props untouched");

    /* TemporalStabilise on a 32x32 image: the left half is a character (flag 4 in gbuffer1's alpha), the right half
     * not. Frames 0-7, the camera still: a pixel and its neighbour swap 0 and 1 every frame (a thin feature moving by a
     * pixel) on both halves; a bright dot stays 6 frames, then jumps 8 px (out of its 3x3 box); a pixel changes colour
     * where the distance doubles (another surface) while its neighbour keeps the old colour. Frames 8-15: the camera
     * pans a pixel a frame past a character standing still (a checkerboard fixed in the world: as drawn every pixel
     * flips, the history must be found a pixel over). Frames 16-23: the camera keeps moving, the character with it (the
     * checkerboard fixed on screen: the history is at the same pixel). From frame 8 the rest of the image is stabilised
     * too (TemporalStabiliseWorld 50%): the right half must stay as sharp; frames 24-31, the camera still again, its
     * flickering pixel pair must be steadied. Frames 32-39: bars on the character move 2 px a frame by themselves (a
     * walking character), the camera still; the motion target says so (CharacterMotionVectors): the character must stay
     * as sharp as drawn. Run as the SMAA edge pass would: the image at t0
     * and the game's target bound, which must be bound again afterwards; the view constants (GFD_PSCONST_SYSTEM, a
     * dynamic buffer as the game's) at the painterly pass's b0. */
    {
        enum { S = 32 };
        D3D11_TEXTURE2D_DESC cd = {S, S, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D *col = NULL, *gt = NULL, *fl = NULL, *dp = NULL, *rb = NULL;
        ID3D11Device_CreateTexture2D(dev, &cd, NULL, &col);
        ID3D11Device_CreateTexture2D(dev, &cd, NULL, &gt); /* the game's own target of the pass (the SMAA edges) */
        static uint8_t fdata[S * S * 4];
        static float ddata[S * S];
        for (int i = 0; i < S * S; i++)
            fdata[i * 4 + 3] = (i % S) < 16 ? 4 : 0, ddata[i] = 0.5f;
        D3D11_SUBRESOURCE_DATA fi = {fdata, S * 4, 0}, di = {ddata, S * 4, 0};
        D3D11_TEXTURE2D_DESC fd = cd;
        fd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(dev, &fd, &fi, &fl);
        fd.Format = DXGI_FORMAT_R32_FLOAT, fd.Usage = D3D11_USAGE_DEFAULT;
        ID3D11Device_CreateTexture2D(dev, &fd, &di, &dp);
        fd = cd, fd.Usage = D3D11_USAGE_STAGING, fd.BindFlags = 0, fd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &fd, NULL, &rb);
        ID3D11ShaderResourceView *csv = NULL, *fsv = NULL, *dsv = NULL;
        ID3D11RenderTargetView* gtv = NULL;
        if (col && gt && fl && dp && rb) {
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)col, NULL, &csv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)fl, NULL, &fsv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)dp, NULL, &dsv);
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)gt, NULL, &gtv);
        }
        g_cfg.q_stab = 0.6f, g_cfg.q_stab_hooks = 1;
        /* the view: no rotation, the camera at (cx, 0, 0); view z = -2 / depth (depth 0.5: 4 units in front), view xy =
         * -view z * ndc: at that distance a pixel is 0.25 units */
        D3D11_BUFFER_DESC sbd = {288, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Buffer* sysb = NULL;
        ID3D11Device_CreateBuffer(dev, &sbd, NULL, &sysb);
        float flick[2][8], other[2][8], dot_old = -1, dot_new = -1, surf = -1, still = -1, first = -1;
        float pan_err = -1, follow_err = -1, wpan_err = -1, wfollow_err = -1, wflick[2] = {-1, -1}, walk_err = -1;
        /* the motion target of a walking character: the left half 2 px to the right of where it was, 4 units away */
        ID3D11Texture2D* fmv = NULL;
        ID3D11ShaderResourceView* fmv_srv = NULL;
        {
            static uint16_t mvd[S * S * 4];
            for (int i = 0; i < S * S; i++) {
                int cx_ = i % S < 16;
                mvd[i * 4 + 0] = to_half(cx_ ? -2.0f : 0), mvd[i * 4 + 1] = 0;
                mvd[i * 4 + 2] = to_half(cx_ ? 4.0f : 0), mvd[i * 4 + 3] = to_half(cx_ ? 1.0f : 0);
            }
            D3D11_TEXTURE2D_DESC md = {S, S, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                       D3D11_BIND_SHADER_RESOURCE, 0, 0};
            D3D11_SUBRESOURCE_DATA mi = {mvd, S * 8, 0};
            ID3D11Device_CreateTexture2D(dev, &md, &mi, &fmv);
            if (fmv)
                ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)fmv, NULL, &fmv_srv);
        }
        int restored = 1;
        static uint16_t img[S * S * 4];
        static float sharp[S * S]; /* frames 15 and 23: the image as drawn */
        for (int fr = 0; fr < 40 && csv && fsv && dsv && gtv && sysb && fmv_srv; fr++) {
            if (fr >= 32) { /* the motion target, as a replay of this frame would leave it */
                g_cfg.q_mv = 1;
                REL(V.mv_srv);
                V.mv_srv = fmv_srv, ID3D11ShaderResourceView_AddRef(fmv_srv), V.w = V.h = S, V.done++;
            }
            float camx = fr < 8 ? 0 : 0.25f * (float)((fr < 24 ? fr : 23) - 8);
            g_cfg.q_stab_world = fr < 8 ? 0 : 0.5f;
            g_cfg.q_stab_world_taps = fr >= 24 && fr < 32 ? 4 : 8; /* the world's flicker phase: 4 neighbours */
            D3D11_MAPPED_SUBRESOURCE sm;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)sysb, 0, D3D11_MAP_WRITE_DISCARD, 0, &sm))) {
                float* k = (float*)sm.pData;
                memset(k, 0, 288);
                for (int r = 0; r < 4; r++)
                    k[(1 + r) * 4 + r] = 1, k[(5 + r) * 4 + r] = 1; /* mtxView, mtxInvView: identity rotation */
                k[1 * 4 + 3] = -camx, k[5 * 4 + 3] = camx;
                k[17 * 4 + 0] = 1, k[17 * 4 + 1] = 1, k[17 * 4 + 2] = 0, k[17 * 4 + 3] = -2; /* invProjParams */
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)sysb, 0);
            }
            for (int i = 0; i < S * S; i++) {
                int x = i % S, y = i / S;
                float v = 0.2f;
                if (fr >= 8 && fr < 24) { /* the checkerboard: fixed in the world while panning, then fixed on screen */
                    int col = x + (fr < 16 ? fr - 8 : 7);
                    v = ((col + y) & 1) ? 0.8f : 0.2f;
                    if (fr == 15 || fr == 23)
                        sharp[i] = v;
                } else if (fr >= 32) { /* bars walking right 2 px a frame on the character */
                    if (x < 16)
                        v = ((x - 2 * (fr - 32) + 64) & 7) < 3 ? 0.8f : 0.2f;
                    if (fr == 39)
                        sharp[i] = v;
                } else if (fr >= 24) { /* the world's flickering pair, the camera still */
                    if (y == 8 && x == 24)
                        v = (float)(fr & 1);
                    if (y == 8 && x == 25)
                        v = (float)!(fr & 1);
                } else {
                if (y == 8 && (x == 8 || x == 24))
                    v = (float)(fr & 1);
                if (y == 8 && (x == 9 || x == 25))
                    v = (float)!(fr & 1);
                if (y == 20 && x == (fr < 6 ? 4 : 12))
                    v = 1.0f;
                if (y == 26 && x == 6)
                    v = fr < 6 ? 0.8f : 0.1f;
                if (y == 26 && x == 7)
                    v = 0.8f;
                if (y == 2 && x == 2)
                    v = 0.5f;
                }
                for (int ch = 0; ch < 4; ch++)
                    img[i * 4 + ch] = to_half(ch == 3 ? 1.0f : v);
            }
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)col, 0, NULL, img, S * 8, 0);
            ddata[26 * S + 6] = fr >= 6 && fr < 8 ? 0.25f : 0.5f; /* the distance doubles (frames 6, 7) */
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)dp, 0, NULL, ddata, S * 4, 0);
            ID3D11ShaderResourceView* pin[2] = {dsv, fsv}; /* the painterly pass: depth t2, gbuffer1 t3, its view b0 */
            ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 2, pin);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &sysb);
            temporal_inputs(cx);
            ID3D11Buffer* nulb = NULL;
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &nulb);
            ID3D11ShaderResourceView* nul[2] = {NULL, NULL};
            ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 2, nul);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &gtv, NULL); /* the edge pass: its target, the image at t0 */
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &csv);
            temporal_run(cx);
            ID3D11RenderTargetView* now_rt = NULL;
            ID3D11ShaderResourceView* now_t0 = NULL;
            ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &now_rt, NULL);
            ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 1, &now_t0);
            restored &= now_rt == gtv && now_t0 == csv;
            if (now_rt)
                ID3D11RenderTargetView_Release(now_rt);
            if (now_t0)
                ID3D11ShaderResourceView_Release(now_t0);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, nul);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)rb, (ID3D11Resource*)col);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)rb, 0, D3D11_MAP_READ, 0, &m))) {
#define PX(x, y) from_half(*(const uint16_t*)((const uint8_t*)m.pData + (size_t)(y) * m.RowPitch + (x) * 8))
                if (fr < 8)
                    flick[0][fr] = PX(8, 8), flick[1][fr] = PX(9, 8), other[0][fr] = PX(24, 8), other[1][fr] = PX(25, 8);
                if (fr == 0)
                    first = PX(8, 8);
                if (fr == 6)
                    dot_old = PX(4, 20), dot_new = PX(12, 20), surf = PX(6, 26);
                if (fr == 7)
                    still = PX(2, 2);
                if (fr == 15 || fr == 23) { /* the largest difference from the image as drawn: the character, the rest */
                    float e = 0, ew = 0;
                    for (int y = 1; y < S - 1; y++)
                        for (int x = 1; x < S - 1; x++) {
                            float d = fabsf(PX(x, y) - sharp[y * S + x]);
                            if (x < 15)
                                e = fmaxf(e, d);
                            else if (x > 16)
                                ew = fmaxf(ew, d);
                        }
                    *(fr == 15 ? &pan_err : &follow_err) = e;
                    *(fr == 15 ? &wpan_err : &wfollow_err) = ew;
                }
                if (fr == 30 || fr == 31)
                    wflick[fr - 30] = PX(24, 8);
                if (fr == 39) { /* the walking character, away from the edge it comes in at */
                    walk_err = 0;
                    for (int y = 1; y < S - 1; y++)
                        for (int x = 3; x < 15; x++)
                            walk_err = fmaxf(walk_err, fabsf(PX(x, y) - sharp[y * S + x]));
                }
#undef PX
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)rb, 0);
            }
        }
        g_cfg.q_stab = 0, g_cfg.q_stab_world = 0, g_cfg.q_stab_hooks = 0, g_cfg.q_mv = 0, g_cfg.q_stab_world_taps = 8;
        REL(V.mv_srv), V.w = V.h = 0;
        REL(fmv_srv), REL(fmv);
        double wamp = fabs(sqrt(fmaxf(wflick[1], 0)) - sqrt(fmaxf(wflick[0], 0)));
        /* the flicker as perceived (square roots): 1 between frames as drawn; with 60% history about a quarter */
        double amp = fabs(sqrt(flick[0][7]) - sqrt(flick[0][6])), amp_n = fabs(sqrt(flick[1][7]) - sqrt(flick[1][6]));
        int other_kept = 1;
        for (int fr = 0; fr < 8; fr++)
            other_kept &= other[0][fr] == (float)(fr & 1) && other[1][fr] == (float)!(fr & 1);
        printf("        TemporalStabilise: %d passes; flickering character pixel frames 6/7: %.3f / %.3f (perceived swing "
               "%.2f, its neighbour %.2f; 1 as drawn); first frame %.2f; the dot's old place %.3f, new %.3f; a new surface "
               "%.3f; a still pixel %.3f\n", (int)g_t_runs, flick[0][6], flick[0][7], amp, amp_n, first, dot_old, dot_new,
               surf, still);
        printf("        TemporalStabilise in motion: the camera panning past a still character - largest change from the "
               "image as drawn %.3f; the camera following it %.3f (a still-camera blend of this flicker: about 0.3)\n",
               pan_err, follow_err);
        printf("        TemporalStabiliseWorld (50%%): the rest of the image panning %.3f, following %.3f; its flickering pixel, "
               "the camera still: perceived swing %.2f (1 as drawn)\n", wpan_err, wfollow_err, wamp);
        printf("        TemporalStabilise with motion vectors: bars walking 2 px a frame on a character - largest change "
               "from the image as drawn %.3f (%lld passes used the motion)\n", walk_err, (long long)g_t_mv);
        EXPECT(g_t_runs == 40 && restored, "TemporalStabilise: a pass each frame; the game's target and image bound again");
        EXPECT(walk_err >= 0 && walk_err < 0.01f && g_t_mv == 8,
               "TemporalStabilise: a character walking by itself stays as sharp as drawn where its motion vectors are known");
        EXPECT(wpan_err >= 0 && wpan_err < 0.01f && wfollow_err >= 0 && wfollow_err < 0.01f && wamp > 0.2 && wamp < 0.45,
               "TemporalStabiliseWorld: the rest of the image as sharp as drawn in motion, its flicker steadied (50%: to "
               "about a third) also with its 4 direct neighbours only (TemporalStabiliseWorldTaps=4)");
        EXPECT(pan_err >= 0 && pan_err < 0.01f && follow_err >= 0 && follow_err < 0.01f,
               "TemporalStabilise: no softening when the camera pans past a still character (its history found where the "
               "camera's motion puts it) nor when the camera follows a character (the same pixel)");
        EXPECT(first == 0.0f && amp > 0.15 && amp < 0.4 && amp_n > 0.15 && amp_n < 0.4 && other_kept,
               "TemporalStabilise: no history on the first frame; a flicker on a character a quarter of its size (60%); "
               "pixels that are not a character exactly as drawn");
        EXPECT(fabs(dot_old - 0.2f) < 0.002 && dot_new > 0.4f && dot_new < 0.5f && fabs(surf - 0.1f) < 0.002 &&
                   fabs(still - 0.5f) < 0.002,
               "TemporalStabilise: a feature that moved out of its 3x3 box leaves no trail, and fades in where it arrived "
               "(45% of it in the first frame: no motion vectors); where the distance changed the history is dropped; a "
               "still pixel keeps its value");
        if (csv)
            ID3D11ShaderResourceView_Release(csv);
        if (fsv)
            ID3D11ShaderResourceView_Release(fsv);
        if (dsv)
            ID3D11ShaderResourceView_Release(dsv);
        if (gtv)
            ID3D11RenderTargetView_Release(gtv);
        if (sysb)
            ID3D11Buffer_Release(sysb);
    }

    /* CharacterMotionVectors on a 64x64 characters' pass (RGBA16F, RGBA8, RGBA16F, R32F targets, D24 depth): a skinned
     * quad (its vertex buffer UAV-capable, rewritten as the game's skinning does) on the left, a still quad behind on the
     * right, one dynamic transform buffer re-mapped for each draw (VS b1, mtxLocalToWorldViewProj in c4..c7). Frame 0:
     * nothing known yet. Frame 1: the quad's vertices move 3 px right (skinning). Frame 2: its transform moves it 2 px.
     * The meshes are drawn as the game does: a depth pre-pass, then with the depth test and no depth writes (0.48.3 left
     * such draws out). Frame 3 comes two frames later (one missed): nothing to match. Frame 4, with MotionVectorView: the vertices move
     * 1 px right; the view's encoding (0.5 + motion / 16, 1 where matched; blended with MAX, so never negative: 0.48.1
     * showed motion one way only). Frame 5: MotionVectorSkinDistance 2 m, the quad (3 m away) moves 2 px by its vertices:
     * beyond the distance only its transform's motion counts (none), and its buffer is not copied. Frames 6 and 7: the same
     * with 5 characters around (a fight, at most MotionVectorSkinCrowd 12), the vertices moving 1 px a frame: frame 6 has
     * no copy of frame 5's vertices (none was made), so nothing; frame 7 then the vertices' own -1 px. Leaving the pass
     * draws the motion. */
    {
        enum { W = 64 };
        static const char gvs[] = "cbuffer T : register(b1) { float4 m[8]; } float4 main(float3 p : POSITION0) : SV_Position {"
                                  " float4 q = float4(p, 1); return float4(dot(q, m[4]), dot(q, m[5]), dot(q, m[6]), dot(q, m[7])); }";
        static const char gps[] = "struct O { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float d : SV_Target3; };"
                                  " O main() { O o; o.a = 1; o.b = 1; o.c = 0; o.d = 1; return o; }";
        ID3D10Blob *gvb = NULL, *gpb = NULL;
        ID3D11VertexShader* gv = NULL;
        ID3D11PixelShader* gp = NULL;
        ID3D11InputLayout* gl = NULL;
        if (compile && SUCCEEDED(compile(gvs, sizeof(gvs) - 1, "gv", NULL, NULL, "main", "vs_5_0", 0, 0, &gvb, &oerr)) &&
            SUCCEEDED(compile(gps, sizeof(gps) - 1, "gp", NULL, NULL, "main", "ps_5_0", 0, 0, &gpb, &oerr))) {
            ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(gvb), ID3D10Blob_GetBufferSize(gvb), NULL, &gv);
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(gpb), ID3D10Blob_GetBufferSize(gpb), NULL, &gp);
            D3D11_INPUT_ELEMENT_DESC le = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            ID3D11Device_CreateInputLayout(dev, &le, 1, ID3D10Blob_GetBufferPointer(gvb), ID3D10Blob_GetBufferSize(gvb), &gl);
            s_mv_layout = gl;
        }
        ID3D11Texture2D* tex[5] = {NULL};
        ID3D11RenderTargetView* rt[5] = {NULL};
        static const DXGI_FORMAT tf[5] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
        for (int i = 0; i < 5; i++) {
            D3D11_TEXTURE2D_DESC td = {W, W, 1, 1, tf[i], {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
            ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex[i]);
            if (tex[i])
                ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)tex[i], NULL, &rt[i]);
        }
        D3D11_TEXTURE2D_DESC dd = {W, W, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D* dt = NULL;
        ID3D11DepthStencilView* dv = NULL;
        ID3D11Device_CreateTexture2D(dev, &dd, NULL, &dt);
        if (dt) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
            memset(&dvd, 0, sizeof(dvd));
            dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT, dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)dt, &dvd, &dv);
        }
        /* the moving quad: x -0.75..-0.25 in NDC, z 0.5; the still one: x 0..1, z 0.8; y -0.5..0.5 */
        static float qa[12] = {-0.75f, -0.5f, 0.5f, -0.25f, -0.5f, 0.5f, -0.75f, 0.5f, 0.5f, -0.25f, 0.5f, 0.5f};
        static const float qb[12] = {0, -0.5f, 0.8f, 1, -0.5f, 0.8f, 0, 0.5f, 0.8f, 1, 0.5f, 0.8f};
        static const uint16_t qi[6] = {0, 1, 2, 2, 1, 3};
        D3D11_BUFFER_DESC vd = {48, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA va = {qa, 0, 0}, vb_ = {qb, 0, 0}, vi = {qi, 0, 0};
        ID3D11Buffer *vba = NULL, *vbb = NULL, *ibq = NULL, *tcb = NULL;
        ID3D11Device_CreateBuffer(dev, &vd, &va, &vba);
        vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ID3D11Device_CreateBuffer(dev, &vd, &vb_, &vbb);
        D3D11_BUFFER_DESC id = {12, D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        ID3D11Device_CreateBuffer(dev, &id, &vi, &ibq);
        D3D11_BUFFER_DESC cd = {256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &cd, NULL, &tcb);
        D3D11_TEXTURE2D_DESC sd = {W, W, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0,
                                   D3D11_CPU_ACCESS_READ, 0};
        ID3D11Texture2D* mrb = NULL;
        ID3D11Device_CreateTexture2D(dev, &sd, NULL, &mrb);
        /* the game's draws with a depth bias (1000 units of D24: 6e-5): an exact depth match finds nothing (PC, 0.48.0) */
        D3D11_RASTERIZER_DESC mrd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 1000, 0, 0, TRUE, FALSE, FALSE, FALSE};
        ID3D11RasterizerState* mnc = NULL;
        ID3D11Device_CreateRasterizerState(dev, &mrd, &mnc);
        /* a particle: blended, in the top-left corner (x -1..-0.8, y 0.6..0.9: pixels 0..3, rows 3..12) */
        static const float qc[12] = {-1, 0.6f, 0.3f, -0.8f, 0.6f, 0.3f, -1, 0.9f, 0.3f, -0.8f, 0.9f, 0.3f};
        D3D11_BUFFER_DESC cvd = {48, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA cvi = {qc, 0, 0};
        ID3D11Buffer* vbc = NULL;
        ID3D11Device_CreateBuffer(dev, &cvd, &cvi, &vbc);
        D3D11_BLEND_DESC pbd;
        memset(&pbd, 0, sizeof(pbd));
        pbd.RenderTarget[0].BlendEnable = TRUE, pbd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        pbd.RenderTarget[0].SrcBlend = pbd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
        pbd.RenderTarget[0].DestBlend = pbd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        pbd.RenderTarget[0].BlendOp = pbd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        ID3D11BlendState* pbs = NULL;
        ID3D11Device_CreateBlendState(dev, &pbd, &pbs);
        float particle_a = -1;
        LONG64 transp0 = g_mv_transp;
        D3D11_DEPTH_STENCIL_DESC nwd;
        memset(&nwd, 0, sizeof(nwd));
        nwd.DepthEnable = TRUE, nwd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, nwd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11DepthStencilState* nws = NULL; /* after the pre-pass: tested, not written */
        ID3D11Device_CreateDepthStencilState(dev, &nwd, &nws);
        float got[8][4]; /* per frame: the moving quad's motion x, its alpha, the still quad's x, alpha; [3]: empty alpha */
        float empty_a[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        LONG64 copies5 = 0;
        int ran = 0;
        for (int fr = 0; fr < 8 && gv && gp && gl && rt[0] && rt[4] && dv && vba && vbb && ibq && tcb && mrb && mnc && nws; fr++) {
            s_seq = fr < 3 ? fr + 1 : fr + 2, s_node_dist = 3, s_carried = 0;
            g_cfg.q_mv = 1, g_cfg.q_mv_dist = 40, g_cfg.q_mv_view = fr == 4, g_cfg.q_mv_skin_dist = fr >= 5 ? 2.0f : 20.0f;
            g_cfg.q_mv_crowd = 12, s_crowd = fr >= 6 ? 5 : 100;
            if (fr == 6) /* the fight began CROWD_HIST frames ago: its count in all of them */
                for (LONG64 k = s_seq - CROWD_HIST + 1; k < s_seq; k++)
                    crowd_note(k, 5);
            if (fr == 5)
                copies5 = g_mv_skins;
            if (fr == 1 || fr >= 4) { /* skinning: the vertices 3 px right (3 / 32 in NDC), then 1, 2, 1, 1 */
                for (int v = 0; v < 4; v++)
                    qa[v * 3] += (fr == 1 ? 3.0f : fr == 5 ? 2.0f : 1.0f) / 32.0f;
                ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)vba, 0, NULL, qa, 0, 0);
            }
            void* rts[4] = {rt[0], rt[1], rt[2], rt[3]};
            motion_bind(cx, 4, rts, dv);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, rt, dv);
            ID3D11DeviceContext_ClearDepthStencilView(ctx, dv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            D3D11_VIEWPORT vp = {0, 0, W, W, 0, 1};
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_RSSetState(ctx, mnc);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
            ID3D11DeviceContext_IASetInputLayout(ctx, gl);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, ibq, DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_VSSetShader(ctx, gv, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, gp, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &tcb);
            for (int q = 0; q < 2; q++) {
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)tcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    float* k = (float*)m.pData;
                    memset(k, 0, 256);
                    for (int r = 0; r < 4; r++)
                        k[(4 + r) * 4 + r] = 1;
                    if (q == 0 && fr >= 2) /* the transform: 2 px right */
                        k[4 * 4 + 3] = 2.0f / 32.0f;
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)tcb, 0);
                }
                UINT st = 12, of = 0;
                ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, q ? &vbb : &vba, &st, &of);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0); /* the pre-pass: not recorded */
                ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, nws, 0);
                ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
                motion_draw(cx, 6, 1, 0, 0);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
            }
            if (vbc && pbs) { /* the particle, blended, after the opaque meshes */
                UINT st = 12, of = 0;
                ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbc, &st, &of);
                ID3D11DeviceContext_OMSetBlendState(ctx, pbs, NULL, 0xffffffffu);
                ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
                motion_draw(cx, 6, 1, 0, 0);
                ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffffu);
            }
            void* one[1] = {rt[4]};
            motion_bind(cx, 1, one, NULL); /* leaving the pass: the motion drawn */
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rt[4], NULL);
            ran++;
            if (V.mv) {
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)mrb, (ID3D11Resource*)V.mv);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)mrb, 0, D3D11_MAP_READ, 0, &m))) {
#define MV(x, y, ch) from_half(((const uint16_t*)((const uint8_t*)m.pData + (size_t)(y) * m.RowPitch + (x) * 8))[ch])
                    /* the moving quad covers x 8..24 (frame 0), 11..27 (1), 13..29 (2); the still one x 32..63 */
                    got[fr][0] = MV(20, 32, 0), got[fr][1] = MV(20, 32, 3), got[fr][2] = MV(48, 32, 0);
                    got[fr][3] = MV(48, 32, 3), empty_a[fr] = MV(4, 4, 3);
                    if (fr == 2)
                        particle_a = MV(1, 8, 3);
#undef MV
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)mrb, 0);
                }
            }
        }
        LONG64 copies_far = g_mv_skins - copies5 - 2; /* frames 6 and 7 copy again (a fight) */
        g_cfg.q_mv = 0, g_cfg.q_mv_view = 0, g_cfg.q_mv_skin_dist = 20.0f, s_crowd = 100;
        printf("        CharacterMotionVectors in a fight (5 characters, MotionVectorSkinCrowd 12), beyond the skin distance: "
               "frame 6 alpha %.0f (no copy of the frame before), frame 7 %.2f (alpha %.0f)\n", got[6][1], got[7][0],
               got[7][1]);
        EXPECT(got[6][1] == 0 && fabsf(got[7][0] + 1) < 0.05f && got[7][1] == 1,
               "MotionVectorSkinCrowd: in a fight (few characters) a skinned mesh beyond the skin distance keeps its own "
               "motion (after one frame to copy it)");
        printf("        CharacterMotionVectors beyond MotionVectorSkinDistance (vertices moved 2 px): %.2f (alpha %.0f), "
               "%lld copies\n", got[5][0], got[5][1], (long long)copies_far);
        EXPECT(fabsf(got[5][0]) < 0.05f && got[5][1] == 1 && copies_far == 0,
               "MotionVectorSkinDistance: a skinned mesh beyond it gets its transform's motion only, its vertices not "
               "copied");
        printf("        CharacterMotionVectors view (1 px right): moving quad %.4f %s, still quad %.4f (alpha %.0f / %.0f)\n",
               got[4][0], got[4][1] == 1 ? "matched" : "not matched", got[4][2], got[4][1], got[4][3]);
        printf("        CharacterMotionVectors: %d frames, %lld passes drawn again, %lld draws, %lld skinned copies | moving "
               "quad: frame 0 alpha %.0f; skinned 3 px: %.2f (alpha %.0f); transform 2 px: %.2f (alpha %.0f); after a missed "
               "frame alpha %.0f | still quad %.2f (alpha %.0f) | empty pixel alpha %.0f\n", ran, (long long)g_mv_replays,
               (long long)g_mv_draws, (long long)g_mv_skins, got[0][1], got[1][0], got[1][1], got[2][0], got[2][1], got[3][1],
               got[1][2], got[1][3], empty_a[1]);
        EXPECT(fabsf(got[4][0] - (0.5f - 1.0f / 16)) < 0.002f && got[4][1] == 1 && fabsf(got[4][2] - 0.5f) < 0.002f &&
                   got[4][3] == 1,
               "MotionVectorView: the motion encoded never negative (0.5 + px / 16), matched pixels marked; a still mesh "
               "at the middle");
        EXPECT(ran == 8 && got[0][1] == 0 && fabsf(got[1][0] + 3) < 0.05f && got[1][1] == 1 && fabsf(got[2][0] + 2) < 0.05f &&
                   got[2][1] == 1 && got[3][1] == 0,
               "CharacterMotionVectors: a skinned mesh's motion from its previous vertices (3 px) and from its previous "
               "transform (2 px), back to where it was, although the game's depth is biased and the draws after a depth "
               "pre-pass do not write depth; nothing on the first frame nor after a missed one");
        /* a pass with the characters' targets but no character in it (the particles' pass, the sky's): nothing recorded,
         * nothing drawn again, the transforms not even copied. Its first draw is an effect near a character (counted as
         * carried by it: tablet log 30), the others are not characters */
        LONG64 replays0 = g_mv_replays, nochar0 = g_mv_nochar;
        if (gv && gp && gl && rt[0] && rt[4] && dv && vbb && ibq && tcb) {
            s_seq = 11, s_node_dist = -2, g_cfg.q_mv = 1;
            void* rts[4] = {rt[0], rt[1], rt[2], rt[3]};
            motion_bind(cx, 4, rts, dv);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, rt, dv);
            UINT st = 12, of = 0;
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbb, &st, &of);
            for (int k = 0; k < 3; k++) {
                s_node_dist = k ? -2 : 3, s_carried = k == 0;
                ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
                motion_draw(cx, 6, 1, 0, 0);
            }
            s_carried = 0;
            void* one[1] = {rt[4]};
            motion_bind(cx, 1, one, NULL);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rt[4], NULL);
            g_cfg.q_mv = 0, s_node_dist = -1;
        }
        printf("        CharacterMotionVectors: a blended particle's pixel alpha %.0f, particle draws left out %lld | a pass "
               "without characters: %lld draws left out, %lld passes drawn again\n", particle_a,
               (long long)(g_mv_transp - transp0), (long long)(g_mv_nochar - nochar0), (long long)(g_mv_replays - replays0));
        EXPECT(g_mv_nochar - nochar0 == 3 && g_mv_replays == replays0,
               "CharacterMotionVectors: a pass with the characters' targets but no character (particles) is not drawn again");
        EXPECT(particle_a == 0 && g_mv_transp - transp0 == ran,
               "CharacterMotionVectors: blended draws (particles, effects) are not drawn again (each frame's one left out)");
        EXPECT(fabsf(got[1][2]) < 0.05f && got[1][3] == 1 && empty_a[1] == 0,
               "CharacterMotionVectors: a still mesh behind it no motion (the transform copied per draw from one re-mapped "
               "buffer); pixels with no mesh unknown");
        /* MotionVectorSkinCrowd decided by the most characters around in the last CROWD_HIST frames: a town's count
         * swinging 5 / 18 with the camera stays a town; a fight is one after CROWD_HIST frames of few characters */
        {
            int town = 1, late = -1;
            LONG64 sq = 1000;
            for (int k = 0; k < 100; k++, sq++)
                town &= crowd_note(sq, k & 1 ? 18 : 5) == 18 || k == 0; /* the first frame has seen only 5 */
            int k = 0;
            for (; k < 200 && crowd_note(sq, 5) != 5; k++, sq++)
                ;
            late = k;
            int back = crowd_note(sq + 1, 14) == 14;
            printf("        MotionVectorSkinCrowd: a town's count swinging 5 / 18 counts as %s; a fight (5) after %d frames "
                   "(CROWD_HIST %d); a crowd again at once: %s\n", town ? "18 every frame" : "SWINGING", late, CROWD_HIST,
                   back ? "yes" : "no");
            EXPECT(town && late == CROWD_HIST - 1 && back,
                   "MotionVectorSkinCrowd: the most characters in the last frames decides (a town's swing does not turn "
                   "the skin distance off; a fight turns it off after CROWD_HIST frames, a crowd back on at once)");
        }
        /* the skinned buffers' table full (the game makes new buffers for each place and fight: the Q48 PC run filled it
         * and in the fight after no character had its limbs' motion): the entry drawn longest ago is reused, its copies
         * holding nothing of the new buffer; one drawn in this frame is never */
        if (V.dev) {
            static char fake[SKIN_N + 3]; /* buffers compared only, never used */
            static char saved[sizeof(V.skin)];
            memcpy(saved, V.skin, sizeof(V.skin));
            int n0 = V.nskin;
            LONG64 seq0 = V.seq, reused0 = g_mv_reused;
            memset(V.skin, 0, sizeof(V.skin));
            V.nskin = 0;
            int filled = 1;
            for (int i = 0; i < SKIN_N; i++) {
                V.seq = 100 + i;
                filled &= skin_of((ID3D11Buffer*)&fake[i], 64) == i;
            }
            V.seq = 100 + SKIN_N;
            V.skin[0].seq[0] = V.seq - 1; /* entry 0 holds a copy of the frame before (of its old buffer) */
            int again = skin_of((ID3D11Buffer*)&fake[1], 64);
            int fresh = skin_of((ID3D11Buffer*)&fake[SKIN_N], 64);
            int fresh_seq = V.skin[fresh < 0 ? 0 : fresh].seq[0];
            int other = skin_of((ID3D11Buffer*)&fake[SKIN_N + 1], 128);
            D3D11_BUFFER_DESC od = {0};
            if (other >= 0 && V.skin[other].copy[0])
                ID3D11Buffer_GetDesc(V.skin[other].copy[0], &od);
            for (int i = 0; i < SKIN_N; i++)
                V.skin[i].used = V.seq;
            int none = skin_of((ID3D11Buffer*)&fake[SKIN_N + 2], 64);
            printf("        Motion vectors' skinned buffers, %d seen (table full): drawn again -> %d, a new one -> %d (the "
                   "oldest; its copy's frame %d), another size -> %d (%u bytes), all drawn in this frame -> %d; %lld "
                   "reused\n", SKIN_N, again, fresh, fresh_seq, other, od.ByteWidth, none,
                   (long long)(g_mv_reused - reused0));
            EXPECT(filled && again == 1 && fresh == 0 && fresh_seq == -9 && other == 2 && od.ByteWidth == 128 && none == -1 &&
                       g_mv_reused - reused0 == 2,
                   "Motion vectors: a full table of skinned buffers reuses the one drawn longest ago (its copies made "
                   "anew, holding nothing yet), never one of this frame's");
            for (int i = 0; i < SKIN_N; i++)
                REL(V.skin[i].copy[0]), REL(V.skin[i].copy[1]);
            memcpy(V.skin, saved, sizeof(V.skin));
            V.nskin = n0, V.seq = seq0;
        }
        for (int i = 0; i < 5; i++) {
            REL(rt[i]), REL(tex[i]);
        }
        REL(dv), REL(dt), REL(vba), REL(vbb), REL(ibq), REL(tcb), REL(mrb), REL(gv), REL(gp), REL(gl), REL(gvb), REL(gpb), REL(mnc), REL(vbc), REL(pbs), REL(nws);
    }
    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
