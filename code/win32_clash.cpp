/*
  NOTE(yigit): The Windows platform layer - the .exe.  Opens the window,
  owns the memory, reads the keyboard and mouse, keeps time, and loads the
  game from game.dll - reloading it whenever it is rebuilt, while running.

  Carried over from the OpenGL project's win32_handmade.cpp: the same memory
  model, file services, DLL hot reload, input recording (L), pause (P), raw
  mouse input, Tab for a free cursor, F11 for fullscreen.  Sound is WASAPI
  rather than DirectSound.  Left behind: the gamepad, and OpenGL - DirectX is
  created by the game itself.

  The platform knows nothing about DirectX.  Drawing is entirely the game's.
  (The one exception: the two Agility SDK numbers exported below.)
*/

/* 
  TODO(yigit):
  D3D12 initialization.
*/

#include "base.h"

// NOTE(yigit): WIN32_LEAN_AND_MEAN leaves out the rarely used half of
// windows.h, which halves its compile time.  NOMINMAX stops it defining
// min() and max() as macros, which break any later code that says "min".
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>      // snprintf

// WASAPI's device list: which sound card is the default one.
#include <mmdeviceapi.h>

#include "platform.h"
#include "win32_clash.h"

/*
  NOTE(yigit): The DirectX 12 Agility SDK - the one place the platform
  mentions DirectX, and only as two numbers.  When d3d12.dll loads, it looks
  for these two exports in the EXE (not in game.dll) and, if it finds them,
  uses the D3D12Core.dll and d3d12SDKLayers.dll in that folder instead of
  the ones built into Windows.  They ship together, so the debug layer always
  matches the runtime - and Windows 10 gets the newest D3D12.

  D3D12SDKVersion must equal D3D12_SDK_VERSION in
  code/external/D3D12AgilitySDK/include/d3d12.h; RendererInitialize logs an
  error if they ever disagree.  D3D12SDKPath is relative to clash.exe -
  build.bat copies the DLLs into build\D3D12\.
*/
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 619; }
extern "C" { __declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\"; }

// The size of the window's INSIDE - what gets drawn into.  The outer size
// adds the title bar and borders on top of this.
#define START_CLIENT_WIDTH  1280
#define START_CLIENT_HEIGHT 720

// NOTE(yigit): The longest dt the game is ever handed, in seconds.  A
// breakpoint or a window drag can stall a frame for seconds, and anything
// that moves by speed times dt would then fly across the scene on the next
// one.  The frame is already lost; this decides whether the camera is too.
#define WIN32_MAX_FRAME_SECONDS 0.1f

global_variable b32 GlobalRunning;
global_variable b32 GlobalPause;

// Set by the window procedure.  The game sees the size through game_input
// and resizes its swapchain itself.
global_variable i32 GlobalClientWidth;
global_variable i32 GlobalClientHeight;

// Tab toggles it - see game_input::CursorFree.
global_variable b32 GlobalCursorFree;

// NOTE(yigit): Remembers where and how big the window was before going
// fullscreen, so coming back restores it exactly - including whether it was
// maximised.
global_variable WINDOWPLACEMENT GlobalWindowPosition = {sizeof(GlobalWindowPosition)};

// ---------------------------------------------------------------------------
// Strings and paths
// ---------------------------------------------------------------------------

// NOTE(yigit): Truncates rather than overruns.  Dest is always null-terminated
// as long as DestCount is non-zero.
internal void
CatStrings(size_t SourceACount, char *SourceA,
           size_t SourceBCount, char *SourceB,
           size_t DestCount, char *Dest)
{
    if(DestCount == 0)
    {
        return;
    }

    // Reserve one byte for the terminator up front, so every copy below is
    // bounded by what is actually left.
    size_t Remaining = DestCount - 1;

    if(SourceACount > Remaining)
    {
        SourceACount = Remaining;
    }
    for(size_t Index = 0; Index < SourceACount; ++Index)
    {
        *Dest++ = *SourceA++;
    }
    Remaining -= SourceACount;

    if(SourceBCount > Remaining)
    {
        SourceBCount = Remaining;
    }
    for(size_t Index = 0; Index < SourceBCount; ++Index)
    {
        *Dest++ = *SourceB++;
    }

    *Dest = 0;
}

internal size_t
StringLength(char *String)
{
    size_t Count = 0;
    while(*String++)
    {
        ++Count;
    }
    return(Count);
}

internal void
Win32GetEXEFileName(win32_state *State)
{
    DWORD SizeOfFilename = GetModuleFileNameA(0, State->EXEFileName, sizeof(State->EXEFileName));

    // NOTE(yigit): On truncation GetModuleFileNameA returns the buffer size
    // and does NOT null-terminate on every Windows version.  A truncated path
    // is useless for building sibling paths anyway, so fall back to an empty
    // prefix - paths relative to the current directory.
    if((SizeOfFilename == 0) || (SizeOfFilename >= sizeof(State->EXEFileName)))
    {
        State->EXEFileName[0] = 0;
    }
    State->EXEFileName[sizeof(State->EXEFileName) - 1] = 0;

    State->OnePastLastEXEFileNameSlash = State->EXEFileName;
    for(char *Scan = State->EXEFileName; *Scan; ++Scan)
    {
        if(*Scan == '\\')
        {
            State->OnePastLastEXEFileNameSlash = Scan + 1;
        }
    }
}

// A file next to the exe: "C:\clash\build\" + FileName.
internal void
Win32BuildEXEPathFileName(win32_state *State, char *FileName,
                          size_t DestCount, char *Dest)
{
    CatStrings((size_t)(State->OnePastLastEXEFileNameSlash - State->EXEFileName), State->EXEFileName,
               StringLength(FileName), FileName,
               DestCount, Dest);
}

/*
  Makes "the current directory" the project root - C:\clash - wherever the
  program was started from.  The exe sits in build\, the shaders in data\, so
  from the root every file is simply "data\name.hlsl".

  NOTE(yigit): Without this, the answer to "where is data\?" depends on how
  the program was started: double-clicked, from a shell, from Visual Studio
  or RemedyBG, which all pick a different starting directory.
*/
internal void
Win32SetWorkingDirectoryToProjectRoot(win32_state *State)
{
    char Path[WIN32_STATE_FILE_NAME_COUNT];
    CatStrings(StringLength(State->EXEFileName), State->EXEFileName, 0, 0,
               sizeof(Path), Path);

    // "C:\clash\build\clash.exe" - cut after the last two backslashes, which
    // leaves "C:\clash".
    for(i32 Cut = 0; Cut < 2; ++Cut)
    {
        char *LastSlash = 0;
        for(char *At = Path; *At; ++At)
        {
            if(*At == '\\')
            {
                LastSlash = At;
            }
        }

        if(LastSlash)
        {
            *LastSlash = 0;
        }
    }

    SetCurrentDirectoryA(Path);
}

// ---------------------------------------------------------------------------
// Services for the game (see platform.h)
// ---------------------------------------------------------------------------

DEBUG_PLATFORM_LOG(DEBUGPlatformLog)
{
    if(Message)
    {
        OutputDebugStringA(Message);
    }
}

DEBUG_PLATFORM_FREE_FILE_MEMORY(DEBUGPlatformFreeFileMemory)
{
    if(Memory)
    {
        VirtualFree(Memory, 0, MEM_RELEASE);
    }
}

DEBUG_PLATFORM_READ_ENTIRE_FILE(DEBUGPlatformReadEntireFile)
{
    debug_read_file_result Result = {};

    HANDLE FileHandle = CreateFileA(Filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if(FileHandle != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER FileSize;
        if(GetFileSizeEx(FileHandle, &FileSize))
        {
            u32 FileSize32 = SafeTruncateUInt64((u64)FileSize.QuadPart);
            Result.Contents = VirtualAlloc(0, FileSize32, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
            if(Result.Contents)
            {
                DWORD BytesRead;
                if(ReadFile(FileHandle, Result.Contents, FileSize32, &BytesRead, 0) &&
                   (FileSize32 == BytesRead))
                {
                    Result.ContentsSize = FileSize32;
                }
                else
                {
                    DEBUGPlatformFreeFileMemory(Thread, Result.Contents);
                    Result.Contents = 0;
                }
            }
        }

        CloseHandle(FileHandle);
    }

    return(Result);
}

DEBUG_PLATFORM_WRITE_ENTIRE_FILE(DEBUGPlatformWriteEntireFile)
{
    b32 Result = false;

    HANDLE FileHandle = CreateFileA(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(FileHandle != INVALID_HANDLE_VALUE)
    {
        DWORD BytesWritten;
        if(WriteFile(FileHandle, Memory, MemorySize, &BytesWritten, 0))
        {
            Result = (BytesWritten == MemorySize);
        }

        CloseHandle(FileHandle);
    }

    return(Result);
}

inline FILETIME
Win32GetLastWriteTime(const char *Filename)
{
    FILETIME LastWriteTime = {};

    WIN32_FILE_ATTRIBUTE_DATA Data;
    if(GetFileAttributesExA(Filename, GetFileExInfoStandard, &Data))
    {
        LastWriteTime = Data.ftLastWriteTime;
    }

    return(LastWriteTime);
}

DEBUG_PLATFORM_GET_FILE_WRITE_TIME(DEBUGPlatformGetFileWriteTime)
{
    // NOTE(yigit): A FILETIME is already two halves of a 64-bit count of
    // 100ns ticks, so packing it loses nothing.  The game treats the result
    // as opaque and only ever tests it for equality.
    FILETIME WriteTime = Win32GetLastWriteTime(Filename);
    ULARGE_INTEGER Result;
    Result.LowPart = WriteTime.dwLowDateTime;
    Result.HighPart = WriteTime.dwHighDateTime;
    return(Result.QuadPart);
}

// ---------------------------------------------------------------------------
// The game DLL, and reloading it
// ---------------------------------------------------------------------------

/*
  NOTE(yigit): Loads a COPY of the DLL, never the DLL itself.  Windows locks
  a loaded DLL, so the compiler could not write a new game.dll over it -
  loading game_temp.dll leaves game.dll free to be rebuilt.
*/
internal win32_game_code
Win32LoadGameCode(char *SourceDLLName, char *TempDLLName)
{
    win32_game_code Result = {};

    Result.DLLLastWriteTime = Win32GetLastWriteTime(SourceDLLName);

    CopyFileA(SourceDLLName, TempDLLName, FALSE);
    Result.GameCodeDLL = LoadLibraryA(TempDLLName);
    if(Result.GameCodeDLL)
    {
        Result.UpdateAndRender = (game_update_and_render *)
            GetProcAddress(Result.GameCodeDLL, "GameUpdateAndRender");
        Result.GetSoundSamples = (game_get_sound_samples *)
            GetProcAddress(Result.GameCodeDLL, "GameGetSoundSamples");
        Result.Shutdown = (game_shutdown *)
            GetProcAddress(Result.GameCodeDLL, "GameShutdown");

        Result.IsValid = (Result.UpdateAndRender && Result.GetSoundSamples && Result.Shutdown);
    }

    if(!Result.IsValid)
    {
        Result.UpdateAndRender = 0;
        Result.GetSoundSamples = 0;
        Result.Shutdown = 0;
    }

    return(Result);
}

internal void
Win32UnloadGameCode(win32_game_code *GameCode)
{
    if(GameCode->GameCodeDLL)
    {
        FreeLibrary(GameCode->GameCodeDLL);
        GameCode->GameCodeDLL = 0;
    }

    GameCode->IsValid = false;
    GameCode->UpdateAndRender = 0;
    GameCode->GetSoundSamples = 0;
    GameCode->Shutdown = 0;
}

// ---------------------------------------------------------------------------
// Sound (WASAPI)
// ---------------------------------------------------------------------------

// How far ahead of the speakers we keep the sound card fed.  Shorter answers
// a key press sooner; longer survives a slow frame without a gap.  50 ms is
// three frames at 60 Hz.
#define WIN32_SOUND_LATENCY_SECONDS 0.05f

/*
  Opens the default sound device and starts it playing.  On any failure the
  sound stays !IsValid and the game runs without it.

  NOTE(yigit): WASAPI is COM, like DirectX: objects are made through
  interfaces and released when done.  Two things differ from DirectSound in
  the OpenGL project.  There is no play cursor or write cursor to reason
  about - only "how many samples are still waiting".  And shared mode mixes
  with every other program's sound, so the device's own format is fixed by
  Windows; AUTOCONVERTPCM makes WASAPI convert ours to it, so we can ask for
  plain 16-bit stereo whatever the device uses.
*/
internal void
Win32InitWASAPI(win32_sound_output *Sound, u32 SamplesPerSecond)
{
    Sound->SamplesPerSecond = SamplesPerSecond;

    // COM has to be started on a thread before any COM object is made on it.
    // S_FALSE means it already was, which is fine too.
    HRESULT Result = CoInitializeEx(0, COINIT_MULTITHREADED);
    if(FAILED(Result))
    {
        OutputDebugStringA("ERROR: CoInitializeEx failed - no sound.\n");
        return;
    }

    // The list of sound devices, and from it the default output - the one
    // Windows' own volume icon controls.
    IMMDeviceEnumerator *Enumerator = 0;
    Result = CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL,
                              IID_PPV_ARGS(&Enumerator));
    if(FAILED(Result))
    {
        OutputDebugStringA("ERROR: Could not list the sound devices - no sound.\n");
        return;
    }

    IMMDevice *Device = 0;
    Result = Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device);
    Enumerator->Release();
    if(FAILED(Result))
    {
        OutputDebugStringA("ERROR: No default sound device - no sound.\n");
        return;
    }

    Result = Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0,
                              (void **)&Sound->AudioClient);
    Device->Release();
    if(FAILED(Result))
    {
        OutputDebugStringA("ERROR: Could not open the sound device - no sound.\n");
        return;
    }

    // What we will hand it: 16-bit samples, stereo, interleaved - the same as
    // game_sound_output_buffer.
    WAVEFORMATEX Format = {};
    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.nChannels = 2;
    Format.nSamplesPerSec = SamplesPerSecond;
    Format.wBitsPerSample = 16;
    Format.nBlockAlign = (Format.nChannels * Format.wBitsPerSample) / 8;
    Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;

    // The ring: a tenth of a second, in WASAPI's unit of 100 nanoseconds.
    // Twice the latency we aim for, so topping up never has to wait.
    REFERENCE_TIME BufferDuration = 10000000 / 10;
    DWORD Flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    Result = Sound->AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, Flags,
                                            BufferDuration, 0, &Format, 0);
    if(FAILED(Result))
    {
        OutputDebugStringA("ERROR: The sound device would not take our format - no sound.\n");
        Sound->AudioClient->Release();
        Sound->AudioClient = 0;
        return;
    }

    Result = Sound->AudioClient->GetService(IID_PPV_ARGS(&Sound->RenderClient));
    if(FAILED(Result))
    {
        OutputDebugStringA("ERROR: No render client from the sound device - no sound.\n");
        Sound->AudioClient->Release();
        Sound->AudioClient = 0;
        return;
    }

    // The ring may not be exactly what was asked for; this is what it is.
    UINT32 BufferSizeInFrames = 0;
    Sound->AudioClient->GetBufferSize(&BufferSizeInFrames);
    Sound->BufferSizeInFrames = BufferSizeInFrames;

    Sound->LatencyFrames = (u32)(WIN32_SOUND_LATENCY_SECONDS * (r32)SamplesPerSecond);
    if(Sound->LatencyFrames > Sound->BufferSizeInFrames)
    {
        Sound->LatencyFrames = Sound->BufferSizeInFrames;
    }

    Sound->Samples = (i16 *)VirtualAlloc(0, Sound->BufferSizeInFrames * 2 * sizeof(i16),
                                           MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(!Sound->Samples)
    {
        return;
    }

    Sound->AudioClient->Start();
    Sound->IsValid = true;
}

/*
  Tops the sound card's ring back up to LatencyFrames, with samples from the
  game - or with silence, when WriteSilence is set (paused, minimised).
*/
internal void
Win32FillSoundBuffer(win32_sound_output *Sound, win32_game_code *Game,
                     thread_context *Thread, game_memory *Memory, b32 WriteSilence)
{
    if(!Sound->IsValid)
    {
        return;
    }

    // How many frames are still waiting to be played.
    UINT32 Padding = 0;
    if(FAILED(Sound->AudioClient->GetCurrentPadding(&Padding)))
    {
        return;
    }

    if(Padding >= Sound->LatencyFrames)
    {
        return;
    }

    u32 FramesToWrite = Sound->LatencyFrames - Padding;

    game_sound_output_buffer SoundBuffer = {};
    SoundBuffer.SamplesPerSecond = (i32)Sound->SamplesPerSecond;
    SoundBuffer.SampleCount = (i32)FramesToWrite;
    SoundBuffer.Samples = Sound->Samples;

    b32 Silent = WriteSilence || !Game->GetSoundSamples;
    if(!Silent)
    {
        Game->GetSoundSamples(Thread, Memory, &SoundBuffer);
    }

    // Borrow that many frames of the ring, fill them, give them back.
    BYTE *Destination = 0;
    if(SUCCEEDED(Sound->RenderClient->GetBuffer(FramesToWrite, &Destination)))
    {
        if(!Silent)
        {
            CopyMemory(Destination, Sound->Samples, FramesToWrite * 2 * sizeof(i16));
        }

        // SILENT tells WASAPI to play zeros for these, without our having to
        // write them.
        Sound->RenderClient->ReleaseBuffer(FramesToWrite, Silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }
}

// ---------------------------------------------------------------------------
// Input recording and playback (L)
//
// Press L: the whole game memory is saved, and every frame's input from
// then on is written to a file.  Press L again: the memory is put back and
// the recorded input replays, looping - so a moment can be watched again and
// again while the game DLL is rebuilt underneath it.  Press L once more to
// stop.
// ---------------------------------------------------------------------------

internal void
Win32GetInputFileLocation(win32_state *State, b32 InputStream,
                          i32 SlotIndex, size_t DestCount, char *Dest)
{
    char Temp[64];
    snprintf(Temp, sizeof(Temp), "loop_edit_%d_%s.hmi", SlotIndex, InputStream ? "input" : "state");
    Win32BuildEXEPathFileName(State, Temp, DestCount, Dest);
}

/*
  NOTE(yigit): Made the first time a slot is used, not at startup.  Each one
  is a file as big as the whole game memory - over a gigabyte - and the
  OpenGL project made all four on every launch, for a feature used now and
  then.
*/
internal win32_replay_buffer *
Win32GetReplayBuffer(win32_state *State, i32 Index)
{
    Assert((Index > 0) && (Index < (i32)ArrayCount(State->ReplayBuffers)));
    win32_replay_buffer *ReplayBuffer = &State->ReplayBuffers[Index];

    if(!ReplayBuffer->MemoryBlock)
    {
        Win32GetInputFileLocation(State, false, Index,
                                  sizeof(ReplayBuffer->FileName), ReplayBuffer->FileName);

        ReplayBuffer->FileHandle = CreateFileA(ReplayBuffer->FileName,
                                               GENERIC_WRITE|GENERIC_READ, 0, 0,
                                               CREATE_ALWAYS, 0, 0);

        LARGE_INTEGER MaxSize;
        MaxSize.QuadPart = (LONGLONG)State->TotalSize;
        ReplayBuffer->MemoryMap = CreateFileMappingA(ReplayBuffer->FileHandle, 0, PAGE_READWRITE,
                                                     (DWORD)MaxSize.HighPart, MaxSize.LowPart, 0);

        ReplayBuffer->MemoryBlock = MapViewOfFile(ReplayBuffer->MemoryMap, FILE_MAP_ALL_ACCESS,
                                                  0, 0, (size_t)State->TotalSize);
        if(!ReplayBuffer->MemoryBlock)
        {
            OutputDebugStringA("ERROR: Could not map an input-recording file.\n");
        }
    }

    return(ReplayBuffer);
}

internal void
Win32BeginRecordingInput(win32_state *State, i32 InputRecordingIndex)
{
    win32_replay_buffer *ReplayBuffer = Win32GetReplayBuffer(State, InputRecordingIndex);
    if(ReplayBuffer->MemoryBlock)
    {
        State->InputRecordingIndex = InputRecordingIndex;

        char FileName[WIN32_STATE_FILE_NAME_COUNT];
        Win32GetInputFileLocation(State, true, InputRecordingIndex, sizeof(FileName), FileName);
        State->RecordingHandle = CreateFileA(FileName, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);

        CopyMemory(ReplayBuffer->MemoryBlock, State->GameMemoryBlock, (size_t)State->TotalSize);
    }
}

internal void
Win32EndRecordingInput(win32_state *State)
{
    CloseHandle(State->RecordingHandle);
    State->InputRecordingIndex = 0;
}

internal void
Win32BeginInputPlayBack(win32_state *State, i32 InputPlayingIndex)
{
    win32_replay_buffer *ReplayBuffer = Win32GetReplayBuffer(State, InputPlayingIndex);
    if(ReplayBuffer->MemoryBlock)
    {
        State->InputPlayingIndex = InputPlayingIndex;

        char FileName[WIN32_STATE_FILE_NAME_COUNT];
        Win32GetInputFileLocation(State, true, InputPlayingIndex, sizeof(FileName), FileName);
        State->PlaybackHandle = CreateFileA(FileName, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);

        CopyMemory(State->GameMemoryBlock, ReplayBuffer->MemoryBlock, (size_t)State->TotalSize);
    }
}

internal void
Win32EndInputPlayBack(win32_state *State)
{
    CloseHandle(State->PlaybackHandle);
    State->InputPlayingIndex = 0;
}

internal void
Win32RecordInput(win32_state *State, game_input *NewInput)
{
    DWORD BytesWritten;
    WriteFile(State->RecordingHandle, NewInput, sizeof(*NewInput), &BytesWritten, 0);
}

internal void
Win32PlayBackInput(win32_state *State, game_input *NewInput)
{
    DWORD BytesRead = 0;
    if(ReadFile(State->PlaybackHandle, NewInput, sizeof(*NewInput), &BytesRead, 0))
    {
        if(BytesRead == 0)
        {
            // The end of the recording: start the loop again - memory and all.
            i32 PlayingIndex = State->InputPlayingIndex;
            Win32EndInputPlayBack(State);
            Win32BeginInputPlayBack(State, PlayingIndex);
            ReadFile(State->PlaybackHandle, NewInput, sizeof(*NewInput), &BytesRead, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

/*
  Windows calls this for everything that happens to the window.  Anything
  not handled here goes to DefWindowProc, which does the standard thing.

  NOTE(yigit): Keyboard and raw mouse messages never arrive here - the main
  loop takes them out of the queue first, in Win32ProcessPendingMessages.
  They are input for this frame, and have to land in this frame's game_input.
*/
internal LRESULT CALLBACK
Win32WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    LRESULT Result = 0;

    switch(Message)
    {
        case WM_CLOSE:
        {
            GlobalRunning = false;
        } break;

        case WM_DESTROY:
        {
            GlobalRunning = false;
            PostQuitMessage(0);
        } break;

        case WM_SIZE:
        {
            // The new INNER size, packed into LParam: width low, height high.
            GlobalClientWidth  = LOWORD(LParam);
            GlobalClientHeight = HIWORD(LParam);
        } break;

        case WM_PAINT:
        {
            // NOTE(yigit): Windows keeps sending WM_PAINT until it is told
            // the window has been painted.  BeginPaint/EndPaint says so, and
            // fills it with the class's black brush - which is all there is
            // to see until the renderer draws something.
            PAINTSTRUCT Paint;
            BeginPaint(Window, &Paint);
            EndPaint(Window, &Paint);
        } break;

        default:
        {
            Result = DefWindowProcA(Window, Message, WParam, LParam);
        } break;
    }

    return(Result);
}

internal void
Win32ToggleFullscreen(HWND Window)
{
    DWORD Style = GetWindowLong(Window, GWL_STYLE);
    if(Style & WS_OVERLAPPEDWINDOW)
    {
        // Going fullscreen: save the current placement, drop the border, and
        // snap to the bounds of whichever monitor the window is mostly on.
        MONITORINFO MonitorInfo = {sizeof(MonitorInfo)};
        if(GetWindowPlacement(Window, &GlobalWindowPosition) &&
           GetMonitorInfo(MonitorFromWindow(Window, MONITOR_DEFAULTTOPRIMARY), &MonitorInfo))
        {
            SetWindowLong(Window, GWL_STYLE, Style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(Window, HWND_TOP,
                         MonitorInfo.rcMonitor.left,
                         MonitorInfo.rcMonitor.top,
                         MonitorInfo.rcMonitor.right - MonitorInfo.rcMonitor.left,
                         MonitorInfo.rcMonitor.bottom - MonitorInfo.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }
    else
    {
        // Coming back: restore the border, then the saved placement.
        SetWindowLong(Window, GWL_STYLE, Style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(Window, &GlobalWindowPosition);
        SetWindowPos(Window, 0, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Records a button going up or down.  Only counts a CHANGE, so Windows'
// key-repeat messages while a key is held do not add transitions.
internal void
Win32ProcessKeyboardMessage(game_button_state *NewState, b32 IsDown)
{
    if(NewState->EndedDown != IsDown)
    {
        NewState->EndedDown = IsDown;
        ++NewState->HalfTransitionCount;
    }
}

/*
  Starts this frame's input from last frame's.

  NOTE(yigit): Two game_input buffers are swapped every frame, so NewInput
  still holds what was written into it TWO frames ago.  Everything is reset
  here and only "is it held" is carried over - from OldInput, the frame just
  gone.  Without that, a key held across the swap would look released, and
  the transition counts would pile up across frames.

  Unlike the OpenGL project, the MOUSE buttons get this too.  There they kept
  their two-frames-old state, which is why "pressed this frame" could not be
  trusted for the mouse.
*/
internal void
Win32BeginInput(game_input *NewInput, game_input *OldInput)
{
    for(u32 Index = 0; Index < ArrayCount(NewInput->Keyboard.Buttons); ++Index)
    {
        NewInput->Keyboard.Buttons[Index].HalfTransitionCount = 0;
        NewInput->Keyboard.Buttons[Index].EndedDown =
            OldInput->Keyboard.Buttons[Index].EndedDown;
    }

    for(u32 Index = 0; Index < MOUSE_BUTTON_COUNT; ++Index)
    {
        NewInput->MouseButtons[Index].HalfTransitionCount = 0;
        NewInput->MouseButtons[Index].EndedDown = OldInput->MouseButtons[Index].EndedDown;
    }

    // Raw mouse movement and the wheel ADD UP across all of a frame's
    // messages, so they start from zero each frame.
    NewInput->MouseDeltaX = 0;
    NewInput->MouseDeltaY = 0;
    NewInput->MouseZ = 0;
}

/*
  Takes everything Windows has queued since last frame.  Keyboard and raw
  mouse messages go into NewInput; the rest go on to Win32WindowProc.
*/
internal void
Win32ProcessPendingMessages(win32_state *State, game_input *NewInput)
{
    game_controller_input *Keyboard = &NewInput->Keyboard;

    MSG Message;
    while(PeekMessageA(&Message, 0, 0, 0, PM_REMOVE))
    {
        switch(Message.message)
        {
            case WM_QUIT:
            {
                GlobalRunning = false;
            } break;

            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_KEYDOWN:
            case WM_KEYUP:
            {
                u32 VKCode = (u32)Message.wParam;

                // NOTE(yigit): Since we are comparing WasDown to IsDown, we
                // MUST use == and != to turn these bit tests into actual 0 or
                // 1 values.
                b32 WasDown = ((Message.lParam & (1 << 30)) != 0);
                b32 IsDown  = ((Message.lParam & (1 << 31)) == 0);

                // Only CHANGES.  Holding a key sends a stream of repeated
                // key-downs, all with WasDown already set.
                if(WasDown != IsDown)
                {
                    if     (VKCode == 'W')       { Win32ProcessKeyboardMessage(&Keyboard->MoveUp, IsDown); }
                    else if(VKCode == 'A')       { Win32ProcessKeyboardMessage(&Keyboard->MoveLeft, IsDown); }
                    else if(VKCode == 'S')       { Win32ProcessKeyboardMessage(&Keyboard->MoveDown, IsDown); }
                    else if(VKCode == 'D')       { Win32ProcessKeyboardMessage(&Keyboard->MoveRight, IsDown); }
                    else if(VKCode == 'Q')       { Win32ProcessKeyboardMessage(&Keyboard->LeftShoulder, IsDown); }
                    else if(VKCode == 'E')       { Win32ProcessKeyboardMessage(&Keyboard->RightShoulder, IsDown); }
                    else if(VKCode == VK_UP)     { Win32ProcessKeyboardMessage(&Keyboard->ActionUp, IsDown); }
                    else if(VKCode == VK_DOWN)   { Win32ProcessKeyboardMessage(&Keyboard->ActionDown, IsDown); }
                    else if(VKCode == VK_LEFT)   { Win32ProcessKeyboardMessage(&Keyboard->ActionLeft, IsDown); }
                    else if(VKCode == VK_RIGHT)  { Win32ProcessKeyboardMessage(&Keyboard->ActionRight, IsDown); }
                    else if(VKCode == VK_SPACE)  { Win32ProcessKeyboardMessage(&Keyboard->Back, IsDown); }
                    else if(VKCode == VK_RETURN) { Win32ProcessKeyboardMessage(&Keyboard->Start, IsDown); }

                    // The rest are the platform's own, and act at once.
                    else if((VKCode == VK_ESCAPE) && IsDown)
                    {
                        GlobalRunning = false;
                    }
                    else if((VKCode == VK_TAB) && IsDown)
                    {
                        GlobalCursorFree = !GlobalCursorFree;

                        // NOTE(yigit): ShowCursor keeps a COUNTER, not an
                        // on/off switch - so one call per toggle, never one
                        // per frame, or the count runs away and the cursor
                        // never hides again.
                        ShowCursor(GlobalCursorFree);
                    }
                    else if((VKCode == VK_F11) && IsDown)
                    {
                        Win32ToggleFullscreen(Message.hwnd);
                    }
#if CLASH_DEBUG
                    else if((VKCode == 'P') && IsDown)
                    {
                        GlobalPause = !GlobalPause;
                    }
                    else if((VKCode == 'L') && IsDown)
                    {
                        // Off -> recording -> playing -> off.
                        if(State->InputPlayingIndex == 0)
                        {
                            if(State->InputRecordingIndex == 0)
                            {
                                Win32BeginRecordingInput(State, 1);
                            }
                            else
                            {
                                Win32EndRecordingInput(State);
                                Win32BeginInputPlayBack(State, 1);
                            }
                        }
                        else
                        {
                            Win32EndInputPlayBack(State);
                        }
                    }
#endif
                }

                // NOTE(yigit): Alt+F4 has to be handled by hand, because key
                // messages are not passed on to DefWindowProc, which is what
                // normally closes the window for it.
                b32 AltKeyWasDown = ((Message.lParam & (1 << 29)) != 0);
                if((VKCode == VK_F4) && AltKeyWasDown)
                {
                    GlobalRunning = false;
                }
            } break;

            case WM_INPUT:
            {
                // NOTE(yigit): Raw input reports PHYSICAL mouse movement, so
                // our own SetCursorPos re-centring never shows up here.  Raw
                // deltas are also unfiltered: no pointer acceleration, no
                // desktop speed setting, and no clamping at the screen edges.
                RAWINPUT Raw;
                UINT Size = sizeof(Raw);
                if(GetRawInputData((HRAWINPUT)Message.lParam, RID_INPUT,
                                   &Raw, &Size, sizeof(RAWINPUTHEADER)) != (UINT)-1)
                {
                    if(Raw.header.dwType == RIM_TYPEMOUSE)
                    {
                        // MOUSE_MOVE_ABSOLUTE comes from tablets and remote
                        // desktop, where lLastX/Y are screen coordinates
                        // rather than a delta.  Ignore those here.
                        if(!(Raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                        {
                            NewInput->MouseDeltaX += Raw.data.mouse.lLastX;
                            NewInput->MouseDeltaY += Raw.data.mouse.lLastY;
                        }
                    }
                }

                // NOTE(yigit): WM_INPUT must reach DefWindowProc so the
                // system can clean up the buffer it allocated for this
                // message.
                DefWindowProcA(Message.hwnd, Message.message, Message.wParam, Message.lParam);
            } break;

            case WM_MOUSEWHEEL:
            {
                // The wheel reports in units of WHEEL_DELTA (120) per notch.
                // Divided here, so the game sees whole notches.
                i32 WheelDelta = GET_WHEEL_DELTA_WPARAM(Message.wParam);
                NewInput->MouseZ += WheelDelta / WHEEL_DELTA;
            } break;

            default:
            {
                TranslateMessage(&Message);
                DispatchMessageA(&Message);
            } break;
        }
    }
}

/*
  The mouse's buttons and position for this frame, and the cursor pinned to
  the window's centre in camera mode.
*/
internal void
Win32UpdateMouse(HWND Window, game_input *NewInput)
{
    if(GetActiveWindow() == Window)
    {
        // Polled rather than read from messages: a button released outside
        // the window never sends this window a button-up message, and would
        // stay "held" forever.
        int VirtualKeys[MOUSE_BUTTON_COUNT] = {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON,
                                              VK_XBUTTON1, VK_XBUTTON2};
        for(u32 Index = 0; Index < MOUSE_BUTTON_COUNT; ++Index)
        {
            b32 IsDown = ((GetKeyState(VirtualKeys[Index]) & (1 << 15)) != 0);
            Win32ProcessKeyboardMessage(&NewInput->MouseButtons[Index], IsDown);
        }

        if(!GlobalCursorFree)
        {
            // Camera mode: pull the hidden cursor back to the centre, so it
            // never reaches the screen's edge and stops moving.
            POINT Centre = {GlobalClientWidth / 2, GlobalClientHeight / 2};
            ClientToScreen(Window, &Centre);
            SetCursorPos(Centre.x, Centre.y);

            NewInput->MouseX = GlobalClientWidth / 2;
            NewInput->MouseY = GlobalClientHeight / 2;
        }
        else
        {
            // Cursor mode: where it really is.  GetCursorPos measures from
            // the MONITOR's corner; ScreenToClient makes it the window's.
            POINT Mouse;
            GetCursorPos(&Mouse);
            ScreenToClient(Window, &Mouse);
            NewInput->MouseX = Mouse.x;
            NewInput->MouseY = Mouse.y;
        }
    }
    else
    {
        // Alt-Tabbed away: no movement, and nothing left held down.
        NewInput->MouseDeltaX = 0;
        NewInput->MouseDeltaY = 0;
        NewInput->MouseZ = 0;

        for(u32 Index = 0; Index < MOUSE_BUTTON_COUNT; ++Index)
        {
            Win32ProcessKeyboardMessage(&NewInput->MouseButtons[Index], false);
        }
    }

    // NOTE(yigit): Outside both branches, so it runs EVERY frame.  Each of
    // the two game_input buffers only holds what was last written into it -
    // skip a frame and one of them keeps a stale mode.
    NewInput->CursorFree = GlobalCursorFree;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int CALLBACK
WinMain(HINSTANCE Instance, HINSTANCE PrevInstance, LPSTR CommandLine, int ShowCode)
{
    win32_state Win32State = {};

    Win32GetEXEFileName(&Win32State);
    Win32SetWorkingDirectoryToProjectRoot(&Win32State);

    char SourceGameCodeDLLFullPath[WIN32_STATE_FILE_NAME_COUNT];
    Win32BuildEXEPathFileName(&Win32State, "game.dll",
                              sizeof(SourceGameCodeDLLFullPath), SourceGameCodeDLLFullPath);

    char TempGameCodeDLLFullPath[WIN32_STATE_FILE_NAME_COUNT];
    Win32BuildEXEPathFileName(&Win32State, "game_temp.dll",
                              sizeof(TempGameCodeDLLFullPath), TempGameCodeDLLFullPath);

    // Exists only while build.bat is writing game.dll - see the loop below.
    char GameCodeLockFullPath[WIN32_STATE_FILE_NAME_COUNT];
    Win32BuildEXEPathFileName(&Win32State, "lock.tmp",
                              sizeof(GameCodeLockFullPath), GameCodeLockFullPath);

    // NOTE(yigit): Without this, Windows pretends the screen is 96 DPI and
    // stretches the window up afterwards on a scaled display (125%, 150%) -
    // everything drawn comes out blurry, and the swapchain is a different
    // size from what is on screen.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSA WindowClass = {};
    WindowClass.style = CS_HREDRAW | CS_VREDRAW;
    WindowClass.lpfnWndProc = Win32WindowProc;
    WindowClass.hInstance = Instance;
    WindowClass.hCursor = LoadCursorA(0, IDC_ARROW);
    WindowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    WindowClass.lpszClassName = "ClashWindowClass";

    if(!RegisterClassA(&WindowClass))
    {
        MessageBoxA(0, "Could not register the window class.", "clash", MB_OK | MB_ICONERROR);
        return(1);
    }

    // Ask for the OUTER size that gives the INNER size we want.
    RECT WindowRect = {0, 0, START_CLIENT_WIDTH, START_CLIENT_HEIGHT};
    AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND Window = CreateWindowExA(0, WindowClass.lpszClassName, "clash",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  WindowRect.right - WindowRect.left,
                                  WindowRect.bottom - WindowRect.top,
                                  0, 0, Instance, 0);
    if(!Window)
    {
        MessageBoxA(0, "Could not create the window.", "clash", MB_OK | MB_ICONERROR);
        return(1);
    }

    // NOTE(yigit): Ask for raw mouse input - movement that is not affected
    // by pointer acceleration, the desktop mouse speed, screen edges, or our
    // own SetCursorPos re-centring.  Usage page 1 / usage 2 is "generic
    // desktop mouse".  dwFlags 0: only while this window is in front.
    RAWINPUTDEVICE RawMouse = {};
    RawMouse.usUsagePage = 0x01;
    RawMouse.usUsage = 0x02;
    RawMouse.dwFlags = 0;
    RawMouse.hwndTarget = Window;
    if(!RegisterRawInputDevices(&RawMouse, 1, sizeof(RawMouse)))
    {
        OutputDebugStringA("ERROR: RegisterRawInputDevices failed - the mouse will not turn anything.\n");
    }

    // Camera mode to start with: cursor hidden.  Tab frees it.
    GlobalCursorFree = false;
    ShowCursor(FALSE);

    // The real inner size.  On a small screen Windows may have shrunk it.
    RECT ClientRect;
    GetClientRect(Window, &ClientRect);
    GlobalClientWidth  = ClientRect.right - ClientRect.left;
    GlobalClientHeight = ClientRect.bottom - ClientRect.top;

    // NOTE(yigit): At a fixed address in development builds, so every run
    // lays the game's memory out identically - a recorded state file then
    // still makes sense in the next run, pointers and all.  Anywhere the
    // system likes in a release build.
#if CLASH_DEBUG
    LPVOID BaseAddress = (LPVOID)Terabytes(2);
#else
    LPVOID BaseAddress = 0;
#endif

    game_memory GameMemory = {};
    GameMemory.PermanentStorageSize = Megabytes(64);
    GameMemory.TransientStorageSize = Gigabytes(1);
    GameMemory.RendererStorageSize  = Megabytes(1);
    GameMemory.PlatformWindow = Window;
#if CLASH_DEBUG
    GameMemory.DEBUGPlatformFreeFileMemory = DEBUGPlatformFreeFileMemory;
    GameMemory.DEBUGPlatformReadEntireFile = DEBUGPlatformReadEntireFile;
    GameMemory.DEBUGPlatformWriteEntireFile = DEBUGPlatformWriteEntireFile;
    GameMemory.DEBUGPlatformLog = DEBUGPlatformLog;
    GameMemory.DEBUGPlatformGetFileWriteTime = DEBUGPlatformGetFileWriteTime;
#endif

    // Permanent and transient as ONE block - the unit input recording
    // saves.  VirtualAlloc hands memory back already zeroed, which the game
    // relies on.
    Win32State.TotalSize = GameMemory.PermanentStorageSize + GameMemory.TransientStorageSize;
    Win32State.GameMemoryBlock = VirtualAlloc(BaseAddress, (size_t)Win32State.TotalSize,
                                              MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    GameMemory.PermanentStorage = Win32State.GameMemoryBlock;
    GameMemory.TransientStorage = ((u8 *)GameMemory.PermanentStorage +
                                   GameMemory.PermanentStorageSize);

    // The renderer's block, apart from the other two - see game_memory.
    GameMemory.RendererStorage = VirtualAlloc(0, (size_t)GameMemory.RendererStorageSize,
                                              MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);

    if(!GameMemory.PermanentStorage || !GameMemory.RendererStorage)
    {
        ShowCursor(TRUE);
        MessageBoxA(Window, "Could not allocate the game's memory.", "clash", MB_OK | MB_ICONERROR);
        return(1);
    }

    // 48000 is what nearly every sound device runs at, so WASAPI rarely has
    // to convert.
    win32_sound_output SoundOutput = {};
    Win32InitWASAPI(&SoundOutput, 48000);

    win32_game_code Game = Win32LoadGameCode(SourceGameCodeDLLFullPath, TempGameCodeDLLFullPath);

    // Two, swapped every frame: this frame's input is built in NewInput,
    // while OldInput still holds last frame's to carry held keys over from.
    game_input Inputs[2] = {};
    game_input *NewInput = &Inputs[0];
    game_input *OldInput = &Inputs[1];

    // The high-resolution timer: counts ticks, Frequency of them per second.
    LARGE_INTEGER Frequency;
    QueryPerformanceFrequency(&Frequency);

    LARGE_INTEGER LastCounter;
    QueryPerformanceCounter(&LastCounter);

    // For the frame time shown in the title bar.
    r32 TitleTimer = 0.0f;
    i32 TitleFrames = 0;

    GlobalRunning = true;
    while(GlobalRunning)
    {
        // A new game.dll since the last frame: swap it in.  The game's
        // memory is untouched, so it carries on exactly where it was.
        //
        // NOTE(yigit): Not while lock.tmp exists - the linker is still
        // writing game.dll.  Caught halfway, the copy either fails or loads
        // a broken DLL, and the finished file then carries the same write
        // time already seen, so it would never be picked up at all.
        FILETIME NewDLLWriteTime = Win32GetLastWriteTime(SourceGameCodeDLLFullPath);
        b32 DLLBeingWritten = (GetFileAttributesA(GameCodeLockFullPath) != INVALID_FILE_ATTRIBUTES);
        if(!DLLBeingWritten && (CompareFileTime(&NewDLLWriteTime, &Game.DLLLastWriteTime) != 0))
        {
            Win32UnloadGameCode(&Game);
            Game = Win32LoadGameCode(SourceGameCodeDLLFullPath, TempGameCodeDLLFullPath);
        }

        Win32BeginInput(NewInput, OldInput);
        Win32ProcessPendingMessages(&Win32State, NewInput);
        Win32UpdateMouse(Window, NewInput);

        // How long the last frame took.
        LARGE_INTEGER Counter;
        QueryPerformanceCounter(&Counter);
        r32 SecondsElapsed = (r32)(Counter.QuadPart - LastCounter.QuadPart) /
                                (r32)Frequency.QuadPart;
        LastCounter = Counter;

        if(SecondsElapsed > WIN32_MAX_FRAME_SECONDS)
        {
            SecondsElapsed = WIN32_MAX_FRAME_SECONDS;
        }

        NewInput->dtForFrame = SecondsElapsed;
        NewInput->WindowWidth = GlobalClientWidth;
        NewInput->WindowHeight = GlobalClientHeight;

        // Minimised: the window is 0x0, and there is nothing to draw into.
        // Sleep instead of spinning the CPU at 100% on an empty loop.
        b32 Minimised = (GlobalClientWidth == 0) || (GlobalClientHeight == 0);
        if(Minimised || GlobalPause)
        {
            // The sound card still has to be fed - with silence - or it runs
            // dry and stutters when the game comes back.
            thread_context Thread = {};
            Win32FillSoundBuffer(&SoundOutput, &Game, &Thread, &GameMemory, true);

            Sleep(10);
        }
        else
        {
            if(Win32State.InputRecordingIndex)
            {
                Win32RecordInput(&Win32State, NewInput);
            }

            if(Win32State.InputPlayingIndex)
            {
                Win32PlayBackInput(&Win32State, NewInput);

                // NOTE(yigit): Everything replays except the window's size.
                // The swapchain has to match the window as it is NOW, not
                // as it was when the loop was recorded.
                NewInput->WindowWidth = GlobalClientWidth;
                NewInput->WindowHeight = GlobalClientHeight;
            }

            thread_context Thread = {};
            if(Game.UpdateAndRender)
            {
                Game.UpdateAndRender(&Thread, &GameMemory, NewInput);
            }

            // After the update, so a key pressed this frame is already heard.
            Win32FillSoundBuffer(&SoundOutput, &Game, &Thread, &GameMemory, false);

            // Frame time and rate in the title bar, twice a second.
            // Averaged, because a single frame's number flickers too fast to
            // read.
            TitleTimer += SecondsElapsed;
            ++TitleFrames;
            if(TitleTimer >= 0.5f)
            {
                char Title[128];
                snprintf(Title, sizeof(Title), "clash  -  %.2f ms  %.0f fps%s%s%s",
                         1000.0f*TitleTimer / (r32)TitleFrames,
                         (r32)TitleFrames / TitleTimer,
                         Win32State.InputRecordingIndex ? "  [RECORDING]" : "",
                         Win32State.InputPlayingIndex ? "  [PLAYING BACK]" : "",
                         SoundOutput.IsValid ? "" : "  [NO SOUND]");
                SetWindowTextA(Window, Title);

                TitleTimer = 0.0f;
                TitleFrames = 0;
            }
        }

        // This frame's input becomes last frame's.
        game_input *Temp = NewInput;
        NewInput = OldInput;
        OldInput = Temp;
    }

    thread_context Thread = {};
    if(Game.Shutdown)
    {
        Game.Shutdown(&Thread, &GameMemory);
    }
    Win32UnloadGameCode(&Game);

    // Stopped before the process ends, or the last few milliseconds in the
    // ring can come out as a click.
    if(SoundOutput.IsValid)
    {
        SoundOutput.AudioClient->Stop();
    }

    return(0);
}
