/* Frame statistics from IDXGISwapChain::Present: frame rate, pacing, time the main
 * thread spends blocked inside Present (GPU/limiter wait) and the V-Sync interval
 * the game requests. Hooks the swap chain's vtable, so it measures whatever object
 * the game actually presents through (DXVK, ReShade/OptiScaler wrappers, native). */
#include "mcpu.h"
#include <string.h>

typedef HRESULT (WINAPI* D3D11CreateDeviceAndSwapChain_t)(void*, int, HMODULE, UINT, const void*, UINT, UINT,
                                                           const void*, void**, void**, void*, void**);
typedef HRESULT (WINAPI* CreateDXGIFactory1_t)(REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* Present_t)(void*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* GetParent_t)(void*, REFIID, void**);
typedef ULONG (STDMETHODCALLTYPE* Release_t)(void*);
typedef HRESULT (STDMETHODCALLTYPE* QueryInterface_t)(void*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* CreateSwapChain_t)(void*, void*, void*, void**);
typedef HRESULT (STDMETHODCALLTYPE* CreateSwapChainForHwnd_t)(void*, void*, HWND, const void*, const void*, void*,
                                                               void**);

/* vtable slots */
enum { VT_QI = 0, VT_RELEASE = 2, VT_GETPARENT = 6, VT_PRESENT = 8, VT_CREATESWAPCHAIN = 10,
       VT_CREATESWAPCHAINFORHWND = 15 };

static const GUID IID_Factory = {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
static const GUID IID_Factory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};

void* o_D3D11CreateDeviceAndSwapChain;
void* o_CreateDXGIFactory1;
FrameTotals g_frames;

#define MAX_VT 8
typedef struct VtHook {
    void** vt;
    void* orig;
} VtHook;
static VtHook g_present[MAX_VT], g_csc[MAX_VT], g_cscfh[MAX_VT];
static int g_present_n, g_csc_n, g_cscfh_n;
static SRWLOCK g_vt_lock = SRWLOCK_INIT;
static uint64_t g_last_present_end;

static void* vt_orig(VtHook* tab, int n, void* self)
{
    void** vt = *(void***)self;
    for (int i = 0; i < n; i++)
        if (tab[i].vt == vt)
            return tab[i].orig;
    return n ? tab[0].orig : NULL;
}

