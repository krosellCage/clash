// The still pipeline's shader: the same inputs and constant buffers as
// cube.hlsl, without the wobble and the pulse - every vertex goes where
// World puts it.

// The constant buffers: World (b0), and ViewProj, PulseColor, gTime (b1).
#include "common.hlsl"

// The parts of model_vertex this shader reads.
struct vertex_in
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct vertex_out
{
    float4 Position : SV_POSITION;
    float3 NormalW : NORMAL;     // in world space
};

vertex_out VS(vertex_in In)
{
    vertex_out Out;

    float4 WorldPosition = mul(float4(In.Position, 1.0f), World);
    Out.Position = mul(WorldPosition, ViewProj);

    // World's 3x3 part only: a direction has no position to move.
    Out.NormalW = mul(In.Normal, (float3x3)World);

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    // This object's material: its entry in the material buffer.
    material_data Material = gMaterialData[MaterialIndex];

    // Checking the materials reach the shader, before there is light to
    // use them: each object flat in its material's colour.
    return Material.DiffuseAlbedo;
}
