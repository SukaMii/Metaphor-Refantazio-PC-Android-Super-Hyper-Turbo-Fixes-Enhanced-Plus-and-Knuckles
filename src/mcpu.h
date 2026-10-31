/* MetaphorCPU - CPU scheduling fixes and profiler for Metaphor: ReFantazio.
 * Shared declarations. */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>

#define MCPU_VERSION "0.52.1"

/* ------------------------------------------------------------------ config */
typedef struct Config {
    int enabled;
    int profile;              /* PROFILE_ANDROID / PROFILE_PC: which defaults apply ([General] Profile) */
    int profile_auto;         /* chosen from the host (Wine = Android), not set in the INI */
    int profile_when_disabled; /* Enabled=0: still measure (pass-through hooks only) */
    int log_level;            /* 0 errors, 1 info, 2 verbose */

    int prof_enabled;
    int prof_interval_sec;
    int prof_top;
    int frame_stats;          /* hook Present for frame rate / pacing / GPU-wait stats */

    int fix_timer_churn;      /* make the game's per-call timeBeginPeriod/timeEndPeriod no-ops */
    int timer_period_ms;      /* resolution requested once at startup (0 = leave alone) */
    int precise_usleep;       /* replace engine usleep(us) with a microsecond-accurate sleep */

    int adaptive_yield;       /* escalate Sleep(0) storms to short real sleeps */
    int yield_mode;           /* 0 = Sleep(0), 1 = SwitchToThread for the cheap phase */
    int yield_spin_us;        /* microseconds of Sleep(0) spinning before escalating */
    int yield_sleep_us;       /* sleep length once escalated */
    int yield_window_us;      /* gap that ends a Sleep(0) episode */

    int replace_spinlock;     /* replace the engine spinlock (1500 x CAS + Sleep(0)) */
    int spin_iters;
    int spin_yields;
    int spin_wait_ms;

    int cs_spin_count;        /* -1 = unchanged */

    int affinity_mode;        /* 0 passthrough, 1 ignore game pinning, 2 remap into allowed_cores */
    uint64_t allowed_cores;   /* 0 = all cores in the process mask */
    int main_cores_auto;      /* MainThreadCores=auto -> fastest cores */
    uint64_t main_cores;      /* explicit mask, 0 = do not touch */
    uint64_t worker_cores;    /* mask for game-created threads, 0 = do not touch */
    int detect_fast_cores;
    char fast_thread_names[256]; /* comma list of thread-name prefixes pinned to fast cores */
    char output_folder[MAX_PATH]; /* [GpuProbe] OutputFolder: captures, compare shots, mesh censuses go in here */
    char log_copy[MAX_PATH];      /* [General] LogCopy: a second copy of the log (a Linux path under Wine), e.g. on
                                   * the tablet's shared storage, where adb can read it during a run */

    int main_priority;        /* -2..2, 99 = do not touch */
    int worker_priority;      /* priority for jobWorker_* threads, 99 = do not touch */

    int job_pool0;            /* 0 = unchanged (game default 5) */
    int job_pool1;

    int telemetry;            /* Wine/Android: power, temperatures, clocks, thread placement from /sys + /proc */
    int telemetry_sample_ms;

    int limiter_fps;          /* 0 = off; else precise sleeping frame limiter in Present */
    int limiter_margin_us;    /* -1 = auto (learned sleep overshoot) */

    int sampler_hz;           /* 0 = off; else sample busy threads' code locations */
    int sampler_threads;      /* busiest threads sampled in addition to the main thread */

    int ab_enabled;           /* alternate game-as-is (A) and the configured fixes (B) in one session */
    int ab_switch_sec;
    int ab_start_after_sec; /* [ABTest] StartAfterSec: a cycle holds its first variant this long (loading, warming up) */
    int ab_compare;           /* AB_ALL, the setting(s) A turns off (shadowcull|preciseusleep|animcull|cpu), or AB_QUALITY (cycle) */

    int fast_sem;             /* replace the engine's wake-all + handshake semaphore (startup only) */
    int sem_spin;             /* polls before a semaphore wait goes to sleep */

    int gpu_probe;            /* diagnostic: count draws per render pass, find the shadow-map passes */
    int gpu_timing;           /* diagnostic: GPU time per render pass (D3D11 timestamp queries) */
    int gpu_passes;           /* diagnostic: render pass loads/stores/splits as DXVK on a tiled GPU sees them */

    int shadow_cull;          /* skip shadow casters outside each cascade (switchable at run time) */
    int shadow_margin_pct;    /* extra border, % of the cascade width */
    int shadow_verify;        /* diagnostic: skip nothing, occlusion-query every would-be-skipped draw */
    int shadow_skinned_pct;   /* skinned/morphing casters: box margin in % of its size, -1 = never cull */
    int shadow_view;          /* casters whose shadow cannot reach the camera's view: 0 off, 1 measure, 2 skip */
    int shadow_view_depth_pct; /* receiver depth range of a cascade widened by this % of its split distances */

    int wait_profile;         /* diagnostic: where the main thread is blocked, per frame */
    /* [Quality]: options that change the picture (1 = as the game draws it) */
    int q_characters;         /* draw skinned meshes (characters) in the scene passes */
    int q_char_hide;          /* scene-pass modes (bit per interpreter mode 0/2/3) whose character draws are left out */
    int q_outlines;           /* draw character outlines (skinned, front faces culled) */
    int q_char_shadows;       /* skinned meshes cast shadows */
    int q_far_shadows;        /* draw the far shadow cascade */
    int q_shadows;            /* draw shadow maps at all */
    int q_mip_bias;           /* texture LOD bias added to the game's samplers (0 = none) */
    int q_report;             /* log character / outline draw statistics */
    /* the "bubble": objects farther than q_far_dist from the camera */
    int q_far_dist;           /* metres */
    int q_far_chars;          /* 1 = far characters drawn, 0 = left out (with their shadows and small attachments) */
    int q_far_anim;           /* far characters animated every Nth frame (1 = every frame) */
    int q_far_mip;            /* texture LOD bias added for far objects (0 = none) */
    int q_char_lod;           /* metres from the hero beyond which the game shows its simple character models, 0 = the game's */
    int q_alod;               /* adaptive: q_char_lod steps between q_alod_min and the set distance by frame rate (adaptive.c) */
    int q_alod_min;
    int q_alod_down_sec, q_alod_up_sec; /* the windows judged: seconds */
    float q_alod_down, q_alod_up; /* FPS under which it moves in / at or above which it moves out */
    float q_alod_in_speed, q_alod_out_speed; /* metres per second */
    int q_shadow_dist;        /* metres: cap on the shadow distance (all cascade splits follow), 0 = the game's (200 m) */
    float q_tiny_px;          /* scene draws whose bounding sphere is smaller than this many pixels (at 720p) are skipped, 0 = off */
    float q_tiny_texels;      /* static shadow casters smaller than this many texels (1024^2 map) are skipped, 0 = off */
    int q_max_aniso;          /* anisotropic filtering clamped to this level, 0 = the game's */
    int q_far_world;          /* MEASUREMENT: world objects beyond this many metres not drawn, 0 = off */
    int q_shadow_refresh;     /* the far shadow cascade is rendered every Nth frame (1 = every frame) */
    int q_fog;                /* volumetric fog drawn (1 = as the game) */
    int q_glare;              /* glare drawn */
    int q_smaa;               /* anti-aliasing: 3 Ultra (the game's), 2 High, 1 Medium, 0 off */
    int q_fx_active;          /* the post-processing hooks are installed (effects.c) */
    int q_farmesh;            /* simpler far world meshes (farmesh.c): 0 off, 1 seams kept, 2 across seams */
    float q_fm_px;            /* their largest shape error on screen, pixels */
    int q_fm_floor;           /* a simpler version keeps at least this percentage of the triangles */
    int q_fm_both;            /* both kinds are made (switched at run time: A/B, compare shots) */
    float q_fm_shadow;        /* shadow casters: largest error in shadow-map texels (0 = judged by the screen) */
    float q_fm_seams_from;    /* FarMeshes=seams: across seams only from this many metres (nearer: seams kept), 0 = everywhere */
    int compare_shots;        /* [GpuProbe] CompareShots: F10 saves the final image with each effect variant */
    int capture;              /* [GpuProbe] Capture: one frame's render passes saved as images (F10 / after CaptureAfterSec) */
    int capture_after_sec;
    int capture_full;         /* [GpuProbe] CaptureFullSize: the pass images at full size (default: at most 960 wide) */
    int capture_steps;        /* [GpuProbe] CaptureDrawSteps: the characters' pass, the middle of the screen after each draw */
    int dump_tex;             /* [GpuProbe] DumpCharacterTextures: the material textures of near characters as DDS */
    float dump_dist;          /* [GpuProbe] DumpDistance: "near" in metres */
    int mesh_census;          /* [GpuProbe] MeshCensus: F9 records which meshes make the far triangles (meshcensus.c) */
    int occl_census;          /* [GpuProbe] OcclusionCensus: an occlusion query per scene object, hidden share logged */
    int occl_hooks;           /* its hooks are installed (the INI's OcclusionCensus; Compare=occlusion switches the census) */
    int shader_timing;        /* [GpuProbe] ShaderTiming (PC): a timestamp at every pixel-shader change inside a pass */
    uint64_t q_knock;         /* the pixel shader (bytecode hash) whose draws are left out now, 0 = none */
    uint64_t q_knock_list[6]; /* [GpuProbe] KnockoutShaders: the shaders Compare=knockout leaves out in turn */
    int q_knock_n;
    float q_outline_dist;     /* [Quality] OutlineDistance: outlines of objects farther than this (m) left out, 0 = all drawn */
    int q_outline_hooks;      /* outline shaders recognised and the scene objects' distance marked (option, cycle, shots) */
    int q_painterly;          /* [Quality] Painterly: the game's painterly wobble and edge darkening (1 = as the game) */
    int q_outline_carried;    /* [Quality] OutlineCarried: objects inside a character's sphere count as the character's */
    float q_rim_dist;         /* [Quality] RimLight: the toon rim light only on objects within this (m); 0 = the game's, <1 = none */
    int q_rim_hooks;          /* rim-free copies of the toon shaders made at creation */
    float q_inner_dist;       /* [Quality] InnerOutlines: outline lines over the character's own body only within this (m);
                               * 0 = the game's, <1 = none (the silhouette keeps its outline) */
    int q_inner_hooks;        /* copies of the outline shaders without the inner lines made at creation */
    int q_char_tex_q;         /* [Quality] CharacterTextureBias, in quarter mip levels: characters' textures a little softer */
    float q_ol_depth;         /* [Quality] OutlineDepthOffset: the characters' outlines pushed back by this fraction of their
                               * distance (0 = as the game): lines over folds and hair clumps no longer poke through */
    int q_hatch;              /* [Quality] Hatching: the toon shading's hatching texture from this many mip levels lower
                               * (0 = as the game, 99 = its average: no strokes) */
    int q_hatch_hooks;        /* toon shaders' hatching slot known, its view swapped per draw */
    int q_line_mode;          /* [Quality] LineFix: the characters' ink line meshes: 0 as the game, 1 drawn again shifted by
                               * half a pixel (q_line_redraws times), 2 conservative rasterization, 3 left out */
    int q_line_redraws;
    float q_line_dist;        /* [Quality] LineDistance: only for characters within this (m), 0 = all */
    uint64_t q_line_ps[8];    /* [Quality] LineShaders: the pixel shaders of the line meshes */
    int q_line_n;
    int q_line_hooks;
    float q_stab;             /* [Quality] TemporalStabilise: the share of the previous frame a still character pixel keeps
                               * (0 = off; temporal.c) */
    float q_stab_world;       /* [Quality] TemporalStabiliseWorld: the same for every other pixel (0 = off) */
    int q_steady;             /* [Quality] SteadyImage: one switch for the stabilisation, the characters' motion vectors and
                               * SMAA Medium; each part's own key, when set, wins */
    int q_stab_hooks;        /* the pass is set up (the options, or their cycle) */
    int q_mv;                 /* [Quality] CharacterMotionVectors: the characters' motion drawn for the stabilisation */
    float q_mv_dist;          /* [Quality] MotionVectorDistance: only for objects within this (m), 0 = all */
    float q_mv_skin_dist;     /* [Quality] MotionVectorSkinDistance: the skinned vertices copied (the limbs' motion) only
                               * within this (m), beyond the transform's motion alone; 0 = all */
    int q_mv_crowd;           /* [Quality] MotionVectorSkinCrowd: the skin distance only with more characters than this
                               * within MotionVectorDistance (fewer: a fight, every character keeps its limbs' motion) */
    int q_stab_world_taps;    /* [Quality] TemporalStabiliseWorldTaps: the neighbourhood of the world's pixels, 8 or 4 */
    int q_world_tex_q;        /* [Quality] WorldTextureBias in quarter mip levels: the world's textures a little smaller */
    int compute_census;       /* [GpuProbe] ComputeCensus: the compute shaders dispatched, what they write, their time */
    int q_mv_view;            /* [GpuProbe] MotionVectorView: the motion shown instead of the image */
    int q_mv_hooks;           /* their hooks (the option, or its cycle) */
    int q_active;             /* any of the above in use (installs the hooks) */
} Config;

