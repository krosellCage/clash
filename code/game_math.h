#ifndef GAME_MATH_H
#define GAME_MATH_H
/*
  NOTE(yigit): Vectors and matrices.  LEFT-handed - X right, Y up, Z forward
  into the screen, so the camera looks down +Z - with ROW vectors and
  matrices stored ROW-major.  Exactly the convention of Frank Luna's book and
  DirectXMath's ...LH functions, so his maths can be followed as written.

    - Left-handed: point the left hand's fingers along +X, curl them toward
      +Y, and the thumb is +Z.  Cross(X, Y) = Z, as always - it is only the
      picture that is mirrored compared with a right-handed system.

    - A vector is a row, and goes on the LEFT:  v * M.
    - Transforms read left to right, in the order they happen:
          World = Scale * Rotation * Translation
      "scale first, then rotate, then move".
    - E[Row*4 + Col].  Row i of a transform is where axis i lands, and the
      bottom row (E[12..14]) holds the translation.

  Projection is DirectX's: depth comes out 0 at Near and 1 at Far.

  NOTE(yigit): Shaders.  HLSL reads a float4x4 in a constant buffer as
  column-major unless told otherwise, so - exactly as Luna does - transpose a
  matrix on its way into a constant buffer, and multiply vector-first in the
  shader: mul(Position, WorldViewProj).  Forget the transpose and the shader
  sees the matrix mirrored across its diagonal: a cube that is squashed,
  inside out, or not there at all.
*/

#include "base.h"
#include <math.h>

// NOTE(yigit): Thin wrappers so the rest of the code never touches the CRT
// names directly.  These were the only two ever used from the old intrinsics
// header, so they live here now.
// TODO(yigit): Implement these ourselves and drop math.h entirely.
inline r32 Sin(r32 Angle) { return(sinf(Angle)); }
inline r32 Cos(r32 Angle) { return(cosf(Angle)); }

struct mat4
{
    r32 E[16]; // row-major: E[Row*4 + Col]
};

inline mat4
Mat4Identity()
{
    mat4 M = {};
    M.E[0]  = 1; M.E[5]  = 1;
    M.E[10] = 1; M.E[15] = 1;
    return(M);
}

// A * B: A happens first, then B.  Entry (Row, Col) is row Row of A dotted
// with column Col of B.
inline mat4
Mat4Mul(mat4 A, mat4 B)
{
    mat4 M = {};
    for(int Row = 0; Row < 4; ++Row)
    {
        for(int Col = 0; Col < 4; ++Col)
        {
            r32 Sum = 0;
            for(int K = 0; K < 4; ++K)
            {
                Sum += A.E[Row*4 + K] * B.E[K*4 + Col];
            }
            M.E[Row*4 + Col] = Sum;
        }
    }
    return(M);
}

// So World = Scale * Rotation * Translation can be written as it reads.
inline mat4 operator*(mat4 A, mat4 B) { return(Mat4Mul(A, B)); }

/*
  Perspective projection, for DirectX: depth comes out 0 at Near and 1 at Far
  (Luna 5.6.3.5).  F is 1/tan(FovY/2).

    [ F/Aspect  0   0                    0 ]
    [ 0         F   0                    0 ]
    [ 0         0   Far/(Far-Near)       1 ]
    [ 0         0  -Near*Far/(Far-Near)  0 ]

  The 1 at row 2, column 3 copies the view-space Z - the distance in front of
  the camera - into W, and the GPU's divide by W is what makes distant things
  smaller.  The two depth entries then map Z = Near to 0 and Z = Far to 1.

  NOTE(yigit): OpenGL's version maps Near..Far to -1..1.  Used unchanged, it
  would put everything in the front half of the view outside DirectX's 0..1
  range, and it would silently vanish.  Same as XMMatrixPerspectiveFovLH.
*/
inline mat4
Mat4Perspective(r32 FovYRadians, r32 Aspect, r32 Near, r32 Far)
{
    mat4 M = {};
    r32 F = 1.0f / tanf(FovYRadians * 0.5f);
    M.E[0]  =  F / Aspect;
    M.E[5]  =  F;
    M.E[10] = Far / (Far - Near);           M.E[11] = 1.0f;
    M.E[14] = -(Near * Far) / (Far - Near);
    return(M);
}

