#if !defined(MESH_GEN_H)
#define MESH_GEN_H

/*
  Shapes made in code instead of loaded from files - Luna's MeshGen (7.5).
  Every function pushes its vertices and indices onto Arena; they are only
  needed until they are copied to the GPU, so a scratch arena is the place.
*/
#include "arena.h"                 // memory_arena, PushArray - and base.h: u16, u32, r32, Assert
#include "game_directx_math.h"     // XMFLOAT2, XMFLOAT3 (and "using namespace DirectX")
#include <string.h>                // memcpy, in MeshGenAppend
#include <math.h> 

struct mesh_gen_vertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;      // which way the surface faces - chapter 8 (light)
    XMFLOAT3 TangentU;    // along the surface - chapter 19 (normal maps)
    XMFLOAT2 TexC;        // texture coordinates - chapter 9 (textures)
};

struct mesh_gen_data
{
    u32 VertexCount;
    u32 IndexCount;
    mesh_gen_vertex *Vertices;
    u16 *Indices;

    u32 MaxVertices;      // only used when appending into this one
    u32 MaxIndices;
};

struct submesh
{
    u32 IndexCount;
    u32 StartIndex;     // first index to read, counted in indices
    i32 BaseVertex;     // added to every index, so each object's indices can start at 0
};

// A flat grid on the floor (the xz plane), centred on the origin: RowCount
// rows of vertices from the far edge (+z) to the near one, ColumnCount
// columns from left (-x) to right.  (RowCount - 1) x (ColumnCount - 1) cells,
// two triangles each.
internal mesh_gen_data
MeshGenGrid(memory_arena *Arena, r32 Width, r32 Depth, u32 RowCount, u32 ColumnCount)
{
    mesh_gen_data Result = {};
    Result.VertexCount = RowCount*ColumnCount;
    Result.IndexCount = (RowCount - 1)*(ColumnCount - 1)*6;    // 2 triangles per cell, 3 indices each
    Assert(Result.VertexCount <= 65536);                       // u16 indices can only count that far
    Result.Vertices = PushArray(Arena, Result.VertexCount, mesh_gen_vertex);
    Result.Indices = PushArray(Arena, Result.IndexCount, u16);

    r32 HalfWidth = 0.5f*Width;
    r32 HalfDepth = 0.5f*Depth;
    r32 ColumnSpacing = Width / (r32)(ColumnCount - 1);     // distance between columns, along x
    r32 RowSpacing = Depth / (r32)(RowCount - 1);           // distance between rows, along z
    r32 TexStepU = 1.0f / (r32)(ColumnCount - 1);           // texture coordinate steps
    r32 TexStepV = 1.0f / (r32)(RowCount - 1);

    // Vertices, row by row, starting at the far-left corner (-x, +z).
    for(u32 Row = 0; Row < RowCount; ++Row)
    {
        r32 Z = HalfDepth - (r32)Row*RowSpacing;
        for(u32 Column = 0; Column < ColumnCount; ++Column)
        {
            r32 X = -HalfWidth + (r32)Column*ColumnSpacing;

            mesh_gen_vertex *Vertex = &Result.Vertices[Row*ColumnCount + Column];
            Vertex->Position = XMFLOAT3(X, 0.0f, Z);
            Vertex->Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);     // flat floor: faces straight up
            Vertex->TangentU = XMFLOAT3(1.0f, 0.0f, 0.0f);
            Vertex->TexC = XMFLOAT2((r32)Column*TexStepU, (r32)Row*TexStepV);
        }
    }

    // Two triangles per cell, clockwise seen from above ("top" is the far
    // side, +z):
    //   TopLeft     TopRight
    //   BottomLeft  BottomRight
    u32 IndexAt = 0;
    for(u32 Row = 0; Row < RowCount - 1; ++Row)
    {
        for(u32 Column = 0; Column < ColumnCount - 1; ++Column)
        {
            u16 TopLeft = (u16)(Row*ColumnCount + Column);
            u16 TopRight = (u16)(Row*ColumnCount + Column + 1);
            u16 BottomLeft = (u16)((Row + 1)*ColumnCount + Column);
            u16 BottomRight = (u16)((Row + 1)*ColumnCount + Column + 1);

            Result.Indices[IndexAt++] = TopLeft;
            Result.Indices[IndexAt++] = TopRight;
            Result.Indices[IndexAt++] = BottomLeft;

            Result.Indices[IndexAt++] = BottomLeft;
            Result.Indices[IndexAt++] = TopRight;
            Result.Indices[IndexAt++] = BottomRight;
        }
    }

    return(Result);
}

