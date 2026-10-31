/* Shadow-caster culling per cascade ([ShadowCull]).
 *
 * How the engine renders its cascaded shadow maps (METAPHOR.exe, found with tools\xref.py):
 *   - Every frame the scene submits each shadow-casting mesh ONCE into six "job bucket"
 *     lists (a list node holds a pointer to the mesh's pre-built depth-only command block).
 *   - The shadow stage then loops over the 3 cascades:
 *         for k in 0..2: if (cascade_begin(frame, k))   // set target k, clear, viewport, light matrix k
 *                            execute_buckets(frame);      // runs EVERY node of all six lists
 *     so every cascade draws the full caster list (the GPU probe measured 3 x ~875 draws,
 *     ~39M indices per frame, 62% of all draws), although most casters lie outside the
 *     small near cascades.
 *
 * This module skips, per cascade, the draw nodes whose bounding box is entirely outside
 * that cascade's light frustum (left/right/top/bottom; depth is never culled). A skipped
 * caster cannot put a single texel into that shadow map: the image is unchanged, the GPU
 * just stops transforming geometry that the clipper would throw away anyway.
 *
 * Data used, all of it engine data the draw itself uses:
 *   - cascade light view-projection: the table cascade_begin hands to the renderer
 *     (table + frame * 0x288 + k * 0x40);
 *   - the mesh's world matrix: read from the draw block's own "set world matrix" command;
 *   - the mesh's local bounding box (geometry + 0x90), found through the node's visibility
 *     record (geometry + 0xC0 + frame * 0x10), cross-checked against the block.
 * Anything that does not match the expected layout exactly is drawn as before.
 * Skinned meshes ([ShadowCull] AnimatedMarginPercent >= 0): their stored box is the rest pose,
 * which animation leaves (the PC check measured poses needing 50x its size), so they are
 * tested with a bound built from the current bone palette instead (see GEOM_MESH below).
 * Morphing meshes without a palette are never culled.
 *
 * Patches (three 5-byte call retargets, through stubs in int3 padding):
 *   - shadow stage: call cascade_begin  -> records (frame, cascade)
 *   - bucket executor: the two "interpret node command stream" calls -> sc_exec_a / sc_exec_b
 *
 * Verify=1 (PC diagnostic): nothing is skipped; every draw that WOULD be skipped is wrapped
 * in a D3D11 occlusion query. A correct culling gives 0 samples for all of them. */
#define COBJMACROS
#include "mcpu.h"
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- engine layout */
#define NODE_VISREC 0x38    /* node: per-mesh visibility record, NULL for nodes without one */
#define GEOM_FLAGS 0x20
#define GEOM_BBOX 0x90      /* float max[3], min[3] (set from the model file, flag 8) */
#define GEOM_VISREC 0xC0    /* + frame * 0x10 */
#define GEOM_TRANSFORM 0x1A8 /* -> matrices; world of frame f at +0xC0 + f * 0x40 */
#define GF_SKINNED 0x01
#define GF_HAS_BBOX 0x08
#define GF_DYNAMIC_VB 0x40  /* vertices rewritten every frame (skinning / morph output) */
/* Skinned geometry: its mesh object (+0x1B0) holds the list of its geometries (+0x38: int
 * count at +8, pointer array at +0x10) and the CPU bone palette (+0xF8: int count at +8,
 * 4x4 row-major matrices at +0x10). Palette i = inverseBind_i * boneWorld_i * R^-1
 * where R is the node whose world matrix the draw block uses as "world".
 * A skinned vertex is a weighted average of v_bind * P_i, so the box around
 * all P_i(rest-pose box) contains the posed mesh in R space, whatever the animation. */
#define GEOM_MESH 0x1B0
#define MESH_GEOMS 0x38
#define MESH_PALETTE 0xF8
#define VP_FRAME_STRIDE 0x288
#define VP_CASCADE_STRIDE 0x40
#define MAX_CASCADES 4

/* Shadow stage: the cascade loop. +0x30 call cascade_begin(frame, k), +0x3B call execute_buckets(frame) */
static const char SIG_SC_LOOP[] =
    "81 C9 00 00 00 0F E8 ?? ?? ?? ?? 48 39 58 28 74 14 48 8D 4C 24 28 48 89 6C 24 20 48 89 44 24 28 E8 ?? ?? ?? ?? "
    "FF C6 83 FE 06 72 D0 8B D3 8B CF E8 ?? ?? ?? ?? 84 C0 74 10 8B CF E8 ?? ?? ?? ?? 8B D3 8B CF E8 ?? ?? ?? ?? FF "
    "C3 83 FB 03 7C DC";
