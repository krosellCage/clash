/*
  NOTE(yigit): The game layer - built as game.dll, which the platform loads
  and reloads whenever it is rebuilt.  Everything it needs from the platform
  arrives through game_memory and game_input (platform.h); it never calls
  Windows itself.

  Three entry points, the only symbols the platform looks up:
    GameUpdateAndRender   once per frame
    GameGetSoundSamples   once per frame, after it
    GameShutdown          once, on the way out
*/

#include "game.h"

#include <stdio.h>      // snprintf, for the debug log line

#include "dx12_renderer.cpp"

// SimpleMath's constants (Vector3::Up, Matrix::Identity...) are defined in
// its .cpp - and this is a unity build, so it is compiled by including it.
#include "external/DirectXTK12/SimpleMath.cpp"

/*
  The renderer, started on first use.

  NOTE(yigit): From RendererStorage, not WorldArena - see game_memory for why
  it is kept out of the block that input playback copies back.
*/
internal dx12_renderer *
GetRenderer(thread_context *Thread, game_memory *Memory, game_input *Input)
{
    Assert(sizeof(dx12_renderer) <= Memory->RendererStorageSize);
    dx12_renderer *Renderer = (dx12_renderer *)Memory->RendererStorage;

    if(!Renderer->IsInitialized)
    {
        if(RendererInitialize(Renderer, Thread, Memory, (HWND)Memory->PlatformWindow,
                              Input->WindowWidth, Input->WindowHeight))
        {
            Renderer->IsInitialized = true;
        }
        else
        {
            // Nothing to draw with.  Reported once, and then every frame
            // after returns 0 until the next attempt - a rebuilt DLL with
            // the problem fixed gets its chance on the next frame.
            Memory->DEBUGPlatformLog(Thread, "ERROR: RendererInitialize failed.\n");
            return(0);
        }
    }

    return(Renderer);
}

