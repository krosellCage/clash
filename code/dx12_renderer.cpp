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
    6. A textured cube: descriptor heaps, a texture, the upload path.
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

// Constant buffer slots: the pass in slot 0, then one per object drawn this
// frame, handed out in order by DrawShape - so nothing counts slots by hand.
#define PASS_SLOT 0
#define FIRST_OBJECT_SLOT 1
#define OBJECT_SLOT_COUNT 64
#define CONSTANT_BUFFER_SLOT_COUNT (FIRST_OBJECT_SLOT + OBJECT_SLOT_COUNT)

// Frame resources (Luna 7.2): how many frames the CPU may record ahead of the
// GPU.  Each frame gets its own copy of everything it writes - so while the
// GPU still draws frame N, the CPU fills frame N+1's copy instead of
// waiting.  3: the CPU may get up to two frames ahead.
#define FRAME_RESOURCE_COUNT 3

struct frame_resource
{
    // The memory this frame's commands are recorded into.  Reset only once
    // the GPU has run them - i.e. once the fence has reached FenceValue.
    ID3D12CommandAllocator *CommandAllocator;

    // This frame's constant buffer slots (pass + objects), in an UPLOAD heap
    // and mapped for good, like before - one per frame, so writing this
    // frame's never changes a slot an earlier frame's draws still read.
    ID3D12Resource *ConstantBuffer;
    u8 *ConstantBufferMapped;

    // The water's vertices - a dynamic vertex buffer (Luna 7.7.4).
    // Rewritten by the CPU every frame, so it lives where the constant
    // buffer does and for the same reasons: an UPLOAD heap, mapped for
    // good, and one per frame resource so the CPU never writes the vertices
    // an earlier frame's draw is still reading.
    ID3D12Resource *WavesVertexBuffer;
    struct model_vertex *WavesVerticesMapped;

    // The fence value signalled after this frame's commands: once the GPU's
    // fence has reached it, this frame resource is free to reuse.  0 = never
    // used yet, so nothing to wait for.
    u64 FenceValue;
};

// Luna's vertex (8.14.1): everything about one vertex together, in one
// vertex buffer - "interleaved".  44 bytes; the offsets in ModelVertexDesc
// are where each field starts.
struct model_vertex
{
    XMFLOAT3 Pos;        // 12 bytes, at byte 0
    XMFLOAT3 Normal;     // 12 bytes, at byte 12
    XMFLOAT2 TexC;       //  8 bytes, at byte 24 - chapter 9 (textures)
    XMFLOAT3 TangentU;   // 12 bytes, at byte 32 - chapter 19 (normal maps)
};

// Fail the build if the compiler ever puts a field somewhere else than the
// input layout says.
static_assert(offsetof(model_vertex, Normal) == 12, "model_vertex: Normal moved");
static_assert(offsetof(model_vertex, TexC) == 24, "model_vertex: TexC moved");
static_assert(offsetof(model_vertex, TangentU) == 32, "model_vertex: TangentU moved");
static_assert(sizeof(model_vertex) == 44, "model_vertex: size changed");

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


#include "mesh_gen.h"
#define MESH_MAX_SUBMESHES 16

// Geometry on the GPU: the buffers, the views that describe them, and the parts to draw.
// Fixed-size array so a mesh needs no allocation.
struct mesh
{
    // One vertex buffer of model_vertexes (slot 0), and the index buffer.
    // DEFAULT heap (VRAM): fastest for the GPU to read.  (The water's vertex
    // buffer is the exception: 0 here, its view re-pointed every frame at
    // the frame resource's own.)
    ID3D12Resource *VertexBuffer;
    ID3D12Resource *IndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexView;    // built once, since the buffers never move
    D3D12_INDEX_BUFFER_VIEW IndexView;

    u32 SubmeshCount;
    submesh Submeshes[MESH_MAX_SUBMESHES];
};

// The shapes in ShapesMesh: each one's number is its place in Submeshes.
enum shape_id
{
    SHAPE_CUBE,
    SHAPE_PYRAMID,
    SHAPE_FLOOR,
    SHAPE_CYLINDER,
    SHAPE_CONE,
    SHAPE_GEOSPHERE,
    SHAPE_LAND,

    SHAPE_COUNT
};

// Render items (Luna 7.6.2): the scene as a list - one entry per object to
// draw, saying which shape, where, and which layer.  Built once by
// BuildRenderItems; drawn every frame by DrawRenderItems.
//
// A layer is a group of items drawn with the same pipeline, so the
// pipeline is set once per layer instead of once per item.
enum render_layer
{
    RENDER_LAYER_ANIMATED,     // AnimatedPipeline: wobble and pulse
    RENDER_LAYER_STILL,        // StillPipeline: still and plain

    RENDER_LAYER_COUNT
};

