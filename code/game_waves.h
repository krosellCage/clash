#if !defined(GAME_WAVES_H)
/*
  Luna's wave simulation (7.7) - his Waves class in C.  The book leaves the
  maths to [Lengyel02]; what matters for the renderer is only that the
  heights change every frame.

  A grid of points whose heights follow the wave equation: every TimeStep,
  each point's new height comes from its height now, its height one step
  ago, and its 4 neighbours' heights now.  WavesDisturb pokes a point up,
  and the wave spreads out from there and slowly dies down.
*/

#include "arena.h"                 // memory_arena, PushArray
#include "game_directx_math.h"     // XMFLOAT3

struct waves
{
    u32 RowCount;
    u32 ColumnCount;
    u32 VertexCount;

    // The wave equation's constants, worked out once from the speed,
    // damping, spacing and time step.
    r32 K1;
    r32 K2;
    r32 K3;

    r32 TimeStep;           // seconds between simulation steps
    r32 TimeSinceStep;      // added up from each frame's time until it reaches TimeStep

    // The points now, and one step ago.  Each step writes the new heights
    // over the oldest ones, then swaps the two pointers.
    XMFLOAT3 *Previous;
    XMFLOAT3 *Current;
};

// A flat RowCount x ColumnCount grid, Spacing apart, laid out like
// MeshGenGrid: row by row from the far (+z) edge, left (-x) to right - so
// MeshGenGrid's indices fit it.
internal void
WavesInitialize(waves *Waves, memory_arena *Arena, u32 RowCount, u32 ColumnCount,
                r32 Spacing, r32 TimeStep, r32 Speed, r32 Damping)
{
    Waves->RowCount = RowCount;
    Waves->ColumnCount = ColumnCount;
    Waves->VertexCount = RowCount*ColumnCount;
    Waves->TimeStep = TimeStep;
    Waves->TimeSinceStep = 0.0f;

    r32 D = Damping*TimeStep + 2.0f;
    r32 E = (Speed*Speed)*(TimeStep*TimeStep) / (Spacing*Spacing);
    Waves->K1 = (Damping*TimeStep - 2.0f) / D;
    Waves->K2 = (4.0f - 8.0f*E) / D;
    Waves->K3 = (2.0f*E) / D;

    Waves->Previous = PushArray(Arena, Waves->VertexCount, XMFLOAT3);
    Waves->Current = PushArray(Arena, Waves->VertexCount, XMFLOAT3);

    r32 HalfWidth = 0.5f*(r32)(ColumnCount - 1)*Spacing;
    r32 HalfDepth = 0.5f*(r32)(RowCount - 1)*Spacing;
    for(u32 Row = 0; Row < RowCount; ++Row)
    {
        r32 Z = HalfDepth - (r32)Row*Spacing;
        for(u32 Column = 0; Column < ColumnCount; ++Column)
        {
            r32 X = -HalfWidth + (r32)Column*Spacing;
            Waves->Previous[Row*ColumnCount + Column] = XMFLOAT3(X, 0.0f, Z);
            Waves->Current[Row*ColumnCount + Column] = XMFLOAT3(X, 0.0f, Z);
        }
    }
}

// Runs one simulation step whenever TimeStep seconds have gone by.
internal void
WavesUpdate(waves *Waves, r32 DeltaTime)
{
    Waves->TimeSinceStep += DeltaTime;
    if(Waves->TimeSinceStep >= Waves->TimeStep)
    {
        u32 RowLength = Waves->ColumnCount;

        // Only the inside points: the edges have no neighbour on one side,
        // so they stay at height 0.
        for(u32 Row = 1; Row < Waves->RowCount - 1; ++Row)
        {
            for(u32 Column = 1; Column < Waves->ColumnCount - 1; ++Column)
            {
                u32 Here = Row*RowLength + Column;
                r32 Neighbours = Waves->Current[Here - RowLength].y +     // row above
                                 Waves->Current[Here + RowLength].y +     // row below
                                 Waves->Current[Here - 1].y +             // left
                                 Waves->Current[Here + 1].y;              // right

                // Written into Previous: its old value is used here for
                // the last time, so the new height can take its place.
                Waves->Previous[Here].y = Waves->K1*Waves->Previous[Here].y +
                                          Waves->K2*Waves->Current[Here].y +
                                          Waves->K3*Neighbours;
            }
        }

        // Previous holds the newest heights now - swap, so Current does.
        XMFLOAT3 *Swap = Waves->Previous;
        Waves->Previous = Waves->Current;
        Waves->Current = Swap;

        Waves->TimeSinceStep = 0.0f;
    }
}

// Pushes the point at (Row, Column) up by Magnitude, and its 4 neighbours
// by half that.  Not on or next to the edge, so all 4 neighbours exist.
internal void
WavesDisturb(waves *Waves, u32 Row, u32 Column, r32 Magnitude)
{
    Assert((Row > 1) && (Row < Waves->RowCount - 2));
    Assert((Column > 1) && (Column < Waves->ColumnCount - 2));

    u32 RowLength = Waves->ColumnCount;
    u32 Here = Row*RowLength + Column;

    Waves->Current[Here].y += Magnitude;
    Waves->Current[Here - RowLength].y += 0.5f*Magnitude;
    Waves->Current[Here + RowLength].y += 0.5f*Magnitude;
    Waves->Current[Here - 1].y += 0.5f*Magnitude;
    Waves->Current[Here + 1].y += 0.5f*Magnitude;
}

#define GAME_WAVES_H
#endif
