// The still pipeline's shader: the same inputs and constant buffers as
// cube.hlsl, without the wobble and the pulse - every vertex goes where
// World puts it.

cbuffer ObjectConstants : register(b0)
{
    float4x4 World;
};

cbuffer PassConstants : register(b1)
{
    float4x4 ViewProj;
    float4   PulseColor;
    float gTime;
};

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
    // Back to length 1 after blending across the triangle.
    float3 Normal = normalize(In.NormalW);

    // Checking the normals before there is light: each axis from -1..1 to a
    // colour 0..1.  Facing +x is red, +y green, +z blue.
    return float4(0.5f*Normal + 0.5f, 1.0f);
}