enum { AB_ALL = 0, AB_SHADOWCULL, AB_PRECISE_USLEEP, AB_ANIMCULL, AB_CPU, AB_QUALITY, AB_CHARMODES, AB_FAR, AB_BUBBLE, AB_GPU, AB_SHADOWREFRESH, AB_EFFECTS, AB_FARMESH, AB_FMSHADOW, AB_OVERALL, AB_OCCLUSION, AB_KNOCKOUT, AB_OUTLINES, AB_LOOK, AB_LINES, AB_STAB, AB_SMAA, AB_MOTION, AB_SAVINGS, AB_FEATURES };

extern Config g_cfg;
extern HMODULE g_self;
extern int g_is_wine;

enum { PROFILE_ANDROID = 0, PROFILE_PC = 1 };
void config_defaults(Config* c);
void timing_period_release(void);
void config_load(Config* c, const char* ini_path);
void config_neutral(Config* c);

/* ------------------------------------------------------------------ logging */
void log_open(const char* path);
int log_copy_open(const char* path);
void log_msg(int level, const char* fmt, ...);
void output_dir(char* out, size_t cap, const char* kind);
#define LOGE(...) log_msg(0, __VA_ARGS__)
#define LOGI(...) log_msg(1, __VA_ARGS__)
#define LOGV(...) log_msg(2, __VA_ARGS__)

