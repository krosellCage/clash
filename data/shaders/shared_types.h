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
    uint MaterialIndex;     // which entry of the material buffer it's drawn with (8.9.2)

    // World fills 4 rows of 16 bytes exactly; MaterialIndex starts a fifth
    // and uses 4 of its bytes.  The padding fills that row (Luna's
    // PerObjectCB_pad0..2), so C and HLSL are plainly the same size and a
    // field added later starts a clean row.
    uint ObjectPad0;
    uint ObjectPad1;
    uint ObjectPad2;
};

// The most lights one frame can have (8.13.6).  A #define works on both
// sides, so C and HLSL size the Lights array alike.
#define MAX_LIGHTS 16

// One light of any kind (Luna's Light, 8.13.1).  Each kind uses only some
// of the fields.
//
// IMPORTANT(yigit): The order packs into exactly three 16-byte rows - each
// float3 is followed by a float that fills its row:
//   row 1: Strength.xyz  FalloffStart
//   row 2: Direction.xyz FalloffEnd
//   row 3: Position.xyz  SpotPower
// Three float3s in a row would make HLSL pad after each one (two float3s
// don't fit in one row) and C wouldn't, and the two would disagree.
struct light
{
    float3 Strength;        // colour and brightness - every kind
    float FalloffStart;     // point and spot: full strength up to this distance (8.11.1)
    float3 Direction;       // directional and spot: the way the light travels
    float FalloffEnd;       // point and spot: no light from this distance on
    float3 Position;        // point and spot: where the light is
    float SpotPower;        // spot: how tight the cone is (8.12)
};

// b1: the whole frame's (Luna's PerPassCB, 7.4.5 and 8.13.6) - written once
// per frame at the start of the constant buffer.  Matrices transposed, like
// World.
DEFINE_CBUFFER(pass_constants, b1)
{
    float4x4 View;
    float4x4 InvView;
    float4x4 Proj;
    float4x4 InvProj;
    float4x4 ViewProj;
    float4x4 InvViewProj;

    float3 EyePosW;             // the camera's position in the world - for the to-eye vector
    float PassPad0;             // fills EyePosW's row

    float2 RenderTargetSize;    // the back buffer's width and height in pixels
    float2 InvRenderTargetSize; // 1 / those

    float NearZ;                // the near and far planes' distances
    float FarZ;
    float TotalTime;            // seconds since startup
    float DeltaTime;            // seconds this frame took

    float4 AmbientLight;        // the indirect light every surface gets (8.6)

    float4 PulseColor;          // clash's own - the chapter 6 pulse, in cube.hlsl

    // How many of Lights are of each kind.  Directional lights come first,
    // then point lights, then spot lights (8.13.6).
    uint NumDirLights;
    uint NumPointLights;
    uint NumSpotLights;
    uint PassPad1;              // fills the counts' row

    light Lights[MAX_LIGHTS];
};

// One material, as the shaders see it (Luna's MaterialData, 8.9.2) - one
// entry in the material buffer.  A plain struct, not a cbuffer: it's an
// element of a StructuredBuffer, which the shader indexes.
struct material_data
{
    float4 DiffuseAlbedo;           // the fraction of red, green, blue it reflects (8.5)
    float3 FresnelR0;               // how much it reflects head-on (8.7.1)
    float Roughness;                // 0 smooth .. 1 rough (8.7.2)
    float DisplacementScale;        // displacement mapping - a later chapter
    uint DiffuseMapIndex;           // which textures - chapter 9 on; 0xFFFFFFFF = none
    uint NormalMapIndex;
    uint GlossHeightAoMapIndex;
    float4x4 MatTransform;          // moves its texture coordinates - chapter 9
    float TransparencyWeight;       // the ray-tracing chapters
    float IndexOfRefraction;        // the ray-tracing chapters
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