/* execute_buckets: prologue, then per bucket list A (+0x50, next at +0x28) and list B (+0xA0, next at +0x30) */
static const char SIG_SC_EXEC[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 63 E9 48 8D 35";
static const char SIG_SC_EXEC_A[] = "48 8B 0F 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 7F 28";
static const char SIG_SC_EXEC_B[] = "48 8B 0B 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 5B 30";
/* command-stream interpreter, mode 1 (deferred context of the shadow jobs) */
static const char SIG_SC_INTERP[] = "48 83 EC 48 48 89 4C 24 28 C7 44 24 38 01 00 00 00 48 8B 01 48 85 C0";
/* in cascade_begin: lea rax,[table]; shl rsi,6; imul rdx,rbp,0x288 */
static const char SIG_SC_VPTABLE[] = "48 8D 05 ?? ?? ?? ?? 48 C1 E6 06 48 69 D5 88 02 00 00";
/* draw-block builder: material + vertex-buffer commands (lea at +0 and +0x1E) */
static const char SIG_SC_BUILD1[] =
    "48 8D 05 ?? ?? ?? ?? 41 C7 41 08 02 00 00 00 49 89 01 49 89 49 10 49 83 C1 18 48 8B 43 08 4C 8D 05 ?? ?? ?? ?? "
    "8B 53 10 8B 4B 30 4D 89 01";
/* draw-block builder: world (+0x0E, matrix = transform + 0xC0 + frame * 0x40), previous world (+0x2D),
 * draw-indexed (+0x46), index buffer (+0x4F), third matrix (+0x65) */
static const char SIG_SC_BUILD2[] =
    "48 8B 5C 24 50 41 8D 4F 02 48 8B 6C 24 58 48 8D 05 ?? ?? ?? ?? 49 89 01 4D 8B C7 49 C1 E0 06 49 8D 86 C0 00 00 "
    "00 49 03 C0 49 89 41 08 48 8D 05 ?? ?? ?? ?? 49 89 41 10 B8 AB AA AA AA F7 E1 D1 EA 8D 04 52 2B C8 48 8D 15 ?? ?? "
    "?? ?? 8B C1 48 8D 0D ?? ?? ?? ?? 48 83 C0 03 48 C1 E0 06 49 03 C6 49 89 41 18 48 8D 05 ?? ?? ?? ?? 49 89 41 20 "
    "4B 8D 04 30 49 89 41 28";
/* builder prologue: transform = [geometry + 0x1A8] */
static const char SIG_SC_BUILD0[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B 82 B0 01 00 00 45 33 "
    "E4 4C 8B B2 A8 01 00 00";
/* layout assertions */
static const char SIG_SC_NODEINIT[] =
    "33 C0 48 89 41 08 48 89 41 10 48 89 41 18 48 89 41 20 48 8B 05 ?? ?? ?? ?? 48 89 41 38 C3";
static const char SIG_SC_VISREC[] = "8B 0D ?? ?? ?? ?? 48 83 C1 0C 48 C1 E1 04 49 03 CF E8";
static const char SIG_SC_SHADOWBLK[] =
    "8B 15 ?? ?? ?? ?? 41 8B CE 48 83 C2 08 48 C1 E2 05 4A 8B 14 3A 4C 8B 42 08 48 8B D3 4C 89 03";
static const char SIG_SC_BBOX[] =
    "48 89 5C 24 08 57 48 83 EC 20 C5 F8 10 02 48 8D 99 90 00 00 00 48 8B F9 C5 F8 11 03 C5 FB 10 4A 10 C5 FB 11 4B 10";
/* ... and its caller: the model's box (+0x90) is copied when the model has one
 * (flag 8). The setter itself is a jump into the protected code (call at +0x1A) */
static const char SIG_SC_BBOXCALL[] =
    "F6 47 20 08 8B 43 20 8B C8 4C 8B 64 24 58 74 14 48 8D 97 90 00 00 00 48 8B CB E8";

/* The draw-block builder is inside the protected code. Its command handlers are
 * found by their own code instead: world matrix and matrix 3 (the same code, +0x00 and +0x30), previous
 * world (+0x60), vertex buffer (+0x100), index buffer (+0x1A0), draw indexed (+0x3D0)
 * and material */
static const char SIG_SC_H_WM[] =
    "40 53 48 83 EC 20 48 8B 51 08 48 8B D9 8B 49 18 48 8B 52 08 E8 ?? ?? ?? ?? 48 83 43 08 10";
static const char SIG_SC_H_PREV[] = "40 53 48 81 EC A0 00 00 00 48 8B 41 08 48 8B D9 48 8B 50 08 48 85 D2 74 11";
static const char SIG_SC_H_VB[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 63 41 18 48 8B F9 48 8B 15 ?? ?? ?? ?? 48 8B 59 08";
static const char SIG_SC_H_IB[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 41 18 48 8B D9 48 8B 15 ?? ?? ?? ?? 48 8B 79 08";
static const char SIG_SC_H_DRAW[] = "48 89 5C 24 08 57 48 83 EC 30 48 8B 79 08 48 8B D9 8B 51 18 48 8B 0D ?? ?? ?? ?? E8";
static const char SIG_SC_H_MAT[] =
    "40 53 48 83 EC 20 48 8B 51 08 48 8B D9 48 63 41 18 4C 8B 15 ?? ?? ?? ?? 48 6B C8 78 4D 63 4A 10";

/* skinned meshes: palette at mesh+0xF8 (count +8, matrices +0x10), bind data at +0x100 */
static const char SIG_SC_PALETTE[] =
    "48 8B 8B F8 00 00 00 8B 51 08 4C 8B 49 10 48 8B 8B 00 01 00 00 48 8B 01 4C 8B 50 10";
/* the palette upload reads 4x4 matrices from mesh+0xF8 -> +0x10 */
static const char SIG_SC_PALUPLOAD[] = "48 8B 86 F8 00 00 00 49 8B 0E 8B 95 88 00 00 00 44 8B 68 08 48 8B 58 10";
/* each geometry of a mesh (mesh+0x38 -> +0x10) gets geometry+0x1B0 = mesh */
static const char SIG_SC_MESHGEOMS[] =
    "48 8B 46 38 48 8B 48 10 85 DB 74 18 48 8B D3 90 48 8B 01 48 8D 49 08 48 89 B0 B0 01 00 00";

/* camera update: view index, frame slot, then the record's view matrix
 * (record = base + (view * 3 + frame) * 0x59C, view matrix at +0x250) */
static const char SIG_SC_CAMVIEW[] =
    "8B 05 ?? ?? ?? ?? 48 8D 0C 40 8B 05 ?? ?? ?? ?? 48 03 C8 48 69 C1 9C 05 00 00 48 8D 0D ?? ?? ?? ?? C5 FC 11 04 08";
/* ... and its projection at +0x2D0, copied from the camera object */
static const char SIG_SC_CAMPROJ[] =
    "8B 05 ?? ?? ?? ?? 48 03 C8 48 8D 05 ?? ?? ?? ?? 48 69 D1 9C 05 00 00 48 8D 4D A0 48 03 D0 8B 43 60 89 02";
/* cascade setup: far split distance of cascade k stored at record base + an offset read from the
 * instruction (+0x12) + frame * 0x288 + k * 4, i.e. light
 * matrix table + 0x260 */
static const char SIG_SC_SPLITS[] = "48 69 C1 A2 00 00 00 48 03 C6 48 FF C6 C5 FA 11 B4 83 ?? ?? 00 00";

/* scene-pass executors (modes 0, 2 and 3): each
 * runs a node's command stream through the interpreter of its mode; node in rbx, like the
 * shadow list B call */
static const char SIG_SC_SCENESITE[] = "48 8B 0B 48 85 C9 74 05 E8 ?? ?? ?? ?? 48 8B 43 20 48 85 C0";
static const char SIG_SC_INTERP_M[] = "48 83 EC 48 48 89 4C 24 28 C7 44 24 38 ?? 00 00 00 48 8B 01 48 85 C0";
/* Scene buckets: per frame slot an array of buckets (modes 2/3) or one
 * bucket (mode 0). The executors take the slot as their argument; modes 2/3 keep it
 * in esi and the bucket in rdi across the node calls, mode 0 keeps the bucket in rdi. The add
 * functions file each node into the slot of the frame global, the
 * same global the geometry submission picks the node's visibility record with (SIG_SC_VISREC):
 * a scene node's geometry is its record - 0xC0 - slot * 0x10, exactly. */
static const char SIG_SC_SCENEEXEC[] =
    "48 89 74 24 20 57 48 83 EC 20 48 63 F1 44 2B C2 8B FA 48 8D 0D ?? ?? ?? ?? 48 C1 E7 06 48 03 3C F1";
static const char SIG_SC_SCENEEXEC0[] = "48 89 5C 24 08 57 48 83 EC 20 48 63 C1 48 8D 3D ?? ?? ?? ?? 48 8B 3C C7";
static const char SIG_SC_SCENEADD[] =
    "8B 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 44 8B C1 49 C1 E0 06 4D 03 04 C1 48 83 7A 38 00";
/* Character detail switch (per character, per frame): the distance from the hero
 * (a global pointer + 0xC0) to the character is compared with a float in .data (10000 cm by
 * default; MetaphorFix's [LOD] Distance writes it, x1000, found there first); at or beyond it the
 * character cross-fades to the game's simple stand-in model. vcomiss at +0x1D. */
static const char SIG_SC_CHARLOD[] = "C5 FA 10 5F 04 C5 EA 59 CA C5 DA 59 C4 C5 F2 58 D0 C5 D2 59 CD C5 EA 58 D1 C5 EA 51 C2 "
                                     "C5 F8 2F 05 ?? ?? ?? ?? C5 F8 29 7C 24 20 73";
/* Cascade setup: the near distance (light settings + 0x184) and the far distance
 * (+0x188, 20000 cm, or a debug override) are loaded into xmm13 / xmm12; the three split
 * distances (log/linear blend at 1/3 and 2/3) and the cascades' fit all follow from them. The
 * instruction after the far load (vmovss xmm1, [rip+X], +0x1C) is where a cap can be applied. */
static const char SIG_SC_SHADOWFAR[] = "C5 7A 10 A8 84 01 00 00 74 0A C5 7A 10 A0 88 01 00 00 EB 08 C5 7A 10 25 ?? ?? ?? ?? "
                                       "C5 FA 10 0D ?? ?? ?? ?? 8B 05";
/* ... and the end of the cascade setup: the frame-slot global, then the frame's last
 * matrix (whole range, table + 0x180) is written by the call at +0x1B. The three cascade matrices
 * (table + frame * 0x288 + k * 0x40) are final by then. */
static const char SIG_SC_SHADOWDONE[] = "8B 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 69 C9 88 02 00 00 48 8D 55 20 48 03 C8 E8";
static const char SIG_SC_SCENEADD0[] =
    "8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 14 C2 48 C7 41 28 00 00 00 00 48 83 7A 18 00";

static uint8_t* rel_target(uint8_t* insn, int disp_at, int len)
{
    int32_t d;
    memcpy(&d, insn + disp_at, 4);
    return insn + len + d;
}

static uint8_t* find_in(uint8_t* p, size_t len, const char* sig)
{
    int n = 0;
    uint8_t* r = sig_find(p, len, sig, &n);
    return n >= 1 ? r : NULL;
}

/* `sig` matches exactly at p */
static int at(uint8_t* p, const char* sig)
{
    return sig_find(p, 64, sig, NULL) == p;
}

/* The draw-block command handlers by their own code (see SIG_SC_H_WM) */
static int handlers_by_code(uint8_t* text, size_t len, ShadowSites* s)
{
    int n = 0;
    uint8_t* mat = sig_find(text, len, SIG_SC_H_MAT, &n);
    if (n != 1)
        return 0;
    uint8_t* p = text;
    size_t left = len;
    while (left > 64) {
        uint8_t* w = sig_find(p, left, SIG_SC_H_WM, NULL);
        if (!w)
            return 0;
        if (at(w + 0x30, SIG_SC_H_WM) && at(w + 0x60, SIG_SC_H_PREV) && at(w + 0x100, SIG_SC_H_VB) &&
            at(w + 0x1A0, SIG_SC_H_IB) && at(w + 0x3D0, SIG_SC_H_DRAW)) {
            s->h_world = w, s->h_mat3 = w + 0x30, s->h_prev = w + 0x60;
            s->h_vb = w + 0x100, s->h_ib = w + 0x1A0, s->h_draw = w + 0x3D0, s->h_material = mat;
            return 1;
        }
        left -= (size_t)(w + 1 - p);
        p = w + 1;
    }
    return 0;
}

/* the geometry's bounding box at +0x90: its setter, or its caller with a jump stub as the setter */
static int bbox_layout_ok(uint8_t* text, size_t len)
{
    if (find_in(text, len, SIG_SC_BBOX))
        return 1;
    uint8_t* c = find_in(text, len, SIG_SC_BBOXCALL);
    return c && rel_target(c + 0x1A, 1, 5)[0] == 0xE9;
}

/* Resolve every site. Works on the live module or on an image mapped by tests\scan_test.c. */
int shadowcull_resolve(uint8_t* base, ShadowSites* s, int verbose)
{
    uint8_t* text;
    size_t len;
    memset(s, 0, sizeof(*s));
    if (!sig_text_range(base, &text, &len))
        return 0;
    int n = 0;
    uint8_t* loop = sig_find(text, len, SIG_SC_LOOP, &n);
    if (n != 1) {
        if (verbose)
            LOGI("  shadow cull: cascade loop signature %d matches (need 1)", n);
        return 0;
    }
    s->begin_site = loop + 0x30;
    s->exec_site = loop + 0x3B;
    s->begin = rel_target(s->begin_site, 1, 5);
    s->exec = rel_target(s->exec_site, 1, 5);
    const char* fail = NULL;
    uint8_t* b1 = NULL;
    uint8_t* b2 = NULL;
    uint8_t* vt = NULL;
    if (sig_find(s->exec, 32, SIG_SC_EXEC, NULL) != s->exec)
        fail = "bucket executor prologue";
    else if (!sig_find(s->exec + 0x50, 17, SIG_SC_EXEC_A, NULL))
        fail = "bucket list A call";
    else if (!sig_find(s->exec + 0xA0, 17, SIG_SC_EXEC_B, NULL))
        fail = "bucket list B call";
    if (!fail) {
        s->exec_a_site = s->exec + 0x58;
        s->exec_b_site = s->exec + 0xA8;
        s->interp = rel_target(s->exec_a_site, 1, 5);
        if (rel_target(s->exec_b_site, 1, 5) != s->interp || sig_find(s->interp, 23, SIG_SC_INTERP, NULL) != s->interp)
            fail = "command interpreter";
    }
    if (!fail) {
        vt = find_in(s->begin, 0x180, SIG_SC_VPTABLE);
        if (!vt)
            fail = "cascade matrix table";
        else
            s->vp_table = rel_target(vt, 3, 7);
    }
    if (!fail) {
        b1 = sig_find(text, len, SIG_SC_BUILD1, &n);
        b2 = (b1 && n == 1) ? find_in(b1, 0x200, SIG_SC_BUILD2) : NULL;
        if (b2 && find_in(b1 - 0xA0, 0xA0, SIG_SC_BUILD0)) {
            s->h_material = rel_target(b1, 3, 7);
            s->h_vb = rel_target(b1 + 0x1E, 3, 7);
            s->h_world = rel_target(b2 + 0x0E, 3, 7);
            s->h_prev = rel_target(b2 + 0x2D, 3, 7);
            s->h_draw = rel_target(b2 + 0x46, 3, 7);
            s->h_ib = rel_target(b2 + 0x4F, 3, 7);
            s->h_mat3 = rel_target(b2 + 0x65, 3, 7);
        } else if (!handlers_by_code(text, len, s)) {
            fail = "draw-block builder";
        }
    }
    if (!fail) {
        if (!find_in(text, len, SIG_SC_NODEINIT))
            fail = "list node layout";
        else if (!find_in(text, len, SIG_SC_VISREC))
            fail = "visibility record layout";
        else if (!find_in(text, len, SIG_SC_SHADOWBLK))
            fail = "shadow draw block layout";
        else if (!bbox_layout_ok(text, len))
            fail = "bounding box layout";
    }
    /* optional: bone palettes of skinned meshes (animated casters); static culling works without */
    if (!fail)
        s->anim_ok = find_in(text, len, SIG_SC_PALETTE) && find_in(text, len, SIG_SC_PALUPLOAD) &&
                     find_in(text, len, SIG_SC_MESHGEOMS);
    /* optional: the scene-pass interpreters (modes 0, 2, 3) and their call sites, for the
     * [Quality] character options */
    if (!fail) {
        int n = 0;
        uint8_t* p = text;
        size_t left = len;
        while (left > 32) {
            int cnt = 0;
            uint8_t* m = sig_find(p, left, SIG_SC_INTERP_M, &cnt);
            if (!m)
                break;
            int mode = m[13];
            if (mode >= 0 && mode < 4)
                s->scene_interp[mode] = m;
            left -= (size_t)(m + 1 - p);
            p = m + 1;
        }
        p = text, left = len;
        while (left > 32 && n < 8) {
            uint8_t* m = sig_find(p, left, SIG_SC_SCENESITE, NULL);
            if (!m)
                break;
            uint8_t* tgt = rel_target(m + 8, 1, 5);
            for (int mode = 0; mode < 4; mode++)
                if (mode != 1 && tgt == s->scene_interp[mode] && tgt) {
                    s->scene_site[n] = m + 8, s->scene_mode[n] = mode, n++;
                    break;
                }
            left -= (size_t)(m + 1 - p);
            p = m + 1;
        }
        s->nscene = n;
    }
    /* optional: the scene buckets' frame-slot tables, cross-checked as described at SIG_SC_SCENEEXEC
     * (for the [Quality] distance options) */
    if (!fail && s->nscene) {
        uint8_t* vr = find_in(text, len, SIG_SC_VISREC);
        uint8_t* add = find_in(text, len, SIG_SC_SCENEADD);
        uint8_t* add0 = find_in(text, len, SIG_SC_SCENEADD0);
        uint8_t* fg = vr ? rel_target(vr, 2, 6) : NULL;
        int ok = vr && add && add0 && rel_target(add, 2, 6) == fg && rel_target(add0, 2, 6) == fg;
        uint8_t* tab = ok ? rel_target(add + 6, 3, 7) : NULL;
        uint8_t* tab0 = ok ? rel_target(add0 + 6, 3, 7) : NULL;
        for (int i = 0; ok && i < s->nscene; i++) {
            uint8_t* site = s->scene_site[i];
            if (s->scene_mode[i] == 0) {
                uint8_t* ex = site - 0x4F;
                ok = sig_find(ex, 24, SIG_SC_SCENEEXEC0, NULL) == ex && rel_target(ex + 0x0D, 3, 7) == tab0;
            } else {
                uint8_t* ex = NULL;
                for (int back = 0x7F; back <= 0xFF && !ex; back += 0x80) /* first or second node loop */
                    if (sig_find(site - back, 34, SIG_SC_SCENEEXEC, NULL) == site - back)
                        ex = site - back;
                ok = ex && rel_target(ex + 0x12, 3, 7) == tab;
            }
        }
        if (ok)
            s->scene_tab = tab, s->scene_tab0 = tab0, s->scene_ok = 1;
    }
    /* optional: the shadow distance */
    if (!fail) {
        uint8_t* sf = find_in(text, len, SIG_SC_SHADOWFAR);
        if (sf)
            s->shadow_far_site = sf + 0x1C;
        uint8_t* sd = find_in(text, len, SIG_SC_SHADOWDONE);
        if (sd && rel_target(sd + 6, 3, 7) == s->vp_table + 0x180) {
            s->shadow_done_site = sd + 0x1B;
            s->frame_slot = (volatile int*)rel_target(sd, 2, 6);
        }
    }
    /* optional: the character detail distance */
    if (!fail) {
        uint8_t* cl = find_in(text, len, SIG_SC_CHARLOD);
        float* v = cl ? (float*)rel_target(cl + 0x1D, 4, 8) : NULL;
        if (v && *v >= 100.0f && *v <= 1e6f) /* a distance in cm (MetaphorFix: 1000 .. 100000) */
            s->char_lod = v;
    }
    /* optional: camera records for the view-aware test */
    if (!fail) {
        uint8_t* cv = find_in(text, len, SIG_SC_CAMVIEW);
        uint8_t* cp = find_in(text, len, SIG_SC_CAMPROJ);
        uint8_t* sp = find_in(text, len, SIG_SC_SPLITS);
        if (cv && cp && sp) {
            uint8_t* view = rel_target(cv + 26, 3, 7);
            uint8_t* proj = rel_target(cp + 9, 3, 7);
            uint8_t* base = view - 0x250;
            int32_t split_off;
            memcpy(&split_off, sp + 0x12, 4);
            if (proj == base + 0x2D0 && base + split_off == s->vp_table + 0x260) {
                s->cam_base = base;
                s->cam_view_idx = (volatile int*)rel_target(cv, 2, 6);
                s->view_ok = 1;
            }
        }
    }
    if (verbose) {
        if (fail)
            LOGI("  shadow cull: %s not found - culling unavailable", fail);
        else
            LOGI("  shadow cull: cascade loop rva=0x%llx, executor rva=0x%llx, interpreter rva=0x%llx, light matrices "
                 "rva=0x%llx, draw handler rva=0x%llx, bone palettes %s, camera records %s, scene buckets %s, "
                 "character detail distance %s, shadow distance %s, shadow refresh %s",
                 (unsigned long long)(loop - base), (unsigned long long)(s->exec - base),
                 (unsigned long long)(s->interp - base), (unsigned long long)(s->vp_table - base),
                 (unsigned long long)((uint8_t*)s->h_draw - base), s->anim_ok ? "found" : "NOT found",
                 s->view_ok ? "found" : "NOT found", s->scene_ok ? "found" : "NOT found",
                 s->char_lod ? "found" : "NOT found", s->shadow_far_site ? "found" : "NOT found",
                 s->shadow_done_site ? "found" : "NOT found");
    }
    return fail ? 0 : 1;
}

/* ---------------------------------------------------------------- state */
static ShadowSites S;
static int g_installed;
typedef void (*interp_t)(void* cmd);
typedef uint8_t (*begin_t)(int frame, int k);
static interp_t o_interp;
static begin_t o_begin;

/* Current cascade of the calling thread: cascade_begin fills it, the bucket executor (called
 * right after it on the same thread) reads it. Per thread, so two shadow stages running at
 * once could not mix their light matrices. */
/* per-frame bound of a skinned mesh (R space), shared by its geometries and the 3 cascades */
#define MB_N 512
typedef struct MeshBound {
    void* mesh;
    uint32_t seq;
    int ok, bones;
    float bb[6]; /* min xyz, max xyz */
} MeshBound;

/* Receivers of a cascade for the view-aware test: the convex polygon that the camera's view
 * slice of this cascade covers in the light's clip x/y, a*x + b*y + c >= 0 inside each edge. */
#define RECV_MAX 8
typedef struct Recv {
    int n; /* edges, 0 = not usable for this pass */
    float e[RECV_MAX][3];
} Recv;

typedef struct Cascade {
    int frame, k;
    int usable;    /* light matrix finite and non-zero */
    float vp[16];  /* row-vector form (the engine computes world x light per row) */
    uint32_t seq;  /* shadow stages seen by this thread (+1 at cascade 0): mesh bound cache key */
    int view;      /* camera view matched in this stage, -1 = none yet */
    Recv recv;
    /* [Quality] distance options: main camera position for a frame slot (cache) */
    int cam_slot, cam_ok, cam_calls;
    float cam[3];
    float cam_p11; /* projection [1][1] = cot(fovY / 2): on-screen size */
    int cam_vp_ok; /* the view and projection below passed camera_ok (occlusion census: inside the view?) */
    float cam_v[16], cam_pm[16];
    MeshBound mb[MB_N];
} Cascade;
static DWORD g_tls = TLS_OUT_OF_INDEXES;

static Cascade* cascade_of_thread(int create)
{
    Cascade* c = (Cascade*)TlsGetValue(g_tls);
    if (!c && create) {
        c = (Cascade*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Cascade));
        TlsSetValue(g_tls, c);
    }
    return c;
}

typedef struct CasStats {
    volatile LONG64 passes, nodes_a, nodes_b, eligible, culled, idx_eligible, idx_culled;
    volatile LONG64 anim_eligible, anim_culled, idx_anim_eligible, idx_anim_culled;
    volatile LONG64 view_static, idx_view_static, view_anim, idx_view_anim; /* view-aware test */
    volatile LONG64 view_matched, view_nomatch, view_unusable;
    volatile LONG64 tiny, idx_tiny, tiny_skipped; /* static casters under 1 texel; left out by TinyCasterTexels */
} CasStats;
static CasStats g_cs[MAX_CASCADES];
static volatile LONG64 r_skinned, r_nobox, r_block, r_mismatch, r_matrix, vp_affine, vp_proj, vp_bad;
static volatile LONG64 r_nobound; /* skinned, but no usable bone palette / mesh data */
static volatile LONG64 t_decide_ticks, t_decide_samples, t_decide_all;
static volatile LONG64 v_checked, v_drew, v_samples, v_unread, v_noquery;
static uint64_t g_qpf;

/* Animated (skinned / morphing) casters: their stored box is the bind pose, so a pose can
 * reach outside it. For each such caster outside a cascade we compute the margin it would
 * need to be kept ("need", as a fraction of the box's largest half extent added on every
 * side). Verify mode measures them all; the report then shows, per candidate margin, how
 * many draws it would skip and whether any of those drew pixels. */
#define AM_N 7
static const float am_margin[AM_N] = {0.10f, 0.25f, 0.50f, 1.00f, 2.00f, 4.00f, 8.00f};
static volatile LONG64 va_checked, va_drew, va_skip[AM_N], va_skip_idx[AM_N], va_unsafe[AM_N];
static volatile LONG va_need_max_bits; /* float bits of the largest need among draws with pixels */

/* verification metadata per query slot */
#define VQ 8192
typedef struct VMeta {
    void* geom;
    int k;
    uint32_t count;
    float lmax[4]; /* largest value of each side-plane function over the box (< 0 = outside) */
    int animated;
    float need;    /* animated: margin needed to be kept (fraction of the largest half extent) */
    const float* world;
    float box[6];  /* box tested (min, max): stored box, or the bone bound of a skinned mesh */
    int bones;     /* skinned: palette size, 0 otherwise */
    int viewcull;  /* inside the cascade, but its shadow cannot reach the camera's view slice */
} VMeta;
static VMeta g_vm[VQ];
static SRWLOCK g_vlog_lock = SRWLOCK_INIT;
static int g_vlogged;

static int finite_all(const float* v, int n)
{
    for (int i = 0; i < n; i++)
        if (!(v[i] == v[i]) || fabsf(v[i]) > 1e30f)
            return 0;
    return 1;
}

/* Side planes of the clip volume in homogeneous coordinates, for p_clip = (x, y, z, 1) * W * VP:
 *   x <= s*w,  -x <= s*w,  y <= s*w,  -y <= s*w    (s = 1 + margin)
 * Each is a linear function of the local position, so its largest value over the box is
 * exact: value at the centre + sum |coefficient * half extent|. If that largest value is
 * negative for any one plane, every point of the box is outside the clip volume and the
 * rasteriser could not produce a single pixel from it. Works for any projective light
 * matrix (orthographic or perspective, any overall scale, w of either sign). */
/* columns x, y, w of W * VP: clip (x, y, w) of a local point = (p, 1) * M */
static void light_columns(const float* W, const float* vp, float M[4][3])
{
    static const int col[3] = {0, 1, 3};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) {
            int c = col[j];
            M[i][j] = W[i * 4 + 0] * vp[0 * 4 + c] + W[i * 4 + 1] * vp[1 * 4 + c] + W[i * 4 + 2] * vp[2 * 4 + c] +
                      W[i * 4 + 3] * vp[3 * 4 + c];
        }
}

static void plane_maxima(const float M[4][3], const float* bb, float s, float* lmax, float* slope)
{
    float ctr[3], ext[3];
    for (int i = 0; i < 3; i++)
        ctr[i] = (bb[i] + bb[i + 3]) * 0.5f, ext[i] = (bb[i + 3] - bb[i]) * 0.5f;
    static const float sx[4] = {-1, 1, 0, 0}, sy[4] = {0, 0, -1, 1};
    for (int p = 0; p < 4; p++) {
        float v = 0, r = 0, g = 0;
        for (int i = 0; i < 4; i++) {
            float a = s * M[i][2] + sx[p] * M[i][0] + sy[p] * M[i][1];
            if (i < 3)
                v += a * ctr[i], r += fabsf(a * ext[i]), g += fabsf(a);
            else
                v += a;
        }
        lmax[p] = v + r;
        /* growing every half extent by d raises the maximum by d * sum|a| */
        if (slope)
            slope[p] = g;
    }
}

/* Walk a draw block: VB, [material], world, previous world, matrix 3, IB, one indexed draw, end. */
static int parse_block(const uint8_t* c, const float** world, uint32_t* count)
{
    const float* w = NULL;
    uint32_t n = 0;
    int draws = 0;
    for (int i = 0; i < 16; i++) {
        void* h = *(void* const*)c;
        if (!h) {
            if (draws != 1 || !w)
                return 0;
            *world = w, *count = n;
            return 1;
        }
        if (h == S.h_vb)
            c += 0x28;
        else if (h == S.h_world)
            w = *(const float* const*)(c + 8), c += 0x10;
        else if (h == S.h_prev || h == S.h_mat3)
            c += 0x10;
        else if (h == S.h_material || h == S.h_ib)
            c += 0x18;
        else if (h == S.h_draw)
            n += *(const uint32_t*)(c + 0xC), draws++, c += 0x18;
        else
            return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- view-aware test */
/* A caster inside a cascade still cannot change the picture if its shadow falls where no
 * visible pixel samples that cascade. The engine picks a cascade by view depth (it stores the
 * split distances next to the light matrices), so the pixels that read cascade k lie in the
 * camera frustum between its split distances. The light's clip space keeps light rays parallel
 * to clip z (an orthographic light, or a perspective warp perpendicular to the light), so a
 * caster can only shadow pixels whose clip x/y it covers: if its box lies outside the polygon
 * of that frustum slice in clip x/y, none of its texels is ever read. */
#define CAM_STRIDE 0x59C
#define CAM_VIEW 0x250
#define CAM_PROJ 0x2D0
#define FRAME_SPLITS 0x260 /* light matrix table + frame * 0x288: far split distance per cascade */
#define MAX_VIEWS 4

static volatile LONG64 view_bad_split;

/* view matrix (row vectors, rotation orthonormal) and perspective projection as the engine builds them */
static int camera_ok(const float* V, const float* P)
{
    if (!finite_all(V, 16) || !finite_all(P, 16))
        return 0;
    if (fabsf(V[3]) > 1e-3f || fabsf(V[7]) > 1e-3f || fabsf(V[11]) > 1e-3f || fabsf(V[15] - 1) > 1e-3f)
        return 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float d = V[i * 4] * V[j * 4] + V[i * 4 + 1] * V[j * 4 + 1] + V[i * 4 + 2] * V[j * 4 + 2];
            if (fabsf(d - (i == j ? 1.0f : 0.0f)) > 2e-3f)
                return 0;
        }
    return P[0] > 1e-3f && P[5] > 1e-3f && fabsf(P[11] + 1) < 1e-3f && fabsf(P[15]) < 1e-3f;
}

/* Corners of the camera frustum between view depths d0 and d1 (half widths tx, ty per unit of
 * depth; the camera looks down -z), in the light's clip x/y. 0 if a corner is not in front of
 * the light projection (w <= 0). */
static int slice_corners(const float* V, float tx, float ty, float d0, float d1, const float* vp, float xy[8][2])
{
    int n = 0;
    for (int i = 0; i < 2; i++) {
        float d = i ? d1 : d0;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                float q[3] = {sx * d * tx - V[12], sy * d * ty - V[13], -d - V[14]}, p[3];
                for (int j = 0; j < 3; j++) /* world = (view - t) * R^T */
                    p[j] = q[0] * V[j * 4] + q[1] * V[j * 4 + 1] + q[2] * V[j * 4 + 2];
                float X = p[0] * vp[0] + p[1] * vp[4] + p[2] * vp[8] + vp[12];
                float Y = p[0] * vp[1] + p[1] * vp[5] + p[2] * vp[9] + vp[13];
                float Wc = p[0] * vp[3] + p[1] * vp[7] + p[2] * vp[11] + vp[15];
                if (!(Wc > 0) || !finite_all(&X, 1) || !finite_all(&Y, 1))
                    return 0;
                xy[n][0] = X / Wc, xy[n][1] = Y / Wc;
                n++;
            }
    }
    return 1;
}

/* The engine fits each cascade around exactly this slice, so a camera record
 * that belongs to the frame being drawn puts every corner inside the cascade. */
static int camera_matches(const float* V, const float* P, float d0, float d1, const float* vp)
{
    float xy[8][2];
    if (!slice_corners(V, 1.0f / P[0], 1.0f / P[5], d0, d1, vp, xy))
        return 0;
    for (int i = 0; i < 8; i++)
        if (fabsf(xy[i][0]) > 1.05f || fabsf(xy[i][1]) > 1.05f)
            return 0;
    return 1;
}

static float cross2(const float* o, const float* a, const float* b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

/* convex hull, counter-clockwise (monotone chain); returns the vertex count */
static int hull2(float p[8][2], float h[RECV_MAX + 1][2])
{
    for (int i = 1; i < 8; i++) /* sort by x, then y */
        for (int j = i; j > 0 && (p[j][0] < p[j - 1][0] || (p[j][0] == p[j - 1][0] && p[j][1] < p[j - 1][1])); j--) {
            float t0 = p[j][0], t1 = p[j][1];
            p[j][0] = p[j - 1][0], p[j][1] = p[j - 1][1], p[j - 1][0] = t0, p[j - 1][1] = t1;
        }
    float q[16][2];
    int k = 0;
    for (int i = 0; i < 8; i++) {
        while (k >= 2 && cross2(q[k - 2], q[k - 1], p[i]) <= 0)
            k--;
        q[k][0] = p[i][0], q[k][1] = p[i][1], k++;
    }
    for (int i = 6, t = k + 1; i >= 0; i--) {
        while (k >= t && cross2(q[k - 2], q[k - 1], p[i]) <= 0)
            k--;
        q[k][0] = p[i][0], q[k][1] = p[i][1], k++;
    }
    k--; /* the last point repeats the first */
    if (k > RECV_MAX)
        return 0;
    for (int i = 0; i < k; i++)
        h[i][0] = q[i][0], h[i][1] = q[i][1];
    return k;
}

/* Receiver polygon of the slice d0..d1, every edge moved outward by delta (clip units). */
static int receivers_from(const float* V, float tx, float ty, float d0, float d1, const float* vp, float delta, Recv* r)
{
    float xy[8][2], h[RECV_MAX + 1][2];
    r->n = 0;
    if (!slice_corners(V, tx, ty, d0, d1, vp, xy))
        return 0;
    int n = hull2(xy, h);
    if (n < 3)
        return 0;
    for (int i = 0; i < n; i++) {
        const float* a = h[i];
        const float* b = h[(i + 1) % n];
        float ex = -(b[1] - a[1]), ey = b[0] - a[0], len = sqrtf(ex * ex + ey * ey);
        if (!(len > 1e-9f))
            return 0;
        ex /= len, ey /= len;
        r->e[i][0] = ex, r->e[i][1] = ey, r->e[i][2] = -(ex * a[0] + ey * a[1]) + delta;
    }
    r->n = n;
    return 1;
}

/* 1 if every point of the box with w > 0 lies outside one edge of the receiver polygon (points
 * with w <= 0 are clipped and draw nothing). Exact per edge: a*x + b*y + c*w is linear in the
 * local position, so its maximum over the box is centre value + sum |coefficient * half extent|. */
static int outside_receivers(const float M[4][3], const float* bb, const Recv* r)
{
    float ctr[3], ext[3];
    for (int i = 0; i < 3; i++)
        ctr[i] = (bb[i] + bb[i + 3]) * 0.5f, ext[i] = (bb[i + 3] - bb[i]) * 0.5f;
    for (int e = 0; e < r->n; e++) {
        float a = r->e[e][0], b = r->e[e][1], c = r->e[e][2], v = 0;
        for (int i = 0; i < 4; i++) {
            float g = a * M[i][0] + b * M[i][1] + c * M[i][2];
            v += i < 3 ? g * ctr[i] + fabsf(g * ext[i]) : g;
        }
        if (v < 0)
            return 1;
    }
    return 0;
}

/* At cascade_begin: find the camera record of the frame being drawn (the update may already
 * be working on another view), check it against the cascade, build the receiver polygon. */
static void build_receivers(Cascade* c, int frame, int k)
{
    CasStats* st = &g_cs[k];
    const float* sp = (const float*)(S.vp_table + frame * VP_FRAME_STRIDE + FRAME_SPLITS);
    float s0 = k ? sp[k - 1] : 0.0f, s1 = sp[k];
    if (!finite_all(sp, 3) || !(s0 >= 0) || !(s1 > s0) || s1 > 1e7f) {
        view_bad_split++;
        return;
    }
    int first = c->view >= 0 ? c->view : *S.cam_view_idx;
    for (int t = 0; t <= MAX_VIEWS; t++) {
        int v = t ? t - 1 : first;
        if (v < 0 || v >= MAX_VIEWS || (t && v == first))
            continue;
        const uint8_t* rec = S.cam_base + (size_t)(v * 3 + frame) * CAM_STRIDE;
        const float* V = (const float*)(rec + CAM_VIEW);
        const float* P = (const float*)(rec + CAM_PROJ);
        if (!camera_ok(V, P) || !camera_matches(V, P, s0, s1, c->vp))
            continue;
        c->view = v;
        float m = g_cfg.shadow_view_depth_pct / 100.0f;
        float d0 = s0 * (1.0f - m), d1 = s1 * (1.0f + m);
        float delta = 0.02f + g_cfg.shadow_margin_pct * 0.02f; /* filter footprint + the cascade margin */
        if (!receivers_from(V, 1.05f / P[0], 1.05f / P[5], d0 > 0 ? d0 : 0.0f, d1, c->vp, delta, &c->recv)) {
            st->view_unusable++;
            return;
        }
        st->view_matched++;
        static volatile LONG logged;
        LONG n = InterlockedIncrement(&logged);
        if (n <= 3 || n % 9000 < 3) {
            char line[300];
            int w = 0;
            float xy[8][2], h[RECV_MAX + 1][2];
            slice_corners(V, 1.05f / P[0], 1.05f / P[5], d0 > 0 ? d0 : 0.0f, d1, c->vp, xy);
            int hn = hull2(xy, h);
            for (int i = 0; i < hn && w < (int)sizeof(line) - 24; i++)
                w += snprintf(line + w, sizeof(line) - w, " (%.2f %.2f)", h[i][0], h[i][1]);
            LOGI("Shadow cull: view slice cascade %d (camera view %d): depth %.1f..%.1f (splits %.1f %.1f %.1f), "
                 "field of view %.0fx%.0f deg, receiver polygon in light clip x/y:%s",
                 k, v, d0 > 0 ? d0 : 0.0, d1, sp[0], sp[1], sp[2], 2 * atan(1.0 / P[0]) * 57.29578,
                 2 * atan(1.0 / P[5]) * 57.29578, line);
        }
        return;
    }
    st->view_nomatch++;
}

/* ---------------------------------------------------------------- skinned meshes */
#define MAX_BONES 1024
#define MAX_MESH_GEOMS 256

static int plausible_ptr(const void* p)
{
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && v < ((uintptr_t)1 << 47) && (v & 7) == 0;
}

/* Bound of a skinned mesh in the space of its reference node (whose world matrix the draw
 * uses): the box around every palette matrix applied to the rest-pose box of the mesh's
 * skinned geometries. Only the matrices' first three columns are used, like the GPU (the
 * upload keeps a transposed 3x4); the fourth must still read 0 0 0 1, which confirms the
 * row-vector layout. */
static int compute_mesh_bound(uint8_t* mesh, int ng, uint8_t* const* ga, float* u, int* bones)
{
    float rb[6] = {INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < ng; i++) {
        uint32_t f = *(const uint32_t*)(ga[i] + GEOM_FLAGS);
        if (!(f & GF_SKINNED))
            continue;
        const float* raw = (const float*)(ga[i] + GEOM_BBOX);
        if (!(f & GF_HAS_BBOX) || !finite_all(raw, 6))
            return 0;
        for (int a = 0; a < 3; a++) {
            rb[a] = fminf(rb[a], fminf(raw[a], raw[a + 3]));
            rb[a + 3] = fmaxf(rb[a + 3], fmaxf(raw[a], raw[a + 3]));
        }
    }
    if (!(rb[0] <= rb[3])) /* no skinned geometry */
        return 0;
    uint8_t* pal = *(uint8_t**)(mesh + MESH_PALETTE);
    if (!plausible_ptr(pal))
        return 0;
    int n = *(const int*)(pal + 8);
    const float* m = *(const float* const*)(pal + 0x10);
    if (n < 1 || n > MAX_BONES || !plausible_ptr(m))
        return 0;
    float c[3], e[3];
    for (int a = 0; a < 3; a++)
        c[a] = (rb[a] + rb[a + 3]) * 0.5f, e[a] = (rb[a + 3] - rb[a]) * 0.5f;
    for (int a = 0; a < 3; a++)
        u[a] = INFINITY, u[a + 3] = -INFINITY;
    /* branch-free per bone: `bad` collects a 4th column that is not 0 0 0 1 (NaN compares
     * false, so it counts too); `acc` turns NaN / inf if any used entry is one (fminf and
     * fmaxf would silently skip a NaN) */
    int bad = 0;
    float acc = 0;
    for (int b = 0; b < n; b++, m += 16) {
        bad |= !(fabsf(m[3]) <= 1e-3f) | !(fabsf(m[7]) <= 1e-3f) | !(fabsf(m[11]) <= 1e-3f) |
               !(fabsf(m[15] - 1.0f) <= 1e-3f);
        for (int j = 0; j < 3; j++) { /* out_j = sum_k v_k m[k][j] + m[3][j] */
            float cj = c[0] * m[j] + c[1] * m[4 + j] + c[2] * m[8 + j] + m[12 + j];
            float ej = fabsf(e[0] * m[j]) + fabsf(e[1] * m[4 + j]) + fabsf(e[2] * m[8 + j]);
            acc += cj + ej;
            u[j] = fminf(u[j], cj - ej), u[j + 3] = fmaxf(u[j + 3], cj + ej);
        }
    }
    if (bad || !finite_all(&acc, 1) || !finite_all(u, 6))
        return 0;
    *bones = n;
    return 1;
}

/* The bound of `geom`'s mesh, computed once per mesh per shadow stage (per-thread cache). */
static int mesh_bound(Cascade* cas, uint8_t* geom, float* bb, int* bones)
{
    uint8_t* mesh = *(uint8_t**)(geom + GEOM_MESH);
    if (!plausible_ptr(mesh))
        return 0;
    /* the mesh must list this geometry among its parts */
    uint8_t* gl = *(uint8_t**)(mesh + MESH_GEOMS);
    if (!plausible_ptr(gl))
        return 0;
    int ng = *(const int*)(gl + 8);
    uint8_t* const* ga = *(uint8_t* const* const*)(gl + 0x10);
    if (ng < 1 || ng > MAX_MESH_GEOMS || !plausible_ptr(ga))
        return 0;
    int found = 0;
    for (int i = 0; i < ng; i++) {
        if (!plausible_ptr(ga[i]))
            return 0;
        found |= ga[i] == geom;
    }
    if (!found)
        return 0;
    MeshBound* e = &cas->mb[(uint32_t)(((uintptr_t)mesh >> 4) * 2654435761u) >> 23];
    if (e->mesh != mesh || e->seq != cas->seq) {
        e->mesh = mesh, e->seq = cas->seq;
        e->ok = compute_mesh_bound(mesh, ng, ga, e->bb, &e->bones);
    }
    if (!e->ok)
        return 0;
    memcpy(bb, e->bb, sizeof(e->bb));
    *bones = e->bones;
    return 1;
}

enum { D_KEEP = 0, D_CULL = 1, D_INELIGIBLE = 2, D_QSKIP = 3 /* [Quality] CharacterShadows=0 */ };
static volatile LONG64 q_skip_nodes[MAX_CASCADES]; /* shadow draws left out by the [Quality] options */

/* Decide for one list-B node in cascade `cas`. */
/* Characters: the skinned geometries seen in the shadow lists (flag, layout and block checked
 * there). Their visibility records (one per frame slot) are what the scene-pass nodes point
 * at, so a scene node is a character's when its record is in this set - a pointer compare. */
#define CV_N 16384
static void* volatile g_cv[CV_N];
static volatile LONG g_cv_count;

static unsigned cv_hash(const void* p)
{
    return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (CV_N - 1);
}

static void cv_add(void* v)
{
    for (unsigned i = 0, k = cv_hash(v); i < 64; i++, k = (k + 1) & (CV_N - 1)) {
        void* cur = g_cv[k];
        if (cur == v)
            return;
        if (!cur && InterlockedCompareExchangePointer((PVOID volatile*)&g_cv[k], v, NULL) == NULL) {
            InterlockedIncrement(&g_cv_count);
            return;
        }
    }
}

static int cv_has(const void* v)
{
    for (unsigned i = 0, k = cv_hash(v); i < 64; i++, k = (k + 1) & (CV_N - 1)) {
        void* cur = g_cv[k];
        if (cur == v)
            return 1;
        if (!cur)
            return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- distance from the camera ([Quality]) */
/* The "bubble": objects farther than [Quality] FarDistance from the main camera (view 0). The
 * engine works in centimetres (the cascade splits are 846 / 2880 / 20000). */
#define UNITS_PER_M 100.0f
static volatile LONG64 g_frame_seq; /* shadow stages started (one per frame): ages the hidden-character list */

/* camera position from a view matrix [R 0; t 1] (row vectors, R orthonormal): -t R^T */
static int cam_pos_from_view(const float* V, float* p)
{
    if (!finite_all(V, 16))
        return 0;
    for (int j = 0; j < 3; j++) {
        float n = V[j * 4] * V[j * 4] + V[j * 4 + 1] * V[j * 4 + 1] + V[j * 4 + 2] * V[j * 4 + 2];
        if (!(fabsf(n - 1.0f) < 0.01f))
            return 0;
        p[j] = -(V[12] * V[j * 4] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
    }
    return 1;
}

/* the main camera's depth mapping: NDC depth = a + b / d, d the view distance in game units (row vectors,
 * w = -z: z_ndc = (z P[10] + P[14]) / -z) */
static volatile float g_proj_a, g_proj_b;
void shadowcull_depth_proj(float* a, float* b)
{
    *a = g_proj_a, *b = g_proj_b;
}

/* the main camera's position for a frame slot, cached per thread (the slot changes every frame;
 * re-read every 256 calls in case a thread meets the same slot again 3 frames later) */
static const float* scene_camera(Cascade* c, int slot)
{
    if (!S.view_ok || !S.cam_base || slot < 0 || slot > 2)
        return NULL;
    if (c->cam_slot != slot + 1 || (++c->cam_calls & 255) == 0) {
        c->cam_slot = slot + 1;
        const uint8_t* rec = S.cam_base + (size_t)slot * CAM_STRIDE;
        c->cam_ok = cam_pos_from_view((const float*)(rec + CAM_VIEW), c->cam);
        const float* P = (const float*)(rec + CAM_PROJ);
        float p11 = P[5];
        c->cam_p11 = (p11 > 0.05f && p11 < 100.0f) ? p11 : 1.0f;
        if (p11 > 0.05f && p11 < 100.0f && fabsf(P[11] + 1) < 1e-3f && isfinite(P[10]) && isfinite(P[14]))
            g_proj_a = -P[10], g_proj_b = P[14];
        if (g_cfg.occl_census) {
            memcpy(c->cam_v, rec + CAM_VIEW, sizeof(c->cam_v));
            memcpy(c->cam_pm, rec + CAM_PROJ, sizeof(c->cam_pm));
            c->cam_vp_ok = camera_ok(c->cam_v, c->cam_pm);
        }
    }
    return c->cam_ok ? c->cam : NULL;
}

/* A world sphere against the view frustum's side planes and the camera plane (row vectors:
 * clip = [view 1] P; the far plane is left out): 0 inside, 1 crossing an edge, 2 outside. */
static int sphere_view(const Cascade* tc, const float* c, float r)
{
    const float *V = tc->cam_v, *P = tc->cam_pm;
    float v[3];
    for (int j = 0; j < 3; j++)
        v[j] = c[0] * V[j] + c[1] * V[4 + j] + c[2] * V[8 + j] + V[12 + j];
    /* inside where a . v + d <= 0: x <= w, -x <= w, y <= w, -y <= w, w >= 0 */
    static const float sx[5] = {1, -1, 0, 0, 0}, sy[5] = {0, 0, 1, -1, 0};
    float worst = -1e30f;
    for (int p = 0; p < 5; p++) {
        float a[3], d = sx[p] * P[12] + sy[p] * P[13] - P[15];
        for (int i = 0; i < 3; i++)
            a[i] = sx[p] * P[i * 4] + sy[p] * P[i * 4 + 1] - P[i * 4 + 3];
        float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (n > 0) {
            float dist = (a[0] * v[0] + a[1] * v[1] + a[2] * v[2] + d) / n;
            if (dist > worst)
                worst = dist;
        }
    }
    return worst > r ? 2 : worst > -r ? 1 : 0;
}

/* world bounding sphere of a geometry's box (engine order max, min) placed by W */
static void geom_sphere(const float* raw, const float* W, float* c, float* r)
{
    float lc[3], h2 = 0, s2 = 0;
    for (int i = 0; i < 3; i++) {
        float lo = fminf(raw[i], raw[i + 3]), hi = fmaxf(raw[i], raw[i + 3]);
        lc[i] = (lo + hi) * 0.5f, h2 += (hi - lo) * (hi - lo) * 0.25f;
    }
    for (int k = 0; k < 3; k++) {
        c[k] = lc[0] * W[k] + lc[1] * W[4 + k] + lc[2] * W[8 + k] + W[12 + k];
        float rl = W[k * 4] * W[k * 4] + W[k * 4 + 1] * W[k * 4 + 1] + W[k * 4 + 2] * W[k * 4 + 2];
        if (rl > s2)
            s2 = rl;
    }
    *r = sqrtf(h2 * s2);
}

/* distance from the camera to the nearest point of the sphere (0 inside) */
/* the largest scale of a world matrix's axes */
static float world_scale(const float* W)
{
    float s2 = 0;
    for (int k = 0; k < 3; k++) {
        float l = W[k * 4] * W[k * 4] + W[k * 4 + 1] * W[k * 4 + 1] + W[k * 4 + 2] * W[k * 4 + 2];
        if (l > s2)
            s2 = l;
    }
    return sqrtf(s2);
}

static float sphere_dist(const float* c, float r, const float* cam)
{
    float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz) - r;
    return d > 0 ? d : 0;
}

/* Far characters left out: their attachments (bags, weapons: rigid meshes on a bone) must go
 * too. An attachment is recognised as a small rigid mesh that moved since the last frame and
 * sits inside the sphere of a character left out in this or the previous frame. */
#define HC_N 512
typedef struct HidChar {
    float c[3], r;
    volatile LONG64 seq;
} HidChar;
static HidChar g_hc[HC_N];
static volatile LONG g_hc_i;

static void hc_add(const float* c, float r)
{
    HidChar* h = &g_hc[InterlockedIncrement(&g_hc_i) & (HC_N - 1)];
    InterlockedExchange64(&h->seq, 0);
    h->c[0] = c[0], h->c[1] = c[1], h->c[2] = c[2], h->r = r;
    InterlockedExchange64(&h->seq, g_frame_seq + 1);
}

static int hc_near(const float* c, float r)
{
    LONG64 now = g_frame_seq + 1;
    for (int i = 0; i < HC_N; i++) {
        LONG64 sq = g_hc[i].seq;
        if (!sq || sq + 2 < now)
            continue;
        const HidChar* h = &g_hc[i];
        float dx = c[0] - h->c[0], dy = c[1] - h->c[1], dz = c[2] - h->c[2], R = h->r * 1.25f;
        if (dx * dx + dy * dy + dz * dz < R * R && r < h->r * 0.6f)
            return 1;
    }
    return 0;
}

/* Diameter of a sphere in shadow-map texels (1024^2) under a cascade's light matrix (row vectors;
 * a perspective-warped light: the scale at the sphere's centre). 1e9 = behind / unusable. */
static float caster_texels(const float* vp, const float* c, float r)
{
    float w = c[0] * vp[3] + c[1] * vp[7] + c[2] * vp[11] + vp[15];
    if (!(w > 1e-6f))
        return 1e9f;
    float sx = vp[0] * vp[0] + vp[4] * vp[4] + vp[8] * vp[8], sy = vp[1] * vp[1] + vp[5] * vp[5] + vp[9] * vp[9];
    return 2.0f * r * sqrtf(sx > sy ? sx : sy) / w * 512.0f;
}

/* ShadowDistance: the instruction after the far-distance load becomes a jump to
 *   vminss xmm12, xmm12, [cap] ; vmovss xmm1, [original operand] ; jmp back
 * with the cap a float next to the stub (1e30 = the game's distance), written every frame. */
static float* volatile g_shadow_cap;

static size_t build_shadowdist_stub(uint8_t* stub, const uint8_t* site, const float* cap)
{
    const uint8_t* operand = rel_target((uint8_t*)site, 4, 8);
    uint8_t* q = stub;
    int32_t d;
    q[0] = 0xC5, q[1] = 0x1A, q[2] = 0x5D, q[3] = 0x25; /* vminss xmm12, xmm12, [rip + d] */
    d = (int32_t)((const uint8_t*)cap - (q + 8));
    memcpy(q + 4, &d, 4), q += 8;
    q[0] = 0xC5, q[1] = 0xFA, q[2] = 0x10, q[3] = 0x0D; /* vmovss xmm1, [rip + d] */
    d = (int32_t)(operand - (q + 8));
    memcpy(q + 4, &d, 4), q += 8;
    q[0] = 0xE9; /* jmp site + 8 */
    d = (int32_t)((site + 8) - (q + 5));
    memcpy(q + 1, &d, 4), q += 5;
    return (size_t)(q - stub);
}

/* FarShadowRefresh=N: the far cascade (2) is rendered every Nth frame. Its map is a render target of
 * its own, so it keeps its content in between. On the frames in between, the cascade setup's
 * cascade-2 matrix is replaced by the one the map was rendered with (right after the setup, before
 * anything reads it), and cascade_begin reports the cascade as not to be drawn, so it is neither
 * cleared nor drawn. Receivers then sample the old map with the matrix it was made with. */
typedef void (*matwrite_t)(void* dst, void* src);
static matwrite_t o_shadow_done;
static float g_fs_mat[16];
static int g_fs_have, g_fs_last_slot = -1, g_fs_count;
static volatile LONG g_fs_skip[3];
static volatile LONG64 g_fs_skipped, g_fs_drawn;

static void sc_shadow_done(void* dst, void* src)
{
    o_shadow_done(dst, src);
    int slot = S.frame_slot ? *S.frame_slot : -1;
    if (slot < 0 || slot > 2)
        return;
    float* m2 = (float*)(S.vp_table + slot * VP_FRAME_STRIDE + 2 * VP_CASCADE_STRIDE);
    int n = g_cfg.q_shadow_refresh;
    if (slot != g_fs_last_slot) /* a new frame (the setup can run more than once per frame) */
        g_fs_last_slot = slot, g_fs_count++;
    if (n <= 1 || !g_fs_have || g_fs_count % n == 0) {
        memcpy(g_fs_mat, m2, sizeof(g_fs_mat));
        g_fs_have = finite_all(g_fs_mat, 16);
        g_fs_skip[slot] = 0;
    } else {
        memcpy(m2, g_fs_mat, sizeof(g_fs_mat)); /* the matrix of the map as it was last rendered */
        g_fs_skip[slot] = 1;
    }
}

/* world matrix of slot `slot` differs from the previous frame's (xf + 0xC0 + slot * 0x40) */
static int geom_moved(const float* W, int slot)
{
    const uint8_t* xf = (const uint8_t*)W - 0xC0 - slot * 0x40;
    const float* prev = (const float*)(xf + 0xC0 + ((slot + 2) % 3) * 0x40);
    return memcmp(W, prev, 16 * sizeof(float)) != 0;
}

/* [Quality] CharacterLodDistance: the game's own switch to simple character models, moved. The
 * game's value (or MetaphorFix's) is read at the first shadow stage, long after both mods
 * started, and written back when the option is 0 (A/B base). An aligned float store. */
static int g_lod_set = -1;
static float g_lod_orig;

static void lod_apply(void)
{
    int want = g_cfg.q_char_lod;
    if (want == g_lod_set || !S.char_lod)
        return;
    if (g_lod_set < 0)
        g_lod_orig = *S.char_lod;
    *(volatile float*)S.char_lod = want ? want * UNITS_PER_M : g_lod_orig;
    g_lod_set = want;
    LOGI("Quality: simple character models beyond %.0f m from the hero (%s; the game's value %.0f m)",
         *S.char_lod / UNITS_PER_M, want ? "CharacterLodDistance" : "game / MetaphorFix", g_lod_orig / UNITS_PER_M);
}

static int decide(void* cmd, uint8_t* node, int frame, Cascade* cas, float margin, float anim_margin, VMeta* out)
{
    uint8_t* vis = *(uint8_t**)(node + NODE_VISREC);
    if (!vis)
        return D_INELIGIBLE;
    const float* W;
    uint32_t count;
    if (!parse_block((const uint8_t*)cmd, &W, &count)) {
        r_block++;
        return D_INELIGIBLE;
    }
    uint8_t* geom = vis - GEOM_VISREC - frame * 0x10;
    uint8_t* xf = *(uint8_t**)(geom + GEOM_TRANSFORM);
    if (!xf || (const uint8_t*)W != xf + 0xC0 + frame * 0x40) {
        r_mismatch++;
        return D_INELIGIBLE;
    }
    uint32_t flags = *(const uint32_t*)(geom + GEOM_FLAGS);
    if ((flags & GF_SKINNED) && g_cfg.q_active && !cv_has(geom + GEOM_VISREC))
        for (int f = 0; f < 3; f++)
            cv_add(geom + GEOM_VISREC + f * 0x10);
    if (!(flags & GF_HAS_BBOX)) {
        r_nobox++;
        return D_INELIGIBLE;
    }
    /* animated: skinned meshes are bounded through their bone palette; morph-only meshes
     * (vertices rewritten, no palette) are never culled */
    int animated = (flags & (GF_SKINNED | GF_DYNAMIC_VB)) != 0;
    if (animated && !g_cfg.q_char_shadows)
        return D_QSKIP;
    if (!g_cfg.q_far_chars && frame >= 0 && frame < 3 && finite_all(W, 16) &&
        finite_all((const float*)(geom + GEOM_BBOX), 6)) { /* far characters left out: no shadow either */
        const float* cam = scene_camera(cas, frame);
        if (cam) {
            float c[3], r;
            geom_sphere((const float*)(geom + GEOM_BBOX), W, c, &r);
            if (sphere_dist(c, r, cam) > g_cfg.q_far_dist * UNITS_PER_M &&
                ((flags & GF_SKINNED) || (!animated && geom_moved(W, frame) && hc_near(c, r))))
                return D_QSKIP;
        }
    }
    if (animated && (anim_margin < 0 || !(flags & GF_SKINNED) || !S.anim_ok)) {
        r_skinned++;
        return D_INELIGIBLE;
    }
    /* the engine stores max then min (it fixes a flat box by raising [0] above [3]);
     * taking min/max per axis gives the same box whichever order it is */
    const float* raw = (const float*)(geom + GEOM_BBOX);
    if (!finite_all(raw, 6) || !finite_all(W, 16)) {
        r_matrix++;
        return D_INELIGIBLE;
    }
    float bb[6], slope[4];
    int bones = 0;
    if (animated) {
        if (!mesh_bound(cas, geom, bb, &bones)) {
            r_nobound++;
            return D_INELIGIBLE;
        }
    } else {
        for (int i = 0; i < 3; i++)
            bb[i] = fminf(raw[i], raw[i + 3]), bb[i + 3] = fmaxf(raw[i], raw[i + 3]);
        if ((g_cfg.q_tiny_texels > 0 || g_cfg.q_report) && cas->k < MAX_CASCADES) {
            float c[3], r;
            geom_sphere(raw, W, c, &r);
            float t = r > 1.0f ? caster_texels(cas->vp, c, r) : 1e9f; /* degenerate boxes are kept */
            CasStats* st = &g_cs[cas->k];
            if (t < 1.0f)
                st->tiny++, st->idx_tiny += count;
            if (g_cfg.q_tiny_texels > 0 && t < g_cfg.q_tiny_texels) {
                st->tiny_skipped++;
                return D_QSKIP;
            }
        }
    }
    out->geom = geom, out->k = cas->k, out->count = count, out->animated = animated, out->need = 0;
    out->world = W, out->bones = bones;
    memcpy(out->box, bb, sizeof(bb));
    float M[4][3];
    light_columns(W, cas->vp, M);
    plane_maxima(M, bb, 1.0f + margin, out->lmax, slope);
    out->viewcull = 0;
    if (!animated) {
        for (int p = 0; p < 4; p++)
            if (out->lmax[p] < 0)
                return D_CULL;
        out->viewcull = cas->recv.n && outside_receivers(M, bb, &cas->recv);
        return D_KEEP;
    }
    /* animated: the margin (in units of the largest half extent) at which the box would
     * reach into the cascade; skipped if it needs more than anim_margin */
    float emax = fmaxf(fmaxf(bb[3] - bb[0], bb[4] - bb[1]), bb[5] - bb[2]) * 0.5f;
    for (int p = 0; p < 4; p++)
        if (out->lmax[p] < 0) {
            float need = (emax > 0 && slope[p] > 0) ? -out->lmax[p] / (emax * slope[p]) : 1e30f;
            if (need > out->need)
                out->need = need;
        }
    if (out->need > anim_margin)
        return D_CULL;
    if (cas->recv.n) { /* the same margin around the bone bound */
        float g = anim_margin > 0 ? anim_margin * emax : 0.0f, ib[6];
        for (int i = 0; i < 3; i++)
            ib[i] = bb[i] - g, ib[i + 3] = bb[i + 3] + g;
        out->viewcull = outside_receivers(M, ib, &cas->recv);
    }
    return D_KEEP;
}

/* ---------------------------------------------------------------- hooks */
static uint8_t sc_cascade_begin(int frame, int k)
{
    if (k == 2 && frame >= 0 && frame < 3 && o_shadow_done) {
        if (g_fs_skip[frame]) {
            g_fs_skipped++;
            Cascade* c0 = cascade_of_thread(1);
            if (c0)
                c0->k = MAX_CASCADES; /* nothing of this cascade is executed */
            return 0; /* the far cascade keeps last frame's map: not cleared, not drawn */
        }
        g_fs_drawn++;
    }
    Cascade* c = cascade_of_thread(1);
    if (c) {
        if (k == 0) {
            c->seq++, c->view = -1; /* new shadow stage: bone bounds are recomputed, camera matched again */
            InterlockedIncrement64(&g_frame_seq);
            if (g_cfg.q_active) {
                lod_apply();
                if (g_shadow_cap)
                    *(volatile float*)g_shadow_cap = g_cfg.q_shadow_dist ? g_cfg.q_shadow_dist * UNITS_PER_M : 1e30f;
            }
        }
        c->frame = frame, c->k = k, c->usable = 0, c->recv.n = 0;
        if (frame < 0 || frame >= 3 || k < 0 || k >= MAX_CASCADES) {
            c->k = MAX_CASCADES; /* unexpected: execute everything untouched */
        } else {
            const float* vp = (const float*)(S.vp_table + frame * VP_FRAME_STRIDE + k * VP_CASCADE_STRIDE);
            float mag = 0;
            for (int i = 0; i < 16; i++)
                mag += fabsf(vp[i]);
            if (finite_all(vp, 16) && mag > 0) {
                c->usable = 1;
                memcpy(c->vp, vp, sizeof(c->vp));
                if (vp[3] == 0 && vp[7] == 0 && vp[11] == 0)
                    vp_affine++;
                else
                    vp_proj++;
            } else {
                vp_bad++;
            }
            g_cs[k].passes++;
            if (c->usable && g_cfg.shadow_view && S.view_ok)
                build_receivers(c, frame, k);
            /* the first few light matrices in full, for the record */
            static volatile LONG logged;
            LONG n = InterlockedIncrement(&logged);
            if (n <= 3 || n % 9000 < 3) /* first frame, then one frame about every minute */
                LOGI("Shadow cull: light matrix frame slot %d cascade %d: [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | "
                     "%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]",
                     frame, k, vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10], vp[11],
                     vp[12], vp[13], vp[14], vp[15]);
        }
    }
    return o_begin(frame, k);
}

/* For the GPU pass timer: the cascade this thread is rendering, -1 outside the shadow stage. */
int shadowcull_current_cascade(void)
{
    if (g_tls == TLS_OUT_OF_INDEXES)
        return -1;
    Cascade* c = cascade_of_thread(0);
    return (c && c->k < MAX_CASCADES) ? c->k : -1;
}

/* [Quality] Shadows=0 / FarShadows=0: the cascade is still cleared by cascade_begin, so the
 * map stays empty (no shadow there) instead of keeping old content */
static int q_cascade_off(const Cascade* c)
{
    return !g_cfg.q_shadows || (c->k == 2 && !g_cfg.q_far_shadows);
}

static void sc_exec_a(void* cmd, uint8_t* node)
{
    Cascade* c = cascade_of_thread(0);
    if (c && c->k < MAX_CASCADES) {
        g_cs[c->k].nodes_a++;
        if (q_cascade_off(c)) {
            q_skip_nodes[c->k]++;
            return;
        }
    }
    o_interp(cmd);
}

static void sc_exec_b(void* cmd, uint8_t* node)
{
    int on = g_cfg.shadow_cull, verify = g_cfg.shadow_verify;
    Cascade* cas = cascade_of_thread(0);
    if (!cas || cas->k >= MAX_CASCADES) {
        o_interp(cmd);
        return;
    }
    CasStats* st = &g_cs[cas->k];
    if (q_cascade_off(cas)) {
        st->nodes_b++;
        q_skip_nodes[cas->k]++;
        return;
    }
    if ((!on && !verify) || !cas->usable) {
        st->nodes_b++;
        o_interp(cmd);
        return;
    }
    st->nodes_b++;
    VMeta m;
    uint64_t t0 = 0;
    int timed = (++t_decide_all & 15) == 0;
    if (timed)
        t0 = qpc_now();
    /* animated casters: verify mode measures every one that is outside at 0% margin */
    float anim = g_cfg.shadow_skinned_pct < 0 ? -1.0f : g_cfg.shadow_skinned_pct / 100.0f;
    int d = decide(cmd, node, cas->frame, cas, g_cfg.shadow_margin_pct * 0.02f, verify ? 0.0f : anim, &m);
    if (timed) {
        t_decide_ticks += (LONG64)(qpc_now() - t0);
        t_decide_samples++;
    }
    if (d == D_QSKIP) {
        q_skip_nodes[cas->k]++;
        return; /* character shadows switched off */
    }
    if (d != D_INELIGIBLE && m.animated) {
        st->anim_eligible++;
        st->idx_anim_eligible += m.count;
        if (anim >= 0 && m.need > anim)
            st->anim_culled++, st->idx_anim_culled += m.count;
        static volatile LONG anim_logged;
        LONG n = InterlockedIncrement(&anim_logged);
        if (n <= 4 || n % 200000 == 0) {
            const float* rb = (const float*)((uint8_t*)m.geom + GEOM_BBOX);
            const float* bb = m.box;
            const float* W = m.world;
            LOGI("Shadow cull: sample animated caster (cascade %d, %u indices, flags 0x%x, %d bones): rest box %.3g %.3g "
                 "%.3g .. %.3g %.3g %.3g, bone bound %.3g %.3g %.3g .. %.3g %.3g %.3g | world row0 %.3g %.3g %.3g, row3 "
                 "%.4g %.4g %.4g | needs %.0f%% margin to be drawn",
                 cas->k, m.count, *(const uint32_t*)((uint8_t*)m.geom + GEOM_FLAGS), m.bones, rb[0], rb[1], rb[2], rb[3],
                 rb[4], rb[5], bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], W[0], W[1], W[2], W[12], W[13], W[14],
                 m.need > 1e6f ? -1.0 : m.need * 100.0);
        }
    } else if (d != D_INELIGIBLE) {
        st->eligible++;
        st->idx_eligible += m.count;
        static volatile LONG samples_logged;
        if (InterlockedIncrement(&samples_logged) <= 4) {
            const float* bb = (const float*)((uint8_t*)m.geom + GEOM_BBOX);
            LOGI("Shadow cull: sample caster (cascade %d, %u indices): box %.3g %.3g %.3g .. %.3g %.3g %.3g, side-plane "
                 "maxima %.3g %.3g %.3g %.3g -> %s",
                 cas->k, m.count, bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], m.lmax[0], m.lmax[1], m.lmax[2], m.lmax[3],
                 d == D_CULL ? "outside" : "inside");
        }
        if (d == D_CULL)
            st->culled++, st->idx_culled += m.count;
    }
    int skip_view = 0;
    if (d == D_KEEP && m.viewcull) {
        if (m.animated)
            st->view_anim++, st->idx_view_anim += m.count;
        else
            st->view_static++, st->idx_view_static += m.count;
        skip_view = g_cfg.shadow_view == 2 && !verify;
    }
    if (skip_view)
        return; /* its shadow cannot fall on any pixel that reads this cascade */
    if (d != D_CULL) {
        if (g_cfg.q_farmesh && d != D_INELIGIBLE && !m.animated && m.geom && m.world) {
            /* far meshes: the caster's version is picked by its shadow-map texels (or its distance from
             * the camera) */
            float c[3], r;
            geom_sphere((const float*)((uint8_t*)m.geom + GEOM_BBOX), m.world, c, &r);
            const float* cam = scene_camera(cas, cas->frame);
            float tpu = caster_texels(cas->vp, c, 0.5f); /* texels across one world unit */
            gpuprobe_fm_mark(2, cam ? sphere_dist(c, r, cam) : 0, world_scale(m.world), tpu < 1e8f ? tpu : 0);
            o_interp(cmd);
            gpuprobe_fm_mark(0, 0, 0, 0);
            return;
        }
        o_interp(cmd);
        return;
    }
    if (!verify)
        return; /* skipped: not one texel of this cascade's map can come from it */
    int shadow = 0;
    void* ctx = gpuprobe_thread_ctx(&shadow);
    int q = (ctx && shadow) ? gpuprobe_query_begin(ctx) : -1;
    if (q >= 0)
        g_vm[q % VQ] = m;
    else
        v_noquery++;
    o_interp(cmd);
    if (q >= 0)
        gpuprobe_query_end(ctx, q);
}

/* From the GPU probe at frame end: samples < 0 = result never became available. */
void shadowcull_query_result(int slot, int64_t samples)
{
    if (samples < 0) {
        v_unread++;
        return;
    }
    VMeta* m = &g_vm[slot % VQ];
    if (m->animated) {
        /* every animated draw outside at 0% margin was measured: per candidate margin, how
         * many it would skip, and how many of those actually drew pixels */
        va_checked++;
        for (int i = 0; i < AM_N; i++)
            if (m->need > am_margin[i]) {
                va_skip[i]++;
                va_skip_idx[i] += m->count;
                if (samples > 0)
                    va_unsafe[i]++;
            }
        if (samples > 0) {
            va_drew++;
            float need = m->need;
            LONG cur;
            do { /* lock-free max on the float's bits (positive floats order like integers) */
                cur = va_need_max_bits;
                float f;
                memcpy(&f, (const void*)&cur, 4);
                if (need <= f)
                    break;
                LONG nb;
                memcpy(&nb, &need, 4);
                if (InterlockedCompareExchange(&va_need_max_bits, nb, cur) == cur)
                    break;
            } while (1);
        }
        return;
    }
    v_checked++;
    if (samples == 0)
        return;
    v_drew++;
    v_samples += samples;
    AcquireSRWLockExclusive(&g_vlog_lock);
    if (g_vlogged < 8) {
        g_vlogged++;
        LOGI("Shadow cull VERIFY: a skipped caster would have drawn %lld samples: geometry %p cascade %d, %u indices, "
             "side-plane maxima %.4g %.4g %.4g %.4g",
             (long long)samples, m->geom, m->k, m->count, m->lmax[0], m->lmax[1], m->lmax[2], m->lmax[3]);
    }
    ReleaseSRWLockExclusive(&g_vlog_lock);
}

/* For tests\frame_test.c (verify mode): the same query wrapping sc_exec_b applies to a
 * would-be-skipped draw, and the counters it feeds. */
__declspec(dllexport) int MetaphorCPU_TestVerifiedDraw(void (*draw)(void*), void* arg)
{
    int shadow = 0;
    void* ctx = gpuprobe_thread_ctx(&shadow);
    int q = (ctx && shadow) ? gpuprobe_query_begin(ctx) : -1;
    draw(arg);
    if (q >= 0)
        gpuprobe_query_end(ctx, q);
    return q;
}

__declspec(dllexport) void MetaphorCPU_VerifyCounts(LONG64* checked, LONG64* drew, LONG64* unread)
{
    *checked = v_checked, *drew = v_drew, *unread = v_unread;
}

/* ---------------------------------------------------------------- patching */
/* The stubs must be within +-2 GB of the patched calls (rel32). First choice: the int3
 * padding between the EXE's functions (METAPHOR.exe has ~740 runs of 20+ bytes); else a
 * page allocated just below the EXE. */
int g_sc_force_near; /* tests */

static uint8_t* find_cave(size_t need)
{
    uint8_t* text;
    size_t len;
    if (!sig_text_range(game_base(), &text, &len))
        return NULL;
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != 0xCC) {
            run = 0;
            continue;
        }
        if (++run == need + 2) /* keep one int3 on each side */
            return text + i + 1 - run + 1;
    }
    return NULL;
}