/* ------------------------------------------------------------------ stats */
typedef struct Stats {
    volatile LONG64 sleep0_calls;
    volatile LONG64 sleep0_escalated;
    volatile LONG64 sleepn_calls;
    volatile LONG64 period_calls;
    volatile LONG64 period_suppressed;
    volatile LONG64 usleep_calls;
    volatile LONG64 lock_contended;
    volatile LONG64 lock_waits;
    volatile LONG64 affinity_requests;
    volatile LONG64 affinity_blocked;
    volatile LONG64 priority_requests;
    volatile LONG64 threads_created;
    volatile LONG64 cs_inits;
    volatile LONG64 sem_waits;
    volatile LONG64 sem_sleeps;
    volatile LONG64 sem_posts;
    volatile LONG64 sem_wakes;
} Stats;
extern Stats g_stats;

/* ------------------------------------------------------------------ signatures (sigs.c) */
typedef struct Targets {
    uint8_t* spin_lock;       /* gfd spinlock acquire */
    uint8_t* spin_unlock;     /* gfd spinlock release (spin_lock + 0x40) */
    uint8_t* usleep;          /* gfd usleep(us): timeBeginPeriod(2); Sleep(ceil(us/1000)); timeEndPeriod(2) */
    uint8_t* yield;           /* gfd yield: fiber hop on main thread, then Sleep(0) */
    volatile DWORD* main_tid; /* engine's main-thread id variable (read from yield) */
    uint8_t* thread_create;   /* gfd thread create(entry, arg, prio, stack, flags, name) */
    uint8_t* job_init;        /* job pool init; mov edx,5 immediates at +7 and +22 */
    uint8_t* sem_create;      /* gfd semaphore create(initial, max): 0x48-byte struct (layout check) */
    uint8_t* sem_wait;        /* gfd semaphore wait(sem, timeout_us) */
    uint8_t* sem_post;        /* gfd semaphore post(sem, n) */
} Targets;

