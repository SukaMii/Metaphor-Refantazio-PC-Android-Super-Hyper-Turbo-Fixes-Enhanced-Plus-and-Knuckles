#include "mcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config g_cfg;

void config_defaults(Config* c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = 1;
    c->profile_when_disabled = 1;
    c->log_level = 1;

    c->prof_enabled = 1;
    c->prof_interval_sec = 10;
    snprintf(c->output_folder, sizeof(c->output_folder), "MetaphorCPU_compare folder");
    c->prof_top = 16;
    c->frame_stats = 1;

    c->fix_timer_churn = 1;
    c->timer_period_ms = 1;
    c->precise_usleep = 0;

    /* the CPU settings validated on the tablet (presets J1 / AB_animcull); the PC profile
     * turns them off (config_profile_pc) */
    c->adaptive_yield = 1;
    c->yield_mode = 1;
    c->yield_spin_us = 50;
    c->yield_sleep_us = 100;
    c->yield_window_us = 30;

    c->replace_spinlock = 1;
    c->spin_iters = 32;
    c->spin_yields = 1;
    c->spin_wait_ms = 1;

    c->cs_spin_count = -1;

    c->affinity_mode = 1; /* blocks the game's hard pinning; thread placement left to the OS */
    c->allowed_cores = 0;
    c->main_cores_auto = 0;
    c->main_cores = 0;
    c->worker_cores = 0;
    c->detect_fast_cores = 0;
    c->fast_thread_names[0] = 0;

    c->main_priority = 99;
    c->worker_priority = 99;

    c->job_pool0 = 0;
    c->job_pool1 = 0;

    c->telemetry = 1;
    c->telemetry_sample_ms = 1000;
    c->limiter_fps = 0;
    c->limiter_margin_us = -1;
    c->sampler_hz = 0;
    c->sampler_threads = 3;
    c->ab_enabled = 0;
    c->ab_switch_sec = 60;
    c->fast_sem = 1; /* tablet J1: +2 FPS cold, +1 throttled, -8% energy per frame */
    c->sem_spin = -1; /* auto: 256 polls with 4+ cores, else 32 */
    c->gpu_probe = 0;
    c->shadow_cull = 1; /* PC-verified: 0 samples from any skipped draw */
    c->shadow_margin_pct = 1;
    c->shadow_verify = 0;
    c->shadow_skinned_pct = 10; /* skeleton bound, PC-verified: 0 pixels from any skipped draw */
    c->shadow_view = 0;
    c->shadow_view_depth_pct = 25;
    c->q_characters = 1;
    c->q_outlines = 1;
    c->q_char_shadows = 1;
    c->q_far_shadows = 1;
    c->q_shadows = 1;
    c->q_mip_bias = 0;
    c->q_report = 0;
    c->q_far_dist = 50;
    /* the bubble (tablet Q6, 21 Oct 2026: the game's simple NPC models beyond 30 m: GPU busy 93 -> 81%
     * at 607 MHz; 50 m: 84%) and far NPCs animated every 2nd frame (-3%). 60 m since 25 Oct 2026 (the
     * user found 30 m too close), stepping in to 40 m when the frame rate falls (AdaptiveLod); the PC
     * profile keeps the game's own */
    c->q_char_lod = 60;
    c->q_alod = 1;
    c->q_alod_min = 40;
    c->q_alod_down = 25, c->q_alod_down_sec = 3, c->q_alod_in_speed = 2;
    c->q_alod_up = 29, c->q_alod_up_sec = 10, c->q_alod_out_speed = 1;
    c->q_shadow_dist = 100; /* tablet Q8 (21 Oct 2026): GPU -4%, busy 89 -> 86% at 607 MHz; 60 m looked worse */
    c->q_tiny_px = 0;
    c->q_tiny_texels = 0;
    c->q_max_aniso = 0;
    c->q_far_world = 0;
    c->q_fog = 1;
    c->q_painterly = 1;
    c->q_outline_carried = 1;
    c->q_line_redraws = 4;
    c->q_line_ps[0] = 0xdfeac58cd1cc3a84ull; /* the ink line meshes of the clothes (Q35) */
    c->q_line_n = 1;
    /* glare off on Android (the user's choice, 23 Oct 2026: a faint glow at night lamps, tablet GPU
     * time -2.2% where it runs); on PC the game's */
    c->q_glare = 0;
    /* SMAA High on both (the user's choice, 26 Oct 2026: it also smooths diagonal edges, less shimmer;
     * tablet GPU time +0.35 ms against Medium, the default from 0.28.0 to 0.42.0) */
    c->q_smaa = 2;
    /* less shimmer on Android (the user's choice, 26 Oct 2026, from the PC compare shots Q25 and Q28): the
     * characters' ink outlines within 40 m (the transition is hardly noticeable; tablet GPU time about
     * -0.2 ms) and no toon rim light (a broken bright line of 1-2 px at every distance); the PC keeps both */
    c->q_outline_dist = 40;
    c->q_rim_dist = 0.001f;
    /* when CharacterMotionVectors is on: every character's limbs keep their own motion, on both (the user's choice,
     * 31 Oct 2026: limiting them to 20 m in a crowd saved nothing measurable on the tablet, Q52) */
    c->q_mv_dist = 40.0f;
    c->q_mv_skin_dist = 0;
    c->q_mv_crowd = 12;
    /* the world's stabilisation clamped to its 4 direct neighbours, on both (the user's choice, 29 Oct 2026: tablet
     * -0.3 ms a frame) */
    c->q_stab_world_taps = 4;
    /* simpler far meshes across seams on Android (the user's choice, 24 Oct 2026: tablet GPU time -7%;
     * stonework slightly smudged far away, acceptable on the tablet); PC: off until a PC A/B */
    c->q_farmesh = 2;
    c->q_fm_px = 0.5f;
    c->q_fm_floor = 10;
    /* shadow casters by 1 shadow-map texel (Q19 tablet, 24 Oct 2026: shadow maps 2.62 -> 2.14 ms, GPU time -2%;
     * Q18 PC: no visible change); only acts where FarMeshes is on (Android) */
    c->q_fm_shadow = 1.0f;
    /* seams only from 150 m (24 Oct 2026: across seams the block and column edges of nearer buildings
     * looked uneven; offline the far triangles left: Sunlumeo 63% vs 56% everywhere / 71% safe, desert
     * 39% vs 38% / 46%) */
    c->q_fm_seams_from = 150.0f;
    /* far shadow cascade every 2nd frame on both (tablet Q11, 23 Oct 2026: GPU -4% at 660 MHz; on PC
     * indistinguishable from every frame). Every 3rd (-5%) was the Android default in 0.26-0.27.0. */
    c->q_shadow_refresh = 2;
    c->q_far_chars = 1;
    c->q_far_anim = 2;
    c->q_far_mip = 0;
    c->wait_profile = 0;
}

