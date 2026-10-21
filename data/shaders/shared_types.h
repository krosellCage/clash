/*
  Luna's SharedTypes.h (8.9.1): structs both sides use, written once.

  Included by the C code (dx12_renderer.cpp) and by the shaders (through
  common.hlsl).  The same text means two different things:

    in C     DEFINE_CBUFFER(Name, Reg)  ->  struct Name
             float4x4 -> XMFLOAT4X4, float4 -> XMFLOAT4, uint -> u32, ...
    in HLSL  DEFINE_CBUFFER(Name, Reg)  ->  cbuffer Name : register(Reg)
             (float4x4 and the rest are HLSL's own types already)

  so the bytes the C side memcpys and the bytes the shader reads are laid
  out by one definition, and can't drift apart.

  IMPORTANT(yigit): HLSL packs constant buffers in 16-byte rows, and a
  float2, float3 or float4 may not cross into the next row.  C knows nothing
  of that rule - order the fields so HLSL needs no padding (a float3 then a
  float in one row, as Luna does), or pad by hand with a float.
*/

#if !defined(SHARED_TYPES_H)
#define SHARED_TYPES_H

#if defined(HLSL_CODE)

#define DEFINE_CBUFFER(Name, Reg) cbuffer Name : register(Reg)

#else

// The C side: HLSL's type names as their DirectXMath twins (game_directx_math.h
// is included before this).
#define DEFINE_CBUFFER(Name, Reg) struct Name

#define uint u32
#define uint2 XMUINT2
#define uint3 XMUINT3
#define uint4 XMUINT4

#define int2 XMINT2
#define int3 XMINT3
#define int4 XMINT4

#define float2 XMFLOAT2
#define float3 XMFLOAT3
#define float4 XMFLOAT4
#define float4x4 XMFLOAT4X4

#endif

// b0: one object's - written by DrawShape into its slot.  World is stored
// transposed, because HLSL reads constant-buffer matrices column by column.
DEFINE_CBUFFER(object_constants, b0)
{
    float4x4 World;
};

// b1: the whole frame's - written once per frame into slot 0.
DEFINE_CBUFFER(pass_constants, b1)
{
    float4x4 ViewProj;
    float4 PulseColor;
    float gTime;
};

// NOTE(yigit): The C side takes its type names back.  clash is one unity
// build - Dear ImGui and SimpleMath are compiled after this file - and a
// macro called float4 or uint would rewrite their code too.
#if !defined(HLSL_CODE)
#undef DEFINE_CBUFFER
#undef uint
#undef uint2
#undef uint3
#undef uint4
#undef int2
#undef int3
#undef int4
#undef float2
#undef float3
#undef float4
#undef float4x4
#endif

#endif
