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

#include <stdio.h>      // snprintf, for the log lines

// Everything the renderer keeps from one frame to the next.  Add your
// ID3D12Device *, swapchain, command queue and so on here.
struct dx12_renderer
{
    b32 IsInitialized;

    // The size the swapchain was last made for.  RendererRender compares
    // this against the window's size each frame to spot a resize.
    i32 Width;
    i32 Height;

    // DXGI lists the GPUs and makes the swapchain - so the factory is kept,
    // not thrown away after the device is made.
    IDXGIFactory6 *Factory;
    IDXGIAdapter1 *Adapter;     // the GPU the device was made on
    ID3D12Device *Device;

    // What the GPU can do beyond the 12_0 minimum we ask for.  Device5 is
    // the same device through a newer interface - the one with the ray
    // tracing calls - and is only fetched when ray tracing is there to use.
    D3D_FEATURE_LEVEL FeatureLevel;
    b32 SupportsRaytracing;
    b32 SupportsMeshShaders;
    ID3D12Device5 *Device5;

    // The GPU's to-do list, and what the CPU records work into.  The CPU
    // records commands into the list - their memory comes from the
    // allocator - and hands the closed list to the queue.  The GPU works
    // through the queue in its own time.
    ID3D12CommandQueue *Queue;
    ID3D12CommandAllocator *CommandAllocator;
    ID3D12GraphicsCommandList *CommandList;

    ID3D12Fence *Fence;
    u64 FenceValue;        // the last value we asked the queue to Signal
    HANDLE FenceEvent;     // made once, reused by every wait
};

// Every DirectX object is a COM object: Release it once, when done.  Zeroing
// the pointer after means a second Release - or a use after it - is a clean
// null crash in the debugger, not a mystery.
#define ReleaseAndZero(Pointer) do { if(Pointer) { (Pointer)->Release(); (Pointer) = 0; } } while(0)

internal void
RendererLog(thread_context *Thread, game_memory *Memory, const char *Message)
{
#if CLASH_DEBUG
    Memory->DEBUGPlatformLog(Thread, Message);
#endif
}

// RendererInitialize calls it to clean up when it fails halfway.
internal void RendererShutdown(dx12_renderer *Renderer);

// Window is the HWND the swapchain draws into.  Returns false if DirectX 12
// could not be started.
internal b32
RendererInitialize(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
                   HWND Window, i32 Width, i32 Height)
{
    Renderer->Width = Width;
    Renderer->Height = Height;

    // ------------------------------------------------------------------
    // The debug layer.  D3D12 checks every call and prints what is wrong
    // to the debugger's Output window.  It has to be switched on BEFORE
    // the device is made - a device made earlier never gets it.
    // ------------------------------------------------------------------
    UINT FactoryFlags = 0;
#if CLASH_DEBUG
    ID3D12Debug *Debug = 0;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug))))
    {
        Debug->EnableDebugLayer();

        // Even stricter: checks on the GPU itself, as the shaders run.
        // Catches far more, but makes everything much slower - switch it
        // on when hunting a bug the normal layer does not see.
        // ID3D12Debug1 *Debug1 = 0;
        // if(SUCCEEDED(Debug->QueryInterface(IID_PPV_ARGS(&Debug1))))
        // {
        //     Debug1->SetEnableGPUBasedValidation(TRUE);
        //     Debug1->Release();
        // }

        Debug->Release();
    }

    // DXGI's own errors - swapchain mistakes, mostly - to the Output window
    // as well.
    FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    if(FAILED(CreateDXGIFactory2(FactoryFlags, IID_PPV_ARGS(&Renderer->Factory))))
    {
        RendererLog(Thread, Memory, "ERROR: CreateDXGIFactory2 failed.\n");
        return(false);
    }

    // ------------------------------------------------------------------
    // The device, on the fastest GPU that can run DirectX 12.
    //
    // EnumAdapterByGpuPreference lists the high-performance GPU first -
    // the discrete one on a laptop.  Plain EnumAdapters lists the GPU the
    // monitor is plugged into first, which is often the integrated one.
    //
    // 12_0 is the minimum: any DirectX 12 GPU from the last ten years.
    // Ray tracing and mesh shaders are checked for separately below, so
    // the game still starts on a GPU without them.
    // ------------------------------------------------------------------
    for(UINT AdapterIndex = 0;; ++AdapterIndex)
    {
        IDXGIAdapter1 *Adapter = 0;
        if(FAILED(Renderer->Factory->EnumAdapterByGpuPreference(AdapterIndex,
                                                                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                IID_PPV_ARGS(&Adapter))))
        {
            // DXGI_ERROR_NOT_FOUND: no more GPUs to try.
            break;
        }

        DXGI_ADAPTER_DESC1 Desc;
        Adapter->GetDesc1(&Desc);

        // The "Microsoft Basic Render Driver" - DirectX 12 on the CPU.
        // Far too slow to play on; skipped.
        b32 IsSoftware = (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);

        if(!IsSoftware &&
           SUCCEEDED(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_12_0,
                                       IID_PPV_ARGS(&Renderer->Device))))
        {
            Renderer->Adapter = Adapter;
            break;
        }

        Adapter->Release();
    }

    if(!Renderer->Device)
    {
        RendererLog(Thread, Memory, "ERROR: No GPU here can run DirectX 12 (feature level 12_0).\n");
        RendererShutdown(Renderer);
        return(false);
    }

#if CLASH_DEBUG
    // Stop in the debugger ON the call that is wrong, instead of finding
    // the message in the Output window later.  Only with a debugger
    // attached - without one, the break would just crash the game.
    if(IsDebuggerPresent())
    {
        ID3D12InfoQueue *InfoQueue = 0;
        if(SUCCEEDED(Renderer->Device->QueryInterface(IID_PPV_ARGS(&InfoQueue))))
        {
            InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            InfoQueue->Release();
        }
    }