uint8_t* sig_find(uint8_t* start, size_t len, const char* pattern, int* count);
int sig_text_range(uint8_t* base, uint8_t** start, size_t* len);
int sig_resolve(uint8_t* base, Targets* t, int verbose);

/* ------------------------------------------------------------------ patching (patch.c) */
int patch_bytes(void* dst, const void* src, size_t n);
int patch_abs_jmp(void* at, void* to, size_t room);
void* make_trampoline(const uint8_t* orig, size_t prologue_len);
int iat_hook(HMODULE mod, const char* func, void* hook, void** orig_out);

/* ------------------------------------------------------------------ timing (timing.c) */
extern VOID (WINAPI* o_Sleep)(DWORD);
extern MMRESULT (WINAPI* o_timeBeginPeriod)(UINT);
extern MMRESULT (WINAPI* o_timeEndPeriod)(UINT);
void timing_init(void);
void precise_sleep_us(DWORD us);
VOID WINAPI hk_Sleep(DWORD ms);
MMRESULT WINAPI hk_timeBeginPeriod(UINT p);
MMRESULT WINAPI hk_timeEndPeriod(UINT p);
void mcpu_usleep(int us);
void timing_thread_detach(void);
int timing_thread_yields(DWORD tid, LONG64* yields, LONG64* escalated);
int timing_thread_index(void);          /* small per-thread index (1-based), 0 if none */
DWORD timing_index_tid(int index);

