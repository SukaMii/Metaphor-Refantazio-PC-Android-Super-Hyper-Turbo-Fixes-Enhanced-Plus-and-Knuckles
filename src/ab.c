/* In-session A/B testing ([ABTest] Enabled=1).
 *
 * The session alternates every SwitchSec seconds between
 *   A = the game's own behaviour (all switchable fixes off), and
 *   B = the fixes configured in the INI.
 * Both halves share the same temperature, battery level and scene, so the
 * comparison needs no cool-downs and no identical start temperature. Every profiler
 * window is tagged [AB A] / [AB B]; the first window after a switch is tagged
 * "transition" and left out by tools\summarize_logs.py.
 *
 * Switchable at runtime: Sleep(0) handling, timer-resolution churn, engine spinlock
 * (patched function emulates the original in A), engine usleep, affinity policy,
 * main-thread / dxvk-cs placement and priority, shadow culling. Fixed for the whole
 * session (set them in the INI): critical-section spin count, job worker count, frame
 * limiter, job semaphore.
 *
 * Compare=shadowcull / preciseusleep / animcull: A is this INI with only that one setting
 * off (animcull: AnimatedMarginPercent=-1), B this INI as written, so the two halves
 * differ by that setting alone.
 *
 * Compare=overall: a two-variant cycle, the game as-is (config_neutral: CPU fixes, shadow
 * culling and every [Quality] change off) and this INI as written: the mod's total.
 *
 * Compare=features: one session for every change: the INI as written, then each change taken
 * back in turn, then the steadier image built up part by part. */
#include "mcpu.h"
#include <stdio.h>
#include <string.h>

static Config s_a, s_b;
static int s_active, s_mode, s_window, s_windows;

/* Compare=quality: a cycle through variants that each change one thing in the picture,
 * against the INI as written (variant 0). Measures what each area costs. */
#define QV 20
static Config s_q[QV];
static const char* const s_qname_quality[QV] = {"base", "characters-hidden", "character-shadows-off",
                                                "far-shadows-off", "shadows-off", "texture-mip-bias-2", "outlines-off"};
/* Compare=charmodes: which scene pass (interpreter mode) draws what of the characters */
static const char* const s_qname_charmodes[QV] = {"base", "chars-hidden-mode0", "chars-hidden-mode2",
                                                  "chars-hidden-mode3", "chars-hidden-all", "", ""};
/* Compare=far: the "bubble" options (the game's simple character models moved closer, far
 * animation and textures), plus all characters hidden (what the characters cost in total) */
static const char* const s_qname_far[QV] = {"base", "simple-npcs-50m", "simple-npcs-30m", "far-animation-half",
                                            "far-textures-mip2", "all-characters-hidden", "bubble-combined"};
/* Compare=gpu: GPU-load options beyond the bubble, plus far world objects hidden (what simpler
 * world models could save at most) */
static const char* const s_qname_gpu[QV] = {"base", "shadows-100m", "shadows-60m", "tiny-objects-culled",
                                            "aniso-4x", "far-world-hidden-100m", "gpu-combined"};
/* Compare=shadowrefresh: the far shadow cascade rendered every frame / every 2nd / every 3rd */
static const char* const s_qname_refresh[QV] = {"base", "far-shadows-every-2nd", "far-shadows-every-3rd", "", "", "", ""};
/* Compare=effects: post-processing effects off / SMAA presets */
static const char* const s_qname_fx[QV] = {"base", "fog-off", "glare-off", "smaa-high", "smaa-medium", "smaa-off",
                                           "fog-glare-off-smaa-medium"};
/* Compare=farmesh: the game's far meshes / simpler ones, seams kept / across seams */
static const char* const s_qname_fm[QV] = {"farmesh-off", "farmesh-safe", "farmesh-seams-far", "farmesh-seams-everywhere",
                                           "", "", ""};
/* Compare=fmshadow: far meshes as set; their shadow casters judged by the screen / by 0.5 / 1 texel */
static const char* const s_qname_fms[QV] = {"shadows-by-screen", "shadows-0.5-texel", "shadows-1-texel", "", "", "", ""};
/* Compare=overall: the game as-is (every switchable change off) / the INI as written (the mod's defaults) / that with
 * the steadier image (SteadyImage=on) */
