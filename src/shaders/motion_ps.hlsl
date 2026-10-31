// [Quality] CharacterMotionVectors (README §79, §80): where the mesh drawn again is the surface the game drew (its
// distance matches the one in the game's depth buffer), the pixel's motion back to the previous frame (pixels), its
// distance then (view units: the previous clip w) and 1 in alpha (known). Elsewhere nothing is written: alpha stays 0.
// The match is relative (the game may offset its characters' depth a little: an exact depth test found none on PC,
// 28 Oct 2026); a character behind another is farther by much more. With [GpuProbe] MotionVectorView the fragments
// that do not match are kept too, with alpha 0.25, to tell "drawn in the wrong place" from "drawn but rejected".
cbuffer MotionConst : register(b0)
{
    float4 viewport; // x, y, width, height of the characters' pass
    float tol;       // the largest relative distance difference that still counts as the same surface
    float depth_a;   // the camera's depth mapping: NDC = a + b / distance (b = 0: not known, NDC compared)
    float depth_b;
    float view;      // MotionVectorView
};
Texture2D<float> depth : register(t0);

float4 main(float4 pos : SV_Position, float4 pc : TEXCOORD0) : SV_Target
{
    float zg = depth.Load(int3(pos.xy, 0));
    float rel = depth_b != 0 ? abs(1 - (pos.z - depth_a) / (zg - depth_a)) // |d_game - d_ours| / d_game
                             : abs(zg - pos.z) * 5e4;                      // 2e-5 NDC at tol 1
    if (!(rel <= tol) || !(pc.w > 0)) {
        if (view <= 0)
            discard;
        return float4(0, 0, 0, 0.25);
    }
    float2 prev_px = viewport.xy + float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5) * viewport.zw;
    if (view > 0) // blended with MAX: only values that are never negative, the matched ones above the rejected ones
        return float4(saturate(0.5 + (prev_px - pos.xy) / 16), 1, 1);
    return float4(prev_px - pos.xy, pc.w, 1);
}