/*
  Orthographic projection - no perspective divide, so distance does not shrink
  anything.  What 2D overlays are drawn with: HUD, debug text, anything meant
  to sit flat on the screen.

  NOTE(yigit): The difference from Mat4Perspective is the entry that is NOT
  set here.  Perspective puts 1 in E[11] (row 2, column 3), which lands the
  view-space Z in the clip-space W, and the divide by W is what makes distant
  things smaller.  Ortho leaves E[11] at 0 and E[15] at 1, so W is always 1
  and no divide happens at all.

  This is a plain remap: the box (Left..Right, Bottom..Top, Near..Far) becomes
  x and y from -1 to 1 and depth from 0 to 1, DirectX's clip space.  The
  diagonal - E[0], E[5], E[10] - is the scale that sizes the box, and the
  bottom row - E[12..14] - is the shift that centres it.

  For screen-space text call it as

      Mat4Ortho(0, Width, Height, 0, -1, 1)

  which puts (0,0) at the TOP left, with Y growing downward - how Windows,
  and DirectX's own viewport, count pixels.
*/
inline mat4
Mat4Ortho(r32 Left, r32 Right, r32 Bottom, r32 Top,
          r32 Near, r32 Far)
{
    mat4 M = {};

    M.E[0]  =  2.0f / (Right - Left);
    M.E[5]  =  2.0f / (Top - Bottom);

    // Positive: the camera looks down +Z, and Near and Far are distances in
    // front of it.  1 over the depth range rather than OpenGL's 2: DirectX's
    // depth only spans 0..1.  Same as XMMatrixOrthographicOffCenterLH.
    M.E[10] = 1.0f / (Far - Near);

    M.E[12] = -(Right + Left) / (Right - Left);
    M.E[13] = -(Top + Bottom) / (Top - Bottom);
    M.E[14] = -Near / (Far - Near);
    M.E[15] =  1.0f;

    return(M);
}

// Translation matrix - the move goes in the bottom row:
//   [ 1 0 0 0 ]
//   [ 0 1 0 0 ]
//   [ 0 0 1 0 ]
//   [ X Y Z 1 ]
// A point has W = 1, so v * M adds X, Y, Z to it.  A direction has W = 0,
// and is not moved.
inline mat4
Mat4Translation(r32 X, r32 Y, r32 Z)
{
    mat4 M = Mat4Identity();
    M.E[12] = X;
    M.E[13] = Y;
    M.E[14] = Z;
    return(M);
}

// Scale matrix - the diagonal, so each axis is multiplied independently
inline mat4
Mat4Scale(r32 X, r32 Y, r32 Z)
{
    mat4 M = Mat4Identity();
    M.E[0]  = X;
    M.E[5]  = Y;
    M.E[10] = Z;
    return(M);
}

// Rotations about one axis.  A positive angle turns CLOCKWISE when you look
// from the positive end of the axis toward the origin - left-handed.  (The
// numbers are the same in a right-handed system; only that picture is
// mirrored.)  So Mat4RotationY with a positive angle turns +Z toward +X:
// forward toward right, a turn to the right seen from above.  Each row is
// where that axis ends up - which is why the axis being rotated around keeps
// its own row untouched.  Same as XMMatrixRotationX/Y/Z.
inline mat4
Mat4RotationX(r32 Angle)
{
    mat4 M = Mat4Identity();
    r32 c = cosf(Angle);
    r32 s = sinf(Angle);

    // [ 1  0   0   0 ]
    // [ 0  c   s   0 ]
    // [ 0 -s   c   0 ]
    // [ 0  0   0   1 ]
    M.E[5] =  c;  M.E[6]  = s;
    M.E[9] = -s;  M.E[10] = c;
    return(M);
}

