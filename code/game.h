#if !defined(GAME_H)
/*
  NOTE(yigit): The game layer's own types - game_state, and the headers it
  is built from.  The game code itself never calls Windows - but windows.h
  does arrive here, by way of SimpleMath (see game_directx_math.h), and the
  renderer below it needs it anyway: DirectX 12 only exists on Windows.
*/

#include "platform.h"
#include "arena.h"
#include "game_math.h"
#include "game_directx_math.h"      // Luna's maths, and the bridge to ours
#include "game_camera.h"
#include "game_sound.h"

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

    // Sound.  Space toggles it.  Plays data\music.wav if there is one, and
    // a quiet test tone if not - so there is always something to hear.
    bool32 SoundOn;
    loaded_sound Music;         // samples in WorldArena
    uint32 MusicPlayCursor;     // which frame of Music plays next
    real32 TonePhase;           // where the test tone's sine wave is, 0..2pi

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