struct render_item
{
    mesh *Mesh;                // whose buffers: ShapesMesh or WavesMesh (Luna's Geo)
    u32 Submesh;               // which part of them - for ShapesMesh, a shape_id
    XMFLOAT4X4 World;          // where it is - XMFLOAT4X4 to store, XMMATRIX to calculate
    render_layer Layer;        // which pipeline draws it
};

#define MAX_RENDER_ITEMS 256

// Collects shapes into one set of arrays, so they all go to the GPU as one
// vertex buffer and one index buffer.  Each Add copies a shape onto the end
// and returns its submesh - where it landed.
#define SHAPE_MAX_VERTICES 65536
#define SHAPE_MAX_INDICES (3*65536)
struct shape_builder
{
    model_vertex *Vertices;
    u16 *Indices;
    u32 VertexCount;
    u32 IndexCount;
    u32 MaxVertices;
    u32 MaxIndices;
};

internal shape_builder
BeginShapes(memory_arena *Arena, u32 MaxVertices, u32 MaxIndices)
{
    shape_builder Result = {};
    Result.Vertices = PushArray(Arena, MaxVertices, model_vertex);
    Result.Indices = PushArray(Arena, MaxIndices, u16);
    Result.MaxVertices = MaxVertices;
    Result.MaxIndices = MaxIndices;
    return(Result);
}

// A shape made in clash's own vertex format, and indices counting from its
// own vertex 0 - the submesh's BaseVertex moves them.
internal submesh
AddShape(shape_builder *Builder, model_vertex *Vertices, u32 VertexCount,
         u16 *Indices, u32 IndexCount)
{
    Assert((Builder->VertexCount + VertexCount) <= Builder->MaxVertices);
    Assert((Builder->IndexCount + IndexCount) <= Builder->MaxIndices);

    submesh Result;
    Result.IndexCount = IndexCount;
    Result.StartIndex = Builder->IndexCount;
    Result.BaseVertex = (i32)Builder->VertexCount;

    memcpy(Builder->Vertices + Builder->VertexCount, Vertices, VertexCount*sizeof(model_vertex));
    memcpy(Builder->Indices + Builder->IndexCount, Indices, IndexCount*sizeof(u16));
    Builder->VertexCount += VertexCount;
    Builder->IndexCount += IndexCount;

    return(Result);
}

// One MeshGen vertex in clash's vertex format: the same four things,
// mesh_gen_vertex just keeps them in a different order.
internal model_vertex
ModelVertexFromMeshGen(mesh_gen_vertex *Source)
{
    model_vertex Result;
    Result.Pos = Source->Position;
    Result.Normal = Source->Normal;
    Result.TexC = Source->TexC;
    Result.TangentU = Source->TangentU;
    return(Result);
}

// A shape from MeshGen: its vertices converted, its indices copied.
internal submesh
AddGeneratedShape(shape_builder *Builder, mesh_gen_data *Shape)
{
    Assert((Builder->VertexCount + Shape->VertexCount) <= Builder->MaxVertices);
    Assert((Builder->IndexCount + Shape->IndexCount) <= Builder->MaxIndices);

    submesh Result;
    Result.IndexCount = Shape->IndexCount;
    Result.StartIndex = Builder->IndexCount;
    Result.BaseVertex = (i32)Builder->VertexCount;

    for(u32 VertexIndex = 0; VertexIndex < Shape->VertexCount; ++VertexIndex)
    {
        Builder->Vertices[Builder->VertexCount + VertexIndex] = ModelVertexFromMeshGen(&Shape->Vertices[VertexIndex]);
    }
    memcpy(Builder->Indices + Builder->IndexCount, Shape->Indices, Shape->IndexCount*sizeof(u16));
    Builder->VertexCount += Shape->VertexCount;
    Builder->IndexCount += Shape->IndexCount;

    return(Result);
}

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

    // CPU records commands into the list (memory from an allocator); the queue
    // runs closed lists on the GPU.  This allocator is for one-off work at
    // startup (uploads) - each frame records into its own frame resource's.
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

    // The scene: how its shaders take input, the two pipelines, and its geometry.
    ID3D12RootSignature *RootSignature;
    ID3D12PipelineState *AnimatedPipeline;
    ID3D12PipelineState *StillPipeline;     // same root signature, floor.hlsl: no wobble, no pulse
    mesh ShapesMesh;
    mesh WavesMesh;     // the water: static colours and indices, positions per frame resource
    u32 WavesVertexCount;
    // One set of per-frame things for each frame in flight, used in turn.
    // FrameIndex is the one the frame being recorded uses.
    frame_resource Frames[FRAME_RESOURCE_COUNT];
    u32 FrameIndex;
    u32 ObjectSlotsUsed;            // this frame's, so far - DrawShape takes the next one

    // The scene: everything to draw, filled once by BuildRenderItems.  The
    // boxes are BOX_COUNT items from FirstBoxItem on - their World comes
    // from the game, every frame.
    render_item RenderItems[MAX_RENDER_ITEMS];
    u32 RenderItemCount;
    u32 FirstBoxItem;

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