static uint8_t* g_near;
static size_t g_near_used;

static uint8_t* near_mem(size_t n)
{
    if (!g_near) {
        uint8_t* base = game_base();
        for (uintptr_t d = 0x10000; d < 0x70000000 && !g_near; d += 0x10000)
            g_near = (uint8_t*)VirtualAlloc(base - d, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_near)
            return NULL;
    }
    if (g_near_used + n > 0x1000)
        return NULL;
    uint8_t* p = g_near + g_near_used;
    g_near_used = (g_near_used + n + 15) & ~(size_t)15;
    return p;
}

/* stub: [mov rdx, <reg>]; jmp qword ptr [rip+0]; dq target */
static uint8_t* make_stub(const uint8_t* prefix, size_t plen, void* target)
{
    uint8_t buf[32];
    size_t n = 0;
    if (plen)
        memcpy(buf, prefix, plen);
    n = plen;
    buf[n++] = 0xFF, buf[n++] = 0x25;
    memset(buf + n, 0, 4), n += 4;
    memcpy(buf + n, &target, 8), n += 8;
    uint8_t* at = g_sc_force_near ? NULL : find_cave(n);
    if (at && patch_bytes(at, buf, n))
        return at;
    at = near_mem(n);
    if (!at)
        return NULL;
    memcpy(at, buf, n);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return at;
}