internal mesh_gen_data
MeshGenCylinder(memory_arena *Arena, r32 BottomRadius, r32 TopRadius,
        r32 Height, u32 SliceCount, u32 StackCount)
{
    // Each ring has SliceCount + 1 vertices: the last one sits on top of the
    // first, but gets a different texture coordinate (u = 1 instead of 0).
    u32 RingCount = StackCount + 1;
    u32 RingVertexCount = SliceCount + 1;

    u32 SideVertexCount = RingCount*RingVertexCount;
    u32 SideIndexCount = StackCount*SliceCount*6;          // 2 triangles per square, 3 indices each
    u32 CapVertexCount = RingVertexCount + 1;              // one ring plus the centre
    u32 CapIndexCount = SliceCount*3;                      // one triangle per slice

    mesh_gen_data Result = {};
    Result.VertexCount = SideVertexCount + 2*CapVertexCount;
    Result.IndexCount = SideIndexCount + 2*CapIndexCount;
    Assert(Result.VertexCount <= 65536);                   // u16 indices can only count that far
    Result.Vertices = PushArray(Arena, Result.VertexCount, mesh_gen_vertex);
    Result.Indices = PushArray(Arena, Result.IndexCount, u16);

    r32 StackHeight = Height / (r32)StackCount;
    r32 RadiusStep = (TopRadius - BottomRadius) / (r32)StackCount;
    r32 SliceAngle = XM_2PI / (r32)SliceCount;              // angle between two vertices on a ring
    r32 HalfHeight = 0.5f*Height;

    // ------------------------------------------------------------------
    // Side vertices: ring by ring from the bottom, around each ring.
    // ------------------------------------------------------------------
    u32 VertexAt = 0;
    for(u32 Ring = 0; Ring < RingCount; ++Ring)
    {
        r32 Y = -HalfHeight + (r32)Ring*StackHeight;
        r32 Radius = BottomRadius + (r32)Ring*RadiusStep;

        for(u32 Slice = 0; Slice < RingVertexCount; ++Slice)
        {
            r32 Sin, Cos;
            XMScalarSinCos(&Sin, &Cos, (r32)Slice*SliceAngle);

            mesh_gen_vertex *Vertex = &Result.Vertices[VertexAt++];
            Vertex->Position = XMFLOAT3(Radius*Cos, Y, Radius*Sin);
            Vertex->TexC = XMFLOAT2((r32)Slice / (r32)SliceCount,
                                    1.0f - (r32)Ring / (r32)StackCount);

            // Tangent: around the ring (the direction the angle grows), length 1.
            Vertex->TangentU = XMFLOAT3(-Sin, 0.0f, Cos);

            // Bitangent: down the side, from this ring's point towards the
            // bottom ring's - leaning in or out when the radii differ.
            r32 RadiusDifference = BottomRadius - TopRadius;
            XMFLOAT3 Bitangent(RadiusDifference*Cos, -Height, RadiusDifference*Sin);

            // The normal is at right angles to both, pointing out of the side.
            XMVECTOR T = XMLoadFloat3(&Vertex->TangentU);
            XMVECTOR B = XMLoadFloat3(&Bitangent);
            XMStoreFloat3(&Vertex->Normal, XMVector3Normalize(XMVector3Cross(T, B)));
        }
    }

    // ------------------------------------------------------------------
    // Side indices: each square between two rings and two slices, 2
    // triangles, clockwise seen from outside:
    //   UpperLeft  UpperRight      (ring + 1)
    //   LowerLeft  LowerRight      (ring)
    // ------------------------------------------------------------------
    u32 IndexAt = 0;
    for(u32 Ring = 0; Ring < StackCount; ++Ring)
    {
        for(u32 Slice = 0; Slice < SliceCount; ++Slice)
        {
            u16 LowerLeft = (u16)(Ring*RingVertexCount + Slice);
            u16 LowerRight = (u16)(Ring*RingVertexCount + Slice + 1);
            u16 UpperLeft = (u16)((Ring + 1)*RingVertexCount + Slice);
            u16 UpperRight = (u16)((Ring + 1)*RingVertexCount + Slice + 1);

            Result.Indices[IndexAt++] = LowerLeft;
            Result.Indices[IndexAt++] = UpperLeft;
            Result.Indices[IndexAt++] = UpperRight;

            Result.Indices[IndexAt++] = LowerLeft;
            Result.Indices[IndexAt++] = UpperRight;
            Result.Indices[IndexAt++] = LowerRight;
        }
    }

    // ------------------------------------------------------------------
    // Caps: a ring of their own (the side's rings have sideways normals; a
    // cap needs them straight up or down) plus a centre vertex, and a fan
    // of triangles from the centre to each pair of ring vertices.
    // Cap 0 is the top, cap 1 the bottom.
    // ------------------------------------------------------------------
    for(u32 Cap = 0; Cap < 2; ++Cap)
    {
        b32 IsTop = (Cap == 0);
        r32 Y = IsTop ? HalfHeight : -HalfHeight;
        r32 Radius = IsTop ? TopRadius : BottomRadius;
        r32 NormalY = IsTop ? 1.0f : -1.0f;

        u32 FirstCapVertex = VertexAt;
        for(u32 Slice = 0; Slice < RingVertexCount; ++Slice)
        {
            r32 Sin, Cos;
            XMScalarSinCos(&Sin, &Cos, (r32)Slice*SliceAngle);
            r32 X = Radius*Cos;
            r32 Z = Radius*Sin;

            mesh_gen_vertex *Vertex = &Result.Vertices[VertexAt++];
            Vertex->Position = XMFLOAT3(X, Y, Z);
            Vertex->Normal = XMFLOAT3(0.0f, NormalY, 0.0f);
            Vertex->TangentU = XMFLOAT3(1.0f, 0.0f, 0.0f);
            // The texture scaled down with the cap's size, centred.
            Vertex->TexC = XMFLOAT2(X/Height + 0.5f, Z/Height + 0.5f);
        }

        u32 CentreVertex = VertexAt;
        mesh_gen_vertex *Centre = &Result.Vertices[VertexAt++];
        Centre->Position = XMFLOAT3(0.0f, Y, 0.0f);
        Centre->Normal = XMFLOAT3(0.0f, NormalY, 0.0f);
        Centre->TangentU = XMFLOAT3(1.0f, 0.0f, 0.0f);
        Centre->TexC = XMFLOAT2(0.5f, 0.5f);

        // Clockwise from outside: seen from above, the angle grows
        // anticlockwise, so the top goes centre, next, this one; the bottom,
        // seen from below, is mirrored and goes the other way.
        for(u32 Slice = 0; Slice < SliceCount; ++Slice)
        {
            u16 This = (u16)(FirstCapVertex + Slice);
            u16 Next = (u16)(FirstCapVertex + Slice + 1);

            Result.Indices[IndexAt++] = (u16)CentreVertex;
            Result.Indices[IndexAt++] = IsTop ? Next : This;
            Result.Indices[IndexAt++] = IsTop ? This : Next;
        }
    }

    Assert(VertexAt == Result.VertexCount);
    Assert(IndexAt == Result.IndexCount);
    return(Result);
}