#endif

    // ------------------------------------------------------------------
    // What this GPU can do.
    // ------------------------------------------------------------------
    D3D_FEATURE_LEVEL LevelsToCheck[] =
    {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS FeatureLevels = {};
    FeatureLevels.NumFeatureLevels = (UINT)ArrayCount(LevelsToCheck);
    FeatureLevels.pFeatureLevelsRequested = LevelsToCheck;
    Renderer->FeatureLevel = D3D_FEATURE_LEVEL_12_0;
    if(SUCCEEDED(Renderer->Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                                                       &FeatureLevels, sizeof(FeatureLevels))))
    {
        Renderer->FeatureLevel = FeatureLevels.MaxSupportedFeatureLevel;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5 = {};
    if(SUCCEEDED(Renderer->Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                       &Options5, sizeof(Options5))))
    {
        Renderer->SupportsRaytracing = (Options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 Options7 = {};
    if(SUCCEEDED(Renderer->Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                       &Options7, sizeof(Options7))))
    {
        Renderer->SupportsMeshShaders = (Options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
    }

    if(Renderer->SupportsRaytracing)
    {
        Renderer->Device->QueryInterface(IID_PPV_ARGS(&Renderer->Device5));
    }

    // Which GPU we got - milestone 1's proof that it worked.
    DXGI_ADAPTER_DESC1 Desc;
    Renderer->Adapter->GetDesc1(&Desc);

    char Line[512];
    snprintf(Line, sizeof(Line),
             "GPU: %ls, %llu MB VRAM, feature level %d_%d, ray tracing %s, mesh shaders %s\n",
             Desc.Description,
             (unsigned long long)(Desc.DedicatedVideoMemory / (1024*1024)),
             (Renderer->FeatureLevel >> 12) & 0xF, (Renderer->FeatureLevel >> 8) & 0xF,
             Renderer->SupportsRaytracing ? "yes" : "no",
             Renderer->SupportsMeshShaders ? "yes" : "no");
    RendererLog(Thread, Memory, Line);

    if(FAILED(Renderer->Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(&Renderer->Fence))))
    {
        RendererLog(Thread, Memory, "ERROR: CreateFence failed.\n");
        RendererShutdown(Renderer);
        return(false);
    }
    Renderer->FenceEvent = CreateEventExA(0, 0, 0, EVENT_ALL_ACCESS);
    if(!Renderer->FenceEvent)
    {
        RendererLog(Thread, Memory, "ERROR: CreateEventExA failed.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // ------------------------------------------------------------------
    // Command queue, allocator and list (Luna 4.3.3).
    //
    // DIRECT is the queue type that can draw; it takes copy and compute
    // work too.  Later, a separate COPY queue can stream assets and a
    // COMPUTE queue can run work alongside drawing ("async compute") -
    // one DIRECT queue is all we need now.
    // ------------------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
    QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if(FAILED(Renderer->Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(&Renderer->Queue))))
    {
        RendererLog(Thread, Memory, "ERROR: CreateCommandQueue failed.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // The memory recorded commands live in.  It must not be Reset until
    // the GPU has finished every command in it - FlushCommandQueue first.
    if(FAILED(Renderer->Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&Renderer->CommandAllocator))))
    {
        RendererLog(Thread, Memory, "ERROR: CreateCommandAllocator failed.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // 0: the first (only) GPU.  No pipeline state yet - nothing is drawn
    // with one until milestone 4, and it can be set while recording.
    if(FAILED(Renderer->Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  Renderer->CommandAllocator, 0,
                                                  IID_PPV_ARGS(&Renderer->CommandList))))
    {
        RendererLog(Thread, Memory, "ERROR: CreateCommandList failed.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // A new list starts open, ready to record.  Every frame begins with
    // Reset, and Reset needs a closed list - so close it now.
    Renderer->CommandList->Close();

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

// Waits until the GPU has finished everything queued so far.
internal void
FlushCommandQueue(dx12_renderer *Renderer)
{
    // Nothing to wait for - and nothing to Signal with - when
    // RendererInitialize failed before these were made.
    if(!Renderer->Queue || !Renderer->Fence)
    {
        return;
    }

    ++Renderer->FenceValue;
    Renderer->Queue->Signal(Renderer->Fence, Renderer->FenceValue);

    if(Renderer->Fence->GetCompletedValue() < Renderer->FenceValue)
    {
        Renderer->Fence->SetEventOnCompletion(Renderer->FenceValue, Renderer->FenceEvent);
        WaitForSingleObject(Renderer->FenceEvent, INFINITE);
    }
}

// Wait for the GPU to finish before anything is released - it may still be
// using the resources the last frame submitted.
internal void
RendererShutdown(dx12_renderer *Renderer)
{
    FlushCommandQueue(Renderer);

    // Checked: a failed RendererInitialize may not have made it, and under
    // a debugger CloseHandle on a bad handle raises an exception.
    if(Renderer->FenceEvent)
    {
        CloseHandle(Renderer->FenceEvent);
        Renderer->FenceEvent = 0;
    }

    //NOTE(yigit): Order matters. Newest first.

    ReleaseAndZero(Renderer->CommandList);
    ReleaseAndZero(Renderer->CommandAllocator);
    ReleaseAndZero(Renderer->Queue);
    ReleaseAndZero(Renderer->Fence);
    ReleaseAndZero(Renderer->Device5);
    ReleaseAndZero(Renderer->Device);
    ReleaseAndZero(Renderer->Adapter);
    ReleaseAndZero(Renderer->Factory);
}