/* PC (native Windows): the CPU-side fixes target costs of Wine on Android (expensive
 * Sleep(0), event round trips through the wineserver, timer calls); Windows does not
 * have them, and without them the game's own CPU behaviour is kept exactly. The GPU-side
 * fixes (shadow culling) help both and stay on. */
static void config_profile_pc(Config* c)
{
    c->fix_timer_churn = 0;
    c->precise_usleep = 0;
    c->adaptive_yield = 0;
    c->replace_spinlock = 0;
    c->affinity_mode = 0;
    c->main_cores_auto = 0;
    c->detect_fast_cores = 0;
    c->fast_thread_names[0] = 0;
    c->main_priority = 99;
    c->worker_priority = 99;
    c->fast_sem = 0;
    c->cs_spin_count = -1;
    c->allowed_cores = 0;
    c->main_cores = 0;
    c->worker_cores = 0;
    c->job_pool0 = 0;
    c->job_pool1 = 0;
}

/* Baseline run: every behavioural change off, measurement on. */
void config_neutral(Config* c)
{
    c->fix_timer_churn = 0;
    c->precise_usleep = 0;
    c->adaptive_yield = 0;
    c->replace_spinlock = 0;
    c->cs_spin_count = -1;
    c->affinity_mode = 0;
    c->allowed_cores = 0;
    c->main_cores_auto = 0;
    c->main_cores = 0;
    c->worker_cores = 0;
    c->detect_fast_cores = 0;
    c->fast_thread_names[0] = 0;
    c->main_priority = 99;
    c->worker_priority = 99;
    c->job_pool0 = 0;
    c->job_pool1 = 0;
    c->limiter_fps = 0;
    c->fast_sem = 0;
    c->shadow_cull = 0; /* Verify stays: it measures without changing what is drawn */
    c->q_char_lod = 0;
    c->q_alod = 0;
    c->q_far_anim = 1;
    c->q_far_mip = 0;
    c->q_shadow_dist = 0;
    c->q_shadow_refresh = 1;
    c->q_fog = 1;
    c->q_glare = 1;
    c->q_smaa = 3;
    c->q_painterly = 1;
    c->q_outline_dist = 0;
    c->q_rim_dist = 0;
    c->q_inner_dist = 0;
    c->q_char_tex_q = 0;
    c->q_ol_depth = 0;
    c->q_hatch = 0;
    c->q_line_mode = 0;
    c->q_stab = 0;
    c->q_stab_world = 0;
    c->q_mv = 0;
    c->q_world_tex_q = 0;
    c->q_farmesh = 0;
    if (c->shadow_view == 2)
        c->shadow_view = 1; /* measuring stays */
}

/* [Quality] key = off / on (`on`) / a percentage 2..95 (above: 95) -> a share 0..0.95; auto or empty keeps `def` */
static float ini_share(const char* ini, const char* key, float def, float on)
{
    char ts[16];
    GetPrivateProfileStringA("Quality", key, "", ts, sizeof(ts), ini);
    if (!_stricmp(ts, "on") || !_stricmp(ts, "yes") || !strcmp(ts, "1"))
        return on;
    if (!_stricmp(ts, "off") || !_stricmp(ts, "no") || !strcmp(ts, "0"))
        return 0;
    if (!ts[0] || !_stricmp(ts, "auto"))
        return def;
    int pc = atoi(ts);
    return pc > 1 && pc <= 95 ? pc / 100.0f : pc > 95 ? 0.95f : 0;
}

static int ini_int(const char* ini, const char* sec, const char* key, int def)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), ini);
    if (!buf[0])
        return def;
    return (int)strtol(buf, NULL, 0);
}

/* an effect that is on, off or on only within a distance: on / 1 / yes = the game's (0), off / 0 / no = none
 * (0.001), a number above 1 = metres, auto or empty = the profile's default */
static float ini_on_off_dist(const char* v, float def)
{
    if (!v[0] || !_stricmp(v, "auto"))
        return def;
    if (!_stricmp(v, "on") || !_stricmp(v, "yes") || !_stricmp(v, "true"))
        return 0;
    if (!_stricmp(v, "off") || !_stricmp(v, "no") || !_stricmp(v, "false"))
        return 0.001f;
    char* end = NULL;
    double d = strtod(v, &end);
    if (end == v || !(d >= 0) || d > 1000.0)
        return def;
    return d == 0 ? 0.001f : d <= 1.0 ? 0 : (float)d;
}

static float ini_float(const char* ini, const char* sec, const char* key, float def)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), ini);
    if (!buf[0])
        return def;
    return (float)strtod(buf, NULL);
}

/* a number, or empty / "auto" for the profile's default */
static float ini_float_auto(const char* ini, const char* sec, const char* key, float def)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), ini);
    if (!buf[0] || _stricmp(buf, "auto") == 0)
        return def;
    char* end = NULL;
    double d = strtod(buf, &end);
    return end == buf ? def : (float)d;
}

/* a number, or empty / "auto" for the profile's default */
static int ini_int_auto(const char* ini, const char* sec, const char* key, int def)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), ini);
    if (!buf[0] || _stricmp(buf, "auto") == 0)
        return def;
    return (int)strtol(buf, NULL, 0);
}

