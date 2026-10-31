/* Frame-stats test: loads MetaphorCPU.dll, creates a D3D11 device + swap chain on the
 * WARP software rasteriser (no GPU needed) and presents at a steady 30 fps, like a
 * capped game. The profiler must report ~30 fps and recognise the cap.
 * Usage: frame_test.exe <MetaphorCPU.dll> <ini> */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noinline)) static HRESULT create(HWND wnd, IDXGISwapChain** sc, ID3D11Device** dev,
                                                ID3D11DeviceContext** ctx)
{
    DXGI_SWAP_CHAIN_DESC d;
    ZeroMemory(&d, sizeof(d));
    d.BufferCount = 2;
    d.BufferDesc.Width = 320;
    d.BufferDesc.Height = 240;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = wnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; /* like the game: flip model, no DXGI blit */
    return D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, sc, dev,
                                         NULL, ctx);
}

/* a short-lived capability-check device, as many games create before the real one */
__attribute__((noinline)) static void throwaway_device(void)
{
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, NULL,
                                                NULL, &dev, NULL, &ctx))) {
        ID3D11DeviceContext_Release(ctx);
        ID3D11Device_Release(dev);
    }
}

/* "waits": a helper holds this lock most of the time, so the main thread's
 * EnterCriticalSection blocks */
static CRITICAL_SECTION g_cs;
static volatile LONG g_stop;
static DWORD WINAPI lock_holder(LPVOID arg)
{
    (void)arg;
    while (!g_stop) {
        EnterCriticalSection(&g_cs);
        Sleep(4);
        LeaveCriticalSection(&g_cs);
        Sleep(1);
    }
    return 0;
}

typedef struct VDraw {
    ID3D11DeviceContext* ctx;
    UINT start;
} VDraw;
static void verified_draw(void* arg)
{
    VDraw* v = (VDraw*)arg;
    ID3D11DeviceContext_DrawIndexed(v->ctx, 36, v->start, 0);
}
static void occ_draw(void* arg) /* occlusion mode: one triangle */
{
    VDraw* v = (VDraw*)arg;
    ID3D11DeviceContext_DrawIndexed(v->ctx, 3, v->start, 0);
}

/* the last-but-one line of the log starting with this text (the last window can be partial) */
static void line_before_last(const char* buf, const char* what, char* out, size_t cap)
{
    const char *last = NULL, *prev = NULL, *t;
    for (t = buf; (t = strstr(t, what)) != NULL; t++)
        prev = last, last = t;
    out[0] = 0;
    if (prev) {
        size_t n = strcspn(prev, "\r\n");
        if (n >= cap)
            n = cap - 1;
        memcpy(out, prev, n);
        out[n] = 0;
    }
}

/* "shaders" mode: a cheap and an expensive pixel shader, compiled before the DLL loads so the INI can name
 * the expensive one for the knock-out */
typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3D10Blob**, ID3D10Blob**);
static ID3D10Blob* compile_src(const char* src, const char* target)
{
    static D3DCompile_t compile;
    if (!compile) {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        compile = dc ? (D3DCompile_t)(void*)GetProcAddress(dc, "D3DCompile") : NULL;
    }
    ID3D10Blob *code = NULL, *err = NULL;
    if (!compile || FAILED(compile(src, strlen(src), "t", NULL, NULL, "main", target, 0, 0, &code, &err)))
        return NULL;
    return code;
}
static unsigned long long fnv64(const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static const char s_fs_vs[] = "float4 main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {"
                              " uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
/* "stabilise": a vertex shader with the outputs of the game's SMAA edge shader's inputs (the painterly shader reads the
 * first two), and the checkerboard of the image, flipped every frame */
static const char s_st_vs[] = "void main(uint id : SV_VertexID, out float4 p : SV_Position, out float2 uv : TEXCOORD0,"
                              " out float4 a : TEXCOORD1, out float4 b : TEXCOORD2, out float4 c : TEXCOORD3) {"
                              " uv = float2((id << 1) & 2, id & 2); p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);"
                              " a = uv.xyxy; b = uv.xyxy; c = uv.xyxy; }";
static const char s_st_chk[2][160] = {
    "float4 main(float4 p : SV_Position) : SV_Target { return ((uint(p.x) + uint(p.y)) & 1) ? 0.8 : 0.2; }",
    "float4 main(float4 p : SV_Position) : SV_Target { return ((uint(p.x) + uint(p.y)) & 1) ? 0.2 : 0.8; }"};
static float half_to_float(unsigned short h)
{
    int e = (h >> 10) & 31;
    unsigned u = e ? (((unsigned)(h & 0x8000) << 16) | ((unsigned)(e - 15 + 127) << 23) | ((unsigned)(h & 0x3ff) << 13)) : 0;
    float v;
    memcpy(&v, &u, 4);
    return v;
}
static unsigned char* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    static unsigned char b[2][65536];
    static int n;
    unsigned char* p = b[n++ & 1];
    *len = fread(p, 1, 65536, f);
    fclose(f);
    return p;
}
static const char s_cheap_ps[] = "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                                 " return float4(uv, 0.5, 1); }";
static const char s_heavy_ps[] = "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                                 " float a = uv.x; [loop] for (int i = 0; i < 48; i++) a = sin(a * 1.7 + uv.y) * cos(a + i);"
                                 " return float4(a, a, a, 1); }";

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: frame_test <MetaphorCPU.dll> <ini>\n");
        return 2;
    }
    int shaders = argc > 3 && strcmp(argv[3], "shaders") == 0;
    ID3D10Blob *s_vsb = NULL, *s_cb = NULL, *s_hb = NULL;
    unsigned long long cheap_h = 0, heavy_h = 0;
    char shaders_ini[MAX_PATH] = "";
    if (shaders) { /* the INI is written here: ShaderTiming, and the knock-out cycle of the expensive shader */
        s_vsb = compile_src(s_fs_vs, "vs_4_0"), s_cb = compile_src(s_cheap_ps, "ps_4_0"), s_hb = compile_src(s_heavy_ps, "ps_4_0");
        if (!s_vsb || !s_cb || !s_hb) {
            printf("[FAIL] shader compiler not available\n");
            return 1;
        }
        cheap_h = fnv64(ID3D10Blob_GetBufferPointer(s_cb), ID3D10Blob_GetBufferSize(s_cb));
        heavy_h = fnv64(ID3D10Blob_GetBufferPointer(s_hb), ID3D10Blob_GetBufferSize(s_hb));
        GetModuleFileNameA(NULL, shaders_ini, sizeof(shaders_ini));
        strcpy(strrchr(shaders_ini, '\\'), "\\frame_test_shaders.ini");
        FILE* f = fopen(shaders_ini, "w");
        if (!f)
            return 1;
        fprintf(f, "[General]\nEnabled=1\nLogLevel=1\n\n[Profiler]\nEnabled=1\nIntervalSec=1\nTopThreads=4\nFrameStats=1\n\n"
                   "[GpuProbe]\nShaderTiming=1\nKnockoutShaders=%016llx\n\n[ABTest]\nEnabled=1\nSwitchSec=2\n"
                   "Compare=knockout\n", heavy_h);
        fclose(f);
    }
    SetEnvironmentVariableA("METAPHORCPU_INI", shaders ? shaders_ini : argv[2]);
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        printf("[FAIL] cannot load %s\n", argv[1]);
        return 1;
    }

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "mcpu_frame_test";
    RegisterClassA(&wc);
    /* client area exactly the 320x240 back buffer (DPI-aware): DXGI then presents without
     * a stretch or copy of its own, which on Windows 11 runs through the app's context */
    SetProcessDPIAware();
    RECT wr = {0, 0, 320, 240};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowA("mcpu_frame_test", "frame test", WS_OVERLAPPEDWINDOW, 0, 0, wr.right - wr.left,
                             wr.bottom - wr.top, NULL, NULL, wc.hInstance, NULL);

    IDXGISwapChain* sc = NULL;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    /* "verify": probe + 5 draws of the third shadow pass wrapped in the shadow-cull
     * verification's occlusion queries (needs an INI with [ShadowCull] Verify=1) */
    int verify = argc > 3 && strcmp(argv[3], "verify") == 0;
    /* "timing": the probe frame with real rasterisation, measured by the GPU pass timer */
    int timing = argc > 3 && strcmp(argv[3], "timing") == 0;
    /* "passes": the probe frame plus four screen passes with known load / store behaviour for
     * the render pass audit (needs an INI with [GpuProbe] Passes=1) */
    int passes = argc > 3 && strcmp(argv[3], "passes") == 0;
    /* "quality": the probe frame plus 6 plain, 4 character (bone-weighted layout) and 2
     * outline (character layout, front faces culled) draws, and a mip-mapped sampler (needs
     * an INI with [Quality] options) */
    int quality = argc > 3 && strcmp(argv[3], "quality") == 0;
    /* "capture": a full-screen pass with a pixel shader writes a gradient into an RGBA16F target,
     * a second one reads it into the back buffer (needs an INI with [GpuProbe] Capture=1) */
    int capmode = argc > 3 && strcmp(argv[3], "capture") == 0;
    /* "occlusion": on a deferred context, a full-screen occluder, then three marked objects: one behind
     * it (hidden), one in front (shows), one alternating every 8 frames (needs an INI with [GpuProbe]
     * OcclusionCensus=1) */
    int occl = argc > 3 && strcmp(argv[3], "occlusion") == 0;
    /* "overall": an INI with [ABTest] Compare=overall: the game-as-is half runs first, yet the far meshes must be
     * set up when the device is created (the mod's half needs them) */
    int overall = argc > 3 && strcmp(argv[3], "overall") == 0;
    /* "features": an INI with [ABTest] Compare=features: the far meshes set up, the cycle held, then moving on */
    int features = argc > 3 && strcmp(argv[3], "features") == 0;
    if (argc > 3 && (strcmp(argv[3], "probe") == 0 || verify))
        throwaway_device();
    HRESULT hr = create(wnd, &sc, &dev, &ctx);
    if (FAILED(hr)) {
        printf("[FAIL] D3D11CreateDeviceAndSwapChain(WARP) 0x%08lx\n", hr);
        return 1;
    }

    /* "capped": present at a steady 30 fps ourselves, like the game's own cap.
     * "free":   present as fast as possible; MetaphorCPU's limiter must hold 30. */
    int free_run = argc > 3 && strcmp(argv[3], "free") == 0;
    /* "probe": each frame imitates a shadow-mapped frame for the GPU probe: three
     * 2048x2048 depth-only passes with 40 / 40 / 30 draws recorded on a DEFERRED context
     * and executed as a command list, then 10 draws to the screen on the immediate
     * context. A throwaway device was created first. */
    int probe = argc > 3 && (strcmp(argv[3], "probe") == 0 || verify || timing || passes || quality);
    typedef int (*VerifiedDraw_t)(void (*)(void*), void*);
    typedef void (*VerifyCounts_t)(LONG64*, LONG64*, LONG64*);
    VerifiedDraw_t vdraw = (VerifiedDraw_t)(void*)GetProcAddress(dll, "MetaphorCPU_TestVerifiedDraw");
    VerifyCounts_t vcounts = (VerifyCounts_t)(void*)GetProcAddress(dll, "MetaphorCPU_VerifyCounts");
    int queries = 0;
    ID3D11DeviceContext* dctx = NULL;
    if (probe && FAILED(ID3D11Device_CreateDeferredContext(dev, 0, &dctx))) {
        printf("[FAIL] CreateDeferredContext\n");
        return 1;
    }
    ID3D11DepthStencilView* dsv[3] = {0};
    ID3D11RenderTargetView* rtv = NULL;
    ID3D11Buffer *vb = NULL, *ib = NULL;
    if (probe) {
        for (int c = 0; c < 3; c++) {
            D3D11_TEXTURE2D_DESC td;
            ZeroMemory(&td, sizeof(td));
            td.Width = td.Height = 2048;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_D32_FLOAT;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ID3D11Texture2D* tex = NULL;
            ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)tex, NULL, &dsv[c]);
            ID3D11Texture2D_Release(tex);
        }
        ID3D11Texture2D* back = NULL;
        IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)back, NULL, &rtv);
        ID3D11Texture2D_Release(back);
        /* vertices 0-2: a triangle in the middle of the target at depth 0.5; everything else
         * degenerate. Only drawn with a vertex shader in verify mode. */
        static float verts[3 * 8] = {-0.5f, -0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
        static unsigned short idx[36 * 40] = {0, 1, 2};
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA sd;
        ZeroMemory(&bd, sizeof(bd));
        ZeroMemory(&sd, sizeof(sd));
        bd.ByteWidth = sizeof(verts);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = verts;
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &vb);
        bd.ByteWidth = sizeof(idx);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = idx;
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &ib);
    }
    /* verify mode: a real vertex shader, so the wrapped draw that covers the triangle is a
     * positive control (it must show samples) while the degenerate ones show none */
    ID3D11VertexShader* vs = NULL;
    ID3D11InputLayout* il = NULL;
    ID3D11RasterizerState* rs = NULL;
    if (verify || timing || occl) {
        typedef HRESULT(WINAPI * D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                               ID3D10Blob**, ID3D10Blob**);
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        D3DCompile_t compile = dc ? (D3DCompile_t)(void*)GetProcAddress(dc, "D3DCompile") : NULL;
        static const char src[] = "float4 main(float3 p : POSITION) : SV_Position { return float4(p, 1); }";
        ID3D10Blob *code = NULL, *err = NULL;
        if (compile && SUCCEEDED(compile(src, sizeof(src) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &code, &err))) {
            ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL,
                                            &vs);
            D3D11_INPUT_ELEMENT_DESC e = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            ID3D11Device_CreateInputLayout(dev, &e, 1, ID3D10Blob_GetBufferPointer(code),
                                           ID3D10Blob_GetBufferSize(code), &il);
            ID3D10Blob_Release(code);
        }
        D3D11_RASTERIZER_DESC rd;
        ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
        if (!vs || !il || !rs)
            printf("        (no shader compiler: positive control skipped)\n");
    }
    ID3D11InputLayout *q_plain = NULL, *q_char = NULL;
    ID3D11RasterizerState* q_front = NULL;
    ID3D11SamplerState* q_samp = NULL;
    float q_bias_seen = -1;
    int q_aniso_seen = -1;
    if (quality) {
        typedef HRESULT(WINAPI * D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                               ID3D10Blob**, ID3D10Blob**);
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        D3DCompile_t compile = dc ? (D3DCompile_t)(void*)GetProcAddress(dc, "D3DCompile") : NULL;
        static const char qsrc[] = "struct I { float3 p : POSITION; uint4 bi : BLENDINDICES; float4 bw : BLENDWEIGHT; };"
                                   "float4 main(I i) : SV_Position { return float4(i.p * i.bw.x + i.bi.x, 1); }";
        ID3D10Blob *code = NULL, *err = NULL;
        if (compile && SUCCEEDED(compile(qsrc, sizeof(qsrc) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &code, &err))) {
            D3D11_INPUT_ELEMENT_DESC e[3] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            ID3D11Device_CreateInputLayout(dev, e, 3, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                                           &q_char);
            ID3D10Blob_Release(code);
        }
        static const char psrc[] = "float4 main(float3 p : POSITION) : SV_Position { return float4(p, 1); }";
        if (compile && SUCCEEDED(compile(psrc, sizeof(psrc) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &code, &err))) {
            D3D11_INPUT_ELEMENT_DESC e = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            ID3D11Device_CreateInputLayout(dev, &e, 1, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                                           &q_plain);
            ID3D10Blob_Release(code);
        }
        D3D11_RASTERIZER_DESC rd;
        ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_FRONT;
        rd.DepthClipEnable = TRUE;
        ID3D11Device_CreateRasterizerState(dev, &rd, &q_front);
        D3D11_SAMPLER_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.Filter = D3D11_FILTER_ANISOTROPIC; /* like the game's material samplers */
        sd.MaxAnisotropy = 16;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(dev, &sd, &q_samp);
        if (!q_plain || !q_char || !q_front || !q_samp)
            printf("        (no shader compiler: quality objects missing)\n");
    }
    ID3D11VertexShader* c_vs = NULL;
    ID3D11PixelShader *c_ps1 = NULL, *c_ps2 = NULL;
    ID3D11RenderTargetView* c_rtv = NULL;
    ID3D11ShaderResourceView* c_srv = NULL;
    ID3D11DeviceContext* c_dctx = NULL; /* records the gradient pass one frame before it runs, like the game */
    ID3D11RenderTargetView* st_rtv[3] = {NULL, NULL, NULL}; /* CaptureDrawSteps: a 3-target pass with depth */
    ID3D11DepthStencilView* st_dsv = NULL;
    ID3D11DepthStencilState* st_dss = NULL;
    ID3D11CommandList* c_cl = NULL;
    if (capmode) {
        typedef HRESULT(WINAPI * D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                               ID3D10Blob**, ID3D10Blob**);
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        D3DCompile_t compile = dc ? (D3DCompile_t)(void*)GetProcAddress(dc, "D3DCompile") : NULL;
        static const char vsrc[] = "float4 main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {"
                                   " uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
        static const char p1src[] = "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                                    " return float4(uv.x * 4, uv.y, 0.25, 1); }";
        static const char p2src[] = "Texture2D t : register(t0); SamplerState s : register(s0);"
                                    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
                                    " float4 c = t.Sample(s, uv); return float4(c.rgb / (1 + c.rgb), 1); }";
        ID3D10Blob *code = NULL, *err = NULL;
        if (compile && SUCCEEDED(compile(vsrc, sizeof(vsrc) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &code, &err))) {
            ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &c_vs);
            ID3D10Blob_Release(code);
        }
        if (compile && SUCCEEDED(compile(p1src, sizeof(p1src) - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &code, &err))) {
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &c_ps1);
            ID3D10Blob_Release(code);
        }
        if (compile && SUCCEEDED(compile(p2src, sizeof(p2src) - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &code, &err))) {
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &c_ps2);
            ID3D10Blob_Release(code);
        }
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = 256, td.Height = 128, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* tex = NULL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) {
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)tex, NULL, &c_rtv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)tex, NULL, &c_srv);
            ID3D11Texture2D_Release(tex);
        }
        if (!rtv) { /* the back buffer, for the second pass */
            ID3D11Texture2D* back = NULL;
            IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back);
            if (back) {
                ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)back, NULL, &rtv);
                ID3D11Texture2D_Release(back);
            }
        }
        ID3D11Device_CreateDeferredContext(dev, 0, &c_dctx);
        if (!c_vs || !c_ps1 || !c_ps2 || !c_rtv || !c_srv || !c_dctx)
            printf("        (no shader compiler: capture objects missing)\n");
        /* CaptureDrawSteps: a pass like the characters' (3 targets, RGBA16F first, with depth), 512x512 */
        td.Width = td.Height = 512, td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 3; i++) {
            ID3D11Texture2D* t3 = NULL;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t3))) {
                ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)t3, NULL, &st_rtv[i]);
                ID3D11Texture2D_Release(t3);
            }
        }
        td.Format = DXGI_FORMAT_D32_FLOAT, td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* dt = NULL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &dt))) {
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)dt, NULL, &st_dsv);
            ID3D11Texture2D_Release(dt);
        }
        D3D11_DEPTH_STENCIL_DESC dsd;
        ZeroMemory(&dsd, sizeof(dsd)); /* depth off: every draw lands */
        ID3D11Device_CreateDepthStencilState(dev, &dsd, &st_dss);
    }
    typedef void (*OccNode_t)(void (*)(void*), void*, unsigned long long, float, LONG64);
    OccNode_t occ_node = (OccNode_t)(void*)GetProcAddress(dll, "MetaphorCPU_TestOcclusionNode");
    ID3D11DeviceContext* o_dctx = NULL;
    ID3D11DepthStencilView* o_dsv = NULL;
    ID3D11Buffer *o_vb = NULL, *o_ib = NULL;
    if (occl) {
        /* triangle 0: full screen at depth 0.2 (the occluder); 1: small at 0.8 (behind it); 2: the same in front;
         * 3: nearer still (in front of 2, which is drawn before it at the same place) */
        static const float ov[36] = {-1, -1, 0.2f, -1, 3, 0.2f, 3, -1, 0.2f,  -0.5f, -0.5f, 0.8f, 0, 0.5f, 0.8f,
                                     0.5f, -0.5f, 0.8f, -0.5f, -0.5f, 0.1f, 0, 0.5f, 0.1f, 0.5f, -0.5f, 0.1f,
                                     -0.5f, -0.5f, 0.05f, 0, 0.5f, 0.05f, 0.5f, -0.5f, 0.05f};
        static const unsigned short oi[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA sd;
        ZeroMemory(&bd, sizeof(bd));
        ZeroMemory(&sd, sizeof(sd));
        bd.ByteWidth = sizeof(ov), bd.BindFlags = D3D11_BIND_VERTEX_BUFFER, sd.pSysMem = ov;
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &o_vb);
        bd.ByteWidth = sizeof(oi), bd.BindFlags = D3D11_BIND_INDEX_BUFFER, sd.pSysMem = oi;
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &o_ib);
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = 320, td.Height = 240, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D* tex = NULL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) {
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)tex, NULL, &o_dsv);
            ID3D11Texture2D_Release(tex);
        }
        ID3D11Device_CreateDeferredContext(dev, 0, &o_dctx);
        if (!occ_node || !vs || !il || !rs || !o_vb || !o_ib || !o_dsv || !o_dctx)
            printf("        (occlusion objects missing: export %p, shader %p)\n", (void*)occ_node, (void*)vs);
    }
    ID3D11VertexShader* s_vs = NULL;
    ID3D11PixelShader *s_cheap = NULL, *s_heavy = NULL;
    if (shaders) {
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(s_vsb), ID3D10Blob_GetBufferSize(s_vsb), NULL, &s_vs);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(s_cb), ID3D10Blob_GetBufferSize(s_cb), NULL, &s_cheap);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(s_hb), ID3D10Blob_GetBufferSize(s_hb), NULL, &s_heavy);
        if (!rtv) {
            ID3D11Texture2D* back = NULL;
            IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back);
            if (back) {
                ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)back, NULL, &rtv);
                ID3D11Texture2D_Release(back);
            }
        }
    }
    /* "stabilise": [Quality] TemporalStabilise end to end, recorded on a deferred context as the game does: a checkerboard
     * that flips every frame into an RGBA16F image whose left half is marked as a character (gbuffer1's alpha 4), the
     * game's painterly shader drawn with the flags and a D24 depth bound (t3, t2), then the game's SMAA edge shader
     * reading the image (t0). The INI's folder holds the game's shaders (data\). */
    int stab = argc > 3 && strcmp(argv[3], "stabilise") == 0;
    ID3D11DeviceContext* st_dc = NULL;
    ID3D11VertexShader* st_vs = NULL;
    ID3D11PixelShader *st_paint = NULL, *st_edge = NULL, *st_chk[2] = {NULL, NULL};
    ID3D11Texture2D *st_col = NULL, *st_rb = NULL;
    ID3D11RenderTargetView *st_colv = NULL, *st_edgev = NULL, *st_paintv = NULL;
    ID3D11ShaderResourceView *st_cols = NULL, *st_flags = NULL, *st_depth = NULL;
    ID3D11DepthStencilView* sb_dsv = NULL;
    ID3D11Buffer* st_sys = NULL; /* the painterly pass's view constants (GFD_PSCONST_SYSTEM): a still camera */
    float st_l[2] = {-1, -1}, st_r[2] = {-1, -1}; /* a left (character) and a right pixel, the last two frames */
    if (stab) {
        char dir[MAX_PATH], path[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s", argv[2]);
        char* sl = strrchr(dir, '\\');
        if (sl)
            *sl = 0;
        size_t lp = 0, le = 0;
        snprintf(path, sizeof(path), "%s\\data\\ps_b3ebc8024b526721.dxbc", dir);
        unsigned char* pc = read_file(path, &lp);
        snprintf(path, sizeof(path), "%s\\data\\ps_5b1af0481406dae1.dxbc", dir);
        unsigned char* ec = read_file(path, &le);
        ID3D10Blob* vb = compile_src(s_st_vs, "vs_5_0");
        if (pc && ec && vb) {
            ID3D11Device_CreatePixelShader(dev, pc, lp, NULL, &st_paint);
            ID3D11Device_CreatePixelShader(dev, ec, le, NULL, &st_edge);
            ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &st_vs);
        }
        for (int k = 0; k < 2; k++) {
            ID3D10Blob* b = compile_src(s_st_chk[k], "ps_5_0");
            if (b)
                ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &st_chk[k]);
        }
        D3D11_TEXTURE2D_DESC td = {320, 240, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st_col);
        ID3D11Texture2D *edges = NULL, *paint = NULL, *fl = NULL, *dt = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &paint);
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &edges);
        static unsigned char fdata[320 * 240 * 4];
        for (int p = 0; p < 320 * 240; p++)
            fdata[p * 4 + 3] = (p % 320) < 160 ? 4 : 0;
        D3D11_SUBRESOURCE_DATA fi = {fdata, 320 * 4, 0};
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(dev, &td, &fi, &fl);
        td.Format = DXGI_FORMAT_R24G8_TYPELESS, td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &dt);
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st_rb);
        if (st_col && edges && paint && fl && dt) {
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)st_col, NULL, &st_colv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)st_col, NULL, &st_cols);
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)edges, NULL, &st_edgev);
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)paint, NULL, &st_paintv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)fl, NULL, &st_flags);
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
            ZeroMemory(&dvd, sizeof(dvd));
            dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT, dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)dt, &dvd, &sb_dsv);
            D3D11_SHADER_RESOURCE_VIEW_DESC svd;
            ZeroMemory(&svd, sizeof(svd));
            svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS, svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MipLevels = 1;
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)dt, &svd, &st_depth);
        }
        static float sysk[72];
        for (int r = 0; r < 4; r++)
            sysk[(1 + r) * 4 + r] = 1, sysk[(5 + r) * 4 + r] = 1;
        sysk[68] = 1, sysk[69] = 1, sysk[70] = 0, sysk[71] = -2;
        D3D11_BUFFER_DESC sbd = {288, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA ssd = {sysk, 0, 0};
        ID3D11Device_CreateBuffer(dev, &sbd, &ssd, &st_sys);
        ID3D11Device_CreateDeferredContext(dev, 0, &st_dc);
        if (!st_paint || !st_edge || !st_vs || !st_chk[0] || !st_chk[1] || !st_colv || !st_cols || !st_edgev ||
            !st_paintv || !st_flags || !sb_dsv || !st_depth || !st_rb || !st_dc || !st_sys)
            printf("        (stabilise objects missing: the game's shaders read %d / %d)\n", pc != NULL, ec != NULL);
    }
    /* "compute": [GpuProbe] ComputeCensus, a compute shader writing a vertex buffer (as the game's skinning does), 4
     * thread groups a frame; the log must name it by its hash and resources and say what it writes. Its own timing row
     * cannot be checked here: WARP's timestamps do not bracket compute work (checked on the tablet); a compute row, if
     * any, must carry the shader's name. */
    int compute = argc > 3 && strcmp(argv[3], "compute") == 0;
    ID3D11ComputeShader* cm_cs = NULL;
    ID3D11UnorderedAccessView* cm_uav = NULL;
    unsigned long long cm_hash = 0;
    if (compute) {
        static const char csrc[] = "RWByteAddressBuffer rw_skinnedVertices : register(u0);"
                                   " [numthreads(64, 1, 1)] void main(uint3 t : SV_DispatchThreadID) {"
                                   " rw_skinnedVertices.Store(t.x * 4, t.x); }";
        ID3D10Blob* cb = compile_src(csrc, "cs_5_0");
        if (cb) {
            cm_hash = fnv64(ID3D10Blob_GetBufferPointer(cb), ID3D10Blob_GetBufferSize(cb));
            ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb), ID3D10Blob_GetBufferSize(cb), NULL,
                                             &cm_cs);
        }
        D3D11_BUFFER_DESC bd = {1024, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, 0,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        ID3D11Buffer* vb = NULL;
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb);
        if (vb) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
            ZeroMemory(&ud, sizeof(ud));
            ud.Format = DXGI_FORMAT_R32_TYPELESS, ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 256, ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource*)vb, &ud, &cm_uav);
            ID3D11Buffer_Release(vb);
        }
        if (!cm_cs || !cm_uav)
            printf("        (compute objects missing)\n");
    }
    /* "waits": Sleep(2), a 3 ms event wait and a contended critical section every frame */
    int waits = argc > 3 && strcmp(argv[3], "waits") == 0;
    HANDLE never = CreateEventA(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&g_cs);
    ID3D11Buffer *gpu_buf = NULL, *staging = NULL;
    ID3D11RenderTargetView* prt[3] = {0};
    ID3D11DepthStencilView* pds = NULL;
    ID3D11DepthStencilState* ds_off = NULL;
    ID3D11DeviceContext1* ctx1 = NULL;
    if (passes) {
        for (int i = 0; i < 4; i++) {
            D3D11_TEXTURE2D_DESC td;
            ZeroMemory(&td, sizeof(td));
            td.Width = 320, td.Height = 240;
            td.MipLevels = td.ArraySize = 1;
            td.Format = i < 3 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_D32_FLOAT;
            td.SampleDesc.Count = 1;
            td.BindFlags = i < 3 ? D3D11_BIND_RENDER_TARGET : D3D11_BIND_DEPTH_STENCIL;
            ID3D11Texture2D* tex = NULL;
            ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
            if (i < 3)
                ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)tex, NULL, &prt[i]);
            else
                ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource*)tex, NULL, &pds);
            ID3D11Texture2D_Release(tex);
        }
        D3D11_DEPTH_STENCIL_DESC dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.DepthEnable = FALSE;
        ID3D11Device_CreateDepthStencilState(dev, &dd, &ds_off);
        ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void**)&ctx1);
    }
    if (waits) {
        timeBeginPeriod(1);
        CreateThread(NULL, 0, lock_holder, NULL, 0, NULL);
    }
    if (waits || passes) {
        /* plus a GPU -> CPU readback every frame (CopyResource + Map READ) for the D3D timing */
        static UINT init[1024];
        D3D11_BUFFER_DESC bd;
        D3D11_SUBRESOURCE_DATA sd;
        ZeroMemory(&bd, sizeof(bd));
        ZeroMemory(&sd, sizeof(sd));
        bd.ByteWidth = sizeof(init);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = init;
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &gpu_buf);
        bd.BindFlags = 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &staging);
    }
    LARGE_INTEGER f, now, next;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&next);
    LONGLONG step = f.QuadPart / 30;
    int nframes = shaders ? 240 : 150; /* shaders: the knock-out cycle needs a few 2 s turns */
    for (int i = 0; i < nframes; i++) {
        if (probe) {
            UINT stride = 12, off = 0;
            if (vs && il && rs) {
                D3D11_VIEWPORT vp = {0, 0, 2048, 2048, 0, 1};
                ID3D11DeviceContext_IASetInputLayout(dctx, il);
                ID3D11DeviceContext_IASetPrimitiveTopology(dctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11DeviceContext_VSSetShader(dctx, vs, NULL, 0);
                ID3D11DeviceContext_RSSetState(dctx, rs);
                ID3D11DeviceContext_RSSetViewports(dctx, 1, &vp);
            }
            for (int c = 0; c < 3; c++) {
                ID3D11DeviceContext_OMSetRenderTargets(dctx, 0, NULL, dsv[c]);
                if (vs || passes)
                    ID3D11DeviceContext_ClearDepthStencilView(dctx, dsv[c], D3D11_CLEAR_DEPTH, 1.0f, 0);
                ID3D11DeviceContext_IASetIndexBuffer(dctx, ib, DXGI_FORMAT_R16_UINT, 0);
                ID3D11DeviceContext_IASetVertexBuffers(dctx, 0, 1, &vb, &stride, &off);
                for (int d = 0; d < (c == 2 ? 30 : 40); d++) {
                    if (verify && vdraw && c == 2 && d < 5) {
                        VDraw v = {dctx, (UINT)d * 36};
                        queries += vdraw(verified_draw, &v) >= 0;
                    } else {
                        ID3D11DeviceContext_DrawIndexed(dctx, 36, d * 36, 0);
                    }
                }
            }
            ID3D11CommandList* cl = NULL;
            ID3D11DeviceContext_FinishCommandList(dctx, FALSE, &cl);
            ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE);
            ID3D11CommandList_Release(cl);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            for (int d = 0; d < 10; d++) {
                if (passes && d == 5 && gpu_buf && staging) /* a copy in the middle: the render pass splits */
                    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)gpu_buf);
                ID3D11DeviceContext_DrawIndexed(ctx, 36, d * 36, 0);
            }
        }
        if (passes && prt[2] && pds && ds_off) {
            /* a full-screen triangle without blending over the whole target: its load is avoidable */
            D3D11_VIEWPORT vp = {0, 0, 320, 240, 0, 1};
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &prt[0], NULL);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            /* discarded before it is bound: no load */
            if (ctx1)
                ID3D11DeviceContext1_DiscardView(ctx1, (ID3D11View*)prt[1]);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &prt[1], NULL);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            /* cleared target (no load) with a depth buffer that no draw uses (loaded and stored for nothing) */
            static const float black[4] = {0, 0, 0, 0};
            ID3D11DeviceContext_ClearRenderTargetView(ctx, prt[2], black);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &prt[2], pds);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
            ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
            ID3D11DeviceContext_DrawIndexed(ctx, 36, 36, 0);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
        }
        if (quality && q_plain && q_char && q_front && q_samp) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_IASetInputLayout(ctx, q_plain);
            for (int d = 0; d < 6; d++)
                ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
            ID3D11DeviceContext_IASetInputLayout(ctx, q_char);
            for (int d = 0; d < 4; d++)
                ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
            ID3D11DeviceContext_RSSetState(ctx, q_front);
            for (int d = 0; d < 2; d++)
                ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
            ID3D11DeviceContext_RSSetState(ctx, NULL);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &q_samp);
            ID3D11SamplerState* bound = NULL;
            ID3D11DeviceContext_PSGetSamplers(ctx, 0, 1, &bound);
            if (bound) {
                D3D11_SAMPLER_DESC bd;
                ID3D11SamplerState_GetDesc(bound, &bd);
                q_bias_seen = bd.MipLODBias;
                q_aniso_seen = (int)bd.MaxAnisotropy;
                ID3D11SamplerState_Release(bound);
            }
        }
        if (capmode && c_vs && c_ps1 && c_ps2 && c_rtv && c_srv && c_dctx) {
            if (c_cl) { /* last frame's list: the gradient pass */
                ID3D11DeviceContext_ExecuteCommandList(ctx, c_cl, FALSE);
                ID3D11CommandList_Release(c_cl);
                c_cl = NULL;
            }
            D3D11_VIEWPORT vp1 = {0, 0, 256, 128, 0, 1};
            ID3D11DeviceContext_OMSetRenderTargets(c_dctx, 1, &c_rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(c_dctx, 1, &vp1);
            ID3D11DeviceContext_IASetInputLayout(c_dctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(c_dctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(c_dctx, c_vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(c_dctx, c_ps1, NULL, 0);
            ID3D11DeviceContext_Draw(c_dctx, 3, 0);
            ID3D11DeviceContext_FinishCommandList(c_dctx, FALSE, &c_cl); /* runs next frame */
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, c_vs, NULL, 0);
            D3D11_VIEWPORT vp2 = {0, 0, 320, 240, 0, 1};
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp2);
            ID3D11DeviceContext_PSSetShader(ctx, c_ps2, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &c_srv);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11ShaderResourceView* none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
            if (st_rtv[0] && st_rtv[1] && st_rtv[2] && st_dsv && st_dss) {
                /* the stepped pass: the whole target, then a 64x64 square in the middle, then one in the corner (outside
                 * the middle 320x320: not listed) */
                static const D3D11_VIEWPORT sv[3] = {{0, 0, 512, 512, 0, 1}, {200, 200, 64, 64, 0, 1}, {0, 0, 64, 64, 0, 1}};
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 3, st_rtv, st_dsv);
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, st_dss, 0);
                ID3D11DeviceContext_PSSetShader(ctx, c_ps1, NULL, 0);
                for (int d = 0; d < 3; d++) {
                    ID3D11DeviceContext_RSSetViewports(ctx, 1, &sv[d]);
                    ID3D11DeviceContext_Draw(ctx, 3, 0);
                }
                ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            }
        }
        if (shaders && s_vs && s_cheap && s_heavy && rtv) { /* one pass: the cheap shader 3 times, then the expensive one */
            D3D11_VIEWPORT vp = {0, 0, 320, 240, 0, 1};
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, s_cheap, NULL, 0);
            for (int d = 0; d < 3; d++)
                ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11DeviceContext_PSSetShader(ctx, s_heavy, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        if (occl && occ_node && vs && il && rs && o_vb && o_ib && o_dsv && o_dctx) {
            UINT stride = 12, off = 0;
            D3D11_VIEWPORT vp = {0, 0, 320, 240, 0, 1};
            ID3D11DeviceContext_IASetInputLayout(o_dctx, il);
            ID3D11DeviceContext_IASetPrimitiveTopology(o_dctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(o_dctx, vs, NULL, 0);
            ID3D11DeviceContext_RSSetState(o_dctx, rs);
            ID3D11DeviceContext_RSSetViewports(o_dctx, 1, &vp);
            ID3D11DeviceContext_OMSetRenderTargets(o_dctx, 0, NULL, o_dsv);
            ID3D11DeviceContext_ClearDepthStencilView(o_dctx, o_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            ID3D11DeviceContext_IASetIndexBuffer(o_dctx, o_ib, DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_IASetVertexBuffers(o_dctx, 0, 1, &o_vb, &stride, &off);
            ID3D11DeviceContext_DrawIndexed(o_dctx, 3, 0, 0); /* the occluder: not a scene object */
            VDraw behind = {o_dctx, 3}, front = {o_dctx, 6}, alt = {o_dctx, (i / 8) % 2 ? 9u : 3u};
            occ_node(occ_draw, &behind, 1, 20.0f, i + 1); /* 15-30 m band: always hidden */
            occ_node(occ_draw, &front, 2, 5.0f, i + 1);   /* < 15 m: always shows */
            occ_node(occ_draw, &alt, 3, 60.0f, i + 1);    /* 50-100 m: hidden 8 frames, then shows 8 */
            ID3D11CommandList* cl = NULL;
            ID3D11DeviceContext_FinishCommandList(o_dctx, FALSE, &cl);
            ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE);
            ID3D11CommandList_Release(cl);
        }
        if (waits) { /* known waits on the main thread for the wait profile */
            Sleep(2);
            WaitForSingleObject(never, 3);
            EnterCriticalSection(&g_cs);
            LeaveCriticalSection(&g_cs);
            if (gpu_buf && staging) {
                D3D11_MAPPED_SUBRESOURCE ms;
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)staging, (ID3D11Resource*)gpu_buf);
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &ms)))
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)staging, 0);
            }
        }
        if (compute && cm_cs && cm_uav) {
            ID3D11DeviceContext_CSSetShader(ctx, cm_cs, NULL, 0);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &cm_uav, NULL);
            /* a hooked call between, as a game makes them: the Windows runtime rewrites context entries when state
             * changes, and the mod puts its hooks back at its next hooked call (DXVK never rewrites them) */
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
            ID3D11DeviceContext_Dispatch(ctx, 4, 1, 1);
            ID3D11UnorderedAccessView* nul = NULL;
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nul, NULL);
        }
        if (stab && st_dc && st_vs && st_paint && st_edge && st_chk[0] && st_chk[1] && st_colv && st_cols && st_edgev &&
            st_paintv && st_flags && sb_dsv && st_depth && st_rb && st_sys) {
            ID3D11DeviceContext* d = st_dc;
            D3D11_VIEWPORT vp = {0, 0, 320, 240, 0, 1};
            ID3D11ShaderResourceView* nul[2] = {NULL, NULL};
            ID3D11DeviceContext_RSSetViewports(d, 1, &vp);
            ID3D11DeviceContext_IASetInputLayout(d, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(d, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(d, st_vs, NULL, 0);
            ID3D11DeviceContext_OMSetRenderTargets(d, 1, &st_colv, NULL); /* the image */
            ID3D11DeviceContext_PSSetShader(d, st_chk[i & 1], NULL, 0);
            ID3D11DeviceContext_Draw(d, 3, 0);
            ID3D11DeviceContext_ClearDepthStencilView(d, sb_dsv, D3D11_CLEAR_DEPTH, 0.9f, 0);
            ID3D11ShaderResourceView* pin[2] = {st_depth, st_flags}; /* the painterly pass */
            ID3D11DeviceContext_OMSetRenderTargets(d, 1, &st_paintv, NULL);
            ID3D11DeviceContext_PSSetShaderResources(d, 2, 2, pin);
            ID3D11DeviceContext_PSSetConstantBuffers(d, 0, 1, &st_sys);
            ID3D11DeviceContext_PSSetShader(d, st_paint, NULL, 0);
            ID3D11DeviceContext_Draw(d, 3, 0);
            ID3D11DeviceContext_PSSetShaderResources(d, 2, 2, nul);
            ID3D11DeviceContext_OMSetRenderTargets(d, 1, &st_edgev, NULL); /* the SMAA edges */
            ID3D11DeviceContext_PSSetShaderResources(d, 0, 1, &st_cols);
            ID3D11DeviceContext_PSSetShader(d, st_edge, NULL, 0);
            ID3D11DeviceContext_Draw(d, 3, 0);
            ID3D11DeviceContext_PSSetShaderResources(d, 0, 1, nul);
            ID3D11CommandList* cl = NULL;
            ID3D11DeviceContext_FinishCommandList(d, FALSE, &cl);
            if (cl) {
                ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE);
                ID3D11CommandList_Release(cl);
            }
            if (i >= nframes - 2) {
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource*)st_rb, (ID3D11Resource*)st_col);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)st_rb, 0, D3D11_MAP_READ, 0, &m))) {
                    const unsigned char* row = (const unsigned char*)m.pData + 120 * (size_t)m.RowPitch;
                    st_l[i & 1] = half_to_float(*(const unsigned short*)(row + 41 * 8));
                    st_r[i & 1] = half_to_float(*(const unsigned short*)(row + 281 * 8));
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)st_rb, 0);
                }
            }
        }
        IDXGISwapChain_Present(sc, 0, 0);
        if (free_run)
            continue;
        next.QuadPart += step;
        for (;;) {
            QueryPerformanceCounter(&now);
            LONGLONG left = next.QuadPart - now.QuadPart;
            if (left <= 0)
                break;
            if (left > f.QuadPart / 250)
                Sleep(1);
        }
    }
    g_stop = 1;
    Sleep(2200); /* let a profiler window close */

    char logp[MAX_PATH];
    GetModuleFileNameA(dll, logp, MAX_PATH);
    strcpy(strrchr(logp, '.'), ".log");
    HANDLE lf = CreateFileA(logp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    static char buf[1 << 17];
    DWORD got = 0;
    if (lf != INVALID_HANDLE_VALUE) {
        ReadFile(lf, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(lf);
    }
    buf[got] = 0;
    int fail = 0;
#define EXPECT(c, what)                                                                                       \
    do {                                                                                                      \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                   \
        if (!(c))                                                                                             \
            fail++;                                                                                           \
    } while (0)
    EXPECT(strstr(buf, "hooked IDXGISwapChain::Present") != NULL, "Present hooked via D3D11CreateDeviceAndSwapChain");
    const char* fr = strstr(buf, "frames: ");
    double fps = fr ? atof(fr + 8) : 0;
    printf("        first frame report: %.1f fps\n", fps);
    if (!probe && !overall && !features && !shaders) /* overall: the game-as-is half gives the coarse timer back */
        EXPECT(fps > 27 && fps < 33, "frame rate measured at ~30 fps");
    if (waits) {
        const char* w = strstr(buf, "main thread per frame (");
        double sleeps = 0, events = 0, cs = 0, ecalls = 0;
        if (w) {
            const char* p;
            if ((p = strstr(w, "| sleeps ")))
                sleeps = atof(p + 9);
            if ((p = strstr(w, "| event waits "))) {
                events = atof(p + 14);
                const char* q = strchr(p + 14, '(');
                ecalls = q ? atof(q + 1) : 0;
            }
            if ((p = strstr(w, "| critical sections ")))
                cs = atof(p + 20);
        }
        printf("        main thread per frame: sleeps %.1f ms, event waits %.1f ms (%.1f calls), critical sections "
               "%.1f ms\n", sleeps, events, ecalls, cs);
        EXPECT(w != NULL, "wait profile reports the main thread per frame");
        EXPECT(events >= 2.5 && events < 6 && ecalls > 0.9 && ecalls < 1.1, "3 ms event wait measured once per frame");
        EXPECT(sleeps >= 2.0, "Sleep(2) per frame measured (plus the pacing sleeps)");
        EXPECT(cs >= 0.2, "contended critical section measured");
        EXPECT(strstr(buf, "main thread event waits at: frame_test.exe+") != NULL, "event wait traced to its call site");
        const char* d3 = strstr(buf, "main thread inside D3D calls per frame: ");
        EXPECT(d3 && strstr(d3, "Map(read)") && strstr(d3, "(1.0x)") && strstr(d3, "Copy"),
               "D3D calls on the main thread timed (one Copy + Map(read) per frame)");
        EXPECT(strstr(buf, "main thread D3D calls at: frame_test.exe+") != NULL, "D3D call traced to its call site");
    }
    if (timing) {
        const char* t = strstr(buf, "GPU time per pass (");
        const char* sm = strstr(buf, "2048x2048 depth only (D32) = shadow map");
        const char* scr = strstr(buf, "320x240 1x RGBA8");
        double share = 0, per = 0;
        if (sm) { /* the line reads: <ms> ms/frame <share>%  <n>x/frame  <label> */
            const char* ls = sm;
            while (ls > buf && ls[-1] != '\n')
                ls--;
            const char* q = strstr(ls, "ms/frame");
            if (q && q < sm) {
                share = atof(q + 8);
                const char* x = strchr(q + 8, '%');
                per = x ? atof(x + 1) : 0;
            }
        }
        printf("        shadow passes: %.0f%% of the measured GPU time, %.1f per frame\n", share, per);
        EXPECT(t != NULL && strstr(t, "no data") != t + 19, "GPU timestamps resolved and attributed per pass");
        EXPECT(sm != NULL && per > 2.7 && per < 3.3, "the three 2048x2048 depth passes found, 3 per frame");
        (void)scr; /* the tiny screen pass rounds to 0 ms on WARP */
        double busy = 0, idle = 0;
        if (t) {
            const char* b = strstr(t, "busy ");
            const char* d = strstr(t, "in passes, ");
            busy = b ? atof(b + 5) : 0, idle = d ? atof(d + 11) : 0;
        }
        printf("        busy %.1f ms/frame in passes, idle %.1f ms/frame\n", busy, idle);
        EXPECT(share > 90, "the depth passes with the big triangle carry the GPU time, the tiny screen pass ~none");
        EXPECT(idle > busy && busy + idle > 25 && busy + idle < 40,
               "idle time of the 30 fps pacing kept out of the passes (busy + idle = frame time)");
    } else if (capmode) {
        /* the newest capture folder in the output folder next to this exe */
        char dir[MAX_PATH], pat[MAX_PATH], best[MAX_PATH] = "";
        GetModuleFileNameA(NULL, dir, sizeof(dir));
        *strrchr(dir, '\\') = 0;
        strcat(dir, "\\MetaphorCPU_compare folder");
        snprintf(pat, sizeof(pat), "%s\\MetaphorCPU_capture_*", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (strcmp(fd.cFileName, best) > 0)
                    snprintf(best, sizeof(best), "%s", fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        static char ix[65536];
        ix[0] = 0;
        int nps = 0;
        if (best[0]) {
            snprintf(pat, sizeof(pat), "%s\\%s\\capture.txt", dir, best);
            FILE* f = fopen(pat, "rb");
            if (f) {
                size_t r = fread(ix, 1, sizeof(ix) - 1, f);
                ix[r] = 0;
                fclose(f);
            }
            snprintf(pat, sizeof(pat), "%s\\%s\\ps_*.dxbc", dir, best);
            h = FindFirstFileA(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do
                    nps++;
                while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        printf("        capture folder %s, %d pixel shaders\n", best[0] ? best : "(none)", nps);
        EXPECT(strstr(ix, "p000 deferred") && strstr(ix, "of them not recorded 0"),
               "the list recorded one frame before it ran is in the written frame (no list missing)");
        EXPECT(strstr(ix, "256x128 RGBA16F -> p") != NULL, "the gradient pass's RGBA16F target written as an image");
        EXPECT(strstr(ix, "R 0.00781..3.99") != NULL, "its red channel holds the gradient 0..4");
        EXPECT(strstr(ix, "in t0: R") && strstr(ix, "(written by p"), "the second pass's input traced to the first pass");
        EXPECT(strstr(ix, "3 vertices, full-screen") != NULL, "full-screen draws recognised");
        EXPECT(nps >= 2, "both pixel shaders written");
        const char* fin = strstr(ix, "final.png");
        EXPECT(fin && strstr(fin, "R 0..0 ") == NULL, "the back buffer written, with the second pass's picture");
        /* CaptureDrawSteps: the middle 320x320 of the 3-target pass after each draw; the corner draw changed nothing */
        const char* stp = strstr(ix, "steps: the middle 320x320 at (96,96)");
        const char* d1 = stp ? strstr(stp, "draw   1: PS ") : NULL;
        printf("        steps: %s; draw 1 %s; draw 2 %s\n", stp ? "listed" : "missing", d1 ? "listed" : "missing",
               stp && strstr(stp, "draw   2: PS ") ? "listed" : "not listed");
        EXPECT(stp && strstr(stp, "draw   0: PS ") && d1 && strstr(d1, "in (104,104)-(167,167)") &&
                   !strstr(stp, "draw   2: PS "),
               "draw steps: the draws that changed the middle listed with where, the one outside it not");
    } else if (compute) {
        char line[64], row[48];
        snprintf(line, sizeof(line), "CS %016llx: 1.0 dispatches, 4 thread groups a frame", cm_hash);
        snprintf(row, sizeof(row), "compute CS %016llx", cm_hash);
        const char* l = strstr(buf, line);
        printf("        compute census: %s | timing row %s\n", l ? "listed" : "missing",
               strstr(buf, row) ? "named" : strstr(buf, "compute (Dispatch)") ? "UNNAMED" : "none (WARP)");
        EXPECT(l && strstr(l, "a vertex buffer: skinning output") && strstr(l, "rw_skinnedVertices:rwraw@0"),
               "compute census: the shader named by its hash and resources, its dispatches, groups and what it writes");
        EXPECT(!strstr(buf, "compute (Dispatch)"), "compute census: no compute timing row without its shader's name");
    } else if (stab) {
        /* pixel (41, 120): 0.8 on even frames (x + y odd), 0.2 on odd ones; the last frame is odd */
        const char* made = strstr(buf, "Temporal stabilisation: textures for the 320x240 image made");
        static const char k_rep[] = "Temporal stabilisation: on (history: characters 40%, the rest 0%): ";
        const char* rep = strstr(buf, k_rep);
        int passes = rep ? atoi(rep + strlen(k_rep)) : -1;
        printf("        stabilise: textures %s, passes in a report window %d; the last two frames: character pixel "
               "%.3f / %.3f (drawn 0.8 / 0.2), other pixel %.3f / %.3f\n", made ? "made" : "NOT made", passes, st_l[0],
               st_l[1], st_r[0], st_r[1]);
        EXPECT(strstr(buf, "painterly wobble shader found") && strstr(buf, "SMAA edge shader High variant: 1 constants patched, created"),
               "stabilise: the game's painterly and SMAA edge shaders recognised");
        EXPECT(made && passes >= 20, "stabilise: its pass runs every frame before the SMAA edges (deferred context)");
        EXPECT(fabsf(st_r[0] - 0.8f) < 0.002f && fabsf(st_r[1] - 0.2f) < 0.002f && st_l[0] > 0.3f && st_l[0] < 0.7f && st_l[1] > 0.3f &&
                   st_l[1] < 0.7f && st_l[0] - st_l[1] > 0.05f && st_l[0] - st_l[1] < 0.4f,
               "stabilise: the flickering checkerboard steadied where the flags mark a character, exact elsewhere");
    } else if (shaders) {
        char ch[32], hh[32], tag[40];
        snprintf(ch, sizeof(ch), "PS %016llx (instr", cheap_h);
        snprintf(hh, sizeof(hh), "PS %016llx (instr", heavy_h);
        snprintf(tag, sizeof(tag), "[Q1 no-%08llx]", heavy_h >> 32);
        char fp[40];
        snprintf(fp, sizeof(fp), "first PS %016llx", cheap_h);
        EXPECT(strstr(buf, fp) != NULL, "the pass named after its first draw's pixel shader");
        /* the base window: both shaders timed on their own, the expensive one costlier; draws per frame 3 / 1 */
        const char* b0 = strstr(buf, "[Q0 base]");
        const char* b1 = b0 ? strstr(b0 + 1, "---- CPU report") : NULL;
        const char *lc = b0 ? strstr(b0, ch) : NULL, *lh = b0 ? strstr(b0, hh) : NULL;
        if (b1 && lc > b1)
            lc = NULL;
        if (b1 && lh > b1)
            lh = NULL;
        double mc = -1, mh = -1, dc = -1, dh = -1;
        for (int k = 0; k < 2; k++) {
            const char* l = k ? lh : lc;
            if (!l)
                continue;
            const char* ls = l;
            while (ls > buf && ls[-1] != '\n')
                ls--;
            const char* body = strstr(ls, "] "); /* after the log line's clock */
            double ms = body ? atof(body + 2) : -1, dr = -1;
            const char* q = strstr(l, ")  ");
            if (q)
                dr = atof(q + 3);
            if (k)
                mh = ms, dh = dr;
            else
                mc = ms, dc = dr;
        }
        printf("        base window: cheap %.2f ms (%.1f draws/frame), expensive %.2f ms (%.1f draws/frame)\n", mc, dc, mh, dh);
        EXPECT(lc && lh, "both pixel shaders timed on their own (a timestamp at the change inside the pass)");
        EXPECT(mh > mc && mc >= 0, "the expensive shader costs more than the cheap one drawn 3 times");
        EXPECT(dc > 2.5 && dc < 3.5 && dh > 0.7 && dh < 1.3, "draws per frame counted per shader: 3 and 1");
        /* the knock-out window: the expensive shader's draws left out, no time for it */
        const char* k0 = strstr(buf, tag);
        const char* k1 = k0 ? strstr(k0 + 1, "---- CPU report") : NULL;
        const char* kl = k0 ? strstr(k0, "Knock-out: pixel shader ") : NULL;
        const char* kt = k0 ? strstr(k0, hh) : NULL;
        if (k1 && kl > k1)
            kl = NULL;
        double left = 0;
        if (kl) {
            const char* q = strstr(kl, "left out: ");
            left = q ? atof(q + 10) : 0;
        }
        printf("        knock-out window: %.0f draws left out\n", left);
        EXPECT(k0 && left > 15, "knock-out: the expensive shader's draws left out in its variant");
        EXPECT(k0 && !(kt && (!k1 || kt < k1)), "knock-out: no time measured for the left-out shader");
        /* the bytecode of the timed shaders written to the output folder */
        const char* sd = strstr(buf, "bytecode goes to ");
        char dirp[MAX_PATH] = "", fpath[MAX_PATH];
        if (sd)
            sscanf(sd + 17, "%259[^\r\n]", dirp);
        snprintf(fpath, sizeof(fpath), "%s\\ps_%016llx.dxbc", dirp, heavy_h);
        EXPECT(sd && GetFileAttributesA(fpath) != INVALID_FILE_ATTRIBUTES, "the expensive shader's bytecode written");
    } else if (features) {
        EXPECT(strstr(buf, "Features cycle: ") != NULL, "the features cycle started");
        EXPECT(strstr(buf, "Far meshes: ") != NULL, "far meshes set up at device creation");
        EXPECT(strstr(buf, "[Q0 mod-defaults transition]") != NULL, "the first variant held, its windows left out");
        EXPECT(strstr(buf, "now running variant 1 (no-shadow-culling)") != NULL, "the cycle moved on after the hold");
    } else if (overall) {
        EXPECT(strstr(buf, "Overall cycle: ") != NULL, "the overall cycle started");
        EXPECT(strstr(buf, "Far meshes: ") != NULL, "far meshes set up at device creation although the game-as-is half runs first");
        EXPECT(strstr(buf, "now running variant 1 (mod-defaults)") != NULL, "the cycle reached the mod's half");
        EXPECT(strstr(buf, "then that with the steadier image") != NULL, "the cycle has the steadier-image variant");
    } else if (occl) {
        char l1[1500], l2[1500], l3[600];
        line_before_last(buf, "Occlusion census: ", l1, sizeof(l1));
        line_before_last(buf, "Occlusion census over time", l2, sizeof(l2));
        line_before_last(buf, "Occlusion census by bucket", l3, sizeof(l3));
        printf("        %s\n        %s\n        %s\n", l1, l2, l3);
        double objs = 0, wrong = -1, wrong4 = -1;
        int hid = -1, order = -1, b[5] = {-1, -1, -1, -1, -1};
        const char* p;
        if ((p = strstr(l1, "Occlusion census: ")))
            objs = atof(p + 18);
        if ((p = strstr(l1, "hidden (0 samples): ")))
            hid = atoi(p + 20);
        if ((p = strstr(l1, "m: hidden ")))
            sscanf(p + 10, "%d / %d / %d / %d / %d", &b[0], &b[1], &b[2], &b[3], &b[4]);
        if ((p = strstr(l2, "of the triangles, ")))
            wrong = atof(p + 18);
        if (p && (p = strstr(p + 18, "of the triangles, ")))
            wrong4 = atof(p + 18);
        if ((p = strstr(l2, "draw order: ")))
            order = atoi(p + 12);
        EXPECT(objs > 2.8 && objs < 3.2, "3 scene objects per frame measured");
        EXPECT(hid >= 40 && hid <= 60, "half of them hidden (one always, one 8 frames in 16)");
        EXPECT(b[0] == 0 && b[1] == 100 && b[3] >= 30 && b[3] <= 70,
               "per distance band: in front 0%, behind 100%, the alternating one about half");
        EXPECT(strstr(l1, ", 0 without a free query, 0 unread") != NULL, "every query read back");
        EXPECT(wrong > 0.1 && wrong < 0.3 && wrong4 > 0.1 && wrong4 < 0.3,
               "read 3 frames late, the alternating object would be left out wrongly 3 frames in 16");
        EXPECT(order >= 30 && order <= 36, "draw order: 1 of 3 neighbouring objects farther than the one before");
        EXPECT(strstr(l3, "1 (320x240, 0 targets, world): 3 objects") != NULL, "objects filed under their bucket and pass");
    } else if (quality) {
        /* the sampler bound by the game carries the +2 bias (characters are recognised in the
         * engine, not testable here) */
        const char *q = NULL, *t;
        for (t = buf; (t = strstr(t, "Quality: samplers biased")) != NULL; t++)
            q = t;
        char l[600] = "";
        if (q)
            sscanf(q, "%599[^\n]", l);
        printf("        %s\n        sampler bias seen by the game's context: %.1f\n", l, q_bias_seen);
        EXPECT(q_bias_seen > 1.99f && q_bias_seen < 2.01f, "TextureMipBias=2: the bound sampler carries +2");
        /* MaxAnisotropy: the game's 16x sampler is bound clamped (or untouched without the option) */
        int clamp = -1;
        for (t = buf; (t = strstr(t, "anisotropy clamp ")) != NULL; t++)
            clamp = atoi(t + 17);
        printf("        anisotropy clamp in the log %d, the bound sampler's MaxAnisotropy %d\n", clamp, q_aniso_seen);
        EXPECT(clamp >= 0 && q_aniso_seen == (clamp ? clamp : 16), "the bound sampler carries the anisotropy clamp");
        const char* g = NULL;
        for (t = buf; (t = strstr(t, "scene triangles per frame")) != NULL; t++)
            g = t;
        const char* od = g ? strstr(g, "other draws ") : NULL;
        EXPECT(od && atof(od + 12) > 0, "triangle accounting: the test's draws counted outside scene objects");
    } else if (passes) {
        /* last full window; per frame: 3 cleared 2048^2 D32 shadow passes (16 MiB stored each);
         * the 320x240 RGBA8 back buffer loaded, split by a copy, reloaded; a full-screen pass
         * (avoidable load); a discarded target; a cleared target with an unused D32 depth
         * (loaded and stored for nothing). 320x240x4 = 300 KiB per attachment. */
        const char *rp = NULL, *at = NULL, *sn = NULL, *q;
        for (q = buf; (q = strstr(q, "Render passes: ")) != NULL; q++)
            rp = q;
        for (q = buf; (q = strstr(q, "Render pass attachments per frame")) != NULL; q++)
            at = q;
        for (q = buf; (q = strstr(q, "Render passes of one frame")) != NULL; q++)
            sn = q;
        char l1[400] = "", l2[600] = "", l3[700] = "";
        if (rp)
            sscanf(rp, "%399[^\n]", l1);
        if (at)
            sscanf(at, "%599[^\n]", l2);
        if (sn)
            sscanf(sn, "%699[^\n]", l3);
        printf("        %s\n        %s\n        %s\n", l1, l2, l3);
        EXPECT(strstr(l1, "Render passes: 7 per frame") && strstr(l1, "-> 8 render pass instances (1.0 splits: 1.0 by copies"),
               "7 passes, 8 render pass instances, the copy splits one");
        EXPECT(strstr(l2, "4.0 cleared, 1.0 discarded, 3.0 loaded at pass start (0.9 MB), 1.0 reloaded after splits (0.3 MB)"),
               "clears (bound and deferred), the discard, loads and the reload after the split");
        EXPECT(strstr(l2, "stored 49.8 MB"), "stores: 3 x 16 MiB + 6 x 300 KiB");
        EXPECT(strstr(l2, "0.3 MB of loads in 1.0 passes that overwrite") && strstr(l2, "0.6 MB load+store of depth bound but unused in 1.0"),
               "the full-screen pass and the unused depth buffer flagged");
        EXPECT(strstr(l3, "2048x2048S+D:C:40") && strstr(l3, "320x240:L/2") && strstr(l3, "320x240:F:1") &&
                   strstr(l3, "320x240:D:1") && strstr(l3, "320x240+D:CLu:2"),
               "one frame's pass sequence");
    } else if (probe) {
        double passes = 0;
        for (const char* p = strstr(buf, "GPU probe: "); p; p = strstr(p + 1, "GPU probe: ")) {
            char* end;
            double v = strtod(p + 11, &end);
            if (end != p + 11 && strncmp(end, " shadow-map passes/frame", 24) == 0) {
                passes = v;
                break;
            }
        }
        printf("        shadow-map passes per frame: %.1f\n", passes);
        EXPECT(strstr(buf, "hooked deferred context method table") != NULL,
               "deferred-context creation hooked (command lists)");
        EXPECT(strstr(buf, "92% recorded on deferred contexts") != NULL &&
                   strstr(buf, "1.0 command lists finished, 1.0 executed") != NULL,
               "draws on deferred contexts and command lists counted");
        EXPECT(passes > 2.9 && passes < 3.1, "GPU probe finds 3 shadow-map passes per frame");
        EXPECT(strstr(buf, "draws 40 / 40 / 30") != NULL, "draws per shadow pass measured (40/40/30)");
        EXPECT(strstr(buf, "size: 50%") != NULL, "identical-object-set check (pass 2 same, pass 3 different)");
        EXPECT(strstr(buf, "call chain") != NULL && strstr(buf, "frame_test.exe+") != NULL,
               "shadow draws traced back to the calling code");
        EXPECT(strstr(buf, "2048x2048Sd:40") != NULL && strstr(buf, "320x240:10") != NULL,
               "per-frame pass picture (2048x2048 deferred shadow, 320x240 screen)");
        if (verify) {
            LONG64 checked = 0, drew = 0, unread = 0;
            if (vcounts)
                vcounts(&checked, &drew, &unread);
            printf("        occlusion queries: %d issued, %lld read back, %lld with samples, %lld unavailable\n",
                   queries, checked, drew, unread);
            EXPECT(vdraw && queries == 5 * 150, "every wrapped draw got an occlusion query on the deferred context");
            EXPECT(checked >= queries - 5 * 3 && unread == 0,
                   "query results read back on the immediate context after the command list ran");
            if (vs) /* per frame: 1 wrapped draw covers the triangle, 4 are degenerate */
                EXPECT(drew >= checked / 5 - 1 && drew <= checked / 5 + 1,
                       "positive control: exactly the visible wrapped draw shows samples");
            else
                EXPECT(drew == 0, "draws without shaders produced no samples");
        }
    } else if (free_run) {
        EXPECT(strstr(buf, "by the MetaphorCPU limiter") != NULL, "MetaphorCPU limiter holds 30 fps");
        const char* sd = strstr(buf, ", sd ");
        double sdv = sd ? atof(sd + 5) : 99;
        printf("        frame time sd with the limiter: %.1f ms\n", sdv);
        EXPECT(sdv < 2.0, "even frame pacing (sd < 2 ms)");
    } else {
        EXPECT(strstr(buf, "standard cap value") != NULL, "30 fps cap recognised by the verdict");
    }
    if (strstr(buf, "Sampler:")) {
        EXPECT(strstr(buf, "samples: tid") != NULL && strstr(buf, "executing:") != NULL,
               "sampler attributed samples to code locations");
    }
    printf("\n%s (%d failure%s)\n", fail ? "FAIL" : "PASS", fail, fail == 1 ? "" : "s");
    return fail ? 1 : 0;
}