// Builds everything the scene needs, once at startup: the root signature,
// the shaders and both pipelines (animated, still), every shape's geometry
// in VRAM (ShapesMesh) plus the water's static parts (WavesMesh), and each
// frame resource's constant buffer and water position buffer.
internal b32
CreateScene(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory, memory_arena *Scratch)
{
    // ------------------------------------------------------------------
    // 1. Root signature: tells the pipeline the shaders take two constant
    //    buffers, b0 and b1, each passed as a plain GPU address (root CBV)
    //    so no descriptor heap is needed.  The still pipeline uses this
    //    same root signature: its shader reads the same b0 and b1.
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
                                                          IID_PPV_ARGS(&Renderer->RootSignature)));
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
    //    shader's inputs.  Luna's, for model_vertex (8.14.1): all four from
    //    one buffer (slot 0), each at its byte offset in the vertex.  A
    //    shader may read only some of them - ours read POSITION and NORMAL.
    // ------------------------------------------------------------------
    D3D12_INPUT_ELEMENT_DESC ModelVertexDesc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // ------------------------------------------------------------------
    // 4. Pipeline state object: all draw settings baked into one object, so
    //    the driver validates and compiles them once instead of per draw.
    // ------------------------------------------------------------------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC PipelineDesc = {};
    PipelineDesc.pRootSignature = Renderer->RootSignature;
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
    PipelineDesc.InputLayout.pInputElementDescs = ModelVertexDesc;
    PipelineDesc.InputLayout.NumElements = (UINT)ArrayCount(ModelVertexDesc);
    PipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PipelineDesc.NumRenderTargets = 1;
    PipelineDesc.RTVFormats[0] = SWAP_CHAIN_FORMAT;
    PipelineDesc.DSVFormat = DEPTH_FORMAT;
    PipelineDesc.SampleDesc.Count = 1;
    PipelineDesc.SampleDesc.Quality = 0;

    // The PSO keeps its own copy of the shaders, so the blobs can be released.
    b32 NoPipeline =
        DX12_FAILED(Renderer->Device->CreateGraphicsPipelineState(&PipelineDesc,
                                                                  IID_PPV_ARGS(&Renderer->AnimatedPipeline)));
    ReleaseAndZero(VertexShader);
    ReleaseAndZero(PixelShader);
    if(NoPipeline)
    {
        return(false);
    }

    // The still pipeline: the same description with floor.hlsl's shaders
    // swapped in.  Shaders are baked into a PSO, so a different shader means
    // a different PSO - switched with SetPipelineState before the still layer.
    IDxcBlob *StillVertexShader = CompileShader(Renderer, Thread, Memory,
                                                L"data\\shaders\\floor.hlsl", L"VS", L"vs_6_6");
    IDxcBlob *StillPixelShader = CompileShader(Renderer, Thread, Memory,
                                               L"data\\shaders\\floor.hlsl", L"PS", L"ps_6_6");
    if(!StillVertexShader || !StillPixelShader)
    {
        ReleaseAndZero(StillVertexShader);
        ReleaseAndZero(StillPixelShader);
        return(false);
    }
    PipelineDesc.VS.pShaderBytecode = StillVertexShader->GetBufferPointer();
    PipelineDesc.VS.BytecodeLength = StillVertexShader->GetBufferSize();
    PipelineDesc.PS.pShaderBytecode = StillPixelShader->GetBufferPointer();
    PipelineDesc.PS.BytecodeLength = StillPixelShader->GetBufferSize();

    b32 NoStillPipeline =
        DX12_FAILED(Renderer->Device->CreateGraphicsPipelineState(&PipelineDesc,
                                                                  IID_PPV_ARGS(&Renderer->StillPipeline)));
    ReleaseAndZero(StillVertexShader);
    ReleaseAndZero(StillPixelShader);
    if(NoStillPipeline)
    {
        return(false);
    }

    // ------------------------------------------------------------------
    // 5. Two shapes typed by hand: the cube's 8 corners, then the
    //    pyramid's 5, each in its own local space (the World matrix moves
    //    them).  Positions only - their normals are worked out below.
    // ------------------------------------------------------------------
    XMFLOAT3 Positions[] =
    {
        // Cube: vertices 0-7.
        XMFLOAT3(-1.0f, -1.0f, -1.0f),    // 0
        XMFLOAT3(-1.0f, +1.0f, -1.0f),    // 1
        XMFLOAT3(+1.0f, +1.0f, -1.0f),    // 2
        XMFLOAT3(+1.0f, -1.0f, -1.0f),    // 3
        XMFLOAT3(-1.0f, -1.0f, +1.0f),    // 4
        XMFLOAT3(-1.0f, +1.0f, +1.0f),    // 5
        XMFLOAT3(+1.0f, +1.0f, +1.0f),    // 6
        XMFLOAT3(+1.0f, -1.0f, +1.0f),    // 7

        // Pyramid: vertices 8-12, numbered 0-4 in its own indices.
        XMFLOAT3(-1.0f, -1.0f, -1.0f),    // 0: base
        XMFLOAT3(+1.0f, -1.0f, -1.0f),    // 1: base
        XMFLOAT3(-1.0f, -1.0f, +1.0f),    // 2: base
        XMFLOAT3(+1.0f, -1.0f, +1.0f),    // 3: base
        XMFLOAT3(+0.0f, +1.0f, +0.0f),    // 4: tip
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

    // ------------------------------------------------------------------
    // 7. Every shape into one builder (scratch memory), each one's submesh
    //    kept under its shape_id.
    // ------------------------------------------------------------------
    shape_builder Builder = BeginShapes(Scratch, SHAPE_MAX_VERTICES, SHAPE_MAX_INDICES);
    mesh *Mesh = &Renderer->ShapesMesh;

    // The hand-written ones have no normals of their own: each vertex gets
    // the average of its triangles' face normals (8.2.1).  The cube is
    // vertices 0-7 and indices 0-35 of the arrays above, the pyramid the
    // rest.
    //
    // NOTE(yigit): The cube's 8 corners are each shared by 3 faces, so
    // each gets a diagonal normal and the lit cube looks rounded.  Luna's
    // box keeps sharp edges with its own 4 vertices per face (24 in all).
    XMFLOAT3 CubeNormals[8];
    XMFLOAT3 PyramidNormals[5];
    MeshGenComputeNormals(Positions, 8, Indices, 36, CubeNormals);
    MeshGenComputeNormals(Positions + 8, 5, Indices + 36, 18, PyramidNormals);

    // Into clash's vertex format: position and normal, the rest 0 until
    // chapters 9 and 19 need them.
    model_vertex CubeVertices[8] = {};
    for(u32 Index = 0; Index < 8; ++Index)
    {
        CubeVertices[Index].Pos = Positions[Index];
        CubeVertices[Index].Normal = CubeNormals[Index];
    }
    model_vertex PyramidVertices[5] = {};
    for(u32 Index = 0; Index < 5; ++Index)
    {
        PyramidVertices[Index].Pos = Positions[8 + Index];
        PyramidVertices[Index].Normal = PyramidNormals[Index];
    }
    Mesh->Submeshes[SHAPE_CUBE] = AddShape(&Builder, CubeVertices, 8, Indices, 36);
    Mesh->Submeshes[SHAPE_PYRAMID] = AddShape(&Builder, PyramidVertices, 5, Indices + 36, 18);

    // The MeshGen shapes come with normals (and texture coordinates and
    // tangents) already.
    // The floor: 120 x 120 units, 21 x 21 vertices = 20 x 20 cells.
    mesh_gen_data Grid = MeshGenGrid(Scratch, 120.0f, 120.0f, 21, 21);
    Mesh->Submeshes[SHAPE_FLOOR] = AddGeneratedShape(&Builder, &Grid);

    // A cylinder: radius 1 at the bottom, 0.5 at the top, 3 tall, 20 slices
    // around, 4 stacks up.
    mesh_gen_data Cylinder = MeshGenCylinder(Scratch, 1.0f, 0.5f, 3.0f, 20, 4);
    Mesh->Submeshes[SHAPE_CYLINDER] = AddGeneratedShape(&Builder, &Cylinder);

    // A cone: a cylinder whose top radius is 0.
    mesh_gen_data Cone = MeshGenCylinder(Scratch, 1.0f, 0.0f, 3.0f, 20, 4);
    Mesh->Submeshes[SHAPE_CONE] = AddGeneratedShape(&Builder, &Cone);

    mesh_gen_data Geosphere = MeshGenGeosphere(Scratch, 1.0f, 3);
    Mesh->Submeshes[SHAPE_GEOSPHERE] = AddGeneratedShape(&Builder, &Geosphere);

    // The land (Luna 7.7.3): a grid lifted by HillsHeight.  Lifting it
    // tilts every surface, so the grid's straight-up normals are wrong -
    // the right ones come from the hills' slopes, HillsNormal (8.14.2).
    // (Its colour-by-height is gone: in Luna's LitWaves the land's colour
    // comes from its material.)
    mesh_gen_data Land = MeshGenGrid(Scratch, 160.0f, 160.0f, 50, 50);
    model_vertex *LandVertices = PushArray(Scratch, Land.VertexCount, model_vertex);
    for(u32 Index = 0; Index < Land.VertexCount; ++Index)
    {
        LandVertices[Index] = ModelVertexFromMeshGen(&Land.Vertices[Index]);
        LandVertices[Index].Pos.y = HillsHeight(LandVertices[Index].Pos.x, LandVertices[Index].Pos.z);
        LandVertices[Index].Normal = HillsNormal(LandVertices[Index].Pos.x, LandVertices[Index].Pos.z);
    }
    Mesh->Submeshes[SHAPE_LAND] = AddShape(&Builder, LandVertices, Land.VertexCount,
                                           Land.Indices, Land.IndexCount);
    Mesh->SubmeshCount = SHAPE_COUNT;

    // Upload the vertices and indices to VRAM.
    u32 VertexCount = Builder.VertexCount;
    u32 IndexCount = Builder.IndexCount;
    if(!CreateStaticBuffer(Renderer, Thread, Memory, Builder.Vertices, 
                VertexCount*sizeof(model_vertex), &Mesh->VertexBuffer) ||
       !CreateStaticBuffer(Renderer, Thread, Memory, Builder.Indices, IndexCount*sizeof(u16), &Mesh->IndexBuffer))
    {
        return(false);
    }

    // Vertex view: address, total size (every shape's vertices), and
    // stride - 44 bytes from one vertex to the next.
    Mesh->VertexView.BufferLocation = Mesh->VertexBuffer->GetGPUVirtualAddress();
    Mesh->VertexView.SizeInBytes = VertexCount*(UINT)sizeof(model_vertex);
    Mesh->VertexView.StrideInBytes = (UINT)sizeof(model_vertex);

    // Index view: address, size, and 16-bit format to match the u16 array.
    Mesh->IndexView.BufferLocation = Mesh->IndexBuffer->GetGPUVirtualAddress();
    Mesh->IndexView.SizeInBytes = IndexCount*(UINT)sizeof(u16);
    Mesh->IndexView.Format = DXGI_FORMAT_R16_UINT;

    // The water's static part: its indices.  A grid laid out like the waves
    // (128 x 128 points, 1 apart), used only for those.  Its vertices change
    // every frame, so they live in the frame resources instead, and its
    // vertex view is pointed at this frame's every frame.
    mesh_gen_data Water = MeshGenGrid(Scratch, 127.0f, 127.0f, 128, 128);
    mesh *WavesMesh = &Renderer->WavesMesh;
    if(!CreateStaticBuffer(Renderer, Thread, Memory, Water.Indices, Water.IndexCount*sizeof(u16), &WavesMesh->IndexBuffer))
    {
        return(false);
    }

    // Indices: 16-bit, like the shapes'.
    WavesMesh->IndexView.BufferLocation = WavesMesh->IndexBuffer->GetGPUVirtualAddress();
    WavesMesh->IndexView.SizeInBytes = Water.IndexCount*(UINT)sizeof(u16);
    WavesMesh->IndexView.Format = DXGI_FORMAT_R16_UINT;

    // Kept for the per-frame vertex buffers, made below with the constant
    // buffers.
    Renderer->WavesVertexCount = Water.VertexCount;

    // One part: the whole grid.
    WavesMesh->SubmeshCount = 1;
    WavesMesh->Submeshes[0].IndexCount = Water.IndexCount;
    WavesMesh->Submeshes[0].StartIndex = 0;
    WavesMesh->Submeshes[0].BaseVertex = 0;

    // Everything pushed on Scratch above is on the GPU now.
    ResetArena(Scratch);

    // ------------------------------------------------------------------
    // 8. Constant buffers, one per frame resource: 256-byte slots - the
    //    pass, then one per object - in an UPLOAD heap because the CPU
    //    rewrites them every frame.  Mapped once and left mapped, which
    //    upload heaps allow.
    // ------------------------------------------------------------------
    D3D12_HEAP_PROPERTIES UploadHeap = DescribeHeap(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC ConstantDesc = DescribeBuffer(CONSTANT_BUFFER_SLOT_COUNT*CONSTANT_BUFFER_SLOT_SIZE);
    for(u32 FrameIndex = 0; FrameIndex < FRAME_RESOURCE_COUNT; ++FrameIndex)
    {
        frame_resource *Frame = &Renderer->Frames[FrameIndex];
        if(DX12_FAILED(Renderer->Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &ConstantDesc,
                                                                 D3D12_RESOURCE_STATE_GENERIC_READ, 0,
                                                                 IID_PPV_ARGS(&Frame->ConstantBuffer))))
        {
            return(false);
        }

        // Empty read range: the CPU only writes.
        D3D12_RANGE NoRead = {0, 0};
        if(DX12_FAILED(Frame->ConstantBuffer->Map(0, &NoRead, (void **)&Frame->ConstantBufferMapped)))
        {
            return(false);
        }

        // The water's vertices: made and mapped the same way, sized for
        // every water vertex.
        D3D12_RESOURCE_DESC WavesDesc = DescribeBuffer(Renderer->WavesVertexCount*sizeof(model_vertex));
        if(DX12_FAILED(Renderer->Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &WavesDesc,
                                                                 D3D12_RESOURCE_STATE_GENERIC_READ, 0,
                                                                 IID_PPV_ARGS(&Frame->WavesVertexBuffer))) ||
           DX12_FAILED(Frame->WavesVertexBuffer->Map(0, &NoRead, (void **)&Frame->WavesVerticesMapped)))
        {
            return(false);
        }
    }

    return(true);
}

