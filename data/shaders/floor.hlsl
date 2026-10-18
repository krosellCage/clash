// The floor: the cube's inputs and constant buffers, without the wobble and
// the pulse - every vertex goes where World puts it, in its own colour.

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

struct vertex_in
{
    float3 Position : POSITION;
    float4 Color : COLOR;
};

struct vertex_out
{
    float4 Position : SV_POSITION;
    float4 Color : COLOR;
};

vertex_out VS(vertex_in In)
{
    vertex_out Out;

    float4 WorldPosition = mul(float4(In.Position, 1.0f), World);
    Out.Position = mul(WorldPosition, ViewProj);
    Out.Color = In.Color;

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    return In.Color;
}
