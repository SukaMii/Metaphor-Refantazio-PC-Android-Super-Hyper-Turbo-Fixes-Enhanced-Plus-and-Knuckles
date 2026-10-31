/* [Quality] AdaptiveLod: the distance of the game's simple NPC models (CharacterLodDistance) follows
 * the frame rate between AdaptiveLodMin and the set distance, gliding a little every second:
 *   - in, AdaptiveLodInSpeed m per second, while the last AdaptiveLodDownSec seconds averaged under
 *     AdaptiveLodDownFps;
 *   - back out, AdaptiveLodOutSpeed m per second, while the last AdaptiveLodUpSec seconds averaged at
 *     least AdaptiveLodUpFps (with the 30 FPS cap: 29 = holding the cap);
 *   - in between it holds. The gap between the two keeps it from swinging.
 * Moving a metre or two at a time, only the NPCs right at the edge change model, one or two at a
 * time, instead of a whole band at once. Seconds with a stall (a frame over 0.5 s: loading, a pause
 * in rendering) are not judged. Off while an A/B cycle switches the options.
 * Cost: a counter per frame and a sum of up to 64 numbers per second, on the presenting thread;
 * the shadow hook writes the new distance into the game's switch at the next frame. */
#include "mcpu.h"

#define SEC_N 64
static int g_sec_frames[SEC_N];
static uint8_t g_sec_stall[SEC_N];
static uint64_t g_sec0;      /* start of the current second (ms) */
static int g_cur_frames, g_cur_stall;
static int64_t g_secs;       /* seconds completed */
static float g_d;            /* the distance now (m) */
static int g_max;            /* the set distance (m) */
static int g_dir;            /* moving: -1 in, +1 out, 0 holding */
static int g_glides_in, g_glides_out;
static double g_last_fps;

static double fps_over(int n, int* ok)
{
    int sum = 0;
    *ok = n > 0 && g_secs >= n;
    for (int k = 1; *ok && k <= n; k++) {
        int i = (int)((g_secs - k) % SEC_N);
        sum += g_sec_frames[i];
        if (g_sec_stall[i])
            *ok = 0;
    }
    return *ok ? (double)sum / n : 0;
}

static void decide(void)
{
    float lo = (float)(g_cfg.q_alod_min < g_max ? g_cfg.q_alod_min : g_max);
    int ok_dn, ok_up;
    double fd = fps_over(g_cfg.q_alod_down_sec, &ok_dn), fu = fps_over(g_cfg.q_alod_up_sec, &ok_up);
    if (ok_dn)
        g_last_fps = fd;
    int dir = 0;
    if (ok_dn && fd < g_cfg.q_alod_down && g_d > lo)
        dir = -1;
    else if (ok_up && fu >= g_cfg.q_alod_up && g_d < g_max)
        dir = 1;
    if (dir != g_dir) { /* a glide starts or ends: logged once, not every metre */
        if (dir < 0)
            LOGI("Adaptive NPC distance: the last %d s averaged %.1f FPS (< %.0f) -> moving in from %.0f m",
                 g_cfg.q_alod_down_sec, fd, g_cfg.q_alod_down, g_d), g_glides_in++;
        else if (dir > 0)
            LOGI("Adaptive NPC distance: the last %d s held %.1f FPS (>= %.0f) -> moving out from %.0f m",
                 g_cfg.q_alod_up_sec, fu, g_cfg.q_alod_up, g_d), g_glides_out++;
        else
            LOGI("Adaptive NPC distance: holding at %.0f m", g_d);
        g_dir = dir;
    }
    if (dir < 0)
        g_d = g_d - g_cfg.q_alod_in_speed < lo ? lo : g_d - g_cfg.q_alod_in_speed;
    else if (dir > 0)
        g_d = g_d + g_cfg.q_alod_out_speed > g_max ? (float)g_max : g_d + g_cfg.q_alod_out_speed;
    int m = (int)(g_d + 0.5f);
    if (m != g_cfg.q_char_lod)
        g_cfg.q_char_lod = m;
}

/* one presented frame at now_ms, interval_ms after the one before */
void adaptive_frame_at(uint64_t now_ms, double interval_ms)
{
    if (!g_cfg.q_alod || g_cfg.ab_enabled)
        return;
    if (!g_sec0) {
        g_sec0 = now_ms;
        g_max = g_cfg.q_char_lod;
        g_d = (float)g_max;
    }
    if (!g_max)
        return; /* the game's own distance: nothing to move */
    if (interval_ms > 500.0)
        g_cur_stall = 1;
    else
        g_cur_frames++;
    while (now_ms - g_sec0 >= 1000) { /* close the second(s) that ended */
        int i = (int)(g_secs % SEC_N);
        g_sec_frames[i] = g_cur_frames, g_sec_stall[i] = (uint8_t)g_cur_stall;
        g_cur_frames = 0;
        g_sec0 += 1000;
        g_secs++;
        g_cur_stall = now_ms - g_sec0 >= 1000; /* a gap of whole seconds without frames */
        decide();
    }
}

void adaptive_frame(double interval_ms) { adaptive_frame_at(GetTickCount64(), interval_ms); }

void adaptive_report(void)
{
    if (!g_cfg.q_alod || g_cfg.ab_enabled || !g_max)
        return;
    LOGI("  Adaptive NPC distance: simple NPC models from %d m now (%s; range %d-%d m; %d glides in, %d out so far; "
         "last %d s %.1f FPS)",
         g_cfg.q_char_lod, g_dir < 0 ? "moving in" : g_dir > 0 ? "moving out" : "holding",
         g_cfg.q_alod_min < g_max ? g_cfg.q_alod_min : g_max, g_max, g_glides_in, g_glides_out, g_cfg.q_alod_down_sec,
         g_last_fps);
}