static int retarget_call(uint8_t* site, void* to)
{
    if (site[0] != 0xE8)
        return 0;
    intptr_t d = (uint8_t*)to - (site + 5);
    if (d < INT32_MIN || d > INT32_MAX)
        return 0;
    int32_t rel = (int32_t)d;
    return patch_bytes(site + 1, &rel, 4);
}

#ifdef MCPU_TESTS
uint8_t* shadowcull_test_make_stub(const uint8_t* prefix, size_t plen, void* target)
{
    return make_stub(prefix, plen, target);
}
int shadowcull_test_retarget(uint8_t* site, void* to) { return retarget_call(site, to); }
#endif

/* ---------------------------------------------------------------- scene passes ([Quality]) */
static interp_t o_scene[4];
/* statistics: plain increments (a lost count now and then is fine; locked ones on these shared
 * lines would cost every job thread on every scene draw) */
static volatile LONG64 sn_nodes[4], sn_char[4], sn_hidden[4];
static volatile LONG64 sn_unresolved, sn_far_nodes, sn_far_chars, sn_far_hidden, sn_attach_hidden, sn_anim_skipped;
static volatile LONG64 sn_m0_checked, sn_m0_bad, sn_tiny, sn_world_hidden;
#define SN_BUCKETS 64
static volatile LONG64 sn_bucket_all[SN_BUCKETS], sn_bucket_char[SN_BUCKETS];

