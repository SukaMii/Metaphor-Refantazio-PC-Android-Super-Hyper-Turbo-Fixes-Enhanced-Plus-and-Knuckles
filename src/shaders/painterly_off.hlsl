// [Quality] Painterly=0: replaces the game's painterly wobble shader (pass p022, pixel shader
// b3ebc8024b526721; README §55). The game's version offsets the background by a wobble texture projected
// along the surface normals, and writes an edge strength by distance into alpha, which the next pass
// (p023, 1be84722b6b574f3) uses to darken edges; that pass also writes the background into the scene image.
// This version copies the background unchanged with an edge strength of 0, so p023 passes it through
// without darkening edges. Pixels the game masks out (flag 4 in gbuffer1's alpha: the characters, drawn
// before) keep -1, which p023 discards, as with the game's shader. Same registers and signature as the
// original (cb0 resolution, t3 gbuffer1, t5 the lit background, s1 point sampler).
cbuffer GFD_PSCONST_SYSTEM : register(b0)
{
    float2 resolution;
};
SamplerState pointWrapSampler : register(s1);
Texture2D<float4> gbuffer1Texture : register(t3);
Texture2D<float4> maskBgTexture : register(t5);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float3 c = maskBgTexture.Sample(pointWrapSampler, uv).xyz;
    uint flags = (uint)(gbuffer1Texture.Load(int3(int2(uv * resolution), 0)).w * 255.0);
    return float4(c, (flags & 4) ? -1.0 : 0.0);
}