static const char* const s_qname_all[QV] = {"game-as-is", "mod-defaults", "mod-steady-image", "", "", "", ""};
/* Compare=occlusion: the occlusion census off / on (what its queries cost) */
static const char* const s_qname_oc[QV] = {"census-off", "census-on", "", "", "", "", ""};
/* Compare=knockout: base, then one [GpuProbe] KnockoutShaders pixel shader's draws left out at a time (named at start) */
static char s_kname[QV][24];
/* Compare=outlines: the characters' ink outlines all drawn / left out beyond 50 m / beyond 30 m / all left out */
static const char* const s_qname_ol[QV] = {"outlines-game", "outlines-off-beyond-50m", "outlines-off-beyond-30m",
                                           "outlines-all-off", "", "", ""};
/* Compare=look: the look as the INI sets it (Android: SMAA High, outlines within 40 m, no rim) / the same with the rim
 * light on / the look of 0.41.0 (SMAA Medium, every outline, the rim on) */
static const char* const s_qname_look[QV] = {"new-look", "new-look-rim-on", "old-look", "", "", "", ""};
/* Compare=lines: the characters' ink line meshes as the game / drawn again shifted, 4 copies / 8 copies (thick); within
 * LineDistance */
static const char* const s_qname_lines[QV] = {"lines-game", "lines-redraw", "lines-thick", "", "", "", ""};
/* Compare=stabilise: the image as the game draws it / blended with the previous frame (TemporalStabilise and
 * TemporalStabiliseWorld as set; neither set: the characters at 40%) */
static const char* const s_qname_stab[QV] = {"stabilise-off", "stabilise-on", "", "", "", "", ""};
/* Compare=smaa: SMAA as set / off (under Luma, whose FSR replaces SMAA's blend: what its edge and weight passes cost) */
static const char* const s_qname_smaa[QV] = {"smaa-as-set", "smaa-off", "", "", "", "", ""};
/* Compare=motion: the stabilisation as set, without / with the characters' motion vectors */
static const char* const s_qname_mv[QV] = {"motion-vectors-off", "motion-vectors-on", "", "", "", "", ""};
/* Compare=savings: the stabilisation with the characters' motion (40%, 50%; the skinned vertices of every character
 * within MotionVectorDistance, the world's 3x3 neighbourhood, the world's textures as the game's), then one saving at a
 * time: the skinned vertices only within 20 m (MotionVectorSkinCrowd as set) / the world's textures +0.5 mip / the
 * world's 4 neighbours; then all three */
static const char* const s_qname_sav[QV] = {"savings-none", "skin-within-20m", "world-textures-0.5", "world-4-taps",
                                            "all-savings", "", ""};
/* Compare=features: the INI as written, repeated through the round so that drift can be fitted; each change taken back;
 * the steadier image part by part */
static const char* const s_qname_feat[QV] = {
    "mod-defaults", "no-shadow-culling", "no-cpu-fixes", "no-far-meshes", "far-shadows-every-frame", "shadows-200m",
    "mod-defaults", "smaa-ultra", "outlines-all", "rim-on", "glare-on", "far-animation-every-frame", "npcs-100m",
    "mod-defaults", "stabilise-characters", "stabilise-whole-image", "stabilise-8-neighbours", "steady-all-limbs",
    "steady-limbs-20m", "steady-image"};
#define FV_NO_CPU 2
static const char* s_qname_ko[QV];
static const char* const* s_qname = s_qname_quality;
static int s_qn = QV;
static char s_qtag[64];

static const char* const s_single[] = {NULL, "shadow culling", "precise engine usleep", "animated-caster culling",
                                        "CPU fixes (timer, spinlock)", NULL, NULL, NULL,
                                        "bubble (simple NPC models, far animation, far textures)"};