internal mesh_gen_vertex
MeshGenMidPoint(mesh_gen_vertex *A, mesh_gen_vertex *B)
{
    XMVECTOR Position = 0.5f*(XMLoadFloat3(&A->Position) + XMLoadFloat3(&B->Position));
    XMVECTOR Normal = XMVector3Normalize(0.5f*(XMLoadFloat3(&A->Normal) + XMLoadFloat3(&B->Normal)));
    XMVECTOR TangentU =XMVector3Normalize(0.5f*(XMLoadFloat3(&A->TangentU) + XMLoadFloat3(&B->TangentU)));
    XMVECTOR TexC = 0.5f*(XMLoadFloat2(&A->TexC) + XMLoadFloat2(&B->TexC));

    mesh_gen_vertex Result;
    XMStoreFloat3(&Result.Position, Position);
    XMStoreFloat3(&Result.Normal, Normal);
    XMStoreFloat3(&Result.TangentU, TangentU);
    XMStoreFloat2(&Result.TexC, TexC);

    return(Result);
}

internal mesh_gen_data
MeshGenSubdivide(memory_arena *Arena, mesh_gen_data *Input)
{
    u32 TriangleCount = Input->IndexCount / 3;

    mesh_gen_data Result = {};
    Result.VertexCount = TriangleCount * 6;
    Result.IndexCount = TriangleCount * 12;
    Assert(Result.VertexCount <= 65536);
    Result.Vertices = PushArray(Arena, Result.VertexCount, mesh_gen_vertex);
    Result.Indices = PushArray(Arena, Result.IndexCount, u16);

    for(u32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
    {
        mesh_gen_vertex *V0 = &Input->Vertices[Input->Indices[Triangle*3 + 0]];
        mesh_gen_vertex *V1 = &Input->Vertices[Input->Indices[Triangle*3 + 1]];
        mesh_gen_vertex *V2 = &Input->Vertices[Input->Indices[Triangle*3 + 2]];

        mesh_gen_vertex M0 = MeshGenMidPoint(V0, V1);
        mesh_gen_vertex M1 = MeshGenMidPoint(V1, V2);
        mesh_gen_vertex M2 = MeshGenMidPoint(V0, V2);

        Result.Vertices[Triangle*6 + 0] = *V0;
        Result.Vertices[Triangle*6 + 1] = *V1;
        Result.Vertices[Triangle*6 + 2] = *V2;
        Result.Vertices[Triangle*6 + 3] = M0;
        Result.Vertices[Triangle*6 + 4] = M1;
        Result.Vertices[Triangle*6 + 5] = M2;

        u32 First = Triangle*6;

        Result.Indices[Triangle*12 + 0]  = (u16)(First + 0);     // bottom-left
        Result.Indices[Triangle*12 + 1]  = (u16)(First + 3);
        Result.Indices[Triangle*12 + 2]  = (u16)(First + 5);

        Result.Indices[Triangle*12 + 3]  = (u16)(First + 3);     // middle
        Result.Indices[Triangle*12 + 4]  = (u16)(First + 4);
        Result.Indices[Triangle*12 + 5]  = (u16)(First + 5);

        Result.Indices[Triangle*12 + 6]  = (u16)(First + 5);     // bottom-right
        Result.Indices[Triangle*12 + 7]  = (u16)(First + 4);
        Result.Indices[Triangle*12 + 8]  = (u16)(First + 2);

        Result.Indices[Triangle*12 + 9]  = (u16)(First + 3);     // top
        Result.Indices[Triangle*12 + 10] = (u16)(First + 1);
        Result.Indices[Triangle*12 + 11] = (u16)(First + 4);
    }

    return(Result);
}

internal mesh_gen_data
MeshGenGeosphere(memory_arena *Arena, r32 Radius, u32 SubdivisionCount)
{
    // An icosahedron: 12 vertices, 20 equal triangles - the roundest shape
    // made of equal triangles.  X and Z put its corners at distance 1 from
    // the centre.
    r32 X = 0.525731f;
    r32 Z = 0.850651f;

    XMFLOAT3 Positions[12] =
    {
        XMFLOAT3(-X, 0.0f, Z),  XMFLOAT3(X, 0.0f, Z),
        XMFLOAT3(-X, 0.0f, -Z), XMFLOAT3(X, 0.0f, -Z),
        XMFLOAT3(0.0f, Z, X),   XMFLOAT3(0.0f, Z, -X),
        XMFLOAT3(0.0f, -Z, X),  XMFLOAT3(0.0f, -Z, -X),
        XMFLOAT3(Z, X, 0.0f),   XMFLOAT3(-Z, X, 0.0f),
        XMFLOAT3(Z, -X, 0.0f),  XMFLOAT3(-Z, -X, 0.0f),
    };

    u16 Indices[60] =
    {
        1,4,0,   4,9,0,   4,5,9,   8,5,4,   1,8,4,
        1,10,8,  10,3,8,  8,3,5,   3,2,5,   3,7,2,
        3,10,7,  10,6,7,  6,11,7,  6,0,11,  6,1,0,
        10,1,6,  11,0,9,  2,11,9,  5,2,9,   11,2,7,
    };

    mesh_gen_data Result = {};
    Result.VertexCount = 12;          // how many positions?
    Result.IndexCount = 60;           // how many indices?
    Result.Vertices = PushArray(Arena, Result.VertexCount, mesh_gen_vertex);
    Result.Indices = PushArray(Arena, Result.IndexCount, u16);

    for(u32 Index = 0; Index < 12; ++Index)
    {
        Result.Vertices[Index] = {};
        Result.Vertices[Index].Position = Positions[Index];
    }

    memcpy(Result.Indices, Indices, sizeof(Indices));

    if(SubdivisionCount > 5)
    {
        SubdivisionCount = 5;
    }

    for(u32 Subdivision = 0; Subdivision < SubdivisionCount; ++Subdivision)
    {
        Result = MeshGenSubdivide(Arena, &Result);
    }

    for(u32 Vertex = 0; Vertex < Result.VertexCount; Vertex++)
    {
        XMVECTOR Direction = XMVector3Normalize(XMLoadFloat3(&Result.Vertices[Vertex].Position));
        XMStoreFloat3(&Result.Vertices[Vertex].Position, Radius*Direction);   // Radius × Direction
        XMStoreFloat3(&Result.Vertices[Vertex].Normal, Direction);     // just Direction

        // Texture coordinates from the point's two angles on the sphere:
        // Theta around the y axis (0..2pi), Phi down from the top (0..pi).
        XMFLOAT3 *P = &Result.Vertices[Vertex].Position;
        r32 Theta = atan2f(P->z, P->x);
        if(Theta < 0.0f)
        {
            Theta += XM_2PI;
        }
        r32 Phi = XMScalarACos(P->y / Radius);
        Result.Vertices[Vertex].TexC = XMFLOAT2(Theta / XM_2PI, Phi / XM_PI);

        // TangentU: the direction Theta grows in - around the y axis.
        XMVECTOR Tangent = XMVectorSet(-sinf(Phi)*sinf(Theta), 0.0f, sinf(Phi)*cosf(Theta), 0.0f);
        XMStoreFloat3(&Result.Vertices[Vertex].TangentU, XMVector3Normalize(Tangent));
    }
    return(Result);
}

internal r32
HillsHeight(r32 X, r32 Z)
{
    return(0.3f*( Z*sinf(0.1f*X) + X*cosf(0.1f*Z)));
}

// Room for several shapes, filled by MeshGenAppend.
internal mesh_gen_data
MeshGenAllocate(memory_arena *Arena, u32 MaxVertices, u32 MaxIndices)
{
    mesh_gen_data Result = {};
    Result.MaxVertices = MaxVertices;
    Result.MaxIndices = MaxIndices;
    Result.Vertices = PushArray(Arena, MaxVertices, mesh_gen_vertex);
    Result.Indices = PushArray(Arena, MaxIndices, u16);
    return(Result);
}

// Copies Source onto the end of Dest, and returns where it landed - the
// numbers DrawIndexedInstanced needs to draw just that shape.  Its indices
// are copied unchanged: BaseVertex shifts them (exercise 7).
internal submesh
MeshGenAppend(mesh_gen_data *Dest, mesh_gen_data *Source)
{
    Assert((Dest->VertexCount + Source->VertexCount) <= Dest->MaxVertices);
    Assert((Dest->IndexCount + Source->IndexCount) <= Dest->MaxIndices);

    submesh Result;
    Result.IndexCount = Source->IndexCount;
    Result.StartIndex = Dest->IndexCount;
    Result.BaseVertex = (i32)Dest->VertexCount;

    memcpy(Dest->Vertices + Dest->VertexCount, Source->Vertices, Source->VertexCount*sizeof(mesh_gen_vertex));
    memcpy(Dest->Indices + Dest->IndexCount, Source->Indices, Source->IndexCount*sizeof(u16));
    Dest->VertexCount += Source->VertexCount;
    Dest->IndexCount += Source->IndexCount;

    return(Result);
}
#endif