static uint64_t ini_mask(const char* ini, const char* sec, const char* key, uint64_t def, int* is_auto)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), ini);
    if (is_auto)
        *is_auto = 0;
    if (!buf[0])
        return def;
    if (_stricmp(buf, "auto") == 0) {
        if (is_auto)
            *is_auto = 1;
        return 0;
    }
    return strtoull(buf, NULL, 0);
}

void config_load(Config* c, const char* ini)
{
    /* profile first: its defaults apply even without an INI, keys below override them */
    int have_ini = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    char prof[16] = "auto";
    if (have_ini)
        GetPrivateProfileStringA("General", "Profile", "auto", prof, sizeof(prof), ini);
    c->profile_auto = _stricmp(prof, "pc") != 0 && _stricmp(prof, "android") != 0;
    c->profile = _stricmp(prof, "pc") == 0 ? PROFILE_PC
                 : _stricmp(prof, "android") == 0 ? PROFILE_ANDROID
                 : (g_is_wine ? PROFILE_ANDROID : PROFILE_PC);
    if (c->profile == PROFILE_PC) {
        config_profile_pc(c);
        c->q_char_lod = 0; /* [Quality] bubble: the game's own on PC unless the INI sets it */
        c->q_alod = 0;
        c->q_far_anim = 1;
        c->q_shadow_dist = 0;
        c->q_glare = 1;
        c->q_farmesh = 0;
        c->q_outline_dist = 0;
        c->q_rim_dist = 0;
    }
    if (!have_ini) { /* keep defaults */
        c->q_rim_hooks = c->q_rim_dist > 0;
        c->q_outline_hooks = c->q_outline_dist > 0 || c->q_rim_hooks;
        c->q_fx_active = !c->q_fog || !c->q_glare || c->q_smaa != 3 || c->q_outline_hooks;
        c->q_active = c->q_char_lod || c->q_far_anim > 1 || c->q_shadow_dist || c->q_shadow_refresh > 1 ||
                      c->q_fx_active || c->q_farmesh;
        return;
    }

    c->enabled = ini_int(ini, "General", "Enabled", c->enabled);
    c->profile_when_disabled = ini_int(ini, "General", "ProfileWhenDisabled", c->profile_when_disabled);
    c->log_level = ini_int(ini, "General", "LogLevel", c->log_level);

    c->prof_enabled = ini_int(ini, "Profiler", "Enabled", c->prof_enabled);
    c->prof_interval_sec = ini_int(ini, "Profiler", "IntervalSec", c->prof_interval_sec);
    c->prof_top = ini_int(ini, "Profiler", "TopThreads", c->prof_top);
    c->frame_stats = ini_int(ini, "Profiler", "FrameStats", c->frame_stats);
    GetPrivateProfileStringA("General", "LogCopy", "", c->log_copy, sizeof(c->log_copy), ini);

    c->fix_timer_churn = ini_int(ini, "Timer", "FixTimerChurn", c->fix_timer_churn);
    c->timer_period_ms = ini_int(ini, "Timer", "TimerPeriodMs", c->timer_period_ms);
    c->precise_usleep = ini_int(ini, "Timer", "PreciseUSleep", c->precise_usleep);

    c->adaptive_yield = ini_int(ini, "Yield", "AdaptiveYield", c->adaptive_yield);
    c->yield_mode = ini_int(ini, "Yield", "YieldMode", c->yield_mode);
    {
        char probe[16];
        GetPrivateProfileStringA("Yield", "YieldSpinUs", "", probe, sizeof(probe), ini);
        if (probe[0] && _stricmp(probe, "auto") != 0)
            c->yield_spin_us = (int)strtol(probe, NULL, 0);
    }
    c->yield_sleep_us = ini_int(ini, "Yield", "YieldSleepUs", c->yield_sleep_us);
    c->yield_window_us = ini_int(ini, "Yield", "YieldWindowUs", c->yield_window_us);

    c->replace_spinlock = ini_int(ini, "SpinLock", "ReplaceSpinLock", c->replace_spinlock);
    c->spin_iters = ini_int(ini, "SpinLock", "SpinIterations", c->spin_iters);
    c->spin_yields = ini_int(ini, "SpinLock", "YieldIterations", c->spin_yields);
    c->spin_wait_ms = ini_int(ini, "SpinLock", "WaitTimeoutMs", c->spin_wait_ms);

    c->cs_spin_count = ini_int(ini, "CriticalSection", "SpinCount", c->cs_spin_count);

    c->affinity_mode = ini_int(ini, "Affinity", "Mode", c->affinity_mode);
    c->allowed_cores = ini_mask(ini, "Affinity", "AllowedCores", c->allowed_cores, NULL);
    {
        int is_auto = 0;
        char probe[8];
        GetPrivateProfileStringA("Affinity", "MainThreadCores", "", probe, sizeof(probe), ini);
        if (probe[0]) {
            c->main_cores = ini_mask(ini, "Affinity", "MainThreadCores", 0, &is_auto);
            c->main_cores_auto = is_auto;
        }
    }
    c->worker_cores = ini_mask(ini, "Affinity", "WorkerCores", c->worker_cores, NULL);
    c->detect_fast_cores = ini_int(ini, "Affinity", "DetectFastCores", c->detect_fast_cores);
    GetPrivateProfileStringA("Affinity", "FastThreadNames", c->fast_thread_names,
                             c->fast_thread_names, sizeof(c->fast_thread_names), ini);

    c->main_priority = ini_int(ini, "Priority", "MainThread", c->main_priority);
    c->worker_priority = ini_int(ini, "Priority", "JobWorkers", c->worker_priority);

    c->job_pool0 = ini_int(ini, "Jobs", "WorkerPool0", c->job_pool0);
    c->job_pool1 = ini_int(ini, "Jobs", "WorkerPool1", c->job_pool1);

    c->telemetry = ini_int(ini, "Telemetry", "Enabled", c->telemetry);
    c->telemetry_sample_ms = ini_int(ini, "Telemetry", "SampleMs", c->telemetry_sample_ms);
    c->limiter_fps = ini_int(ini, "FrameLimiter", "Fps", c->limiter_fps);
    c->limiter_margin_us = ini_int(ini, "FrameLimiter", "SpinMarginUs", c->limiter_margin_us);
    c->sampler_hz = ini_int(ini, "Sampler", "Hz", c->sampler_hz);
    c->sampler_threads = ini_int(ini, "Sampler", "Threads", c->sampler_threads);
    c->fast_sem = ini_int(ini, "JobSync", "FastSemaphore", c->fast_sem);
    {
        char probe[16];
        GetPrivateProfileStringA("JobSync", "SpinCount", "", probe, sizeof(probe), ini);
        if (probe[0] && _stricmp(probe, "auto") != 0)
            c->sem_spin = (int)strtol(probe, NULL, 0);
    }
    if (c->sem_spin < -1) c->sem_spin = -1;
    if (c->sem_spin > 10000) c->sem_spin = 10000;
    c->gpu_probe = ini_int(ini, "GpuProbe", "Enabled", c->gpu_probe);
    c->gpu_timing = ini_int(ini, "GpuProbe", "Timing", c->gpu_timing);
    c->gpu_passes = ini_int(ini, "GpuProbe", "Passes", c->gpu_passes);
    c->shadow_cull = ini_int(ini, "ShadowCull", "Enabled", c->shadow_cull);
    c->shadow_margin_pct = ini_int(ini, "ShadowCull", "MarginPercent", c->shadow_margin_pct);
    c->shadow_verify = ini_int(ini, "ShadowCull", "Verify", c->shadow_verify);
    c->shadow_skinned_pct = ini_int(ini, "ShadowCull", "AnimatedMarginPercent", c->shadow_skinned_pct);
    c->shadow_view = ini_int(ini, "ShadowCull", "ViewCull", c->shadow_view);
    c->shadow_view_depth_pct = ini_int(ini, "ShadowCull", "ViewCullDepthMarginPercent", c->shadow_view_depth_pct);
    if (c->shadow_view < 0 || c->shadow_view > 2) c->shadow_view = 0;
    if (c->shadow_view_depth_pct < 0) c->shadow_view_depth_pct = 0;
    if (c->shadow_view_depth_pct > 200) c->shadow_view_depth_pct = 200;
    if (c->shadow_margin_pct < 0) c->shadow_margin_pct = 0;
    if (c->shadow_margin_pct > 50) c->shadow_margin_pct = 50;
    if (c->shadow_skinned_pct < -1) c->shadow_skinned_pct = -1;
    if (c->shadow_skinned_pct > 1000) c->shadow_skinned_pct = 1000;
    c->wait_profile = ini_int(ini, "WaitProfile", "Enabled", c->wait_profile);
    c->q_characters = ini_int(ini, "Quality", "Characters", c->q_characters) ? 1 : 0;
    /* diagnostic: hide characters only in these interpreter modes (bit 0 = mode 0, bit 2, bit 3) */
    c->q_char_hide = c->q_characters ? 0 : 0x0D;
    {
        char hm[16];
        GetPrivateProfileStringA("Quality", "CharacterHideModes", "", hm, sizeof(hm), ini);
        if (hm[0])
            c->q_char_hide = (int)strtol(hm, NULL, 0) & 0x0D;
    }
    c->q_outlines = ini_int(ini, "Quality", "Outlines", c->q_outlines) ? 1 : 0;
    c->q_char_shadows = ini_int(ini, "Quality", "CharacterShadows", c->q_char_shadows) ? 1 : 0;
    c->q_far_shadows = ini_int(ini, "Quality", "FarShadows", c->q_far_shadows) ? 1 : 0;
    c->q_shadows = ini_int(ini, "Quality", "Shadows", c->q_shadows) ? 1 : 0;
    c->q_mip_bias = ini_int(ini, "Quality", "TextureMipBias", c->q_mip_bias);
    if (c->q_mip_bias < 0) c->q_mip_bias = 0;
    if (c->q_mip_bias > 4) c->q_mip_bias = 4;
    c->q_report = ini_int(ini, "Quality", "Report", c->q_report);
    c->q_far_dist = ini_int(ini, "Quality", "FarDistance", c->q_far_dist);
    if (c->q_far_dist < 5) c->q_far_dist = 5;
    if (c->q_far_dist > 500) c->q_far_dist = 500;
    c->q_far_chars = ini_int(ini, "Quality", "FarCharacters", c->q_far_chars) ? 1 : 0;
    c->q_far_anim = ini_int_auto(ini, "Quality", "FarCharacterAnimation", c->q_far_anim);
    if (c->q_far_anim < 1) c->q_far_anim = 1;
    if (c->q_far_anim > 8) c->q_far_anim = 8;
    c->q_char_lod = ini_int_auto(ini, "Quality", "CharacterLodDistance", c->q_char_lod);
    if (c->q_char_lod < 0) c->q_char_lod = 0;
    if (c->q_char_lod > 0 && c->q_char_lod < 5) c->q_char_lod = 5;
    if (c->q_char_lod > 200) c->q_char_lod = 200;
    c->q_alod = ini_int_auto(ini, "Quality", "AdaptiveLod", c->q_alod) ? 1 : 0;
    c->q_alod_min = ini_int(ini, "Quality", "AdaptiveLodMin", c->q_alod_min);
    if (c->q_alod_min < 5) c->q_alod_min = 5;
    c->q_alod_down = ini_float(ini, "Quality", "AdaptiveLodDownFps", c->q_alod_down);
    c->q_alod_up = ini_float(ini, "Quality", "AdaptiveLodUpFps", c->q_alod_up);
    if (!(c->q_alod_up > c->q_alod_down)) c->q_alod_up = c->q_alod_down + 4;
    c->q_alod_down_sec = ini_int(ini, "Quality", "AdaptiveLodDownSec", c->q_alod_down_sec);
    c->q_alod_up_sec = ini_int(ini, "Quality", "AdaptiveLodUpSec", c->q_alod_up_sec);
    if (c->q_alod_down_sec < 1 || c->q_alod_down_sec > 60) c->q_alod_down_sec = 3;
    if (c->q_alod_up_sec < 1 || c->q_alod_up_sec > 60) c->q_alod_up_sec = 10;
    c->q_alod_in_speed = ini_float(ini, "Quality", "AdaptiveLodInSpeed", c->q_alod_in_speed);
    c->q_alod_out_speed = ini_float(ini, "Quality", "AdaptiveLodOutSpeed", c->q_alod_out_speed);
    if (!(c->q_alod_in_speed > 0) || c->q_alod_in_speed > 50) c->q_alod_in_speed = 2;
    if (!(c->q_alod_out_speed > 0) || c->q_alod_out_speed > 50) c->q_alod_out_speed = 1;
    c->q_shadow_dist = ini_int_auto(ini, "Quality", "ShadowDistance", c->q_shadow_dist);
    if (c->q_shadow_dist < 0) c->q_shadow_dist = 0;
    if (c->q_shadow_dist > 0 && c->q_shadow_dist < 20) c->q_shadow_dist = 20;
    c->q_tiny_px = ini_float(ini, "Quality", "TinyObjectPixels", c->q_tiny_px);
    if (!(c->q_tiny_px >= 0) || c->q_tiny_px > 16) c->q_tiny_px = 0;
    c->q_tiny_texels = ini_float(ini, "Quality", "TinyCasterTexels", c->q_tiny_texels);
    if (!(c->q_tiny_texels >= 0) || c->q_tiny_texels > 16) c->q_tiny_texels = 0;
    c->q_max_aniso = ini_int(ini, "Quality", "MaxAnisotropy", c->q_max_aniso);
    if (c->q_max_aniso < 0 || c->q_max_aniso > 16) c->q_max_aniso = 0;
    c->q_far_world = ini_int(ini, "Quality", "FarWorldHidden", c->q_far_world);
    c->q_shadow_refresh = ini_int_auto(ini, "Quality", "FarShadowRefresh", c->q_shadow_refresh);
    if (c->q_shadow_refresh < 1) c->q_shadow_refresh = 1;
    if (c->q_shadow_refresh > 4) c->q_shadow_refresh = 4;
    c->capture = ini_int(ini, "GpuProbe", "Capture", c->capture);
    c->compare_shots = ini_int(ini, "GpuProbe", "CompareShots", c->compare_shots);
    c->mesh_census = ini_int(ini, "GpuProbe", "MeshCensus", c->mesh_census);
    c->occl_census = ini_int(ini, "GpuProbe", "OcclusionCensus", c->occl_census) ? 1 : 0;
    c->occl_hooks = c->occl_census;
    c->shader_timing = ini_int(ini, "GpuProbe", "ShaderTiming", c->shader_timing) ? 1 : 0;
    if (c->shader_timing)
        c->gpu_timing = 1; /* the marks are the pass timer's */
    {
        char kl[256];
        GetPrivateProfileStringA("GpuProbe", "KnockoutShaders", "", kl, sizeof(kl), ini);
        c->q_knock_n = 0;
        for (char* p = kl; *p && c->q_knock_n < 6;) {
            char* end;
            unsigned long long h = strtoull(p, &end, 16);
            if (end == p)
                break;
            if (h)
                c->q_knock_list[c->q_knock_n++] = h;
            p = end;
            while (*p == ',' || *p == ' ')
                p++;
        }
    }
    GetPrivateProfileStringA("GpuProbe", "OutputFolder", c->output_folder, c->output_folder, sizeof(c->output_folder),
                             ini);
    {
        char fm[16];
        GetPrivateProfileStringA("Quality", "FarMeshes", "", fm, sizeof(fm), ini);
        if (!_stricmp(fm, "safe") || !strcmp(fm, "1"))
            c->q_farmesh = 1;
        else if (!_stricmp(fm, "seams") || !strcmp(fm, "2"))
            c->q_farmesh = 2;
        else if (!_stricmp(fm, "off") || !strcmp(fm, "0"))
            c->q_farmesh = 0;
        /* empty or auto: the profile's */
    }
    c->q_fm_px = ini_float(ini, "Quality", "FarMeshPixels", c->q_fm_px);
    if (!(c->q_fm_px > 0.05f) || c->q_fm_px > 4.0f) c->q_fm_px = 0.5f;
    c->q_fm_floor = ini_int(ini, "Quality", "FarMeshMinPercent", c->q_fm_floor);
    c->q_fm_shadow = ini_float(ini, "Quality", "FarMeshShadowTexels", c->q_fm_shadow);
    c->q_fm_seams_from = ini_float(ini, "Quality", "FarMeshSeamsFrom", c->q_fm_seams_from);
    {
        char od[16];
        GetPrivateProfileStringA("Quality", "OutlineDistance", "", od, sizeof(od), ini);
        if (!_stricmp(od, "all"))
            c->q_outline_dist = 0.001f; /* every outline left out (measurement) */
        else if (!_stricmp(od, "off") || !_stricmp(od, "on")) /* no distance: every outline drawn, as the game */
            c->q_outline_dist = 0;
        else if (od[0] && _stricmp(od, "auto"))
            c->q_outline_dist = (float)strtod(od, NULL);
        if (!(c->q_outline_dist >= 0) || c->q_outline_dist > 1000.0f)
            c->q_outline_dist = 0;
    }
    if (!(c->q_fm_seams_from >= 0) || c->q_fm_seams_from > 1000.0f) c->q_fm_seams_from = 150.0f;
    if (!(c->q_fm_shadow >= 0) || c->q_fm_shadow > 4.0f) c->q_fm_shadow = 0;
    if (c->q_fm_floor < 1 || c->q_fm_floor > 90) c->q_fm_floor = 10;
    if (c->mesh_census)
        c->q_report = 1; /* the scene objects mark their draws (class, distance) */
    c->q_fog = ini_int(ini, "Quality", "VolumetricFog", c->q_fog) ? 1 : 0;
    c->q_painterly = ini_int(ini, "Quality", "Painterly", c->q_painterly) ? 1 : 0;
    c->q_outline_carried = ini_int(ini, "Quality", "OutlineCarried", c->q_outline_carried) ? 1 : 0;
    {
        char rl[16];
        GetPrivateProfileStringA("Quality", "RimLight", "", rl, sizeof(rl), ini);
        c->q_rim_dist = ini_on_off_dist(rl, c->q_rim_dist);
    }
    {
        char il[16];
        GetPrivateProfileStringA("Quality", "InnerOutlines", "", il, sizeof(il), ini);
        c->q_inner_dist = ini_on_off_dist(il, c->q_inner_dist);
    }
    {
        char lf[16];
        GetPrivateProfileStringA("Quality", "LineFix", "", lf, sizeof(lf), ini);
        if (!_stricmp(lf, "redraw")) /* 4 copies: continuous in every direction (fx_test) */
            c->q_line_mode = 1, c->q_line_redraws = 4;
        else if (!_stricmp(lf, "redraw2")) /* 2 copies (right, down): fewer gaps, cheaper */
            c->q_line_mode = 1, c->q_line_redraws = 2;
        else if (!_stricmp(lf, "thick")) /* 8 copies, a third and two thirds of a pixel: bolder, about 2 px */
            c->q_line_mode = 1, c->q_line_redraws = 8;
        else if (!_stricmp(lf, "conservative"))
            c->q_line_mode = 2;
        else if (!_stricmp(lf, "hide"))
            c->q_line_mode = 3;
        else if (!_stricmp(lf, "none") || !_stricmp(lf, "off") || !strcmp(lf, "0"))
            c->q_line_mode = 0;
        c->q_line_dist = ini_float(ini, "Quality", "LineDistance", c->q_line_dist);
        if (!(c->q_line_dist >= 0) || c->q_line_dist > 1000.0f)
            c->q_line_dist = 0;
        char ls[200];
        GetPrivateProfileStringA("Quality", "LineShaders", "", ls, sizeof(ls), ini);
        if (ls[0]) {
            c->q_line_n = 0;
            for (char* p = ls; *p && c->q_line_n < 8;) {
                while (*p == ' ' || *p == ',' || *p == ';')
                    p++;
                char* end = NULL;
                unsigned long long h = strtoull(p, &end, 16);
                if (end == p)
                    break;
                if (h)
                    c->q_line_ps[c->q_line_n++] = h;
                p = end;
            }
        }
    }
    {
        char ht[16];
        GetPrivateProfileStringA("Quality", "Hatching", "", ht, sizeof(ht), ini);
        if (!_stricmp(ht, "flat"))
            c->q_hatch = 99; /* the average tone, no strokes */
        else if (!_stricmp(ht, "on") || !strcmp(ht, "0"))
            c->q_hatch = 0;
        else if (ht[0] && _stricmp(ht, "auto")) {
            int lv = atoi(ht);
            c->q_hatch = lv > 0 && lv <= 12 ? lv : 0; /* strokes from this many mip levels lower: softer */
        }
    }
    /* SteadyImage (the user's wish, 29 Oct 2026: one switch): the characters at 40%, the rest at 50%, the characters'
     * motion vectors and SMAA Medium (the history smooths the diagonal edges High was chosen for; tablet -0.33 ms).
     * Off on both until the user decides. Set before the parts' own keys, which win when set (auto / empty follow it) */
    {
        char st[16];
        GetPrivateProfileStringA("Quality", "SteadyImage", "", st, sizeof(st), ini);
        if (!_stricmp(st, "on") || !_stricmp(st, "yes") || !strcmp(st, "1"))
            c->q_steady = 1;
        else if (!_stricmp(st, "off") || !_stricmp(st, "no") || !strcmp(st, "0"))
            c->q_steady = 0;
        if (c->q_steady)
            c->q_stab = 0.4f, c->q_stab_world = 0.5f, c->q_mv = 1, c->q_smaa = 1;
    }
    /* TemporalStabilise (the characters; on = 40%, the user's choice in Q39, 28 Oct 2026: steadier, little smearing) and
     * TemporalStabiliseWorld (every other pixel, reprojected exactly; on = 50%): off / on / the history's share in % */
    c->q_stab =ini_share(ini, "TemporalStabilise", c->q_stab, 0.4f);
    c->q_stab_world = ini_share(ini, "TemporalStabiliseWorld", c->q_stab_world, 0.5f);
    {
        char mv[16];
        GetPrivateProfileStringA("Quality", "CharacterMotionVectors", "", mv, sizeof(mv), ini);
        if (!_stricmp(mv, "on") || !_stricmp(mv, "yes") || !strcmp(mv, "1"))
            c->q_mv = 1;
        else if (!_stricmp(mv, "off") || !_stricmp(mv, "no") || !strcmp(mv, "0"))
            c->q_mv = 0;
    }
    c->q_mv_dist = ini_float(ini, "Quality", "MotionVectorDistance", c->q_mv_dist > 0 ? c->q_mv_dist : 40.0f);
    if (!(c->q_mv_dist >= 0) || c->q_mv_dist > 1000.0f)
        c->q_mv_dist = 40.0f;
    /* the skinned vertex buffers are copied whole every frame (tablet log 31: about 49 a frame, part of the +1.5 ms);
     * auto = 0 (all), a number limits them */
    {
        float def = c->q_mv_skin_dist;
        c->q_mv_skin_dist = ini_float_auto(ini, "Quality", "MotionVectorSkinDistance", def);
        if (!(c->q_mv_skin_dist >= 0) || c->q_mv_skin_dist > 1000.0f)
            c->q_mv_skin_dist = def;
    }
    /* a fight has the party and a few enemies: the user's figure, 29 Oct 2026 */
    c->q_mv_crowd = ini_int(ini, "Quality", "MotionVectorSkinCrowd", c->q_mv_crowd > 0 ? c->q_mv_crowd : 12);
    if (c->q_mv_crowd < 0 || c->q_mv_crowd > 1000)
        c->q_mv_crowd = 12;
    /* 4 by default: -0.3 ms a frame on the tablet (Q49), no smear seen on PC (Q48) */
    c->q_stab_world_taps = ini_int(ini, "Quality", "TemporalStabiliseWorldTaps", c->q_stab_world_taps ? c->q_stab_world_taps : 4) == 8 ? 8 : 4;
    {
        float b = ini_float(ini, "Quality", "WorldTextureBias", 0.25f * (float)c->q_world_tex_q);
        c->q_world_tex_q = b > 0 && b <= 2.0f ? (int)(b * 4.0f + 0.5f) : 0; /* quarter mip levels, up to 2 levels */
    }
    c->compute_census = ini_int(ini, "GpuProbe", "ComputeCensus", 0) ? 1 : 0;
    c->q_mv_view = ini_int(ini, "GpuProbe", "MotionVectorView", 0) ? 1 : 0;
    {
        float od = ini_float(ini, "Quality", "OutlineDepthOffset", 100.0f * c->q_ol_depth); /* percent of the distance */
        c->q_ol_depth = od > 0 && od <= 5.0f ? od / 100.0f : 0;
    }
    {
        float b = ini_float(ini, "Quality", "CharacterTextureBias", 0.25f * (float)c->q_char_tex_q);
        c->q_char_tex_q = b > 0 && b <= 2.0f ? (int)(b * 4.0f + 0.5f) : 0; /* quarter mip levels, up to 2 levels */
    }
    c->q_glare = ini_int_auto(ini, "Quality", "Glare", c->q_glare) ? 1 : 0;
    { /* auto or empty: High, Medium with SteadyImage */
        char sm[16];
        GetPrivateProfileStringA("Quality", "Smaa", "", sm, sizeof(sm), ini);
        if (!_stricmp(sm, "high"))
            c->q_smaa = 2;
        else if (!_stricmp(sm, "medium"))
            c->q_smaa = 1;
        else if (!_stricmp(sm, "off") || !strcmp(sm, "0"))
            c->q_smaa = 0;
        else if (!_stricmp(sm, "ultra"))
            c->q_smaa = 3;
    }
    c->capture_after_sec = ini_int(ini, "GpuProbe", "CaptureAfterSec", c->capture_after_sec);
    c->capture_full = ini_int(ini, "GpuProbe", "CaptureFullSize", c->capture_full) ? 1 : 0;
    c->capture_steps = ini_int(ini, "GpuProbe", "CaptureDrawSteps", c->capture_steps) ? 1 : 0;
    c->dump_tex = ini_int(ini, "GpuProbe", "DumpCharacterTextures", c->dump_tex) ? 1 : 0;
    c->dump_dist = ini_float(ini, "GpuProbe", "DumpDistance", c->dump_dist > 0 ? c->dump_dist : 8.0f);
    if (!(c->dump_dist > 0) || c->dump_dist > 1000.0f)
        c->dump_dist = 8.0f;
    if (c->q_far_world < 0) c->q_far_world = 0;
    c->q_far_mip = ini_int(ini, "Quality", "FarTextureMipBias", c->q_far_mip);
    if (c->q_far_mip < 0) c->q_far_mip = 0;
    if (c->q_far_mip > 4) c->q_far_mip = 4;
    if (c->shadow_verify)
        c->gpu_probe = 1; /* the occlusion queries go through the probe's context hooks */
    c->ab_enabled = ini_int(ini, "ABTest", "Enabled", c->ab_enabled);
    c->ab_switch_sec = ini_int(ini, "ABTest", "SwitchSec", c->ab_switch_sec);
    c->ab_start_after_sec = ini_int(ini, "ABTest", "StartAfterSec", c->ab_start_after_sec);
    {
        char cmp[32];
        GetPrivateProfileStringA("ABTest", "Compare", "all", cmp, sizeof(cmp), ini);
        c->ab_compare = _stricmp(cmp, "shadowcull") == 0      ? AB_SHADOWCULL
                        : _stricmp(cmp, "preciseusleep") == 0 ? AB_PRECISE_USLEEP
                        : _stricmp(cmp, "animcull") == 0      ? AB_ANIMCULL
                        : _stricmp(cmp, "cpu") == 0           ? AB_CPU
                        : _stricmp(cmp, "quality") == 0       ? AB_QUALITY
                        : _stricmp(cmp, "charmodes") == 0     ? AB_CHARMODES
                        : _stricmp(cmp, "far") == 0           ? AB_FAR
                        : _stricmp(cmp, "bubble") == 0        ? AB_BUBBLE
                        : _stricmp(cmp, "gpu") == 0           ? AB_GPU
                        : _stricmp(cmp, "shadowrefresh") == 0 ? AB_SHADOWREFRESH
                        : _stricmp(cmp, "effects") == 0       ? AB_EFFECTS
                        : _stricmp(cmp, "farmesh") == 0       ? AB_FARMESH
                        : _stricmp(cmp, "fmshadow") == 0      ? AB_FMSHADOW
                        : _stricmp(cmp, "overall") == 0       ? AB_OVERALL
                        : _stricmp(cmp, "occlusion") == 0     ? AB_OCCLUSION
                        : _stricmp(cmp, "knockout") == 0      ? AB_KNOCKOUT
                        : _stricmp(cmp, "outlines") == 0      ? AB_OUTLINES
                        : _stricmp(cmp, "look") == 0          ? AB_LOOK
                        : _stricmp(cmp, "lines") == 0         ? AB_LINES
                        : _stricmp(cmp, "stabilise") == 0     ? AB_STAB
                        : _stricmp(cmp, "smaa") == 0          ? AB_SMAA
                        : _stricmp(cmp, "motion") == 0        ? AB_MOTION
                        : _stricmp(cmp, "savings") == 0       ? AB_SAVINGS
                        : _stricmp(cmp, "features") == 0      ? AB_FEATURES
                                                              : AB_ALL;
    }
    /* KnockoutShaders without the cycle: the first one left out all session (to see what it draws) */
    c->q_knock = c->q_knock_n && !(c->ab_enabled && c->ab_compare == AB_KNOCKOUT) ? c->q_knock_list[0] : 0;
    if (c->telemetry_sample_ms < 200) c->telemetry_sample_ms = 200;
    if (c->limiter_fps < 0 || c->limiter_fps > 240) c->limiter_fps = 0;
    if (c->sampler_hz < 0 || c->sampler_hz > 1000) c->sampler_hz = 0;
    if (c->sampler_threads < 0) c->sampler_threads = 0;
    if (c->sampler_threads > 5) c->sampler_threads = 5;

    /* clamp to sane values */
    if (c->prof_interval_sec < 1) c->prof_interval_sec = 1;
    if (c->yield_spin_us < -1) c->yield_spin_us = -1;
    if (c->yield_sleep_us < 1) c->yield_sleep_us = 1;
    if (c->yield_sleep_us > 2000) c->yield_sleep_us = 2000;
    if (c->spin_iters < 1) c->spin_iters = 1;
    if (c->spin_wait_ms < 1) c->spin_wait_ms = 1;
    if (c->job_pool0 < 0 || c->job_pool0 > 32) c->job_pool0 = 0;
    if (c->job_pool1 < 0 || c->job_pool1 > 32) c->job_pool1 = 0;
    /* PC: the CPU-side keys are ignored even when the INI sets them (e.g. a tablet preset
     * copied to the PC: tuned for Wine, they slow native Windows down). To try them on a
     * PC anyway, set [General] Profile=android. */
    /* the quality hooks: installed when any option is changed, statistics asked for, or the
     * quality cycle runs */
    c->q_active = c->q_char_hide || !c->q_outlines || !c->q_char_shadows || !c->q_far_shadows || !c->q_shadows ||
                  c->q_mip_bias || c->q_report || !c->q_far_chars || c->q_far_anim > 1 || c->q_far_mip ||
                  c->q_char_lod || c->q_shadow_dist || c->q_tiny_px > 0 || c->q_tiny_texels > 0 || c->q_max_aniso ||
                  c->q_far_world || c->q_shadow_refresh > 1 || c->occl_census ||
                  (c->ab_enabled && (c->ab_compare == AB_QUALITY || c->ab_compare == AB_CHARMODES ||
                                     c->ab_compare == AB_FAR || c->ab_compare == AB_GPU ||
                                     c->ab_compare == AB_SHADOWREFRESH || c->ab_compare == AB_EFFECTS ||
                                     c->ab_compare == AB_OVERALL || c->ab_compare == AB_FEATURES));
    /* outline shaders recognised, scene objects' distance marked: the option, its cycle, its compare shots */
    c->q_rim_hooks = c->q_rim_dist > 0 || c->compare_shots == 6 || c->compare_shots == 7 || c->compare_shots == 8 ||
                     c->compare_shots == 9 ||
                     (c->ab_enabled && c->ab_compare == AB_LOOK);
    c->q_inner_hooks = c->q_inner_dist > 0 || c->compare_shots == 7;
    c->q_hatch_hooks = c->q_hatch > 0 || c->compare_shots == 9;
    c->q_line_hooks = (c->q_line_mode && c->q_line_n) || c->compare_shots == 11 ||
                      (c->ab_enabled && c->ab_compare == AB_LINES && c->q_line_n);
    /* Compare=overall: its third variant is the mod with the steadier image */
    c->q_stab_hooks = c->q_stab > 0 || c->q_stab_world > 0 ||
                      (c->ab_enabled && (c->ab_compare == AB_STAB || c->ab_compare == AB_SAVINGS ||
                                         c->ab_compare == AB_OVERALL || c->ab_compare == AB_FEATURES));
    /* the motion vectors need the scene objects' marks (the outline hooks) and the effects' hooks */
    c->q_mv_hooks = (c->q_mv || (c->ab_enabled && (c->ab_compare == AB_MOTION || c->ab_compare == AB_SAVINGS ||
                                                   c->ab_compare == AB_OVERALL || c->ab_compare == AB_FEATURES))) &&
                    c->q_stab_hooks;
    /* the outline hooks also mark each scene object's distance: the rim and the texture dump need it too */
    c->q_outline_hooks = c->q_outline_dist > 0 || c->compare_shots == 4 || c->compare_shots == 5 || c->compare_shots == 6 ||
                         c->compare_shots == 7 || c->compare_shots == 8 || c->compare_shots == 9 || c->q_ol_depth > 0 ||
                         c->q_rim_hooks || c->dump_tex || c->compare_shots == 10 || c->q_line_hooks ||
                         c->q_inner_hooks || c->q_mv_hooks ||
                         (c->ab_enabled && (c->ab_compare == AB_OUTLINES || c->ab_compare == AB_LOOK));
    c->q_active |= c->q_outline_hooks || c->q_char_tex_q || c->q_world_tex_q || c->q_hatch_hooks || c->compare_shots == 12 ||
                   (c->ab_enabled && c->ab_compare == AB_SAVINGS);
    c->q_fx_active = !c->q_fog || !c->q_glare || c->q_smaa != 3 || c->compare_shots == 1 || c->compare_shots == 5 ||
                     c->q_knock_n || !c->q_painterly ||
                     c->q_outline_hooks || c->q_hatch_hooks || c->q_stab_hooks ||
                     (c->ab_enabled && (c->ab_compare == AB_EFFECTS || c->ab_compare == AB_OVERALL ||
                                        c->ab_compare == AB_SMAA || c->ab_compare == AB_FEATURES));
    c->q_active |= c->q_fx_active;
    /* far meshes: both kinds when they are switched at run time. Compare=overall starts with them off
     * (the game as-is) before the device exists: their hooks and read-back must still be set up */
    c->q_fm_both = c->compare_shots == 2 ||
                   (c->ab_enabled && (c->ab_compare == AB_FARMESH ||
                                       ((c->ab_compare == AB_OVERALL || c->ab_compare == AB_FEATURES) && c->q_farmesh)));
    c->q_active |= c->q_farmesh || c->q_fm_both;
    if (c->profile == PROFILE_PC) {
        config_profile_pc(c);
        /* the CPU fixes that may help on Windows too, opted into in their own section (a
         * tablet preset copied to the PC cannot turn them on) */
        c->fix_timer_churn = ini_int(ini, "PC", "FixTimerChurn", 0) ? 1 : 0;
        c->replace_spinlock = ini_int(ini, "PC", "ReplaceSpinLock", 0) ? 1 : 0;
        c->fast_sem = ini_int(ini, "PC", "FastSemaphore", 0) ? 1 : 0;
    }
}