/* ------------------------------------------------------------------ job semaphore (jobsync.c) */
int jobsync_init(void);
int mcpu_sem_wait(void* sem, int timeout_us);
int mcpu_sem_post(void* sem, int n);
uint64_t qpc_now(void);
double qpc_to_us(uint64_t ticks);

/* ------------------------------------------------------------------ spinlock (spinlock.c) */
void spinlock_init(void);
LONG mcpu_spin_lock(volatile LONG* l);
LONG mcpu_spin_unlock(volatile LONG* l);

/* ------------------------------------------------------------------ threads (threads.c) */
enum ThreadKind { TK_UNKNOWN = 0, TK_MAIN, TK_JOB_WORKER, TK_ENGINE, TK_CRT, TK_WIN32 };

void threads_init(HMODULE game);
void threads_register(DWORD tid, int kind, const char* name, void* start, void* caller);
int threads_lookup(DWORD tid, char* name, size_t cap, int* kind);
DWORD threads_find_main(void);
DWORD threads_main_tid(void);
uint64_t threads_process_mask(void);
uint64_t detect_fast_cores(uint64_t allowed);
void threads_apply_main_policy(DWORD main_tid);
void threads_apply_name_policy(void);
void threads_reset_placement(void);
extern uint64_t g_fast_mask;
extern void* o_gfd_thread_create;
void* hk_gfd_thread_create(void* entry, void* arg, int prio, int stack, int flags, const char* name);

extern HANDLE (WINAPI* o_CreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
extern uintptr_t (__cdecl* o_beginthreadex)(void*, unsigned, unsigned (__stdcall*)(void*), void*, unsigned, unsigned*);
extern DWORD_PTR (WINAPI* o_SetThreadAffinityMask)(HANDLE, DWORD_PTR);
extern BOOL (WINAPI* o_SetThreadPriority)(HANDLE, int);
extern BOOL (WINAPI* o_InitializeCriticalSectionAndSpinCount)(LPCRITICAL_SECTION, DWORD);
HANDLE WINAPI hk_CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
uintptr_t __cdecl hk_beginthreadex(void*, unsigned, unsigned (__stdcall*)(void*), void*, unsigned, unsigned*);
DWORD_PTR WINAPI hk_SetThreadAffinityMask(HANDLE, DWORD_PTR);
BOOL WINAPI hk_SetThreadPriority(HANDLE, int);
BOOL WINAPI hk_InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION, DWORD);

/* ------------------------------------------------------------------ call-site histograms (sites.c) */
#define SITE_SLOTS 1024
typedef struct SiteTable {
    void* volatile addr[SITE_SLOTS];
    volatile LONG64 count[SITE_SLOTS];
} SiteTable;
extern SiteTable g_sites_yield, g_sites_escalated, g_sites_lock, g_sites_sleepn;
void site_add(SiteTable* t, void* addr, LONG64 n);
void site_report(const char* title, SiteTable* t, LONG64* prev, double seconds, int top);

/* ------------------------------------------------------------------ frame stats (frames.c) */
#define FRAME_BUCKETS 8 /* frame time <14, 14-18, 18-25, 25-31, 31-36, 36-50, 50-100, >=100 ms */
typedef struct FrameTotals {     /* cumulative, written by the presenting thread */
    volatile LONG64 frames;
    volatile LONG64 stalls;      /* gaps > 500 ms (loading, alt-tab) */
    volatile LONG64 vsync_frames;
    volatile LONG64 buckets[FRAME_BUCKETS];
    volatile LONG64 window_max_us; /* reset by the profiler */
    volatile double sum_interval_ms;
    volatile double sum_sq_interval_ms;
    volatile double sum_inside_ms;
    volatile double sum_limiter_ms;  /* time our limiter waited (sleep + spin) */
    volatile double sum_limiter_spin_ms;
    volatile UINT last_sync;
} FrameTotals;
void frames_init(void);
extern FrameTotals g_frames;
extern void* o_D3D11CreateDeviceAndSwapChain;
extern void* o_CreateDXGIFactory1;
HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(void*, int, HMODULE, UINT, const void*, UINT, UINT, const void*, void**,
                                                void**, void*, void**);
HRESULT WINAPI hk_CreateDXGIFactory1(REFIID, void**);

