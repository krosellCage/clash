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

// The parts of model_vertex this shader reads.  The input layout has four;
// a shader may take only some of them, found by their names.
struct vertex_in
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct vertex_out
{
    float4 Position : SV_POSITION;
    float3 NormalW : NORMAL;     // in world space, so it turns with the object
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

    // The normal into world space: only World's 3x3 part, since a direction
    // has no position to move.  Fine without the inverse-transpose (8.2.2)
    // as long as no World scales unevenly.
    Out.NormalW = mul(In.Normal, (float3x3)World);

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    // Blending across the triangle shortens the normal - back to length 1.
    float3 Normal = normalize(In.NormalW);

    // Checking the normals before there is light to use them: each axis
    // from -1..1 to a colour 0..1.  Facing +x is red, +y green, +z blue.
    float4 NormalColor = float4(0.5f*Normal + 0.5f, 1.0f);

    const float pi = 3.14159;
    // Oscillate a value in [0,1] over time using a sine function.
    float s = 0.5f*sin(2*gTime - 0.25f*pi)+ 0.5f;

    float4 c = lerp(NormalColor, PulseColor, s);
    return c;
}