inline mat4
Mat4RotationY(r32 Angle)
{
    mat4 M = Mat4Identity();
    r32 c = cosf(Angle);
    r32 s = sinf(Angle);

    // [ c  0  -s   0 ]
    // [ 0  1   0   0 ]
    // [ s  0   c   0 ]
    // [ 0  0   0   1 ]
    M.E[0] = c;  M.E[2]  = -s;
    M.E[8] = s;  M.E[10] =  c;
    return(M);
}

inline mat4
Mat4RotationZ(r32 Angle)
{
    mat4 M = Mat4Identity();
    r32 c = cosf(Angle);
    r32 s = sinf(Angle);

    // [ c  s   0   0 ]
    // [-s  c   0   0 ]
    // [ 0  0   1   0 ]
    // [ 0  0   0   1 ]
    M.E[0] =  c;  M.E[1] = s;
    M.E[4] = -s;  M.E[5] = c;
    return(M);
}

// NOTE(yigit): Added for OBJ texture coordinates.  Deliberately bare - no
// operators until something actually needs one.
struct vec2
{
    r32 X, Y;
};

inline vec2
Vec2(r32 X, r32 Y)
{
    vec2 Result = {X, Y};
    return(Result);
}

struct vec3
{
    r32 X, Y, Z;
};

// Forward declarations - Mat4RotationAxis below needs these, and they are
// defined further down with the rest of the vector maths.
inline vec3 Vec3(r32 X, r32 Y, r32 Z);
inline vec3 Normalize(vec3 A);

// Rotation about any axis (Rodrigues' formula).  This is the general case -
// Mat4RotationX/Y/Z are just this with the axis on a basis vector, and they
// stay because they are cheaper and clearer when that is all you need.
//
// NOTE(yigit): The axis is normalized here, so callers can pass something
// like (0.5, 1, 0) without worrying about its length.  GLM's rotate does the
// same.  A zero-length axis yields the identity rather than NaNs.
inline mat4
Mat4RotationAxis(vec3 Axis, r32 Angle)
{
    mat4 M = Mat4Identity();

    vec3 A = Normalize(Axis);
    r32 c = cosf(Angle);
    r32 s = sinf(Angle);
    r32 t = 1.0f - c;

    // As in Luna chapter 3, one source line per row:
    //   [ c + t*x*x     t*x*y + s*z   t*x*z - s*y ]
    //   [ t*x*y - s*z   c + t*y*y     t*y*z + s*x ]
    //   [ t*x*z + s*y   t*y*z - s*x   c + t*z*z   ]
    M.E[0] = t*A.X*A.X + c;      M.E[1] = t*A.X*A.Y + s*A.Z;  M.E[2]  = t*A.X*A.Z - s*A.Y;
    M.E[4] = t*A.X*A.Y - s*A.Z;  M.E[5] = t*A.Y*A.Y + c;      M.E[6]  = t*A.Y*A.Z + s*A.X;
    M.E[8] = t*A.X*A.Z + s*A.Y;  M.E[9] = t*A.Y*A.Z - s*A.X;  M.E[10] = t*A.Z*A.Z + c;

    return(M);
}

inline vec3
Vec3(r32 X, r32 Y, r32 Z)
{
    vec3 Result = {X, Y, Z};
    return(Result);
}

struct vec4
{
    r32 X, Y, Z, W;
};

inline vec4
Vec4(r32 X, r32 Y, r32 Z, r32 W)
{
    vec4 Result = {X, Y, Z, W};
    return(Result);
}

// NOTE(yigit): Mirrors GLSL's vec4(someVec3, w).
inline vec4
Vec4(vec3 XYZ, r32 W)
{
    vec4 Result = {XYZ.X, XYZ.Y, XYZ.Z, W};
    return(Result);
}