static void copy_runtime(Config* dst, const Config* src)
{
    dst->adaptive_yield = src->adaptive_yield;
    dst->yield_mode = src->yield_mode;
    dst->yield_spin_us = src->yield_spin_us;
    dst->yield_sleep_us = src->yield_sleep_us;
    dst->fix_timer_churn = src->fix_timer_churn;
    dst->precise_usleep = src->precise_usleep;
    dst->replace_spinlock = src->replace_spinlock;
    dst->spin_iters = src->spin_iters;
    dst->spin_yields = src->spin_yields;
    dst->affinity_mode = src->affinity_mode;
    dst->main_cores_auto = src->main_cores_auto;
    dst->main_cores = src->main_cores;
    memcpy(dst->fast_thread_names, src->fast_thread_names, sizeof(dst->fast_thread_names));
    dst->main_priority = src->main_priority;
    dst->worker_priority = src->worker_priority;
    dst->shadow_cull = src->shadow_cull;
    dst->shadow_skinned_pct = src->shadow_skinned_pct;
    dst->q_characters = src->q_characters;
    dst->q_char_hide = src->q_char_hide;
    dst->q_outlines = src->q_outlines;
    dst->q_char_shadows = src->q_char_shadows;
    dst->q_far_shadows = src->q_far_shadows;
    dst->q_shadows = src->q_shadows;
    dst->q_mip_bias = src->q_mip_bias;
    dst->q_far_dist = src->q_far_dist;
    dst->q_far_chars = src->q_far_chars;
    dst->q_far_anim = src->q_far_anim;
    dst->q_far_mip = src->q_far_mip;
    dst->q_char_lod = src->q_char_lod;
    dst->q_shadow_dist = src->q_shadow_dist;
    dst->q_tiny_px = src->q_tiny_px;
    dst->q_tiny_texels = src->q_tiny_texels;
    dst->q_max_aniso = src->q_max_aniso;
    dst->q_far_world = src->q_far_world;
    dst->q_shadow_refresh = src->q_shadow_refresh;
    dst->q_fog = src->q_fog;
    dst->q_glare = src->q_glare;
    dst->q_smaa = src->q_smaa;
    dst->q_farmesh = src->q_farmesh;
    dst->q_fm_shadow = src->q_fm_shadow;
    dst->q_fm_seams_from = src->q_fm_seams_from;
    dst->occl_census = src->occl_census;
    dst->q_knock = src->q_knock;
    dst->q_outline_dist = src->q_outline_dist;
    dst->q_painterly = src->q_painterly;
    dst->q_rim_dist = src->q_rim_dist;
    dst->q_inner_dist = src->q_inner_dist;
    dst->q_char_tex_q = src->q_char_tex_q;
    dst->q_ol_depth = src->q_ol_depth;
    dst->q_hatch = src->q_hatch;
    dst->q_line_mode = src->q_line_mode;
    dst->q_line_redraws = src->q_line_redraws;
    dst->q_stab = src->q_stab;
    dst->q_stab_world = src->q_stab_world;
    dst->q_mv = src->q_mv;
    dst->q_mv_skin_dist = src->q_mv_skin_dist;
    dst->q_stab_world_taps = src->q_stab_world_taps;
    dst->q_world_tex_q = src->q_world_tex_q;
}

static int cycling(void)
{
    return g_cfg.ab_compare == AB_QUALITY || g_cfg.ab_compare == AB_CHARMODES || g_cfg.ab_compare == AB_FAR ||
           g_cfg.ab_compare == AB_GPU || g_cfg.ab_compare == AB_SHADOWREFRESH || g_cfg.ab_compare == AB_EFFECTS ||
           g_cfg.ab_compare == AB_FARMESH || g_cfg.ab_compare == AB_FMSHADOW || g_cfg.ab_compare == AB_OVERALL ||
           g_cfg.ab_compare == AB_OCCLUSION || g_cfg.ab_compare == AB_KNOCKOUT || g_cfg.ab_compare == AB_OUTLINES ||
           g_cfg.ab_compare == AB_LOOK || g_cfg.ab_compare == AB_LINES || g_cfg.ab_compare == AB_STAB ||
           g_cfg.ab_compare == AB_SMAA || g_cfg.ab_compare == AB_MOTION || g_cfg.ab_compare == AB_SAVINGS ||
           g_cfg.ab_compare == AB_FEATURES;
}

static void apply(int mode)
{
    if (cycling()) {
        copy_runtime(&g_cfg, &s_q[mode]);
        s_mode = mode;
        s_window = 0;
        if (g_cfg.ab_compare == AB_OVERALL || g_cfg.ab_compare == AB_FEATURES) { /* the CPU side as in Compare=all */
            int game_cpu = g_cfg.ab_compare == AB_OVERALL ? mode == 0 : mode == FV_NO_CPU;
            if (!g_cfg.fix_timer_churn)
                timing_period_release();
            if (game_cpu)
                threads_reset_placement();
            else
                threads_apply_main_policy(threads_main_tid());
        }
        LOGI("Quality cycle: now running variant %d (%s) for %d s", mode, s_qname[mode],
             s_windows * g_cfg.prof_interval_sec);
        return;
    }
    copy_runtime(&g_cfg, mode ? &s_b : &s_a);
    if (!g_cfg.fix_timer_churn)
        timing_period_release(); /* side A: the engine's own timer use, without our permanent resolution */
    s_mode = mode;
    s_window = 0;
    if (g_cfg.ab_compare != AB_ALL) {
        LOGI("A/B: now running %s %s %s for %d s", mode ? "B:" : "A:", s_single[g_cfg.ab_compare],
             mode ? "on" : "off", s_windows * g_cfg.prof_interval_sec);
        return; /* thread placement is the same in both halves */
    }
    if (mode)
        threads_apply_main_policy(threads_main_tid());
    else
        threads_reset_placement();
    LOGI("A/B: now running %s for %d s", mode ? "B (MetaphorCPU fixes)" : "A (game as-is)",
         s_windows * g_cfg.prof_interval_sec);
}

