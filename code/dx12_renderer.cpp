/*
  NOTE(yigit): Everything DirectX 12 lives here.  This file is YOURS - it
  starts empty on purpose.

  It is part of the game DLL, and game.cpp calls it:

    RendererInitialize  once, on the first frame
    RendererResize      whenever the window's inner size changes
    RendererRender      once per frame
    RendererShutdown    once, on the way out

  IMPORTANT(yigit): NO GLOBAL VARIABLES IN THIS FILE.  The DLL is unloaded
  and reloaded every time it is rebuilt, and its globals go back to zero
  when that happens - while the device, the swapchain and the GPU carry on.
  Everything the renderer keeps goes in struct dx12_renderer, which lives in
  game_memory::RendererStorage and survives a reload.  (The DirectX objects
  themselves survive too: their code is in Windows' DLLs, not in ours.)

  Milestones - finish each one, and see it work, before the next:

    1. Debug layer on (only when CLASH_DEBUG is 1), then a device.
       Log which GPU you got.
    2. Command queue, swapchain for the window, command allocator and list,
       a fence.  CLEAR THE WINDOW TO A COLOUR every frame.
    3. RendererResize: the swapchain's buffers have to be resized.
    4. Root signature, pipeline state object, a vertex buffer, a shader in
       data\ - A TRIANGLE.
    5. A spinning cube: index buffer, constant buffer, depth buffer.
    6. A textured cube: descriptor heaps, a texture, the upload path.
*/

// b32, internal, game_input, game_memory...  game.cpp has already
// included this before it includes us, so in the build it does nothing - the
// include guard skips it.  It is here so this file makes sense on its own,
// to a reader and to the editor.
#include "game.h"

// NOTE(yigit): The one file in the game layer that includes windows.h - the
// swapchain needs the HWND, and DirectX 12 is Windows-only anyway.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// DirectX 12 itself, and DXGI - the part of Windows that owns GPUs,
// swapchains and presenting.  d3dcompiler turns HLSL into GPU code at run
// time.  All three .libs are already linked for game.dll in build.bat.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

// Everything the renderer keeps from one frame to the next.  Add your
// ID3D12Device *, swapchain, command queue and so on here.
struct dx12_renderer
{
    b32 IsInitialized;

    // The size the swapchain was last made for.  RendererRender compares
    // this against the window's size each frame to spot a resize.
    i32 Width;
    i32 Height;
};

// Window is the HWND the swapchain draws into.  Returns false if DirectX 12
// could not be started.
internal b32
RendererInitialize(dx12_renderer *Renderer, game_memory *Memory, HWND Window,
                   i32 Width, i32 Height)
{
    Renderer->Width = Width;
    Renderer->Height = Height;

    return(true);
}

// Width and Height are the window's new INNER size - the part you draw into.
// Never called with 0: the platform does not run the game while minimised.
internal void
RendererResize(dx12_renderer *Renderer, i32 Width, i32 Height)
{
    Renderer->Width = Width;
    Renderer->Height = Height;
}

// Input holds this frame's keyboard and mouse (see platform.h), and
// Input->dtForFrame how long the last frame took, for anything that moves.
internal void
RendererRender(dx12_renderer *Renderer, game_input *Input)
{
}

// Wait for the GPU to finish before anything is released - it may still be
// using the resources the last frame submitted.
internal void
RendererShutdown(dx12_renderer *Renderer)
{
}
