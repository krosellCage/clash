/*
  NOTE(yigit): Everything DirectX 12 lives here.
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
    6. Frame resources (Luna 7): the CPU builds the next frame while the
       GPU draws this one - no flush every frame - and many objects, each
       with its own constants.
    7. Light (Luna 8): normals; directional, point and spot lights;
       Blinn-Phong.
    8. A textured cube (Luna 9): descriptor heaps, a texture, the upload
       path, samplers, mipmaps.  PNG/JPG through stb_image.
    9. A model from a file: glTF through cgltf - DamagedHelmet first, then
       SPONZA, lit with milestone 7's lights.
   10. PBR: Blinn-Phong swapped for GGX (learnopengl's PBR chapters,
       Karis's "Real Shading in UE4"), metal/roughness textures.  Checked
       against MetalRoughSpheres' reference image.
   11. Normal maps and sun shadows (Luna 19, 20).
   12. HDR render target and tone mapping, SSAO, a sky (Luna 21, 18).
   13. SUN TEMPLE, LIT - the showcase.  FBX through ufbx (or a converter
       to clash's own asset format), the sun, the fires and the emissive
       surfaces in HDR.
*/

// Include guard: game_imgui.cpp includes this file too, so it must compile once.
#if !defined(DX12_RENDERER_CPP)

// Brings in b32, internal, game_memory etc., so the file also stands alone in the editor.
#include "game.h"

// NOTE(yigit): The one file in the game layer that includes windows.h - the
// swapchain needs the HWND, and DirectX 12 is Windows-only anyway.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// D3D12 for the device and commands, DXGI for GPUs and the swap chain, DXC to compile shaders.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include "dxcapi.h"

#include <stdio.h>      // snprintf: formats log lines
#include <string.h>     // memcpy: copies data into mapped GPU memory

// Dear ImGui and its DX12 backend.  Obsolete API off so old-style calls fail to
// compile; math operators on because imgui's .cpp files need them from the first
// include; warnings off because its headers aren't ours to fix.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DEFINE_MATH_OPERATORS
#pragma warning(push, 0)
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#pragma warning(pop)

// Double buffering: one image on screen while the other is drawn.
#define SWAP_CHAIN_BUFFER_COUNT 2
// 8 bits per channel, read by shaders as 0..1: the standard display format.
#define SWAP_CHAIN_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

// Slots in the one shader-visible heap; only one can be bound, so it holds everything shaders read.
#define SRV_HEAP_CAPACITY 1024

// 32-bit float depth (precise far away, ready for reversed-Z) plus 8 bits of
// stencil for masks like selection outlines.
//
// TODO(yigit): Reversed-Z, before the scene gets big.  A third-person
// camera sees from the hero's feet to the horizon, and normal depth spends
// almost all its precision near the camera - far things z-fight.  Reversed-Z
// stores 1 at the near plane and 0 at the far one: clear to 0.0f, test with
// GREATER, and swap near and far in the projection matrix.
#define DEPTH_FORMAT        DXGI_FORMAT_D32_FLOAT_S8X24_UINT
#define DEPTH_CLEAR_VALUE   1.0f    // far plane, so anything drawn is nearer
#define STENCIL_CLEAR_VALUE 0       // no pixel marked

// Constant buffers must start on 256-byte boundaries, so each one gets a 256-byte slot.
#define CONSTANT_BUFFER_SLOT_SIZE D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT
#define BOX_COUNT 9

// Positions only: vertex buffer slot 0.
struct VPosData
{
    XMFLOAT3 Pos;
};
// Colours only: vertex buffer slot 1.
struct VColorData
{
    XMCOLOR Color;
};

// Byte-for-byte copies of cube.hlsl's cbuffers b0 and b1, filled with transposed
// matrices because HLSL reads them column by column.
struct object_constants
{
    XMFLOAT4X4 World;
};

struct pass_constants
{
    XMFLOAT4X4 ViewProj;
    XMFLOAT4 PulseColor;
    r32 gTime;
};

// Fail the build if a constants struct no longer fits its 256-byte slot.
static_assert(sizeof(object_constants) <= CONSTANT_BUFFER_SLOT_SIZE, "object_constants outgrew its slot");
static_assert(sizeof(pass_constants) <= CONSTANT_BUFFER_SLOT_SIZE, "pass_constants outgrew its slot");

// A descriptor heap plus what's needed to address slot i (Start + i * DescriptorSize).
// The size varies by GPU and type, so it's queried once and stored.
struct descriptor_heap
{
    ID3D12DescriptorHeap *Heap;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuStart;   // stored so it isn't queried on every use
    D3D12_GPU_DESCRIPTOR_HANDLE GpuStart;   // 0 for heaps shaders can't see (RTV, DSV)
    u32 DescriptorSize;
    u32 Count;
};

// One part of a mesh: the three numbers DrawIndexedInstanced needs, so several
// objects can share one vertex and index buffer.
struct submesh
{
    u32 IndexCount;
    u32 StartIndex;     // first index to read, counted in indices
    i32 BaseVertex;     // added to every index, so each object's indices can start at 0
};

#define MESH_MAX_SUBMESHES 16

// Geometry on the GPU: the buffers, the views that describe them, and the parts to draw.
// Fixed-size array so a mesh needs no allocation.
struct mesh
{
    ID3D12Resource *VertexBuffers[2];       // DEFAULT heap (VRAM): fastest for the GPU to read
    ID3D12Resource *IndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexViews[2];    // built once, since the buffers never move
    D3D12_INDEX_BUFFER_VIEW IndexView;

    u32 SubmeshCount;
    submesh Submeshes[MESH_MAX_SUBMESHES];
};

// All renderer state.  Lives in game memory so it survives game.dll hot reloads.
struct dx12_renderer
{
    b32 IsInitialized;

    // Size the swap chain was last built for, to detect a resize.
    i32 Width;
    i32 Height;

    // Factory kept: it's needed again for the swap chain and tearing checks.
    IDXGIFactory6 *Factory;
    IDXGIAdapter1 *Adapter;     // the GPU the device runs on
    ID3D12Device *Device;

    // Optional GPU features, checked once so the game can still run without them.
    // Device5 is the device's ray-tracing interface, fetched only if supported.
    D3D_FEATURE_LEVEL FeatureLevel;
    b32 SupportsRaytracing;
    b32 SupportsMeshShaders;
    ID3D12Device5 *Device5;

    // CPU records commands into the list (memory from the allocator); the queue
    // runs closed lists on the GPU.
    ID3D12CommandQueue *Queue;
    ID3D12CommandAllocator *CommandAllocator;
    ID3D12GraphicsCommandList *CommandList;

    // Lets the CPU wait until the GPU reaches a point in the queue.
    ID3D12Fence *Fence;
    u64 FenceValue;        // last value the queue was told to signal
    HANDLE FenceEvent;     // created once, reused for every wait

    // Version 3 for GetCurrentBackBufferIndex: with flip-model the next buffer isn't always 0.
    IDXGISwapChain3 *SwapChain;

    // Tearing support allows vsync-off on variable-refresh monitors.  The flags
    // are stored because ResizeBuffers must get the same ones as creation.
    b32 SupportsTearing;
    UINT SwapChainFlags;

