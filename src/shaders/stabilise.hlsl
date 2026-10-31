// [Quality] TemporalStabilise, TemporalStabiliseWorld (README §75, §76, §78): the image blended with the previous frame,
// just before the game's SMAA (it is finished there: lit, tone-mapped, graded, no HUD yet). Thin features (the ink line
// meshes, laces, hair strands; railings, ropes, roof edges, far windows) cover a pixel's centre in one frame and miss it
// in the next as a character sways or the camera moves: they flicker. Each pixel keeps part of its previous value (one
// share for the characters, another for the rest). That value is looked for in two places, and the one that fits best
// is used:
//   - where the camera's motion says the same surface point was (the world, and a character standing while the camera
//     moves): the depth and the game's view of this frame and the last (GFD_PSCONST_SYSTEM, copied at the painterly
//     pass); exact for whatever does not move;
//   - the same pixel (a character that moves with the camera: the hero it follows).
// Where the characters' motion vectors are known (CharacterMotionVectors, README §79), the first place is where the
// pixel's own motion puts it instead: walking, turning, the limbs' swing. The fit is the distance stored with the
// history against the one expected, then the colour. The value is then clamped
// to the range of the current 3x3 neighbourhood (so a feature that moved away leaves no trail), counts less the faster
// the camera moves, and is dropped where neither distance fits (another surface). The history is read with a Catmull-Rom
// filter: a bilinear one would soften it a little more every frame. No jitter and no motion vectors of the characters'
// own movements: it steadies what flickers, it adds no detail while the camera stands still (no jitter), and walking
// characters stay a little soft.
// Character pixels: flag 4 in gbuffer1's alpha, as the game's painterly pass uses it.
cbuffer StabiliseConst : register(b0)
{
    float weight;    // the share of the history a still character pixel keeps (0..0.95)
    float valid;     // 0: no history of this image (first frame, a new size, after a pause)
    float depth_tol; // a relative distance change from which the history counts less (none from twice this)
    float motion_k;  // the history's share is divided by 1 + motion_k * (the camera's motion here, pixels)
    int2 size;
    float weight_world;   // the same for every other pixel (0 = left as drawn)
    float motion_k_world; // their reprojection is exact: less reason to trust the history less in motion
    float mv_on;          // the motion target holds this frame's motion
    float mv_view;        // [GpuProbe] MotionVectorView: the motion shown instead of the image
    float world_cross;    // TemporalStabiliseWorldTaps=4: the world's pixels clamped to their 4 direct neighbours
};
// GFD_PSCONST_SYSTEM of this frame and of the previous one: [1..4] mtxView, [5..8] mtxInvView, [17] invProjParams
// (view z = w / (depth + z), view xy = -view z * ndc * xy, as the game's painterly shader reconstructs it)
cbuffer SysNow : register(b1)
{
    float4 now[18];
};
cbuffer SysPrev : register(b2)
{
    float4 prev[18];
};
SamplerState linear_clamp : register(s0);
Texture2D<float4> current : register(t0); // this frame's image (a copy)
Texture2D<float4> history : register(t1); // the result of the previous frame; alpha: its distance (view units)
Texture2D<float4> gbuffer1 : register(t2);
Texture2D<float> depth : register(t3);
Texture2D<float4> motion : register(t4); // back to where the pixel was (pixels), its distance then; alpha 1: known

struct Out
{
    float4 colour : SV_Target0;
    float4 hist : SV_Target1;
};

