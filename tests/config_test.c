/* The [Quality] look keys as written in an INI, on the Android and PC profiles: on / off / a distance / auto. In
 * 0.42.0-0.43.0 RimLight=on and OutlineDistance=off kept the Android default (rim off, outlines within 40 m). */
#include "../src/mcpu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void load(const char* profile, const char* quality, Config* c)
{
    char path[MAX_PATH];
    GetTempPathA(sizeof(path), path);
    strncat(path, "mcpu_config_test.ini", sizeof(path) - strlen(path) - 1);
    FILE* f = fopen(path, "w");
    fprintf(f, "[General]\nProfile=%s\n[Quality]\n%s\n", profile, quality);
    fclose(f);
    config_defaults(c);
    config_load(c, path);
    DeleteFileA(path);
}

static void check(const char* profile, const char* quality, float rim, float outline, float inner, int smaa)
{
    Config c;
    load(profile, quality, &c);
    int ok = c.q_rim_dist == rim && c.q_outline_dist == outline && c.q_inner_dist == inner && c.q_smaa == smaa;
    printf("  [%s] %-8s %-48s rim %.3f outlines %.3f inner %.3f smaa %d\n", ok ? "PASS" : "FAIL", profile, quality,
           c.q_rim_dist, c.q_outline_dist, c.q_inner_dist, c.q_smaa);
    fails += !ok;
}