    // Swap chain images and their render target views (slot i = buffer i).
    // Released before every resize, because ResizeBuffers fails while they're held.
    ID3D12Resource *BackBuffers[SWAP_CHAIN_BUFFER_COUNT];
    descriptor_heap RtvHeap;

    // Per-pixel depth, so nearer surfaces hide farther ones.  Ours, not the
    // swap chain's, so it's rebuilt on resize.  View in DsvHeap slot 0.
    ID3D12Resource *DepthBuffer;
    descriptor_heap DsvHeap;

    // Shader-visible heap with a free list, so slots can be given back and reused.
    descriptor_heap SrvHeap;
    u32 SrvNextUnused;
    u32 SrvFreeCount;
    u32 SrvFreeList[SRV_HEAP_CAPACITY];

    // DXC, loaded at runtime to compile HLSL at startup.
    HMODULE DxcModule;
    IDxcUtils *DxcUtils;            // loads shader files
    IDxcCompiler3 *DxcCompiler;     // compiles them

    // The cube: how its shaders take input, all its pipeline settings, and its geometry.
    ID3D12RootSignature *CubeRootSignature;
    ID3D12PipelineState *CubePipeline;
    mesh CubeMesh;

    // Upload-heap buffer for per-frame matrices, mapped permanently so the CPU
    // can write it every frame without Map/Unmap.
    ID3D12Resource *ConstantBuffer;
    u8 *ConstantBufferMapped;       // CPU pointer into it

    // ImGui's state; 0 when not running.  Created after each game.dll load,
    // destroyed in GameUnload.
    ImGuiContext *ImGui;

    // Values the debug UI shows and edits.
    r32 ClearColor[4];
    char GpuName[128];
    u64 DedicatedVideoMemory;   // VRAM on the card, in bytes
    IDXGIAdapter3 *Adapter3;    // same GPU, for QueryVideoMemoryInfo; may be 0
};

// Releases a COM object and zeroes the pointer, so a second release or later
// use is a clean null crash instead of a hidden bug.
#define ReleaseAndZero(Pointer) do { if(Pointer) { (Pointer)->Release(); (Pointer) = 0; } } while(0)

// Sends a line to the platform log; compiled out of release builds.
internal void
RendererLog(thread_context *Thread, game_memory *Memory, const char *Message)
{
#if CLASH_DEBUG
    Memory->DEBUGPlatformLog(Thread, Message);
#endif
}

/*
  DX12_FAILED(Call): runs Call and returns true if its HRESULT failed, after
  logging the file, line, call text and Windows' description of the error.
  A macro so __FILE__, __LINE__ and #Call come from the caller.
  IMPORTANT(yigit): It logs through Thread and Memory - so it can only be
  used where variables of those names are in scope.
*/
internal b32
DX12LogIfFailed(HRESULT Result, const char *Call, const char *File, int LineNumber,
                thread_context *Thread, game_memory *Memory)
{
    b32 Failed = FAILED(Result);
    if(Failed)
    {
        // Windows' text for the error code, cut at the line break it ends with.
        char Reason[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
                       (DWORD)Result, 0, Reason, sizeof(Reason), 0);
        for(char *At = Reason; *At; ++At)
        {
            if((*At == '\r') || (*At == '\n'))
            {
                *At = 0;
                break;
            }
        }

        char Line[1024];
        snprintf(Line, sizeof(Line), "ERROR: %s(%d): %s\n       failed with 0x%08X: %s\n",
                 File, LineNumber, Call, (u32)Result,
                 Reason[0] ? Reason : "(Windows has no description for this code)");
        RendererLog(Thread, Memory, Line);
    }
    return(Failed);
}

#define DX12_FAILED(Call) DX12LogIfFailed((Call), #Call, __FILE__, __LINE__, Thread, Memory)

// Declared early: RendererInitialize cleans up with RendererShutdown when it
// fails, and CreateStaticBuffer waits with FlushCommandQueue.
internal void RendererShutdown(dx12_renderer *Renderer);
internal void FlushCommandQueue(dx12_renderer *Renderer);

// Creates a descriptor heap and stores its start handles and slot size.
// CBV/SRV/UAV and sampler heaps are made shader-visible, because only shaders read them.
internal b32
DescriptorHeapInit(descriptor_heap *Heap, ID3D12Device *Device,
                   D3D12_DESCRIPTOR_HEAP_TYPE Type, u32 Count,
                   thread_context *Thread, game_memory *Memory)
{
    b32 ShaderVisible = ((Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) ||
                         (Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));

    D3D12_DESCRIPTOR_HEAP_DESC Desc = {};
    Desc.NumDescriptors = Count;
    Desc.Type = Type;
    Desc.Flags = ShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                               : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Desc.NodeMask = 0;          // single GPU
    if(DX12_FAILED(Device->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&Heap->Heap))))
    {
        return(false);
    }

    Heap->CpuStart = Heap->Heap->GetCPUDescriptorHandleForHeapStart();
    if(ShaderVisible)
    {
        // Only shader-visible heaps have a GPU address.
        Heap->GpuStart = Heap->Heap->GetGPUDescriptorHandleForHeapStart();
    }
    Heap->DescriptorSize = Device->GetDescriptorHandleIncrementSize(Type);
    Heap->Count = Count;

    return(true);
}

// CPU handle of slot Index: used to write descriptors and by commands like ClearRenderTargetView.
inline D3D12_CPU_DESCRIPTOR_HANDLE
DescriptorCpuHandle(descriptor_heap *Heap, u32 Index)
{
    Assert(Index < Heap->Count);
    D3D12_CPU_DESCRIPTOR_HANDLE Result = Heap->CpuStart;
    Result.ptr += (SIZE_T)Index * Heap->DescriptorSize;
    return(Result);
}

// GPU handle of slot Index: what descriptor tables give shaders.  Shader-visible heaps only.
inline D3D12_GPU_DESCRIPTOR_HANDLE
DescriptorGpuHandle(descriptor_heap *Heap, u32 Index)
{
    Assert(Index < Heap->Count);
    Assert(Heap->GpuStart.ptr);
    D3D12_GPU_DESCRIPTOR_HANDLE Result = Heap->GpuStart;
    Result.ptr += (UINT64)Index * Heap->DescriptorSize;
    return(Result);
}

// Hands out a shader-visible heap slot, reusing freed ones first so the heap
// doesn't run out.  False when full.
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

// Gives a slot back to the free list for SrvAllocate to reuse.
internal void
SrvRelease(dx12_renderer *Renderer, u32 Index)
{
    Assert(Index < Renderer->SrvNextUnused);
    Assert(Renderer->SrvFreeCount < SRV_HEAP_CAPACITY);
    Renderer->SrvFreeList[Renderer->SrvFreeCount++] = Index;
}

