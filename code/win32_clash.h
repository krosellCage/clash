#if !defined(WIN32_CLASH_H)
/*
  NOTE(yigit): The platform layer's own types - nothing here crosses to the
  game.
*/

// HMODULE, FILETIME, HANDLE - and the game's entry point types.  Both are
// already included by win32_clash.cpp, so their include guards make these do
// nothing in the build; they are here so the file stands on its own.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// WASAPI - Windows' audio API (IAudioClient, IAudioRenderClient).
#include <audioclient.h>

#include "platform.h"

// The game DLL as currently loaded.
struct win32_game_code
{
    HMODULE GameCodeDLL;
    FILETIME DLLLastWriteTime;

    // IMPORTANT(yigit): Either can be 0 - a DLL that failed to load, or one
    // caught half-written by the compiler.  Check before calling.
    game_update_and_render *UpdateAndRender;
    game_get_sound_samples *GetSoundSamples;
    game_shutdown *Shutdown;
    game_unload *Unload;

    b32 IsValid;
};

/*
  The sound card, through WASAPI.

  WASAPI keeps a ring of samples waiting to be played.  Every frame we ask
  how many are still waiting, and top it back up to LatencyFrames - enough
  to last until the next frame, with room to spare for a slow one.
*/
struct win32_sound_output
{
    // False when there is no sound device, or it would not start.  The game
    // runs on regardless, silently.
    b32 IsValid;

    IAudioClient *AudioClient;
    IAudioRenderClient *RenderClient;

    u32 SamplesPerSecond;

    // Frames: one left and one right 16-bit sample - 4 bytes.
    u32 BufferSizeInFrames;         // the whole ring, as WASAPI made it
    u32 LatencyFrames;              // how full we keep it

    // Where the game writes before it is copied into WASAPI's ring.  Big
    // enough for the whole ring, the most we could ever ask for at once.
    i16 *Samples;
};

#define WIN32_STATE_FILE_NAME_COUNT MAX_PATH

/*
  One input-recording slot: a file on disk the size of the whole game memory,
  mapped into our address space, so saving the game's memory into it is a
  single copy.
*/
struct win32_replay_buffer
{
    HANDLE FileHandle;
    HANDLE MemoryMap;
    char FileName[WIN32_STATE_FILE_NAME_COUNT];
    void *MemoryBlock;
};

struct win32_state
{
    // The permanent and transient storage, as one block - which is what
    // input recording saves and restores.
    u64 TotalSize;
    void *GameMemoryBlock;

    // NOTE(yigit): Slot 0 means "not recording", so real slots start at 1.
    win32_replay_buffer ReplayBuffers[4];

    HANDLE RecordingHandle;
    i32 InputRecordingIndex;

    HANDLE PlaybackHandle;
    i32 InputPlayingIndex;

    // The exe's own path, for building the paths of files next to it.
    char EXEFileName[WIN32_STATE_FILE_NAME_COUNT];
    char *OnePastLastEXEFileNameSlash;
};

#define WIN32_CLASH_H
#endif