int main(void)
{
    const float OFF = 0.001f;
    /* the defaults: Android rim off, outlines within 40 m; PC the game's; SMAA High on both */
    check("android", "", OFF, 40, 0, 2);
    check("pc", "", 0, 0, 0, 2);
    check("android", "RimLight=auto\nOutlineDistance=auto", OFF, 40, 0, 2);
    /* on brings the game's look back on Android */
    check("android", "RimLight=on\nOutlineDistance=off\nInnerOutlines=on", 0, 0, 0, 2);
    check("android", "RimLight=1\nOutlineDistance=0", 0, 0, 0, 2);
    check("android", "RimLight=yes\nOutlineDistance=on", 0, 0, 0, 2);
    /* off and distances */
    check("pc", "RimLight=off\nInnerOutlines=0\nOutlineDistance=all", OFF, OFF, OFF, 2);
    check("pc", "RimLight=20\nInnerOutlines=15\nOutlineDistance=30\nSmaa=medium", 20, 30, 15, 1);
    /* nonsense keeps the default */
    check("android", "RimLight=maybe\nOutlineDistance=auto", OFF, 40, 0, 2);
    /* TemporalStabilise: off by default; on = 40% history; a percentage (up to 95); its hooks with it */
    static const struct {
        const char* profile;
        const char* key;
        float want;
    } ts[] = {{"android", "", 0}, {"pc", "", 0}, {"android", "TemporalStabilise=on", 0.4f},
              {"pc", "TemporalStabilise=1", 0.4f}, {"pc", "TemporalStabilise=75", 0.75f},
              {"android", "TemporalStabilise=99", 0.95f}, {"android", "TemporalStabilise=off", 0},
              {"pc", "TemporalStabilise=auto", 0}};
    for (unsigned i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
        Config c;
        load(ts[i].profile, ts[i].key, &c);
        int ok = fabsf(c.q_stab - ts[i].want) < 1e-6f && c.q_stab_hooks == (ts[i].want > 0) &&
                 (!c.q_stab_hooks || c.q_fx_active);
        printf("  [%s] %-8s %-48s history %.2f, hooks %d\n", ok ? "PASS" : "FAIL", ts[i].profile, ts[i].key, c.q_stab,
               c.q_stab_hooks);
        fails += !ok;
    }
    /* TemporalStabiliseWorld (0.47.0): on = 50%, a percentage; it alone sets the hooks up */
    {
        Config a, b;
        load("android", "TemporalStabiliseWorld=on", &a);
        load("pc", "TemporalStabilise=40\nTemporalStabiliseWorld=70", &b);
        int ok = fabsf(a.q_stab_world - 0.5f) < 1e-6f && a.q_stab == 0 && a.q_stab_hooks && a.q_fx_active &&
                 fabsf(b.q_stab_world - 0.7f) < 1e-6f && fabsf(b.q_stab - 0.4f) < 1e-6f && b.q_stab_hooks;
        printf("  [%s] TemporalStabiliseWorld=on / =70 with TemporalStabilise=40      world %.2f / %.2f, characters %.2f / %.2f\n",
               ok ? "PASS" : "FAIL", a.q_stab_world, b.q_stab_world, a.q_stab, b.q_stab);
        fails += !ok;
    }
    /* CharacterMotionVectors (0.48.0): only with the stabilisation; it needs the scene objects' marks (outline hooks) */
    {
        Config a, b;
        load("pc", "TemporalStabilise=40\nCharacterMotionVectors=on\nMotionVectorDistance=25", &a);
        load("pc", "CharacterMotionVectors=on", &b);
        int ok = a.q_mv && a.q_mv_hooks && a.q_outline_hooks && a.q_fx_active && fabsf(a.q_mv_dist - 25) < 1e-6f &&
                 b.q_mv && !b.q_mv_hooks;
        printf("  [%s] CharacterMotionVectors=on with / without TemporalStabilise           hooks %d / %d, outline hooks %d, "
               "distance %.0f\n", ok ? "PASS" : "FAIL", a.q_mv_hooks, b.q_mv_hooks, a.q_outline_hooks, a.q_mv_dist);
        fails += !ok;
    }
    /* 0.49.0: WorldTextureBias (quarter mip levels), MotionVectorSkinCrowd (12), TemporalStabiliseWorldTaps (8 / 4),
     * ComputeCensus; Compare=savings sets the stabilisation and the motion vectors up */
    {
        Config a, b;
        load("android", "WorldTextureBias=0.5\nTemporalStabiliseWorldTaps=4\n[GpuProbe]\nComputeCensus=1", &a);
        load("pc", "MotionVectorSkinCrowd=20\n[ABTest]\nEnabled=1\nCompare=savings", &b);
        /* 0.49.2: 4 taps by default, 8 when asked. 0.49.3: the skin distance 20 m on Android only (auto), 0 (all) on
         * PC, either set by a number */
        Config e, p20, a0, pauto;
        load("android", "TemporalStabiliseWorldTaps=8\nMotionVectorSkinDistance=auto", &e);
        load("pc", "MotionVectorSkinDistance=20", &p20);
        load("android", "MotionVectorSkinDistance=0", &a0);
        load("pc", "MotionVectorSkinDistance=auto", &pauto);
        int ok = a.q_world_tex_q == 2 && a.q_stab_world_taps == 4 && a.compute_census && a.q_mv_crowd == 12 &&
                 a.q_active && b.q_mv_crowd == 20 && b.q_world_tex_q == 0 && b.q_stab_world_taps == 4 &&
                 b.ab_compare == AB_SAVINGS && b.q_stab_hooks && b.q_mv_hooks && e.q_stab_world_taps == 8 &&
                 a.q_mv_skin_dist == 20.0f && e.q_mv_skin_dist == 20.0f && b.q_mv_skin_dist == 0 &&
                 pauto.q_mv_skin_dist == 0 && p20.q_mv_skin_dist == 20.0f && a0.q_mv_skin_dist == 0;
        printf("  [%s] WorldTextureBias, TemporalStabiliseWorldTaps, MotionVectorSkinCrowd, Compare=savings     bias %d, taps "
               "%d (default %d, =8 %d), skin distance android %.0f / auto %.0f / 0: %.0f, pc %.0f / auto %.0f / 20: %.0f, "
               "crowd %d / %d, cycle hooks %d %d\n", ok ? "PASS" : "FAIL",
               a.q_world_tex_q, a.q_stab_world_taps, b.q_stab_world_taps, e.q_stab_world_taps, a.q_mv_skin_dist,
               e.q_mv_skin_dist, a0.q_mv_skin_dist, b.q_mv_skin_dist, pauto.q_mv_skin_dist, p20.q_mv_skin_dist,
               a.q_mv_crowd, b.q_mv_crowd, b.q_stab_hooks, b.q_mv_hooks);
        fails += !ok;
    }
    /* 0.50.0: SteadyImage turns on the stabilisation (40% / 50%), the motion vectors and SMAA Medium; each part's own
     * key wins; auto follows it. Compare=overall sets the stabilisation and the motion vectors up for its 3rd variant */
    {
        Config a, b, c, d, e, o;
        load("android", "SteadyImage=on\nTemporalStabilise=auto\nTemporalStabiliseWorld=auto\nCharacterMotionVectors=auto"
                        "\nSmaa=auto", &a);
        load("pc", "SteadyImage=on\nTemporalStabiliseWorld=off\nSmaa=high", &b);
        load("android", "SteadyImage=off\nSmaa=auto\nTemporalStabilise=auto", &c);
        load("pc", "TemporalStabilise=on", &d);
        load("android", "SteadyImage=1\nCharacterMotionVectors=off\nTemporalStabilise=30", &e);
        load("android", "[ABTest]\nEnabled=1\nCompare=overall", &o);
        int ok = a.q_steady && a.q_stab == 0.4f && a.q_stab_world == 0.5f && a.q_mv && a.q_mv_hooks && a.q_smaa == 1 &&
                 b.q_stab == 0.4f && b.q_stab_world == 0 && b.q_mv && b.q_smaa == 2 &&
                 !c.q_steady && c.q_stab == 0 && !c.q_mv && !c.q_stab_hooks && c.q_smaa == 2 &&
                 !d.q_steady && d.q_stab == 0.4f && !d.q_mv && d.q_smaa == 2 &&
                 e.q_stab == 0.3f && e.q_stab_world == 0.5f && !e.q_mv && e.q_smaa == 1 &&
                 o.q_stab == 0 && o.q_stab_hooks && o.q_mv_hooks && !o.q_mv && o.q_smaa == 2;
        printf("  [%s] SteadyImage: on / parts set / off / unset / =1 with parts, Compare=overall       stab %.2f %.2f mv %d "
               "smaa %d | %.2f %.2f %d %d | %.2f %d %d | %.2f %d %d | %.2f %.2f %d %d | overall hooks %d %d\n",
               ok ? "PASS" : "FAIL", a.q_stab, a.q_stab_world, a.q_mv, a.q_smaa, b.q_stab, b.q_stab_world, b.q_mv, b.q_smaa,
               c.q_stab, c.q_mv, c.q_smaa, d.q_stab, d.q_mv, d.q_smaa, e.q_stab, e.q_stab_world, e.q_mv, e.q_smaa,
               o.q_stab_hooks, o.q_mv_hooks);
        fails += !ok;
    }
    /* 0.52.0: Compare=features sets up everything its variants switch; StartAfterSec */
    {
        Config a;
        load("android", "\n[ABTest]\nEnabled=1\nCompare=features\nStartAfterSec=240", &a);
        int ok = a.ab_compare == AB_FEATURES && a.ab_start_after_sec == 240 && a.q_stab_hooks && a.q_mv_hooks && a.q_fm_both &&
                 a.q_fx_active && a.q_active && a.q_stab == 0 && !a.q_mv && a.q_smaa == 2;
        printf("  [%s] Compare=features, StartAfterSec                                  cycle %d, start after %d, hooks %d %d %d %d\n",
               ok ? "PASS" : "FAIL", a.ab_compare, a.ab_start_after_sec, a.q_stab_hooks, a.q_mv_hooks, a.q_fm_both, a.q_fx_active);
        fails += !ok;
    }
    /* 0.51.0: [General] LogCopy, a Linux path kept as written (converted when the log opens) */
    {
        Config a, b;
        char path[MAX_PATH];
        GetTempPathA(sizeof(path), path);
        strncat(path, "mcpu_config_test_log.ini", sizeof(path) - strlen(path) - 1);
        FILE* f = fopen(path, "w");
        fprintf(f, "[General]\nProfile=android\nLogCopy=/storage/emulated/0/Download/x.log\n");
        fclose(f);
        config_defaults(&a);
        config_load(&a, path);
        DeleteFileA(path);
        load("android", "", &b);
        int ok = !strcmp(a.log_copy, "/storage/emulated/0/Download/x.log") && !b.log_copy[0];
        printf("  [%s] LogCopy                                                          '%s' / '%s'\n", ok ? "PASS" : "FAIL",
               a.log_copy, b.log_copy);
        fails += !ok;
    }
    /* Compare=smaa (0.46.2): the cycle, with the effects hooks on the PC profile too (which has no glare change) */
    {
        Config c;
        load("pc", "TemporalStabilise=off\n[ABTest]\nEnabled=1\nCompare=smaa", &c);
        int ok = c.ab_enabled && c.ab_compare == AB_SMAA && c.q_fx_active && c.q_smaa == 2;
        printf("  [%s] pc       Compare=smaa                                      cycle %d, effects hooks %d, smaa %d\n",
               ok ? "PASS" : "FAIL", c.ab_compare, c.q_fx_active, c.q_smaa);
        fails += !ok;
    }
    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
