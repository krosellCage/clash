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

// game.cpp includes this first, and game_imgui.cpp includes it again so it
// makes sense on its own in the editor - the guard keeps it to once.
#if !defined(DX12_RENDERER_CPP)

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

// Dear ImGui's API and its DX12 backend (see game_imgui.cpp).  Level 0: its
// headers are not ours to keep free of -W4 warnings.  The old pre-1.91.5
// API is switched off, so Luna's ImGui_ImplDX12_Init call cannot creep in.
// The math operators (on ImGui's own ImVec2 only) because its .cpp files,
// compiled later in this same unity build, need imgui.h to have them the
// first time it is included.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DEFINE_MATH_OPERATORS
#pragma warning(push, 0)
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#pragma warning(pop)

// Two: one on screen, one being drawn into (double buffering).
#define SWAP_CHAIN_BUFFER_COUNT 2
#define SWAP_CHAIN_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

// The one shader-visible CBV/SRV/UAV heap - everything shaders read goes in
// it: Dear ImGui's font texture now, the game's textures from milestone 6.
#define SRV_HEAP_CAPACITY 1024

// The depth buffer (Luna 4.3.7): 32-bit float depth, no stencil - nothing
// uses stencil yet, and D24_UNORM_S8_UINT has less precision far away.
//
// TODO(yigit): Reversed-Z, before the scene gets big.  A third-person
// camera sees from the hero's feet to the horizon, and normal depth spends
// almost all its precision near the camera - far things z-fight.  Reversed-Z
// stores 1 at the near plane and 0 at the far one: clear to 0.0f, test with
// GREATER, and swap near and far in the projection matrix.
#define DEPTH_FORMAT      DXGI_FORMAT_D32_FLOAT
#define DEPTH_CLEAR_VALUE 1.0f      // the far plane

/*
  A descriptor heap (Luna 4.3.5) - an array of descriptors, one type per
  heap, fixed in size when it is made.  Slot i lives at Start + i *
  DescriptorSize, and the size differs between GPUs and between types, so it
  is asked for once and kept.

  Only CBV_SRV_UAV and SAMPLER heaps are shader visible - shaders read those.
  RTVs and DSVs are only ever handed to commands, by their CPU handle, so
  their heaps have no GPU handles at all.
*/
struct descriptor_heap
{
    ID3D12DescriptorHeap *Heap;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuStart;   // looked up once, not on every use
    D3D12_GPU_DESCRIPTOR_HANDLE GpuStart;   // 0 unless shader visible
    u32 DescriptorSize;
    u32 Count;
};

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

    // The images that take turns on screen (Luna 4.1.4, 4.3.4).  Version 3
    // for GetCurrentBackBufferIndex: with the flip model, the buffer to
    // draw into is not always number 0 - ask every frame.
    IDXGISwapChain3 *SwapChain;

    // Vsync off without tearing artefacts on G-Sync / FreeSync monitors,
    // and the lowest input latency.  Present needs DXGI_PRESENT_ALLOW_TEARING
    // to use it, and ResizeBuffers must be given the same flags as creation.
    b32 SupportsTearing;
    UINT SwapChainFlags;

    // The swap chain's buffers, and a render target view of each in RtvHeap
    // slot i (Luna 4.3.6).  GetBuffer adds a reference, and ResizeBuffers
    // fails while any is held - so these are released before every resize
    // and fetched again after it.
    ID3D12Resource *BackBuffers[SWAP_CHAIN_BUFFER_COUNT];
    descriptor_heap RtvHeap;

    // How far away the nearest surface drawn so far is, for every pixel -
    // the same size as the back buffer, and ours, not the swap chain's, so
    // RendererResize makes a new one.  Its one view is in DsvHeap slot 0.
    ID3D12Resource *DepthBuffer;
    descriptor_heap DsvHeap;

    // The shader-visible heap, and who has which slot.  Slots are handed
    // out from NextUnused upwards; given-back ones go on the free list and
    // are reused first.
    descriptor_heap SrvHeap;
    u32 SrvNextUnused;
    u32 SrvFreeCount;
    u32 SrvFreeList[SRV_HEAP_CAPACITY];

    // Dear ImGui's context - all of its state.  Made by game_imgui.cpp on
    // the first frame after every load of game.dll, destroyed in GameUnload.
    // 0 when it is not running.
    ImGuiContext *ImGui;

    // Things the debug UI shows and edits.
    r32 ClearColor[4];
    char GpuName[128];
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
internal void FlushCommandQueue(dx12_renderer *Renderer);

