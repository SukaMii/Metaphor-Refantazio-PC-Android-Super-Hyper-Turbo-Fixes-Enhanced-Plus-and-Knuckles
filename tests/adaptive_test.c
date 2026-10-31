/* Adaptive NPC distance (adaptive.c) on simulated time, second by second: steady 30 FPS leaves 60 m;
 * under 25 FPS over 3 s it glides in 2 m per second down to 40 m; back at the cap (29+ over 10 s) it
 * glides out 1 m per second up to 60 m; a 1 s hitch does not move it; 27 FPS holds; stalls (loading)
 * are not judged; an A/B cycle turns it off. And what it costs per frame. */
#include "../src/adaptive.c"
#include <stdio.h>

static int fails;
#define EXPECT(c, what)                                                                                         \
    do {                                                                                                        \
        printf("  [%s] %s\n", (c) ? "PASS" : "FAIL", what);                                                     \
        if (!(c))                                                                                               \
            fails++;                                                                                            \
    } while (0)

static uint64_t t = 1000000;
static int sec;            /* simulated seconds so far */
static int hist[2000];     /* the distance at the end of each second */

/* n seconds at fps frames per second, the last frame exactly at the end of each second */
static void run(int n, int fps)
{
    for (int s = 0; s < n; s++, sec++) {
        uint64_t s0 = 1000000 + (uint64_t)sec * 1000;
        for (int f = 0; f < fps; f++) {
            t = s0 + (uint64_t)(f + 1) * 1000 / fps;
            adaptive_frame_at(t, 1000.0 / fps);
        }
        hist[sec] = g_cfg.q_char_lod;
    }
}

/* largest change from one second to the next over [a, b) */
static int max_step(int a, int b)
{
    int m = 0;
    for (int i = a + 1; i < b; i++) {
        int d = hist[i] - hist[i - 1];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

static int first_at(int a, int b, int value) /* first second in [a, b) with that distance, -1 if none */
{
    for (int i = a; i < b; i++)
        if (hist[i] == value)
            return i;
    return -1;
}

int main(void)
{
    g_cfg.q_alod = 1, g_cfg.q_char_lod = 60, g_cfg.q_alod_min = 40;
    g_cfg.q_alod_down = 25, g_cfg.q_alod_down_sec = 3, g_cfg.q_alod_in_speed = 2;
    g_cfg.q_alod_up = 29, g_cfg.q_alod_up_sec = 10, g_cfg.q_alod_out_speed = 1;
    g_cfg.ab_enabled = 0;
    run(40, 30);
    EXPECT(g_cfg.q_char_lod == 60, "40 s at 30 FPS: 60 m");
    run(1, 22);
    run(10, 30);
    EXPECT(first_at(40, sec, 60) == 40 && max_step(40, sec) == 0, "a 1 s hitch to 22 FPS: stays at 60 m");
    int b = sec;
    run(30, 22);
    int at50 = first_at(b, sec, 50), at40 = first_at(b, sec, 40);
    printf("        at 22 FPS: 50 m after %d s, 40 m after %d s\n", at50 - b + 1, at40 - b + 1);
    EXPECT(at40 >= 0 && at40 - b + 1 <= 12, "under 25 FPS: in to 40 m within 12 s");
    EXPECT(max_step(b, sec) <= 2 && at50 > b, "gliding: at most 2 m per second (no 5 m jumps)");
    EXPECT(g_cfg.q_char_lod == 40, "no closer than 40 m");
    int c = sec;
    run(40, 30);
    int out0 = -1;
    for (int i = c; i < sec && out0 < 0; i++)
        if (hist[i] > 40)
            out0 = i;
    int at60 = first_at(c, sec, 60);
    printf("        back at 30 FPS: moving out after %d s, 60 m after %d s\n", out0 - c + 1, at60 - c + 1);
    EXPECT(out0 >= 0 && out0 - c + 1 <= 10, "back at the cap: moving out within 10 s");
    EXPECT(max_step(c, sec) <= 1 && at60 >= 0 && at60 - c + 1 <= 31, "gliding out 1 m per second, at 60 m within 31 s");
    EXPECT(g_cfg.q_char_lod == 60, "no further than 60 m");
    int d = sec;
    run(20, 27);
    EXPECT(max_step(d, sec) == 0 && g_cfg.q_char_lod == 60, "27 FPS, between the two limits: it holds");
    /* a loading screen: one 3 s frame, then 22 FPS - the windows holding the stall are not judged */
    t += 3000, sec += 3;
    adaptive_frame_at(t, 3000.0);
    hist[sec - 1] = g_cfg.q_char_lod;
    int e = sec;
    run(2, 22);
    EXPECT(g_cfg.q_char_lod == 60, "within 3 s of a stall: not judged");
    run(2, 22);
    EXPECT(g_cfg.q_char_lod < 60, "then judged again: moving in");
    g_cfg.ab_enabled = 1;
    int before = g_cfg.q_char_lod;
    run(20, 15);
    EXPECT(g_cfg.q_char_lod == before, "an A/B cycle running: left alone");
    (void)e;

    /* what it costs: one call per frame */
    g_cfg.ab_enabled = 0;
    LARGE_INTEGER f, a0, a1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a0);
    run(1000, 30); /* 30 000 frames, 1000 seconds closed */
    QueryPerformanceCounter(&a1);
    double ns = (double)(a1.QuadPart - a0.QuadPart) * 1e9 / f.QuadPart / 30000.0;
    printf("        %.0f ns per frame on this PC (the second closing included)\n", ns);
    EXPECT(ns < 2000.0, "under 2 microseconds per frame (a 30 FPS frame lasts 33 333)");
    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