// -------------------------------------------------------------------------
// vec3 operators
// -------------------------------------------------------------------------
inline vec3 operator-(vec3 A)              { return(Vec3(-A.X, -A.Y, -A.Z)); }
inline vec3 operator+(vec3 A, vec3 B)      { return(Vec3(A.X + B.X, A.Y + B.Y, A.Z + B.Z)); }
inline vec3 operator-(vec3 A, vec3 B)      { return(Vec3(A.X - B.X, A.Y - B.Y, A.Z - B.Z)); }
inline vec3 operator*(vec3 A, r32 S)    { return(Vec3(A.X * S, A.Y * S, A.Z * S)); }
inline vec3 operator*(r32 S, vec3 A)    { return(Vec3(A.X * S, A.Y * S, A.Z * S)); }
inline vec3 operator/(vec3 A, r32 S)    { return(Vec3(A.X / S, A.Y / S, A.Z / S)); }

inline vec3 &operator+=(vec3 &A, vec3 B)   { A = A + B; return(A); }
inline vec3 &operator-=(vec3 &A, vec3 B)   { A = A - B; return(A); }
inline vec3 &operator*=(vec3 &A, r32 S) { A = A * S; return(A); }

// -------------------------------------------------------------------------
// vec4 operators
// -------------------------------------------------------------------------
inline vec4 operator-(vec4 A)              { return(Vec4(-A.X, -A.Y, -A.Z, -A.W)); }
inline vec4 operator+(vec4 A, vec4 B)      { return(Vec4(A.X + B.X, A.Y + B.Y, A.Z + B.Z, A.W + B.W)); }
inline vec4 operator-(vec4 A, vec4 B)      { return(Vec4(A.X - B.X, A.Y - B.Y, A.Z - B.Z, A.W - B.W)); }
inline vec4 operator*(vec4 A, r32 S)    { return(Vec4(A.X * S, A.Y * S, A.Z * S, A.W * S)); }
inline vec4 operator*(r32 S, vec4 A)    { return(Vec4(A.X * S, A.Y * S, A.Z * S, A.W * S)); }
inline vec4 operator/(vec4 A, r32 S)    { return(Vec4(A.X / S, A.Y / S, A.Z / S, A.W / S)); }

inline vec4 &operator+=(vec4 &A, vec4 B)   { A = A + B; return(A); }
inline vec4 &operator-=(vec4 &A, vec4 B)   { A = A - B; return(A); }
inline vec4 &operator*=(vec4 &A, r32 S) { A = A * S; return(A); }

// -------------------------------------------------------------------------
// Transforming a vector by a matrix: v * M.  The result is a mix of M's rows,
// with v's components as the amounts - X of row 0, plus Y of row 1, plus Z of
// row 2, plus W of row 3.  So each output component is v dotted with one
// COLUMN of M.
//
// NOTE(yigit): W decides whether translation applies.  W = 1 means "this is a
// position", so the bottom row (E[12..14]) is added in.  W = 0 means "this
// is a direction", and translation drops out - which is what you want for
// normals and velocities, since moving something shouldn't rotate which way
// it faces.
// -------------------------------------------------------------------------
inline vec4
operator*(vec4 V, mat4 M)
{
    vec4 Result;
    Result.X = V.X*M.E[0] + V.Y*M.E[4] + V.Z*M.E[8]  + V.W*M.E[12];
    Result.Y = V.X*M.E[1] + V.Y*M.E[5] + V.Z*M.E[9]  + V.W*M.E[13];
    Result.Z = V.X*M.E[2] + V.Y*M.E[6] + V.Z*M.E[10] + V.W*M.E[14];
    Result.W = V.X*M.E[3] + V.Y*M.E[7] + V.Z*M.E[11] + V.W*M.E[15];
    return(Result);
}

// -------------------------------------------------------------------------
// Vector products and lengths
// -------------------------------------------------------------------------

// Sum of componentwise products.  For unit vectors this is the cosine of the
// angle between them: 1 means same direction, 0 perpendicular, -1 opposite.
inline r32 Dot(vec3 A, vec3 B) { return(A.X*B.X + A.Y*B.Y + A.Z*B.Z); }
inline r32 Dot(vec4 A, vec4 B) { return(A.X*B.X + A.Y*B.Y + A.Z*B.Z + A.W*B.W); }