/* The geometry a scene node draws (see SIG_SC_SCENEEXEC) and its world matrix for that slot. The
 * record is only set up for geometries with a box, and the world matrix must be affine. */
static uint8_t* scene_geom(uint8_t* vis, int slot, const float** W)
{
    uint8_t* g = vis - GEOM_VISREC - slot * 0x10;
    if (!(*(const uint32_t*)(g + GEOM_FLAGS) & GF_HAS_BBOX))
        return NULL;
    uint8_t* xf = *(uint8_t**)(g + GEOM_TRANSFORM);
    if (!plausible_ptr(xf))
        return NULL;
    const float* w = (const float*)(xf + 0xC0 + slot * 0x40);
    if (fabsf(w[3]) > 1e-3f || fabsf(w[7]) > 1e-3f || fabsf(w[11]) > 1e-3f || fabsf(w[15] - 1.0f) > 1e-3f ||
        !finite_all(w, 16) || !finite_all((const float*)(g + GEOM_BBOX), 6))
        return NULL;
    *W = w;
    return g;
}

/* once: how a skinned geometry's animation output is buffered (one vertex buffer for all frame
 * slots, or one per slot) - decides whether animating every 2nd frame can look smooth */
static void log_skinned_buffers(uint8_t* geom)
{
    static volatile LONG done;
    if (done || InterlockedExchange(&done, 1))
        return;
    char line[700];
    int n = 0;
    for (int f = 0; f < 3 && n < (int)sizeof(line) - 160; f++) {
        uint8_t* obj = *(uint8_t**)(geom + 0x100 + f * 0x20);
        uint8_t* blk = plausible_ptr(obj) ? *(uint8_t**)(obj + 8) : NULL;
        uint8_t* an = *(uint8_t**)(geom + 0xF0 + f * 0x20);
        void* acmd = plausible_ptr(an) ? *(void**)(an + 8) : NULL;
        if (plausible_ptr(blk) && *(void**)blk == S.h_vb) {
            const uint64_t* q = (const uint64_t*)(blk + 8);
            n += snprintf(line + n, sizeof(line) - n,
                          " | slot %d: animation block %p, shadow block vertex buffers %llx %llx %llx %llx", f, acmd,
                          (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                          (unsigned long long)q[3]);
        } else {
            n += snprintf(line + n, sizeof(line) - n, " | slot %d: animation block %p, no shadow block", f, acmd);
        }
    }
    LOGI("Quality: sample skinned geometry (flags 0x%x)%s", *(const uint32_t*)(geom + GEOM_FLAGS), line);
}

/* occlusion census: the scene frame, counted where the frame slot of the mode 3 nodes changes (their
 * threads record one frame at a time) */
static volatile LONG g_oc_slot = -1;
static volatile LONG64 g_oc_seq;

/* OutlineDistance: the spheres of the characters drawn in the last scene frames (3 buffers by frame: one
 * filled, the one before read, the next one emptied), for the objects they carry */
#define CS_MAX 2048
typedef struct CharSphere {
    float c[3], r;
} CharSphere;
static CharSphere g_csph[3][CS_MAX];
static volatile LONG g_ncsph[3];

/* [Quality] MotionVectorSkinCrowd: the characters within MotionVectorDistance in a frame, by their mesh objects (one per
 * character drawn: it holds that character's bone palette), to tell a crowd (a town) from a fight. Draws of a frame come
 * from several threads: a lock-free set per frame (by sequence, three in turn; the one after next is cleared when a
 * frame starts, in scene_seq). */
#define CROWD_N 1024
static void* volatile g_crowd[3][CROWD_N];
static volatile LONG g_ncrowd[3];
static volatile LONG g_crowd_done = -1; /* the count of the last complete frame */

static LONG64 scene_seq(int slot)
{
    LONG old = g_oc_slot;
    if (old != slot && InterlockedCompareExchange(&g_oc_slot, slot, old) == old) {
        LONG64 s = InterlockedIncrement64(&g_oc_seq);
        g_ncsph[(s + 1) % 3] = 0; /* ready for the next frame */
        if (g_cfg.q_mv_hooks) { /* the frame before this one is complete; the one after next starts afresh */
            if (s >= 1)
                g_crowd_done = g_ncrowd[(s - 1) % 3];
            for (int i = 0; i < CROWD_N; i++)
                g_crowd[(s + 1) % 3][i] = NULL;
            g_ncrowd[(s + 1) % 3] = 0;
        }
        return s;
    }
    return g_oc_seq;
}

static void crowd_add(uint8_t* geom, LONG64 seq)
{
    uint8_t* mesh = *(uint8_t**)(geom + GEOM_MESH);
    void* key = plausible_ptr(mesh) ? (void*)mesh : (void*)geom;
    int k = (int)(seq % 3);
    unsigned h = (unsigned)(((uintptr_t)key >> 4) * 2654435761u) & (CROWD_N - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (CROWD_N - 1)) {
        void* old = InterlockedCompareExchangePointer((void* volatile*)&g_crowd[k][h], key, NULL);
        if (!old) {
            InterlockedIncrement(&g_ncrowd[k]);
            return;
        }
        if (old == key)
            return;
    }
}

/* the characters within MotionVectorDistance in the last complete frame, -1 not known */
int shadowcull_crowd(void)
{
    return g_crowd_done;
}

/* 1 if the point lies inside a character drawn in the scene frame before seq: a bag, a weapon, a hat */
int shadowcull_carried(const float* c, LONG64 seq)
{
    int b = (int)((seq + 2) % 3);
    LONG n = g_ncsph[b];
    if (n > CS_MAX)
        n = CS_MAX;
    for (LONG i = 0; i < n; i++) {
        const CharSphere* k = &g_csph[b][i];
        float dx = c[0] - k->c[0], dy = c[1] - k->c[1], dz = c[2] - k->c[2];
        if (dx * dx + dy * dy + dz * dz < k->r * k->r)
            return 1;
    }
    return 0;
}

static void scene_node(void* cmd, uint8_t* node, int mode, int slot, int bucket)
{
    sn_nodes[mode]++;
    uint8_t* vis = *(uint8_t**)(node + NODE_VISREC);
    if (!vis) {
        o_scene[mode](cmd);
        return;
    }
    const float* W = NULL;
    uint8_t* geom = (S.scene_ok && slot >= 0 && slot < 3) ? scene_geom(vis, slot, &W) : NULL;
    int is_char;
    if (geom) {
        is_char = (*(const uint32_t*)(geom + GEOM_FLAGS) & GF_SKINNED) != 0;
    } else {
        is_char = cv_has(vis); /* fallback: the skinned geometries of the shadow lists */
        sn_unresolved++;
    }
    if (g_cfg.q_report && bucket >= 0) {
        int b = bucket < SN_BUCKETS ? bucket : SN_BUCKETS - 1;
        sn_bucket_all[b]++;
        if (is_char)
            sn_bucket_char[b]++;
    }
    if (is_char) {
        sn_char[mode]++;
        if (g_cfg.q_char_hide >> mode & 1) {
            sn_hidden[mode]++;
            return; /* [Quality]: this character draw is left out */
        }
    }
    if (mode == 0 && geom && is_char) { /* the animation node runs the geometry's own block of this slot */
        uint8_t* an = *(uint8_t**)(geom + 0xF0 + slot * 0x20);
        sn_m0_checked++;
        if (!plausible_ptr(an) || *(void**)(an + 8) != cmd)
            sn_m0_bad++;
        else if (g_cfg.q_report)
            log_skinned_buffers(geom);
    }
    int fm = mode == 3 && (g_cfg.q_farmesh || g_cfg.q_fm_both);
    int oc = mode == 3 && g_cfg.occl_census;
    int ol = mode == 3 && g_cfg.q_outline_hooks; /* OutlineDistance: the draws inside know the object's distance */
    int need = mode == 0 ? (!g_cfg.q_far_chars || g_cfg.q_far_anim > 1 || g_cfg.q_report)
                         : (!g_cfg.q_far_chars || g_cfg.q_far_mip || g_cfg.q_char_tex_q || g_cfg.q_world_tex_q ||
                            g_cfg.compare_shots == 7 || g_cfg.compare_shots == 12 ||
                            g_cfg.q_report || g_cfg.q_tiny_px > 0 ||
                            g_cfg.q_far_world || fm || oc || ol);
    Cascade* tc = (geom && need) ? cascade_of_thread(1) : NULL;
    const float* cam = tc ? scene_camera(tc, slot) : NULL;
    if (!cam) {
        o_scene[mode](cmd);
        return;
    }
    float c[3], r;
    geom_sphere((const float*)(geom + GEOM_BBOX), W, c, &r);
    float d = sphere_dist(c, r, cam), lim = g_cfg.q_far_dist * UNITS_PER_M;
    if (mode == 0) { /* animation (skinning) of one skinned geometry */
        if (is_char && d > lim) {
            sn_far_chars++;
            if (!g_cfg.q_far_chars) {
                hc_add(c, r);
                /* not drawn: no need to pose it; 3 m of margin, so it is posed again before it is drawn */
                if (d > lim + 3.0f * UNITS_PER_M) {
                    sn_anim_skipped++;
                    return;
                }
            } else if (g_cfg.q_far_anim > 1 &&
                       (unsigned long long)(g_frame_seq + ((uintptr_t)geom >> 8)) % (unsigned)g_cfg.q_far_anim) {
                sn_anim_skipped++;
                return; /* keeps the pose of its last animated frame */
            }
        }
        o_scene[0](cmd);
        return;
    }
    /* on-screen diameter of the sphere in pixels at 720p (half height 360); boxes under 1 cm
     * (points, billboards expanded in the vertex shader) count as large */
    float dc = d + r, dpx = (r >= 1.0f && dc > r) ? 2.0f * r / dc * tc->cam_p11 * 360.0f : 1e9f;
    if (g_cfg.q_tiny_px > 0 && dpx < g_cfg.q_tiny_px) {
        sn_tiny++;
        return; /* smaller than the set number of pixels */
    }
    if (g_cfg.q_far_world && !is_char && d > g_cfg.q_far_world * UNITS_PER_M) {
        sn_world_hidden++;
        return; /* MEASUREMENT: what the far world costs */
    }
    int bias = 0;
    if (d > lim) {
        sn_far_nodes++;
        if (!g_cfg.q_far_chars) {
            if (is_char) {
                sn_far_hidden++;
                return;
            }
            if (geom_moved(W, slot) && hc_near(c, r)) {
                sn_attach_hidden++;
                return;
            }
        }
        bias = 4 * g_cfg.q_far_mip; /* smaller texture mips for this object's draws (quarter levels) */
    }
    if (is_char) /* [Quality] CharacterTextureBias: thin painted lines (hair, folds) a little softer */
        bias += g_cfg.q_char_tex_q;
    else /* [Quality] WorldTextureBias: the world's textures a little smaller (less memory traffic) */
        bias += g_cfg.q_world_tex_q;
    if (fm) { /* far meshes: the draws inside know their object (characters: known, never simplified) */
        farmesh_view(tc->cam_p11);
        gpuprobe_fm_mark(is_char ? 3 : 1, d, world_scale(W), 0);
    }
    if (oc) /* occlusion census: one query around the object's draws; its geometry names it (the visibility
             * record is one per frame slot) */
        gpuprobe_oc_node(1, ((uint64_t)(uintptr_t)geom << 8) ^ (uint64_t)(bucket & 0xFF), d / UNITS_PER_M, dpx,
                         is_char, bucket, tc->cam_vp_ok ? sphere_view(tc, c, r) : -1, scene_seq(slot));
    if (ol) { /* characters, and what they carry; 3D texts and speech bubbles use the outline shaders too */
        LONG64 seq = scene_seq(slot);
        if (is_char && g_cfg.q_mv_hooks && (g_cfg.q_mv_dist <= 0 || d <= g_cfg.q_mv_dist * UNITS_PER_M))
            crowd_add(geom, seq);
        if (is_char) {
            LONG i = InterlockedIncrement(&g_ncsph[seq % 3]) - 1;
            if (i < CS_MAX) {
                CharSphere* k = &g_csph[seq % 3][i];
                k->c[0] = c[0], k->c[1] = c[1], k->c[2] = c[2], k->r = r;
            }
        }
        gpuprobe_node_mark(d / UNITS_PER_M, is_char, c, seq);
    }
    if (bias || g_cfg.q_report) { /* the draws inside know their object (texture bias, triangle accounting) */
        int band = d < 1500.0f ? 0 : d < 3000.0f ? 1 : d < 5000.0f ? 2 : d < 10000.0f ? 3 : 4;
        float area = dpx < 1e8f ? 0.785398f * dpx * dpx : 1e12f;
        gpuprobe_scene_node(bias, (is_char ? GEO_BANDS : 0) + band, area, d / UNITS_PER_M);
        o_scene[mode](cmd);
        gpuprobe_scene_node(0, -1, 0, 0);
    } else {
        o_scene[mode](cmd);
    }
    if (oc)
        gpuprobe_oc_node(0, 0, 0, 0, 0, 0, 0, 0);
    if (ol)
        gpuprobe_node_mark(-1, 0, NULL, 0);
    if (fm)
        gpuprobe_fm_mark(0, 0, 0, 0);
}

/* bucket index of a mode 2/3 node, -1 if the bucket is not in the slot's array */
static int scene_bucket(int64_t slot, uint8_t* bucket)
{
    if (!S.scene_ok || slot < 0 || slot > 2)
        return -1;
    uint8_t* base = ((uint8_t* const*)S.scene_tab)[slot];
    intptr_t d = bucket - base;
    return (base && d >= 0 && (d & 0x3F) == 0 && d < 0x10000) ? (int)(d >> 6) : -1;
}

/* The stubs pass the node (rbx) in rdx; modes 2/3 also the frame slot (rsi) in r8 and the
 * bucket (rdi) in r9; mode 0 its bucket (rdi) in r8, whose entry in the per-slot table is the slot. */
static void sc_scene0(void* cmd, uint8_t* node, uint8_t* bucket)
{
    int slot = -1;
    if (S.scene_ok)
        for (int f = 0; f < 3; f++)
            if (((uint8_t* const*)S.scene_tab0)[f] == bucket)
                slot = f;
    scene_node(cmd, node, 0, slot, -1);
}
static void sc_scene2(void* cmd, uint8_t* node, int64_t slot, uint8_t* bucket)
{
    scene_node(cmd, node, 2, (int)slot, scene_bucket(slot, bucket));
}
static void sc_scene3(void* cmd, uint8_t* node, int64_t slot, uint8_t* bucket)
{
    scene_node(cmd, node, 3, (int)slot, scene_bucket(slot, bucket));
}

static void scene_install(void)
{
    static const uint8_t pre0[6] = {0x48, 0x89, 0xDA, 0x49, 0x89, 0xF8}; /* mov rdx, rbx; mov r8, rdi */
    static const uint8_t pre23[9] = {0x48, 0x89, 0xDA, 0x49, 0x89, 0xF0,
                                     0x49, 0x89, 0xF9}; /* mov rdx, rbx; mov r8, rsi; mov r9, rdi */
    void* hooks[4] = {(void*)sc_scene0, NULL, (void*)sc_scene2, (void*)sc_scene3};
    uint8_t* stubs[4] = {0};
    int done = 0;
    for (int i = 0; i < S.nscene; i++) {
        int m = S.scene_mode[i];
        o_scene[m] = (interp_t)S.scene_interp[m];
        if (!stubs[m])
            stubs[m] = m == 0 ? make_stub(pre0, sizeof(pre0), hooks[m]) : make_stub(pre23, sizeof(pre23), hooks[m]);
        if (stubs[m] && retarget_call(S.scene_site[i], stubs[m]))
            done++;
    }
    LOGI("Quality: scene-pass hooks %d of %d installed (interpreter modes 0/2/3); characters = skinned geometries%s",
         done, S.nscene,
         S.scene_ok ? (S.view_ok ? ", distance from the camera known for every scene draw"
                                 : " (no camera records: distance options unavailable)")
                    : " of the shadow lists (scene buckets not found: distance options unavailable)");
}

void shadowcull_install(void)
{
    if (!g_cfg.shadow_cull && !g_cfg.shadow_verify)
        return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (uint64_t)f.QuadPart;
    if (!shadowcull_resolve(game_base(), &S, 1)) {
        LOGI("Shadow cull -> not installed (engine code differs from the analysed version)");
        return;
    }
    if (!shadowcull_selftest(&S)) {
        LOGI("Shadow cull -> self-test FAILED, not installed");
        return;
    }
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) {
        LOGI("Shadow cull -> no thread-local slot, not installed");
        return;
    }
    o_interp = (interp_t)S.interp;
    o_begin = (begin_t)S.begin;
    static const uint8_t mov_rdx_rdi[3] = {0x48, 0x89, 0xFA}, mov_rdx_rbx[3] = {0x48, 0x89, 0xDA};
    uint8_t* st_begin = make_stub(NULL, 0, (void*)sc_cascade_begin);
    uint8_t* st_a = make_stub(mov_rdx_rdi, 3, (void*)sc_exec_a);
    uint8_t* st_b = make_stub(mov_rdx_rbx, 3, (void*)sc_exec_b);
    if (!st_begin || !st_a || !st_b) {
        LOGI("Shadow cull -> no room for the call stubs, not installed");
        return;
    }
    /* order: the per-node hooks first, the cascade hook (which arms them) last */
    int ok = retarget_call(S.exec_a_site, st_a) && retarget_call(S.exec_b_site, st_b) &&
             retarget_call(S.begin_site, st_begin);
    g_installed = ok;
    if (ok && g_cfg.q_active) {
        scene_install();
        if (S.shadow_far_site) {
            uint8_t* stub = near_mem(32);
            if (stub) {
                float* cap = (float*)(stub + 24);
                *cap = 1e30f;
                size_t n = build_shadowdist_stub(stub, S.shadow_far_site, cap);
                FlushInstructionCache(GetCurrentProcess(), stub, n);
                uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x00};
                int32_t rel = (int32_t)(stub - (S.shadow_far_site + 5));
                memcpy(jmp + 1, &rel, 4);
                if (patch_bytes(S.shadow_far_site, jmp, sizeof(jmp)))
                    g_shadow_cap = cap;
            }
            LOGI("Quality: shadow distance cap %s", g_shadow_cap ? "installed (ShadowDistance)" : "NOT installed");
        }
        if (S.shadow_done_site && S.frame_slot) {
            uint8_t* st = make_stub(NULL, 0, (void*)sc_shadow_done);
            o_shadow_done = (matwrite_t)rel_target(S.shadow_done_site, 1, 5);
            if (!st || !retarget_call(S.shadow_done_site, st))
                o_shadow_done = NULL;
            LOGI("Quality: far shadow refresh %s", o_shadow_done ? "installed (FarShadowRefresh)" : "NOT installed");
        }
    }
    LOGI("Shadow cull -> %s, stubs %s (%s, margin %d%% of the cascade width)%s", ok ? "installed" : "FAILED",
         g_near ? "in a page below the EXE" : "in the EXE's int3 padding",
         g_cfg.shadow_verify ? "VERIFY: nothing skipped, would-be-skipped draws measured with occlusion queries"
                             : (g_cfg.shadow_cull ? "skipping casters outside each cascade" : "off"),
         g_cfg.shadow_margin_pct, g_cfg.ab_enabled ? " [switched by A/B]" : "");
}

