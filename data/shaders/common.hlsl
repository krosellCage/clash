// What every shader of the scene shares (Luna's Common.hlsl): the constant
// buffers the root signature binds.  Included by cube.hlsl and floor.hlsl;
// found through the -I data\shaders switch in CompileShader.

#if !defined(COMMON_HLSL)
#define COMMON_HLSL

// b0: one object's - written by DrawShape into its slot.
cbuffer ObjectConstants : register(b0)
{
    float4x4 World;
};

// b1: the whole frame's - written once per frame into slot 0.
cbuffer PassConstants : register(b1)
{
    float4x4 ViewProj;
    float4   PulseColor;
    float gTime;
};

#endif
