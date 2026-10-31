/* Far meshes (farmesh.c) on a WARP device: a bumpy grid in immutable buffers is queued by a far
 * scene draw, read back over a few frames, simplified in the background, and then drawn as its
 * simpler version - the coarsest one whose error stays under the pixel limit at the distance.
 * Also: no version when near, the mode switch, a draw outside known objects stops the swap for
 * good, and meshes the game let go are freed. */
#define COBJMACROS
#include "../src/farmesh.c"
#include <d3dcommon.h>
#include <stdio.h>

static int fails;
#define EXPECT(c, what)                                                                                         \
    do {                                                                                                        \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                     \
        if (!(c))                                                                                               \
            fails++;                                                                                            \
    } while (0)

/* the indices of a version, read back */
static int lod_indices(ID3D11Device* dev, ID3D11DeviceContext* imm, const FmPick* p, uint16_t* out)
{
    D3D11_BUFFER_DESC d = {p->count * 2, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer* st = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, NULL, &st)))
        return 0;
    D3D11_BOX box = {p->start * 2, 0, 0, (p->start + p->count) * 2, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(imm, (ID3D11Resource*)st, 0, 0, 0, 0, (ID3D11Resource*)p->lod, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp;
    int ok = SUCCEEDED(ID3D11DeviceContext_Map(imm, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &mp));
    if (ok) {
        memcpy(out, mp.pData, p->count * 2);
        ID3D11DeviceContext_Unmap(imm, (ID3D11Resource*)st, 0);
    }
    ID3D11Buffer_Release(st);
    return ok;
}

int main(void)
{
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* imm = NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &imm))) {
        printf("[FAIL] no WARP device\n");
        return 1;
    }
    /* a 40 x 40 grid, 4 m wide, bumps of a few millimetres; behind 100 unused vertices (base vertex 100) and
     * after 6 unused indices */
    enum { G = 40, NV = G * G, NI = (G - 1) * (G - 1) * 6 };
    static float vtx[100 + NV][5];
    for (int i = 0; i < NV; i++) {
        float x = (float)(i % G) * 10.0f, z = (float)(i / G) * 10.0f;
        vtx[100 + i][0] = x, vtx[100 + i][1] = 0.3f * sinf(x * 0.7f) * cosf(z * 0.5f), vtx[100 + i][2] = z;
        vtx[100 + i][3] = x / 400.0f, vtx[100 + i][4] = z / 400.0f;
    }
    static uint16_t idx[6 + NI];
    int n = 6;
    for (int y = 0; y < G - 1; y++)
        for (int x = 0; x < G - 1; x++) {
            uint16_t a = (uint16_t)(y * G + x);
            uint16_t q[6] = {a, (uint16_t)(a + G), (uint16_t)(a + 1), (uint16_t)(a + 1), (uint16_t)(a + G), (uint16_t)(a + G + 1)};
            memcpy(&idx[n], q, sizeof(q));
            n += 6;
        }
    D3D11_BUFFER_DESC vd = {sizeof(vtx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC id = {sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vs = {vtx, 0, 0}, is = {idx, 0, 0};
    ID3D11Buffer *vb = NULL, *ib = NULL;
    ID3D11Device_CreateBuffer(dev, &vd, &vs, &vb);
    ID3D11Device_CreateBuffer(dev, &id, &is, &ib);
    D3D11_INPUT_ELEMENT_DESC el[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    typedef HRESULT(WINAPI * D3DCompile_t)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                           UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t compile = dc ? (D3DCompile_t)(void*)GetProcAddress(dc, "D3DCompile") : NULL;
    static const char src[] = "float4 main(float3 p : POSITION, float2 t : TEXCOORD) : SV_Position { return float4(p + t.xxy, 1); }";
    ID3DBlob* code = NULL;
    ID3D11InputLayout* layout = NULL;
    if (compile && SUCCEEDED(compile(src, sizeof(src) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &code, NULL)))
        ID3D11Device_CreateInputLayout(dev, el, 2, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                                       &layout);
    census_layout_created(layout, el, 2); /* the device hook does this in the game */
    EXPECT(vb && ib && layout, "grid buffers and input layout created");

    g_cfg.q_farmesh = 2, g_cfg.q_fm_both = 1, g_cfg.q_fm_px = 0.5f, g_cfg.q_fm_floor = 10;
    farmesh_init();
    EXPECT(g_init, "far meshes set up (background thread)");
    UINT stride = 20, off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(imm, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(imm, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetInputLayout(imm, layout);

    FmPick p;
    memset(&p, 0, sizeof(p));
    EXPECT(farmesh_pick(imm, NI, 6, 100, 1, 1000.0f, 1.0f, 0.0f, &p) == 0 && farmesh_test_state(ib, 0, 6, NI, 100, NULL) == -1,
           "a mesh first seen 10 m away is not queued");
    EXPECT(farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 1.0f, 0.0f, &p) == 0 && farmesh_test_state(ib, 0, 6, NI, 100, NULL) == FM_NEW,
           "seen 100 m away: queued, the original drawn meanwhile");
    int s = -1, frames = 0;
    for (; frames < 3000 && (s = farmesh_test_state(ib, 0, 6, NI, 100, NULL)) != FM_READY && s != FM_NONE; frames++) {
        farmesh_frame_end(imm);
        Sleep(1);
    }
    printf("        ready after %d frames (state %d)\n", frames, s);
    EXPECT(s == FM_READY, "read back over a few frames, simplified in the background");
    unsigned counts[8];
    farmesh_test_state(ib, 0, 6, NI, 100, counts);
    printf("        versions (triangles): safe %u %u %u %u | seams %u %u %u %u (of %d)\n", counts[0] / 3, counts[1] / 3,
           counts[2] / 3, counts[3] / 3, counts[4] / 3, counts[5] / 3, counts[6] / 3, counts[7] / 3, NI / 3);
    unsigned best = 0;
    int ok_floor = 1;
    for (int k = 0; k < 8; k++)
        if (counts[k]) {
            best = counts[k] > best ? counts[k] : best;
            if (counts[k] < (unsigned)(NI / 10 / 3 * 3) || counts[k] % 3)
                ok_floor = 0;
        }
    EXPECT(best > 0 && best < (unsigned)NI * 6 / 10, "versions with far fewer triangles");
    EXPECT(ok_floor, "no version under 10% of the triangles");

    /* the draw: the version's indices refer to the game's own vertices */
    memset(&p, 0, sizeof(p));
    int got = farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 1.0f, 0.0f, &p);
    EXPECT(got && p.lod && p.count < NI && p.ib == (void*)ib && p.fmt == DXGI_FORMAT_R16_UINT && p.off == 0,
           "100 m: a simpler version to draw, with the game's index buffer to put back");
    static uint16_t li[NI];
    if (got && lod_indices(dev, imm, &p, li)) {
        int in_range = 1;
        for (UINT i = 0; i < p.count; i++)
            if (li[i] >= NV)
                in_range = 0;
        EXPECT(in_range, "its indices are the grid's own vertex numbers (base vertex unchanged)");
    }
    unsigned c100 = got ? p.count : 0, s100 = got ? p.start : 0;
    farmesh_done(&p);
    memset(&p, 0, sizeof(p));
    EXPECT(farmesh_pick(imm, NI, 6, 100, 1, 2000.0f, 1.0f, 0.0f, &p) == 0, "20 m: the original (the finest version's 2 cm "
                                                                     "would be over 0.5 px)");
    got = farmesh_pick(imm, NI, 6, 100, 1, 40000.0f, 1.0f, 0.0f, &p);
    EXPECT(got && p.count <= c100, "400 m: the same or a coarser version");
    farmesh_done(&p);
    got = farmesh_pick(imm, NI, 6, 100, 2, 10000.0f, 1.0f, 0.0f, &p);
    EXPECT(got, "a shadow caster 100 m away: swapped too");
    farmesh_done(&p);
    got = farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 50.0f, 0.0f, &p);
    EXPECT(!got, "scaled up 50x the error grows with it: the original");
    farmesh_done(&p);
    g_cfg.q_farmesh = 1;
    got = farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 1.0f, 0.0f, &p);
    EXPECT(got && p.start < s100, "FarMeshes=safe: the version made with the seams kept (stored before the others)");
    farmesh_done(&p);
    g_cfg.q_farmesh = 0;
    EXPECT(farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 1.0f, 0.0f, &p) == 0, "FarMeshes=off: the original");
    g_cfg.q_farmesh = 2;

    /* FarMeshSeamsFrom: across seams only from that distance; nearer the seams-kept version; casters always across */
    g_cfg.q_fm_seams_from = 150.0f;
    got = farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 1.0f, 0.0f, &p);
    EXPECT(got && p.start < s100, "seams from 150 m, an object 100 m away: the version with the seams kept");
    farmesh_done(&p);
    got = farmesh_pick(imm, NI, 6, 100, 1, 20000.0f, 1.0f, 0.0f, &p);
    EXPECT(got && p.start >= s100, "an object 200 m away: the version across seams");
    farmesh_done(&p);
    got = farmesh_pick(imm, NI, 6, 100, 2, 10000.0f, 1.0f, 0.0f, &p);
    EXPECT(got && p.start >= s100, "a shadow caster 100 m away: across seams (only its outline shows)");
    farmesh_done(&p);
    g_cfg.q_fm_seams_from = 0;

    /* shadow casters judged by their shadow-map texels (FarMeshShadowTexels) */
    g_cfg.q_fm_shadow = 0.5f;
    got = farmesh_pick(imm, NI, 6, 100, 2, 0, 1.0f, 0.1f, &p); /* 10 cm per texel: 2 cm = 0.2 texel */
    EXPECT(got, "caster in a coarse cascade (10 cm per texel), camera distance unknown: swapped");
    farmesh_done(&p);
    EXPECT(farmesh_pick(imm, NI, 6, 100, 2, 0, 1.0f, 1.0f, &p) == 0,
           "caster in a fine cascade (1 cm per texel: 2 cm = 2 texels): the original");
    EXPECT(farmesh_pick(imm, NI, 6, 100, 2, 1000.0f, 1.0f, 0.1f, &p) == 1 && (farmesh_done(&p), 1),
           "a caster 10 m from the camera but in a coarse cascade: swapped (the screen does not decide)");
    g_cfg.q_fm_shadow = 0;
    EXPECT(farmesh_pick(imm, NI, 6, 100, 2, 1000.0f, 1.0f, 0.1f, &p) == 0,
           "FarMeshShadowTexels=0: casters judged by the screen again (10 m: the original)");
    g_cfg.q_fm_shadow = 0.5f;
    farmesh_pick(imm, NI - 6, 6, 100, 2, 0, 1.0f, 0.1f, &p); /* a mesh only seen as a caster so far */
    EXPECT(farmesh_test_state(ib, 0, 6, NI - 6, 100, NULL) == FM_NEW, "a mesh first drawn as a caster in a coarse cascade: queued");
    for (int i = 0, st; i < 3000 && (st = farmesh_test_state(ib, 0, 6, NI - 6, 100, NULL)) != FM_READY && st != FM_NONE; i++) {
        farmesh_frame_end(imm); /* let it finish before the clean-up below */
        Sleep(1);
    }
    g_cfg.q_fm_shadow = 0;

    /* drawn once outside a known object: never swapped again */
    farmesh_pick(imm, NI, 6, 100, 0, 0, 0, 0.0f, &p);
    EXPECT(farmesh_pick(imm, NI, 6, 100, 1, 10000.0f, 1.0f, 0.0f, &p) == 0 && farmesh_test_state(ib, 0, 6, NI, 100, NULL) == 100 + FM_READY,
           "drawn outside a known object once: never swapped");
    farmesh_pick(imm, 1536, 6, 100, 0, 0, 0, 0.0f, &p);
    EXPECT(farmesh_test_state(ib, 0, 6, 1536, 100, NULL) == 100 + FM_NONE,
           "an unknown mesh drawn outside known objects first: kept from ever being swapped");

    /* a "bush": 500 closed pieces of 1 cm scattered over 2 m. Within 2 cm of shape error they would all
     * collapse; the versions lose their surface, so the mesh keeps the original */
    {
        enum { P = 500 };
        static float bv[P * 4][5];
        static uint16_t bi[P * 12];
        for (int i = 0; i < P; i++) {
            float x = (float)(i % 10) * 20.0f, y = (float)((i / 10) % 10) * 20.0f, z = (float)(i / 100) * 20.0f;
            float t[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            for (int k = 0; k < 4; k++)
                bv[i * 4 + k][0] = x + t[k][0], bv[i * 4 + k][1] = y + t[k][1], bv[i * 4 + k][2] = z + t[k][2],
                              bv[i * 4 + k][3] = 0, bv[i * 4 + k][4] = 0;
            uint16_t b = (uint16_t)(i * 4), f[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
            for (int k = 0; k < 12; k++)
                bi[i * 12 + k] = (uint16_t)(b + f[k]);
        }
        D3D11_BUFFER_DESC bvd = {sizeof(bv), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_BUFFER_DESC bid = {sizeof(bi), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA bvs = {bv, 0, 0}, bis = {bi, 0, 0};
        ID3D11Buffer *bvb = NULL, *bib = NULL;
        ID3D11Device_CreateBuffer(dev, &bvd, &bvs, &bvb);
        ID3D11Device_CreateBuffer(dev, &bid, &bis, &bib);
        ID3D11DeviceContext_IASetVertexBuffers(imm, 0, 1, &bvb, &stride, &off);
        ID3D11DeviceContext_IASetIndexBuffer(imm, bib, DXGI_FORMAT_R16_UINT, 0);
        LONG64 rejected0 = st_area_rejected;
        farmesh_pick(imm, P * 12, 0, 0, 1, 10000.0f, 1.0f, 0.0f, &p);
        int bs = -1;
        for (int i = 0; i < 3000 && (bs = farmesh_test_state(bib, 0, 0, P * 12, 0, NULL)) != FM_READY && bs != FM_NONE; i++) {
            farmesh_frame_end(imm);
            Sleep(1);
        }
        EXPECT(bs == FM_NONE && st_area_rejected > rejected0 &&
                   farmesh_pick(imm, P * 12, 0, 0, 1, 10000.0f, 1.0f, 0.0f, &p) == 0,
               "a mesh of small closed pieces (foliage, bushes): versions that lose surface are left out, the "
               "original stays");
        ID3D11DeviceContext_IASetVertexBuffers(imm, 0, 1, &vb, &stride, &off);
        ID3D11DeviceContext_IASetIndexBuffer(imm, ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D11Buffer_Release(bvb);
        ID3D11Buffer_Release(bib);
    }

    /* the game lets the buffers go: after a minute without draws the entries go too */
    ID3D11DeviceContext_ClearState(imm);
    ID3D11Buffer_Release(ib);
    ID3D11Buffer_Release(vb);
    for (int i = 0; i < 2400; i++)
        farmesh_frame_end(imm);
    int gone = 1;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < FM_N; i++)
        if (g_fm[i].state != FM_FREE && g_fm[i].state != FM_DEAD)
            gone = 0;
    ReleaseSRWLockShared(&g_lock);
    EXPECT(gone, "entries freed once the game no longer holds their buffers");
    farmesh_report();
    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