extern "C" GAME_UPDATE_AND_RENDER(GameUpdateAndRender)
{
    Assert(sizeof(game_state) <= Memory->PermanentStorageSize);

    game_state *State = (game_state *)Memory->PermanentStorage;

    if(!Memory->IsInitialized)
    {
        // NOTE(yigit): game_state sits at the very front of PermanentStorage,
        // so WorldArena starts just past it and is that much smaller.  Start
        // it at offset 0 instead and its first allocation would hand out
        // memory game_state is already using.
        InitializeArena(&State->WorldArena,
                        (memory_index)(Memory->PermanentStorageSize - sizeof(game_state)),
                        (u8 *)Memory->PermanentStorage + sizeof(game_state));

        InitializeArena(&State->TransientArena,
                        (memory_index)Memory->TransientStorageSize,
                        (u8 *)Memory->TransientStorage);

        // NOTE(yigit): The storage starts zeroed, so every camera field would
        // be 0 - and a zero-length Front makes the view matrix degenerate.
        CameraInitialize(&State->Camera, Vec3(0.0f, 0.0f, 3.0f));

        // Music is optional.  Asked about first, so a project without a
        // music file does not log an error on every start.
        const char *MusicFileName = "data\\music.wav";
        if(Memory->DEBUGPlatformGetFileWriteTime(Thread, MusicFileName))
        {
            State->Music = LoadWAV(Thread, Memory, &State->WorldArena, MusicFileName);
        }

        Memory->IsInitialized = true;
    }

    State->TimeSeconds += Input->dtForFrame;

    // ------------------------------------------------------------------
    // Input
    // ------------------------------------------------------------------
    game_controller_input *Keyboard = &Input->Keyboard;
    r32 DeltaTime = Input->dtForFrame;

    // The mouse turns the camera - but not in cursor mode (Tab), where it is
    // pointing at things instead.
    if(!Input->CursorFree)
    {
        CameraProcessMouseLook(&State->Camera,
                               (r32)Input->MouseDeltaX,
                               (r32)Input->MouseDeltaY);
    }

    CameraProcessZoom(&State->Camera, (r32)Input->MouseZ);

    // Held keys: move every frame they are down, by speed times dt.
    if(Keyboard->MoveUp.EndedDown)    { CameraProcessMovement(&State->Camera, CameraMovement_Forward,  DeltaTime); }
    if(Keyboard->MoveDown.EndedDown)  { CameraProcessMovement(&State->Camera, CameraMovement_Backward, DeltaTime); }
    if(Keyboard->MoveLeft.EndedDown)  { CameraProcessMovement(&State->Camera, CameraMovement_Left,     DeltaTime); }
    if(Keyboard->MoveRight.EndedDown) { CameraProcessMovement(&State->Camera, CameraMovement_Right,    DeltaTime); }

    // Q and E adjust how fast the camera flies.  Scaled by DeltaTime like the
    // movement itself - otherwise it would change faster at a higher frame
    // rate.
    if(Keyboard->LeftShoulder.EndedDown)  { State->Camera.MovementSpeed -= CAMERA_SPEED_ADJUST_RATE * DeltaTime; }
    if(Keyboard->RightShoulder.EndedDown) { State->Camera.MovementSpeed += CAMERA_SPEED_ADJUST_RATE * DeltaTime; }

    if(State->Camera.MovementSpeed < CAMERA_MIN_SPEED) { State->Camera.MovementSpeed = CAMERA_MIN_SPEED; }
    if(State->Camera.MovementSpeed > CAMERA_MAX_SPEED) { State->Camera.MovementSpeed = CAMERA_MAX_SPEED; }

    // Space: sound on and off.
    if(WasPressed(Keyboard->Back))
    {
        State->SoundOn = !State->SoundOn;
        Memory->DEBUGPlatformLog(Thread, State->SoundOn ? "sound on\n" : "sound off\n");
    }

    // Pressed, not held: once per press.  Until there is something on screen,
    // this is how to see that the camera really moves - the line appears in
    // the debugger's Output window.
    if(WasPressed(Keyboard->Start))     // Enter
    {
        char Line[128];
        snprintf(Line, sizeof(Line), "camera  pos %.2f %.2f %.2f   yaw %.1f  pitch %.1f  speed %.1f\n",
                 State->Camera.Position.X, State->Camera.Position.Y, State->Camera.Position.Z,
                 State->Camera.Yaw * (180.0f / Pi32), State->Camera.Pitch * (180.0f / Pi32),
                 State->Camera.MovementSpeed);
        Memory->DEBUGPlatformLog(Thread, Line);
    }

    // ------------------------------------------------------------------
    // Camera matrices, for the renderer
    // ------------------------------------------------------------------

    // NOTE(yigit): Guard the divide - a minimised window reports height 0,
    // which would make Aspect infinite and fill the matrix with NaNs.
    r32 Aspect = 1.0f;
    if(Input->WindowHeight > 0)
    {
        Aspect = (r32)Input->WindowWidth / (r32)Input->WindowHeight;
    }

    State->View = CameraGetViewMatrix(&State->Camera);
    State->Projection = Mat4Perspective(State->Camera.Zoom, Aspect, 0.1f, 500.0f);

    dx12_renderer *Renderer = GetRenderer(Thread, Memory, Input);
    State->Renderer = Renderer;
    if(!Renderer)
    {
        return;
    }

    // The window changed size since the swapchain was made for it.
    if((Input->WindowWidth != Renderer->Width) ||
       (Input->WindowHeight != Renderer->Height))
    {
        RendererResize(Renderer, Input->WindowWidth, Input->WindowHeight);
    }

    RendererRender(Renderer, Input);
}

extern "C" GAME_GET_SOUND_SAMPLES(GameGetSoundSamples)
{
    game_state *State = (game_state *)Memory->PermanentStorage;

    // NOTE(yigit): Silence has to be WRITTEN.  Leave the buffer alone and
    // the sound card plays whatever happened to be sitting in it - noise,
    // not quiet.
    if(!State->SoundOn)
    {
        WriteSilence(SoundBuffer);
    }
    else if(State->Music.Samples)
    {
        // NOTE(yigit): No resampling.  A file at a different rate from the
        // output's (44100 against 48000, say) plays too fast or too slow,
        // and at the wrong pitch - convert the file to match instead.
        PlaySoundLooping(&State->Music, &State->MusicPlayCursor, SoundBuffer);
    }
    else
    {
        // Middle C, at a tenth of full volume - loud enough to hear, quiet
        // enough not to hurt in headphones.
        WriteTestTone(&State->TonePhase, SoundBuffer, 261.63f, 0.1f);
    }
}

extern "C" GAME_SHUTDOWN(GameShutdown)
{
    dx12_renderer *Renderer = (dx12_renderer *)Memory->RendererStorage;
    if(Renderer && Renderer->IsInitialized)
    {
        RendererShutdown(Renderer);
    }
}
