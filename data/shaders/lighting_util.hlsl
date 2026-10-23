// Luna's LightingUtil.hlsl (8.13): the lighting equation (8.4) for the three
// kinds of light, and the sum over all of a frame's lights.  Included by
// the lit shaders, after common.hlsl (it uses the pass's light counts).

#if !defined(LIGHTING_UTIL_HLSL)
#define LIGHTING_UTIL_HLSL

// ------------------------------------------------------------------------
// 8.13.2 Common helpers.
// ------------------------------------------------------------------------

// Point and spot lights fade in a straight line: full strength up to
// FalloffStart, nothing from FalloffEnd on (8.11.1).
float CalcAttenuation(float D, float FalloffStart, float FalloffEnd)
{
    // saturate clamps to 0..1: 1 before the start, 0 after the end.
    return saturate((FalloffEnd - D) / (FalloffEnd - FalloffStart));
}

// The Schlick approximation of Fresnel (8.7.1): how much light a surface
// reflects, R0 head-on, rising to all of it at a grazing angle.
float3 SchlickFresnel(float3 R0, float3 Normal, float3 LightVec)
{
    float CosIncidentAngle = saturate(dot(Normal, LightVec));

    float F0 = 1.0f - CosIncidentAngle;
    float3 ReflectPercent = R0 + (1.0f - R0)*(F0*F0*F0*F0*F0);     // (1 - cos)^5

    return ReflectPercent;
}

// The parts of a material the lighting equation uses - Luna's Material in
// LightingUtil.hlsl.  Shininess is 1 - roughness.
struct light_material
{
    float4 DiffuseAlbedo;
    float3 FresnelR0;
    float Shininess;
};

// The light reflected into the eye (8.8): the diffuse part plus the
// specular part, both scaled by the light that arrives, LightStrength -
// already scaled down by Lambert's law by the caller.
float3 BlinnPhong(float3 LightStrength, float3 LightVec, float3 Normal, float3 ToEye,
                  light_material Material)
{
    // The roughness exponent m (8.7.2), from the shininess: 0..256.
    const float M = Material.Shininess*256.0f;

    // The halfway vector: the facets that reflect the light into the eye
    // face this way.
    float3 HalfVec = normalize(ToEye + LightVec);

    // How many facets face that way, with the energy-keeping (m + 8)/8 -
    // and how much they reflect at this angle (Fresnel).
    float RoughnessFactor = (M + 8.0f)*pow(max(dot(HalfVec, Normal), 0.0f), M) / 8.0f;
    float3 FresnelFactor = SchlickFresnel(Material.FresnelR0, HalfVec, LightVec);

    float3 SpecAlbedo = FresnelFactor*RoughnessFactor;

    // The specular formula can go above 1, and the back buffer can only
    // hold 0..1 (no HDR yet) - squeeze it down so highlights fade instead
    // of clipping hard.
    SpecAlbedo = SpecAlbedo / (SpecAlbedo + 1.0f);

    return (Material.DiffuseAlbedo.rgb + SpecAlbedo)*LightStrength;
}

// ------------------------------------------------------------------------
// 8.13.3 - 8.13.5 One light of each kind.  Normal and ToEye have length 1.
// ------------------------------------------------------------------------

// A directional light (8.10): the same direction everywhere.
float3 ComputeDirectionalLight(light L, light_material Material, float3 Normal, float3 ToEye)
{
    // The light vector aims the opposite way the light travels.
    float3 LightVec = -L.Direction;

    // Lambert's cosine law (8.4).
    float NdotL = max(dot(LightVec, Normal), 0.0f);
    float3 LightStrength = L.Strength*NdotL;

    return BlinnPhong(LightStrength, LightVec, Normal, ToEye, Material);
}

// A point light (8.11): a different direction at every point, fading with
// distance.
float3 ComputePointLight(light L, light_material Material, float3 Pos, float3 Normal, float3 ToEye)
{
    // From the surface towards the light: Q - P (8.11).
    float3 LightVec = L.Position - Pos;

    float D = length(LightVec);

    // Out of range: no light at all - and no need to work the rest out.
    if(D > L.FalloffEnd)
    {
        return 0.0f;
    }

    LightVec /= D;      // length 1

    float NdotL = max(dot(LightVec, Normal), 0.0f);
    float3 LightStrength = L.Strength*NdotL;

    LightStrength *= CalcAttenuation(D, L.FalloffStart, L.FalloffEnd);

    return BlinnPhong(LightStrength, LightVec, Normal, ToEye, Material);
}

// A spotlight (8.12): a point light that only shines inside a cone.
float3 ComputeSpotLight(light L, light_material Material, float3 Pos, float3 Normal, float3 ToEye)
{
    float3 LightVec = L.Position - Pos;

    float D = length(LightVec);

    if(D > L.FalloffEnd)
    {
        return 0.0f;
    }

    LightVec /= D;

    float NdotL = max(dot(LightVec, Normal), 0.0f);
    float3 LightStrength = L.Strength*NdotL;

    LightStrength *= CalcAttenuation(D, L.FalloffStart, L.FalloffEnd);

    // Brightest along the cone's middle, fading towards its edge -
    // SpotPower makes the cone tighter (8.12).
    float SpotFactor = pow(max(dot(-LightVec, L.Direction), 0.0f), L.SpotPower);
    LightStrength *= SpotFactor;

    return BlinnPhong(LightStrength, LightVec, Normal, ToEye, Material);
}

// ------------------------------------------------------------------------
// 8.13.6 Every light of the frame, added up.  Directional lights come
// first in Lights, then point lights, then spotlights.  ShadowFactor dims
// the directional lights - all 1 until the shadow chapter.
// ------------------------------------------------------------------------
float4 ComputeLighting(light Lights[MAX_LIGHTS], light_material Material,
                       float3 Pos, float3 Normal, float3 ToEye, float3 ShadowFactor)
{
    float3 Result = 0.0f;

    uint Index = 0;

    for(Index = 0; Index < NumDirLights; ++Index)
    {
        Result += ShadowFactor[Index]*ComputeDirectionalLight(Lights[Index], Material, Normal, ToEye);
    }

    for(Index = NumDirLights; Index < NumDirLights + NumPointLights; ++Index)
    {
        Result += ComputePointLight(Lights[Index], Material, Pos, Normal, ToEye);
    }

    for(Index = NumDirLights + NumPointLights;
        Index < NumDirLights + NumPointLights + NumSpotLights;
        ++Index)
    {
        Result += ComputeSpotLight(Lights[Index], Material, Pos, Normal, ToEye);
    }

    return float4(Result, 0.0f);
}

#endif
