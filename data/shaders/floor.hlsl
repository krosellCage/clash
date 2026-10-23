// The still pipeline's shader - Luna's BasicLit.hlsl (8.13.7): every vertex
// where World puts it, every pixel lit by the frame's lights in its
// object's material.

// The constant buffers (b0, b1) and the material buffer (t0).
#include "common.hlsl"
// The lighting equation and ComputeLighting.
#include "lighting_util.hlsl"

// The parts of model_vertex this shader reads.
struct vertex_in
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct vertex_out
{
    float4 PositionH : SV_POSITION;     // on screen (homogeneous clip space)
    float3 PositionW : POSITION2;       // in the world - lighting needs where the point is
    float3 NormalW : NORMAL;            // in the world
};

vertex_out VS(vertex_in In)
{
    vertex_out Out;

    // Into the world.
    float4 PositionW = mul(float4(In.Position, 1.0f), World);
    Out.PositionW = PositionW.xyz;

    // World's 3x3 part only: a direction has no position to move.  Fine
    // without the inverse-transpose (8.2.2) while no World scales unevenly.
    Out.NormalW = mul(In.Normal, (float3x3)World);

    // Onto the screen.
    Out.PositionH = mul(PositionW, ViewProj);

    return Out;
}

float4 PS(vertex_out In) : SV_TARGET
{
    // This object's material: its entry in the material buffer.
    material_data MaterialData = gMaterialData[MaterialIndex];

    // Blending across the triangle shortens the normal - back to length 1.
    float3 Normal = normalize(In.NormalW);

    // From this point towards the eye (8.3).
    float3 ToEye = normalize(EyePosW - In.PositionW);

    // The bounced light every surface gets, in its colour (8.6).
    float4 Ambient = AmbientLight*MaterialData.DiffuseAlbedo;

    // Every light of the frame (8.13.6).  No shadows yet: factor 1.
    light_material Material = {MaterialData.DiffuseAlbedo, MaterialData.FresnelR0,
                               1.0f - MaterialData.Roughness};
    float3 ShadowFactor = 1.0f;
    float4 DirectLight = ComputeLighting(Lights, Material, In.PositionW, Normal, ToEye, ShadowFactor);

    float4 LitColor = Ambient + DirectLight;

    // The usual convention: alpha from the diffuse albedo.
    LitColor.a = MaterialData.DiffuseAlbedo.a;

    return LitColor;
}
