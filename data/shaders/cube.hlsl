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

float EaseInOut(float x)
{
    float Result = 0.5 - 0.5 * cos(x);
    return Result;
}

vertex_out VS(vertex_in In)
{
    vertex_out Out;

    // The wobble.
    In.Position.xy += 0.5f*sin(In.Position.x)*sin(3.0f*gTime);
    In.Position.z *= 0.6f + 0.4f*sin(2.0f*gTime);

    float4 WorldPosition = mul(float4(In.Position, 1.0f), World);
    Out.Position = mul(WorldPosition, ViewProj);
    Out.Color = In.Color; 

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    const float pi = 3.14159;
    // Oscillate a value in [0,1] over time using a sine function.
    float s = 0.5f*sin(2*gTime - 0.25f*pi)+ 0.5f;

    float4 c = lerp(In.Color, PulseColor, s);
    return c;
}