/* ------------------------------------------------------------------ GPU probe (gpuprobe.c) */
void gpuprobe_on_device(void* dev, void* ctx);
void gpuprobe_frame_end(void);
void gpuprobe_before_present(void);
void gpuprobe_report(double seconds);
void* gpuprobe_thread_ctx(int* shadow_pass);  /* context of this thread's last render-target bind */
void gpuprobe_set_far(int bias);              /* [Quality] extra texture LOD bias (quarter levels) for this thread's draws, 0 = none */
/* [Quality] the scene object this thread is drawing: texture bias, geometry class (-1 = none) and its
 * on-screen area in pixels at 720p, for the triangle accounting */
void gpuprobe_scene_node(int far_bias, int cls, float area_px, float dist_m);
/* meshcensus.c: [GpuProbe] MeshCensus (F9) */
void census_buffer_created(void* buf, const void* desc, int has_init);
void census_layout_created(void* lay, const void* elems, unsigned n);
void census_draw(void* ctx, unsigned count, unsigned inst, unsigned start, int base, int cls, float area, float dist_m,
                 int shadow);
int census_recording(void);
void census_start(int frames);
void census_frame_end(void* imm);
int mesh_layout_pos(void* lay, int* fmt, int* off, int* slot, int* skinned);
/* farmesh.c: [Quality] FarMeshes */
typedef struct FmPick {
    void *ib, *lod; /* the game's index buffer (held), the version's (held) */
    int fmt;
    unsigned off, start, count;
} FmPick;
void farmesh_init(void);
/* adaptive.c: [Quality] AdaptiveLod */
void adaptive_frame(double interval_ms);
void adaptive_frame_at(uint64_t now_ms, double interval_ms);
void adaptive_report(void);
int farmesh_pick(void* ctx, unsigned count, unsigned start, int base, int kind, float near_cm, float scale, float tpu,
                 FmPick* p);
void farmesh_done(FmPick* p);
void farmesh_frame_end(void* imm);
void farmesh_view(float p11);
void farmesh_present(void* swapchain);
void farmesh_report(void);
int farmesh_test_state(void* ib, unsigned off, unsigned start, unsigned count, int base, unsigned* counts);
void gpuprobe_fm_mark(int kind, float near_cm, float scale, float tpu);
/* [Quality] OutlineDistance: the scene object being drawn: its distance from the camera (m; -1 = none), whether it
 * is a character, its centre and scene frame; and the distance that counts for its outlines (-2 = not a character
 * nor carried by one; carried = 1 for a bag / weapon / hat inside a character's sphere) */
void gpuprobe_node_mark(float dist_m, int is_char, const float* c, LONG64 seq);
float gpuprobe_node_dist(int* carried);
float gpuprobe_node_raw_dist(void);         /* the scene object's own distance (m), -1 none */
void gpuprobe_bind_ps(void* ctx, void* ps);  /* binds a pixel shader with the context's original method */
void* gpuprobe_method(void* ctx, size_t slot); /* the context's method in a slot: the original where it is hooked */
LONG64 gpuprobe_node_seq(void);               /* the frame (scene sequence) of the object being drawn, 0 none */
int shadowcull_crowd(void); /* the characters within MotionVectorDistance in the last complete frame, -1 not known */
int shadowcull_carried(const float* c, LONG64 seq);
/* [GpuProbe] OcclusionCensus: the scene object whose draws follow (on = 1) or that ended (on = 0); key names
 * it across frames, dpx is its sphere's diameter in pixels at 720p, view where its sphere is against the view
 * frustum (0 inside, 1 at the edge, 2 outside, -1 not known), seq the scene frame */
void gpuprobe_oc_node(int on, uint64_t key, float dist_m, float dpx, int is_char, int bucket, int view, LONG64 seq);
#define GEO_BANDS 5      /* distance bands: <15, 15-30, 30-50, 50-100, >100 m */
#define GEO_CLASSES 10   /* world objects, then characters, per band */
int gpuprobe_query_begin(void* ctx);          /* occlusion query slot, -1 if none free */
void gpuprobe_query_end(void* ctx, int slot);

/* ------------------------------------------------------------------ frame capture (capture.c) */
int capture_recording(void);
void capture_bind(void* ctx, UINT n, void* const* rtvs, void* dsv);
void capture_draw(void* ctx, UINT verts, int nonindexed);
void capture_srvs(void* ctx, UINT start, UINT n, void* const* srvs);
void capture_ps(void* ctx, void* ps);
void capture_end_pass(void* ctx);
void capture_finish_cl(void* ctx, void* cl);
void capture_exec_cl(void* ctx, void* cl, int restore);
void capture_clear_state(void* ctx);
void capture_present(void* swapchain);
void capture_frame_end(void);
void capture_shader_created(void* ps, const void* code, size_t len);