internal b32
DescriptorHeapInit(descriptor_heap *Heap, ID3D12Device *Device,
                   D3D12_DESCRIPTOR_HEAP_TYPE Type, u32 Count)
{
    b32 ShaderVisible = ((Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) ||
                         (Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));

    D3D12_DESCRIPTOR_HEAP_DESC Desc = {};
    Desc.NumDescriptors = Count;
    Desc.Type = Type;
    Desc.Flags = ShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                               : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Desc.NodeMask = 0;          // only for several GPUs linked together
    if(FAILED(Device->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&Heap->Heap))))
    {
        return(false);
    }

    Heap->CpuStart = Heap->Heap->GetCPUDescriptorHandleForHeapStart();
    if(ShaderVisible)
    {
        Heap->GpuStart = Heap->Heap->GetGPUDescriptorHandleForHeapStart();
    }
    Heap->DescriptorSize = Device->GetDescriptorHandleIncrementSize(Type);
    Heap->Count = Count;

    return(true);
}

// Where the CPU writes slot Index, and what commands like
// ClearRenderTargetView take.
inline D3D12_CPU_DESCRIPTOR_HANDLE
DescriptorCpuHandle(descriptor_heap *Heap, u32 Index)
{
    Assert(Index < Heap->Count);
    D3D12_CPU_DESCRIPTOR_HANDLE Result = Heap->CpuStart;
    Result.ptr += (SIZE_T)Index * Heap->DescriptorSize;
    return(Result);
}

// Where shaders find slot Index.  Shader-visible heaps only.
inline D3D12_GPU_DESCRIPTOR_HANDLE
DescriptorGpuHandle(descriptor_heap *Heap, u32 Index)
{
    Assert(Index < Heap->Count);
    Assert(Heap->GpuStart.ptr);
    D3D12_GPU_DESCRIPTOR_HANDLE Result = Heap->GpuStart;
    Result.ptr += (UINT64)Index * Heap->DescriptorSize;
    return(Result);
}

// A free slot in the shader-visible heap: a given-back one if there is one,
// else the next never-used one.  False when the heap is full.
internal b32
SrvAllocate(dx12_renderer *Renderer, u32 *Index)
{
    b32 Result = true;
    if(Renderer->SrvFreeCount)
    {
        *Index = Renderer->SrvFreeList[--Renderer->SrvFreeCount];
    }
    else if(Renderer->SrvNextUnused < Renderer->SrvHeap.Count)
    {
        *Index = Renderer->SrvNextUnused++;
    }
    else
    {
        Result = false;
    }
    return(Result);
}

internal void
SrvRelease(dx12_renderer *Renderer, u32 Index)
{
    Assert(Index < Renderer->SrvNextUnused);
    Assert(Renderer->SrvFreeCount < SRV_HEAP_CAPACITY);
    Renderer->SrvFreeList[Renderer->SrvFreeCount++] = Index;
}

// Takes the swap chain's buffers and writes a render target view of each
// into the RTV heap.  After creation, and after every ResizeBuffers.
internal b32
AcquireBackBuffers(dx12_renderer *Renderer)
{
    for(u32 BufferIndex = 0; BufferIndex < SWAP_CHAIN_BUFFER_COUNT; ++BufferIndex)
    {
        if(FAILED(Renderer->SwapChain->GetBuffer(BufferIndex,
                                                 IID_PPV_ARGS(&Renderer->BackBuffers[BufferIndex]))))
        {
            return(false);
        }

        // 0 for the view's description: view it exactly as it was made -
        // the swap chain's format, mip level 0.
        Renderer->Device->CreateRenderTargetView(Renderer->BackBuffers[BufferIndex], 0,
                                                 DescriptorCpuHandle(&Renderer->RtvHeap, BufferIndex));
    }

    return(true);
}

