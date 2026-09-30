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
    uint8 *Base;
    memory_index Used;
};

// NOTE(yigit): An arena starts zeroed along with the rest of the storage it
// sits in, so its Size is 0 and the first push would assert.  Every arena
// has to be handed its block explicitly first.
internal void
InitializeArena(memory_arena *Arena, memory_index Size, uint8 *Base)
{
    Arena->Size = Size;
    Arena->Used = 0;
    Arena->Base = Base;
}

#define PushStruct(Arena, type) (type *)PushSize_(Arena, sizeof(type))
#define PushArray(Arena, Count, type) (type *)PushSize_(Arena, (Count)*sizeof(type))

internal void *
PushSize_(memory_arena *Arena, memory_index Size)
{
    Assert((Arena->Used + Size) <= Arena->Size);
    void *Result = Arena->Base + Arena->Used;
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