/* ------------------------------------------------------------------ post-processing options (effects.c) */
void effects_shader_created(void* dev, int vs, void* sh, const void* code, size_t len, void* create);
void* effects_bind_ps(void* ctx, void* ps);
void* effects_bind_vs(void* ctx, void* vs);
int effects_draw(void* ctx, unsigned verts); /* before a draw: 1 = leave it out */
void effects_reset(void* ctx);
void effects_report(void);
void effects_present(void); /* on the presenting thread before Present: the texture dump's readback */
/* CompareShots=10: the pixel shaders drawn for characters within DumpDistance lately (bytecode hashes, most draws
 * first), at most max; the count */
int fx_near_char_ps(uint64_t* out, int max);
int effects_redraws(void* ctx); /* [Quality] LineFix=redraw: how many shifted copies of the draw just made (0 none) */
/* temporal.c: [Quality] TemporalStabilise */
void temporal_inputs(void* ctx); /* at the painterly pass's draw: its flags and depth */
void temporal_run(void* ctx);    /* before the SMAA edge pass's draw */
float temporal_weight(void);     /* the history's share now (0 = off: the option, or F7) */
void temporal_report(void);
/* motion.c: [Quality] CharacterMotionVectors */
void motion_bind(void* ctx, unsigned n, void* const* rtvs, void* dsv); /* before the game binds targets */
void motion_flush(void* ctx);                                          /* FinishCommandList, ClearState */
void motion_draw(void* ctx, unsigned count, unsigned inst, unsigned start, int base); /* after an indexed draw */
void* motion_view(unsigned w, unsigned h); /* ID3D11ShaderResourceView*: this frame's motion (referenced), or NULL */
void motion_report(void);
int effects_bound_ps_skip(void* ctx); /* the bound pixel shader redraws meshes for another purpose (no motion) */
uint64_t effects_bound_ps_hash(void* ctx); /* the bound pixel shader's bytecode hash, 0 not known */
int dxbc_bind_names(const uint8_t* code, size_t len, char* out, size_t cap); /* "name:kind@slot ..." from RDEF */
uint64_t fx_fnv64(const void* p, size_t n);
/* every pixel shader the game creates (GPU timing by shader, knock-out): bytecode hash and DXBC STAT size */
typedef struct PsInfo {
    uint64_t hash;
    int instr, tex, flow; /* instruction slots, texture instructions, dynamic branches */
    int outline;          /* draws the characters' ink outlines ("outlineColor" in its constant buffers) */
    int rim;              /* [Quality] RimLight: 1 a rim-free copy exists (norim), -1 could not be made, 0 no rim */
    void* norim;
    int inner;            /* [Quality] InnerOutlines (outline shaders): 1 a copy without inner lines exists (noinner), -1 not */
    void* noinner;
    int hatch_slot;       /* the t register of the toon shading's hatching texture, -1 none */
    int mat_slot[3];      /* the t registers of baseTexture, toonShadowColorTexture, toonParamsTexture (-1 none) */
    void* code;           /* the bytecode ([GpuProbe] ShaderTiming only) */
    size_t len;
} PsInfo;
/* dxbc.c: the bind slot of a constant buffer and a variable's byte offset in it (RDEF); a copy of the bytecode with
 * every read of cb<slot>[reg] reading 0 (NULL if none, or a form not understood), checksum rebuilt */
int dxbc_cbuffer_var(const uint8_t* code, size_t len, const char* cbuf, const char* var, int* slot, int* offset);
int dxbc_texture_slot(const uint8_t* code, size_t len, const char* name); /* its t register, -1 if none */
uint8_t* dxbc_zero_cb_reads(const uint8_t* code, size_t len, int slot, int reg, size_t* out_len, int* replaced);
/* the same, limited to instructions with this opcode (-1 any) and to reads of one component (0-3 x-w, -1 any),
 * reading value instead of 0 */
uint8_t* dxbc_patch_cb_reads(const uint8_t* code, size_t len, int slot, int reg, int opcode, int comp, float value,
                             size_t* out_len, int* replaced);
int fx_ps_id(const void* sh);       /* 1.. for a known pixel shader, 0 none */
const PsInfo* fx_ps_info(int id);
void fx_ps_counts(long* shaders, long* objects); /* distinct pixel shaders known, shader objects mapped */
void fx_dxbc_checksum(const uint8_t* dxbc, size_t size, uint32_t out[4]);
/* [Quality] OutlineDepthOffset: the rasterizer depth bias moving a draw at the distance dist (game units) k x dist
 * farther away, for a depth format and test and the camera's depth mapping (NDC = a + b / d); 0 if not known */
