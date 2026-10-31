/* Mesh census (meshcensus.c) on a WARP device: buffers and an input layout made the way the game
 * makes them, indexed draws filed while recording, then census.txt and the mesh files written and
 * read back (indices rebased to the lowest vertex used, the vertices of that range). */
#define COBJMACROS
#include "../src/meshcensus.c"
#include <d3dcommon.h>
#include <stdio.h>

static int fails;
#define EXPECT(c, what)                                                                                         \
    do {                                                                                                        \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                     \
        if (!(c))                                                                                               \
            fails++;                                                                                            \
    } while (0)

static char* slurp(const char* path, long* len)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((size_t)*len + 1);
    fread(b, 1, (size_t)*len, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

int main(void)
{
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* imm = NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &imm))) {
        printf("[FAIL] no WARP device\n");
        return 1;
    }
    /* a grid of 10x10 vertices (position float3 + uv float2, stride 20) behind 50 unused ones */
    static float vtx[150][5];
    for (int i = 0; i < 100; i++)
        vtx[50 + i][0] = (float)(i % 10), vtx[50 + i][1] = 0, vtx[50 + i][2] = (float)(i / 10), vtx[50 + i][3] = 0.5f;
    static uint16_t idx[600 + 12];
    int n = 12; /* 12 indices of another mesh first */
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            uint16_t a = (uint16_t)(y * 10 + x);
            uint16_t q[6] = {a, (uint16_t)(a + 10), (uint16_t)(a + 1), (uint16_t)(a + 1), (uint16_t)(a + 10), (uint16_t)(a + 11)};
            memcpy(&idx[n], q, sizeof(q));
            n += 6;
        }
    D3D11_BUFFER_DESC vd = {sizeof(vtx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC id = {sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA vs = {vtx, 0, 0}, is = {idx, 0, 0};
    ID3D11Buffer *vb = NULL, *ib = NULL;
    ID3D11Device_CreateBuffer(dev, &vd, &vs, &vb);
    ID3D11Device_CreateBuffer(dev, &id, &is, &ib);
    census_buffer_created(vb, &vd, 1);
    census_buffer_created(ib, &id, 1);
    D3D11_INPUT_ELEMENT_DESC el[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
                                       D3D11_INPUT_PER_VERTEX_DATA, 0}};
    /* a real input layout needs a vertex shader with that input */
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
    EXPECT(layout != NULL, "input layout created (d3dcompiler_47)");
    census_layout_created(layout, el, 2);
    LayInfo li = lay_info(layout);
    EXPECT(li.pos_fmt == DXGI_FORMAT_R32G32B32_FLOAT && li.pos_off == 0 && li.pos_slot == 0 && !li.skinned,
           "input layout: position float3 at offset 0 of slot 0");
    BufInfo bi = buf_info(ib);
    EXPECT(bi.usage == D3D11_USAGE_IMMUTABLE && bi.init == 1, "index buffer: immutable, created with data");

    UINT stride = 20, off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(imm, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(imm, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetInputLayout(imm, layout);
    census_draw(imm, 486, 1, 12, 50, 3, 1000.0f, 60.0f, 0); /* not recording: ignored */
    census_start(3);
    EXPECT(census_recording(), "F9 / census_start: recording");
    for (int f = 0; f < 3; f++) {
        census_draw(imm, 486, 1, 12, 50, 3, 1000.0f, 60.0f + f, 0); /* the grid, 50-100 m */
        census_draw(imm, 486, 1, 12, 50, -1, 0, 0, 1);              /* its shadow */
        census_draw(imm, 12, 1, 0, 0, 0, 5000.0f, 5.0f, 0);         /* another mesh, near */
        census_draw(imm, 486, 1, 12, 50, -1, 0, 0, 0);              /* not a scene draw: ignored */
        census_frame_end(imm);
    }
    EXPECT(!census_recording(), "recording ends after 3 frames");

    /* the newest MetaphorCPU_meshes_* folder next to the exe */
    char dir[MAX_PATH], pat[MAX_PATH], newest[MAX_PATH] = "";
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    *strrchr(dir, '\\') = 0;
    snprintf(pat, sizeof(pat), "%s\\MetaphorCPU_meshes_*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    FILETIME best = {0};
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0)
                best = fd.ftLastWriteTime, snprintf(newest, sizeof(newest), "%s\\%s", dir, fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    char p[MAX_PATH];
    long len = 0;
    snprintf(p, sizeof(p), "%s\\census.txt", newest);
    char* txt = slurp(p, &len);
    EXPECT(txt != NULL, "census.txt written");
    if (txt) {
        printf("%.900s\n", txt);
        EXPECT(strstr(txt, "3 frames, 2 meshes") != NULL, "2 meshes filed in 3 frames");
        EXPECT(strstr(txt, "Triangles beyond 50 m: 0.2 k/frame, from 2 meshes; 50% of them in 1 meshes") != NULL,
               "162 far triangles per frame, all in one mesh");
        EXPECT(strstr(txt, "immutable 100%") && strstr(txt, "created with their data (index and vertex buffer) 100%"),
               "buffer kinds of the far triangles");
        EXPECT(strstr(txt, "position formats (DXGI): 6 = 100%") != NULL, "position format float3 (6)");
        EXPECT(strstr(txt, "   0    0.2     0.0     0.2    1.00      486    60-62") != NULL,
               "top mesh: 0.2 k far and 0.2 k shadow triangles, 1 draw/frame, 486 indices, 60-62 m");
        EXPECT(strstr(txt, "1 of the top 40 meshes written") != NULL, "the far mesh written as a file");
    }
    snprintf(p, sizeof(p), "%s\\mesh_000.bin", newest);
    char* bin = slurp(p, &len);
    if (bin) {
        MeshFileHeader* hd = (MeshFileHeader*)bin;
        uint32_t* ix = (uint32_t*)(bin + sizeof(MeshFileHeader));
        float* v = (float*)(ix + hd->index_count);
        EXPECT(!memcmp(hd->magic, "MCM1", 4) && hd->index_count == 486 && hd->vertex_count == 100 &&
                   hd->vertex_stride == 20 && hd->first_vertex == 50 && hd->index_bits == 16,
               "mesh file: 486 indices, the 100 vertices from vertex 50, stride 20");
        EXPECT(ix[0] == 0 && ix[1] == 10 && ix[5] == 11 && v[5 * 11 + 0] == 1.0f && v[5 * 11 + 2] == 1.0f,
               "indices rebased; vertex 11 is at (1, 0, 1)");
        EXPECT(len == (long)(sizeof(MeshFileHeader) + 486 * 4 + 100 * 20), "file size");
    } else {
        EXPECT(0, "mesh_000.bin written");
    }
    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
