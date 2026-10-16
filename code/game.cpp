/*
  NOTE(yigit): The game layer - built as game.dll, which the platform loads
  and reloads whenever it is rebuilt.  Everything it needs from the platform
  arrives through game_memory and game_input (platform.h); it never calls
  Windows itself.

  Four entry points, the only symbols the platform looks up:
    GameUpdateAndRender   once per frame
    GameGetSoundSamples   once per frame, after it
    GameShutdown          once, on the way out
    GameUnload            just before a hot reload unloads this DLL
*/

#include "game.h"

#include <stdio.h>      // snprintf, for the debug log line

#include "dx12_renderer.cpp"

// SimpleMath's constants (Vector3::Up, Matrix::Identity...) are defined in
// its .cpp - and this is a unity build, so it is compiled by including it.
#include "external/DirectXTK12/SimpleMath.cpp"

// Dear ImGui's setup and input.  Its own .cpp files are compiled at the very
// END of this file - see there.
#include "game_imgui.cpp"

// The debug UI's windows - where the widgets are made.
#include "game_debug_ui.cpp"

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
        // 5 units back from the origin, looking at it - down +Z.  Far
        // enough that the whole spinning cube fits: its corners reach 1.73
        // from its centre.
        CameraInitialize(&State->Camera, Vec3(0.0f, 0.0f, -5.0f));

        // Music is optional.  Asked about first, so a project without a
        // music file does not log an error on every start.
        const char *MusicFileName = "data\\music.wav";
        if(Memory->DEBUGPlatformGetFileWriteTime(Thread, MusicFileName))
        {
            State->Music = LoadWAV(Thread, Memory, &State->WorldArena, MusicFileName);
        }

        Memory->IsInitialized = true;
    }

    State->TimeSeconds += (r64)Input->dtForFrame;

    // The renderer first, so Dear ImGui can start its frame before the
    // camera reads the mouse - the camera has to know whether ImGui wants it.
    dx12_renderer *Renderer = GetRenderer(Thread, Memory, Input);
    State->Renderer = Renderer;
    if(Renderer)
    {
        ImGuiBeginFrame(Renderer, Input);
    }
    b32 UIHasMouse = ImGuiWantsMouse(Renderer);

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

    // Not while the wheel is scrolling an ImGui window.
    if(!UIHasMouse)
    {
        CameraProcessZoom(&State->Camera, (r32)Input->MouseZ);
    }

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

    if(!Renderer)
    {
        return;
    }

    // The window changed size since the swapchain was made for it.
    if((Input->WindowWidth != Renderer->Width) ||
       (Input->WindowHeight != Renderer->Height))
    {
        RendererResize(Renderer, Thread, Memory, Input->WindowWidth, Input->WindowHeight);
    }

    // The debug UI.  ImGui::Render draws nothing itself - it builds the
    // triangles, and RendererRender records them into the frame.
    if(Renderer->ImGui)
    {
        DrawDebugUI(State, Renderer);
        ImGui::Render();
    }

    // Milestone 5: the cube, with DirectXMath - Luna's maths, as in his Box
    // demo.  XMMATRIX is for DOING maths (SSE registers, locals only);
    // XMFLOAT4X4 is for storing a result and passing it on.
    //
    // The cube spins - about Y, and half as fast about X so every face comes
    // round.  World * View * Projection, read left to right: place the cube,
    // then look at it, then project it.
    r32 Time = (r32)State->TimeSeconds;
    XMMATRIX World = XMMatrixRotationY(Time) * XMMatrixRotationX(0.5f*Time);

    // The camera's view, from its position, a point one unit ahead, and its
    // up.  XMLoadVec3 (game_directx_math.h) turns our vec3s into XMVECTORs:
    // W = 1 for the two points, 0 for the up direction.
    camera *Camera = &State->Camera;
    XMMATRIX View = XMMatrixLookAtLH(XMLoadVec3(Camera->Position, 1.0f),
                                     XMLoadVec3(Camera->Position + Camera->Front, 1.0f),
                                     XMLoadVec3(Camera->Up, 0.0f));
    XMMATRIX Projection = XMMatrixPerspectiveFovLH(Camera->Zoom, Aspect, 0.1f, 500.0f);

    XMFLOAT4X4 CubeWorld;
    XMFLOAT4X4 ViewProj;
    XMStoreFloat4x4(&CubeWorld, World);
    XMStoreFloat4x4(&ViewProj, View * Projection);

    RendererRender(Renderer, Thread, Memory, Input, &CubeWorld, &ViewProj);
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
        // ImGui first - it uses the renderer's device, queue and heap.
        ImGuiStop(Renderer);
        RendererShutdown(Renderer);
    }
}

// Just before a hot reload.  The renderer carries on - its objects live in
// Windows' DLLs - but Dear ImGui lives partly in ours.  See game_imgui.cpp.
extern "C" GAME_UNLOAD(GameUnload)
{
    dx12_renderer *Renderer = (dx12_renderer *)Memory->RendererStorage;
    if(Renderer && Renderer->IsInitialized)
    {
        ImGuiStop(Renderer);
    }
}

/*
  Dear ImGui 1.92.9b and its DX12 backend, compiled here as part of the unity
  build - at warning level 0, since its warnings are not ours to fix.
  imgui_demo.cpp is ImGui::ShowDemoWindow: every widget, with the code that
  makes it.

  NOTE(yigit): LAST in the file, on purpose.  These files #define things for
  their own use (snprintf among them) that would otherwise leak into the
  game's code after them.  Everything above only needs imgui.h.
*/
#pragma warning(push, 0)
#include "external/imgui/imgui.cpp"
#include "external/imgui/imgui_draw.cpp"
#include "external/imgui/imgui_tables.cpp"
#include "external/imgui/imgui_widgets.cpp"
#include "external/imgui/imgui_demo.cpp"
#include "external/imgui/backends/imgui_impl_dx12.cpp"
#pragma warning(pop)