int fx_outline_bias(int fmt, int func, float a, float b, float k, float dist);
void shadowcull_depth_proj(float* a, float* b); /* the main camera's depth mapping (0, 0 until seen) */

/* ------------------------------------------------------------------ shadow culling (shadowcull.c) */
typedef struct ShadowSites {
    uint8_t *begin_site, *exec_site, *exec_a_site, *exec_b_site; /* call instructions we retarget */
    uint8_t *begin, *exec, *interp;  /* cascade_begin(frame, k), execute_buckets(frame), interpreter */
    uint8_t* vp_table;               /* light view-projection per frame slot and cascade */
    void *h_vb, *h_material, *h_world, *h_prev, *h_mat3, *h_ib, *h_draw; /* draw-block command handlers */
    int anim_ok;                     /* skinned-mesh bone palette layout confirmed */
    uint8_t* scene_site[8];          /* calls of the scene-pass command interpreters (modes 0, 2, 3) */
    int scene_mode[8];
    int nscene;
    uint8_t* scene_interp[4];        /* interpreter per mode (index = mode; [1] = shadows) */
    uint8_t* scene_tab;              /* modes 2/3: bucket array per frame slot (executor arg = slot) */
    uint8_t* scene_tab0;             /* mode 0: bucket per frame slot */
    int scene_ok;                    /* both tables found: a scene node's geometry is known exactly */
    float* char_lod;                 /* the game's character detail distance (cm), NULL if not found */
    uint8_t* shadow_far_site;        /* cascade setup: the instruction after the far distance is loaded (xmm12) */
    uint8_t* shadow_done_site;       /* cascade setup: the call that writes the frame's last light matrix */
    volatile int* frame_slot;        /* the frame-slot global */
    uint8_t* cam_base;               /* per-view, per-frame camera records (view-aware test) */
    volatile int* cam_view_idx;      /* view the camera update last wrote */
    int view_ok;                     /* camera record layout confirmed */
} ShadowSites;
int shadowcull_resolve(uint8_t* base, ShadowSites* s, int verbose);
int shadowcull_selftest(const ShadowSites* s);
void shadowcull_install(void);
void shadowcull_report(double seconds);
void shadowcull_query_result(int slot, int64_t samples);
int shadowcull_current_cascade(void);

/* ------------------------------------------------------------------ wait profile (waitprof.c) */
enum { WP_SLEEP, WP_EVENT, WP_CS, WP_YIELD, WP_COUNT };
enum { D3D_EXEC, D3D_MAP_WRITE, D3D_MAP_READ, D3D_UNMAP, D3D_UPDATE, D3D_COPY, D3D_GETDATA, D3D_FLUSH, D3D_END,
       D3D_COUNT };
void waitprof_init(void);
void waitprof_add(int cat, void* site, uint64_t ticks);
void waitprof_add_d3d(int method, void* site, uint64_t ticks); /* main thread inside a D3D call */
void waitprof_report(LONG64 frames, double cpu_ms, double frame_ms);
extern DWORD (WINAPI* o_WaitForSingleObject)(HANDLE, DWORD);
extern DWORD (WINAPI* o_WaitForSingleObjectEx)(HANDLE, DWORD, BOOL);
extern DWORD (WINAPI* o_SleepEx)(DWORD, BOOL);
extern void (WINAPI* o_EnterCriticalSection)(LPCRITICAL_SECTION);
DWORD WINAPI hk_WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WINAPI hk_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WINAPI hk_SleepEx(DWORD ms, BOOL alertable);
void WINAPI hk_EnterCriticalSection(LPCRITICAL_SECTION cs);

/* ------------------------------------------------------------------ telemetry (telemetry.c) */
void telemetry_start(void);
void telemetry_report(double seconds);

/* ------------------------------------------------------------------ sampler (sampler.c) */
void sampler_start(void);
void sampler_set_targets(const DWORD* tids, const char (*names)[72], int n);
void sampler_report(void);
int mcpu_is_own_thread(DWORD tid);
void mcpu_register_own_thread(DWORD tid);

/* ------------------------------------------------------------------ in-session A/B (ab.c) */
void ab_start(void);
const char* ab_window_tag(void);
void ab_after_window(void);

/* ------------------------------------------------------------------ profiler (profiler.c) */
void profiler_start(void);
extern volatile DWORD* g_engine_main_tid;

/* ------------------------------------------------------------------ core init (core.c) */
int mcpu_init(HMODULE self, const char* ini_override);
uint8_t* game_base(void);
size_t game_size(void);
const char* fmt_addr(void* p, char* buf, size_t cap);
