#if !defined(ARENA_H)
/*
  NOTE(yigit): The memory arena - how the game allocates.  On its own
  because both game_state (game.h) and the loaders that fill it (the WAV
  loader in game_sound.h) need it, and neither should have to include the
  other to get it.
*/

#include "base.h"

/*
  A block of memory handed out front to back, and never freed piece by
  piece - only all at once, with ResetArena.  All the game's allocation goes
  through these, carved out of the blocks in game_memory.
*/
struct memory_arena
{
    memory_index Size;
    u8 *Base;
    memory_index Used;
};

// NOTE(yigit): An arena starts zeroed along with the rest of the storage it
// sits in, so its Size is 0 and the first push would assert.  Every arena
// has to be handed its block explicitly first.
internal void
InitializeArena(memory_arena *Arena, memory_index Size, u8 *Base)
{
    Arena->Size = Size;
    Arena->Used = 0;
    Arena->Base = Base;
}

#define PushStruct(Arena, type, ...) (type *)PushSize_(Arena, sizeof(type), ## __VA_ARGS__)
#define PushArray(Arena, Count, type, ...) (type *)PushSize_(Arena, (Count)*sizeof(type), ## __VA_ARGS__)

// How many bytes to skip so the next push starts on an Alignment boundary.
//
// NOTE(yigit): Measured on the ADDRESS, not on Used.  An arena's Base is not
// aligned itself - WorldArena starts just past game_state - so a Used that is
// a multiple of 16 can still be an address that is not.
internal memory_index
GetAlignmentOffset(memory_arena *Arena, memory_index Alignment)
{
    // A power of two has exactly one bit set, so subtracting one sets every
    // bit below it - which is the mask the & below needs.
    Assert((Alignment & (Alignment - 1)) == 0);

    memory_index ResultPointer = (memory_index)(Arena->Base + Arena->Used);
    memory_index AlignmentMask = Alignment - 1;

    memory_index AlignmentOffset = 0;
    if(ResultPointer & AlignmentMask)
    {
        AlignmentOffset = Alignment - (ResultPointer & AlignmentMask);
    }

    return(AlignmentOffset);
}

/*
  Every push starts on a 16-byte boundary, unless asked otherwise:

    PushStruct(Arena, thing)         16
    PushStruct(Arena, thing, 64)     64 - a cache line
    PushArray(Arena, 100, i16, 2)  2  - for packing small things tightly

  NOTE(yigit): 16 because that is what the SSE types need - XMVECTOR and
  XMMATRIX are loaded with instructions that crash on an address that is not
  a multiple of 16.  Wasting up to 15 bytes a push is the price.
*/
internal void *
PushSize_(memory_arena *Arena, memory_index Size, memory_index Alignment = 16)
{
    memory_index AlignmentOffset = GetAlignmentOffset(Arena, Alignment);
    Size += AlignmentOffset;

    Assert((Arena->Used + Size) <= Arena->Size);
    void *Result = Arena->Base + Arena->Used + AlignmentOffset;
    Arena->Used += Size;

    return(Result);
}

// Hands the whole block back at once.  Only safe when nothing still points
// into the arena.
internal void
ResetArena(memory_arena *Arena)
{
    Arena->Used = 0;
}

#define ARENA_H
#endif