/* ---------------------------------------------------------------- self-test (synthetic data) */
#define FAIL_AT(f) (LOGI("Shadow cull: self-test check at line %d failed", __LINE__), (f)++)

static int box_is(const float* b, float x0, float y0, float z0, float x1, float y1, float z1)
{
    const float e[6] = {x0, y0, z0, x1, y1, z1};
    for (int i = 0; i < 6; i++)
        if (fabsf(b[i] - e[i]) > 1e-4f)
            return 0;
    return 1;
}

/* Skinned geometry (unit rest box, world = identity + translation) under the orthographic
 * light x/y in [-10, 10], with a mesh object and bone palette laid out like the engine's. */
static int selftest_skinned(void* block, uint8_t* node, uint8_t* geom, float* W, int frame, Cascade* cas)
{
    static _Alignas(16) uint8_t mesh[0x130], glist[0x18], pal[0x18], other[0x1C0];
    static _Alignas(16) float bones[2 * 16];
    static uint8_t* parts[1];
    VMeta m;
    int fails = 0;
    memset(mesh, 0, sizeof(mesh)), memset(glist, 0, sizeof(glist)), memset(pal, 0, sizeof(pal));
    memset(bones, 0, sizeof(bones));
    for (int b = 0; b < 2; b++)
        bones[b * 16 + 0] = bones[b * 16 + 5] = bones[b * 16 + 10] = bones[b * 16 + 15] = 1;
    *(uint8_t**)(geom + GEOM_MESH) = mesh;
    *(uint8_t**)(mesh + MESH_GEOMS) = glist;
    *(int*)(glist + 8) = 1, *(uint8_t***)(glist + 0x10) = parts, parts[0] = geom;
    *(uint8_t**)(mesh + MESH_PALETTE) = pal;
    *(int*)(pal + 8) = 1, *(float**)(pal + 0x10) = bones;
    S.anim_ok = 1;
    memset(cas->vp, 0, sizeof(cas->vp));
    cas->vp[0] = cas->vp[5] = 0.1f, cas->vp[10] = 0.01f, cas->vp[15] = 1;
    cas->seq++;
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_HAS_BBOX | GF_SKINNED;
    /* never culled while the animated margin is off */
    if (decide(block, node, frame, cas, 0.0f, -1.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    /* one identity bone = the rest box: at x = 11.5 (x clip 1.05..1.25) it needs 50% of its
     * half extent added to reach the cascade */
    W[12] = 11.5f;
    if (decide(block, node, frame, cas, 0.0f, 0.25f, &m) != D_CULL || m.need < 0.49f || m.need > 0.51f || m.bones != 1)
        FAIL_AT(fails);
    if (decide(block, node, frame, cas, 0.0f, 1.0f, &m) != D_KEEP)
        FAIL_AT(fails);
    /* a second bone moves vertices +3 in x: bound x -1..4. At x = -11.5 the rest box is outside
     * but posed vertices reach -7.5, so it must be drawn */
    *(int*)(pal + 8) = 2, bones[16 + 12] = 3, cas->seq++;
    W[12] = -11.5f;
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_KEEP)
        FAIL_AT(fails);
    /* at x = 11.5 the bound (10.5..15.5) needs 20% of its half extent 2.5 */
    W[12] = 11.5f;
    if (decide(block, node, frame, cas, 0.0f, 0.1f, &m) != D_CULL || m.need < 0.19f || m.need > 0.21f ||
        !box_is(m.box, -1, -1, -1, 4, 1, 1) || m.bones != 2)
        FAIL_AT(fails);
    if (decide(block, node, frame, cas, 0.0f, 0.25f, &m) != D_KEEP)
        FAIL_AT(fails);
    /* the bound is kept for the rest of the shadow stage and recomputed at the next one */
    bones[16 + 12] = 30;
    decide(block, node, frame, cas, 0.0f, 0.0f, &m);
    if (!box_is(m.box, -1, -1, -1, 4, 1, 1))
        FAIL_AT(fails);
    cas->seq++;
    decide(block, node, frame, cas, 0.0f, 0.0f, &m);
    if (!box_is(m.box, -1, -1, -1, 31, 1, 1))
        FAIL_AT(fails);
    /* rotation: rest box x 0..4 turned 90 degrees about z (x' = -y, y' = x) -> x -1..1, y 0..4;
     * with the identity bone: x -1..4, y -1..4 */
    const float rest[6] = {4, 1, 1, 0, -1, -1}, unit[6] = {1, 1, 1, -1, -1, -1};
    memcpy(geom + GEOM_BBOX, rest, sizeof(rest));
    float* r = bones + 16;
    memset(r, 0, 64);
    r[1] = 1, r[4] = -1, r[10] = 1, r[15] = 1;
    cas->seq++;
    W[12] = 0;
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_KEEP || !box_is(m.box, -1, -1, -1, 4, 4, 1))
        FAIL_AT(fails);
    /* drawn as before: a palette not in the expected layout (4th column not 0 0 0 1), a geometry
     * its mesh does not list, a morphing mesh without palette */
    r[3] = 0.5f, cas->seq++;
    W[12] = 11.5f;
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    r[3] = 0, r[13] = NAN, cas->seq++; /* a NaN in a used entry */
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    r[13] = 0, r[11] = NAN, cas->seq++; /* ...and in the 4th column */
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    r[11] = 0, parts[0] = other, cas->seq++;
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    parts[0] = geom, cas->seq++;
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_HAS_BBOX | GF_DYNAMIC_VB;
    if (decide(block, node, frame, cas, 0.0f, 0.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    /* back to the plain static geometry */
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_HAS_BBOX;
    *(uint8_t**)(geom + GEOM_MESH) = NULL;
    memcpy(geom + GEOM_BBOX, unit, sizeof(unit));
    W[12] = 0;
    return fails;
}

/* View-aware test. Camera at the origin looking down -z with 90 degrees both ways; the light
 * looks straight down: clip x = world x / 10, clip y = world z / 10, w = 1. The camera slice
 * 0..10 then covers the triangle (0 0) (-1 -1) (1 -1) of the cascade. */
/* distance options: camera position, spheres, scene geometry lookup, hidden-character list */
static int selftest_scene(void)
{
    int fails = 0;
    /* camera at (100, 20, -50) turned 90 degrees about y: view = translate(-pos) * rotate */
    float V[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1}, p[3];
    const float pos[3] = {100, 20, -50};
    for (int k = 0; k < 3; k++)
        V[12 + k] = -(pos[0] * V[k] + pos[1] * V[4 + k] + pos[2] * V[8 + k]);
    if (!cam_pos_from_view(V, p) || fabsf(p[0] - 100) > 1e-3f || fabsf(p[1] - 20) > 1e-3f || fabsf(p[2] + 50) > 1e-3f)
        FAIL_AT(fails);
    V[0] = 2; /* scaled rotation: not a camera */
    if (cam_pos_from_view(V, p))
        FAIL_AT(fails);
    /* box 2x4x2 around (0, 2, 0), scaled by 2 and moved to (1000, 0, 0): sphere at (1000, 4, 0), r = 2 * sqrt(6) */
    static _Alignas(16) uint8_t geom[0x1C0], xf[0x180];
    memset(geom, 0, sizeof(geom));
    memset(xf, 0, sizeof(xf));
    float bb[6] = {1, 4, 1, -1, 0, -1};
    memcpy(geom + GEOM_BBOX, bb, sizeof(bb));
    *(uint8_t**)(geom + GEOM_TRANSFORM) = xf;
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_HAS_BBOX | GF_SKINNED;
    for (int f = 0; f < 3; f++) {
        float* w = (float*)(xf + 0xC0 + f * 0x40);
        w[0] = w[5] = w[10] = 2, w[15] = 1, w[12] = 1000;
    }
    float c[3], r, cam[3] = {0, 4, 0};
    geom_sphere(bb, (const float*)(xf + 0xC0), c, &r);
    if (fabsf(c[0] - 1000) > 1e-2f || fabsf(c[1] - 4) > 1e-3f || fabsf(c[2]) > 1e-3f || fabsf(r - 4.89898f) > 1e-3f)
        FAIL_AT(fails);
    if (fabsf(sphere_dist(c, r, cam) - (1000 - 4.89898f)) > 1e-2f || sphere_dist(c, r, c) != 0)
        FAIL_AT(fails);
    { /* carried objects: inside a character drawn the frame before (bag at hip height), not above it (bubble) */
        g_ncsph[2] = 1; /* frame 5 reads buffer (5 + 2) % 3 = 1, frame 6 buffer 2 */
        g_ncsph[1] = 1;
        CharSphere k = {{10, 90, 0}, 100};
        g_csph[1][0] = k;
        float bag[3] = {30, 80, 10}, bubble[3] = {10, 240, 0};
        if (!shadowcull_carried(bag, 5) || shadowcull_carried(bubble, 5) || shadowcull_carried(bag, 7))
            FAIL_AT(fails);
        g_ncsph[0] = g_ncsph[1] = g_ncsph[2] = 0;
    }
    { /* spheres against the view: camera at the origin looking down -z, 90 degrees both ways (x, y <= depth) */
        static Cascade vc;
        memset(&vc, 0, sizeof(vc));
        vc.cam_v[0] = vc.cam_v[5] = vc.cam_v[10] = vc.cam_v[15] = 1;
        vc.cam_pm[0] = vc.cam_pm[5] = 1, vc.cam_pm[10] = -1.0001f, vc.cam_pm[11] = -1, vc.cam_pm[14] = -10.0f;
        static const struct {
            float x, y, z, r;
            int expect;
        } sv[] = {{0, 0, -10, 1, 0},  {0, 0, 10, 1, 2},  {12, 0, -10, 1, 2},     {10, 0, -10, 1, 1},
                  {0, 0, -0.5f, 1, 1}, {0, 9.5f, -10, 0.2f, 0}, {-9.5f, -9.5f, -10, 0.2f, 0}, {0, -11, -10, 0.2f, 2}};
        for (unsigned i = 0; i < sizeof(sv) / sizeof(sv[0]); i++) {
            float sc[3] = {sv[i].x, sv[i].y, sv[i].z};
            if (sphere_view(&vc, sc, sv[i].r) != sv[i].expect)
                FAIL_AT(fails);
        }
    }
    /* node record of slot 2 -> the geometry and that slot's matrix; the other slots' records do not match */
    const float* W = NULL;
    if (scene_geom(geom + GEOM_VISREC + 2 * 0x10, 2, &W) != geom || W != (const float*)(xf + 0xC0 + 2 * 0x40))
        FAIL_AT(fails);
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_SKINNED; /* no box: no record */
    if (scene_geom(geom + GEOM_VISREC + 2 * 0x10, 2, &W))
        FAIL_AT(fails);
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_HAS_BBOX;
    ((float*)(xf + 0xC0 + 0x40))[7] = 0.5f; /* projective: not a world matrix */
    if (scene_geom(geom + GEOM_VISREC + 0x10, 1, &W))
        FAIL_AT(fails);
    /* moved: slot 1 against slot 0 (the previous frame) */
    ((float*)(xf + 0xC0 + 0x40))[7] = 0;
    if (geom_moved((const float*)(xf + 0xC0 + 0x40), 1))
        FAIL_AT(fails);
    ((float*)(xf + 0xC0 + 0x40))[12] = 1001;
    if (!geom_moved((const float*)(xf + 0xC0 + 0x40), 1))
        FAIL_AT(fails);
    /* hidden characters: a bag inside the sphere counts, a big object or one outside does not */
    float ch[3] = {5000, 0, 0}, bag[3] = {5040, 50, 0}, away[3] = {5300, 0, 0};
    hc_add(ch, 120);
    if (!hc_near(bag, 20) || hc_near(bag, 100) || hc_near(away, 20))
        FAIL_AT(fails);
    g_frame_seq += 3; /* three frames later the entry is stale */
    if (hc_near(bag, 20))
        FAIL_AT(fails);
    memset((void*)g_hc, 0, sizeof(g_hc));
    g_frame_seq = 0;
    /* caster size in texels: orthographic light 20 units across (clip = x / 10) */
    float vp[16] = {0.1f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 1};
    float cc[3] = {3, 4, 5};
    if (fabsf(caster_texels(vp, cc, 1.0f) - 102.4f) > 0.01f || fabsf(caster_texels(vp, cc, 0.005f) - 0.512f) > 1e-4f)
        FAIL_AT(fails);
    vp[15] = -1; /* behind a projective light */
    if (caster_texels(vp, cc, 1.0f) < 1e8f)
        FAIL_AT(fails);
    /* the shadow distance stub: displacements land on the cap, the original operand and site + 8 */
    {
        static uint8_t code[64];
        uint8_t* site = code;
        site[0] = 0xC5, site[1] = 0xFA, site[2] = 0x10, site[3] = 0x0D; /* vmovss xmm1, [rip + 0x20] */
        int32_t d0 = 0x20;
        memcpy(site + 4, &d0, 4);
        uint8_t* stub = code + 16;
        float* cap = (float*)(code + 56);
        size_t n = build_shadowdist_stub(stub, site, cap);
        if (n != 21 || stub[0] != 0xC5 || stub[1] != 0x1A || stub[2] != 0x5D || rel_target(stub, 4, 8) != (uint8_t*)cap ||
            rel_target(stub + 8, 4, 8) != site + 8 + 0x20 || stub[16] != 0xE9 || rel_target(stub + 16, 1, 5) != site + 8)
            FAIL_AT(fails);
    }
    return fails;
}

static int selftest_view(void* block, uint8_t* node, uint8_t* geom, float* W, int frame, Cascade* cas)
{
    int fails = 0;
    float V[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float P[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.0001f, -1, 0, 0, -0.2f, 0};
    memset(cas->vp, 0, sizeof(cas->vp));
    cas->vp[0] = 0.1f, cas->vp[9] = 0.1f, cas->vp[6] = -0.01f, cas->vp[14] = 0.5f, cas->vp[15] = 1;
    if (!camera_ok(V, P) || !camera_matches(V, P, 0, 10, cas->vp) || camera_matches(V, P, 0, 20, cas->vp))
        FAIL_AT(fails);
    float Vb[16];
    memcpy(Vb, V, sizeof(V));
    Vb[0] = 2; /* not a rotation */
    if (camera_ok(Vb, P))
        FAIL_AT(fails);
    Recv r;
    if (!receivers_from(V, 1, 1, 0, 10, cas->vp, 0.0f, &r) || r.n != 3)
        FAIL_AT(fails);
    cas->recv = r;
    /* unit box at these world positions: clip x/y centre (x/10, z/10), half size 0.1 */
    struct {
        float x, y, z;
        int viewcull;
    } t[] = {{5, 0, 5, 1} /* behind the camera */, {0, 0, -5, 0} /* in view */, {-8, 0, -3, 1} /* beside the view */,
             {-2.5f, 0, -3, 0} /* touches the edge */, {0, 50, 5, 1} /* high above, behind */};
    VMeta m;
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        W[12] = t[i].x, W[13] = t[i].y, W[14] = t[i].z;
        if (decide(block, node, frame, cas, 0.0f, -1.0f, &m) != D_KEEP || m.viewcull != t[i].viewcull)
            FAIL_AT(fails);
    }
    /* a camera turned 180 degrees about y (looking down +z) sees the other side */
    V[0] = -1, V[10] = -1;
    if (!receivers_from(V, 1, 1, 0, 10, cas->vp, 0.0f, &cas->recv))
        FAIL_AT(fails);
    W[12] = 5, W[13] = 0, W[14] = 5;
    if (decide(block, node, frame, cas, 0.0f, -1.0f, &m) != D_KEEP || m.viewcull)
        FAIL_AT(fails);
    W[12] = W[13] = W[14] = 0;
    cas->recv.n = 0;
    return fails;
}

/* Runs the real parser and decision code on a mesh, draw block and list node built in memory
 * exactly like the engine's (with the resolved command-handler addresses). */
int shadowcull_selftest(const ShadowSites* sites)
{
    S = *sites;
    /* geometry object, draw block and node laid out exactly like the engine's */
    static _Alignas(16) uint8_t geom[0x1C0], xf[0x180], node[0x40];
    static uint64_t block[0x14];
    const int frame = 1;
    memset(geom, 0, sizeof(geom));
    memset(xf, 0, sizeof(xf));
    *(uint8_t**)(geom + GEOM_TRANSFORM) = xf;
    *(uint32_t*)(geom + GEOM_FLAGS) = GF_HAS_BBOX;
    float bb[6] = {1, 1, 1, -1, -1, -1}; /* engine order: max, min */
    memcpy(geom + GEOM_BBOX, bb, sizeof(bb));
    float* W = (float*)(xf + 0xC0 + frame * 0x40);
    memset(W, 0, 64);
    W[0] = W[5] = W[10] = W[15] = 1;
    uint8_t* c = (uint8_t*)block;
    memset(block, 0, sizeof(block));
    *(void**)c = S.h_vb, c += 0x28;
    *(void**)c = S.h_world, *(float**)(c + 8) = W, c += 0x10;
    *(void**)c = S.h_prev, c += 0x10;
    *(void**)c = S.h_mat3, c += 0x10;
    *(void**)c = S.h_ib, c += 0x18;
    *(void**)c = S.h_draw, *(uint32_t*)(c + 0xC) = 36, c += 0x18;
    *(uint8_t**)(node + NODE_VISREC) = geom + GEOM_VISREC + frame * 0x10;

    /* light 1: orthographic, x/y in [-10, 10] -> [-1, 1] (row vectors, unit box at the translation) */
    static Cascade cas; /* holds the bone-bound cache: too big for the stack */
    memset(&cas, 0, sizeof(cas));
    cas.usable = 1, cas.frame = frame;
    cas.vp[0] = cas.vp[5] = 0.1f, cas.vp[10] = 0.01f, cas.vp[15] = 1;
    struct {
        float tx, ty, tz;
        int expect;
    } ortho[] = {{0, 0, 0, D_KEEP}, {10.5f, 0, 0, D_KEEP}, {11.5f, 0, 0, D_CULL}, {0, -12, 0, D_CULL},
                 {-10.9f, 5, 0, D_KEEP}, {0, 0, 500, D_KEEP}};
    VMeta m;
    int fails = 0;
    for (int scale = 0; scale < 2; scale++) { /* the same light scaled by 949 (w = 949) must agree */
        for (unsigned i = 0; i < sizeof(ortho) / sizeof(ortho[0]); i++) {
            W[12] = ortho[i].tx, W[13] = ortho[i].ty, W[14] = ortho[i].tz;
            if (decide(block, node, frame, &cas, 0.0f, -1.0f, &m) != ortho[i].expect)
                FAIL_AT(fails);
        }
        for (int i = 0; i < 16; i++)
            cas.vp[i] *= 949.0f;
    }
    /* light 2: perspective looking down +z, 90 degrees: x_clip = x, y_clip = y, w = z */
    memset(cas.vp, 0, sizeof(cas.vp));
    cas.vp[0] = cas.vp[5] = cas.vp[10] = cas.vp[11] = 1, cas.vp[14] = -0.1f;
    struct {
        float tx, ty, tz;
        int expect;
    } persp[] = {{0, 0, 10, D_KEEP}, {20, 0, 10, D_CULL}, {10.5f, 0, 10, D_KEEP}, {0, -30, 10, D_CULL},
                 {0, 0, -10, D_CULL} /* behind the light */, {0, 0, 0, D_KEEP} /* around the light */};
    for (unsigned i = 0; i < sizeof(persp) / sizeof(persp[0]); i++) {
        W[12] = persp[i].tx, W[13] = persp[i].ty, W[14] = persp[i].tz;
        if (decide(block, node, frame, &cas, 0.0f, -1.0f, &m) != persp[i].expect)
            FAIL_AT(fails);
    }
    W[12] = W[13] = W[14] = 0;
    fails += selftest_skinned(block, node, geom, W, frame, &cas);
    fails += selftest_view(block, node, geom, W, frame, &cas);
    fails += selftest_scene();
    { /* the character set: records found again, others not */
        static uint8_t a[0x40], b[0x40];
        cv_add(a + 0x10);
        if (!cv_has(a + 0x10) || cv_has(b + 0x10) || cv_has(a + 0x20))
            FAIL_AT(fails);
        memset((void*)g_cv, 0, sizeof(g_cv));
        g_cv_count = 0;
    }
    S.anim_ok = sites->anim_ok;
    /* foreign blocks are never culled */
    *(void**)((uint8_t*)block + 0x28) = (void*)&fails; /* unknown command */
    if (decide(block, node, frame, &cas, 0.0f, -1.0f, &m) != D_INELIGIBLE)
        FAIL_AT(fails);
    r_block = r_skinned = r_mismatch = r_nobox = r_matrix = r_nobound = 0; /* keep the self-test out of the report */
    LOGI("Shadow cull: self-test %s (%d failures)", fails ? "FAILED" : "passed", fails);
    return fails == 0;
}

/* ---------------------------------------------------------------- report */
void shadowcull_report(double seconds)
{
    static CasStats p[MAX_CASCADES];
    static LONG64 p_ticks, p_samples, p_all, p_checked, p_drew, p_unread, p_noq;
    if (!g_installed)
        return;
    LONG64 frames = 0;
    for (int k = 0; k < MAX_CASCADES; k++) {
        CasStats now = g_cs[k];
        LONG64 passes = now.passes - p[k].passes;
        if (k == 0)
            frames = passes;
        if (passes <= 0) {
            p[k] = now;
            continue;
        }
        double el = (double)(now.eligible - p[k].eligible), cu = (double)(now.culled - p[k].culled);
        double ie = (double)(now.idx_eligible - p[k].idx_eligible), ic = (double)(now.idx_culled - p[k].idx_culled);
        LOGI("  Shadow cull%s cascade %d: %.0f nodes/pass (list A %.0f, list B %.0f), %.0f checkable; %s %.0f "
             "(%.0f%%), indices %.2fM -> %.2fM (-%.0f%%)",
             g_cfg.shadow_verify ? " [verify]" : (g_cfg.shadow_cull ? "" : " [off]"), k,
             (double)(now.nodes_a - p[k].nodes_a + now.nodes_b - p[k].nodes_b) / passes,
             (double)(now.nodes_a - p[k].nodes_a) / passes, (double)(now.nodes_b - p[k].nodes_b) / passes,
             el / passes, g_cfg.shadow_verify || !g_cfg.shadow_cull ? "would skip" : "skipped", cu / passes,
             el ? cu * 100.0 / el : 0.0, ie / passes / 1e6, (ie - ic) / passes / 1e6, ie ? ic * 100.0 / ie : 0.0);
        double ae = (double)(now.anim_eligible - p[k].anim_eligible), ac = (double)(now.anim_culled - p[k].anim_culled);
        double aie = (double)(now.idx_anim_eligible - p[k].idx_anim_eligible);
        double aic = (double)(now.idx_anim_culled - p[k].idx_anim_culled);
        LONG64 tn = now.tiny - p[k].tiny, ti = now.idx_tiny - p[k].idx_tiny, ts = now.tiny_skipped - p[k].tiny_skipped;
        if (tn > 0 || ts > 0)
            LOGI("  Shadow cull cascade %d tiny static casters (< 1 texel of 1024): %.0f/pass (%.2fM indices); left out "
                 "by TinyCasterTexels %.0f/pass",
                 k, (double)tn / passes, (double)ti / passes / 1e6, (double)ts / passes);
        if (ae > 0 && g_cfg.shadow_skinned_pct >= 0 && !g_cfg.shadow_verify)
            LOGI("  Shadow cull cascade %d animated (margin %d%%): %.0f checkable/pass, skipped %.0f (%.0f%%), indices "
                 "%.2fM -> %.2fM",
                 k, g_cfg.shadow_skinned_pct, ae / passes, ac / passes, ac * 100.0 / ae, aie / passes / 1e6,
                 (aie - aic) / passes / 1e6);
        else if (ae > 0)
            LOGI("  Shadow cull cascade %d animated: %.0f checkable/pass (%.2fM indices)", k, ae / passes,
                 aie / passes / 1e6);
        if (g_cfg.shadow_view && S.view_ok) {
            double vs = (double)(now.view_static - p[k].view_static), vi = (double)(now.idx_view_static - p[k].idx_view_static);
            double va = (double)(now.view_anim - p[k].view_anim), vai = (double)(now.idx_view_anim - p[k].idx_view_anim);
            int skipping = g_cfg.shadow_view == 2 && g_cfg.shadow_cull && !g_cfg.shadow_verify;
            LOGI("  Shadow cull cascade %d view-aware: %s %.0f casters/pass (%.2fM indices) + %.0f animated (%.2fM) whose "
                 "shadow cannot reach the camera's view | camera matched in %lld of %lld passes (no match %lld, polygon "
                 "unusable %lld)",
                 k, skipping ? "skipped" : "would skip", vs / passes, vi / passes / 1e6, va / passes, vai / passes / 1e6,
                 (long long)(now.view_matched - p[k].view_matched), (long long)passes,
                 (long long)(now.view_nomatch - p[k].view_nomatch), (long long)(now.view_unusable - p[k].view_unusable));
        }
        p[k] = now;
    }
    if (g_cfg.q_active && frames > 0) {
        static LONG64 p_n[4], p_c[4], p_h[4];
        char line[300];
        int n = 0;
        for (int m = 0; m < 4; m++) {
            LONG64 nn = sn_nodes[m] - p_n[m], cc = sn_char[m] - p_c[m], hh = sn_hidden[m] - p_h[m];
            p_n[m] = sn_nodes[m], p_c[m] = sn_char[m], p_h[m] = sn_hidden[m];
            if (m == 1)
                continue;
            n += snprintf(line + n, sizeof(line) - n, "%smode %d: %.0f nodes (%.0f characters, %.0f left out)",
                          n ? " | " : "", m, (double)nn / frames, (double)cc / frames, (double)hh / frames);
        }
        LOGI("  Quality: scene passes per frame - %s | character geometries known %ld", line, g_cv_count);
        static LONG64 p_un, p_fn, p_fc, p_fh, p_ah, p_as, p_mc, p_mb;
        LONG64 un = sn_unresolved - p_un, fn = sn_far_nodes - p_fn, fc = sn_far_chars - p_fc, fh = sn_far_hidden - p_fh;
        LONG64 ah = sn_attach_hidden - p_ah, as = sn_anim_skipped - p_as, mc = sn_m0_checked - p_mc, mb = sn_m0_bad - p_mb;
        p_un = sn_unresolved, p_fn = sn_far_nodes, p_fc = sn_far_chars, p_fh = sn_far_hidden;
        p_ah = sn_attach_hidden, p_as = sn_anim_skipped, p_mc = sn_m0_checked, p_mb = sn_m0_bad;
        LOGI("  Quality: beyond %d m - %.0f scene draws/frame, %.0f animated character geometries | left out: %.0f "
             "character draws, %.0f attachment draws, %.0f animations | draws of unknown geometry %.0f/frame, "
             "animation nodes checked %lld (%lld mismatched) | simple character models beyond %.0f m",
             g_cfg.q_far_dist, (double)fn / frames, (double)fc / frames, (double)fh / frames, (double)ah / frames,
             (double)as / frames, (double)un / frames, mc, mb, S.char_lod ? *S.char_lod / UNITS_PER_M : -1.0);
        static LONG64 p_fs, p_fd;
        LONG64 fsk = g_fs_skipped - p_fs, fdr = g_fs_drawn - p_fd;
        p_fs = g_fs_skipped, p_fd = g_fs_drawn;
        if (fsk || g_cfg.q_shadow_refresh > 1)
            LOGI("  Quality: far shadow cascade drawn %.0f%% of frames (FarShadowRefresh %d): %lld drawn, %lld kept",
                 fsk + fdr ? fdr * 100.0 / (double)(fsk + fdr) : 100.0, g_cfg.q_shadow_refresh, fdr, fsk);
        static LONG64 p_ti, p_wh;
        LONG64 ti = sn_tiny - p_ti, wh = sn_world_hidden - p_wh;
        p_ti = sn_tiny, p_wh = sn_world_hidden;
        if (ti || wh || g_cfg.q_tiny_px > 0 || g_cfg.q_far_world)
            LOGI("  Quality: left out per frame: %.0f tiny scene draws (< %.1f px), %.0f world draws beyond %d m "
                 "(measurement) | shadow distance %s",
                 (double)ti / frames, g_cfg.q_tiny_px, (double)wh / frames, g_cfg.q_far_world,
                 g_cfg.q_shadow_dist ? "capped" : "the game's");
        if (g_cfg.q_report) { /* which render buckets (passes) draw the characters */
            static LONG64 p_ba[SN_BUCKETS], p_bc[SN_BUCKETS];
            LONG64 ba[SN_BUCKETS], bc[SN_BUCKETS];
            for (int b = 0; b < SN_BUCKETS; b++)
                ba[b] = sn_bucket_all[b] - p_ba[b], bc[b] = sn_bucket_char[b] - p_bc[b], p_ba[b] = sn_bucket_all[b],
                p_bc[b] = sn_bucket_char[b];
            char bl[700];
            int w = 0;
            for (int r = 0; r < 12 && w < (int)sizeof(bl) - 60; r++) {
                int best = -1;
                for (int b = 0; b < SN_BUCKETS; b++)
                    if (ba[b] > 0 && (best < 0 || ba[b] > ba[best]))
                        best = b;
                if (best < 0)
                    break;
                w += snprintf(bl + w, sizeof(bl) - w, " b%d %.0f (%.0f characters)", best, (double)ba[best] / frames,
                              (double)bc[best] / frames);
                ba[best] = 0;
            }
            if (w)
                LOGI("  Quality: scene draws per frame by render bucket:%s", bl);
        }
    }
    {
        static LONG64 p_q[MAX_CASCADES];
        LONG64 q[MAX_CASCADES], any = 0;
        for (int k = 0; k < MAX_CASCADES; k++)
            q[k] = q_skip_nodes[k] - p_q[k], p_q[k] = q_skip_nodes[k], any |= q[k];
        if (any && frames > 0)
            LOGI("  Shadow cull: [Quality] left out per frame: cascade 0 %.0f, cascade 1 %.0f, cascade 2 %.0f shadow draws",
                 (double)q[0] / frames, (double)q[1] / frames, (double)q[2] / frames);
    }
    LONG64 ticks = t_decide_ticks - p_ticks, samples = t_decide_samples - p_samples, all = t_decide_all - p_all;
    p_ticks = t_decide_ticks, p_samples = t_decide_samples, p_all = t_decide_all;
    if (frames > 0 && g_qpf)
        LOGI("  Shadow cull: decision cost %.3f ms per frame (%.2f us per node); not checkable (total): animated "
             "(off/morph) %lld, skinned without bone bound %lld, no box %lld, other block %lld, mismatch %lld, bad "
             "box/matrix %lld | light matrices (total): affine %lld, projective %lld, unusable %lld",
             samples ? (double)ticks / samples * all / frames * 1000.0 / g_qpf : 0.0,
             samples ? (double)ticks / samples * 1e6 / g_qpf : 0.0, r_skinned, r_nobound, r_nobox, r_block, r_mismatch,
             r_matrix, vp_affine, vp_proj, vp_bad);
    if (g_cfg.shadow_verify) {
        LONG64 ch = v_checked - p_checked, dr = v_drew - p_drew, un = v_unread - p_unread, nq = v_noquery - p_noq;
        p_checked = v_checked, p_drew = v_drew, p_unread = v_unread, p_noq = v_noquery;
        LOGI("  Shadow cull VERIFY: %lld would-be-skipped draws measured, %lld of them drew samples (must be 0) %s; "
             "%lld results unavailable, %lld not measured (no query)",
             ch, dr, dr ? "-> NOT SAFE" : (ch ? "-> OK, image unchanged" : ""), un, nq);
        static LONG64 pa_checked, pa_drew, pa_skip[AM_N], pa_idx[AM_N], pa_unsafe[AM_N];
        LONG64 ach = va_checked - pa_checked, adr = va_drew - pa_drew;
        pa_checked = va_checked, pa_drew = va_drew;
        if (ach > 0 && frames > 0) {
            char line[700];
            int n = 0;
            for (int i = 0; i < AM_N && n < (int)sizeof(line) - 90; i++) {
                LONG64 sk = va_skip[i] - pa_skip[i], ix = va_skip_idx[i] - pa_idx[i], un2 = va_unsafe[i] - pa_unsafe[i];
                pa_skip[i] = va_skip[i], pa_idx[i] = va_skip_idx[i], pa_unsafe[i] = va_unsafe[i];
                n += snprintf(line + n, sizeof(line) - n, "%s%.0f%%: skip %.0f/frame (%.2fM idx), %lld drew",
                              i ? " | " : "", am_margin[i] * 100.0, (double)sk / frames, ix / 1e6 / frames, un2);
            }
            float worst;
            LONG bits = va_need_max_bits;
            memcpy(&worst, &bits, 4);
            LOGI("  Shadow cull VERIFY animated: %lld measured, %lld drew pixels (they needed up to %.0f%% margin "
                 "so far) | per margin: %s",
                 ach, adr, worst > 1e6f ? -1.0 : worst * 100.0, line);
        } else {
            for (int i = 0; i < AM_N; i++)
                pa_skip[i] = va_skip[i], pa_idx[i] = va_skip_idx[i], pa_unsafe[i] = va_unsafe[i];
        }
    }
    (void)seconds;
}
