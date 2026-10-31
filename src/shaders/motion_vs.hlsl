// [Quality] CharacterMotionVectors (README §79): a mesh of the characters' pass drawn again, where it is now and where
// it was in the previous frame. The game's transform constants (GFD_VSCONST_TRANSFORM, its VS b1) of this draw and of
// the same object's draw in the previous frame are copied on the GPU; registers 4..7 hold mtxLocalToWorldViewProj by
// columns, as the game's own vertex shaders read them (clip.x = dot(position, c4), ...). The previous position comes from
// a copy of the vertex buffer made after its previous frame's use (the skinned meshes; the others: the same buffer).
cbuffer Now : register(b0)
{
    float4 now[8];
};
cbuffer Prev : register(b1)
{
    float4 prev[8];
};

void main(float3 pos : POSITION0, float3 ppos : TEXCOORD7, out float4 o : SV_Position, out float4 pc : TEXCOORD0)
{
    float4 p = float4(pos, 1), q = float4(ppos, 1);
    o = float4(dot(p, now[4]), dot(p, now[5]), dot(p, now[6]), dot(p, now[7]));
    pc = float4(dot(q, prev[4]), dot(q, prev[5]), dot(q, prev[6]), dot(q, prev[7]));
}
