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
MeshGenCylinder()
{

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