// Gets the swap chain's buffers and writes a render target view for each, so
// they can be cleared and drawn into.  Called at startup and after every resize.
internal b32
AcquireBackBuffers(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory)
{
    for(u32 BufferIndex = 0; BufferIndex < SWAP_CHAIN_BUFFER_COUNT; ++BufferIndex)
    {
        if(DX12_FAILED(Renderer->SwapChain->GetBuffer(BufferIndex,
                                                      IID_PPV_ARGS(&Renderer->BackBuffers[BufferIndex]))))
        {
            return(false);
        }

        // No view description (0): view the buffer in its own format, mip 0.
        Renderer->Device->CreateRenderTargetView(Renderer->BackBuffers[BufferIndex], 0,
                                                 DescriptorCpuHandle(&Renderer->RtvHeap, BufferIndex));
    }

    return(true);
}

// Drops our references to the swap chain buffers so ResizeBuffers can succeed.
// The views stay; AcquireBackBuffers overwrites them.
internal void
ReleaseBackBuffers(dx12_renderer *Renderer)
{
    for(u32 BufferIndex = 0; BufferIndex < SWAP_CHAIN_BUFFER_COUNT; ++BufferIndex)
    {
        ReleaseAndZero(Renderer->BackBuffers[BufferIndex]);
    }
}

// Creates a depth buffer of this size and its view in DsvHeap slot 0.
// Called at startup and on resize, since it must match the back buffer's size.
internal b32
CreateDepthBuffer(dx12_renderer *Renderer, i32 Width, i32 Height,
                  thread_context *Thread, game_memory *Memory)
{
    // DEFAULT heap (VRAM): only the GPU touches it.
    D3D12_HEAP_PROPERTIES HeapProperties = {};
    HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Alignment = 0;                     // default 64 KB
    Desc.Width = (UINT64)Width;             // must match the render target
    Desc.Height = (UINT)Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DEPTH_FORMAT;
    Desc.SampleDesc.Count = 1;              // must match the render target
    Desc.SampleDesc.Quality = 0;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;  // let the GPU pick its fastest layout
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // Clearing to these exact values is faster; the per-frame clear uses the
    // same ones, or the debug layer warns.
    D3D12_CLEAR_VALUE OptimizedClear = {};
    OptimizedClear.Format = DEPTH_FORMAT;
    OptimizedClear.DepthStencil.Depth = DEPTH_CLEAR_VALUE;
    OptimizedClear.DepthStencil.Stencil = STENCIL_CLEAR_VALUE;

    // Committed: memory and resource created together.  Starts in DEPTH_WRITE,
    // its only use, so no barrier is ever needed.
    if(DX12_FAILED(Renderer->Device->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE,
                                                             &Desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                             &OptimizedClear,
                                                             IID_PPV_ARGS(&Renderer->DepthBuffer))))
    {
        return(false);
    }

    // No view description (0): the resource's own format, mip 0.
    Renderer->Device->CreateDepthStencilView(Renderer->DepthBuffer, 0,
                                             DescriptorCpuHandle(&Renderer->DsvHeap, 0));

    return(true);
}

// Builds a barrier moving a whole resource from one state to another, so the
// GPU can finish, flush or re-layout it before the next use.
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

// Loads dxcompiler.dll and creates the DXC utils and compiler.  LoadLibrary
// instead of linking keeps the DLL loaded across game.dll hot reloads.
internal b32
LoadShaderCompiler(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory)
{
    Renderer->DxcModule = LoadLibraryA("dxcompiler.dll");
    if(!Renderer->DxcModule)
    {
        RendererLog(Thread, Memory, "ERROR: dxcompiler.dll not found next to clash.exe.\n");
        return(false);
    }

    // The DLL's only export; every DXC object is created through it.
    DxcCreateInstanceProc CreateInstance =
        (DxcCreateInstanceProc)(void *)GetProcAddress(Renderer->DxcModule, "DxcCreateInstance");
    if(!CreateInstance)
    {
        RendererLog(Thread, Memory, "ERROR: dxcompiler.dll has no DxcCreateInstance.\n");
        return(false);
    }

    if(DX12_FAILED(CreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Renderer->DxcUtils))) ||
       DX12_FAILED(CreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Renderer->DxcCompiler))))
    {
        return(false);
    }

    return(true);
}

// Compiles one entry point (e.g. "VS") of an .hlsl file for a target (e.g.
// "vs_6_6").  Returns the bytecode for the caller to Release, or 0 with the
// compiler's errors logged.
internal IDxcBlob *
CompileShader(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
              LPCWSTR FileName, LPCWSTR EntryPoint, LPCWSTR Target)
{
    IDxcBlob *Result = 0;

    IDxcBlobEncoding *Source = 0;
    if(DX12_FAILED(Renderer->DxcUtils->LoadFile(FileName, 0, &Source)))
    {
        return(0);
    }

    DxcBuffer SourceBuffer;
    SourceBuffer.Ptr = Source->GetBufferPointer();
    SourceBuffer.Size = Source->GetBufferSize();
    SourceBuffer.Encoding = DXC_CP_ACP;     // let DXC detect the text encoding

    // dxc.exe command-line switches.  File name first so errors name the file.
    LPCWSTR Arguments[] =
    {
        FileName,
        L"-E", EntryPoint,
        L"-T", Target,
#if CLASH_DEBUG
        DXC_ARG_DEBUG,                  // -Zi: debug info, for PIX
        DXC_ARG_SKIP_OPTIMIZATIONS,     // -Od: so PIX can step line by line
        L"-Qembed_debug",               // store the debug info inside the shader
#endif
    };

    IDxcResult *Compiled = 0;
    if(!DX12_FAILED(Renderer->DxcCompiler->Compile(&SourceBuffer, Arguments, (UINT32)ArrayCount(Arguments),
                                                   0, IID_PPV_ARGS(&Compiled))))
    {
        // Log errors and warnings as dxc.exe would print them.
        IDxcBlobUtf8 *Errors = 0;
        Compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&Errors), 0);
        if(Errors && Errors->GetStringLength())
        {
            RendererLog(Thread, Memory, Errors->GetStringPointer());
        }
        ReleaseAndZero(Errors);

        // Compile() succeeds even when the shader has errors; the status tells which.
        HRESULT Status = E_FAIL;
        Compiled->GetStatus(&Status);
        if(SUCCEEDED(Status))
        {
            Compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&Result), 0);
        }
        else
        {
            char Line[512];
            snprintf(Line, sizeof(Line), "ERROR: %ls (%ls, %ls) did not compile.\n",
                     FileName, EntryPoint, Target);
            RendererLog(Thread, Memory, Line);
        }
    }

    ReleaseAndZero(Compiled);
    ReleaseAndZero(Source);
    return(Result);
}

// Heap properties for a heap type; every other field stays at its 0 default.
inline D3D12_HEAP_PROPERTIES
DescribeHeap(D3D12_HEAP_TYPE Type)
{
    D3D12_HEAP_PROPERTIES Result = {};
    Result.Type = Type;
    return(Result);
}

// Resource description for a buffer of Size bytes; all buffers differ only in size.
inline D3D12_RESOURCE_DESC
DescribeBuffer(u64 Size)
{
    D3D12_RESOURCE_DESC Result = {};
    Result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    Result.Width = Size;
    Result.Height = 1;
    Result.DepthOrArraySize = 1;
    Result.MipLevels = 1;
    Result.Format = DXGI_FORMAT_UNKNOWN;
    Result.SampleDesc.Count = 1;
    Result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return(Result);
}