/* Called at the end of initialisation, with g_cfg holding the INI (= side B). */
void ab_start(void)
{
    if (!g_cfg.ab_enabled)
        return;
    if (!g_cfg.enabled) {
        LOGI("A/B: needs Enabled=1 (the INI describes side B); A/B not started");
        return;
    }
    s_b = g_cfg;
    s_a = g_cfg;
    switch (g_cfg.ab_compare) {
    case AB_SHADOWCULL:
        s_a.shadow_cull = 0;
        break;
    case AB_PRECISE_USLEEP:
        s_a.precise_usleep = 0;
        if (!s_b.precise_usleep)
            LOGI("A/B: Compare=preciseusleep but [Timer] PreciseUSleep=0: both halves are the same");
        break;
    case AB_CPU: /* the runtime-switchable CPU fixes; the job semaphore stays as set for the session */
        s_a.fix_timer_churn = 0;
        s_a.replace_spinlock = 0;
        s_a.precise_usleep = 0;
        s_a.adaptive_yield = 0;
        if (!s_b.fix_timer_churn && !s_b.replace_spinlock)
            LOGI("A/B: Compare=cpu but the timer and spinlock fixes are off: both halves are the same");
        break;
    case AB_QUALITY:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].q_characters = 0, s_q[1].q_char_hide = 0x0D;
        s_q[2].q_char_shadows = 0;
        s_q[3].q_far_shadows = 0;
        s_q[4].q_shadows = 0;
        s_q[5].q_mip_bias = 2;
        s_qn = 6; /* outlines: added once the pass that draws them is known (Compare=charmodes) */
        break;
    case AB_CHARMODES:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].q_char_hide = 0x01;
        s_q[2].q_char_hide = 0x04;
        s_q[3].q_char_hide = 0x08;
        s_q[4].q_char_hide = 0x0D;
        s_qname = s_qname_charmodes;
        s_qn = 5;
        break;
    case AB_FAR:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].q_char_lod = 50;
        s_q[2].q_char_lod = 30;
        s_q[3].q_far_anim = 2;
        s_q[4].q_far_mip = 2;
        s_q[5].q_characters = 0, s_q[5].q_char_hide = 0x0D;
        s_q[6].q_char_lod = 50, s_q[6].q_far_anim = 2, s_q[6].q_far_mip = 2;
        s_qname = s_qname_far;
        s_qn = 7;
        break;
    case AB_GPU:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].q_shadow_dist = 100;
        s_q[2].q_shadow_dist = 60;
        s_q[3].q_tiny_px = 2.0f, s_q[3].q_tiny_texels = 1.0f;
        s_q[4].q_max_aniso = 4;
        s_q[5].q_far_world = 100;
        s_q[6].q_shadow_dist = 100, s_q[6].q_tiny_px = 2.0f, s_q[6].q_tiny_texels = 1.0f, s_q[6].q_max_aniso = 4;
        s_qname = s_qname_gpu;
        s_qn = 7;
        break;
    case AB_EFFECTS: /* every variant against the game's own post-processing */
        for (int i = 0; i < QV; i++) {
            s_q[i] = g_cfg;
            s_q[i].q_fog = 1, s_q[i].q_glare = 1, s_q[i].q_smaa = 3;
        }
        s_q[1].q_fog = 0;
        s_q[2].q_glare = 0;
        s_q[3].q_smaa = 2;
        s_q[4].q_smaa = 1;
        s_q[5].q_smaa = 0;
        s_q[6].q_fog = 0, s_q[6].q_glare = 0, s_q[6].q_smaa = 1;
        s_qname = s_qname_fx;
        s_qn = 7;
        break;
    case AB_FMSHADOW:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_fm_shadow = 0;
        s_q[1].q_fm_shadow = 0.5f;
        s_q[2].q_fm_shadow = 1.0f;
        s_qname = s_qname_fms;
        s_qn = 3;
        if (!g_cfg.q_farmesh)
            LOGI("A/B: Compare=fmshadow but [Quality] FarMeshes is off: all variants are the same");
        break;
    case AB_FARMESH:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_farmesh = 0;
        s_q[1].q_farmesh = 1;
        s_q[2].q_farmesh = 2; /* across seams from FarMeshSeamsFrom on (150 m unless set) */
        if (s_q[2].q_fm_seams_from <= 0)
            s_q[2].q_fm_seams_from = 150.0f;
        s_q[3].q_farmesh = 2, s_q[3].q_fm_seams_from = 0; /* across seams at every distance */
        s_qname = s_qname_fm;
        s_qn = 4;
        break;
    case AB_OVERALL: /* variant 0: every change that switches at run time off (the baseline run's settings); 2: the
                      * steadier image added, as SteadyImage=on sets it (left out when the INI has it on already) */
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        config_neutral(&s_q[0]);
        s_qname = s_qname_all;
        s_qn = 2;
        if (g_cfg.q_stab <= 0 && g_cfg.q_stab_world <= 0) {
            s_q[2].q_stab = 0.4f, s_q[2].q_stab_world = 0.5f, s_q[2].q_mv = 1, s_q[2].q_smaa = 1;
            s_qn = 3;
        }
        break;
    case AB_FEATURES:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].shadow_cull = 0;
        s_q[FV_NO_CPU].adaptive_yield = 0, s_q[FV_NO_CPU].fix_timer_churn = 0, s_q[FV_NO_CPU].precise_usleep = 0;
        s_q[FV_NO_CPU].replace_spinlock = 0, s_q[FV_NO_CPU].affinity_mode = 0, s_q[FV_NO_CPU].main_cores_auto = 0;
        s_q[FV_NO_CPU].main_cores = 0, s_q[FV_NO_CPU].fast_thread_names[0] = 0;
        s_q[FV_NO_CPU].main_priority = 99, s_q[FV_NO_CPU].worker_priority = 99;
        s_q[3].q_farmesh = 0;
        s_q[4].q_shadow_refresh = 1;
        s_q[5].q_shadow_dist = 0;
        s_q[7].q_smaa = 3;
        s_q[8].q_outline_dist = 0;
        s_q[9].q_rim_dist = 0;
        s_q[10].q_glare = 1;
        s_q[11].q_far_anim = 1;
        s_q[12].q_char_lod = 0;
        s_q[14].q_stab = 0.4f;
        for (int i = 15; i < QV; i++) /* the whole image, 4 neighbours unless said otherwise */
            s_q[i].q_stab = 0.4f, s_q[i].q_stab_world = 0.5f, s_q[i].q_stab_world_taps = 4;
        s_q[16].q_stab_world_taps = 8;
        s_q[17].q_mv = 1, s_q[17].q_mv_skin_dist = 0;
        s_q[18].q_mv = 1, s_q[18].q_mv_skin_dist = 20;
        s_q[19].q_mv = 1, s_q[19].q_mv_skin_dist = 0, s_q[19].q_smaa = 1;
        s_qname = s_qname_feat;
        s_qn = QV;
        break;
    case AB_OUTLINES:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_outline_dist = 0;
        s_q[1].q_outline_dist = 50;
        s_q[2].q_outline_dist = 30;
        s_q[3].q_outline_dist = 0.001f;
        s_qname = s_qname_ol;
        s_qn = 4;
        break;
    case AB_LOOK:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].q_rim_dist = 0;
        s_q[2].q_rim_dist = 0, s_q[2].q_smaa = 1, s_q[2].q_outline_dist = 0;
        s_qname = s_qname_look;
        s_qn = 3;
        break;
    case AB_LINES:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_line_mode = 0;
        s_q[1].q_line_mode = 1, s_q[1].q_line_redraws = 4;
        s_q[2].q_line_mode = 1, s_q[2].q_line_redraws = 8;
        s_qname = s_qname_lines;
        s_qn = 3;
        break;
    case AB_STAB:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_stab = 0, s_q[0].q_stab_world = 0;
        if (g_cfg.q_stab <= 0 && g_cfg.q_stab_world <= 0)
            s_q[1].q_stab = 0.4f;
        s_qname = s_qname_stab;
        s_qn = 2;
        break;
    case AB_SAVINGS:
        for (int i = 0; i < QV; i++) {
            s_q[i] = g_cfg;
            s_q[i].q_stab = g_cfg.q_stab > 0 ? g_cfg.q_stab : 0.4f;
            s_q[i].q_stab_world = g_cfg.q_stab_world > 0 ? g_cfg.q_stab_world : 0.5f;
            s_q[i].q_mv = 1, s_q[i].q_mv_skin_dist = 0, s_q[i].q_world_tex_q = 0, s_q[i].q_stab_world_taps = 8;
        }
        s_q[1].q_mv_skin_dist = 20;
        s_q[2].q_world_tex_q = 2;
        s_q[3].q_stab_world_taps = 4;
        s_q[4].q_mv_skin_dist = 20, s_q[4].q_world_tex_q = 2, s_q[4].q_stab_world_taps = 4;
        s_qname = s_qname_sav;
        s_qn = 5;
        break;
    case AB_MOTION:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_mv = 0, s_q[1].q_mv = 1;
        s_qname = s_qname_mv;
        s_qn = 2;
        break;
    case AB_SMAA:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[1].q_smaa = 0;
        s_qname = s_qname_smaa;
        s_qn = 2;
        break;
    case AB_KNOCKOUT:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_knock = 0;
        snprintf(s_kname[0], sizeof(s_kname[0]), "base");
        s_qname_ko[0] = s_kname[0];
        for (int i = 0; i < g_cfg.q_knock_n && i + 1 < QV; i++) {
            s_q[i + 1].q_knock = g_cfg.q_knock_list[i];
            snprintf(s_kname[i + 1], sizeof(s_kname[i + 1]), "no-%08llx", (unsigned long long)(g_cfg.q_knock_list[i] >> 32));
            s_qname_ko[i + 1] = s_kname[i + 1];
        }
        s_qname = s_qname_ko;
        s_qn = 1 + (g_cfg.q_knock_n < QV - 1 ? g_cfg.q_knock_n : QV - 1);
        if (!g_cfg.q_knock_n)
            LOGI("A/B: Compare=knockout but [GpuProbe] KnockoutShaders is empty: only the base runs");
        break;
    case AB_OCCLUSION:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].occl_census = 0;
        s_qname = s_qname_oc;
        s_qn = 2;
        if (!g_cfg.occl_hooks)
            LOGI("A/B: Compare=occlusion but [GpuProbe] OcclusionCensus=0: both variants are the same");
        break;
    case AB_SHADOWREFRESH:
        for (int i = 0; i < QV; i++)
            s_q[i] = g_cfg;
        s_q[0].q_shadow_refresh = 1;
        s_q[1].q_shadow_refresh = 2;
        s_q[2].q_shadow_refresh = 3;
        s_qname = s_qname_refresh;
        s_qn = 3;
        break;
    case AB_BUBBLE: /* A: the game's NPC model distance and full-rate animation */
        s_a.q_char_lod = 0;
        s_a.q_far_anim = 1;
        s_a.q_far_mip = 0;
        if (!s_b.q_char_lod && s_b.q_far_anim <= 1 && !s_b.q_far_mip)
            LOGI("A/B: Compare=bubble but the bubble options are off: both halves are the same");
        break;
    case AB_ANIMCULL:
        s_a.shadow_skinned_pct = -1;
        if (s_b.shadow_skinned_pct < 0 || !s_b.shadow_cull)
            LOGI("A/B: Compare=animcull but [ShadowCull] Enabled=0 or AnimatedMarginPercent<0: both halves are the same");
        break;
    default:
        s_a.shadow_cull = 0;
        s_a.adaptive_yield = 0;
        s_a.fix_timer_churn = 0;
        s_a.precise_usleep = 0;
        s_a.replace_spinlock = 0;
        s_a.affinity_mode = 0;
        s_a.main_cores_auto = 0;
        s_a.main_cores = 0;
        s_a.fast_thread_names[0] = 0;
        s_a.main_priority = 99;
        s_a.worker_priority = 99;
        break;
    }
    g_cfg.prof_enabled = 1;
    s_windows = g_cfg.ab_switch_sec / (g_cfg.prof_interval_sec ? g_cfg.prof_interval_sec : 10);
    if (s_windows < 2)
        s_windows = 2;
    s_active = 1;
    if (g_cfg.ab_compare == AB_SAVINGS)
        LOGI("Savings cycle: the stabilisation with the characters' motion (characters %.0f%%, the rest %.0f%%), then the "
             "skinned vertices only within 20 m (when more than %d characters are within %.0f m) / the world's textures "
             "+0.5 mip / the world's 4 neighbours / all three; %d s each, repeated; first window of each is a transition",
             100.0 * s_q[0].q_stab, 100.0 * s_q[0].q_stab_world, g_cfg.q_mv_crowd, g_cfg.q_mv_dist,
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_MOTION)
        LOGI("Motion cycle: the stabilisation as set (characters %.0f%%, the rest %.0f%%) without, then with the "
             "characters' motion vectors; %d s each, repeated; first window of each is a transition",
             100.0 * g_cfg.q_stab, 100.0 * g_cfg.q_stab_world, s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_SMAA)
        LOGI("SMAA cycle: SMAA as set (%s), then off (its edge and weight passes left out, the blend a plain copy); %d s "
             "each, repeated; first window of each is a transition",
             g_cfg.q_smaa == 2 ? "High" : g_cfg.q_smaa == 1 ? "Medium" : g_cfg.q_smaa == 3 ? "Ultra" : "off",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_STAB)
        LOGI("Stabilise cycle: the image as the game draws it, then blended with the previous frame (history: characters "
             "%.0f%%, the rest %.0f%%); %d s each, repeated; first window of each is a transition", 100.0 * s_q[1].q_stab,
             100.0 * s_q[1].q_stab_world, s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_LINES)
        LOGI("Lines cycle: the characters' ink line meshes as the game, drawn again shifted (4 copies), thick (8 copies), for "
             "characters within %.0f m (0 = all); %d s each, repeated; first window of each is a transition", g_cfg.q_line_dist,
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_LOOK)
        LOGI("Look cycle: the look as set (SMAA %s, outlines %s, rim %s), the same with the rim light on, the look of "
             "0.41.0 (SMAA Medium, every outline, rim on); %d s each, repeated; first window of each is a transition",
             g_cfg.q_smaa == 2 ? "High" : g_cfg.q_smaa == 1 ? "Medium" : g_cfg.q_smaa == 3 ? "Ultra" : "off",
             g_cfg.q_outline_dist > 0 ? "within a distance" : "all", g_cfg.q_rim_dist > 0 ? "off or near only" : "on",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_OUTLINES)
        LOGI("Outline cycle: the characters' ink outlines all drawn, left out beyond 50 m, beyond 30 m from the camera, "
             "all left out; %d s each, repeated; first window of each is a transition", s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_KNOCKOUT)
        LOGI("Knock-out cycle: base, then the draws of one pixel shader left out at a time (%d shaders: the "
             "variant names carry the top 32 bits of its bytecode hash); %d s each, repeated; first window of each "
             "is a transition", s_qn - 1, s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_OCCLUSION)
        LOGI("Occlusion census cycle: the census off, then on (an occlusion query around every scene object); %d s "
             "each, repeated; first window of each is a transition", s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_FEATURES)
        LOGI("Features cycle: the INI as written (NPC stand-ins at a fixed %d m), each change taken back in turn, then the "
             "steadier image part by part; %d variants, %d s each, repeated, after the first variant held for %d s; first "
             "window of each is a transition. Startup-only, the same in all: job semaphore %s",
             g_cfg.q_char_lod, s_qn, s_windows * g_cfg.prof_interval_sec, g_cfg.ab_start_after_sec,
             g_cfg.fast_sem ? "replaced" : "the game's");
    else if (g_cfg.ab_compare == AB_OVERALL)
        LOGI("Overall cycle: the game as-is (shadow culling, CPU fixes and every [Quality] change off), then the mod as "
             "this INI sets it (NPC stand-ins at a fixed %d m: the adaptive distance waits for the cycle to end)%s; %d s "
             "each, repeated; first window of each is a transition. Startup-only, the same in all: job semaphore "
             "%s, CS spin %d, job pools %d/%d, limiter %d fps",
             g_cfg.q_char_lod,
             s_qn == 3 ? ", then that with the steadier image (stabilisation: characters 40%, the rest 50% clamped to "
                         "the taps set; the characters' motion vectors; SMAA Medium)"
                       : " (its stabilisation is on: no steadier-image variant)",
             s_windows * g_cfg.prof_interval_sec, g_cfg.fast_sem ? "replaced" : "the game's",
             g_cfg.cs_spin_count, g_cfg.job_pool0, g_cfg.job_pool1, g_cfg.limiter_fps);
    else if (g_cfg.ab_compare == AB_FMSHADOW)
        LOGI("Far mesh shadow cycle: shadow casters' simpler versions judged by the screen, by 0.5 shadow-map texel, "
             "by 1 texel; %d s each, repeated; first window of each is a transition",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_FARMESH)
        LOGI("Far mesh cycle: the game's meshes, simpler ones with the seams kept, simpler across seams from %.0f m, "
             "simpler across seams everywhere; %d s each, repeated; first window of each is a transition",
             s_q[2].q_fm_seams_from,
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_EFFECTS)
        LOGI("Effects cycle: base, volumetric fog off, glare off, SMAA High, SMAA Medium, SMAA off, fog + glare off with "
             "SMAA Medium; %d s each, repeated; first window of each is a transition",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_SHADOWREFRESH)
        LOGI("Far shadow refresh cycle: the far cascade rendered every frame, every 2nd, every 3rd; %d s each, "
             "repeated; first window of each is a transition",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_GPU)
        LOGI("GPU cycle: base, shadow distance 100 m, 60 m, tiny objects culled (< 2 px, casters < 1 texel), "
             "anisotropic filtering max 4x, world objects beyond 100 m hidden (measurement), combined (100 m + tiny "
             "+ 4x); %d s each, repeated; first window of each is a transition",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_FAR)
        LOGI("Bubble cycle: base, the game's simple character models beyond 50 m / 30 m from the hero, characters "
             "beyond %d m from the camera animated every 2nd frame, textures beyond %d m mip +2, all characters "
             "hidden, combined (50 m models + animation + textures); %d s each, repeated; first window of each is a "
             "transition",
             g_cfg.q_far_dist, g_cfg.q_far_dist, s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_CHARMODES)
        LOGI("Character pass cycle: base, characters hidden in interpreter mode 0, 2, 3, all; %d s each, repeated",
             s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare == AB_QUALITY)
        LOGI("Quality cycle: %d variants (base, characters hidden, character shadows off, far shadows off, shadows "
             "off, texture mip bias +2), %d s each, repeated; first window of each is a transition",
             s_qn, s_windows * g_cfg.prof_interval_sec);
    else if (g_cfg.ab_compare != AB_ALL)
        LOGI("A/B: alternating A (%s off) and B (%s on) every %d s, everything else as in this INI; first window "
             "after each switch is a transition",
             s_single[g_cfg.ab_compare], s_single[g_cfg.ab_compare], s_windows * g_cfg.prof_interval_sec);
    else
        LOGI("A/B: alternating A (game as-is) and B (this INI) every %d s; first window after each switch "
             "is a transition. Fixed for both halves: CS spin %d, job pools %d/%d, limiter %d fps",
             s_windows * g_cfg.prof_interval_sec, g_cfg.cs_spin_count, g_cfg.job_pool0, g_cfg.job_pool1,
             g_cfg.limiter_fps);
    apply(0);
    if (cycling() && g_cfg.ab_start_after_sec > 0) /* these windows count as transitions: the tools leave them out */
        s_window = -(g_cfg.ab_start_after_sec / (g_cfg.prof_interval_sec ? g_cfg.prof_interval_sec : 10));
}

const char* ab_window_tag(void)
{
    if (!s_active)
        return "";
    if (cycling()) {
        snprintf(s_qtag, sizeof(s_qtag), " [Q%d %s%s]", s_mode, s_qname[s_mode], s_window <= 0 ? " transition" : "");
        return s_qtag;
    }
    if (s_window == 0)
        return s_mode ? " [AB B transition]" : " [AB A transition]";
    return s_mode ? " [AB B]" : " [AB A]";
}

void ab_after_window(void)
{
    if (!s_active)
        return;
    if (++s_window >= s_windows)
        apply(cycling() ? (s_mode + 1) % s_qn : !s_mode);
}

/* [GpuProbe] CompareShots=13: capture.c saves one image per variant of Compare=features */
int ab_shot_count(void)
{
    return s_active && g_cfg.ab_compare == AB_FEATURES ? s_qn : 0;
}

const char* ab_shot_name(int i)
{
    return s_qname[i];
}

void ab_shot_apply(int i)
{
    apply(i);
}
