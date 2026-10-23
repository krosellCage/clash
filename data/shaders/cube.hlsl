// The animated pipeline's shader: floor.hlsl's lighting (Luna's BasicLit),
// with the chapter 6 exercises on top - the vertices wobble, and the lit
// colour pulses towards PulseColor.

// The constant buffers (b0, b1) and the material buffer (t0).
#include "common.hlsl"
// The lighting equation and ComputeLighting.
#include "lighting_util.hlsl"

// The parts of model_vertex this shader reads.  The input layout has four;
// a shader may take only some of them, found by their names.
struct vertex_in
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct vertex_out
{
    float4 PositionH : SV_POSITION;     // on screen
    float3 PositionW : POSITION2;       // in the world
    float3 NormalW : NORMAL;            // in the world, so it turns with the object
};

vertex_out VS(vertex_in In)
{
    vertex_out Out;

    // The wobble.
    In.Position.xy += 0.5f*sin(In.Position.x)*sin(3.0f*TotalTime);
    In.Position.z *= 0.6f + 0.4f*sin(2.0f*TotalTime);

    float4 PositionW = mul(float4(In.Position, 1.0f), World);
    Out.PositionW = PositionW.xyz;

    // The normal into world space: only World's 3x3 part, since a direction
    // has no position to move.  Fine without the inverse-transpose (8.2.2)
    // as long as no World scales unevenly.  (The wobble bends the box but
    // not its normals - close enough for an exercise.)
    Out.NormalW = mul(In.Normal, (float3x3)World);

    Out.PositionH = mul(PositionW, ViewProj);

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    material_data MaterialData = gMaterialData[MaterialIndex];

    float3 Normal = normalize(In.NormalW);
    float3 ToEye = normalize(EyePosW - In.PositionW);

    float4 Ambient = AmbientLight*MaterialData.DiffuseAlbedo;

    light_material Material = {MaterialData.DiffuseAlbedo, MaterialData.FresnelR0,
                               1.0f - MaterialData.Roughness};
    float3 ShadowFactor = 1.0f;
    float4 DirectLight = ComputeLighting(Lights, Material, In.PositionW, Normal, ToEye, ShadowFactor);

    float4 LitColor = Ambient + DirectLight;
    LitColor.a = MaterialData.DiffuseAlbedo.a;

    // The pulse: a value going 0..1 and back over time, mixing the lit
    // colour towards PulseColor.
    const float Pi = 3.14159f;
    float S = 0.5f*sin(2.0f*TotalTime - 0.25f*Pi) + 0.5f;

    return lerp(LitColor, PulseColor, S);
}