// Adds one object to the scene: whose buffers, which part of them, where,
// and in which layer.  Returns its place in the list, for items that are
// moved later.
internal u32
AddMeshRenderItem(dx12_renderer *Renderer, mesh *Mesh, u32 Submesh, FXMMATRIX World, render_layer Layer)
{
    Assert(Renderer->RenderItemCount < MAX_RENDER_ITEMS);
    u32 Result = Renderer->RenderItemCount++;

    render_item *Item = &Renderer->RenderItems[Result];
    Item->Mesh = Mesh;
    Item->Submesh = Submesh;
    XMStoreFloat4x4(&Item->World, World);
    Item->Layer = Layer;

    return(Result);
}

// The same, for one of ShapesMesh's shapes - most of the scene.
internal u32
AddRenderItem(dx12_renderer *Renderer, shape_id Shape, FXMMATRIX World, render_layer Layer)
{
    u32 Result = AddMeshRenderItem(Renderer, &Renderer->ShapesMesh, Shape, World, Layer);
    return(Result);
}

// The scene (Luna's BuildRenderItems).  Runs once, at startup - after a
// change here, restart clash: a hot reload keeps the old list.
internal void
BuildRenderItems(dx12_renderer *Renderer)
{
    Renderer->RenderItemCount = 0;

    // The 3x3 boxes: placeholders here - RendererRender overwrites their
    // World every frame with the game's.
    Renderer->FirstBoxItem = Renderer->RenderItemCount;
    for(u32 BoxIndex = 0; BoxIndex < BOX_COUNT; ++BoxIndex)
    {
        AddRenderItem(Renderer, SHAPE_CUBE, XMMatrixIdentity(), RENDER_LAYER_ANIMATED);
    }

    // The land, 5 below the origin.
    AddRenderItem(Renderer, SHAPE_LAND, XMMatrixTranslation(0.0f, -5.0f, 0.0f), RENDER_LAYER_STILL);

    // The water, at the same height as the land, so it fills the valleys.
    // Its own mesh - WavesMesh, part 0 - since its positions change.
    AddMeshRenderItem(Renderer, &Renderer->WavesMesh, 0, XMMatrixTranslation(0.0f, -5.0f, 0.0f),
                      RENDER_LAYER_STILL);

    // A row of 5 cylinders behind the boxes, standing on the floor (3 tall
    // and centred, so their middle is 1.5 above -5).
    for(u32 CylinderIndex = 0; CylinderIndex < 5; ++CylinderIndex)
    {
        AddRenderItem(Renderer, SHAPE_CYLINDER,
                      XMMatrixTranslation((r32)CylinderIndex*3.0f - 6.0f, -3.5f, 9.0f),
                      RENDER_LAYER_STILL);
    }

    // In front of the boxes: the pyramid and the cone on the floor, the
    // geosphere above the cone.
    AddRenderItem(Renderer, SHAPE_PYRAMID, XMMatrixTranslation(0.0f, -4.0f, -10.0f), RENDER_LAYER_STILL);
    AddRenderItem(Renderer, SHAPE_CONE, XMMatrixTranslation(0.0f, -3.5f, -5.0f), RENDER_LAYER_STILL);
    AddRenderItem(Renderer, SHAPE_GEOSPHERE, XMMatrixTranslation(0.0f, 3.5f, -5.0f), RENDER_LAYER_STILL);
}

