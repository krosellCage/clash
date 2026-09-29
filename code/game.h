#if !defined(GAME_H)
/*
  NOTE(yigit): The game layer's own types: the memory arena and game_state.
  Platform-independent - no windows.h here.  (The renderer below it is not:
  DirectX 12 only exists on Windows.)
*/

#include "platform.h"
#include "game_math.h"
#include "game_camera.h"

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

// Declared, never defined here - see the Renderer member below.
struct dx12_renderer;

struct game_state
{
    // NOTE(yigit): Seconds since startup, added up from Input->dtForFrame
    // rather than read off a clock.  dtForFrame travels inside game_input,
    // which input recording saves every frame, so time built this way plays
    // back exactly.
    real32 TimeSeconds;

    // NOTE(yigit): By value, in PermanentStorage - so it survives a DLL
    // reload, and input playback puts it back where the loop started.
    camera Camera;

    // Where the frame is seen from, worked out from Camera each frame for the
    // renderer.  (Nothing reads them yet - they are for the cube milestone.)
    mat4 View;
    mat4 Projection;

    // NOTE(yigit): An INCOMPLETE type, and only ever a pointer - into
    // RendererStorage, not into this block (see game_memory).  The game can
    // hand the renderer around without knowing what is inside one.
    dx12_renderer *Renderer;

    // Everything in PermanentStorage after game_state itself.
    memory_arena WorldArena;

    // All of TransientStorage.  Scratch: reset before each use.
    memory_arena TransientArena;
};

#define GAME_H
#endif
