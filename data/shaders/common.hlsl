// What every shader of the scene shares (Luna's Common.hlsl).  Included by
// cube.hlsl and floor.hlsl; found through the -I data\shaders switch in
// CompileShader.

#if !defined(COMMON_HLSL)
#define COMMON_HLSL

// The constant buffers - b0: World; b1: ViewProj, PulseColor, gTime - from
// the file the C code reads too.  HLSL_CODE first, so it turns them into
// cbuffers rather than C structs.
#define HLSL_CODE 1
#include "shared_types.h"

// t0: every material (8.9.2) - an array in GPU memory, one material_data
// per material, bound once per frame as root parameter 2.  A shader picks
// its object's entry with b0's MaterialIndex.
StructuredBuffer<material_data> gMaterialData : register(t0);

#endif
