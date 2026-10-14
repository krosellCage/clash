/*
  NOTE(yigit): Dear ImGui (Luna 4.5.6) - the debug UI: windows, sliders,
  checkboxes, read-outs.

  It is immediate mode: there are no widget objects.  Every frame the game
  just CALLS ImGui::Checkbox("Sound", &On), which draws the box, handles a
  click on it, flips the variable, and is gone again.  The game's own
  variables are the only state.

  Where clash differs from Luna:

  - No imgui_impl_win32.  The platform knows nothing about ImGui; its input
    comes from game_input, in ImGuiBeginFrame below.

  - Hot reload.  ImGui keeps its current context, its allocator and its
    font loader in game.dll's globals, and its memory comes from game.dll's
    own C runtime heap (-MTd) - all of it gone when the DLL is unloaded.  So
    GameUnload stops it while the old DLL is still loaded, and the new DLL
    starts it again on its first frame.  Window positions and sizes survive
    in build\imgui.ini.

  - The renderer draws it, last, in RendererRender.
*/

// dx12_renderer, the SRV heap, imgui.h and the DX12 backend's header.
// game.cpp has already included it before us, so in the build the include
// guard skips it - it is here so this file makes sense on its own, to a
// reader and to the editor.
#include "dx12_renderer.cpp"

// The DX12 backend asks for a descriptor in the shader-visible heap for each
// texture it makes - the font atlas, which can be remade as fonts grow - and
// gives them back.  UserData is the renderer.
internal void
ImGuiSrvAllocate(ImGui_ImplDX12_InitInfo *Info, D3D12_CPU_DESCRIPTOR_HANDLE *OutCpu,
                 D3D12_GPU_DESCRIPTOR_HANDLE *OutGpu)
{
    dx12_renderer *Renderer = (dx12_renderer *)Info->UserData;

    u32 Index = 0;
    b32 Allocated = SrvAllocate(Renderer, &Index);
    Assert(Allocated);

    *OutCpu = DescriptorCpuHandle(&Renderer->SrvHeap, Index);
    *OutGpu = DescriptorGpuHandle(&Renderer->SrvHeap, Index);
}

internal void
ImGuiSrvFree(ImGui_ImplDX12_InitInfo *Info, D3D12_CPU_DESCRIPTOR_HANDLE Cpu,
             D3D12_GPU_DESCRIPTOR_HANDLE Gpu)
{
    dx12_renderer *Renderer = (dx12_renderer *)Info->UserData;

    u32 Index = (u32)((Cpu.ptr - Renderer->SrvHeap.CpuStart.ptr) / Renderer->SrvHeap.DescriptorSize);
    SrvRelease(Renderer, Index);
}

internal b32
ImGuiStart(dx12_renderer *Renderer)
{
    IMGUI_CHECKVERSION();
    Renderer->ImGui = ImGui::CreateContext();
    ImGui::SetCurrentContext(Renderer->ImGui);

    ImGuiIO &IO = ImGui::GetIO();
    IO.IniFilename = "build\\imgui.ini";    // the working directory is the project's
    IO.BackendPlatformName = "clash";
    ImGui::StyleColorsDark();

    ImGui_ImplDX12_InitInfo Info;           // its constructor zeroes it
    Info.Device = Renderer->Device;
    Info.CommandQueue = Renderer->Queue;    // for uploading its textures
    Info.NumFramesInFlight = SWAP_CHAIN_BUFFER_COUNT;
    Info.RTVFormat = SWAP_CHAIN_FORMAT;

    // The depth buffer is bound while ImGui draws, so its pipeline has to
    // say the same format or the debug layer objects.
    Info.DSVFormat = DEPTH_FORMAT;

    Info.UserData = Renderer;
    Info.SrvDescriptorHeap = Renderer->SrvHeap.Heap;
    Info.SrvDescriptorAllocFn = ImGuiSrvAllocate;
    Info.SrvDescriptorFreeFn = ImGuiSrvFree;

    if(!ImGui_ImplDX12_Init(&Info))
    {
        ImGui::DestroyContext(Renderer->ImGui);
        Renderer->ImGui = 0;
        return(false);
    }

    return(true);
}

// While the DLL that started it is still loaded - see the top of the file.
internal void
ImGuiStop(dx12_renderer *Renderer)
{
    if(Renderer->ImGui)
    {
        // Its vertex buffers and font texture may still be in use.
        FlushCommandQueue(Renderer);

        ImGui::SetCurrentContext(Renderer->ImGui);
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext(Renderer->ImGui);     // writes imgui.ini
        Renderer->ImGui = 0;
    }
}

// Starts ImGui if this DLL has not yet, hands it this frame's input, and
// begins its frame.  Everything between this and ImGui::Render() can make
// windows.
internal void
ImGuiBeginFrame(dx12_renderer *Renderer, game_input *Input)
{
    if(!Renderer->ImGui && !ImGuiStart(Renderer))
    {
        return;
    }

    // A DLL global, so reloading resets it - set every frame.
    ImGui::SetCurrentContext(Renderer->ImGui);

    ImGuiIO &IO = ImGui::GetIO();
    IO.DisplaySize = ImVec2((r32)Input->WindowWidth, (r32)Input->WindowHeight);
    IO.DeltaTime = (Input->dtForFrame > 0.0f) ? Input->dtForFrame : (1.0f / 60.0f);

    // Camera mode (Tab): the cursor is hidden and pinned to the centre of
    // the window.  ImGui must not see a mouse at all then, or the middle of
    // the screen would hover - and click - whatever window is there.
    b32 HasMouse = Input->CursorFree;
    if(HasMouse)
    {
        IO.AddMousePosEvent((r32)Input->MouseX, (r32)Input->MouseY);
    }
    else
    {
        IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    // game_input: left, middle, right, X1, X2.  ImGui: left, right, middle,
    // X1, X2.  ImGui ignores repeats of the state it already has.
    int ImGuiButton[MOUSE_BUTTON_COUNT] = {0, 2, 1, 3, 4};
    for(u32 ButtonIndex = 0; ButtonIndex < MOUSE_BUTTON_COUNT; ++ButtonIndex)
    {
        b32 IsDown = HasMouse && Input->MouseButtons[ButtonIndex].EndedDown;
        IO.AddMouseButtonEvent(ImGuiButton[ButtonIndex], IsDown != 0);
    }

    if(HasMouse && Input->MouseZ)
    {
        IO.AddMouseWheelEvent(0.0f, (r32)Input->MouseZ);
    }

    // TODO(yigit): Keyboard and typed characters, for text boxes - the
    // platform does not pass on WM_CHAR yet.

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
}

// True when the mouse is over an ImGui window this frame - the game should
// leave the mouse alone then (no zooming the camera while scrolling a list).
internal b32
ImGuiWantsMouse(dx12_renderer *Renderer)
{
    b32 Result = (Renderer && Renderer->ImGui && ImGui::GetIO().WantCaptureMouse);
    return(Result);
}