// Gives back the references GetBuffer took.  The RTVs are left in the heap -
// they are just data, and AcquireBackBuffers overwrites them.
internal void
ReleaseBackBuffers(dx12_renderer *Renderer)
{
    for(u32 BufferIndex = 0; BufferIndex < SWAP_CHAIN_BUFFER_COUNT; ++BufferIndex)
    {
        ReleaseAndZero(Renderer->BackBuffers[BufferIndex]);
    }
}

// Makes the depth buffer at this size, and writes its view into DsvHeap
// slot 0.  At startup, and after every resize.
internal b32
CreateDepthBuffer(dx12_renderer *Renderer, i32 Width, i32 Height)
{
    // DEFAULT: in VRAM, where only the GPU reads and writes it.
    D3D12_HEAP_PROPERTIES HeapProperties = {};
    HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Alignment = 0;                     // the default, 64 KB
    Desc.Width = (UINT64)Width;             // must match the render target
    Desc.Height = (UINT)Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DEPTH_FORMAT;
    Desc.SampleDesc.Count = 1;              // must match the render target too
    Desc.SampleDesc.Quality = 0;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;  // the GPU's own tiled layout
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // A clear with exactly these values can be faster - and the per-frame
    // clear uses the same DEPTH_CLEAR_VALUE, or the debug layer warns.
    D3D12_CLEAR_VALUE OptimizedClear = {};
    OptimizedClear.Format = DEPTH_FORMAT;
    OptimizedClear.DepthStencil.Depth = DEPTH_CLEAR_VALUE;
    OptimizedClear.DepthStencil.Stencil = 0;

    // "Committed": the memory and the resource made together, in one call.
    // Made straight into DEPTH_WRITE - unlike Luna's COMMON-then-barrier -
    // since depth testing is all it is ever used for.
    if(FAILED(Renderer->Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE,
                                                        &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                        &OptimizedClear,
                                                        IID_PPV_ARGS(&Renderer->DepthBuffer))))
    {
        return(false);
    }

    // 0 for the view's description: the format it was made with, mip 0.
    Renderer->Device->CreateDepthStencilView(Renderer->DepthBuffer, 0,
                                             DescriptorCpuHandle(&Renderer->DsvHeap, 0));

    return(true);
}