/*
  Creates a VRAM (DEFAULT heap) buffer holding Data, because VRAM is fastest
  for the GPU to read.  The CPU can't write VRAM, so the data goes into a
  temporary UPLOAD buffer and the GPU copies it across.  Waits for the copy,
  so it's for startup only.

  No barriers: buffers change state automatically for the copy and for
  drawing, and return to COMMON after each ExecuteCommandLists.
*/
internal b32
CreateStaticBuffer(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
                   const void *Data, u64 Size, ID3D12Resource **Result)
{
    D3D12_RESOURCE_DESC Desc = DescribeBuffer(Size);

    // The final buffer, in VRAM.
    D3D12_HEAP_PROPERTIES DefaultHeap = DescribeHeap(D3D12_HEAP_TYPE_DEFAULT);
    if(DX12_FAILED(Renderer->Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                             D3D12_RESOURCE_STATE_COMMON, 0,
                                                             IID_PPV_ARGS(Result))))
    {
        return(false);
    }

    // A temporary buffer the CPU can write.
    ID3D12Resource *Upload = 0;
    D3D12_HEAP_PROPERTIES UploadHeap = DescribeHeap(D3D12_HEAP_TYPE_UPLOAD);
    if(DX12_FAILED(Renderer->Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, 0,
                                                             IID_PPV_ARGS(&Upload))))
    {
        return(false);
    }

    // Copy the data in.  Empty read range: the CPU never reads it back.
    void *Mapped = 0;
    D3D12_RANGE NoRead = {0, 0};
    if(DX12_FAILED(Upload->Map(0, &NoRead, &Mapped)))
    {
        Upload->Release();
        return(false);
    }
    memcpy(Mapped, Data, Size);
    Upload->Unmap(0, 0);

    // Record and run a GPU copy into VRAM, then wait so the upload buffer can be freed.
    b32 Copied = false;
    if(!DX12_FAILED(Renderer->CommandAllocator->Reset()) &&
       !DX12_FAILED(Renderer->CommandList->Reset(Renderer->CommandAllocator, 0)))
    {
        Renderer->CommandList->CopyBufferRegion(*Result, 0, Upload, 0, Size);
        if(!DX12_FAILED(Renderer->CommandList->Close()))
        {
            ID3D12CommandList *Lists[] = {Renderer->CommandList};
            Renderer->Queue->ExecuteCommandLists(1, Lists);
            FlushCommandQueue(Renderer);
            Copied = true;
        }
    }

    Upload->Release();
    return(Copied);
}