/* Patch slot `slot` of self's vtable to `hook` once per distinct vtable. */
static int vt_hook(VtHook* tab, int* n, void* self, int slot, void* hook, const char* what)
{
    void** vt = *(void***)self;
    int done = 0;
    AcquireSRWLockExclusive(&g_vt_lock);
    for (int i = 0; i < *n; i++)
        if (tab[i].vt == vt)
            done = 1;
    if (!done && vt[slot] != hook && *n < MAX_VT) {
        tab[*n].vt = vt;
        tab[*n].orig = vt[slot];
        (*n)++;
        void* h = hook;
        done = patch_bytes(&vt[slot], &h, sizeof(h));
        char at[64];
        LOGI("Frame stats: hooked %s (original %s)", what, fmt_addr(tab[*n - 1].orig, at, sizeof(at)));
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
    return done;
}

static int bucket_of_ms(double ms)
{
    static const double edges[FRAME_BUCKETS - 1] = {14, 18, 25, 31, 36, 50, 100};
    for (int i = 0; i < FRAME_BUCKETS - 1; i++)
        if (ms < edges[i])
            return i;
    return FRAME_BUCKETS - 1;
}

/* ---- precise frame limiter: sleep until the next deadline, spin only for the
 * learned oversleep margin. Replaces the game's own cap (set that to Unlimited). */
static uint64_t g_period_ticks, g_next_deadline;
static double g_overshoot_us = 300.0;

void frames_init(void)
{
    if (g_cfg.limiter_fps > 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_period_ticks = (uint64_t)f.QuadPart / (uint64_t)g_cfg.limiter_fps;
        LOGI("Frame limiter: %d fps, sleeping wait, spin margin %s", g_cfg.limiter_fps,
             g_cfg.limiter_margin_us >= 0 ? "fixed" : "learned from sleep overshoot");
    }
}

static void limiter_wait(void)
{
    uint64_t now = qpc_now();
    if (!g_next_deadline || now > g_next_deadline + g_period_ticks) {
        g_next_deadline = now + g_period_ticks; /* first frame or after a long stall: resync */
        return;
    }
    if (now < g_next_deadline) {
        uint64_t start = now;
        double remain_us = qpc_to_us(g_next_deadline - now);
        double margin = g_cfg.limiter_margin_us >= 0 ? g_cfg.limiter_margin_us : g_overshoot_us + 50.0;
        if (remain_us > margin) {
            DWORD req = (DWORD)(remain_us - margin);
            uint64_t t0 = qpc_now();
            precise_sleep_us(req);
            double over = qpc_to_us(qpc_now() - t0) - req;
            if (over < 0)
                over = 0;
            g_overshoot_us = g_overshoot_us * 0.9 + over * 0.1;
            if (g_overshoot_us > 3000)
                g_overshoot_us = 3000;
        }
        uint64_t spin_start = qpc_now();
        while (qpc_now() < g_next_deadline)
            YieldProcessor();
        uint64_t end = qpc_now();
        g_frames.sum_limiter_ms += qpc_to_us(end - start) / 1000.0;
        g_frames.sum_limiter_spin_ms += qpc_to_us(end - spin_start) / 1000.0;
    }
    g_next_deadline += g_period_ticks;
}

static HRESULT STDMETHODCALLTYPE hk_Present(void* self, UINT sync, UINT flags)
{
    Present_t orig = (Present_t)vt_orig(g_present, g_present_n, self);
    if (flags & 0x1 /* DXGI_PRESENT_TEST */)
        return orig(self, sync, flags);
    if (g_period_ticks)
        limiter_wait();
    gpuprobe_before_present(); /* GPU timing: end-of-frame mark goes in before Present flushes */
    if (g_cfg.capture || g_cfg.compare_shots)
        capture_present(self); /* frame capture: the back buffer, and the recording's frame boundaries */
    if (g_cfg.dump_tex)
        effects_present(); /* the character texture dump's readback */
    if (g_cfg.q_farmesh || g_cfg.q_fm_both)
        farmesh_present(self); /* far meshes: the screen height for their pixel error */
    uint64_t t0 = qpc_now();
    HRESULT hr = orig(self, sync, flags);
    uint64_t t1 = qpc_now();

    /* single writer (the presenting thread); the profiler reads cumulative values */
    if (g_last_present_end) {
        double interval_ms = qpc_to_us(t1 - g_last_present_end) / 1000.0;
        double inside_ms = qpc_to_us(t1 - t0) / 1000.0;
        if (interval_ms > 500.0) {
            g_frames.stalls++; /* loading screens, alt-tab: keep them out of pacing stats */
        } else {
            g_frames.frames++;
            g_frames.sum_interval_ms += interval_ms;
            g_frames.sum_sq_interval_ms += interval_ms * interval_ms;
            g_frames.sum_inside_ms += inside_ms;
            g_frames.buckets[bucket_of_ms(interval_ms)]++;
            if (sync)
                g_frames.vsync_frames++;
            g_frames.last_sync = sync;
            LONG64 us = (LONG64)(interval_ms * 1000.0);
            if (us > g_frames.window_max_us)
                g_frames.window_max_us = us;
        }
        if (g_cfg.q_alod)
            adaptive_frame(interval_ms); /* the simple-NPC-model distance follows the frame rate */
    }
    g_last_present_end = t1;
    gpuprobe_frame_end();
    if (g_cfg.capture || g_cfg.compare_shots)
        capture_frame_end();
    return hr;
}

static void hook_factory(void* factory);

static void hook_swapchain(void* sc)
{
    if (!sc)
        return;
    vt_hook(g_present, &g_present_n, sc, VT_PRESENT, (void*)hk_Present, "IDXGISwapChain::Present");
    /* also catch swap chains created later through the device's factory */
    void* factory = NULL;
    GetParent_t gp = (GetParent_t)(*(void***)sc)[VT_GETPARENT];
    if (SUCCEEDED(gp(sc, &IID_Factory, &factory)) && factory) {
        hook_factory(factory);
        ((Release_t)(*(void***)factory)[VT_RELEASE])(factory);
    }
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(void* self, void* dev, void* desc, void** out)
{
    CreateSwapChain_t orig = (CreateSwapChain_t)vt_orig(g_csc, g_csc_n, self);
    HRESULT hr = orig(self, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out)
        vt_hook(g_present, &g_present_n, *out, VT_PRESENT, (void*)hk_Present, "IDXGISwapChain::Present");
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(void* self, void* dev, HWND wnd, const void* desc,
                                                          const void* fs, void* output, void** out)
{
    CreateSwapChainForHwnd_t orig = (CreateSwapChainForHwnd_t)vt_orig(g_cscfh, g_cscfh_n, self);
    HRESULT hr = orig(self, dev, wnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out)
        vt_hook(g_present, &g_present_n, *out, VT_PRESENT, (void*)hk_Present, "IDXGISwapChain1::Present");
    return hr;
}

static void hook_factory(void* factory)
{
    vt_hook(g_csc, &g_csc_n, factory, VT_CREATESWAPCHAIN, (void*)hk_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
    void* f2 = NULL;
    QueryInterface_t qi = (QueryInterface_t)(*(void***)factory)[VT_QI];
    if (SUCCEEDED(qi(factory, &IID_Factory2, &f2)) && f2) {
        vt_hook(g_cscfh, &g_cscfh_n, f2, VT_CREATESWAPCHAINFORHWND, (void*)hk_CreateSwapChainForHwnd,
                "IDXGIFactory2::CreateSwapChainForHwnd");
        ((Release_t)(*(void***)f2)[VT_RELEASE])(f2);
    }
}

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(void* adapter, int type, HMODULE sw, UINT flags, const void* levels,
                                                UINT nlevels, UINT sdk, const void* desc, void** sc, void** dev,
                                                void* level, void** ctx)
{
    HRESULT hr = ((D3D11CreateDeviceAndSwapChain_t)o_D3D11CreateDeviceAndSwapChain)(
        adapter, type, sw, flags, levels, nlevels, sdk, desc, sc, dev, level, ctx);
    if (SUCCEEDED(hr) && sc && *sc)
        hook_swapchain(*sc);
    if (SUCCEEDED(hr))
        gpuprobe_on_device(dev ? *dev : NULL, ctx ? *ctx : NULL);
    return hr;
}

HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void** out)
{
    HRESULT hr = ((CreateDXGIFactory1_t)o_CreateDXGIFactory1)(riid, out);
    if (SUCCEEDED(hr) && out && *out)
        hook_factory(*out);
    return hr;
}
