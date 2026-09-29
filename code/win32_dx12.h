#if !defined(WIN32_DX12_H)
/*
  NOTE(yigit): The platform layer's own types - nothing here crosses to the
  game.
*/

// HMODULE, FILETIME, HANDLE - and the game's entry point types.  Both are
// already included by win32_dx12.cpp, so their include guards make these do
// nothing in the build; they are here so the file stands on its own.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "platform.h"

// The game DLL as currently loaded.
struct win32_game_code
{
    HMODULE GameCodeDLL;
    FILETIME DLLLastWriteTime;

    // IMPORTANT(yigit): Either can be 0 - a DLL that failed to load, or one
    // caught half-written by the compiler.  Check before calling.
    game_update_and_render *UpdateAndRender;
    game_shutdown *Shutdown;

    bool32 IsValid;
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
    uint64 TotalSize;
    void *GameMemoryBlock;

    // NOTE(yigit): Slot 0 means "not recording", so real slots start at 1.
    win32_replay_buffer ReplayBuffers[4];

    HANDLE RecordingHandle;
    int32 InputRecordingIndex;

    HANDLE PlaybackHandle;
    int32 InputPlayingIndex;

    // The exe's own path, for building the paths of files next to it.
    char EXEFileName[WIN32_STATE_FILE_NAME_COUNT];
    char *OnePastLastEXEFileNameSlash;
};

#define WIN32_DX12_H
#endif