// Starts DirectX 12 on the given window: device, queue, swap chain, heaps,
// depth buffer, then the scene.  False if DirectX 12 can't start.
internal b32
RendererInitialize(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
                   HWND Window, i32 Width, i32 Height, memory_arena *Scratch)
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

    // One more allocator per frame resource, so a frame can record while the
    // GPU still runs the commands in the previous frames' allocators.
    for(u32 FrameIndex = 0; FrameIndex < FRAME_RESOURCE_COUNT; ++FrameIndex)
    {
        if(DX12_FAILED(Renderer->Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                IID_PPV_ARGS(&Renderer->Frames[FrameIndex].CommandAllocator))))
        {
            RendererShutdown(Renderer);
            return(false);
        }
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

    // A missing compiler or scene isn't fatal: the game runs with just the clear colour.
    if(!LoadShaderCompiler(Renderer, Thread, Memory))
    {
        RendererLog(Thread, Memory, "ERROR: No shader compiler - nothing will be drawn.\n");
    }
    else if(!CreateScene(Renderer, Thread, Memory, Scratch))
    {
        RendererLog(Thread, Memory, "ERROR: No scene - see the errors above.\n");
    }
    else
    {
        BuildRenderItems(Renderer);
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

// Sleeps until the GPU's fence has reached Value - until the commands
// signalled with it have run.  Returns at once if it already has (and for 0,
// which every fence starts past).
internal void
WaitForFence(dx12_renderer *Renderer, u64 Value)
{
    if(Renderer->Fence->GetCompletedValue() < Value)
    {
        Renderer->Fence->SetEventOnCompletion(Value, Renderer->FenceEvent);
        WaitForSingleObject(Renderer->FenceEvent, INFINITE);
    }
}

// Draws part Submesh of Mesh at World: writes World to the next free object
// slot of this frame's constant buffer, points b0 at that slot, and draws
// that part of the buffers.  The pipeline, b1 and Mesh's buffers must
// already be set.
internal void
DrawShape(dx12_renderer *Renderer, mesh *Mesh, u32 Submesh, FXMMATRIX World)
{
    Assert(Renderer->ObjectSlotsUsed < OBJECT_SLOT_COUNT);
    u32 Slot = FIRST_OBJECT_SLOT + Renderer->ObjectSlotsUsed++;
    frame_resource *Frame = &Renderer->Frames[Renderer->FrameIndex];

    object_constants Object;
    XMStoreFloat4x4(&Object.World, XMMatrixTranspose(World));
    memcpy(Frame->ConstantBufferMapped + Slot * CONSTANT_BUFFER_SLOT_SIZE,
           &Object, sizeof(Object));
    Renderer->CommandList->SetGraphicsRootConstantBufferView(0,
            Frame->ConstantBuffer->GetGPUVirtualAddress() + Slot * CONSTANT_BUFFER_SLOT_SIZE);
    submesh *Part = &Mesh->Submeshes[Submesh];
    Renderer->CommandList->DrawIndexedInstanced(Part->IndexCount, 1, Part->StartIndex, Part->BaseVertex, 0);
}

// Draws every render item in Layer.  The layer's pipeline must already be
// set.  Like Luna's DrawRenderItems, each item binds its own mesh's buffers -
// but only when they differ from the last item's, since most share ShapesMesh.
internal void
DrawRenderItems(dx12_renderer *Renderer, render_layer Layer)
{
    mesh *BoundMesh = 0;
    for(u32 ItemIndex = 0; ItemIndex < Renderer->RenderItemCount; ++ItemIndex)
    {
        render_item *Item = &Renderer->RenderItems[ItemIndex];
        if(Item->Layer == Layer)
        {
            if(Item->Mesh != BoundMesh)
            {
                Renderer->CommandList->IASetVertexBuffers(0, 1, &Item->Mesh->VertexView);
                Renderer->CommandList->IASetIndexBuffer(&Item->Mesh->IndexView);
                BoundMesh = Item->Mesh;
            }
            DrawShape(Renderer, Item->Mesh, Item->Submesh, XMLoadFloat4x4(&Item->World));
        }
    }
}

// Records and submits one frame: clear, draw the render items, draw ImGui, present.
// Worlds (the boxes) and ViewProj are row-vector matrices from the game.
internal void
RendererRender(dx12_renderer *Renderer, thread_context *Thread, game_memory *Memory,
               game_input *Input, const XMFLOAT4X4 *Worlds, const XMFLOAT4X4 *ViewProj, r32 Time,
               const waves *Waves)
{
    if(!Renderer->SwapChain || !Renderer->BackBuffers[0] || !Renderer->DepthBuffer)
    {
        return;
    }

    // On to the next frame resource.  The GPU may still be running the frame
    // that last used it, FRAME_RESOURCE_COUNT frames ago - wait until it has
    // finished.  Usually it already has, and this returns at once: the wait
    // only happens when the CPU gets too far ahead.
    Renderer->FrameIndex = (Renderer->FrameIndex + 1) % FRAME_RESOURCE_COUNT;
    frame_resource *Frame = &Renderer->Frames[Renderer->FrameIndex];
    WaitForFence(Renderer, Frame->FenceValue);

    // Now its allocator's memory and its constant buffer are free: reuse the
    // memory and start recording.
    if(DX12_FAILED(Frame->CommandAllocator->Reset()) ||
       DX12_FAILED(Renderer->CommandList->Reset(Frame->CommandAllocator, 0)))
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
    // Only if the scene was created successfully.
    if(Renderer->AnimatedPipeline && Frame->ConstantBufferMapped)
    {
        // The pass: camera and time, into slot 0 of this frame's constant
        // buffer, read by every draw through b1.
        // Transposed because HLSL reads constant-buffer matrices column by column.
        pass_constants Pass;
        XMStoreFloat4x4(&Pass.ViewProj, XMMatrixTranspose(XMLoadFloat4x4(ViewProj)));
        Pass.gTime = Time;
        Pass.PulseColor = {1.0f,0.0f,0.50f,1.0f};
        memcpy(Frame->ConstantBufferMapped + PASS_SLOT*CONSTANT_BUFFER_SLOT_SIZE, &Pass, sizeof(Pass));

        // Set once for every shape: the root signature and b1 at the pass
        // slot.  (The buffers are set per mesh, in DrawRenderItems.)
        Renderer->CommandList->SetGraphicsRootSignature(Renderer->RootSignature);
        Renderer->CommandList->SetGraphicsRootConstantBufferView(1,
                Frame->ConstantBuffer->GetGPUVirtualAddress() + PASS_SLOT*CONSTANT_BUFFER_SLOT_SIZE);
        // Every 3 indices form one triangle.
        Renderer->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // The water (Luna's UpdateWaves): this frame's wave positions into
        // this frame resource's vertex buffer, then the water's vertex view
        // pointed at that buffer - a different one each frame, in turn.
        //
        // NOTE(yigit): Every field written, every frame: this is an upload
        // heap, written straight into GPU-visible memory - nothing in it
        // can be left over from "last time" and trusted.
        Assert(Waves->VertexCount == Renderer->WavesVertexCount);
        for(u32 Index = 0; Index < Waves->VertexCount; ++Index)
        {
            model_vertex *Vertex = &Frame->WavesVerticesMapped[Index];
            Vertex->Pos = Waves->Current[Index];
            // TODO(yigit): A5 - the waves' own normals.  Straight up until then.
            Vertex->Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            Vertex->TexC = XMFLOAT2(0.0f, 0.0f);
            Vertex->TangentU = XMFLOAT3(1.0f, 0.0f, 0.0f);
        }

        D3D12_VERTEX_BUFFER_VIEW *WavesView = &Renderer->WavesMesh.VertexView;
        WavesView->BufferLocation = Frame->WavesVertexBuffer->GetGPUVirtualAddress();
        WavesView->SizeInBytes = Renderer->WavesVertexCount*(UINT)sizeof(model_vertex);
        WavesView->StrideInBytes = (UINT)sizeof(model_vertex);

        // A new frame: every object slot is free again.
        Renderer->ObjectSlotsUsed = 0;

        // The boxes spin, so their World changes every frame: copy the
        // game's into their render items (Luna does this in UpdateObjectCBs).
        for(u32 BoxIndex = 0; BoxIndex < BOX_COUNT; ++BoxIndex)
        {
            Renderer->RenderItems[Renderer->FirstBoxItem + BoxIndex].World = Worlds[BoxIndex];
        }

        // Then the whole list, one layer at a time: its pipeline once, then
        // every item in it.
        Renderer->CommandList->SetPipelineState(Renderer->AnimatedPipeline);
        DrawRenderItems(Renderer, RENDER_LAYER_ANIMATED);

        Renderer->CommandList->SetPipelineState(Renderer->StillPipeline);
        DrawRenderItems(Renderer, RENDER_LAYER_STILL);
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

    // Mark the end of this frame's commands with a new fence value, and
    // remember it in the frame resource.  No waiting here any more: the CPU
    // goes straight on to the next frame, and only waits at the top of
    // RendererRender if it comes back round to this frame resource before
    // the GPU has reached the mark.
    Frame->FenceValue = ++Renderer->FenceValue;
    Renderer->Queue->Signal(Renderer->Fence, Frame->FenceValue);
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
    // Every frame resource's value is lower, so they are all free after this.
    ++Renderer->FenceValue;
    Renderer->Queue->Signal(Renderer->Fence, Renderer->FenceValue);
    WaitForFence(Renderer, Renderer->FenceValue);
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
    for(u32 FrameIndex = 0; FrameIndex < FRAME_RESOURCE_COUNT; ++FrameIndex)
    {
        frame_resource *Frame = &Renderer->Frames[FrameIndex];
        if(Frame->ConstantBuffer && Frame->ConstantBufferMapped)
        {
            Frame->ConstantBuffer->Unmap(0, 0);
            Frame->ConstantBufferMapped = 0;
        }
        ReleaseAndZero(Frame->ConstantBuffer);
        if(Frame->WavesVertexBuffer && Frame->WavesVerticesMapped)
        {
            Frame->WavesVertexBuffer->Unmap(0, 0);
            Frame->WavesVerticesMapped = 0;
        }
        ReleaseAndZero(Frame->WavesVertexBuffer);
    }
    ReleaseAndZero(Renderer->ShapesMesh.IndexBuffer);
    ReleaseAndZero(Renderer->ShapesMesh.VertexBuffer);
    ReleaseAndZero(Renderer->WavesMesh.IndexBuffer);
    ReleaseAndZero(Renderer->StillPipeline);
    ReleaseAndZero(Renderer->AnimatedPipeline);
    ReleaseAndZero(Renderer->RootSignature);
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
    for(u32 FrameIndex = 0; FrameIndex < FRAME_RESOURCE_COUNT; ++FrameIndex)
    {
        ReleaseAndZero(Renderer->Frames[FrameIndex].CommandAllocator);
    }
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