// YCoCg of the square root (about the perceived brightness): the clamp and the blend act as the eye sees them
float3 to_ycocg(float3 c)
{
    c = sqrt(max(c, 0));
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 from_ycocg(float3 y)
{
    float t = y.x - y.z;
    float3 c = max(float3(t + y.y, y.x + y.z, t - y.y), 0);
    return c * c;
}

// 5-tap Catmull-Rom (the corner taps left out): exact at texel centres, sharp in between
float3 history_cr(float2 uv)
{
    float2 pos = uv * size, c = floor(pos - 0.5) + 0.5, f = pos - c;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, inv = 1.0 / float2(size);
    float2 t0 = (c - 1) * inv, t12 = (c + w2 / w12) * inv, t3 = (c + 2) * inv;
    float a = w12.x * w0.y, b = w0.x * w12.y, m = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
    float3 r = history.SampleLevel(linear_clamp, float2(t12.x, t0.y), 0).rgb * a +
               history.SampleLevel(linear_clamp, float2(t0.x, t12.y), 0).rgb * b +
               history.SampleLevel(linear_clamp, t12, 0).rgb * m +
               history.SampleLevel(linear_clamp, float2(t3.x, t12.y), 0).rgb * d +
               history.SampleLevel(linear_clamp, float2(t12.x, t3.y), 0).rgb * e;
    return max(r / (a + b + m + d + e), 0);
}

Out main(float4 pos : SV_Position)
{
    int2 p = int2(pos.xy);
    float2 uv = pos.xy / float2(size);
    float4 c = current.Load(int3(p, 0));
    float vz = now[17].w / (depth.Load(int3(p, 0)) + now[17].z); // view z (negative in front)
    float dist = min(abs(vz), 60000.0);
    Out o;
    o.colour = c;
    o.hist = float4(c.rgb, dist);
    float4 mv = mv_on > 0 ? motion.Load(int3(p, 0)) : 0;
    bool has_mv = mv.a > 0.5;
    if (mv_view > 0) { // the view's encoding (motion_ps.hlsl): r, g = 0.5 + motion / 16, b = 1 where it matched
        // known: green, plus yellow where it came from the left (moving right on screen), cyan from the right, darker
        // the more it moves up or down (8 px: full); magenta: drawn again but rejected; dark: nothing drawn
        float2 d = (mv.xy - 0.5) * 16;
        o.colour.rgb = mv.b > 0.99 ? float3(saturate(-d.x / 8), 1, saturate(d.x / 8)) * (1 - 0.6 * saturate(abs(d.y) / 8))
                       : mv.a > 0 ? float3(1, 0, 1) : c.rgb * 0.25;
        has_mv = false; // the view's values are not motion: not used to stabilise
    }
    bool chr = ((uint)(gbuffer1.Load(int3(p, 0)).w * 255.0 + 0.5) & 4) != 0;
    float share = chr ? weight : weight_world;
    if (share <= 0 || valid == 0)
        return o;
    float3 y = to_ycocg(c.rgb), lo = y, hi = y;
    [branch] if (chr || world_cross <= 0) { // the 3x3 neighbourhood
        [unroll] for (int j = -1; j <= 1; j++) {
            [unroll] for (int i = -1; i <= 1; i++) {
                if (i != 0 || j != 0) {
                    float3 n = to_ycocg(current.Load(int3(clamp(p + int2(i, j), 0, size - 1), 0)).rgb);
                    lo = min(lo, n), hi = max(hi, n);
                }
            }
        }
    } else { // the world: its 4 direct neighbours (half the reads; its reprojection is exact)
        static const int2 cross_[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
        [unroll] for (int k = 0; k < 4; k++) {
            float3 n = to_ycocg(current.Load(int3(clamp(p + cross_[k], 0, size - 1), 0)).rgb);
            lo = min(lo, n), hi = max(hi, n);
        }
    }
    // where this surface point was: its own motion where known, else had it stood still (through the world position
    // into the previous view)
    float2 ndc = uv * 2 - 1;
    float4 v = float4(-vz * ndc * now[17].xy, vz, 1);
    float4 w = float4(dot(v, now[5]), dot(v, now[6]), dot(v, now[7]), 1);
    float3 pv = float3(dot(w, prev[1]), dot(w, prev[2]), dot(w, prev[3]));
    float2 puv = (pv.xy / (-pv.z * prev[17].xy)) * 0.5 + 0.5;
    float pdist = abs(pv.z);
    bool on_screen = pv.z < 0;
    if (has_mv)
        puv = (pos.xy + mv.xy) / float2(size), pdist = mv.z, on_screen = true;
    on_screen = on_screen && all(puv > 0) && all(puv < 1);
    if (!on_screen && (!chr || has_mv)) // coming into view at the screen's edge: nothing of it in the history
        return o;
    // the two candidates: their distance against the expected one (relative), then the colour
    float4 hs = history.Load(int3(p, 0));
    float fit_s = abs(hs.a - dist) / max(dist, 1e-4);
    float3 hm = hs.rgb;
    float fit_m = 1e9, moved = 0;
    if (on_screen) {
        float hma = history.Load(int3(min(int2(puv * size), size - 1), 0)).a;
        fit_m = abs(hma - pdist) / max(pdist, 1e-4);
        hm = history_cr(puv);
        moved = length((puv - uv) * size);
    }
    float sc_s = fit_s / depth_tol + abs(to_ycocg(hs.rgb).x - y.x);
    float sc_m = fit_m / depth_tol + abs(to_ycocg(hm).x - y.x);
    bool use_m = sc_m < sc_s;
    float fit = use_m ? fit_m : fit_s;
    float mk = chr && !has_mv ? motion_k : motion_k_world; // with the pixel's own motion the history is where it was
    float k = share * saturate(2 - fit / depth_tol) / (1 + mk * (use_m ? moved : 0));
    if (k <= 0)
        return o;
    float3 r = from_ycocg(lerp(y, clamp(to_ycocg(use_m ? hm : hs.rgb), lo, hi), k));
    if (mv_view <= 0)
        o.colour.rgb = r;
    o.hist.rgb = r;
    return o;
}