// Builds everything needed to draw the cube: root signature, shaders, pipeline
// state, geometry in VRAM, and the constant buffer for its matrices.
internal b32
CreateCube(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory)
{
    // ------------------------------------------------------------------
    // 1. Root signature: tells the pipeline the shaders take two constant
    //    buffers, b0 and b1, each passed as a plain GPU address (root CBV)
    //    so no descriptor heap is needed.
    // ------------------------------------------------------------------
    D3D12_ROOT_PARAMETER Parameters[2] = {};
    Parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    Parameters[0].Descriptor.ShaderRegister = 0;        // b0: ObjectConstants
    Parameters[0].Descriptor.RegisterSpace = 0;
    Parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    Parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    Parameters[1].Descriptor.ShaderRegister = 1;        // b1: PassConstants
    Parameters[1].Descriptor.RegisterSpace = 0;
    Parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT: we feed vertices through an input layout.
    D3D12_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.NumParameters = (UINT)ArrayCount(Parameters);
    RootDesc.pParameters = Parameters;
    RootDesc.NumStaticSamplers = 0;
    RootDesc.pStaticSamplers = 0;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize to bytes first (CreateRootSignature only takes bytes), logging any errors.
    ID3DBlob *Serialized = 0;
    ID3DBlob *SerializeErrors = 0;
    HRESULT SerializeResult = D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                          &Serialized, &SerializeErrors);
    if(SerializeErrors)
    {
        RendererLog(Thread, Memory, (char *)SerializeErrors->GetBufferPointer());
        ReleaseAndZero(SerializeErrors);
    }
    if(DX12_FAILED(SerializeResult))
    {
        ReleaseAndZero(Serialized);
        return(false);
    }

    b32 NoRootSignature =
        DX12_FAILED(Renderer->Device->CreateRootSignature(0, Serialized->GetBufferPointer(),
                                                          Serialized->GetBufferSize(),
                                                          IID_PPV_ARGS(&Renderer->CubeRootSignature)));
    ReleaseAndZero(Serialized);
    if(NoRootSignature)
    {
        return(false);
    }

    // ------------------------------------------------------------------
    // 2. Compile cube.hlsl's vertex and pixel shaders.
    // ------------------------------------------------------------------
    IDxcBlob *VertexShader = CompileShader(Renderer, Thread, Memory,
                                           L"data\\shaders\\cube.hlsl", L"VS", L"vs_6_6");
    IDxcBlob *PixelShader = CompileShader(Renderer, Thread, Memory,
                                          L"data\\shaders\\cube.hlsl", L"PS", L"ps_6_6");
    if(!VertexShader || !PixelShader)
    {
        ReleaseAndZero(VertexShader);
        ReleaseAndZero(PixelShader);
        return(false);
    }

    // ------------------------------------------------------------------
    // 3. Input layout: tells the GPU how to turn buffer bytes into the vertex
    //    shader's inputs - POSITION from slot 0, COLOR from slot 1, each at
    //    offset 0 because each buffer holds only one attribute.
    // ------------------------------------------------------------------

    D3D12_INPUT_ELEMENT_DESC vertexDesc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // ------------------------------------------------------------------
    // 4. Pipeline state object: all draw settings baked into one object, so
    //    the driver validates and compiles them once instead of per draw.
    // ------------------------------------------------------------------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC PipelineDesc = {};
    PipelineDesc.pRootSignature = Renderer->CubeRootSignature;
    PipelineDesc.VS.pShaderBytecode = VertexShader->GetBufferPointer();
    PipelineDesc.VS.BytecodeLength = VertexShader->GetBufferSize();
    PipelineDesc.PS.pShaderBytecode = PixelShader->GetBufferPointer();
    PipelineDesc.PS.BytecodeLength = PixelShader->GetBufferSize();

    // Blending off: the pixel shader's colour replaces what's there.
    D3D12_RENDER_TARGET_BLEND_DESC *Blend = &PipelineDesc.BlendState.RenderTarget[0];
    Blend->BlendEnable = FALSE;
    Blend->LogicOpEnable = FALSE;
    Blend->SrcBlend = D3D12_BLEND_ONE;
    Blend->DestBlend = D3D12_BLEND_ZERO;
    Blend->BlendOp = D3D12_BLEND_OP_ADD;
    Blend->SrcBlendAlpha = D3D12_BLEND_ONE;
    Blend->DestBlendAlpha = D3D12_BLEND_ZERO;
    Blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    Blend->LogicOp = D3D12_LOGIC_OP_NOOP;
    Blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    PipelineDesc.SampleMask = 0xFFFFFFFF;

    // Filled triangles; back faces (counter-clockwise on screen) skipped, since they're hidden.
    PipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PipelineDesc.RasterizerState.FrontCounterClockwise = FALSE;
    PipelineDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PipelineDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PipelineDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PipelineDesc.RasterizerState.DepthClipEnable = TRUE;

    // Depth test on: a pixel is kept only if it's nearer (LESS) than what's
    // already there, and its depth is written.  Stencil unused.
    D3D12_DEPTH_STENCIL_DESC *DepthStencil = &PipelineDesc.DepthStencilState;
    DepthStencil->DepthEnable = TRUE;
    DepthStencil->DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    DepthStencil->DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    DepthStencil->StencilEnable = FALSE;
    DepthStencil->StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    DepthStencil->StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    D3D12_DEPTH_STENCILOP_DESC KeepAlways = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                             D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    DepthStencil->FrontFace = KeepAlways;
    DepthStencil->BackFace = KeepAlways;

    // Triangles drawn into one back buffer plus the depth buffer, no MSAA;
    // formats must match the real targets.
    PipelineDesc.InputLayout.pInputElementDescs = vertexDesc;
    PipelineDesc.InputLayout.NumElements = (UINT)ArrayCount(vertexDesc);
    PipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PipelineDesc.NumRenderTargets = 1;
    PipelineDesc.RTVFormats[0] = SWAP_CHAIN_FORMAT;
    PipelineDesc.DSVFormat = DEPTH_FORMAT;
    PipelineDesc.SampleDesc.Count = 1;
    PipelineDesc.SampleDesc.Quality = 0;

    // The PSO keeps its own copy of the shaders, so the blobs can be released.
    b32 NoPipeline =
        DX12_FAILED(Renderer->Device->CreateGraphicsPipelineState(&PipelineDesc,
                                                                  IID_PPV_ARGS(&Renderer->CubePipeline)));
    ReleaseAndZero(VertexShader);
    ReleaseAndZero(PixelShader);
    if(NoPipeline)
    {
        return(false);
    }

    // ------------------------------------------------------------------
    // 5. Two shapes packed into one pair of vertex buffers: the cube's 8
    //    corners, then the pyramid's 5, each in its own local space (the
    //    World matrix moves them).  Same order in both arrays: vertex i is
    //    Positions[i] with Colors[i].
    // ------------------------------------------------------------------
    VPosData Positions[] =
    {
        // Cube: vertices 0-7.
        {{-1.0f, -1.0f, -1.0f}},    // 0
        {{-1.0f, +1.0f, -1.0f}},    // 1
        {{+1.0f, +1.0f, -1.0f}},    // 2
        {{+1.0f, -1.0f, -1.0f}},    // 3
        {{-1.0f, -1.0f, +1.0f}},    // 4
        {{-1.0f, +1.0f, +1.0f}},    // 5
        {{+1.0f, +1.0f, +1.0f}},    // 6
        {{+1.0f, -1.0f, +1.0f}},    // 7

        // Pyramid: vertices 8-12, numbered 0-4 in its own indices.
        {{-1.0f, -1.0f, -1.0f}},    // 0: base
        {{+1.0f, -1.0f, -1.0f}},    // 1: base
        {{-1.0f, -1.0f, +1.0f}},    // 2: base
        {{+1.0f, -1.0f, +1.0f}},    // 3: base
        {{+0.0f, +1.0f, +0.0f}},    // 4: tip
    };

    VColorData Colors[] =
    {
        // Cube.
        {{1.0f, 1.0f, 1.0f, 1.0f}},     // 0: white
        {{0.0f, 0.0f, 0.0f, 1.0f}},     // 1: black
        {{1.0f, 0.0f, 0.0f, 1.0f}},     // 2: red
        {{0.0f, 1.0f, 0.0f, 1.0f}},     // 3: green
        {{0.0f, 0.0f, 1.0f, 1.0f}},     // 4: blue
        {{1.0f, 1.0f, 0.0f, 1.0f}},     // 5: yellow
        {{0.0f, 1.0f, 1.0f, 1.0f}},     // 6: cyan
        {{1.0f, 0.0f, 1.0f, 1.0f}},     // 7: magenta

        // Pyramid.
        {{0.0f, 1.0f, 0.0f, 1.0f}},     // 0: green
        {{0.0f, 1.0f, 0.0f, 1.0f}},     // 1: green
        {{0.0f, 1.0f, 0.0f, 1.0f}},     // 2: green
        {{0.0f, 1.0f, 0.0f, 1.0f}},     // 3: green
        {{1.0f, 0.0f, 0.0f, 1.0f}},     // 4: red
    };

    // ------------------------------------------------------------------
    // 6. Indices, cube's then pyramid's.  Each shape's indices count from
    //    its own vertex 0, unchanged - the submesh's BaseVertex shifts them.
    //    Clockwise seen from outside, so back faces cull.  16-bit: half
    //    the memory of 32-bit.
    // ------------------------------------------------------------------
    u16 Indices[] =
    {
        // Cube: 36 indices, 12 triangles.
        0, 1, 2,   0, 2, 3,     // front  (z = -1)
        4, 6, 5,   4, 7, 6,     // back   (z = +1)
        4, 5, 1,   4, 1, 0,     // left   (x = -1)
        3, 2, 6,   3, 6, 7,     // right  (x = +1)
        1, 5, 6,   1, 6, 2,     // top    (y = +1)
        4, 0, 3,   4, 3, 7,     // bottom (y = -1)

        // Pyramid: 18 indices, 6 triangles.
        0, 1, 3,   0, 3, 2,     // base   (y = -1)
        0, 4, 1,   1, 4, 3,     // sides at z = -1 and x = +1
        2, 3, 4,   0, 2, 4,     // sides at z = +1 and x = -1
    };

    // Upload positions, colours and indices to VRAM.
    mesh *Mesh = &Renderer->CubeMesh;
    if(!CreateStaticBuffer(Renderer, Thread, Memory, Positions, sizeof(Positions), &Mesh->VertexBuffers[0]) ||
            !CreateStaticBuffer(Renderer, Thread, Memory, Colors, sizeof(Colors), &Mesh->VertexBuffers[1]) ||
            !CreateStaticBuffer(Renderer, Thread, Memory, Indices, sizeof(Indices), &Mesh->IndexBuffer))
    {
        return(false);
    }

    // Vertex views: address, total size, and stride (bytes per vertex) of each buffer.
    // Slot 0: positions, 12 bytes each.
    Mesh->VertexViews[0].BufferLocation = Mesh->VertexBuffers[0]->GetGPUVirtualAddress();
    Mesh->VertexViews[0].SizeInBytes = (UINT)sizeof(Positions);
    Mesh->VertexViews[0].StrideInBytes = (UINT)sizeof(VPosData);

    // Slot 1: colours, 16 bytes each.
    Mesh->VertexViews[1].BufferLocation = Mesh->VertexBuffers[1]->GetGPUVirtualAddress();
    Mesh->VertexViews[1].SizeInBytes = (UINT)sizeof(Colors);
    Mesh->VertexViews[1].StrideInBytes = (UINT)sizeof(VColorData);

    // Index view: address, size, and 16-bit format to match the u16 array.
    Mesh->IndexView.BufferLocation = Mesh->IndexBuffer->GetGPUVirtualAddress();
    Mesh->IndexView.SizeInBytes = (UINT)sizeof(Indices);
    Mesh->IndexView.Format = DXGI_FORMAT_R16_UINT;

    Mesh->SubmeshCount = 2;
    Mesh->Submeshes[0].IndexCount = 36;
    Mesh->Submeshes[0].StartIndex = 0;
    Mesh->Submeshes[0].BaseVertex = 0;
    Mesh->Submeshes[1].IndexCount = 18;
    Mesh->Submeshes[1].StartIndex = 36;
    Mesh->Submeshes[1].BaseVertex = 8;
    // ------------------------------------------------------------------
    // 7. Constant buffer: two 256-byte slots (World, ViewProj) in an UPLOAD
    //    heap because the CPU rewrites them every frame.  Mapped once and
    //    left mapped, which upload heaps allow.
    // ------------------------------------------------------------------
    D3D12_HEAP_PROPERTIES UploadHeap = DescribeHeap(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC ConstantDesc = DescribeBuffer((BOX_COUNT + 1)*CONSTANT_BUFFER_SLOT_SIZE);
    if(DX12_FAILED(Renderer->Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &ConstantDesc,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, 0,
                                                             IID_PPV_ARGS(&Renderer->ConstantBuffer))))
    {
        return(false);
    }

    // Empty read range: the CPU only writes.
    D3D12_RANGE NoRead = {0, 0};
    if(DX12_FAILED(Renderer->ConstantBuffer->Map(0, &NoRead, (void **)&Renderer->ConstantBufferMapped)))
    {
        return(false);
    }

    return(true);
}