// A vector perpendicular to both inputs.  Order matters - Cross(A, B) points
// the opposite way from Cross(B, A).  Only meaningful in 3D.
inline vec3
Cross(vec3 A, vec3 B)
{
    vec3 Result;
    Result.X = A.Y*B.Z - A.Z*B.Y;
    Result.Y = A.Z*B.X - A.X*B.Z;
    Result.Z = A.X*B.Y - A.Y*B.X;
    return(Result);
}

// -------------------------------------------------------------------------
// View matrix from a camera position, a point to look at, and which way is up
// (Luna 5.6.2) - his u, v, w are R, U, F here.
//
// The camera's own world transform has its axes (right, up, forward) as its
// ROWS and Eye as its bottom row - that is what placing the camera in the
// world means.  The view matrix is the INVERSE of that: it moves the whole
// world in front of a fixed eye at the origin.  Inverting a rotation is just
// transposing it, so the camera's axes become the COLUMNS here; and undoing
// the move turns Eye into -Eye dotted with each axis, in the bottom row.
// Together: a point's view-space X is (P - Eye) dotted with R - how far to
// the camera's right - and likewise Y with U and Z with F.
//
// NOTE(yigit): F points FORWARD, from the eye toward the target, because this
// left-handed camera looks down +Z - so Target - Eye.  And Right is
// Cross(Up, F), in that order: (0,1,0) x (0,0,1) = (1,0,0).
// Same as XMMatrixLookAtLH.
// -------------------------------------------------------------------------
inline mat4
Mat4LookAt(vec3 Eye, vec3 Target, vec3 Up)
{
    vec3 F = Normalize(Target - Eye);       // forward
    vec3 R = Normalize(Cross(Up, F));       // right
    vec3 U = Cross(F, R);                   // true up, already unit length

    mat4 M = Mat4Identity();

    //   [ R.X       U.X       F.X       0 ]
    //   [ R.Y       U.Y       F.Y       0 ]
    //   [ R.Z       U.Z       F.Z       0 ]
    //   [ -R.Eye    -U.Eye    -F.Eye    1 ]
    M.E[0]  = R.X;           M.E[1]  = U.X;           M.E[2]  = F.X;
    M.E[4]  = R.Y;           M.E[5]  = U.Y;           M.E[6]  = F.Y;
    M.E[8]  = R.Z;           M.E[9]  = U.Z;           M.E[10] = F.Z;
    M.E[12] = -Dot(R, Eye);  M.E[13] = -Dot(U, Eye);  M.E[14] = -Dot(F, Eye);

    return(M);
}

// NOTE(yigit): Squared length skips the square root.  Use it whenever you are
// only comparing distances - "is A nearer than B" needs no sqrt.
inline r32 LengthSq(vec3 A) { return(Dot(A, A)); }
inline r32 LengthSq(vec4 A) { return(Dot(A, A)); }

inline r32 Length(vec3 A) { return(sqrtf(LengthSq(A))); }
inline r32 Length(vec4 A) { return(sqrtf(LengthSq(A))); }

// NOTE(yigit): Returns a zero vector rather than NaNs if the input has no
// length.  A NaN silently poisons every later calculation and is miserable to
// track down, so the branch is worth it.
inline vec3
Normalize(vec3 A)
{
    vec3 Result = {};
    r32 Len = Length(A);
    if(Len > 0.0f)
    {
        Result = A / Len;
    }
    return(Result);
}

inline vec4
Normalize(vec4 A)
{
    vec4 Result = {};
    r32 Len = Length(A);
    if(Len > 0.0f)
    {
        Result = A / Len;
    }
    return(Result);
}

// NOTE(yigit): Standard FPS camera basis vectors, derived from Yaw only
// (so movement stays on the ground plane even while looking up/down).
// Yaw = 0 looks down +Z, and a positive Yaw turns right - the same as
// Mat4RotationY(Yaw) applied to +Z and +X.
inline vec3
CameraForward(r32 Yaw)
{
    return Vec3(sinf(Yaw), 0.0f, cosf(Yaw));
}

inline vec3
CameraRight(r32 Yaw)
{
    return Vec3(cosf(Yaw), 0.0f, -sinf(Yaw));
}


#endif