// A barrier that moves a whole resource from one state to another.
inline D3D12_RESOURCE_BARRIER
TransitionBarrier(ID3D12Resource *Resource, D3D12_RESOURCE_STATES Before,
                  D3D12_RESOURCE_STATES After)
{
    D3D12_RESOURCE_BARRIER Result = {};
    Result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Result.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    Result.Transition.pResource = Resource;
    Result.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Result.Transition.StateBefore = Before;
    Result.Transition.StateAfter = After;
    return(Result);
}

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

    // Kept for the debug UI.
    snprintf(Renderer->GpuName, sizeof(Renderer->GpuName), "%ls", Desc.Description);

    // The Agility SDK (see the top of win32_clash.cpp).  The exe's export
    // says which runtime version to load; these headers were written for
    // D3D12_SDK_VERSION.  If the two disagree after an SDK upgrade, D3D12
    // quietly falls back to the runtime built into Windows.
    u32 *ExeSDKVersion = (u32 *)(void *)GetProcAddress(GetModuleHandleA(0), "D3D12SDKVersion");
    if(!ExeSDKVersion)
    {
        RendererLog(Thread, Memory, "ERROR: clash.exe does not export D3D12SDKVersion - no Agility SDK.\n");
    }
    else if(*ExeSDKVersion != D3D12_SDK_VERSION)
    {
        snprintf(Line, sizeof(Line),
                 "ERROR: clash.exe asks for D3D12 SDK %u but the headers are %u - "
                 "update D3D12SDKVersion in win32_clash.cpp.\n",
                 *ExeSDKVersion, (u32)D3D12_SDK_VERSION);
        RendererLog(Thread, Memory, Line);
    }

    // Which D3D12Core.dll is really running: build\D3D12\ means the Agility
    // SDK's, C:\Windows\System32 means the one built into Windows.
    char CorePath[MAX_PATH] = {};
    HMODULE Core = GetModuleHandleA("D3D12Core.dll");
    if(Core)
    {
        GetModuleFileNameA(Core, CorePath, sizeof(CorePath));
    }
    snprintf(Line, sizeof(Line), "D3D12 runtime: %s\n", Core ? CorePath : "D3D12Core.dll not loaded");
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

    // ------------------------------------------------------------------
    // The swap chain (Luna 4.3.4).
    // ------------------------------------------------------------------
    BOOL AllowTearing = FALSE;
    if(SUCCEEDED(Renderer->Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &AllowTearing, sizeof(AllowTearing))))
    {
        Renderer->SupportsTearing = AllowTearing;
    }
    Renderer->SwapChainFlags = Renderer->SupportsTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    DXGI_SWAP_CHAIN_DESC1 SwapDesc = {};
    SwapDesc.Width = (UINT)Width;
    SwapDesc.Height = (UINT)Height;

    // 8 bits each of red, green, blue, alpha; UNORM reads 0-255 as 0-1.
    // Flip-model swap chains cannot be _SRGB - for sRGB later, make the
    // render target VIEW R8G8B8A8_UNORM_SRGB instead.
    SwapDesc.Format = SWAP_CHAIN_FORMAT;
    SwapDesc.Stereo = FALSE;

    // No MSAA: flip-model swap chains cannot be multisampled.
    SwapDesc.SampleDesc.Count = 1;
    SwapDesc.SampleDesc.Quality = 0;

    SwapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    SwapDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;

    // A window that is not the buffer's size shows it unstretched.
    SwapDesc.Scaling = DXGI_SCALING_NONE;

    // The flip model - required in D3D12.  DISCARD: the back buffer's old
    // contents are gone after Present, so every frame is drawn in full.
    SwapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    SwapDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    // No ALLOW_MODE_SWITCH, unlike Luna: the platform does borderless
    // fullscreen on F11 and never changes the monitor's resolution.
    SwapDesc.Flags = Renderer->SwapChainFlags;

    // The QUEUE, not the device: in D3D12, Present goes through the queue,
    // after the work submitted before it.  The two zeros: no fullscreen
    // settings (windowed), and any monitor may show it.
    IDXGISwapChain1 *SwapChain1 = 0;
    if(FAILED(Renderer->Factory->CreateSwapChainForHwnd(Renderer->Queue, Window, &SwapDesc,
                                                        0, 0, &SwapChain1)))
    {
        RendererLog(Thread, Memory, "ERROR: CreateSwapChainForHwnd failed.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // Up to version 3, for GetCurrentBackBufferIndex.
    HRESULT SwapChainResult = SwapChain1->QueryInterface(IID_PPV_ARGS(&Renderer->SwapChain));
    SwapChain1->Release();
    if(FAILED(SwapChainResult))
    {
        RendererLog(Thread, Memory, "ERROR: No IDXGISwapChain3 - Windows is too old.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // Left alone, DXGI makes Alt+Enter switch to its own fullscreen - which
    // would fight the platform's F11 one.
    Renderer->Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);

    // ------------------------------------------------------------------
    // The RTV heap - one slot per swap chain buffer - and a render target
    // view of each buffer in it (Luna 4.3.5, 4.3.6).
    // ------------------------------------------------------------------
    if(!DescriptorHeapInit(&Renderer->RtvHeap, Renderer->Device,
                           D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SWAP_CHAIN_BUFFER_COUNT))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the RTV descriptor heap.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    if(!AcquireBackBuffers(Renderer))
    {
        RendererLog(Thread, Memory, "ERROR: Could not get the swap chain's buffers.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // ------------------------------------------------------------------
    // The depth buffer, and a one-slot DSV heap for its view (Luna 4.3.7).
    // ------------------------------------------------------------------
    if(!DescriptorHeapInit(&Renderer->DsvHeap, Renderer->Device,
                           D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the DSV descriptor heap.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    if(!CreateDepthBuffer(Renderer, Width, Height))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the depth buffer.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // ------------------------------------------------------------------
    // The shader-visible heap.  Only one CBV/SRV/UAV heap can be bound at a
    // time, so this one heap holds everything shaders read.
    // ------------------------------------------------------------------
    if(!DescriptorHeapInit(&Renderer->SrvHeap, Renderer->Device,
                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP_CAPACITY))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the shader-visible descriptor heap.\n");
        RendererShutdown(Renderer);
        return(false);
    }
    Renderer->SrvNextUnused = 0;
    Renderer->SrvFreeCount = 0;

    // Hot pink: 255, 105, 180.  The debug UI can change it.
    Renderer->ClearColor[0] = 1.0f;
    Renderer->ClearColor[1] = 0.41f;
    Renderer->ClearColor[2] = 0.71f;
    Renderer->ClearColor[3] = 1.0f;

    return(true);
}

// Width and Height are the window's new INNER size - the part you draw into.
// Never called with 0: the platform does not run the game while minimised.
internal void
RendererResize(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
               i32 Width, i32 Height)
{
    Renderer->Width = Width;
    Renderer->Height = Height;

    if(!Renderer->SwapChain)
    {
        return;
    }

    // The GPU may still be drawing into, or presenting, the old buffers.
    FlushCommandQueue(Renderer);

    // IMPORTANT(yigit): ResizeBuffers fails while ANYTHING still holds one
    // of the old buffers - so ours go first, and come back after.
    ReleaseBackBuffers(Renderer);

    // 0 and DXGI_FORMAT_UNKNOWN keep the buffer count and the format.  The
    // flags must be the ones the swap chain was made with, or tearing
    // support is silently lost.
    HRESULT Result = Renderer->SwapChain->ResizeBuffers(0, (UINT)Width, (UINT)Height,
                                                        DXGI_FORMAT_UNKNOWN,
                                                        Renderer->SwapChainFlags);

    char Line[128];
    if(FAILED(Result))
    {
        snprintf(Line, sizeof(Line), "ERROR: ResizeBuffers to %dx%d failed (0x%08X).\n",
                 Width, Height, (u32)Result);
    }
    else
    {
        snprintf(Line, sizeof(Line), "swap chain resized to %dx%d\n", Width, Height);
    }
    RendererLog(Thread, Memory, Line);

    // New buffers, so new views - written over the old ones, same slots.
    // If the resize failed, these are the old buffers again.
    if(!AcquireBackBuffers(Renderer))
    {
        RendererLog(Thread, Memory, "ERROR: Could not get the swap chain's buffers after a resize.\n");
    }

    // ResizeBuffers only resizes the swap chain's own buffers.  The depth
    // buffer is ours: a new one at the new size, its view in the same slot.
    ReleaseAndZero(Renderer->DepthBuffer);
    if(!CreateDepthBuffer(Renderer, Width, Height))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the depth buffer after a resize.\n");
    }
}

// Input holds this frame's keyboard and mouse (see platform.h), and
// Input->dtForFrame how long the last frame took, for anything that moves.
internal void
RendererRender(dx12_renderer *Renderer, game_input *Input)
{
    if(!Renderer->SwapChain || !Renderer->BackBuffers[0] || !Renderer->DepthBuffer)
    {
        return;
    }

    // Last frame ended with a flush, so the GPU is done with everything in
    // the allocator - safe to reuse its memory.  Then start recording.
    Renderer->CommandAllocator->Reset();
    Renderer->CommandList->Reset(Renderer->CommandAllocator, 0);

    // With the flip model, the buffer to draw into is not always number 0.
    u32 BackBufferIndex = Renderer->SwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource *BackBuffer = Renderer->BackBuffers[BackBufferIndex];

    // A resource is in one state at a time, and the GPU must be told when it
    // changes - it may need to finish reads, flush caches or change how the
    // memory is laid out first.  The buffer comes back from the screen in
    // PRESENT; drawing into it needs RENDER_TARGET.
    D3D12_RESOURCE_BARRIER ToRenderTarget =
        TransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    Renderer->CommandList->ResourceBarrier(1, &ToRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = DescriptorCpuHandle(&Renderer->RtvHeap, BackBufferIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DescriptorCpuHandle(&Renderer->DsvHeap, 0);

    // Hot pink to start with - the debug UI can change it.
    Renderer->CommandList->ClearRenderTargetView(Rtv, Renderer->ClearColor, 0, 0);

    // Every pixel starts at the far plane, so anything drawn is in front.
    // Depth only - there is no stencil in DEPTH_FORMAT.
    Renderer->CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH,
                                                 DEPTH_CLEAR_VALUE, 0, 0, 0);

    // "Output merger": the colour image and the depth buffer the next draws
    // write into.
    Renderer->CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);

    // The viewport (Luna 4.3.8): where on the back buffer to draw - all of
    // it.  It turns the shaders' -1..1 coordinates into pixels, and maps
    // depth 0..1 onto MinDepth..MaxDepth (left as they are).  Reset clears
    // it, so it is set every frame.
    D3D12_VIEWPORT Viewport = {};
    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width = (r32)Renderer->Width;
    Viewport.Height = (r32)Renderer->Height;
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;
    Renderer->CommandList->RSSetViewports(1, &Viewport);

    // The scissor rectangle (Luna 4.3.9): pixels outside it are thrown away.
    // IMPORTANT(yigit): In D3D12 the scissor test is ALWAYS on, and the
    // default rectangle is empty - forget this and nothing is ever drawn.
    // Clears ignore both, which is why the pink worked without them.
    D3D12_RECT Scissor = {0, 0, Renderer->Width, Renderer->Height};
    Renderer->CommandList->RSSetScissorRects(1, &Scissor);

    // Nothing is drawn yet - milestone 4's triangle goes here.

    // Dear ImGui last, so it is on top of everything.  Its font texture is
    // in the shader-visible heap, which has to be bound for it to be read.
    // GetDrawData is 0 unless ImGui::Render ran this frame.
    if(Renderer->ImGui)
    {
        ImDrawData *DrawData = ImGui::GetDrawData();
        if(DrawData && DrawData->Valid)
        {
            ID3D12DescriptorHeap *Heaps[] = {Renderer->SrvHeap.Heap};
            Renderer->CommandList->SetDescriptorHeaps(1, Heaps);
            ImGui_ImplDX12_RenderDrawData(DrawData, Renderer->CommandList);
        }
    }

    // And back to PRESENT, the only state Present accepts.
    D3D12_RESOURCE_BARRIER ToPresent =
        TransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT);
    Renderer->CommandList->ResourceBarrier(1, &ToPresent);

    Renderer->CommandList->Close();
    ID3D12CommandList *Lists[] = {Renderer->CommandList};
    Renderer->Queue->ExecuteCommandLists(1, Lists);

    // Sync interval 1: wait for the monitor's next refresh (vsync).  The
    // platform has no frame limiter of its own, so this is what paces the
    // game.  Vsync off would be Present(0, DXGI_PRESENT_ALLOW_TEARING) when
    // Renderer->SupportsTearing - but only with a frame limiter, or the
    // loop runs at thousands of frames a second.
    Renderer->SwapChain->Present(1, 0);

    // NOTE(yigit): Waits for the GPU every frame, so the CPU and GPU take
    // turns instead of working at the same time.
    FlushCommandQueue(Renderer);
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

    // ImGui uses SrvHeap, so it must be stopped first - GameShutdown and
    // GameUnload see to that.
    Assert(!Renderer->ImGui);
    ReleaseAndZero(Renderer->SrvHeap.Heap);
    ReleaseAndZero(Renderer->DepthBuffer);
    ReleaseAndZero(Renderer->DsvHeap.Heap);
    ReleaseBackBuffers(Renderer);
    ReleaseAndZero(Renderer->RtvHeap.Heap);
    ReleaseAndZero(Renderer->SwapChain);
    ReleaseAndZero(Renderer->CommandList);
    ReleaseAndZero(Renderer->CommandAllocator);
    ReleaseAndZero(Renderer->Queue);
    ReleaseAndZero(Renderer->Fence);
    ReleaseAndZero(Renderer->Device5);
    ReleaseAndZero(Renderer->Device);
    ReleaseAndZero(Renderer->Adapter);
    ReleaseAndZero(Renderer->Factory);
}

#define DX12_RENDERER_CPP
#endif