// Starts DirectX 12 on the given window: device, queue, swap chain, heaps,
// depth buffer, then the cube.  False if DirectX 12 can't start.
internal b32
RendererInitialize(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
                   HWND Window, i32 Width, i32 Height)
{
    Renderer->Width = Width;
    Renderer->Height = Height;

    // ------------------------------------------------------------------
    // Debug layer: validates every D3D12 call and reports mistakes.  Must be
    // enabled before the device is created, or that device won't have it.
    // ------------------------------------------------------------------
    UINT FactoryFlags = 0;
#if CLASH_DEBUG
    ID3D12Debug *Debug = 0;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug))))
    {
        Debug->EnableDebugLayer();

        // GPU-based validation: checks shaders as they run.  Much slower, so
        // left off; uncomment to hunt bugs the normal layer misses.
        // ID3D12Debug1 *Debug1 = 0;
        // if(SUCCEEDED(Debug->QueryInterface(IID_PPV_ARGS(&Debug1))))
        // {
        //     Debug1->SetEnableGPUBasedValidation(TRUE);
        //     Debug1->Release();
        // }

        Debug->Release();
    }

    // Also report DXGI (swap chain) errors.
    FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    if(DX12_FAILED(CreateDXGIFactory2(FactoryFlags, IID_PPV_ARGS(&Renderer->Factory))))
    {
        return(false);
    }

    // ------------------------------------------------------------------
    // Device: try GPUs fastest-first (so laptops use the discrete GPU) and
    // take the first that supports feature level 12_0.  Extras like ray
    // tracing are checked separately so older GPUs still work.
    // ------------------------------------------------------------------
    for(UINT AdapterIndex = 0;; ++AdapterIndex)
    {
        IDXGIAdapter1 *Adapter = 0;
        if(FAILED(Renderer->Factory->EnumAdapterByGpuPreference(AdapterIndex,
                                                                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                IID_PPV_ARGS(&Adapter))))
        {
            // No more GPUs to try.
            break;
        }

        DXGI_ADAPTER_DESC1 Desc;
        Adapter->GetDesc1(&Desc);

        // Skip the software (CPU) renderer: far too slow to play on.
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
    // With a debugger attached, break on the exact call that causes a D3D12
    // error, instead of finding the message later.  Without one, a break would crash.
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
    // Query the highest feature level and optional features, so later code
    // can use them only where available.
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

    // Log the GPU and its features, to confirm which GPU is in use.
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

    // Stored for the debug UI.
    snprintf(Renderer->GpuName, sizeof(Renderer->GpuName), "%ls", Desc.Description);
    Renderer->DedicatedVideoMemory = (u64)Desc.DedicatedVideoMemory;

    // Adapter3 has QueryVideoMemoryInfo for the debug UI's VRAM display.
    // Optional: without it the display is just missing.
    Renderer->Adapter->QueryInterface(IID_PPV_ARGS(&Renderer->Adapter3));

    // Warn if clash.exe's Agility SDK version doesn't match these headers,
    // because D3D12 then silently falls back to Windows' older runtime.
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

    // Log which D3D12Core.dll loaded: build\D3D12\ is the Agility SDK, System32 is Windows' own.
    char CorePath[MAX_PATH] = {};
    HMODULE Core = GetModuleHandleA("D3D12Core.dll");
    if(Core)
    {
        GetModuleFileNameA(Core, CorePath, sizeof(CorePath));
    }
    snprintf(Line, sizeof(Line), "D3D12 runtime: %s\n", Core ? CorePath : "D3D12Core.dll not loaded");
    RendererLog(Thread, Memory, Line);

    // Fence and event, so the CPU can wait for the GPU (FlushCommandQueue).
    if(DX12_FAILED(Renderer->Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(&Renderer->Fence))))
    {
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
    // Command queue, allocator and list.  One DIRECT queue, the type that can
    // draw, copy and compute - enough until async copy/compute is needed.
    // ------------------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
    QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if(DX12_FAILED(Renderer->Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(&Renderer->Queue))))
    {
        RendererShutdown(Renderer);
        return(false);
    }

    // Memory for recorded commands.  Only Reset it once the GPU has finished them.
    if(DX12_FAILED(Renderer->Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&Renderer->CommandAllocator))))
    {
        RendererShutdown(Renderer);
        return(false);
    }

    // Node 0 (single GPU), no initial PSO (set while recording instead).
    if(DX12_FAILED(Renderer->Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       Renderer->CommandAllocator, 0,
                                                       IID_PPV_ARGS(&Renderer->CommandList))))
    {
        RendererShutdown(Renderer);
        return(false);
    }

    // New lists start open; close it because each frame starts with Reset, which needs a closed list.
    Renderer->CommandList->Close();

    // ------------------------------------------------------------------
    // Swap chain: the images shown on screen in turn.
    // ------------------------------------------------------------------
    // Enable tearing if supported, for vsync-off on variable-refresh monitors.
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

    // Not _SRGB: flip-model swap chains don't allow it; for sRGB, use an _SRGB render target view.
    SwapDesc.Format = SWAP_CHAIN_FORMAT;
    SwapDesc.Stereo = FALSE;

    // No MSAA: flip-model swap chains can't be multisampled.
    SwapDesc.SampleDesc.Count = 1;
    SwapDesc.SampleDesc.Quality = 0;

    SwapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    SwapDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;

    // Don't stretch the image if the window size differs.
    SwapDesc.Scaling = DXGI_SCALING_NONE;

    // Flip model, required by D3D12.  DISCARD: old contents are dropped, since
    // every frame is redrawn fully.
    SwapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    SwapDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    // No ALLOW_MODE_SWITCH: fullscreen is borderless (F11), never a resolution change.
    SwapDesc.Flags = Renderer->SwapChainFlags;

    // Created on the queue, because Present is queued after the frame's work.
    // Zeros: windowed, any monitor.
    IDXGISwapChain1 *SwapChain1 = 0;
    if(DX12_FAILED(Renderer->Factory->CreateSwapChainForHwnd(Renderer->Queue, Window, &SwapDesc,
                                                             0, 0, &SwapChain1)))
    {
        RendererShutdown(Renderer);
        return(false);
    }

    // Upgrade to version 3 for GetCurrentBackBufferIndex.
    b32 NoSwapChain3 = DX12_FAILED(SwapChain1->QueryInterface(IID_PPV_ARGS(&Renderer->SwapChain)));
    SwapChain1->Release();
    if(NoSwapChain3)
    {
        RendererShutdown(Renderer);
        return(false);
    }

    // Disable DXGI's Alt+Enter fullscreen so it doesn't fight the platform's F11.
    Renderer->Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);

    // ------------------------------------------------------------------
    // RTV heap (one slot per back buffer) and a render target view of each.
    // ------------------------------------------------------------------
    if(!DescriptorHeapInit(&Renderer->RtvHeap, Renderer->Device,
                           D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SWAP_CHAIN_BUFFER_COUNT,
                           Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the RTV descriptor heap.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    if(!AcquireBackBuffers(Renderer, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not get the swap chain's buffers.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // ------------------------------------------------------------------
    // Depth buffer, with a one-slot DSV heap for its view.
    // ------------------------------------------------------------------
    if(!DescriptorHeapInit(&Renderer->DsvHeap, Renderer->Device,
                           D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the DSV descriptor heap.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    if(!CreateDepthBuffer(Renderer, Width, Height, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the depth buffer.\n");
        RendererShutdown(Renderer);
        return(false);
    }

    // ------------------------------------------------------------------
    // Shader-visible heap: one for everything shaders read, since only one
    // can be bound at a time.
    // ------------------------------------------------------------------
    if(!DescriptorHeapInit(&Renderer->SrvHeap, Renderer->Device,
                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP_CAPACITY,
                           Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the shader-visible descriptor heap.\n");
        RendererShutdown(Renderer);
        return(false);
    }
    Renderer->SrvNextUnused = 0;
    Renderer->SrvFreeCount = 0;

    // Hot pink (255, 105, 180), so the cleared background is obvious; the debug UI can change it.
    Renderer->ClearColor[0] = 0.0f;
    Renderer->ClearColor[1] = 0.0f;
    Renderer->ClearColor[2] = 0.0f;
    Renderer->ClearColor[3] = 1.0f;

    // A missing compiler or cube isn't fatal: the game runs with just the clear colour.
    if(!LoadShaderCompiler(Renderer, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: No shader compiler - nothing will be drawn.\n");
    }
    else if(!CreateCube(Renderer, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: No cube - see the errors above.\n");
    }

    return(true);
}

// Resizes the swap chain and depth buffer to the window's new inner size.
// Never called with 0: the game doesn't run while minimised.
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

    // Wait: the GPU may still be using the old buffers.
    FlushCommandQueue(Renderer);

    // IMPORTANT(yigit): ResizeBuffers fails while ANYTHING still holds one
    // of the old buffers - so ours go first, and come back after.
    ReleaseBackBuffers(Renderer);

    // 0 and UNKNOWN keep the buffer count and format.  Same flags as creation,
    // or tearing support is silently lost.
    if(!DX12_FAILED(Renderer->SwapChain->ResizeBuffers(0, (UINT)Width, (UINT)Height,
                                                       DXGI_FORMAT_UNKNOWN,
                                                       Renderer->SwapChainFlags)))
    {
        char Line[128];
        snprintf(Line, sizeof(Line), "swap chain resized to %dx%d\n", Width, Height);
        RendererLog(Thread, Memory, Line);
    }

    // Re-fetch the buffers and rewrite their views (the old ones again if the resize failed).
    if(!AcquireBackBuffers(Renderer, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not get the swap chain's buffers after a resize.\n");
    }

    // The depth buffer isn't part of the swap chain, so rebuild it at the new size.
    ReleaseAndZero(Renderer->DepthBuffer);
    if(!CreateDepthBuffer(Renderer, Width, Height, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: Could not make the depth buffer after a resize.\n");
    }
}

// Records and submits one frame: clear, draw the cube, draw ImGui, present.
// CubeWorld and ViewProj are row-vector matrices from the game.
internal void
RendererRender(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
               game_input *Input, const XMFLOAT4X4 *Worlds, const XMFLOAT4X4 *ViewProj, r32 Time)
{
    if(!Renderer->SwapChain || !Renderer->BackBuffers[0] || !Renderer->DepthBuffer)
    {
        return;
    }

    // Reuse the allocator's memory (safe: last frame ended with a flush) and start recording.
    if(DX12_FAILED(Renderer->CommandAllocator->Reset()) ||
       DX12_FAILED(Renderer->CommandList->Reset(Renderer->CommandAllocator, 0)))
    {
        return;
    }

    // Ask which buffer to draw into: with flip-model it alternates.
    u32 BackBufferIndex = Renderer->SwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource *BackBuffer = Renderer->BackBuffers[BackBufferIndex];

    // PRESENT -> RENDER_TARGET, so the GPU finishes showing it before we draw into it.
    D3D12_RESOURCE_BARRIER ToRenderTarget =
        TransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    Renderer->CommandList->ResourceBarrier(1, &ToRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv = DescriptorCpuHandle(&Renderer->RtvHeap, BackBufferIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DescriptorCpuHandle(&Renderer->DsvHeap, 0);

    // Clear colour, so no old frame shows through.
    Renderer->CommandList->ClearRenderTargetView(Rtv, Renderer->ClearColor, 0, 0);

    // Depth to the far plane so anything drawn is nearer; stencil to 0.
    Renderer->CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                 DEPTH_CLEAR_VALUE, STENCIL_CLEAR_VALUE, 0, 0);

    // Draw into this back buffer and the depth buffer.
    Renderer->CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);

    // Viewport: map clip space onto the whole window and depth onto 0..1.
    // Reset clears it, so it's set every frame.
    D3D12_VIEWPORT Viewport = {};
    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width = (r32)Renderer->Width;
    Viewport.Height = (r32)Renderer->Height;
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;
    Renderer->CommandList->RSSetViewports(1, &Viewport);

    // Scissor: keep pixels inside the whole window.
    // IMPORTANT(yigit): In D3D12 the scissor test is ALWAYS on, and the
    // default rectangle is empty - forget this and nothing is ever drawn.
    // Clears ignore both, which is why the pink worked without them.
    D3D12_RECT Scissor = {0, 0, Renderer->Width, Renderer->Height};
    Renderer->CommandList->RSSetScissorRects(1, &Scissor);

    //NOTE:(yigit): Drawing
    // Only if the cube was created successfully.
    if(Renderer->CubePipeline && Renderer->ConstantBufferMapped)
    {
        // Transpose because HLSL reads constant-buffer matrices column by column.
        object_constants Object;
        pass_constants Pass;
        // Each box: fill the box with its World, copy it to its own slot.
        for(u32 BoxIndex = 0; BoxIndex < BOX_COUNT; ++BoxIndex)
        {
            XMStoreFloat4x4(&Object.World, XMMatrixTranspose(XMLoadFloat4x4(&Worlds[BoxIndex])));
            memcpy(Renderer->ConstantBufferMapped + BoxIndex*CONSTANT_BUFFER_SLOT_SIZE,
                    &Object, sizeof(Object));
        }

        // Pass: camera and time, to slot 2.
        XMStoreFloat4x4(&Pass.ViewProj, XMMatrixTranspose(XMLoadFloat4x4(ViewProj)));
        Pass.gTime = Time;
        Pass.PulseColor = {1.0f,0.0f,0.50f,1.0f};
        memcpy(Renderer->ConstantBufferMapped + BOX_COUNT*CONSTANT_BUFFER_SLOT_SIZE, &Pass, sizeof(Pass));
        D3D12_GPU_VIRTUAL_ADDRESS Constants = Renderer->ConstantBuffer->GetGPUVirtualAddress();

        Renderer->CommandList->SetGraphicsRootSignature(Renderer->CubeRootSignature);
        Renderer->CommandList->SetPipelineState(Renderer->CubePipeline);

        // Point b0 and b1 at their slots.
        Renderer->CommandList->SetGraphicsRootConstantBufferView(1, Constants + BOX_COUNT*CONSTANT_BUFFER_SLOT_SIZE);
        // Every 3 indices form one triangle; positions from slot 0, colours from slot 1.
        Renderer->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mesh *Mesh = &Renderer->CubeMesh;
        Renderer->CommandList->IASetVertexBuffers(0, 2, Mesh->VertexViews);
        Renderer->CommandList->IASetIndexBuffer(&Mesh->IndexView);

        submesh *Box = &Mesh->Submeshes[0];
        for(u32 BoxIndex = 0; BoxIndex < BOX_COUNT; ++BoxIndex)
        {
            Renderer->CommandList->SetGraphicsRootConstantBufferView(0, Constants + BoxIndex*CONSTANT_BUFFER_SLOT_SIZE);
            Renderer->CommandList->DrawIndexedInstanced(Box->IndexCount, 1, Box->StartIndex,
                    Box->BaseVertex, 0);
        }
    }

    // ImGui last, so it's drawn on top.  Bind the shader-visible heap because
    // its font texture lives there.
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

    // RENDER_TARGET -> PRESENT, the only state Present accepts.
    D3D12_RESOURCE_BARRIER ToPresent =
        TransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT);
    Renderer->CommandList->ResourceBarrier(1, &ToPresent);

    // Close ends recording; it fails if a mistake was recorded, and then the list must not run.
    if(DX12_FAILED(Renderer->CommandList->Close()))
    {
        return;
    }
    ID3D12CommandList *Lists[] = {Renderer->CommandList};
    Renderer->Queue->ExecuteCommandLists(1, Lists);

    // Show the frame at the next monitor refresh (vsync), which also paces the game.
    DX12_FAILED(Renderer->SwapChain->Present(1, 0));

    // NOTE(yigit): Waits for the GPU every frame, so the CPU and GPU take
    // turns instead of working at the same time.
    FlushCommandQueue(Renderer);
}

/*
  Fills in this process's video memory budget and usage, for the debug UI.
  Local = the card's VRAM, NonLocal = system RAM the GPU can use.  Going over
  Budget makes Windows move memory out to RAM, which causes stutters.
  False if there's no IDXGIAdapter3 to ask.
*/
internal b32
RendererGetVideoMemory(dx12_renderer *Renderer, DXGI_QUERY_VIDEO_MEMORY_INFO *Local,
                       DXGI_QUERY_VIDEO_MEMORY_INFO *NonLocal)
{
    b32 Result = false;
    if(Renderer->Adapter3)
    {
        // Node 0: single GPU.
        Result = (SUCCEEDED(Renderer->Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, Local)) &&
                  SUCCEEDED(Renderer->Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, NonLocal)));
    }
    return(Result);
}

// Blocks until the GPU has finished all submitted work, so its memory can be
// reused or released safely.
internal void
FlushCommandQueue(dx12_renderer *Renderer)
{
    // Nothing to wait on if initialization failed before these existed.
    if(!Renderer->Queue || !Renderer->Fence)
    {
        return;
    }

    // Queue a signal with a new value, then sleep until the GPU reaches it.
    ++Renderer->FenceValue;
    Renderer->Queue->Signal(Renderer->Fence, Renderer->FenceValue);

    if(Renderer->Fence->GetCompletedValue() < Renderer->FenceValue)
    {
        Renderer->Fence->SetEventOnCompletion(Renderer->FenceValue, Renderer->FenceEvent);
        WaitForSingleObject(Renderer->FenceEvent, INFINITE);
    }
}

// Releases everything, after waiting for the GPU to stop using it.
internal void
RendererShutdown(dx12_renderer *Renderer)
{
    FlushCommandQueue(Renderer);

    // Checked because a failed RendererInitialize may not have created it,
    // and closing a bad handle under a debugger raises an exception.
    if(Renderer->FenceEvent)
    {
        CloseHandle(Renderer->FenceEvent);
        Renderer->FenceEvent = 0;
    }

    //NOTE(yigit): Order matters. Newest first.

    // ImGui must already be stopped (GameShutdown/GameUnload do it) because it uses SrvHeap.
    Assert(!Renderer->ImGui);
    if(Renderer->ConstantBuffer && Renderer->ConstantBufferMapped)
    {
        Renderer->ConstantBuffer->Unmap(0, 0);
        Renderer->ConstantBufferMapped = 0;
    }
    ReleaseAndZero(Renderer->ConstantBuffer);
    ReleaseAndZero(Renderer->CubeMesh.IndexBuffer);
    ReleaseAndZero(Renderer->CubeMesh.VertexBuffers[0]);
    ReleaseAndZero(Renderer->CubeMesh.VertexBuffers[1]);
    ReleaseAndZero(Renderer->CubePipeline);
    ReleaseAndZero(Renderer->CubeRootSignature);
    ReleaseAndZero(Renderer->DxcCompiler);
    ReleaseAndZero(Renderer->DxcUtils);
    if(Renderer->DxcModule)
    {
        FreeLibrary(Renderer->DxcModule);
        Renderer->DxcModule = 0;
    }
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
    ReleaseAndZero(Renderer->Adapter3);
    ReleaseAndZero(Renderer->Device5);
    ReleaseAndZero(Renderer->Device);
    ReleaseAndZero(Renderer->Adapter);
    ReleaseAndZero(Renderer->Factory);
}

#define DX12_RENDERER_CPP
#endif
