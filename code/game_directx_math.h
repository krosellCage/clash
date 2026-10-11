#if !defined(GAME_DIRECTX_MATH_H)
/*
  NOTE(yigit): DirectXMath, next to our own game_math.h.

  DirectXMath is the maths library Frank Luna's book uses.  It ships with the
  Windows SDK as headers only - nothing to link, nothing to add to build.bat.
  Its fast types are SIMD registers:

    XMVECTOR   four floats in one SSE register  - for local variables only
    XMMATRIX   four XMVECTORs                   - for local variables only
    XMFLOAT3, XMFLOAT4, XMFLOAT4X4              - plain structs, for storage

  IMPORTANT(yigit): Keep XMVECTOR and XMMATRIX out of anything stored -
  game_state, arenas, constant-buffer structs.  They need 16-byte alignment
  and crash without it.  Store XMFLOATn / XMFLOAT4X4 (or vec3 / mat4) and
  load them into registers to do maths.

  ---------------------------------------------------------------------------
  game_math.h, DirectXMath and SimpleMath all share one convention - row
  vectors, row-major - so they mix freely:

  1. RIGHT-HANDED.  Our camera looks down -Z.  Use the ...RH functions
     (XMMatrixLookAtRH, XMMatrixPerspectiveFovRH), never Luna's ...LH ones.
     (SimpleMath's Create... functions are already RH.)

  2. THE SAME ORDER.  Vectors go on the left (v * M), and transforms read
     left to right in the order they happen:

        XMMATRIX WVP = World * View * Projection;   // DirectXMath
        mat4     WVP = World * View * Projection;   // game_math
        Matrix   WVP = World * View * Projection;   // SimpleMath

  3. THE SAME 16 FLOATS.  A mat4, an XMFLOAT4X4 and a SimpleMath Matrix for
     the same transform have identical bytes, so moving between them is a
     plain copy.  The helpers below rely on this.

  4. SHADERS.  As in Luna: transpose on the way into a constant buffer
     (XMMatrixTranspose), and multiply vector-first in HLSL:
     mul(Position, WorldViewProj).
*/

#include <DirectXMath.h>

/*
  NOTE(yigit): SimpleMath - DirectXTK12's friendlier face for DirectXMath,
  which Luna's 2nd edition uses from chapter 24 on (and his MathHelper
  everywhere).  Copied unchanged from misc\d3d12book_2ed\External\DirectXTK12
  into external\DirectXTK12 (MIT licence, LICENSE.txt beside it).

  Its types ARE the storage types with operators added - Vector3 is an
  XMFLOAT3, Matrix is an XMFLOAT4X4 - so unlike XMVECTOR/XMMATRIX they are
  safe in structs and arenas, and you can write A + B * 2.0f:

    Vector3 P = Vector3(1, 2, 3) + Vector3::Up * 2.0f;
    Matrix  W = Matrix::CreateScale(2.0f) * Matrix::CreateTranslation(P);

  Same rules as DirectXMath underneath: row vectors, so S * R * T reads
  left to right.  And it is RIGHT-HANDED - Vector3::Forward is (0, 0, -1),
  CreatePerspectiveFieldOfView and CreateLookAt are the RH versions - which
  is our convention, not Luna's LH.

  IMPORTANT(yigit): It includes dxgi1_2.h (for its Viewport type), and with
  it windows.h - so these two must be defined before it, or windows.h's
  min/max macros break std::min/std::max everywhere after.
*/
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "external/DirectXTK12/SimpleMath.h"

#include "game_math.h"

// Lets Luna's code be typed as it is in the book - XMVECTOR rather than
// DirectX::XMVECTOR.  Every name in the namespace starts with XM, so nothing
// of ours can collide with it.
using namespace DirectX;

// NOTE(yigit): The same short names Luna's MathHelper.h uses.  Named one by
// one rather than "using namespace DirectX::SimpleMath" - that would also
// pull in SimpleMath::Rectangle, which collides with the Win32 GDI function
// of the same name.
using Vector2    = DirectX::SimpleMath::Vector2;
using Vector3    = DirectX::SimpleMath::Vector3;
using Vector4    = DirectX::SimpleMath::Vector4;
using Matrix     = DirectX::SimpleMath::Matrix;
using Quaternion = DirectX::SimpleMath::Quaternion;
using Plane      = DirectX::SimpleMath::Plane;
using Color      = DirectX::SimpleMath::Color;

// ---------------------------------------------------------------------------
// vec3 / vec4  <->  XMVECTOR
// ---------------------------------------------------------------------------

// NOTE(yigit): W is the one thing to think about.  0 for a direction, which
// translation must not move; 1 for a position, which it must.
inline XMVECTOR XM_CALLCONV
XMLoadVec3(vec3 V, real32 W = 0.0f)
{
    return(XMVectorSet(V.X, V.Y, V.Z, W));
}

inline XMVECTOR XM_CALLCONV
XMLoadVec4(vec4 V)
{
    return(XMVectorSet(V.X, V.Y, V.Z, V.W));
}

inline vec3 XM_CALLCONV
XMStoreVec3(FXMVECTOR V)
{
    XMFLOAT3 F;
    XMStoreFloat3(&F, V);
    return(Vec3(F.x, F.y, F.z));
}

inline vec4 XM_CALLCONV
XMStoreVec4(FXMVECTOR V)
{
    XMFLOAT4 F;
    XMStoreFloat4(&F, V);
    return(Vec4(F.x, F.y, F.z, F.w));
}

// ---------------------------------------------------------------------------
// mat4  <->  XMMATRIX
// ---------------------------------------------------------------------------

// Same convention, same bytes (rule 3 above), so these only copy.
static_assert(sizeof(mat4) == sizeof(XMFLOAT4X4), "mat4 and XMFLOAT4X4 must be the same 16 floats");

inline XMMATRIX XM_CALLCONV
XMLoadMat4(const mat4 *M)
{
    return(XMLoadFloat4x4((const XMFLOAT4X4 *)M->E));
}

inline mat4 XM_CALLCONV
XMStoreMat4(FXMMATRIX M)
{
    mat4 Result;
    XMStoreFloat4x4((XMFLOAT4X4 *)Result.E, M);
    return(Result);
}

#define GAME_DIRECTX_MATH_H
#endif
