#if !defined(PLATFORM_H)
/*
  NOTE(yigit): The contract between the platform layer (win32_clash.cpp, the
  .exe) and the game (game.cpp, the .dll).  Everything that crosses between
  them is declared here, and nothing else does:

    platform -> game   input, memory, file services, the window
    game -> platform   three functions: GameUpdateAndRender,
                       GameGetSoundSamples, GameShutdown

  Carried over from the OpenGL project's handmade_platform.h.  Differences:
  no gamepad, no OpenGL function table - and a third block of memory,
  RendererStorage, for the reason given on game_memory.
*/

#include "base.h"

// NOTE(yigit): C linkage for everything the .exe and the .dll hand each
// other, so the function names GetProcAddress looks up are not mangled.
#ifdef __cplusplus
extern "C" {
#endif

// Placeholder for when there are threads.  Every platform service takes one,
// so adding threading later does not mean changing every call.
typedef struct thread_context
{
    i32 Placeholder;
} thread_context;

/*
  Services the platform provides to the game.
*/
#if CLASH_DEBUG
/* IMPORTANT(yigit):
   These are NOT for anything in a shipping game - they block, and the write
   does not protect against lost data.
*/
typedef struct debug_read_file_result
{
    u32 ContentsSize;
    void *Contents;
} debug_read_file_result;

#define DEBUG_PLATFORM_FREE_FILE_MEMORY(name) void name(thread_context *Thread, void *Memory)
typedef DEBUG_PLATFORM_FREE_FILE_MEMORY(debug_platform_free_file_memory);

#define DEBUG_PLATFORM_READ_ENTIRE_FILE(name) debug_read_file_result name(thread_context *Thread, const char *Filename)
typedef DEBUG_PLATFORM_READ_ENTIRE_FILE(debug_platform_read_entire_file);

#define DEBUG_PLATFORM_WRITE_ENTIRE_FILE(name) b32 name(thread_context *Thread, const char *Filename, u32 MemorySize, void *Memory)
typedef DEBUG_PLATFORM_WRITE_ENTIRE_FILE(debug_platform_write_entire_file);

// So the game can report errors without knowing what OutputDebugStringA is.
// Message must be null-terminated.
#define DEBUG_PLATFORM_LOG(name) void name(thread_context *Thread, const char *Message)
typedef DEBUG_PLATFORM_LOG(debug_platform_log);

// Opaque timestamp - the game only ever compares it for equality, so the
// platform can pack whatever it likes into it.  0 if the file is not there.
#define DEBUG_PLATFORM_GET_FILE_WRITE_TIME(name) u64 name(thread_context *Thread, const char *Filename)
typedef DEBUG_PLATFORM_GET_FILE_WRITE_TIME(debug_platform_get_file_write_time);
#endif

/*
  Input.
*/
typedef struct game_button_state
{
    // How many times the button changed - down or up - during the frame.
    // Usually 0 or 1; 2 means it was pressed AND released within one frame,
    // a tap too quick for EndedDown alone to ever show.
    i32 HalfTransitionCount;

    // Held when the frame ended.
    b32 EndedDown;
} game_button_state;

typedef struct game_controller_input
{
    union
    {
        game_button_state Buttons[12];
        struct
        {
            game_button_state MoveUp;           // W
            game_button_state MoveDown;         // S
            game_button_state MoveLeft;         // A
            game_button_state MoveRight;        // D

            game_button_state ActionUp;         // arrow up
            game_button_state ActionDown;       // arrow down
            game_button_state ActionLeft;       // arrow left
            game_button_state ActionRight;      // arrow right

            game_button_state LeftShoulder;     // Q
            game_button_state RightShoulder;    // E

            game_button_state Back;             // space
            game_button_state Start;            // enter

            // NOTE(yigit): All buttons must be added above this line.  It
            // exists only so the check below can prove the named buttons and
            // the Buttons[] array still line up.

            game_button_state Terminator;
        };
    };
} game_controller_input;

#define MOUSE_BUTTON_COUNT 5

typedef struct game_input
{
    game_controller_input Keyboard;

    // Left, middle, right, then the two side buttons.
    game_button_state MouseButtons[MOUSE_BUTTON_COUNT];

    // Where the cursor is inside the window, in pixels from the top-left.
    // In camera mode it is pinned to the centre, so these are the centre.
    i32 MouseX, MouseY;

    // How far the mouse physically moved this frame, in raw counts - what a
    // camera should turn by.  Unaffected by pointer acceleration or the
    // screen's edges.
    i32 MouseDeltaX, MouseDeltaY;

    // Wheel notches this frame: positive away from you.
    i32 MouseZ;

    // Tab toggles it.  False: the cursor is hidden and pinned, and the mouse
    // is for turning a camera.  True: the cursor is visible and free, for
    // pointing at things.
    b32 CursorFree;

    // How long the last frame took, in seconds.
    r32 dtForFrame;

    // The window's inner size.  The renderer resizes its swapchain when it
    // changes, and the projection matrix takes its aspect ratio from it.
    i32 WindowWidth;
    i32 WindowHeight;
} game_input;

/*
  Sound.  Each frame the platform asks for however many samples the sound
  card is about to run short of, and the game writes exactly that many.
*/
typedef struct game_sound_output_buffer
{
    i32 SamplesPerSecond;

    // How many to write - in FRAMES: one frame is a left and a right sample.
    i32 SampleCount;

    // Where to write them: SampleCount pairs of 16-bit samples, left then
    // right, back to back.
    i16 *Samples;
} game_sound_output_buffer;

/*
  Memory.  The game never allocates: the platform hands it these blocks once,
  cleared to zero, and the game carves everything it needs out of them.
*/
typedef struct game_memory
{
    b32 IsInitialized;

    // Everything that must last - game_state first, then WorldArena.
    u64 PermanentStorageSize;
    void *PermanentStorage;             // NOTE(yigit): REQUIRED to be cleared to zero at startup

    // Scratch - model parsing and the like.  Reset before each use.
    u64 TransientStorageSize;
    void *TransientStorage;             // NOTE(yigit): REQUIRED to be cleared to zero at startup

    /*
      NOTE(yigit): The renderer's own block, and deliberately NOT part of the
      other two.  Input playback (L) works by saving the whole permanent +
      transient block and copying it back at the start of every loop.  That
      is fine for game state, but the renderer's fence values would jump
      backwards while the GPU's did not, and any DirectX object made after
      the save would be forgotten while it still exists.  Kept apart, the
      renderer simply carries on through a loop.
    */
    u64 RendererStorageSize;
    void *RendererStorage;              // NOTE(yigit): REQUIRED to be cleared to zero at startup

    // The window, as the platform's own handle - an HWND on Windows.  Opaque
    // to the game; only the renderer turns it back into an HWND, to make the
    // swapchain.
    void *PlatformWindow;

#if CLASH_DEBUG
    debug_platform_free_file_memory *DEBUGPlatformFreeFileMemory;
    debug_platform_read_entire_file *DEBUGPlatformReadEntireFile;
    debug_platform_write_entire_file *DEBUGPlatformWriteEntireFile;
    debug_platform_log *DEBUGPlatformLog;
    debug_platform_get_file_write_time *DEBUGPlatformGetFileWriteTime;
#endif
} game_memory;

/*
  Services the game provides to the platform.
*/
#define GAME_UPDATE_AND_RENDER(name) void name(thread_context *Thread, game_memory *Memory, game_input *Input)
typedef GAME_UPDATE_AND_RENDER(game_update_and_render);

// NOTE(yigit): Has to be quick - it runs every frame, and a slow one delays
// the frame it is part of.  Only ever called after GameUpdateAndRender has
// run at least once, so game_state is set up by then.
#define GAME_GET_SOUND_SAMPLES(name) void name(thread_context *Thread, game_memory *Memory, game_sound_output_buffer *SoundBuffer)
typedef GAME_GET_SOUND_SAMPLES(game_get_sound_samples);

// Called once, on the way out, while the game's memory is still valid - so
// the renderer can wait for the GPU before anything is released.
#define GAME_SHUTDOWN(name) void name(thread_context *Thread, game_memory *Memory)
typedef GAME_SHUTDOWN(game_shutdown);

// NOTE(yigit): Called just before game.dll is unloaded for a hot reload,
// while its code is still loaded.  Anything holding pointers INTO the DLL -
// its functions, its string constants, memory from its own C runtime heap -
// is torn down here, and started again by the new DLL.  Dear ImGui is the
// one thing that needs it so far.  Not called at the final exit: GameShutdown
// covers that.
#define GAME_UNLOAD(name) void name(thread_context *Thread, game_memory *Memory)
typedef GAME_UNLOAD(game_unload);

#ifdef __cplusplus
}
#endif

// Checked when compiling, not when running: add a button above Terminator
// without growing Buttons[] and this refuses to build.
static_assert((offsetof(game_controller_input, Terminator) / sizeof(game_button_state)) ==
              ArrayCount(((game_controller_input *)0)->Buttons),
              "game_controller_input: named buttons and Buttons[] disagree");

// Went down during this frame - one frame per press, not every frame while
// held.  For toggles ("press F to...") rather than movement.
inline b32
WasPressed(game_button_state Button)
{
    b32 Result = ((Button.HalfTransitionCount > 1) ||
                     ((Button.HalfTransitionCount == 1) && Button.EndedDown));
    return(Result);
}

#define PLATFORM_H
#endif
