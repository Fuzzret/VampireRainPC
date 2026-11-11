// Direct3D 12 backend. See d3d12_backend.h.
#include "d3d12_backend.h"
#include "../settings.h"
#include "../gpu.h"

#ifndef _WIN32

namespace d3d12
{
    bool Init(void*, const std::string&) { return false; }
    bool Active() { return false; }
    void LoadShader(uint32_t, const std::vector<uint32_t>&, uint64_t) {}
    void Draw(const xe::gpu::RegisterFile&, uint32_t, uint32_t, uint32_t) {}
    void Resolve(const xe::gpu::RegisterFile&, int32_t, int32_t, int32_t, int32_t) {}
    void Swap(const xe::gpu::RegisterFile&, uint32_t, uint32_t, uint32_t) {}
    void RequestCapture() {}
    void NoteCpuSync() {}
    void SetRenderSize(uint32_t, uint32_t) {}
    void SetFrameRate(uint32_t) {}
    void RequestTextureDump() {}
    void ReloadModTextures() {}
    bool Capturing() { return false; }
}

#else

#include "../kernel.h"
#include "../memory.h"
#include "../shader_translator.h"
#include "hlsl_pixel.h"
#include "hlsl_sources.h"
#include "texture_files.h"

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define XE_HAVE_DXC 1
#else
#define XE_HAVE_DXC 0
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
using namespace xe::gpu;

namespace d3d12
{
    namespace
    {
        // ------------------------------------------------------------------
        // Configuration
        // ------------------------------------------------------------------
        // Frames in flight. Three, so that with in-between frames (frame
        // rates above 30) one frame can be shown while the next waits and a
        // third is being recorded.
        constexpr uint32_t kFrames = 3;
        constexpr uint64_t kUploadSize = 96ull << 20;       // per frame
        constexpr uint64_t kSharedSize = 512ull << 20;      // guest physical memory
        constexpr uint64_t kEdramBaseSize = 2048ull * 5120; // 10 MB at 1x
        // Render resolution scale (launcher setting): EDRAM samples, viewport
        // and resolved surfaces are this many times wider and taller.
        uint32_t g_resScale = 1;
        uint64_t g_edramSize = kEdramBaseSize;
        // Pool for the scaled copies of resolved surfaces.
        constexpr uint64_t kScaledPoolBase = 48ull << 20; // x scale^2
        constexpr uint64_t kStagingSize = 64ull << 20;      // texture untiling
        constexpr uint32_t kPageShift = 12;
        constexpr uint32_t kPageCount = uint32_t(kSharedSize >> kPageShift);
        // Shader-visible views: split between the frames in flight, so a frame
        // reuses its half once the GPU has finished the frame before it.
        constexpr uint32_t kViewDescriptors = 786432;
        constexpr uint32_t kSamplers = 2048;                 // distinct samplers (the D3D12 limit)
        constexpr uint32_t kSysSlots = 85;

        // Static descriptors at the start of the shader-visible heap.
        enum : uint32_t
        {
            kDescSharedSrv = 0,  // draw table: t0 shared, u0 EDRAM (ROV)
            kDescEdramUavDraw = 1,
            kDescEdramUav = 2,   // compute table: u0 EDRAM, u1 shared, u2 staging
            kDescSharedUav = 3,
            kDescStagingUav = 4,
            kDescScaledUav = 5,  // u3: scaled resolved surfaces
            kDescNullTex = 6,    // scratch null SRV
            kDescPresent = 8,    // front buffer view per frame slot (in-between frames)
            kDescStatic = 16,
        };

        // Root parameter indices (draw).
        // kRootVsPrev: the draw's vertex shader constants in the previous game
        // frame, kRootInterp: the blend factor (see "In-between frames").
        enum : uint32_t { kRootSys, kRootVsConsts, kRootPsConsts, kRootMisc, kRootMemory, kRootTextures, kRootSamplers, kRootVsPrev, kRootInterp, kRootCount };

        struct Shared
        {
            ComPtr<ID3D12Device> device;
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGISwapChain3> swapChain;
            HWND hwnd = nullptr;
            uint32_t swapW = 0, swapH = 0;
            ComPtr<ID3D12DescriptorHeap> rtvHeap;
            ComPtr<ID3D12Resource> backBuffers[kFrames];

            ComPtr<ID3D12CommandAllocator> allocators[kFrames];
            ComPtr<ID3D12GraphicsCommandList> lists[kFrames];
            ComPtr<ID3D12GraphicsCommandList> list; // lists[frame]
            ComPtr<ID3D12Fence> fence;
            HANDLE fenceEvent = nullptr;
            uint64_t fenceValue = 0;
            uint64_t frameFence[kFrames] = {};
            uint32_t frame = 0;          // index into per-frame resources
            uint64_t frameNumber = 0;

            ComPtr<ID3D12Resource> upload[kFrames];
            uint8_t* uploadPtr[kFrames] = {};
            uint64_t uploadUsed = 0;

            ComPtr<ID3D12Resource> shared, edram, staging;
            D3D12_RESOURCE_STATES sharedState = D3D12_RESOURCE_STATE_COMMON;

            ComPtr<ID3D12DescriptorHeap> viewHeap, samplerHeap, cpuViewHeap, cpuSamplerHeap;
            uint32_t viewIncrement = 0, samplerIncrement = 0;
            uint32_t viewRingPos = kDescStatic;
            // Draw sequence numbers: what the GPU has certainly finished
            // (sampler tables last used at or before completedSeq can be reused).
            uint64_t useSeq = 0, completedSeq = 0, frameSeq[kFrames] = {};
            uint32_t flushCount = 0;   // FlushAndReset calls (allocations made before one are void)
            uint32_t flushes = 0;      // ... since the last report
            uint32_t cpuViewNext = 32; // slots 0..31: null texture SRVs (a ready-made empty table)
            std::vector<uint32_t> cpuViewFree;

            ComPtr<ID3D12RootSignature> drawRoot, computeRoot, presentRoot;
            ComPtr<ID3D12PipelineState> resolvePso, resolveScaledPso, clearPso, untilePso, presentPso;
            ComPtr<ID3D12Resource> scaledPool;
            uint64_t scaledPoolSize = 0;

            std::string cacheDir;
            pD3DCompile compile = nullptr;
            bool tearing = false;      // present without waiting for vsync
            // Timing (microseconds, reset every report).
            double tPresent = 0, tWait = 0, tHash = 0, tDraw = 0, tResolve = 0, tTex = 0;
            uint32_t hashedPages = 0;
        } g;

        bool g_active = false;
        std::atomic<bool> g_captureRequest{ false };
        std::atomic<int64_t> g_frameRateRequest{ -1 }; // in-game menu: new frame rate, applied between frames
        bool g_capturing = false;
        struct Texture;
        struct CaptureTexture { Texture* tex; uint32_t guestFormat; uint32_t address; };
        std::vector<CaptureTexture> g_captureTextures;
        uint32_t g_captureIndex = 0;

        // ------------------------------------------------------------------
        // In-between frames (frame rates above 30)
        //
        // The game advances one step per frame at 30 fps, so it can't simply
        // run faster. Instead each game frame is recorded into one command
        // list without presenting, and a presenter thread executes that list
        // several times: in-between frames first, then the real one. Vertex
        // shaders blend each constant from its value in the matching draw of
        // the previous game frame (same shaders, buffers and target, in the
        // same order) by a factor read from g_interpBuf, which the presenter
        // sets before each execution - moving positions, the camera and
        // bones along. Shown frames trail the game by about one game frame.
        // ------------------------------------------------------------------
        struct InterpState
        {
            bool on = false;           // frame rate setting above 30
            std::atomic<uint32_t> rate{ 0 }; // target presents per second (0 = unlimited)
            ComPtr<ID3D12Resource> buf;      // float4 { t, 0, 0, 0 }, read by vertex shaders
            ComPtr<ID3D12Resource> tUpload;  // one 256-byte slot per presenter unit
            uint8_t* tPtr = nullptr;
            // Constants of the previous and current game frame by draw key.
            // Grouped by draw signature, in draw order. Draws of the same mesh
            // (roof tiles, crowds) are told apart by their constants.
            struct Entry { uint32_t offset, bytes; bool used; };
            std::unordered_map<uint64_t, std::vector<Entry>> prev, cur;
            std::vector<uint8_t> prevData, curData;
            std::unordered_map<uint64_t, uint32_t> occurrences; // this frame, by draw signature
            uint32_t matched = 0, cuts = 0;  // this frame
            uint32_t matchedSum = 0, cutsSum = 0; // since the last report
            bool replayable = true;          // no mid-frame submission this frame
            D3D12_RESOURCE_STATES frameStartShared = D3D12_RESOURCE_STATE_COMMON;

            // Presenter
            struct Job
            {
                ID3D12GraphicsCommandList* scene = nullptr;
                uint32_t slot = 0;
                bool replay = false, front = false;
                uint32_t swizzle = 0, aa = 0, width = 1280, height = 720, frontW = 1, frontH = 1;
                std::chrono::steady_clock::time_point arrival;
            };
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<Job> jobs;
            bool busy = false;
            bool slotPending[kFrames] = {};
            uint64_t slotFence[kFrames] = {};
            ComPtr<ID3D12Fence> fence;       // presenter's own fence
            HANDLE fenceEvent = nullptr;
            uint64_t fenceValue = 0;
            static constexpr uint32_t kUnits = 4;
            struct Unit
            {
                ComPtr<ID3D12CommandAllocator> alloc;
                ComPtr<ID3D12GraphicsCommandList> pre, post;
                uint64_t fence = 0;
            } units[kUnits];
            uint32_t unitNext = 0;
            std::atomic<uint32_t> presents{ 0 }, interpolated{ 0 }, unblended{ 0 };
            std::atomic<uint64_t> gpuWaitUs{ 0 }, presentUs{ 0 }; // presenter: waiting for the GPU / in Present
        } g_interp;

        // ------------------------------------------------------------------
        // Helpers
        // ------------------------------------------------------------------
        // Fills blend/depth state with valid (disabled) values.
        void DefaultStates(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, UINT8 writeMask)
        {
            for (auto& rt : d.BlendState.RenderTarget)
            {
                rt.BlendEnable = FALSE;
                rt.LogicOpEnable = FALSE;
                rt.SrcBlend = D3D12_BLEND_ONE;
                rt.DestBlend = D3D12_BLEND_ZERO;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt.RenderTargetWriteMask = 0;
            }
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = writeMask;
            d.DepthStencilState.DepthEnable = FALSE;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            d.DepthStencilState.StencilEnable = FALSE;
            d.DepthStencilState.StencilReadMask = 0xFF;
            d.DepthStencilState.StencilWriteMask = 0xFF;
            D3D12_DEPTH_STENCILOP_DESC op = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
            d.DepthStencilState.FrontFace = op;
            d.DepthStencilState.BackFace = op;
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.RasterizerState.DepthClipEnable = TRUE;
            d.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            d.SampleDesc.Count = 1;
        }

        bool Check(HRESULT hr, const char* what)
        {
            if (SUCCEEDED(hr))
                return true;
            LOG("[d3d12] %s failed: 0x%08lX", what, (unsigned long)hr);
            if (hr == DXGI_ERROR_DEVICE_REMOVED && g.device)
                LOG("[d3d12] device removed reason 0x%08lX", (unsigned long)g.device->GetDeviceRemovedReason());
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE ViewCpu(uint32_t i)
        {
            auto h = g.viewHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i) * g.viewIncrement;
            return h;
        }
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGpu(uint32_t i)
        {
            auto h = g.viewHeap->GetGPUDescriptorHandleForHeapStart();
            h.ptr += UINT64(i) * g.viewIncrement;
            return h;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE CpuView(uint32_t i)
        {
            auto h = g.cpuViewHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i) * g.viewIncrement;
            return h;
        }
        uint32_t AllocCpuView()
        {
            if (!g.cpuViewFree.empty())
            {
                uint32_t i = g.cpuViewFree.back();
                g.cpuViewFree.pop_back();
                return i;
            }
            return g.cpuViewNext++;
        }

        void Barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            if (before == after)
                return;
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            g.list->ResourceBarrier(1, &b);
        }
        void UavBarrier(ID3D12Resource* res = nullptr)
        {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = res;
            g.list->ResourceBarrier(1, &b);
        }

        constexpr D3D12_RESOURCE_STATES kSharedReadState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER;
        void SharedTo(D3D12_RESOURCE_STATES s)
        {
            if (g.sharedState == s)
            {
                if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    UavBarrier(g.shared.Get());
                return;
            }
            Barrier(g.shared.Get(), g.sharedState, s);
            g.sharedState = s;
        }

        ComPtr<ID3D12Resource> CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = heap;
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = size;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            d.Flags = flags;
            ComPtr<ID3D12Resource> res;
            if (!Check(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&res)), "CreateCommittedResource(buffer)"))
                return nullptr;
            return res;
        }

        void SetHeaps()
        {
            ID3D12DescriptorHeap* heaps[] = { g.viewHeap.Get(), g.samplerHeap.Get() };
            g.list->SetDescriptorHeaps(2, heaps);
        }

        // Waits until the presenter has submitted every frame handed to it
        // (in-between frames); called before the recording thread submits work
        // itself, which must come after those frames.
        void DrainPresenter()
        {
            if (!g_interp.on)
                return;
            std::unique_lock lock(g_interp.mutex);
            g_interp.cv.wait(lock, [] { return g_interp.jobs.empty() && !g_interp.busy; });
        }

        // Waits until the GPU has finished all submitted work.
        void WaitIdle()
        {
            DrainPresenter();
            g.fenceValue++;
            g.queue->Signal(g.fence.Get(), g.fenceValue);
            if (g.fence->GetCompletedValue() < g.fenceValue)
            {
                g.fence->SetEventOnCompletion(g.fenceValue, g.fenceEvent);
                WaitForSingleObject(g.fenceEvent, INFINITE);
            }
            g.completedSeq = g.useSeq;
        }

        constexpr uint32_t kViewHalf = (kViewDescriptors - kDescStatic) / kFrames;
        uint32_t RingStart(uint32_t frame) { return kDescStatic + frame * kViewHalf; }
        void ResetViewCaches(); // tables of views cached in the ring (below)

        // Submits the open command list and waits for it; used when a per-frame
        // ring runs out of space in the middle of a frame.
        enum FlushReason { kFlushUpload, kFlushViews, kFlushSamplers, kFlushOther, kFlushReasons };
        uint32_t g_flushWhy[kFlushReasons] = {};
        void FlushAndReset(FlushReason why = kFlushOther)
        {
            g_flushWhy[why]++;
            // This frame is now partly submitted, so it can't be executed again
            // for in-between frames.
            DrainPresenter();
            g_interp.replayable = false;
            g.list->Close();
            ID3D12CommandList* lists[] = { g.list.Get() };
            g.queue->ExecuteCommandLists(1, lists);
            WaitIdle();
            g.allocators[g.frame]->Reset();
            g.list->Reset(g.allocators[g.frame].Get(), nullptr);
            SetHeaps();
            g.uploadUsed = 0;
            g.viewRingPos = RingStart(g.frame);
            ResetViewCaches();
            g_interp.frameStartShared = g.sharedState;
            g.flushCount++;
            g.flushes++;
        }

        // Allocates upload memory for this frame. Returns the CPU pointer and
        // the offset within the frame's upload buffer.
        uint8_t* UploadAlloc(uint64_t size, uint64_t align, uint64_t& offset)
        {
            uint64_t o = (g.uploadUsed + align - 1) & ~(align - 1);
            if (o + size > kUploadSize)
            {
                if (size > kUploadSize)
                    return nullptr;
                FlushAndReset(kFlushUpload);
                o = 0;
            }
            g.uploadUsed = o + size;
            offset = o;
            return g.uploadPtr[g.frame] + o;
        }
        D3D12_GPU_VIRTUAL_ADDRESS UploadGpu(uint64_t offset) { return g.upload[g.frame]->GetGPUVirtualAddress() + offset; }

        uint32_t AllocViews(uint32_t count)
        {
            if (g.viewRingPos + count > RingStart(g.frame) + kViewHalf)
                FlushAndReset(kFlushViews);
            uint32_t i = g.viewRingPos;
            g.viewRingPos += count;
            return i;
        }

        // ------------------------------------------------------------------
        // Guest memory mirror
        // ------------------------------------------------------------------
        struct Page
        {
            uint64_t hash = 0;
            uint64_t stamp = 0;     // bumped whenever the GPU copy changes
            uint64_t checked = 0;   // epoch of the last CPU comparison
            bool valid = false;
            bool gpu = false;       // last written by the GPU (hash is a sparse sample)
            uint64_t sparse = 0;    // sparse sample of the CPU contents at the last full check
            uint64_t checkedSync = 0; // g_syncEpoch of the last full comparison
        };
        std::vector<Page> g_pages;
        uint64_t g_epoch = 1, g_stamp = 1;
        // Bumped whenever the CPU may have written data that later commands
        // use (new commands submitted, or a wait on the CPU finished). Small
        // dynamic buffers are fully rechecked once per sync, not per draw.
        uint64_t g_syncEpoch = 1;
        uint64_t g_syncsThisPeriod = 0; // for the report
        // The size the game renders at (other aspect ratios, see aspect.cpp);
        // its HUD is still laid out for 1280x720.
        uint32_t g_renderW = 1280, g_renderH = 720;
        // Render-to-texture copies this frame: (generation, start, end).
        struct ResolveRange { uint64_t gen; uint32_t start, end; };
        std::vector<ResolveRange> g_resolveRanges;
        uint64_t g_resolveGen = 0;

        uint8_t* PhysPtr(uint32_t p) { return g_base + mem::PHYSICAL_BASE + (p & 0x1FFFFFFF); }

        uint64_t HashPage(const uint8_t* p)
        {
            // Four independent lanes so the multiplies overlap (about twice
            // as fast as two); the page is 512 64-bit words.
            const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
            uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
            for (int i = 0; i < 512; i += 4)
            {
                a = (a ^ q[i]) * 0xFF51AFD7ED558CCDull;
                b = (b ^ q[i + 1]) * 0xC4CEB9FE1A85EC53ull;
                c = (c ^ q[i + 2]) * 0xFF51AFD7ED558CCDull;
                d = (d ^ q[i + 3]) * 0xC4CEB9FE1A85EC53ull;
                a ^= a >> 29;
                b ^= b >> 31;
                c ^= c >> 29;
                d ^= d >> 31;
            }
            return a ^ (b * 0x94D049BB133111EBull) ^ (c * 0xBF58476D1CE4E5B9ull) ^ (d * 0x9E3779B97F4A7C15ull);
        }

        // Cheap sample of a page, used for pages the GPU wrote (render-to-
        // texture copies): only needs to notice the CPU overwriting them.
        uint64_t SparseHash(const uint8_t* p)
        {
            const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
            uint64_t h = 0x9E3779B97F4A7C15ull;
            for (int i = 0; i < 512; i += 37)
                h = (h ^ q[i]) * 0xFF51AFD7ED558CCDull;
            return h ^ (h >> 29);
        }

        // Makes the GPU copy of [addr, addr+size) match guest memory. Pages
        // already compared in this epoch are skipped unless `force`.
        void EnsureRange(uint32_t addr, uint32_t size, bool force)
        {
            if (size == 0)
                return;
            struct Timer
            {
                std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                ~Timer() { g.tHash += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
            } timer;
            addr &= 0x1FFFFFFF;
            uint64_t end = std::min<uint64_t>(uint64_t(addr) + size, kSharedSize);
            uint32_t first = addr >> kPageShift, last = uint32_t((end - 1) >> kPageShift);
            uint32_t runStart = UINT32_MAX;
            auto flushRun = [&](uint32_t endPage) {
                if (runStart == UINT32_MAX)
                    return;
                uint64_t bytes = uint64_t(endPage - runStart) << kPageShift;
                uint64_t off;
                uint8_t* dst = UploadAlloc(bytes, 256, off);
                if (!dst)
                {
                    runStart = UINT32_MAX;
                    return;
                }
                memcpy(dst, PhysPtr(runStart << kPageShift), bytes);
                SharedTo(D3D12_RESOURCE_STATE_COPY_DEST);
                g.list->CopyBufferRegion(g.shared.Get(), uint64_t(runStart) << kPageShift, g.upload[g.frame].Get(), off, bytes);
                runStart = UINT32_MAX;
            };
            for (uint32_t p = first; p <= last; p++)
            {
                Page& pg = g_pages[p];
                if (force ? pg.checkedSync == g_syncEpoch : pg.checked == g_epoch)
                {
                    flushRun(p);
                    continue;
                }
                pg.checked = g_epoch;
                if (force)
                    pg.checkedSync = g_syncEpoch;
                g.hashedPages++;
                const uint8_t* cpu = PhysPtr(p << kPageShift);
                if (pg.gpu)
                {
                    if (SparseHash(cpu) == pg.hash)
                    {
                        flushRun(p);
                        continue;
                    }
                    pg.gpu = false; // the CPU wrote over GPU-written data
                }
                // Per-frame checks of large ranges (textures) compare a cheap
                // sample; each page still gets a full hash every 16 frames.
                uint64_t sp = SparseHash(cpu);
                if (pg.valid && !force && sp == pg.sparse && ((p + g_epoch) & 15) != 0)
                {
                    flushRun(p);
                    continue;
                }
                uint64_t h = HashPage(cpu);
                pg.sparse = sp;
                if (pg.valid && h == pg.hash)
                {
                    flushRun(p);
                    continue;
                }
                pg.hash = h;
                pg.valid = true;
                pg.stamp = ++g_stamp;
                if (runStart == UINT32_MAX)
                    runStart = p;
            }
            flushRun(last + 1);
        }

        // After the GPU wrote [addr, addr+size): remember the CPU contents so
        // the GPU data is kept until the CPU writes there itself.
        void MarkGpuWritten(uint32_t addr, uint32_t size)
        {
            addr &= 0x1FFFFFFF;
            uint64_t end = std::min<uint64_t>(uint64_t(addr) + size, kSharedSize);
            for (uint32_t p = addr >> kPageShift; p <= uint32_t((end - 1) >> kPageShift); p++)
            {
                Page& pg = g_pages[p];
                pg.hash = SparseHash(PhysPtr(p << kPageShift));
                pg.gpu = true;
                pg.valid = true;
                pg.checked = g_epoch;
                pg.stamp = ++g_stamp;
            }
        }

        uint64_t MaxStamp(uint32_t addr, uint32_t size)
        {
            addr &= 0x1FFFFFFF;
            uint64_t end = std::min<uint64_t>(uint64_t(addr) + std::max<uint32_t>(size, 1), kSharedSize);
            uint64_t m = 0;
            for (uint32_t p = addr >> kPageShift; p <= uint32_t((end - 1) >> kPageShift); p++)
                m = std::max(m, g_pages[p].stamp);
            return m;
        }

        // ------------------------------------------------------------------
        // Shader compilation
        // ------------------------------------------------------------------
        uint64_t Fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
        {
            auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; i++)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        // Shader compiler: DXC (dxcompiler.dll + dxil.dll next to the exe,
        // copied from the Windows SDK by the build) when available - it is
        // much faster and more robust than the old fxc - otherwise fxc.
        typedef HRESULT(WINAPI* CreateBlobFn)(SIZE_T, ID3DBlob**);
        CreateBlobFn g_createBlob = nullptr;
#if XE_HAVE_DXC
        DxcCreateInstanceProc g_dxcCreate = nullptr;
#endif

        ComPtr<ID3DBlob> MakeBlob(const void* data, size_t size)
        {
            ComPtr<ID3DBlob> blob;
            if (g_createBlob && SUCCEEDED(g_createBlob(size, &blob)))
                memcpy(blob->GetBufferPointer(), data, size);
            return blob;
        }

        bool UsingDxc()
        {
#if XE_HAVE_DXC
            return g_dxcCreate != nullptr;
#else
            return false;
#endif
        }

        ComPtr<ID3DBlob> CompileHlsl(const std::string& sourceIn, const char* entry, const char* target51)
        {
            // Every shader sees the render resolution scale as a constant. The
            // host viewport spans min(8192 x S, 16384) pixels (the D3D12
            // limit), i.e. 2 / XE_NDC_K guest pixels.
            char prelude[160];
            snprintf(prelude, sizeof(prelude), "static const uint XE_RES_SCALE = %uu;\nstatic const float XE_NDC_K = %.9f;\n",
                     g_resScale, 2.0 * g_resScale / double(std::min<uint32_t>(8192 * g_resScale, 16384)));
            const std::string source = prelude + sourceIn;
            // Shader model 6 targets for DXC, 5.1 for fxc.
            std::string target = target51;
            if (UsingDxc())
                target = target.substr(0, 3) + "6_0";
            uint64_t key = Fnv(source.data(), source.size(), Fnv(target.data(), target.size(), Fnv(entry, strlen(entry))));
            char name[64];
            snprintf(name, sizeof(name), "%016llx.cso", (unsigned long long)key);
            std::filesystem::path path = std::filesystem::path(g.cacheDir) / name;
            {
                std::ifstream in(path, std::ios::binary);
                if (in)
                {
                    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
                    if (!bytes.empty())
                        return MakeBlob(bytes.data(), bytes.size());
                }
            }
            auto t0 = std::chrono::steady_clock::now();
            ComPtr<ID3DBlob> code;
            std::string errorText;
#if XE_HAVE_DXC
            if (UsingDxc())
            {
                ComPtr<IDxcCompiler3> dxc;
                if (SUCCEEDED(g_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc))))
                {
                    std::wstring wentry(entry, entry + strlen(entry)), wtarget(target.begin(), target.end());
                    LPCWSTR args[] = { L"-E", wentry.c_str(), L"-T", wtarget.c_str(), L"-HV", L"2018", L"-O3" };
                    DxcBuffer buf = { source.data(), source.size(), DXC_CP_UTF8 };
                    ComPtr<IDxcResult> result;
                    HRESULT status = E_FAIL;
                    if (SUCCEEDED(dxc->Compile(&buf, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))))
                    {
                        result->GetStatus(&status);
                        ComPtr<IDxcBlobUtf8> errors;
                        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
                        if (errors && errors->GetStringLength())
                            errorText = errors->GetStringPointer();
                        ComPtr<IDxcBlob> obj;
                        if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj)
                            code = MakeBlob(obj->GetBufferPointer(), obj->GetBufferSize());
                    }
                }
                else
                    errorText = "DxcCreateInstance failed";
            }
            else
#endif
            {
                ComPtr<ID3DBlob> errors;
                HRESULT hr = g.compile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target.c_str(),
                                       D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
                if (errors)
                    errorText = static_cast<const char*>(errors->GetBufferPointer());
                if (FAILED(hr))
                    code = nullptr;
            }
            if (!code)
            {
                LOG("[d3d12] shader compile failed (%s %s): %s", entry, target.c_str(), errorText.empty() ? "unknown error" : errorText.c_str());
                std::ofstream dump(std::filesystem::path(g.cacheDir) / (std::string(name) + ".failed.hlsl"));
                dump << source;
                return nullptr;
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            LOG("[d3d12] compiled %s %s in %.0f ms", target.c_str(), entry, ms);
            std::ofstream out(path, std::ios::binary);
            out.write(static_cast<const char*>(code->GetBufferPointer()), std::streamsize(code->GetBufferSize()));
            return code;
        }

        struct GuestShader
        {
            uint32_t type = 0;
            uint64_t key = 0;
            xlat::TranslatedShader xlat;
            ComPtr<ID3DBlob> blob;
            // 0 = not started, 1 = compiling in the background, 2 = ready, 3 = failed
            std::atomic<int> state{ 0 };
        };
        std::unordered_map<uint64_t, std::unique_ptr<GuestShader>> g_shaders;
        GuestShader* g_vs = nullptr;
        GuestShader* g_ps = nullptr;
        GuestShader g_emptyPs; // used when a draw has no pixel shader (depth only)

        std::string BuildSource(bool pixel, const xlat::TranslatedShader& t, bool mips = true)
        {
            std::string s;
            s += d3d12src::kDrawDecls;
            s += "#define XE_TEMPS " + std::to_string(std::max<uint32_t>(t.tempCount, 16)) + "u\n";
            s += pixel ? "#define XE_CONST_SIZE xe_sys[3].x\n" : "#define XE_CONST_SIZE xe_sys[2].w\n";
            if (!pixel)
                s += "#define XE_INTERP 1\n"
                     "cbuffer XeFloatPrev : register(b3) { float4 xe_cfp[256]; };\n"
                     "cbuffer XeInterp : register(b4) { float4 xe_interp; };\n";
            s += d3d12src::kCommon;
            s += d3d12src::kLoopAddress;
            s += d3d12src::kDialect;
            if (pixel && mips)
                s += "#define XE_PS 1\n";
            s += d3d12src::kTextures;
            if (!pixel)
                s += d3d12src::kFitHud;
            std::string tmpl = pixel ? d3d12src::kPixel : d3d12src::kVertex;
            size_t at = tmpl.find("XE_BODY");
            tmpl.replace(at, 7, t.body);
            s += tmpl;
            return s;
        }

        void CompileNow(GuestShader& sh)
        {
            if (!sh.xlat.ok)
            {
                LOG("[d3d12] shader %016llx not translated: %s", (unsigned long long)sh.key, sh.xlat.error.c_str());
                sh.state = 3;
                return;
            }
            sh.blob = CompileHlsl(BuildSource(sh.type == 1, sh.xlat), "main", sh.type == 1 ? "ps_5_1" : "vs_5_1");
            if (!sh.blob && sh.type == 1 && sh.xlat.textureFetchMask)
            {
                // Mip selection uses screen-space derivatives inside the
                // shader's control flow, which a compiler may refuse; fall
                // back to always sampling the full-size level.
                LOG("[d3d12] pixel shader %016llx: retrying without mip selection", (unsigned long long)sh.key);
                sh.blob = CompileHlsl(BuildSource(true, sh.xlat, false), "main", "ps_5_1");
            }
            sh.state = sh.blob ? 2 : 3;
        }

        // Background compilation: draws whose shaders aren't ready yet are
        // skipped instead of stalling the game.
        std::mutex g_compileMutex;
        std::condition_variable g_compileCv;
        std::deque<GuestShader*> g_compileQueue;
        bool g_compileThreadsStarted = false;

        void CompileWorker()
        {
            for (;;)
            {
                GuestShader* sh;
                {
                    std::unique_lock lock(g_compileMutex);
                    g_compileCv.wait(lock, [] { return !g_compileQueue.empty(); });
                    sh = g_compileQueue.front();
                    g_compileQueue.pop_front();
                }
                CompileNow(*sh);
            }
        }

        // Returns true if the shader is ready to use.
        bool EnsureCompiled(GuestShader& sh)
        {
            int st = sh.state.load();
            if (st == 2) return true;
            if (st != 0) return false;
            sh.state = 1;
            if (!g_compileThreadsStarted)
            {
                g_compileThreadsStarted = true;
                unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
                for (unsigned i = 0; i < n; i++)
                    std::thread(CompileWorker).detach();
            }
            {
                std::lock_guard lock(g_compileMutex);
                g_compileQueue.push_back(&sh);
            }
            g_compileCv.notify_one();
            return false;
        }

        ComPtr<ID3DBlob> g_rectGs;

        // ------------------------------------------------------------------
        // Pipelines
        // ------------------------------------------------------------------
        struct PipelineKey
        {
            uint64_t vs, ps;
            uint32_t topology; // D3D12_PRIMITIVE_TOPOLOGY_TYPE
            uint32_t rect, cull, frontCcw, cut;
            bool operator==(const PipelineKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct PipelineKeyHash { size_t operator()(const PipelineKey& k) const { return size_t(Fnv(&k, sizeof(k))); } };

        // ------------------------------------------------------------------
        // Shader warm-up: every guest shader and pipeline the game has used is
        // listed in the shader cache folder (shaders.vrs, pipelines.vrp). At
        // startup they are all built before the game starts - quick when the
        // compiled shaders and pipelines are already cached, a minute or so
        // after a renderer change (new shader code) or a driver update - so
        // objects don't pop in while their shaders compile in the background.
        // ------------------------------------------------------------------
        std::unordered_set<uint64_t> g_manifestShaders;
        std::unordered_set<uint64_t> g_manifestPipelines; // by hash of the key
        std::filesystem::path ShaderManifestPath() { return std::filesystem::path(g.cacheDir) / "shaders.vrs"; }
        std::filesystem::path PipelineManifestPath() { return std::filesystem::path(g.cacheDir) / "pipelines.vrp"; }

        void RecordShader(uint32_t type, uint64_t key, const std::vector<uint32_t>& ucode)
        {
            if (!g_manifestShaders.insert(key).second)
                return;
            std::ofstream out(ShaderManifestPath(), std::ios::binary | std::ios::app);
            uint32_t n = uint32_t(ucode.size());
            out.write(reinterpret_cast<const char*>(&type), 4);
            out.write(reinterpret_cast<const char*>(&key), 8);
            out.write(reinterpret_cast<const char*>(&n), 4);
            out.write(reinterpret_cast<const char*>(ucode.data()), std::streamsize(n) * 4);
        }

        void RecordPipeline(const PipelineKey& key)
        {
            if (!g_manifestPipelines.insert(Fnv(&key, sizeof(key))).second)
                return;
            std::ofstream out(PipelineManifestPath(), std::ios::binary | std::ios::app);
            out.write(reinterpret_cast<const char*>(&key), sizeof(key));
        }

        // Pipelines are created on background threads (draws that need one
        // still being created are skipped for those frames), and kept in a
        // pipeline library saved to the shader cache, so the next run loads
        // them at once instead of making the driver compile them again.
        struct PipelineEntry
        {
            ComPtr<ID3D12PipelineState> pso;
            std::atomic<int> state{ 0 }; // 0 = being created, 2 = ready, 3 = failed
        };
        std::unordered_map<PipelineKey, std::unique_ptr<PipelineEntry>, PipelineKeyHash> g_pipelines;
        std::atomic<uint32_t> g_psoPending{ 0 };

        ComPtr<ID3D12PipelineLibrary> g_psoLib;
        std::vector<char> g_psoLibData; // must outlive the library
        std::mutex g_psoLibMutex;
        bool g_psoLibDirty = false;

        std::filesystem::path PipelineLibraryPath() { return std::filesystem::path(g.cacheDir) / "pipelines.bin"; }

        void InitPipelineLibrary()
        {
            ComPtr<ID3D12Device1> dev1;
            if (FAILED(g.device.As(&dev1)))
                return;
            {
                std::ifstream in(PipelineLibraryPath(), std::ios::binary);
                if (in)
                    g_psoLibData.assign(std::istreambuf_iterator<char>(in), {});
            }
            if (!g_psoLibData.empty() &&
                SUCCEEDED(dev1->CreatePipelineLibrary(g_psoLibData.data(), g_psoLibData.size(), IID_PPV_ARGS(&g_psoLib))))
            {
                LOG("[d3d12] pipeline cache loaded (%zu KB)", g_psoLibData.size() >> 10);
                return;
            }
            // Missing, or made by another driver version: start a new one.
            g_psoLibData.clear();
            if (FAILED(dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g_psoLib))))
                g_psoLib.Reset();
        }

        void SavePipelineLibrary()
        {
            std::lock_guard lock(g_psoLibMutex);
            if (!g_psoLib || !g_psoLibDirty)
                return;
            size_t size = g_psoLib->GetSerializedSize();
            std::vector<char> data(size);
            if (!size || FAILED(g_psoLib->Serialize(data.data(), size)))
                return;
            g_psoLibDirty = false;
            std::filesystem::path path = PipelineLibraryPath(), tmp = path;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary);
                out.write(data.data(), std::streamsize(size));
                if (!out)
                    return;
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
        }

        std::mutex g_psoQueueMutex;
        std::condition_variable g_psoQueueCv;
        std::deque<std::function<void()>> g_psoQueue;
        bool g_psoThreadsStarted = false;
        unsigned g_psoThreadCount = 0; // set before the first job for more threads (warm-up)

        void QueuePipelineJob(std::function<void()> job)
        {
            if (!g_psoThreadsStarted)
            {
                g_psoThreadsStarted = true;
                unsigned n = g_psoThreadCount ? g_psoThreadCount : std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
                for (unsigned i = 0; i < n; i++)
                    std::thread([] {
                        for (;;)
                        {
                            std::function<void()> j;
                            {
                                std::unique_lock lock(g_psoQueueMutex);
                                g_psoQueueCv.wait(lock, [] { return !g_psoQueue.empty(); });
                                j = std::move(g_psoQueue.front());
                                g_psoQueue.pop_front();
                            }
                            j();
                        }
                    }).detach();
            }
            {
                std::lock_guard lock(g_psoQueueMutex);
                g_psoQueue.push_back(std::move(job));
            }
            g_psoQueueCv.notify_one();
        }

        ID3D12PipelineState* GetPipeline(const PipelineKey& key, GuestShader& vs, GuestShader& ps)
        {
            auto& slot = g_pipelines[key];
            if (slot)
                return slot->state.load(std::memory_order_acquire) == 2 ? slot->pso.Get() : nullptr;
            slot = std::make_unique<PipelineEntry>();
            RecordPipeline(key);
            PipelineEntry* e = slot.get();

            struct Job
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
                ComPtr<ID3DBlob> vs, ps, gs; // keep the bytecode alive
                wchar_t name[96] = {};
            };
            auto job = std::make_shared<Job>();
            job->vs = vs.blob;
            job->ps = ps.blob;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC& d = job->d;
            d.pRootSignature = g.drawRoot.Get();
            d.VS = { vs.blob->GetBufferPointer(), vs.blob->GetBufferSize() };
            d.PS = { ps.blob->GetBufferPointer(), ps.blob->GetBufferSize() };
            if (key.rect && g_rectGs)
            {
                job->gs = g_rectGs;
                d.GS = { g_rectGs->GetBufferPointer(), g_rectGs->GetBufferSize() };
            }
            DefaultStates(d, 0);
            d.RasterizerState.CullMode = D3D12_CULL_MODE(key.cull);
            d.RasterizerState.FrontCounterClockwise = key.frontCcw ? TRUE : FALSE;
            d.RasterizerState.DepthClipEnable = FALSE;
            d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(key.cut);
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology);
            d.NumRenderTargets = 0;
            d.SampleDesc.Count = 1;
            // Named by everything that goes into it (the compiled shaders
            // differ with the render scale, so their bytes are included).
            uint64_t h1 = Fnv(&key, sizeof(key));
            uint64_t h2 = Fnv(d.VS.pShaderBytecode, d.VS.BytecodeLength, Fnv(d.PS.pShaderBytecode, d.PS.BytecodeLength));
            if (d.GS.pShaderBytecode)
                h2 = Fnv(d.GS.pShaderBytecode, d.GS.BytecodeLength, h2);
            swprintf(job->name, 96, L"vr3_%016llx_%016llx", (unsigned long long)h1, (unsigned long long)h2);

            // Already in the cache: load it now (fast).
            if (g_psoLib)
            {
                std::lock_guard lock(g_psoLibMutex);
                ComPtr<ID3D12PipelineState> pso;
                if (SUCCEEDED(g_psoLib->LoadGraphicsPipeline(job->name, &d, IID_PPV_ARGS(&pso))))
                {
                    e->pso = pso;
                    e->state.store(2, std::memory_order_release);
                    return e->pso.Get();
                }
            }
            g_psoPending++;
            QueuePipelineJob([job, e] {
                ComPtr<ID3D12PipelineState> pso;
                if (Check(g.device->CreateGraphicsPipelineState(&job->d, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState"))
                {
                    if (g_psoLib)
                    {
                        std::lock_guard lock(g_psoLibMutex);
                        if (SUCCEEDED(g_psoLib->StorePipeline(job->name, pso.Get())))
                            g_psoLibDirty = true;
                    }
                    e->pso = pso;
                    e->state.store(2, std::memory_order_release);
                }
                else
                    e->state.store(3, std::memory_order_release);
                g_psoPending--;
            });
            return nullptr;
        }

        // ------------------------------------------------------------------
        // Textures
        // ------------------------------------------------------------------
        struct HostFormat
        {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            uint32_t bpbLog2 = 2;
            bool block = false;   // 4x4 compressed
            uint32_t flags = 0;   // tex_info.w: bit0 BGR remap
        };

        HostFormat MapFormat(uint32_t f)
        {
            switch (f)
            {
            case 2: case 8: case 9: return { DXGI_FORMAT_R8_UNORM, 0, false, 0 };
            case 3: return { DXGI_FORMAT_B5G5R5A1_UNORM, 1, false, 1 };
            case 4: return { DXGI_FORMAT_B5G6R5_UNORM, 1, false, 1 };
            case 6: case 14: case 50: return { DXGI_FORMAT_R8G8B8A8_UNORM, 2, false, 0 };
            case 7: case 54: return { DXGI_FORMAT_R10G10B10A2_UNORM, 2, false, 0 };
            case 10: return { DXGI_FORMAT_R8G8_UNORM, 1, false, 0 };
            case 15: return { DXGI_FORMAT_B4G4R4A4_UNORM, 1, false, 1 };
            case 18: case 51: return { DXGI_FORMAT_BC1_UNORM, 3, true, 0 };
            case 19: case 52: return { DXGI_FORMAT_BC2_UNORM, 4, true, 0 };
            case 20: case 53: return { DXGI_FORMAT_BC3_UNORM, 4, true, 0 };
            case 22: case 23: return { DXGI_FORMAT_R8G8B8A8_UNORM, 2, false, 0 }; // depth: raw bits
            case 24: return { DXGI_FORMAT_R16_UNORM, 1, false, 0 };
            case 25: return { DXGI_FORMAT_R16G16_UNORM, 2, false, 0 };
            case 26: return { DXGI_FORMAT_R16G16B16A16_UNORM, 3, false, 0 };
            case 30: return { DXGI_FORMAT_R16_FLOAT, 1, false, 0 };
            case 31: return { DXGI_FORMAT_R16G16_FLOAT, 2, false, 0 };
            case 32: return { DXGI_FORMAT_R16G16B16A16_FLOAT, 3, false, 0 };
            case 36: return { DXGI_FORMAT_R32_FLOAT, 2, false, 0 };
            case 37: return { DXGI_FORMAT_R32G32_FLOAT, 3, false, 0 };
            case 38: return { DXGI_FORMAT_R32G32B32A32_FLOAT, 4, false, 0 };
            case 49: return { DXGI_FORMAT_BC5_UNORM, 4, true, 0 };
            case 59: return { DXGI_FORMAT_BC4_UNORM, 3, true, 0 };
            default: return {};
            }
        }

    // ----------------------------------------------------------------------
    // Scaled copies of resolved surfaces (render resolution > 1x)
    // ----------------------------------------------------------------------
    // A surface is a tiled image in guest memory that resolves write to (a
    // render target copy, the front buffer). Its scaled copy has the same
    // layout at S x the size, in g.scaledPool. Resolves into part of a
    // surface (e.g. the right half of the screen, written at base + an offset
    // of whole 32x32 tiles) find the surface they belong to.
    struct ScaledSurface
    {
        uint32_t base = 0, bytes = 0;      // guest memory range
        uint32_t pitch = 0, height = 0;    // guest pixels (multiples of 32)
        uint32_t bppLog2 = 0;
        uint64_t poolOffset = 0;
        uint64_t gen = 0;                  // g_resolveGen of the last write
    };
    static std::vector<ScaledSurface> g_scaledSurfaces;
    static uint64_t g_scaledPoolUsed = 0;

    static void DropScaledSurfaces(uint32_t base, uint32_t bytes)
    {
        g_scaledSurfaces.erase(std::remove_if(g_scaledSurfaces.begin(), g_scaledSurfaces.end(),
                                              [&](const ScaledSurface& s) { return s.base < base + bytes && base < s.base + s.bytes; }),
                               g_scaledSurfaces.end());
    }

    // Position (guest pixels) of `addr` in a tiled surface, if it's at a whole tile.
    static bool TileOrigin(const ScaledSurface& s, uint32_t addr, uint32_t& x, uint32_t& y)
    {
        uint32_t delta = addr - s.base;
        uint32_t tileBytes = 1024u << s.bppLog2, tilesPerRow = s.pitch / 32;
        if (delta % tileBytes || !tilesPerRow)
            return false;
        uint32_t idx = delta / tileBytes;
        x = (idx % tilesPerRow) * 32;
        y = (idx / tilesPerRow) * 32;
        return true;
    }

    // The surface a resolve of (w x h guest pixels at its (0,0)) to `base`
    // writes, creating it if needed. Sets the resolve's origin in it.
    static ScaledSurface* GetScaledSurface(uint32_t base, uint32_t pitch, uint32_t height, uint32_t bppLog2,
                                           uint32_t w, uint32_t h, uint32_t& dx, uint32_t& dy)
    {
        for (auto it = g_scaledSurfaces.begin(); it != g_scaledSurfaces.end(); ++it)
        {
            if (base < it->base || base >= it->base + it->bytes)
                continue;
            // (A resolve may reach past the surface's right edge - the second
            // half of a 1696-wide picture is resolved 864 wide - and is clipped.)
            if (it->pitch == pitch && it->bppLog2 == bppLog2 && TileOrigin(*it, base, dx, dy) &&
                dx < it->pitch && dy < it->height)
                return &*it;
            g_scaledSurfaces.erase(it); // laid out differently now
            break;
        }
        uint64_t bytes = uint64_t(pitch) * height << bppLog2;
        uint64_t scaledBytes = (bytes * g_resScale * g_resScale + 255) & ~255ull;
        if (scaledBytes > g.scaledPoolSize)
            return nullptr;
        if (g_scaledPoolUsed + scaledBytes > g.scaledPoolSize)
        {
            // Full: start over (surfaces are recreated by their next resolve).
            LOG("[d3d12] scaled surface pool full - recycling");
            g_scaledSurfaces.clear();
            g_scaledPoolUsed = 0;
        }
        DropScaledSurfaces(base, uint32_t(bytes));
        ScaledSurface s;
        s.base = base;
        s.bytes = uint32_t(bytes);
        s.pitch = pitch;
        s.height = height;
        s.bppLog2 = bppLog2;
        s.poolOffset = g_scaledPoolUsed;
        g_scaledPoolUsed += scaledBytes;
        g_scaledSurfaces.push_back(s);
        dx = dy = 0;
        return &g_scaledSurfaces.back();
    }


        struct Texture
        {
            ComPtr<ID3D12Resource> res;
            uint32_t srv = UINT32_MAX;   // CPU heap index
            uint32_t width = 0, height = 0, hostW = 0, hostH = 0;
            uint64_t builtStamp = 0;
            uint64_t validEpoch = 0, validGen = 0; // last full validation
            uint32_t base = 0, bytes = 0;
            HostFormat fmt;
            uint64_t lastUse = 0;
            bool ok = false;
            uint32_t scale = 1;        // > 1: built from a scaled surface copy
            uint64_t surfaceGen = 0;   // that surface's generation when built
            uint32_t mipBase = 0, mipBytes = 0, mipLevels = 1;
            bool cube = false;         // six faces (+X -X +Y -Y +Z -Z), sampled as a cube map
            bool replaced = false;     // made from a file in mods\textures (whole texture = UV 0..1)
        };
        uint32_t g_nullCubeSrv = UINT32_MAX; // CPU heap: empty cube view for cube slots without a texture

        // Where a texture's smaller mip levels are in guest memory. Levels 1+
        // follow each other from the mip address, each at power-of-two size
        // rounded up to whole 32x32-block tiles and 4 KB; levels smaller than
        // 16 texels are packed together inside one level's tiles. (Layout as
        // documented by Xenia's texture_util.cc.)
        struct MipLevel { uint32_t base, pitchBlocks, xBlocks, yBlocks, wBlocks, hBlocks; };
        constexpr uint32_t kMaxMips = 13;

        uint32_t Log2Ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
        uint32_t Log2Floor(uint32_t v) { uint32_t r = 0; while (v >>= 1) r++; return r; }

        // Returns the number of levels including the base (1 = no mips).
        uint32_t GuestMips(const xenos::xe_gpu_texture_fetch_t& f, uint32_t w, uint32_t h, uint32_t blockDim, uint32_t bpb,
                           MipLevel* levels, uint32_t& mipBytes)
        {
            mipBytes = 0;
            if (f.dimension != xenos::DataDimension::k2DOrStacked || f.stacked || !f.mip_address || !f.base_address)
                return 1;
            uint32_t sizeMax = Log2Floor(std::max(w, h));
            uint32_t maxLevel = std::min<uint32_t>({ uint32_t(f.mip_max_level), sizeMax, kMaxMips - 1 });
            if (maxLevel == 0 || f.mip_min_level != 0)
                return 1;
            uint32_t lw = Log2Ceil(w), lh = Log2Ceil(h), ls = std::min(lw, lh);
            uint32_t packed = f.packed_mips ? (ls > 4 ? ls - 4 : 0) : UINT32_MAX;
            if (packed == 0)
                return 1; // tiny texture with every level packed into the base: base only
            auto align = [](uint32_t v, uint32_t a) { return (v + a - 1) / a * a; };
            uint32_t pw2 = 1u << lw, ph2 = 1u << lh;
            uint32_t offset[kMaxMips] = {}, pitch[kMaxMips] = {};
            uint32_t running = 0, lastStored = std::min(maxLevel, packed);
            for (uint32_t sl = 1; sl <= lastStored; sl++)
            {
                uint32_t pB = align(align(std::max(pw2 >> sl, 1u), blockDim) / blockDim, 32);
                uint32_t rB = align(align(std::max(ph2 >> sl, 1u), blockDim) / blockDim, 32);
                uint32_t rowBytes = pB * bpb;
                if (!f.tiled)
                    rowBytes = align(rowBytes, 256);
                offset[sl] = running;
                pitch[sl] = rowBytes / bpb;
                running += align(rowBytes * rB, 4096);
            }
            for (uint32_t L = 1; L <= maxLevel; L++)
            {
                uint32_t sl = std::min(L, packed);
                MipLevel& m = levels[L];
                m.base = (f.mip_address << 12) + offset[sl];
                m.pitchBlocks = pitch[sl];
                m.xBlocks = m.yBlocks = 0;
                if (L >= packed)
                {
                    uint32_t pm = L - packed;
                    if (pm < 3)
                    {
                        if (lw > lh) m.yBlocks = 16 >> pm; else m.xBlocks = 16 >> pm;
                    }
                    else
                    {
                        uint32_t off = (1u << ((lw > lh ? lw : lh) - packed)) >> (pm - 2);
                        if (lw > lh) m.xBlocks = off; else m.yBlocks = off;
                    }
                    m.xBlocks /= blockDim;
                    m.yBlocks /= blockDim;
                }
                m.wBlocks = align(std::max(w >> L, 1u), blockDim) / blockDim;
                m.hBlocks = align(std::max(h >> L, 1u), blockDim) / blockDim;
            }
            mipBytes = running;
            return maxLevel + 1;
        }
        std::unordered_map<uint64_t, std::unique_ptr<Texture>> g_textures;
        struct Retired { ComPtr<ID3D12Resource> res; uint32_t srv; uint64_t frame; };
        std::vector<Retired> g_retired;

        // ------------------------------------------------------------------
        // Texture replacements (mods\textures next to the executable)
        //
        // A texture is named by a hash of its data in guest memory, its format
        // and size: <16 hex digits>.dds or .png (anything after the digits,
        // like "_brick", is ignored). F11 saves the textures of the next frame
        // to mods\dump under those names, to edit or upscale and put into
        // mods\textures; F10 reloads that folder while playing.
        // ------------------------------------------------------------------
        struct ModTextures
        {
            std::filesystem::path root;
            std::unordered_map<uint64_t, std::filesystem::path> files;            // by hash
            std::unordered_map<uint64_t, std::shared_ptr<texfiles::Image>> images; // loaded (nullptr: unusable)
            std::atomic<bool> dumpRequest{ false }, reloadRequest{ false };
            bool dumping = false;
            struct DumpEntry { Texture* tex; uint32_t format, width, height; };
            std::vector<DumpEntry> dumpList;
        } g_mods;

        uint64_t TextureHash(uint32_t base, uint32_t bytes, uint32_t format, uint32_t w, uint32_t h)
        {
            const uint8_t* p = PhysPtr(base);
            uint32_t n = std::min<uint32_t>(bytes, uint32_t(kSharedSize - (base & 0x1FFFFFFF)));
            uint64_t a = 0x9E3779B97F4A7C15ull ^ (uint64_t(format) << 32 | (w << 16) | h), b = 0xC2B2AE3D27D4EB4Full ^ n;
            uint32_t i = 0;
            for (; i + 16 <= n; i += 16)
            {
                uint64_t x, y;
                memcpy(&x, p + i, 8);
                memcpy(&y, p + i + 8, 8);
                a = (a ^ x) * 0xFF51AFD7ED558CCDull;
                b = (b ^ y) * 0xC4CEB9FE1A85EC53ull;
                a ^= a >> 29;
                b ^= b >> 31;
            }
            for (; i < n; i++)
                a = (a ^ p[i]) * 0x100000001B3ull;
            uint64_t h64 = a ^ (b * 0x94D049BB133111EBull);
            return h64 ^ (h64 >> 33);
        }

        bool GpuWritten(uint32_t base, uint32_t bytes)
        {
            uint32_t first = (base & 0x1FFFFFFF) >> kPageShift;
            uint32_t last = std::min<uint32_t>(uint32_t(((base & 0x1FFFFFFF) + std::max(bytes, 1u) - 1) >> kPageShift), kPageCount - 1);
            for (uint32_t pg = first; pg <= last; pg++)
                if (g_pages[pg].gpu)
                    return true;
            return false;
        }

        std::filesystem::path ExeDir()
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            return std::filesystem::path(path).parent_path();
        }

        void ScanModTextures()
        {
            g_mods.root = ExeDir() / "mods";
            std::error_code ec;
            std::filesystem::create_directories(g_mods.root / "textures", ec);
            g_mods.files.clear();
            g_mods.images.clear();
            for (auto it = std::filesystem::recursive_directory_iterator(g_mods.root / "textures", ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                std::wstring name = it->path().filename().wstring();
                std::wstring ext = it->path().extension().wstring();
                for (auto& c : ext) c = wchar_t(towlower(c));
                if (name.size() < 17 || (ext != L".dds" && ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".bmp"))
                    continue;
                uint64_t hash = 0;
                bool ok = true;
                for (int i = 0; i < 16 && ok; i++)
                {
                    wchar_t c = name[i];
                    int v = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
                    ok = v >= 0;
                    hash = (hash << 4) | uint64_t(v & 15);
                }
                if (ok && (name[16] == L'.' || name[16] == L'_' || name[16] == L'-' || name[16] == L' '))
                    g_mods.files.emplace(hash, it->path()); // (the first one found wins)
            }
            LOG("[mods] %zu replacement textures in %s", g_mods.files.size(), (g_mods.root / "textures").string().c_str());
        }

        const texfiles::Image* FindReplacement(uint64_t hash)
        {
            auto f = g_mods.files.find(hash);
            if (f == g_mods.files.end())
                return nullptr;
            auto& img = g_mods.images[hash];
            if (!img)
            {
                auto loaded = std::make_shared<texfiles::Image>();
                std::string error;
                if (texfiles::Load(f->second.wstring(), *loaded, error))
                {
                    img = loaded;
                    LOG("[mods] using %s (%ux%u)", f->second.filename().string().c_str(), loaded->width, loaded->height);
                }
                else
                {
                    LOG("[mods] can't use %s: %s", f->second.filename().string().c_str(), error.c_str());
                    g_mods.images.erase(hash);
                    g_mods.files.erase(f); // don't try again
                    return nullptr;
                }
            }
            return img.get();
        }

        // (Re)builds t from a replacement image.
        bool BuildReplacement(Texture& t, const texfiles::Image& img, uint32_t guestW, uint32_t guestH)
        {
            uint32_t levels = uint32_t(img.levels.size());
            if (!t.replaced || !t.res || t.hostW != img.width || t.hostH != img.height || t.mipLevels != levels || t.fmt.format != img.format)
            {
                if (t.res)
                {
                    g_retired.push_back({ t.res, t.srv, g.frameNumber });
                    t.res.Reset();
                    t.srv = UINT32_MAX;
                }
                D3D12_HEAP_PROPERTIES hp = {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC d = {};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width = img.width;
                d.Height = img.height;
                d.DepthOrArraySize = 1;
                d.MipLevels = UINT16(levels);
                d.Format = img.format;
                d.SampleDesc.Count = 1;
                if (!Check(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&t.res)), "CreateCommittedResource(replacement)"))
                    return false;
                t.srv = AllocCpuView();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = img.format;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = levels;
                g.device->CreateShaderResourceView(t.res.Get(), &sd, CpuView(t.srv));
            }
            // Upload through a buffer of its own (big images would crowd the
            // frame's upload memory), released once the GPU is done with it.
            D3D12_RESOURCE_DESC rd = t.res->GetDesc();
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(levels);
            std::vector<UINT> rows(levels);
            std::vector<UINT64> rowBytes(levels);
            UINT64 total = 0;
            g.device->GetCopyableFootprints(&rd, 0, levels, 0, fps.data(), rows.data(), rowBytes.data(), &total);
            ComPtr<ID3D12Resource> buf = CreateBuffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            uint8_t* dst = nullptr;
            D3D12_RANGE none = { 0, 0 };
            if (!buf || FAILED(buf->Map(0, &none, reinterpret_cast<void**>(&dst))))
                return false;
            for (uint32_t m = 0; m < levels; m++)
            {
                const auto& l = img.levels[m];
                for (uint32_t r = 0; r < rows[m] && r < l.rows; r++)
                    memcpy(dst + fps[m].Offset + size_t(r) * fps[m].Footprint.RowPitch, l.data.data() + size_t(r) * l.rowBytes,
                           std::min<size_t>(l.rowBytes, size_t(rowBytes[m])));
            }
            buf->Unmap(0, nullptr);
            constexpr auto kSrv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Barrier(t.res.Get(), kSrv, D3D12_RESOURCE_STATE_COPY_DEST);
            for (uint32_t m = 0; m < levels; m++)
            {
                D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
                to.pResource = t.res.Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = m;
                from.pResource = buf.Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                from.PlacedFootprint = fps[m];
                g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            Barrier(t.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
            g_retired.push_back({ buf, UINT32_MAX, g.frameNumber });
            t.replaced = true;
            t.cube = false;
            t.scale = 1;
            t.surfaceGen = 0;
            t.width = guestW;
            t.height = guestH;
            t.hostW = img.width;
            t.hostH = img.height;
            t.mipLevels = levels;
            t.fmt = HostFormat{ img.format, 2, img.format != DXGI_FORMAT_R8G8B8A8_UNORM, 0 };
            return true;
        }

        // Returns the texture for a fetch constant (uploading/untiling as
        // needed), or nullptr if the format isn't supported.
        Texture* GetTexture(const xenos::xe_gpu_texture_fetch_t& f)
        {
            uint32_t w, h;
            if (f.dimension == xenos::DataDimension::k1D) { w = f.size_1d.width + 1; h = 1; }
            else if (f.dimension == xenos::DataDimension::k3D) { w = f.size_3d.width + 1; h = f.size_3d.height + 1; }
            else { w = f.size_2d.width + 1; h = f.size_2d.height + 1; }
            w = std::min<uint32_t>(w, 8192);
            h = std::min<uint32_t>(h, 8192);
            HostFormat hf = MapFormat(uint32_t(f.format));
            if (hf.format == DXGI_FORMAT_UNKNOWN)
                return nullptr;

            uint32_t key32[5] = { f.base_address, uint32_t(f.format) | (uint32_t(f.endianness) << 8) | (uint32_t(f.tiled) << 10) | (f.pitch << 11),
                                  w | (h << 16), uint32_t(f.dimension),
                                  f.mip_address | (uint32_t(f.mip_max_level) << 20) | (uint32_t(f.mip_min_level) << 24) | (uint32_t(f.packed_mips) << 28) };
            uint64_t key = Fnv(key32, sizeof(key32));
            auto& slot = g_textures[key];
            if (!slot)
                slot = std::make_unique<Texture>();
            Texture& t = *slot;
            t.lastUse = g.frameNumber;

            uint32_t blockDim = hf.block ? 4 : 1;
            uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
            uint32_t pitchBlocks = std::max((f.pitch << 5) / blockDim, bw);
            if (f.tiled)
                pitchBlocks = (pitchBlocks + 31) & ~31u;
            uint32_t bpb = 1u << hf.bpbLog2;
            // Small textures (16 texels or less on the short side) with packed
            // mips keep even their full-size level inside the packed mip tail,
            // at an offset: 16 texels across (or down, if wider than tall).
            // Reading from the corner instead gave black or garbage (e.g. the
            // 8x8 tiles on the rooftops).
            uint32_t baseXBlocks = 0, baseYBlocks = 0;
            if (f.packed_mips && f.dimension == xenos::DataDimension::k2DOrStacked &&
                std::min(Log2Ceil(w), Log2Ceil(h)) <= 4)
            {
                if (Log2Ceil(w) > Log2Ceil(h)) baseYBlocks = 16 / blockDim;
                else baseXBlocks = 16 / blockDim;
                pitchBlocks = std::max(pitchBlocks, baseXBlocks + bw);
                if (f.tiled)
                    pitchBlocks = (pitchBlocks + 31) & ~31u;
            }
            uint32_t rowsStored = f.tiled ? ((baseYBlocks + bh + 31) & ~31u) : baseYBlocks + bh;
            t.base = f.base_address << 12;
            t.bytes = pitchBlocks * rowsStored * bpb;
            // Cube maps: six faces one after another, each rounded up to whole
            // 32-block-row tiles and 4 KB (Xenia's texture_util.cc layout).
            bool cube = f.dimension == xenos::DataDimension::kCube;
            uint32_t faceStride = 0;
            if (cube)
            {
                faceStride = (pitchBlocks * ((baseYBlocks + bh + 31) & ~31u) * bpb + 4095) & ~4095u;
                t.bytes = faceStride * 6;
            }
            MipLevel mips[kMaxMips] = {};
            uint32_t mipBytes = 0;
            uint32_t mipLevels = GuestMips(f, w, h, blockDim, bpb, mips, mipBytes);
            t.mipBase = mipLevels > 1 ? (f.mip_address << 12) : 0;
            t.mipBytes = mipBytes;

            // Already validated this frame, and no render-to-texture copy has
            // touched it since: nothing to do (the common case, per draw).
            if (t.res && t.validEpoch == g_epoch)
            {
                bool touched = false;
                for (auto it = g_resolveRanges.rbegin(); it != g_resolveRanges.rend() && it->gen > t.validGen; ++it)
                    touched = touched || (it->start < t.base + t.bytes && t.base < it->end) ||
                              (t.mipBytes && it->start < t.mipBase + t.mipBytes && t.mipBase < it->end);
                if (!touched)
                {
                    t.validGen = g_resolveGen;
                    return t.ok ? &t : nullptr;
                }
            }
            t.validEpoch = g_epoch;
            t.validGen = g_resolveGen;
            EnsureRange(t.base, t.bytes, false);
            uint64_t stamp = MaxStamp(t.base, t.bytes);
            if (t.mipBytes)
            {
                EnsureRange(t.mipBase, t.mipBytes, false);
                stamp = std::max(stamp, MaxStamp(t.mipBase, t.mipBytes));
            }

            // At a higher render resolution, a texture that is (part of) a
            // resolved surface the CPU hasn't overwritten since is built from
            // the surface's scaled copy instead.
            const ScaledSurface* sf = nullptr;
            uint32_t ox = 0, oy = 0;
            if (g_resScale > 1 && f.tiled && !hf.block && f.dimension == xenos::DataDimension::k2DOrStacked && !baseXBlocks && !baseYBlocks)
            {
                for (auto& cand : g_scaledSurfaces)
                {
                    if (t.base < cand.base || t.base >= cand.base + cand.bytes || cand.pitch != pitchBlocks ||
                        cand.bppLog2 != hf.bpbLog2 || !TileOrigin(cand, t.base, ox, oy) || ox + w > cand.pitch || oy + h > cand.height)
                        continue;
                    uint32_t end = std::min(t.base + t.bytes, cand.base + cand.bytes);
                    bool gpuOwned = true;
                    for (uint32_t pg = (t.base & 0x1FFFFFFF) >> kPageShift; pg <= ((end - 1) & 0x1FFFFFFF) >> kPageShift && gpuOwned; pg++)
                        gpuOwned = g_pages[pg].gpu;
                    if (gpuOwned)
                        sf = &cand;
                    break;
                }
            }
            uint32_t scale = sf ? g_resScale : 1;
            if (t.res && stamp <= t.builtStamp && scale == t.scale && (!sf || sf->gen <= t.surfaceGen))
                return t.ok ? &t : nullptr;
            // A replacement from mods\textures (not for render targets).
            if (!g_mods.files.empty() && !cube && !sf && f.dimension == xenos::DataDimension::k2DOrStacked && !GpuWritten(t.base, t.bytes))
            {
                uint64_t hash = TextureHash(t.base, t.bytes, uint32_t(f.format), w, h);
                if (const texfiles::Image* img = FindReplacement(hash))
                    if (BuildReplacement(t, *img, w, h))
                    {
                        t.builtStamp = stamp;
                        t.ok = true;
                        return &t;
                    }
            }
            if (t.replaced)
            {
                // Its file is gone or the game changed the texture: back to the original.
                g_retired.push_back({ t.res, t.srv, g.frameNumber });
                t.res.Reset();
                t.srv = UINT32_MAX;
                t.replaced = false;
            }
            if (t.res && scale != t.scale)
            {
                // Switching between the scaled and unscaled version: new size.
                g_retired.push_back({ t.res, t.srv, g.frameNumber });
                t.res.Reset();
                t.srv = UINT32_MAX;
            }
            t.scale = scale;
            t.surfaceGen = sf ? sf->gen : 0;
            if (scale > 1)
                mipLevels = 1;
            if (t.res && (mipLevels != t.mipLevels || cube != t.cube))
            {
                g_retired.push_back({ t.res, t.srv, g.frameNumber });
                t.res.Reset();
                t.srv = UINT32_MAX;
            }
            t.mipLevels = mipLevels;
            t.cube = cube;

            if (!t.res)
            {
                t.width = w;
                t.height = h;
                t.hostW = scale > 1 ? w * scale : bw * blockDim;
                t.hostH = scale > 1 ? h * scale : bh * blockDim;
                t.fmt = hf;
                D3D12_HEAP_PROPERTIES hp = {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC d = {};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width = t.hostW;
                d.Height = t.hostH;
                d.DepthOrArraySize = cube ? 6 : 1;
                d.MipLevels = UINT16(t.mipLevels);
                d.Format = hf.format;
                d.SampleDesc.Count = 1;
                // Created readable, like every texture between uses, so a frame's
                // command list starts and ends with the same states and can be
                // executed again (in-between frames).
                if (!Check(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&t.res)), "CreateCommittedResource(texture)"))
                {
                    t.ok = false;
                    t.builtStamp = UINT64_MAX;
                    return nullptr;
                }
                t.srv = AllocCpuView();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = hf.format;
                sd.ViewDimension = cube ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (cube)
                    sd.TextureCube.MipLevels = t.mipLevels;
                else
                    sd.Texture2D.MipLevels = t.mipLevels;
                g.device->CreateShaderResourceView(t.res.Get(), &sd, CpuView(t.srv));
            }
            Barrier(t.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            // Untile one level into the staging buffer, then copy it into the
            // texture - in bands of rows if it doesn't fit at once.
            // (base, pitch in blocks, origin in blocks, size in blocks.)
            SharedTo(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.list->SetComputeRootSignature(g.computeRoot.Get());
            g.list->SetPipelineState(g.untilePso.Get());
            g.list->SetComputeRootDescriptorTable(1, ViewGpu(kDescEdramUav));
            auto upload = [&](uint32_t level, uint32_t srcBase, uint32_t srcPitch, uint32_t oxB, uint32_t oyB,
                              uint32_t cw, uint32_t chh, bool fromScaled, uint32_t slice = 0) -> bool {
                // The copy can't go past the level's size (rounded up to whole blocks).
                uint32_t levelW = std::max(t.hostW >> level, 1u), levelH = std::max(t.hostH >> level, 1u);
                cw = std::min(cw, (levelW + blockDim - 1) / blockDim);
                chh = std::min(chh, (levelH + blockDim - 1) / blockDim);
                uint32_t rowPitch = (cw * bpb + 255) & ~255u;
                if (rowPitch > kStagingSize || !cw || !chh)
                    return false;
                uint32_t bandRows = uint32_t(std::min<uint64_t>(kStagingSize / rowPitch, chh));
                for (uint32_t row = 0; row < chh; row += bandRows)
                {
                    uint32_t rows = std::min(bandRows, chh - row);
                    uint32_t rc[32] = {};
                    rc[0] = srcBase; rc[1] = srcPitch; rc[2] = cw; rc[3] = rows;
                    rc[4] = hf.bpbLog2; rc[5] = f.tiled ? 1u : 0u; rc[6] = uint32_t(f.endianness); rc[7] = rowPitch;
                    rc[8] = row; rc[9] = fromScaled ? 1u : 0u; rc[10] = oxB; rc[11] = oyB;
                    g.list->SetComputeRoot32BitConstants(0, 32, rc, 0);
                    g.list->Dispatch((cw + 7) / 8, (rows + 7) / 8, 1);
                    Barrier(g.staging.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                    dst.pResource = t.res.Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = level + slice * t.mipLevels;
                    src.pResource = g.staging.Get();
                    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src.PlacedFootprint.Offset = 0;
                    src.PlacedFootprint.Footprint.Format = hf.format;
                    src.PlacedFootprint.Footprint.Width = cw * blockDim;
                    src.PlacedFootprint.Footprint.Height = rows * blockDim;
                    src.PlacedFootprint.Footprint.Depth = 1;
                    src.PlacedFootprint.Footprint.RowPitch = rowPitch;
                    g.list->CopyTextureRegion(&dst, 0, row * blockDim, 0, &src, nullptr);
                    Barrier(g.staging.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                return true;
            };
            bool uploaded = sf ? upload(0, uint32_t(sf->poolOffset), sf->pitch * scale, ox * scale, oy * scale, w * scale, h * scale, true)
                               : upload(0, t.base, pitchBlocks, baseXBlocks, baseYBlocks, bw, bh, false);
            for (uint32_t face = 1; cube && uploaded && face < 6; face++)
                uploaded = upload(0, t.base + face * faceStride, pitchBlocks, baseXBlocks, baseYBlocks, bw, bh, false, face);
            if (!uploaded)
            {
                LOG("[d3d12] texture %ux%u too large to stage", w, h);
                Barrier(t.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                t.ok = false;
                t.builtStamp = UINT64_MAX;
                return nullptr;
            }
            for (uint32_t level = 1; level < t.mipLevels; level++)
            {
                const MipLevel& m = mips[level];
                upload(level, m.base, m.pitchBlocks, m.xBlocks, m.yBlocks, m.wBlocks, m.hBlocks, false);
            }
            Barrier(t.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            t.builtStamp = stamp;
            t.ok = true;
            return &t;
        }

        // Samplers: each distinct one gets a permanent place in the
        // shader-visible heap (shaders index it), so nothing is rewritten while
        // the GPU may use it. Only if 2048 different ones were seen does the
        // heap start over (after waiting for the GPU).
        std::unordered_map<uint64_t, uint32_t> g_samplerIndex;
        uint32_t GetSamplerIndex(const D3D12_SAMPLER_DESC& desc)
        {
            uint64_t key = Fnv(&desc, sizeof(desc));
            auto it = g_samplerIndex.find(key);
            if (it != g_samplerIndex.end())
                return it->second;
            if (g_samplerIndex.size() >= kSamplers)
            {
                FlushAndReset(kFlushSamplers);
                g_samplerIndex.clear();
            }
            uint32_t idx = uint32_t(g_samplerIndex.size());
            auto h = g.samplerHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(idx) * g.samplerIncrement;
            g.device->CreateSampler(&desc, h);
            g_samplerIndex[key] = idx;
            return idx;
        }

        // Texture tables (32 views) already written to this frame's part of the
        // ring, by contents: consecutive draws usually share their textures.
        std::unordered_map<uint64_t, uint32_t> g_texTables;
        void ResetViewCaches() { g_texTables.clear(); }

        uint32_t GetTexTable(uint64_t key, uint32_t mask, const uint32_t* srvs)
        {
            auto it = g_texTables.find(key);
            if (it != g_texTables.end())
                return it->second;
            uint32_t table = AllocViews(32); // (a flush here clears the cache)
            g.device->CopyDescriptorsSimple(32, ViewCpu(table), CpuView(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (uint32_t m = mask; m; m &= m - 1)
            {
                uint32_t i = uint32_t(__builtin_ctz(m));
                if (srvs[i] != UINT32_MAX)
                    g.device->CopyDescriptorsSimple(1, ViewCpu(table + i), CpuView(srvs[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            g_texTables[key] = table;
            return table;
        }

        D3D12_SAMPLER_DESC SamplerFor(const xenos::xe_gpu_texture_fetch_t& f)
        {
            auto addr = [](xenos::ClampMode m) {
                switch (m)
                {
                case xenos::ClampMode::kRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                case xenos::ClampMode::kMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                case xenos::ClampMode::kMirrorClampToEdge:
                case xenos::ClampMode::kMirrorClampToHalfway:
                case xenos::ClampMode::kMirrorClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
                case xenos::ClampMode::kClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                }
            };
            D3D12_SAMPLER_DESC s = {};
            bool magLinear = f.mag_filter == xenos::TextureFilter::kLinear;
            bool minLinear = f.min_filter == xenos::TextureFilter::kLinear;
            s.Filter = magLinear ? (minLinear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT)
                                 : (minLinear ? D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT);
            s.AddressU = addr(f.clamp_x);
            s.AddressV = addr(f.clamp_y);
            s.AddressW = addr(f.clamp_z);
            s.MaxAnisotropy = 1;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            uint32_t bc = uint32_t(f.border_color);
            float v = bc == 2 ? 1.0f : 0.0f;
            s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = v;
            s.BorderColor[3] = bc == 0 ? 0.0f : 1.0f;
            // Mip levels: the fetch constant's mip filter (base map only = no
            // mips), bias in 1/32 units.
            bool mipLinear = f.mip_filter == xenos::TextureFilter::kLinear;
            bool useMips = f.mip_filter != xenos::TextureFilter::kBaseMap;
            s.Filter = D3D12_FILTER((s.Filter & ~0x1) | (useMips && mipLinear ? 0x1 : 0x0)); // bit 0: linear between mips
            s.MinLOD = 0;
            s.MaxLOD = useMips ? float(f.mip_max_level) : 0.0f;
            s.MipLODBias = useMips ? std::clamp(float(f.lod_bias) / 32.0f, -16.0f, 15.99f) : 0.0f;
            return s;
        }

        // ------------------------------------------------------------------
        // Initialization
        // ------------------------------------------------------------------
        bool CreateRootSignatures()
        {
            auto serialize = [](const D3D12_ROOT_SIGNATURE_DESC& d, ComPtr<ID3D12RootSignature>& out, const char* name) {
                ComPtr<ID3DBlob> blob, err;
                if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
                {
                    LOG("[d3d12] root signature %s: %s", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                    return false;
                }
                return Check(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)), name);
            };

            // Draw: b0 system, b1 float consts (VS and PS each), b2 misc,
            // table { t0 shared, u0 EDRAM }, table { t1..t32 }, table { s0..s31 }.
            D3D12_DESCRIPTOR_RANGE memRanges[2] = {};
            memRanges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            memRanges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
            // Textures: t1..t32 as 2D textures, and the same descriptors again as
            // cube maps in register space 1 (each slot is read as one or the other).
            D3D12_DESCRIPTOR_RANGE texRanges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 1, 0, 0 },
                                                    { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 1, 1, 0 } };
            D3D12_DESCRIPTOR_RANGE sampRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplers, 0, 0, 0 };
            D3D12_ROOT_PARAMETER p[kRootCount] = {};
            p[kRootSys].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[kRootSys].Descriptor = { 0, 0 };
            p[kRootSys].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p[kRootVsConsts].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[kRootVsConsts].Descriptor = { 1, 0 };
            p[kRootVsConsts].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            p[kRootPsConsts].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[kRootPsConsts].Descriptor = { 1, 0 };
            p[kRootPsConsts].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            p[kRootMisc].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[kRootMisc].Descriptor = { 2, 0 };
            p[kRootMisc].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p[kRootMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[kRootMemory].DescriptorTable = { 2, memRanges };
            p[kRootMemory].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p[kRootTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[kRootTextures].DescriptorTable = { 2, texRanges };
            p[kRootTextures].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p[kRootSamplers].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[kRootSamplers].DescriptorTable = { 1, &sampRange };
            p[kRootSamplers].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            p[kRootVsPrev].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[kRootVsPrev].Descriptor = { 3, 0 };
            p[kRootVsPrev].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            p[kRootInterp].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[kRootInterp].Descriptor = { 4, 0 };
            p[kRootInterp].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            D3D12_ROOT_SIGNATURE_DESC rd = {};
            rd.NumParameters = kRootCount;
            rd.pParameters = p;
            if (!serialize(rd, g.drawRoot, "draw root signature"))
                return false;

            // Compute: 32 root constants (b0), table { u0 EDRAM, u1 shared, u2 staging }.
            D3D12_DESCRIPTOR_RANGE uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 0 };
            D3D12_ROOT_PARAMETER cp[2] = {};
            cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            cp[0].Constants = { 0, 0, 32 };
            cp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            cp[1].DescriptorTable = { 1, &uavRange };
            cp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC cd = {};
            cd.NumParameters = 2;
            cd.pParameters = cp;
            if (!serialize(cd, g.computeRoot, "compute root signature"))
                return false;

            // Present: table { t0 }, static linear sampler.
            D3D12_DESCRIPTOR_RANGE frontRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            // Table { t0 front buffer }, root constant b0 = fetch constant swizzle.
            D3D12_ROOT_PARAMETER pps[2] = {};
            pps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            pps[0].DescriptorTable = { 1, &frontRange };
            pps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            pps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            pps[1].Constants = { 0, 0, 4 };
            pps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC ss = {};
            ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss.MaxLOD = D3D12_FLOAT32_MAX;
            ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC prd = {};
            prd.NumParameters = 2;
            prd.pParameters = pps;
            prd.NumStaticSamplers = 1;
            prd.pStaticSamplers = &ss;
            return serialize(prd, g.presentRoot, "present root signature");
        }

        bool CreateFixedPipelines()
        {
            std::string compute = std::string(d3d12src::kCommon) + d3d12src::kResolve;
            auto makeCompute = [&](const char* entry, ComPtr<ID3D12PipelineState>& out) {
                ComPtr<ID3DBlob> cs = CompileHlsl(compute, entry, "cs_5_1");
                if (!cs)
                    return false;
                D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
                d.pRootSignature = g.computeRoot.Get();
                d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
                return Check(g.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), entry);
            };
            if (!makeCompute("resolve_main", g.resolvePso) || !makeCompute("resolve_scaled_main", g.resolveScaledPso) ||
                !makeCompute("clear_main", g.clearPso) || !makeCompute("untile_main", g.untilePso))
                return false;

            ComPtr<ID3DBlob> vs = CompileHlsl(d3d12src::kPresent, "vs_main", "vs_5_1");
            ComPtr<ID3DBlob> ps = CompileHlsl(d3d12src::kPresent, "ps_main", "ps_5_1");
            if (!vs || !ps)
                return false;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
            d.pRootSignature = g.presentRoot.Get();
            d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            DefaultStates(d, D3D12_COLOR_WRITE_ENABLE_ALL);
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            if (!Check(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g.presentPso)), "present pipeline"))
                return false;

            std::string gs = std::string(d3d12src::kRectGs);
            g_rectGs = CompileHlsl(gs, "main", "gs_5_1");

            // Pixel shader for draws without one (depth/stencil only).
            g_emptyPs.type = 1;
            g_emptyPs.key = 0;
            g_emptyPs.xlat.ok = true;
            g_emptyPs.xlat.tempCount = 16;
            g_emptyPs.xlat.body = "";
            CompileNow(g_emptyPs);
            return g_emptyPs.state == 2;
        }

        bool CreateSwapChainBuffers()
        {
            for (uint32_t i = 0; i < kFrames; i++)
            {
                g.backBuffers[i].Reset();
                if (!Check(g.swapChain->GetBuffer(i, IID_PPV_ARGS(&g.backBuffers[i])), "GetBuffer"))
                    return false;
                auto h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += SIZE_T(i) * g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                g.device->CreateRenderTargetView(g.backBuffers[i].Get(), nullptr, h);
            }
            return true;
        }

        // Records drawing the game's front buffer (in the view at descriptor
        // `view`) into the current back buffer, letterboxed.
        void RecordPresentBlit(ID3D12GraphicsCommandList* list, bool haveFront, uint32_t view, uint32_t swizzle, uint32_t aa,
                               uint32_t frontW, uint32_t frontH, uint32_t width, uint32_t height)
        {
            uint32_t bb = g.swapChain->GetCurrentBackBufferIndex();
            auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
                D3D12_RESOURCE_BARRIER rb = {};
                rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                rb.Transition.pResource = g.backBuffers[bb].Get();
                rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                rb.Transition.StateBefore = a;
                rb.Transition.StateAfter = b;
                list->ResourceBarrier(1, &rb);
            };
            barrier(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += SIZE_T(bb) * g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const float black[4] = { 0, 0, 0, 1 };
            list->ClearRenderTargetView(rtv, black, 0, nullptr);
            if (haveFront)
            {
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->SetGraphicsRootSignature(g.presentRoot.Get());
                list->SetPipelineState(g.presentPso.Get());
                list->SetGraphicsRootDescriptorTable(0, ViewGpu(view));
                // The game's front buffer may be stored with red and blue swapped;
                // its fetch constant swizzle says how to read it.
                uint32_t pc[4] = { swizzle, aa, 0, 0 };
                float texel[2] = { 1.0f / float(std::max<uint32_t>(frontW, 1)), 1.0f / float(std::max<uint32_t>(frontH, 1)) };
                memcpy(&pc[2], texel, 8);
                list->SetGraphicsRoot32BitConstants(1, 4, pc, 0);
                // Keep the aspect ratio of the game's output.
                float gw = float(std::max<uint32_t>(width, 1)), gh = float(std::max<uint32_t>(height, 1));
                float sw = float(g.swapW), sh = float(g.swapH);
                float w = sw, h = sw * gh / gw;
                if (h > sh) { h = sh; w = sh * gw / gh; }
                D3D12_VIEWPORT vp = { (sw - w) * 0.5f, (sh - h) * 0.5f, w, h, 0.0f, 1.0f };
                D3D12_RECT sc = { 0, 0, LONG(g.swapW), LONG(g.swapH) };
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
            }
            barrier(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }

        // ------------------------------------------------------------------
        // Presenter thread (in-between frames, see InterpState)
        // ------------------------------------------------------------------
        void PresenterWaitFence(uint64_t value)
        {
            if (g_interp.fence->GetCompletedValue() < value)
            {
                g_interp.fence->SetEventOnCompletion(value, g_interp.fenceEvent);
                WaitForSingleObject(g_interp.fenceEvent, INFINITE);
            }
        }

        // Sleeps until `when`, more precisely than the system timer alone.
        void SleepUntilPrecise(std::chrono::steady_clock::time_point when)
        {
            using namespace std::chrono;
            auto now = steady_clock::now();
            if (when - now > milliseconds(2))
                std::this_thread::sleep_until(when - milliseconds(1));
            while (steady_clock::now() < when)
                std::this_thread::yield();
        }

        // Executes the job's frame with blend factor t (1 = the real frame) and
        // presents it. Returns the fence value that marks its completion.
        uint64_t PresentUnit(const InterpState::Job& job, float t)
        {
            auto& ip = g_interp;
            uint32_t u = ip.unitNext;
            ip.unitNext = (ip.unitNext + 1) % InterpState::kUnits;
            auto& unit = ip.units[u];
            PresenterWaitFence(unit.fence);

            // Follow the window size.
            RECT rc;
            GetClientRect(g.hwnd, &rc);
            uint32_t cw = std::max<uint32_t>(uint32_t(rc.right - rc.left), 64), ch = std::max<uint32_t>(uint32_t(rc.bottom - rc.top), 64);
            if (cw != g.swapW || ch != g.swapH)
            {
                ip.fenceValue++;
                g.queue->Signal(ip.fence.Get(), ip.fenceValue);
                PresenterWaitFence(ip.fenceValue);
                for (auto& b : g.backBuffers) b.Reset();
                if (SUCCEEDED(g.swapChain->ResizeBuffers(kFrames, cw, ch, DXGI_FORMAT_R8G8B8A8_UNORM, g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
                {
                    g.swapW = cw;
                    g.swapH = ch;
                }
                CreateSwapChainBuffers();
            }

            unit.alloc->Reset();
            // Set the blend factor (vertex shaders read it as a constant buffer).
            float tv[4] = { t, 0.0f, 0.0f, 0.0f };
            memcpy(ip.tPtr + u * 256, tv, sizeof(tv));
            unit.pre->Reset(unit.alloc.Get(), nullptr);
            {
                D3D12_RESOURCE_BARRIER rb = {};
                rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                rb.Transition.pResource = ip.buf.Get();
                rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                unit.pre->ResourceBarrier(1, &rb);
                unit.pre->CopyBufferRegion(ip.buf.Get(), 0, ip.tUpload.Get(), u * 256, 16);
                std::swap(rb.Transition.StateBefore, rb.Transition.StateAfter);
                unit.pre->ResourceBarrier(1, &rb);
            }
            unit.pre->Close();
            unit.post->Reset(unit.alloc.Get(), nullptr);
            ID3D12DescriptorHeap* heaps[] = { g.viewHeap.Get(), g.samplerHeap.Get() };
            unit.post->SetDescriptorHeaps(2, heaps);
            RecordPresentBlit(unit.post.Get(), job.front, kDescPresent + job.slot, job.swizzle, job.aa, job.frontW, job.frontH, job.width, job.height);
            unit.post->Close();

            // The frame's list may only run once at a time: wait for the last
            // execution (this also keeps the GPU from falling behind).
            auto w0 = std::chrono::steady_clock::now();
            PresenterWaitFence(ip.fenceValue);
            auto w1 = std::chrono::steady_clock::now();
            ip.gpuWaitUs += uint64_t(std::chrono::duration<double, std::micro>(w1 - w0).count());
            // Two submissions: the factor's buffer returns to the common state
            // between them, where the frame's draws read it.
            ID3D12CommandList* pre[] = { unit.pre.Get() };
            g.queue->ExecuteCommandLists(1, pre);
            ID3D12CommandList* rest[] = { job.scene, unit.post.Get() };
            g.queue->ExecuteCommandLists(2, rest);
            bool vsync = settings::Get().vsync;
            HRESULT hr = g.swapChain->Present(vsync ? 1 : 0, (!vsync && g.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
            ip.presentUs += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w1).count());
            if (FAILED(hr))
                Check(hr, "Present");
            ip.fenceValue++;
            g.queue->Signal(ip.fence.Get(), ip.fenceValue);
            unit.fence = ip.fenceValue;
            ip.presents++;
            if (t < 1.0f)
                ip.interpolated++;
            return unit.fence;
        }

        void PresenterThread()
        {
            using clock = std::chrono::steady_clock;
            auto& ip = g_interp;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // even frame pacing
            double interval = 1.0 / 30.0;   // between game frames (seconds), smoothed
            double unitTime = 1.0 / 120.0;  // how long one execution takes (unlimited), smoothed
            clock::time_point lastArrival{};
            bool haveLast = false;
            for (;;)
            {
                InterpState::Job job;
                {
                    std::unique_lock lock(ip.mutex);
                    ip.cv.wait(lock, [&] { return !ip.jobs.empty(); });
                    job = ip.jobs.front();
                    ip.jobs.pop_front();
                    ip.busy = true;
                }
                if (haveLast)
                {
                    double d = std::chrono::duration<double>(job.arrival - lastArrival).count();
                    if (d > 0.005 && d < 0.2)
                        interval = interval * 0.9 + std::clamp(d, 1.0 / 60.0, 1.0 / 15.0) * 0.1;
                }
                lastArrival = job.arrival;
                haveLast = true;

                uint64_t done = 0;
                if (!job.replay)
                {
                    ip.unblended++;
                    done = PresentUnit(job, 1.0f);
                }
                else
                {
                    // Spread the presents over the time until the next game frame:
                    // at 60 fps t = 0.5 now and 1 (the real frame) half a game
                    // frame later; at 120, 0.25, 0.5, 0.75, 1.
                    uint32_t rateNow = ip.rate.load();
                    double step = rateNow ? 1.0 / double(rateNow) : 0.0;
                    auto start = clock::now();
                    for (uint32_t i = 0;; i++)
                    {
                        double elapsed = std::chrono::duration<double>(clock::now() - start).count();
                        double t = (elapsed + (step > 0 ? step : unitTime)) / interval;
                        bool newer;
                        {
                            std::lock_guard lock(ip.mutex);
                            newer = !ip.jobs.empty();
                        }
                        // The next game frame is already waiting: finish with the real one.
                        if (t > 0.97 || newer)
                            t = 1.0;
                        auto u0 = clock::now();
                        done = PresentUnit(job, float(t));
                        unitTime = unitTime * 0.8 + std::chrono::duration<double>(clock::now() - u0).count() * 0.2;
                        if (t >= 1.0)
                            break;
                        if (step > 0)
                            SleepUntilPrecise(start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(step * (i + 1))));
                    }
                }
                {
                    std::lock_guard lock(ip.mutex);
                    ip.slotFence[job.slot] = done;
                    ip.slotPending[job.slot] = false;
                    ip.busy = false;
                }
                ip.cv.notify_all();
            }
        }


        // Builds every shader and pipeline listed from earlier runs (see
        // "Shader warm-up"), showing progress in the game window.
        void WarmUpShaders()
        {
            using clock = std::chrono::steady_clock;
            auto t0 = clock::now();
            std::vector<GuestShader*> list;
            std::vector<std::vector<uint32_t>> codes;
            {
                std::ifstream in(ShaderManifestPath(), std::ios::binary);
                for (;;)
                {
                    uint32_t type = 0, n = 0;
                    uint64_t key = 0;
                    if (!in.read(reinterpret_cast<char*>(&type), 4) || !in.read(reinterpret_cast<char*>(&key), 8) ||
                        !in.read(reinterpret_cast<char*>(&n), 4) || type > 1 || n == 0 || n > (1u << 20))
                        break;
                    std::vector<uint32_t> code(n);
                    if (!in.read(reinterpret_cast<char*>(code.data()), std::streamsize(n) * 4))
                        break;
                    if (!g_manifestShaders.insert(key).second)
                        continue;
                    auto& slot = g_shaders[key];
                    slot = std::make_unique<GuestShader>();
                    slot->type = type;
                    slot->key = key;
                    slot->state = 1;
                    list.push_back(slot.get());
                    codes.push_back(std::move(code));
                }
            }
            std::vector<PipelineKey> keys;
            {
                std::ifstream in(PipelineManifestPath(), std::ios::binary);
                PipelineKey k;
                while (in.read(reinterpret_cast<char*>(&k), sizeof(k)))
                    if (g_manifestPipelines.insert(Fnv(&k, sizeof(k))).second)
                        keys.push_back(k);
            }
            if (list.empty())
                return;
            unsigned threads = std::max(2u, std::thread::hardware_concurrency()) - 1;
            auto status = [](const wchar_t* what, size_t done, size_t total) {
                wchar_t text[160];
                swprintf(text, 160, L"%ls  %zu / %zu", what, done, total);
                gpu::PresenterSetStatus(text);
            };

            // Translate and compile (compiled shaders already in the cache just load).
            std::atomic<size_t> next{ 0 }, done{ 0 };
            std::vector<std::thread> pool;
            for (unsigned i = 0; i < threads; i++)
                pool.emplace_back([&] {
                    for (size_t j; (j = next++) < list.size();)
                    {
                        GuestShader& sh = *list[j];
                        sh.xlat = xlat::Translate(sh.type == 1 ? xlat::ShaderType::kPixel : xlat::ShaderType::kVertex, codes[j].data(),
                                                  uint32_t(codes[j].size()));
                        CompileNow(sh);
                        done++;
                    }
                });
            while (done < list.size())
            {
                status(L"Preparing shaders", done, list.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            for (auto& t : pool)
                t.join();
            double shaderSecs = std::chrono::duration<double>(clock::now() - t0).count();

            // Pipelines: loaded from the pipeline cache, or made by the driver
            // on as many threads as there are cores.
            if (!g_psoThreadsStarted)
                g_psoThreadCount = threads;
            size_t made = 0;
            for (const PipelineKey& k : keys)
            {
                auto vs = g_shaders.find(k.vs);
                GuestShader* ps = k.ps == 0 ? &g_emptyPs : nullptr;
                if (k.ps != 0)
                    if (auto it = g_shaders.find(k.ps); it != g_shaders.end())
                        ps = it->second.get();
                if (vs == g_shaders.end() || !ps || vs->second->state != 2 || ps->state != 2)
                    continue;
                GetPipeline(k, *vs->second, *ps);
                made++;
            }
            size_t pendingStart = g_psoPending.load();
            while (uint32_t pending = g_psoPending.load())
            {
                status(L"Preparing pipelines", pendingStart - pending, pendingStart);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            SavePipelineLibrary();
            gpu::PresenterSetStatus(nullptr);
            LOG("[d3d12] shaders ready before starting: %zu shaders in %.1f s, %zu pipelines (%zu built by the driver) in %.1f s total",
                list.size(), shaderSecs, made, pendingStart, std::chrono::duration<double>(clock::now() - t0).count());
        }

        bool InitPresenter();
        bool InitInterp()
        {
            auto& ip = g_interp;
            uint32_t fr = settings::Get().frameRate;
            ip.on = fr != 30;
            ip.rate = fr;
            ip.buf = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
            ip.tUpload = CreateBuffer(256 * InterpState::kUnits + 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!ip.buf || !ip.tUpload)
                return false;
            D3D12_RANGE none = { 0, 0 };
            ip.tUpload->Map(0, &none, reinterpret_cast<void**>(&ip.tPtr));
            // The factor starts at 1 (real frames only), from the spare slot.
            float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
            memcpy(ip.tPtr + 256 * InterpState::kUnits, one, sizeof(one));
            Barrier(ip.buf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->CopyBufferRegion(ip.buf.Get(), 0, ip.tUpload.Get(), 256 * InterpState::kUnits, 16);
            Barrier(ip.buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            if (!ip.on)
                return true;
            if (!InitPresenter())
            {
                LOG("[d3d12] in-between frames unavailable; running at 30 fps");
                ip.on = false;
            }
            return true;
        }

        bool g_presenterReady = false;
        bool InitPresenter()
        {
            auto& ip = g_interp;
            uint32_t fr = ip.rate;
            if (g_presenterReady)
                return true;
            if (!Check(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ip.fence)), "CreateFence (presenter)"))
                return false;
            ip.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            for (auto& u : ip.units)
            {
                // A new list is open for recording, and an allocator can only
                // back one open list: close each before creating the next.
                if (!Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&u.alloc)), "CreateCommandAllocator (presenter)") ||
                    !Check(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, u.alloc.Get(), nullptr, IID_PPV_ARGS(&u.pre)), "CreateCommandList (presenter)"))
                    return false;
                u.pre->Close();
                if (!Check(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, u.alloc.Get(), nullptr, IID_PPV_ARGS(&u.post)), "CreateCommandList (presenter)"))
                    return false;
                u.post->Close();
            }
            ip.prevData.reserve(16u << 20);
            ip.curData.reserve(16u << 20);
            std::thread(PresenterThread).detach();
            g_presenterReady = true;
            if (fr)
                LOG("[d3d12] %u fps: in-between frames are blended from the game's 30", fr);
            else
                LOG("[d3d12] unlimited frame rate: in-between frames are blended from the game's 30");
            return true;
        }
    }

    // ----------------------------------------------------------------------
    bool Init(void* hwnd, const std::string& cacheDir)
    {
        g.hwnd = static_cast<HWND>(hwnd);
        g.cacheDir = cacheDir;
        g_resScale = std::clamp<uint32_t>(settings::Get().renderScale, 1, 4);
        g_edramSize = kEdramBaseSize * g_resScale * g_resScale;
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);

        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!compiler || !(g.compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"))))
        {
            LOG("[d3d12] d3dcompiler_47.dll not available");
            return false;
        }
        g_createBlob = reinterpret_cast<CreateBlobFn>(GetProcAddress(compiler, "D3DCreateBlob"));
#if XE_HAVE_DXC
        // DXC needs dxil.dll (signing) beside dxcompiler.dll.
        if (!getenv("VR_USE_FXC") && LoadLibraryW(L"dxil.dll"))
            if (HMODULE dxc = LoadLibraryW(L"dxcompiler.dll"))
                g_dxcCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc, "DxcCreateInstance"));
#endif
        LOG("[d3d12] shader compiler: %s", UsingDxc() ? "DXC (shader model 6)" : "fxc (shader model 5.1)");
        if (getenv("VR_D3D12_DEBUG"))
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
        }
        if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(&g.factory)), "CreateDXGIFactory1"))
            return false;
        if (!Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)), "D3D12CreateDevice"))
            return false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
        g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
        if (!opts.ROVsSupported)
        {
            LOG("[d3d12] the GPU doesn't support rasterizer ordered views; using the software renderer");
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Check(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue"))
            return false;

        RECT rc;
        GetClientRect(g.hwnd, &rc);
        g.swapW = std::max<uint32_t>(uint32_t(rc.right - rc.left), 64);
        g.swapH = std::max<uint32_t>(uint32_t(rc.bottom - rc.top), 64);
        {
            ComPtr<IDXGIFactory5> f5;
            BOOL allow = FALSE;
            if (SUCCEEDED(g.factory.As(&f5)) &&
                SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                g.tearing = allow != FALSE;
        }
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = g.swapW;
        sd.Height = g.swapH;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFrames;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // The game paces itself with its own (emulated) vblank; presenting must
        // not also wait for the monitor or the command processor falls behind.
        sd.Flags = g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        if (!Check(g.factory->CreateSwapChainForHwnd(g.queue.Get(), g.hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd"))
            return false;
        g.factory->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (!Check(sc1.As(&g.swapChain), "IDXGISwapChain3"))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kFrames;
        if (!Check(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)), "RTV heap") || !CreateSwapChainBuffers())
            return false;

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kViewDescriptors;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.viewHeap)), "view heap"))
            return false;
        hd.NumDescriptors = 16384;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (!Check(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.cpuViewHeap)), "CPU view heap"))
            return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = kSamplers;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.samplerHeap)), "sampler heap"))
            return false;
        g.viewIncrement = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g.samplerIncrement = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        for (uint32_t i = 0; i < kFrames; i++)
        {
            if (!Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocators[i])), "CreateCommandAllocator"))
                return false;
            g.upload[i] = CreateBuffer(kUploadSize + (64u << 10), /* slack: constant buffers are 4 KB even when fewer constants were written */ D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!g.upload[i])
                return false;
            D3D12_RANGE none = { 0, 0 };
            g.upload[i]->Map(0, &none, reinterpret_cast<void**>(&g.uploadPtr[i]));
        }
        for (uint32_t i = 0; i < kFrames; i++)
        {
            if (!Check(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocators[i].Get(), nullptr, IID_PPV_ARGS(&g.lists[i])), "CreateCommandList"))
                return false;
            if (i > 0)
                g.lists[i]->Close();
        }
        g.list = g.lists[0];
        if (!Check(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "CreateFence"))
            return false;
        g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        g.shared = CreateBuffer(kSharedSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        g.edram = CreateBuffer(g_edramSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.staging = CreateBuffer(kStagingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!g.shared || !g.edram || !g.staging)
            return false;
        {
            // Other aspect ratios render more pixels (e.g. 1680x720 or 1280x960):
            // the pool grows with the picture's area, in whole MB.
            float a = settings::RenderAspect(), wide = 16.0f / 9.0f;
            double area = a > wide ? a / wide : wide / a;
            uint64_t pool = uint64_t(double(kScaledPoolBase) * std::max(1.0, area)) & ~((1ull << 20) - 1);
            g.scaledPoolSize = g_resScale > 1 ? pool * g_resScale * g_resScale : 256;
        }
        g.scaledPool = CreateBuffer(g.scaledPoolSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!g.scaledPool)
            return false;
        g.sharedState = D3D12_RESOURCE_STATE_COMMON;
        g_pages.assign(kPageCount, Page());

        // Static descriptors.
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = UINT(kSharedSize / 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        g.device->CreateShaderResourceView(g.shared.Get(), &srv, ViewCpu(kDescSharedSrv));
        auto rawUav = [](ID3D12Resource* res, uint64_t size, uint32_t slot) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
            u.Format = DXGI_FORMAT_R32_TYPELESS;
            u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            u.Buffer.NumElements = UINT(size / 4);
            u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            g.device->CreateUnorderedAccessView(res, nullptr, &u, ViewCpu(slot));
        };
        rawUav(g.edram.Get(), g_edramSize, kDescEdramUavDraw);
        rawUav(g.edram.Get(), g_edramSize, kDescEdramUav);
        rawUav(g.shared.Get(), kSharedSize, kDescSharedUav);
        rawUav(g.staging.Get(), kStagingSize, kDescStagingUav);
        rawUav(g.scaledPool.Get(), g.scaledPoolSize, kDescScaledUav);
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
        nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels = 1;
        for (uint32_t i = 0; i < 32; i++)
            g.device->CreateShaderResourceView(nullptr, &nullSrv, CpuView(i));
        nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        nullSrv.TextureCube.MipLevels = 1;
        g_nullCubeSrv = AllocCpuView();
        g.device->CreateShaderResourceView(nullptr, &nullSrv, CpuView(g_nullCubeSrv));

        LOG("[d3d12] compiling built-in shaders (slow on the first run only)...");
        if (!CreateRootSignatures() || !CreateFixedPipelines())
            return false;

        SetHeaps();
        ScanModTextures();
        if (!InitInterp())
            return false;
        InitPipelineLibrary();
        g_active = true;
        WarmUpShaders();
        D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
        LOG("[d3d12] Direct3D 12 renderer ready (%ux%u window, vsync %s%s)", g.swapW, g.swapH, settings::Get().vsync ? "on" : "off", g.tearing ? "" : ", tearing unsupported");
        (void)arch;
        return true;
    }

    bool Active() { return g_active; }
    void RequestCapture() { g_captureRequest = true; }
    void NoteCpuSync() { g_syncEpoch++; g_syncsThisPeriod++; }
    void SetRenderSize(uint32_t width, uint32_t height) { g_renderW = width; g_renderH = height; }
    void SetFrameRate(uint32_t fps) { g_frameRateRequest = int64_t(fps); }
    void RequestTextureDump() { g_mods.dumpRequest = true; }
    void ReloadModTextures() { g_mods.reloadRequest = true; }
    bool Capturing() { return g_capturing; }

    void LoadShader(uint32_t type, const std::vector<uint32_t>& ucode, uint64_t key)
    {
        if (!g_active || type > 1)
            return;
        auto& slot = g_shaders[key];
        if (!slot)
        {
            slot = std::make_unique<GuestShader>();
            slot->type = type;
            slot->key = key;
            slot->xlat = xlat::Translate(type == 1 ? xlat::ShaderType::kPixel : xlat::ShaderType::kVertex, ucode.data(), uint32_t(ucode.size()));
            RecordShader(type, key, ucode);
        }
        (type == 0 ? g_vs : g_ps) = slot.get();
    }

    // ----------------------------------------------------------------------
    // Draws
    // ----------------------------------------------------------------------
    static const char* DrawImpl(const RegisterFile& regs, uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize)
    {
        if (!g_active || !g_vs)
            return "no vertex shader";
        struct Timer
        {
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~Timer() { g.tDraw += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
        } timer;
        reg::VGT_DRAW_INITIATOR init;
        init.value = drawInitiator;
        uint32_t count = init.num_indices;
        if (!count)
            return "zero vertices";

        auto modeControl = regs.Get<reg::RB_MODECONTROL>();
        auto surfaceInfo = regs.Get<reg::RB_SURFACE_INFO>();
        auto programCntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
        auto contextMisc = regs.Get<reg::SQ_CONTEXT_MISC>();
        auto scMode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        auto clipCntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
        auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
        auto depthControl = regs.Get<reg::RB_DEPTHCONTROL>();

        bool colorPass = modeControl.edram_mode == xenos::EdramMode::kColorDepth && g_ps;
        uint32_t colorMask = regs[XE_GPU_REG_RB_COLOR_MASK];
        bool depthUsed = depthControl.z_enable || depthControl.stencil_enable;
        uint32_t rtFlags = 0;
        uint32_t sys[kSysSlots * 4] = {};
        static const Register kBlendRegs[4] = { XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2, XE_GPU_REG_RB_BLENDCONTROL3 };
        for (int i = 0; i < 4; i++)
        {
            if (!colorPass || !((colorMask >> (4 * i)) & 15))
                continue;
            if (!(g_ps->xlat.exportMask & (1u << i)))
                continue;
            auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            rtFlags |= 1u << i;
            uint32_t blend = regs[kBlendRegs[i]];
            // src=One, dst=Zero, op=Add for both color and alpha: no blending.
            if ((blend & 0x1FFF1FFF) != 0x00010001)
                rtFlags |= 16u << i;
            sys[5 * 4 + i] = ci.color_base;
            sys[6 * 4 + i] = uint32_t(ci.color_format);
            sys[7 * 4 + i] = blend;
        }
        if (colorPass && rtFlags)
            rtFlags |= 0x100;
        if (!(rtFlags & 0x100) && !depthUsed)
            return "no color or depth output";

        // The pixel shader also runs in depth-only passes (it may kill pixels).
        GuestShader* ps = g_ps ? g_ps : &g_emptyPs;
        // Queue both before checking (no short-circuit), so they compile together.
        bool vsReady = EnsureCompiled(*g_vs);
        bool psReady = EnsureCompiled(*ps);
        if (!vsReady || !psReady)
            return (g_vs->state == 3 || ps->state == 3) ? "shader failed" : "shader compiling";

        // --- Primitive type ---
        uint32_t topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        D3D_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        bool rect = false, convert = false;
        switch (init.prim_type)
        {
        case xenos::PrimitiveType::kPointList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case xenos::PrimitiveType::kLineList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case xenos::PrimitiveType::kLineStrip: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case xenos::PrimitiveType::kTriangleList: break;
        case xenos::PrimitiveType::kTriangleStrip: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case xenos::PrimitiveType::kRectangleList: rect = true; break;
        case xenos::PrimitiveType::kTriangleFan: case xenos::PrimitiveType::kQuadList: convert = true; break;
        default:
        {
            static int logged = 0;
            if (logged++ < 10)
                LOG("[d3d12] primitive type %u not supported", uint32_t(init.prim_type));
            return "unsupported primitive";
        }
        }

        // --- Guest memory used by the draw ---
        bool indexed = init.source_select == xenos::SourceSelect::kDMA;
        bool is32 = init.index_size == xenos::IndexFormat::kInt32;
        reg::VGT_DMA_SIZE ds;
        ds.value = dmaSize;
        uint32_t indexBytes = count * (is32 ? 4 : 2);
        uint32_t indexAddr = dmaBase & ~(is32 ? 3u : 1u) & 0x1FFFFFFF;
        if (indexed)
            EnsureRange(indexAddr, indexBytes, indexBytes <= (256u << 10));
        for (uint32_t w = 0; w < 3; w++)
        {
            uint32_t m = g_vs->xlat.vertexFetchMask[w];
            while (m)
            {
                uint32_t bit = 31 - __builtin_clz(m);
                m &= ~(1u << bit);
                auto vf = regs.GetVertexFetch(w * 32 + bit);
                // Small (dynamic) buffers are rechecked every draw; large
                // static ones once per frame, to keep hashing cheap.
                EnsureRange(vf.address * 4, vf.size * 4, vf.size * 4 <= (256u << 10));
            }
        }

        // Fans and quad lists become triangle lists of the original indices.
        D3D12_INDEX_BUFFER_VIEW ibv = {};
        std::vector<uint32_t> convIndices;
        uint32_t drawCount = count;
        uint32_t indexEndian = 0;
        if (convert)
        {
            std::vector<uint32_t> src(count);
            for (uint32_t i = 0; i < count; i++)
            {
                if (!indexed) { src[i] = i; continue; }
                uint32_t v;
                if (is32)
                {
                    memcpy(&v, PhysPtr(indexAddr + i * 4), 4);
                    if (ds.swap_mode == xenos::Endian::k8in32) v = _byteswap_ulong(v);
                }
                else
                {
                    uint16_t s16;
                    memcpy(&s16, PhysPtr(indexAddr + i * 2), 2);
                    if (ds.swap_mode == xenos::Endian::k8in16 || ds.swap_mode == xenos::Endian::k8in32) s16 = _byteswap_ushort(s16);
                    v = s16;
                }
                src[i] = v;
            }
            std::vector<uint32_t> out;
            if (init.prim_type == xenos::PrimitiveType::kTriangleFan)
                for (uint32_t i = 2; i < count; i++) { out.push_back(src[0]); out.push_back(src[i - 1]); out.push_back(src[i]); }
            else
                for (uint32_t i = 0; i + 3 < count; i += 4)
                {
                    out.push_back(src[i]); out.push_back(src[i + 1]); out.push_back(src[i + 2]);
                    out.push_back(src[i]); out.push_back(src[i + 2]); out.push_back(src[i + 3]);
                }
            if (out.empty())
                return "skipped (1)";
            convIndices = std::move(out); // uploaded with the constants below
            ibv.SizeInBytes = UINT(convIndices.size() * 4);
            ibv.Format = DXGI_FORMAT_R32_UINT;
            drawCount = uint32_t(convIndices.size());
            indexed = true;
        }
        else if (indexed)
        {
            ibv.BufferLocation = g.shared->GetGPUVirtualAddress() + indexAddr;
            ibv.SizeInBytes = indexBytes;
            ibv.Format = is32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            if (ds.swap_mode == xenos::Endian::k8in16 || (!is32 && ds.swap_mode == xenos::Endian::k8in32))
                indexEndian = 1;
            else if (is32 && ds.swap_mode == xenos::Endian::k8in32)
                indexEndian = 2;
        }

        // --- Textures and samplers ---
        uint32_t texMask = g_vs->xlat.textureFetchMask | ps->xlat.textureFetchMask;
        uint32_t srvs[32];
        std::fill(std::begin(srvs), std::end(srvs), UINT32_MAX);
        auto tt0 = std::chrono::steady_clock::now();
        static const D3D12_SAMPLER_DESC kDefaultSampler = SamplerFor(xenos::xe_gpu_texture_fetch_t{});
        D3D12_SAMPLER_DESC samplers[32];
        for (uint32_t i = 0; i < 32; i++)
            samplers[i] = kDefaultSampler;
        // Slots the shaders sample as cube maps (FetchOpDimension 3).
        uint32_t cubeMask = 0;
        for (uint32_t m = texMask; m; m &= m - 1)
        {
            uint32_t i = uint32_t(__builtin_ctz(m));
            if (((g_vs->xlat.textureFetchMask >> i) & 1 && g_vs->xlat.textureDimension[i] == 3) ||
                ((ps->xlat.textureFetchMask >> i) & 1 && ps->xlat.textureDimension[i] == 3))
                cubeMask |= 1u << i;
        }
        for (uint32_t m = cubeMask; m; m &= m - 1)
            srvs[__builtin_ctz(m)] = g_nullCubeSrv;
        for (uint32_t m = texMask; m; m &= m - 1)
        {
            uint32_t i = uint32_t(__builtin_ctz(m));
            auto f = regs.GetTextureFetch(i);
            if (f.type != xenos::FetchConstantType::kTexture)
                continue;
            Texture* t = GetTexture(f);
            // A cube map only fits a slot sampled as one, and the other way round.
            if (t && t->cube != (((cubeMask >> i) & 1) != 0))
                t = nullptr;
            uint32_t* info = &sys[(11 + i) * 4];
            info[0] = f.swizzle;
            info[1] = uint32_t(f.sign_x) | (uint32_t(f.sign_y) << 2) | (uint32_t(f.sign_z) << 4) | (uint32_t(f.sign_w) << 6);
            info[2] = uint32_t(int32_t(f.exp_adjust));
            if (t)
            {
                float* size = reinterpret_cast<float*>(&sys[(43 + i) * 4]);
                info[3] = t->fmt.flags;
                size[0] = t->replaced ? 1.0f : float(t->width * t->scale) / float(t->hostW);
                size[1] = t->replaced ? 1.0f : float(t->height * t->scale) / float(t->hostH);
                if (g_mods.dumping && !t->replaced && !t->cube && f.dimension == xenos::DataDimension::k2DOrStacked)
                {
                    bool seen = false;
                    for (auto& e : g_mods.dumpList)
                        seen = seen || e.tex == t;
                    if (!seen)
                        g_mods.dumpList.push_back({ t, uint32_t(f.format), f.size_2d.width + 1u, f.size_2d.height + 1u });
                }
                size[2] = 1.0f / float(t->width);
                size[3] = 1.0f / float(t->height);
                srvs[i] = t->srv;
            }
            samplers[i] = SamplerFor(f);
        }
        // Sampler heap index per slot (slots without a texture: the default).
        {
            uint32_t defIdx = GetSamplerIndex(kDefaultSampler);
            for (uint32_t i = 0; i < 32; i++)
                sys[75 * 4 + i] = (texMask >> i) & 1u ? GetSamplerIndex(samplers[i]) : defIdx;
        }
        uint64_t texKey = Fnv(&texMask, sizeof(texMask));
        for (uint32_t m = texMask; m; m &= m - 1)
            texKey = Fnv(&srvs[__builtin_ctz(m)], 4, texKey);
        g.tTex += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tt0).count();

        // --- System constants ---
        auto rf = [&](Register r) { float f; memcpy(&f, &regs[r], 4); return f; };
        float* sysf = reinterpret_cast<float*>(sys);
        sysf[0] = vte.vport_x_scale_ena ? rf(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
        sysf[1] = vte.vport_y_scale_ena ? rf(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
        sysf[2] = vte.vport_z_scale_ena ? rf(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
        bool clip = !clipCntl.clip_disable && !vte.vtx_xy_fmt;
        sys[3] = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 4u : 0u) | (clip ? 8u : 0u);
        float halfPixel = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
        auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
        float winX = scMode.vtx_window_offset_enable ? float(wo.window_x_offset) : 0.0f;
        float winY = scMode.vtx_window_offset_enable ? float(wo.window_y_offset) : 0.0f;
        sysf[4] = (vte.vport_x_offset_ena ? rf(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f) + halfPixel + winX;
        sysf[5] = (vte.vport_y_offset_ena ? rf(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f) + halfPixel + winY;
        sysf[6] = vte.vport_z_offset_ena ? rf(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
        sys[7] = convert ? 0u : indexEndian;
        sys[8] = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
        sys[9] = regs[XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF;
        sys[10] = regs[XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
        auto vsConst = regs.Get<reg::SQ_VS_CONST>(XE_GPU_REG_SQ_VS_CONST);
        auto psConst = regs.Get<reg::SQ_VS_CONST>(XE_GPU_REG_SQ_PS_CONST);
        sys[11] = vsConst.size;
        sys[12] = psConst.size;
        sys[13] = std::min<uint32_t>({ 16u, programCntl.ps_num_reg + 1u, programCntl.vs_export_count + 1u });
        sys[14] = (programCntl.param_gen ? 1u : 0u) | (uint32_t(contextMisc.param_gen_pos) << 8);
        sys[15] = xenos::GetSurfacePitchTiles(surfaceInfo.surface_pitch, surfaceInfo.msaa_samples, false);
        sys[16] = (surfaceInfo.msaa_samples >= xenos::MsaaSamples::k4X ? 1u : 0u) | (surfaceInfo.msaa_samples >= xenos::MsaaSamples::k2X ? 2u : 0u);
        sys[17] = colorMask;
        sys[18] = rtFlags;
        auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
        sys[19] = (depthUsed ? 1u : 0u) | (depthInfo.depth_format == xenos::DepthRenderTargetFormat::kD24FS8 ? 2u : 0u);
        for (int i = 0; i < 4; i++)
            sys[8 * 4 + i] = regs[XE_GPU_REG_RB_BLEND_RED + i];
        sys[36] = depthInfo.depth_base;
        sys[37] = depthControl.value;
        sys[38] = regs[XE_GPU_REG_RB_STENCILREFMASK];
        sys[39] = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
        auto colorControl = regs.Get<reg::RB_COLORCONTROL>();
        sys[40] = uint32_t(colorControl.alpha_func);
        sys[41] = regs[XE_GPU_REG_RB_ALPHA_REF];
        sys[42] = colorControl.alpha_test_enable ? 1u : 0u;

        // HUD fit (other aspect ratios): a draw through a viewport for the
        // original 1280-wide (or 720-tall) screen is HUD or a full-screen
        // effect laid out for 1280x720; the vertex shader moves it into a
        // centred 1280x720 box, edge points to the real edges.
        bool hud = false;
        float hudDx = 0, hudDy = 0;
        if (g_renderW != 1280 || g_renderH != 720)
        {
            float vxs = rf(XE_GPU_REG_PA_CL_VPORT_XSCALE), vxo = rf(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
            float vys = rf(XE_GPU_REG_PA_CL_VPORT_YSCALE), vyo = rf(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
            if (g_renderW != 1280)
                hud = vte.vport_x_scale_ena && std::fabs(vxs - 640.0f) < 1.0f && std::fabs(vxo - 640.0f) < 1.5f;
            else
                hud = vte.vport_y_scale_ena && std::fabs(std::fabs(vys) - 360.0f) < 9.0f && std::fabs(vyo - std::fabs(vys)) < 1.5f;
            // Most of the HUD maps its 1280x720 pixel layout 1:1 onto the
            // whole screen instead: flat (w = 1) geometry drawn without depth
            // testing into a full-size surface. Marked 2 - the vertex shader
            // then fits only vertices with w = 1, leaving any 3D alone.
            uint32_t mark = hud ? 1u : 0u;
            // (The surface's pitch is the width rounded up - to 32 pixels for
            // the back buffer, then to whole 80-pixel EDRAM tiles: 1760 for 1680.)
            uint32_t pitch = surfaceInfo.surface_pitch;
            if (!hud && !depthControl.z_enable && (rtFlags & 0x100) && pitch >= g_renderW && pitch <= g_renderW + 160)
            {
                hud = true;
                mark = 2;
            }
            if (hud)
            {
                hudDx = (float(g_renderW) - 1280.0f) * 0.5f;
                hudDy = (float(g_renderH) - 720.0f) * 0.5f;
                float* f = reinterpret_cast<float*>(sys);
                sys[83 * 4] = mark;
                f[83 * 4 + 1] = hudDx;
                f[83 * 4 + 2] = hudDy;
                f[83 * 4 + 3] = winX;
                f[84 * 4 + 0] = float(g_renderW);
                f[84 * 4 + 1] = float(g_renderH);
                f[84 * 4 + 2] = 1280.0f;
                f[84 * 4 + 3] = winY;
            }
        }

        // --- Upload constants (one allocation, so a mid-draw flush can't
        // invalidate part of them) ---
        // Float constants: only as many as the shader reads (a root CBV has no
        // size, and the shader never reads past them).
        constexpr uint64_t kSysBytes = sizeof(sys), kMiscBytes = (48 + 2 + 8) * 16;
        auto align256 = [](uint64_t v) { return (v + 255) & ~uint64_t(255); };
        uint64_t vsBytes = align256(std::max<uint32_t>(g_vs->xlat.floatConstCount, 1) * 16);
        uint64_t psBytes = align256(std::max<uint32_t>(ps->xlat.floatConstCount, 1) * 16);
        uint64_t vsRel = align256(kSysBytes), psRel = vsRel + vsBytes, miscRel = psRel + psBytes;
        // In-between frames: room for the matching draw's constants from the
        // previous game frame (see InterpState).
        uint64_t prevRel = miscRel + align256(kMiscBytes);
        uint32_t vsUsedBytes = std::max<uint32_t>(g_vs->xlat.floatConstCount, 1) * 16;
        uint64_t interpSig = 0;
        uint32_t interpOrdinal = 0;
        const InterpState::Entry* prevEntry = nullptr;
        if (g_interp.on)
        {
            // Same shaders, index buffer, primitive, target and first vertex
            // buffer; the n-th such draw this frame matches the n-th last frame.
            uint64_t h = Fnv(&g_vs->key, 8);
            h = Fnv(&ps->key, 8, h);
            h = Fnv(&drawInitiator, 4, h);
            h = Fnv(&dmaBase, 4, h);
            uint32_t surf = surfaceInfo.value;
            h = Fnv(&surf, 4, h);
            h = Fnv(&sys[5 * 4], 16, h); // colour target bases
            for (uint32_t w = 0; w < 3; w++)
                if (uint32_t m = g_vs->xlat.vertexFetchMask[w])
                {
                    uint32_t idx = w * 32 + uint32_t(std::countr_zero(m));
                    uint32_t addr = regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + idx * 2] & ~3u;
                    h = Fnv(&addr, 4, h);
                    break;
                }
            interpSig = h;
            interpOrdinal = g_interp.occurrences[h]++;
        }
        bool interpUsed = false;
        if (g_interp.on && g_vs->xlat.floatConstCount)
        {
            // This draw's constants, as copyFloats uploads them below.
            uint32_t used = g_vs->xlat.floatConstCount;
            uint32_t first = std::min<uint32_t>(vsConst.base, 512);
            uint32_t n = std::min<uint32_t>({ 256u, 512 - first, used });
            const uint8_t* cur = reinterpret_cast<const uint8_t*>(&regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * first]);
            static std::vector<uint8_t> padded;
            if (n < used)
            {
                padded.assign(size_t(used) * 16, 0);
                memcpy(padded.data(), cur, size_t(n) * 16);
                cur = padded.data();
            }
            // The previous frame's draw with this signature whose constants
            // are closest (draws of one mesh may come in a different order,
            // e.g. sorted by distance): look near the same position in the
            // order, stopping a comparison once it's worse than the best.
            auto git = g_interp.prev.find(interpSig);
            if (git != g_interp.prev.end())
            {
                auto& group = git->second;
                const float* b = reinterpret_cast<const float*>(cur);
                uint32_t count = vsUsedBytes / 4;
                uint32_t lo = interpOrdinal > 24 ? interpOrdinal - 24 : 0;
                uint32_t hi = std::min<uint32_t>(uint32_t(group.size()), interpOrdinal + 25);
                float best = INFINITY;
                InterpState::Entry* bestEntry = nullptr;
                // Try the same position first: usually it's an exact or near match.
                auto consider = [&](InterpState::Entry& cand) {
                    if (cand.used || cand.bytes != vsUsedBytes)
                        return;
                    const float* a = reinterpret_cast<const float*>(g_interp.prevData.data() + cand.offset);
                    float d = 0.0f;
                    for (uint32_t i = 0; i < count && d < best; i += 8)
                    {
                        uint32_t e = std::min(count, i + 8);
                        for (uint32_t j = i; j < e; j++)
                            d += std::fabs(a[j] - b[j]);
                    }
                    if (d < best)
                    {
                        best = d;
                        bestEntry = &cand;
                    }
                };
                if (interpOrdinal < group.size())
                    consider(group[interpOrdinal]);
                for (uint32_t k = lo; k < hi && best > 0.0f; k++)
                    if (k != interpOrdinal)
                        consider(group[k]);
                if (bestEntry)
                {
                    bestEntry->used = true;
                    prevEntry = bestEntry;
                }
            }
            if (prevEntry && memcmp(g_interp.prevData.data() + prevEntry->offset, cur, vsUsedBytes) != 0)
            {
                // Blend only if the change is plausible motion: a big jump (a
                // camera cut, an object reused elsewhere) shows the new values.
                const float* a = reinterpret_cast<const float*>(g_interp.prevData.data() + prevEntry->offset);
                const float* b = reinterpret_cast<const float*>(cur);
                uint32_t jumps = 0;
                for (uint32_t i = 0; i < vsUsedBytes / 4; i++)
                {
                    float fa = std::fabs(a[i]), fb = std::fabs(b[i]);
                    // (written so a NaN counts as a jump)
                    jumps += !(std::fabs(a[i] - b[i]) <= 0.5f * (std::max(fa, fb) + 1.0f));
                }
                g_interp.matched++;
                if (jumps)
                    g_interp.cuts++;
                else
                    interpUsed = true;
            }
            else if (prevEntry)
                g_interp.matched++;
            g_interp.cur[interpSig].push_back({ uint32_t(g_interp.curData.size()), vsUsedBytes, false });
            g_interp.curData.insert(g_interp.curData.end(), cur, cur + vsUsedBytes);
        }
        // Room for the previous constants only when they're used.
        uint64_t allocBytes = interpUsed ? prevRel + vsBytes : prevRel;
        uint64_t base = 0;
        uint8_t* p = nullptr;
        uint32_t texTable = 0;
        g.useSeq++;
        // Upload memory and descriptor tables are void after a flush (a ring
        // ran out mid-frame): allocate everything again if one happened.
        for (int attempt = 0; attempt < 3; attempt++)
        {
        uint32_t flushesBefore = g.flushCount;
        p = UploadAlloc(allocBytes, 256, base);
        if (!p)
            return "skipped (3)";
        memcpy(p, sys, kSysBytes);
        // Only the constants the shader reads (everything if it indexes them).
        auto copyFloats = [&](const reg::SQ_VS_CONST& c, uint8_t* d, uint32_t used) {
            uint32_t first = std::min<uint32_t>(c.base, 512);
            uint32_t n = std::min<uint32_t>({ 256u, 512 - first, used });
            memcpy(d, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * first], n * 16);
            if (n < used)
                memset(d + n * 16, 0, (used - n) * 16);
        };
        copyFloats(vsConst, p + vsRel, g_vs->xlat.floatConstCount);
        copyFloats(psConst, p + psRel, ps->xlat.floatConstCount);
        uint8_t* m = p + miscRel;
        memcpy(m, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 192 * 4);
        memcpy(m + 192 * 4, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 8 * 4);
        memcpy(m + 200 * 4, &regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 32 * 4);
        if (!convIndices.empty())
        {
            uint64_t off;
            uint8_t* ip = UploadAlloc(convIndices.size() * 4, 4, off);
            if (!ip)
                return "skipped (2)";
            memcpy(ip, convIndices.data(), convIndices.size() * 4);
            ibv.BufferLocation = UploadGpu(off);
        }
        texTable = GetTexTable(texKey, texMask, srvs);
        if (g.flushCount == flushesBefore)
            break;
        }
        uint64_t sysOff = base, vsOff = base + vsRel, psOff = base + psRel, miscOff = base + miscRel;
        uint64_t prevOff = vsOff;
        if (interpUsed)
        {
            memcpy(p + prevRel, g_interp.prevData.data() + prevEntry->offset, vsUsedBytes);
            if (vsBytes > vsUsedBytes)
                memset(p + prevRel + vsUsedBytes, 0, vsBytes - vsUsedBytes);
            prevOff = base + prevRel;
        }

        // --- Pipeline ---
        PipelineKey key = {};
        key.vs = g_vs->key;
        key.ps = ps->key;
        key.topology = topoType;
        key.rect = rect ? 1 : 0;
        key.cull = (rect || topoType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) ? D3D12_CULL_MODE_NONE
                   : scMode.cull_front && scMode.cull_back ? D3D12_CULL_MODE_NONE  // both: nothing drawn, handled below
                   : scMode.cull_front ? D3D12_CULL_MODE_FRONT
                   : scMode.cull_back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        if (scMode.cull_front && scMode.cull_back && topoType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && !rect)
            return "skipped (4)";
        key.frontCcw = scMode.face ? 0 : 1; // face: 0 = front is counter-clockwise
        key.cut = (scMode.multi_prim_ib_ena && !convert && topo == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
                      ? (is32 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF)
                      : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        if (key.cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED && topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP)
            key.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        ID3D12PipelineState* pso = GetPipeline(key, *g_vs, *ps);
        if (!pso)
            return "pipeline being created";

        // --- Scissor ---
        auto wtl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
        auto wbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
        int32_t sx0 = wtl.tl_x, sy0 = wtl.tl_y, sx1 = wbr.br_x, sy1 = wbr.br_y;
        if (hud)
        {
            // The HUD's whole-screen scissor covers the real screen; smaller
            // ones (clipping a panel) move with the HUD.
            bool whole = sx0 <= 0 && sy0 <= 0 && sx1 >= 1280 && sy1 >= 720;
            if (whole)
            {
                sx1 = std::max<int32_t>(sx1, int32_t(g_renderW));
                sy1 = std::max<int32_t>(sy1, int32_t(g_renderH));
            }
            else
            {
                sx0 += int32_t(hudDx); sx1 += int32_t(hudDx);
                sy0 += int32_t(hudDy); sy1 += int32_t(hudDy);
            }
        }
        if (!wtl.window_offset_disable)
        {
            sx0 += wo.window_x_offset; sx1 += wo.window_x_offset;
            sy0 += wo.window_y_offset; sy1 += wo.window_y_offset;
        }
        uint32_t stl = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL], sbr = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR];
        sx0 = std::max<int32_t>({ sx0, int32_t(stl & 0x7FFF), 0 });
        sy0 = std::max<int32_t>({ sy0, int32_t((stl >> 16) & 0x7FFF), 0 });
        sx1 = std::min<int32_t>({ sx1, int32_t(sbr & 0x7FFF), int32_t(surfaceInfo.surface_pitch), 8192 });
        sy1 = std::min<int32_t>({ sy1, int32_t((sbr >> 16) & 0x7FFF), 8192 });
        if (sx0 >= sx1 || sy0 >= sy1)
            return "empty scissor";

        // --- Record ---
        SharedTo(kSharedReadState);
        g.list->SetGraphicsRootSignature(g.drawRoot.Get());
        g.list->SetPipelineState(pso);
        g.list->SetGraphicsRootConstantBufferView(kRootSys, UploadGpu(sysOff));
        g.list->SetGraphicsRootConstantBufferView(kRootVsConsts, UploadGpu(vsOff));
        g.list->SetGraphicsRootConstantBufferView(kRootPsConsts, UploadGpu(psOff));
        g.list->SetGraphicsRootConstantBufferView(kRootMisc, UploadGpu(miscOff));
        g.list->SetGraphicsRootConstantBufferView(kRootVsPrev, UploadGpu(prevOff));
        g.list->SetGraphicsRootConstantBufferView(kRootInterp, g_interp.buf->GetGPUVirtualAddress());
        g.list->SetGraphicsRootDescriptorTable(kRootMemory, ViewGpu(kDescSharedSrv));
        g.list->SetGraphicsRootDescriptorTable(kRootTextures, ViewGpu(texTable));
        auto sh = g.samplerHeap->GetGPUDescriptorHandleForHeapStart();
        g.list->SetGraphicsRootDescriptorTable(kRootSamplers, sh);
        // Host pixels = guest pixels x the render resolution scale.
        float vpSize = float(std::min<uint32_t>(8192 * g_resScale, 16384));
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, vpSize, vpSize, 0.0f, 1.0f };
        LONG S = LONG(g_resScale);
        D3D12_RECT sc = { sx0 * S, sy0 * S, std::min<LONG>(sx1 * S, LONG(vpSize)), std::min<LONG>(sy1 * S, LONG(vpSize)) };
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->IASetPrimitiveTopology(topo);
        if (indexed)
        {
            g.list->IASetIndexBuffer(&ibv);
            g.list->DrawIndexedInstanced(drawCount, 1, 0, 0, 0);
        }
        else
            g.list->DrawInstanced(drawCount, 1, 0, 0);
        return "drawn";
    }


    // ---- Frame capture (F12): logs every draw/resolve of one frame ----

    static const char* FormatName(uint32_t f)
    {
        static const char* kNames[] = { "8888", "8888_GAMMA", "2_10_10_10", "2_10_10_10_FLOAT", "16_16", "16_16_16_16",
                                        "16_16_FLOAT", "16_16_16_16_FLOAT", "?8", "?9", "2_10_10_10_AS_10_10_10_10", "?11",
                                        "2_10_10_10_FLOAT_AS_16_16_16_16", "?13", "32_FLOAT", "32_32_FLOAT" };
        return f < 16 ? kNames[f] : "?";
    }

    static void CaptureDraw(const RegisterFile& regs, uint32_t drawInitiator, const char* outcome)
    {
        reg::VGT_DRAW_INITIATOR init;
        init.value = drawInitiator;
        auto mode = regs.Get<reg::RB_MODECONTROL>();
        auto surf = regs.Get<reg::RB_SURFACE_INFO>();
        auto dc = regs.Get<reg::RB_DEPTHCONTROL>();
        auto di = regs.Get<reg::RB_DEPTH_INFO>();
        auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
        auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
        auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        auto f = [&](Register r) { float v; memcpy(&v, &regs[r], 4); return v; };
        char rts[160] = "";
        uint32_t mask = regs[XE_GPU_REG_RB_COLOR_MASK];
        for (int i = 0; i < 4; i++)
        {
            if (!((mask >> (4 * i)) & 15)) continue;
            auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            char b[48];
            snprintf(b, sizeof(b), " rt%d=%s@%u", i, FormatName(uint32_t(ci.color_format)), ci.color_base);
            strncat(rts, b, sizeof(rts) - strlen(rts) - 1);
        }
        char tex[400] = "";
        uint32_t texMask = (g_vs ? g_vs->xlat.textureFetchMask : 0) | (g_ps ? g_ps->xlat.textureFetchMask : 0);
        for (uint32_t m = texMask; m; m &= m - 1)
        {
            uint32_t i = uint32_t(__builtin_ctz(m));
            auto tf = regs.GetTextureFetch(i);
            if (Texture* t = GetTexture(tf); t && t->ok)
            {
                bool seen = false;
                for (auto& c : g_captureTextures)
                    seen = seen || c.tex == t;
                if (!seen)
                    g_captureTextures.push_back({ t, uint32_t(tf.format), uint32_t(tf.base_address) << 12 });
            }
            char b[64];
            snprintf(b, sizeof(b), " t%u=fmt%u %ux%u@%08X%s%s", i, uint32_t(tf.format), tf.size_2d.width + 1, tf.size_2d.height + 1,
                     tf.base_address << 12, MapFormat(uint32_t(tf.format)).format == DXGI_FORMAT_UNKNOWN ? "(UNSUPPORTED)" : "",
                     tf.dimension == xenos::DataDimension::kCube ? "(cube)" : tf.dimension == xenos::DataDimension::k3D ? "(3D)" : "");
            strncat(tex, b, sizeof(tex) - strlen(tex) - 1);
        }
        LOG("[capture] draw %u: %s | prim %u n %u mode %u pitch %u msaa %u%s | depth %s fmt %u @%u z%u w%u func %u stencil %u | "
            "vte %03X clip %s cull %u%u face %u | vp scale %.1f %.1f %.4f off %.1f %.1f %.4f | vs %016llx ps %016llx%s",
            g_captureIndex++, outcome, uint32_t(init.prim_type), uint32_t(init.num_indices), uint32_t(mode.edram_mode),
            uint32_t(surf.surface_pitch), uint32_t(surf.msaa_samples), rts,
            (dc.z_enable || dc.stencil_enable) ? "on" : "off", uint32_t(di.depth_format), uint32_t(di.depth_base),
            uint32_t(dc.z_enable), uint32_t(dc.z_write_enable), uint32_t(dc.zfunc), uint32_t(dc.stencil_enable),
            vte.value, clip.clip_disable ? "off" : "on", uint32_t(sc.cull_front), uint32_t(sc.cull_back), uint32_t(sc.face),
            f(XE_GPU_REG_PA_CL_VPORT_XSCALE), f(XE_GPU_REG_PA_CL_VPORT_YSCALE), f(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
            f(XE_GPU_REG_PA_CL_VPORT_XOFFSET), f(XE_GPU_REG_PA_CL_VPORT_YOFFSET), f(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
            (unsigned long long)(g_vs ? g_vs->key : 0), (unsigned long long)(g_ps ? g_ps->key : 0), tex);
    }

    // Saves a texture's full-size level as a .dds file (what the host GPU
    // sees after untiling). Block-compressed and RGBA8 textures only.
    static bool SaveTextureDds(Texture& t, const std::filesystem::path& file)
    {
        static ComPtr<ID3D12Resource> readback;
        if (!readback)
            readback = CreateBuffer(kStagingSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        DXGI_FORMAT df = t.fmt.format;
        uint32_t fourcc = df == DXGI_FORMAT_BC1_UNORM ? 0x31545844u : df == DXGI_FORMAT_BC2_UNORM ? 0x33545844u
                        : df == DXGI_FORMAT_BC3_UNORM ? 0x35545844u : df == DXGI_FORMAT_BC4_UNORM ? 0x55344342u
                        : df == DXGI_FORMAT_BC5_UNORM ? 0x55354342u : 0;
        bool rgba8 = df == DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!readback || (!fourcc && !rgba8) || !t.res)
            return false;
        D3D12_RESOURCE_DESC rd = t.res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT rows; UINT64 rowBytes, total;
        g.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        if (total > kStagingSize)
            return false;
        constexpr auto kSrv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Barrier(t.res.Get(), kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = t.res.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(t.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);
        FlushAndReset();
        const uint8_t* data = nullptr;
        D3D12_RANGE range = { 0, SIZE_T(total) };
        if (FAILED(readback->Map(0, &range, (void**)&data)))
            return false;
        uint32_t hdr[32] = {};
        hdr[0] = 0x20534444u; // "DDS "
        hdr[1] = 124;
        hdr[2] = 0x1007 | 0x8; // caps|height|width|pixelformat|pitch
        hdr[3] = uint32_t(rd.Height);
        hdr[4] = uint32_t(rd.Width);
        hdr[5] = uint32_t(rowBytes);
        hdr[19] = 32;
        if (fourcc) { hdr[20] = 0x4; hdr[21] = fourcc; }
        else { hdr[20] = 0x41; hdr[22] = 32; hdr[23] = 0x000000FF; hdr[24] = 0x0000FF00; hdr[25] = 0x00FF0000; hdr[26] = 0xFF000000; }
        hdr[27] = 0x1000;
        std::ofstream f(file, std::ios::binary);
        f.write((const char*)hdr, 128);
        for (UINT r = 0; r < rows; r++)
            f.write((const char*)data + fp.Offset + size_t(r) * fp.Footprint.RowPitch, std::streamsize(rowBytes));
        readback->Unmap(0, nullptr);
        return bool(f);
    }

    // Writes every texture the captured frame sampled as a .dds file.
    static void DumpCaptureTextures()
    {
        std::filesystem::path dir = std::filesystem::path(g.cacheDir).parent_path() / "capture" / "textures";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        uint32_t written = 0;
        for (auto& c : g_captureTextures)
        {
            if (written >= 200)
                break;
            char name[96];
            snprintf(name, sizeof(name), "tex_%08X_fmt%u_%ux%u.dds", c.address, c.guestFormat, c.tex->width, c.tex->height);
            if (SaveTextureDds(*c.tex, dir / name))
                written++;
        }
        LOG("[capture] saved %u of %zu sampled textures to %s", written, g_captureTextures.size(), dir.string().c_str());
        g_captureTextures.clear();
    }

    // F11: the textures of the frame just drawn, named for mods\textures.
    static void DumpModTextures()
    {
        std::filesystem::path dir = g_mods.root / "dump";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        uint32_t written = 0, existing = 0, skipped = 0;
        for (auto& e : g_mods.dumpList)
        {
            Texture& t = *e.tex;
            if (GpuWritten(t.base, t.bytes)) // render targets change every frame
            {
                skipped++;
                continue;
            }
            char name[64];
            snprintf(name, sizeof(name), "%016llx.dds", (unsigned long long)TextureHash(t.base, t.bytes, e.format, e.width, e.height));
            if (std::filesystem::exists(dir / name, ec))
                existing++;
            else if (SaveTextureDds(t, dir / name))
                written++;
            else
                skipped++;
        }
        LOG("[mods] F11: saved %u textures to %s (%u were there already, %u skipped: render targets or formats that can't be saved)",
            written, dir.string().c_str(), existing, skipped);
        g_mods.dumpList.clear();
    }

    void Draw(const RegisterFile& regs, uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize)
    {
        const char* outcome = DrawImpl(regs, drawInitiator, dmaBase, dmaSize);
        if (g_capturing)
            CaptureDraw(regs, drawInitiator, outcome);
    }

    // ----------------------------------------------------------------------
    // Capture: EDRAM snapshots written as images (userdata/capture/*.ppm)
    // ----------------------------------------------------------------------
    static ComPtr<ID3D12Resource> g_edramReadback;
    static uint32_t g_dumpIndex = 0;

    // Writes the EDRAM surface (sample 0 of each pixel) as it is right now.
    // Colour formats are decoded as 8888 / 2:10:10:10 / 16:16 float; depth is
    // shown as grey (24-bit depth) with stencil ignored.
    static void DumpEdram(const char* what, uint32_t base, uint32_t pitchTiles, uint32_t w, uint32_t h,
                          uint32_t msaaX, uint32_t msaaY, uint32_t fmt, bool is64, bool isDepth)
    {
        if (!g_edramReadback)
            g_edramReadback = CreateBuffer(g_edramSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_edramReadback || w == 0 || h == 0)
            return;
        UavBarrier(g.edram.Get());
        Barrier(g.edram.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.list->CopyBufferRegion(g_edramReadback.Get(), 0, g.edram.Get(), 0, g_edramSize);
        Barrier(g.edram.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FlushAndReset();
        const uint32_t* e = nullptr;
        D3D12_RANGE range = { 0, SIZE_T(g_edramSize) };
        if (FAILED(g_edramReadback->Map(0, &range, (void**)&e)))
            return;
        auto index = [&](uint32_t xs, uint32_t ys) -> uint32_t {
            const uint32_t S = g_resScale, th = 16 * S, ts = 1280 * S * S;
            if (!is64)
            {
                uint32_t tile = (base + (ys / th) * pitchTiles + xs / (80 * S)) & 2047u;
                return tile * ts + (ys % th) * (80 * S) + xs % (80 * S);
            }
            uint32_t tile = (base + (ys / th) * pitchTiles * 2u + xs / (40 * S)) & 2047u;
            return tile * ts + ((ys % th) * (40 * S) + xs % (40 * S)) * 2u;
        };
        uint32_t minD = UINT32_MAX, maxD = 0;
        uint64_t nonZero = 0;
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        for (uint32_t pass = 0; pass < 2; pass++)
        {
            for (uint32_t y = 0; y < h; y++)
            {
                for (uint32_t x = 0; x < w; x++)
                {
                    uint32_t v = e[index((x << msaaX) * g_resScale, (y << msaaY) * g_resScale)];
                    uint8_t* o = &rgb[(size_t(y) * w + x) * 3];
                    if (isDepth)
                    {
                        uint32_t d = v >> 8;
                        if (pass == 0) { minD = std::min(minD, d); maxD = std::max(maxD, d); nonZero += d != 0; continue; }
                        // Stretch the used range so small differences show.
                        uint32_t span = maxD > minD ? maxD - minD : 1;
                        o[0] = o[1] = o[2] = uint8_t(uint64_t(d - minD) * 255 / span);
                        continue;
                    }
                    if (pass == 0) { nonZero += (v & 0xFFFFFFu) != 0; continue; }
                    if (fmt == 2 || fmt == 10 || fmt == 3 || fmt == 12)
                    {
                        o[0] = uint8_t((v & 0x3FF) >> 2); o[1] = uint8_t(((v >> 10) & 0x3FF) >> 2); o[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
                    }
                    else if (fmt == 6 || fmt == 7)
                    {
                        auto h2f = [](uint32_t hbits) {
                            uint32_t s = (hbits >> 15) & 1, ex = (hbits >> 10) & 31, m = hbits & 1023;
                            float f = ex == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), int(ex) - 25);
                            return s ? -f : f;
                        };
                        auto c = [&](float f) { return uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                        o[0] = c(h2f(v & 0xFFFF)); o[1] = c(h2f(v >> 16)); o[2] = is64 ? c(h2f(e[index((x << msaaX) * g_resScale, (y << msaaY) * g_resScale) + 1] & 0xFFFF)) : 0;
                    }
                    else
                    {
                        o[0] = uint8_t(v); o[1] = uint8_t(v >> 8); o[2] = uint8_t(v >> 16);
                    }
                }
            }
        }
        g_edramReadback->Unmap(0, nullptr);
        std::filesystem::path dir = std::filesystem::path(g.cacheDir).parent_path() / "capture";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        char name[96];
        snprintf(name, sizeof(name), "%03u_%s_%s_at%u_%ux%u.ppm", g_dumpIndex++, what, isDepth ? "depth" : FormatName(fmt), base, w, h);
        std::ofstream f(dir / name, std::ios::binary);
        f << "P6\n" << w << " " << h << "\n255\n";
        f.write((const char*)rgb.data(), std::streamsize(rgb.size()));
        if (isDepth)
            LOG("[capture]   saved %s (depth min %06X max %06X, %llu of %u pixels non-zero)", name, minD, maxD, (unsigned long long)nonZero, w * h);
        else
            LOG("[capture]   saved %s (%llu of %u pixels have non-zero colour)", name, (unsigned long long)nonZero, w * h);
        // The command list was reset; restore what Resolve had bound.
        g.list->SetComputeRootSignature(g.computeRoot.Get());
        g.list->SetComputeRootDescriptorTable(1, ViewGpu(kDescEdramUav));
    }

    // ----------------------------------------------------------------------
    // Resolves
    // ----------------------------------------------------------------------
    void Resolve(const RegisterFile& regs, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!g_active)
            return;
        struct Timer
        {
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~Timer() { g.tResolve += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
        } timer;
        auto copyControl = regs.Get<reg::RB_COPY_CONTROL>();
        auto surfaceInfo = regs.Get<reg::RB_SURFACE_INFO>();
        auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
        uint32_t msaaX = surfaceInfo.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
        uint32_t msaaY = surfaceInfo.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
        uint32_t pitchTiles = xenos::GetSurfacePitchTiles(surfaceInfo.surface_pitch, surfaceInfo.msaa_samples, false);
        bool isDepth = copyControl.copy_src_select >= 4;
        uint32_t srcBase = depthInfo.depth_base, srcFormat = 0;
        bool src64 = false;
        if (!isDepth)
        {
            auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copyControl.copy_src_select & 3]);
            srcBase = ci.color_base;
            srcFormat = uint32_t(ci.color_format);
            src64 = xenos::IsColorRenderTargetFormat64bpp(ci.color_format);
        }

        g.list->SetComputeRootSignature(g.computeRoot.Get());
        g.list->SetComputeRootDescriptorTable(1, ViewGpu(kDescEdramUav));
        UavBarrier(g.edram.Get()); // draws -> compute

        if (copyControl.copy_command == xenos::CopyCommand::kRaw || copyControl.copy_command == xenos::CopyCommand::kConvert)
        {
            auto destInfo = regs.Get<reg::RB_COPY_DEST_INFO>();
            auto destPitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
            uint32_t destBase = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
            uint32_t pitchA = (destPitch.copy_dest_pitch + 31) & ~31u;
            uint32_t heightA = (destPitch.copy_dest_height + 31) & ~31u;
            uint32_t destFormat = uint32_t(destInfo.copy_dest_format);
            HostFormat df = MapFormat(destFormat);
            uint32_t bppLog2 = isDepth ? 2 : (df.format != DXGI_FORMAT_UNKNOWN && !df.block ? df.bpbLog2 : 2);
            bool raw = isDepth || xenos::IsColorResolveFormatBitwiseEquivalent(xenos::ColorRenderTargetFormat(srcFormat), destInfo.copy_dest_format);

            // Make sure surrounding (not overwritten) bytes are current first.
            uint32_t destBytes = destInfo.copy_dest_array ? pitchA * heightA * (destInfo.copy_dest_slice + 1) << bppLog2 : pitchA * heightA << bppLog2;
            EnsureRange(destBase, destBytes, false);
            SharedTo(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.list->SetPipelineState(g.resolvePso.Get());
            uint32_t rc[32] = {};
            rc[0] = uint32_t(x0); rc[1] = uint32_t(y0); rc[2] = uint32_t(x1 - x0); rc[3] = uint32_t(y1 - y0);
            rc[4] = srcBase; rc[5] = pitchTiles; rc[6] = srcFormat;
            rc[7] = msaaX | (msaaY << 1) | (uint32_t(surfaceInfo.msaa_samples) << 2);
            rc[8] = destBase; rc[9] = pitchA; rc[10] = heightA; rc[11] = destFormat;
            rc[12] = uint32_t(destInfo.copy_dest_endian);
            rc[13] = (raw ? 1u : 0u) | (destInfo.copy_dest_swap ? 2u : 0u) | (destInfo.copy_dest_array ? 4u : 0u) | (src64 ? 8u : 0u) | (isDepth ? 16u : 0u);
            rc[14] = destInfo.copy_dest_slice;
            rc[15] = uint32_t(copyControl.copy_sample_select);
            float scale = std::ldexp(1.0f, destInfo.copy_dest_exp_bias);
            memcpy(&rc[16], &scale, 4);
            rc[17] = bppLog2;
            g.list->SetComputeRoot32BitConstants(0, 32, rc, 0);
            g.list->Dispatch((uint32_t(x1 - x0) + 7) / 8, (uint32_t(y1 - y0) + 7) / 8, 1);
            UavBarrier(g.shared.Get());
            MarkGpuWritten(destBase, destBytes);
            g_resolveRanges.push_back({ ++g_resolveGen, destBase, destBase + destBytes });

            // At a higher render resolution, also keep a full-resolution copy
            // (used instead of guest memory when the game samples it).
            if (g_resScale > 1)
            {
                uint32_t dx = 0, dy = 0;
                ScaledSurface* sf = destInfo.copy_dest_array ? nullptr
                                  : GetScaledSurface(destBase, pitchA, heightA, bppLog2, uint32_t(x1), uint32_t(y1), dx, dy);
                if (sf)
                {
                    g.list->SetPipelineState(g.resolveScaledPso.Get());
                    rc[20] = uint32_t(sf->poolOffset);
                    rc[21] = sf->pitch * g_resScale;
                    rc[22] = dx;
                    rc[23] = dy;
                    // Clip to the surface (rc[2]/[3]: the size the kernel writes).
                    rc[2] = std::min<uint32_t>(rc[2], sf->pitch - std::min(dx, sf->pitch));
                    rc[3] = std::min<uint32_t>(rc[3], sf->height - std::min(dy, sf->height));
                    g.list->SetComputeRoot32BitConstants(0, 32, rc, 0);
                    uint32_t w = std::min<uint32_t>(uint32_t(x1 - x0), sf->pitch - std::min(dx, sf->pitch)) * g_resScale;
                    uint32_t h = std::min<uint32_t>(uint32_t(y1 - y0), sf->height - std::min(dy, sf->height)) * g_resScale;
                    g.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    UavBarrier(g.scaledPool.Get());
                    sf->gen = g_resolveGen;
                }
                else
                    DropScaledSurfaces(destBase, destBytes);
            }
        }

        auto clear = [&](uint32_t base, bool is64, uint32_t lo, uint32_t hi) {
            g.list->SetPipelineState(g.clearPso.Get());
            uint32_t rc[32] = {};
            rc[0] = (uint32_t(x0) << msaaX) * g_resScale; rc[1] = (uint32_t(y0) << msaaY) * g_resScale;
            rc[2] = (uint32_t(x1 - x0) << msaaX) * g_resScale; rc[3] = (uint32_t(y1 - y0) << msaaY) * g_resScale;
            rc[4] = base; rc[5] = pitchTiles; rc[6] = is64 ? 1u : 0u;
            rc[18] = lo; rc[19] = hi;
            g.list->SetComputeRoot32BitConstants(0, 32, rc, 0);
            g.list->Dispatch((rc[2] + 7) / 8, (rc[3] + 7) / 8, 1);
            UavBarrier(g.edram.Get());
        };
        if (g_capturing)
        {
            auto di2 = regs.Get<reg::RB_COPY_DEST_INFO>();
            LOG("[capture] resolve %d,%d-%d,%d src %s fmt %s @%u -> dest fmt %u endian %u @%08X pitch %u | copy %u clear color %d depth %d (color %08X depth %08X) exp %d",
                x0, y0, x1, y1, isDepth ? "depth" : "color", isDepth ? "-" : FormatName(srcFormat), srcBase,
                uint32_t(di2.copy_dest_format), uint32_t(di2.copy_dest_endian), regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                regs[XE_GPU_REG_RB_COPY_DEST_PITCH] & 0x3FFF, uint32_t(copyControl.copy_command),
                int(copyControl.color_clear_enable), int(copyControl.depth_clear_enable),
                regs[XE_GPU_REG_RB_COLOR_CLEAR], regs[XE_GPU_REG_RB_DEPTH_CLEAR], int(di2.copy_dest_exp_bias));
            if (g_dumpIndex < 48)
            {
                DumpEdram("resolve", srcBase, pitchTiles, uint32_t(x1), uint32_t(y1), msaaX, msaaY, srcFormat, src64, isDepth);
                // The depth buffer the scene was drawn with, for big colour resolves.
                if (!isDepth && x1 - x0 >= 320)
                    DumpEdram("zbuf", depthInfo.depth_base, pitchTiles, uint32_t(x1), uint32_t(y1), msaaX, msaaY, 0, false, true);
            }
        }
        if (copyControl.color_clear_enable && !isDepth)
            clear(srcBase, src64, regs[XE_GPU_REG_RB_COLOR_CLEAR_LO], regs[XE_GPU_REG_RB_COLOR_CLEAR]);
        if (copyControl.depth_clear_enable)
            clear(depthInfo.depth_base, false, 0, regs[XE_GPU_REG_RB_DEPTH_CLEAR]);
    }

    // ----------------------------------------------------------------------
    // Present
    // ----------------------------------------------------------------------
    void Swap(const RegisterFile& regs, uint32_t frontBufferPhysical, uint32_t width, uint32_t height)
    {
        if (!g_active)
            return;
        g_epoch++;
        g_resolveRanges.clear();
        if (g_capturing)
        {
            auto ff = regs.GetTextureFetch(0);
            LOG("[capture] swap: front buffer fmt %u %ux%u @%08X swizzle %03X - end of capture", uint32_t(ff.format), width, height,
                frontBufferPhysical, uint32_t(ff.swizzle));
            g_capturing = false;
            DumpCaptureTextures();
        }
        // Texture mods: F11 saves this frame's textures (collected while it was
        // drawn), F10 reloads mods\textures.
        if (g_mods.dumping)
        {
            DumpModTextures();
            g_mods.dumping = false;
        }
        if (g_mods.dumpRequest.exchange(false))
            g_mods.dumping = true;
        if (g_mods.reloadRequest.exchange(false))
        {
            ScanModTextures();
            for (auto& [key, tex] : g_textures)
                tex->builtStamp = 0; // rebuilt (with or without a replacement) when next used
        }
        static uint64_t captureAt = getenv("VR_CAPTURE_AT") ? strtoull(getenv("VR_CAPTURE_AT"), nullptr, 10) : 0;
        if (g_captureRequest.exchange(false) || (captureAt && g.frameNumber + 1 == captureAt))
        {
            g_capturing = true;
            g_captureIndex = 0;
            g_dumpIndex = 0;
            LOG("[capture] ---- capturing frame %llu ----", (unsigned long long)(g.frameNumber + 1));
        }

        auto f = regs.GetTextureFetch(0);
        Texture* front = GetTexture(f);
        uint32_t cw = g.swapW, ch = g.swapH;
        if (g_interp.on)
        {
            // Hand the frame to the presenter (see InterpState): the command
            // list must end with the states it started with, as it is executed
            // more than once.
            SharedTo(g_interp.frameStartShared);
            if (front)
                g.device->CopyDescriptorsSimple(1, ViewCpu(kDescPresent + g.frame), CpuView(front->srv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (!Check(g.list->Close(), "Close"))
            {
                g_active = false;
                return;
            }
            InterpState::Job job;
            job.scene = g.list.Get();
            job.slot = g.frame;
            // Not when the frame was partly submitted already, or most of its
            // draws jumped (a camera cut).
            job.replay = g_interp.replayable && !(g_interp.matched >= 8 && g_interp.cuts * 2 > g_interp.matched);
            job.front = front != nullptr;
            job.swizzle = uint32_t(f.swizzle);
            job.aa = settings::Get().antiAliasing;
            job.width = width;
            job.height = height;
            job.frontW = front ? front->hostW : 1;
            job.frontH = front ? front->hostH : 1;
            job.arrival = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(g_interp.mutex);
                g_interp.slotPending[g.frame] = true;
                g_interp.jobs.push_back(job);
            }
            g_interp.cv.notify_all();
            // This frame's constants are the next one's previous ones.
            std::swap(g_interp.prev, g_interp.cur);
            std::swap(g_interp.prevData, g_interp.curData);
            g_interp.cur.clear();
            g_interp.curData.clear();
            g_interp.occurrences.clear();
            g_interp.matchedSum += g_interp.matched;
            g_interp.cutsSum += g_interp.cuts;
            g_interp.matched = g_interp.cuts = 0;
            g_interp.replayable = true;
        }
        else
        {
        // Resize the swap chain with the window.
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        cw = std::max<uint32_t>(uint32_t(rc.right - rc.left), 64);
        ch = std::max<uint32_t>(uint32_t(rc.bottom - rc.top), 64);
        if (front)
        {
            uint32_t view = AllocViews(1);
            g.device->CopyDescriptorsSimple(1, ViewCpu(view), CpuView(front->srv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            RecordPresentBlit(g.list.Get(), true, view, uint32_t(f.swizzle), settings::Get().antiAliasing, front->hostW, front->hostH, width, height);
        }
        else
            RecordPresentBlit(g.list.Get(), false, 0, 0, 0, 1, 1, width, height);

        if (!Check(g.list->Close(), "Close"))
        {
            g_active = false;
            return;
        }
        ID3D12CommandList* lists[] = { g.list.Get() };
        g.queue->ExecuteCommandLists(1, lists);
        auto tp0 = std::chrono::steady_clock::now();
        // Vsync on: wait for the display's refresh. Off: present immediately
        // (tearing allowed when the system supports it).
        bool vsync = settings::Get().vsync;
        HRESULT hr = g.swapChain->Present(vsync ? 1 : 0, (!vsync && g.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        g.tPresent += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tp0).count();
        if (FAILED(hr))
            Check(hr, "Present");
        g.fenceValue++;
        g.queue->Signal(g.fence.Get(), g.fenceValue);
        g.frameFence[g.frame] = g.fenceValue;
        }
        g.frameSeq[g.frame] = g.useSeq;
        g.frameNumber++;

        // Next frame's resources must be free.
        g.frame = (g.frame + 1) % kFrames;
        auto tw0 = std::chrono::steady_clock::now();
        if (g_interp.on)
        {
            uint64_t value;
            {
                std::unique_lock lock(g_interp.mutex);
                g_interp.cv.wait(lock, [] { return !g_interp.slotPending[g.frame]; });
                value = g_interp.slotFence[g.frame];
            }
            if (g_interp.fence->GetCompletedValue() < value)
            {
                g_interp.fence->SetEventOnCompletion(value, g.fenceEvent);
                WaitForSingleObject(g.fenceEvent, INFINITE);
            }
        }
        else if (g.fence->GetCompletedValue() < g.frameFence[g.frame])
        {
            g.fence->SetEventOnCompletion(g.frameFence[g.frame], g.fenceEvent);
            WaitForSingleObject(g.fenceEvent, INFINITE);
        }
        g.completedSeq = std::max(g.completedSeq, g.frameSeq[g.frame]);
        g.tWait += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tw0).count();
        if (g.frameNumber % 300 == 0)
        {
            double n = 300.0;
            static auto lastReport = std::chrono::steady_clock::now();
            auto nowReport = std::chrono::steady_clock::now();
            double secs = std::max(1e-3, std::chrono::duration<double>(nowReport - lastReport).count());
            lastReport = nowReport;
            if (g_interp.on)
            {
                uint32_t shown = g_interp.presents.exchange(0), blended = g_interp.interpolated.exchange(0);
                LOG("[framerate] game %.1f fps, shown %.1f fps (%u blended, %u game frames not blended); per game frame: recording draws %.1f ms, waiting for the presenter %.1f ms, mid-frame flushes %.2f, %u of %u matched draws jumped; presenter per shown frame: waiting for the GPU %.1f ms, in Present %.1f ms",
                    n / secs, shown / secs, blended, g_interp.unblended.exchange(0), g.tDraw / n / 1000, g.tWait / n / 1000, g.flushes / n,
                    g_interp.cutsSum, g_interp.matchedSum, g_interp.gpuWaitUs.exchange(0) / 1000.0 / std::max(shown, 1u),
                    g_interp.presentUs.exchange(0) / 1000.0 / std::max(shown, 1u));
                g_interp.matchedSum = g_interp.cutsSum = 0;
            }
            LOG("[d3d12] per frame (avg of 300): draws %.2f ms (textures %.2f), resolves %.2f ms, memory checks %.2f ms (%.0f pages), present %.2f ms, GPU wait %.2f ms, mid-frame flushes %.2f, pipelines %zu (%u being created)",
                g.tDraw / n / 1000, g.tTex / n / 1000, g.tResolve / n / 1000, g.tHash / n / 1000, g.hashedPages / n, g.tPresent / n / 1000, g.tWait / n / 1000,
                g.flushes / n, g_pipelines.size(), g_psoPending.load());
            LOG("[d3d12]   CPU sync points per frame %.1f", g_syncsThisPeriod / n);
            g_syncsThisPeriod = 0;
            if (g.flushes)
                LOG("[d3d12]   flushes by cause: upload memory %u, views %u, samplers %u, other %u",
                    g_flushWhy[kFlushUpload], g_flushWhy[kFlushViews], g_flushWhy[kFlushSamplers], g_flushWhy[kFlushOther]);
            memset(g_flushWhy, 0, sizeof(g_flushWhy));
            g.tDraw = g.tResolve = g.tHash = g.tPresent = g.tWait = g.tTex = 0;
            g.hashedPages = 0;
            g.flushes = 0;
        }
        g.allocators[g.frame]->Reset();
        g.list = g.lists[g.frame];
        g.list->Reset(g.allocators[g.frame].Get(), nullptr);
        SetHeaps();
        g.uploadUsed = 0;
        // This frame's part of the view ring was last used kFrames frames ago,
        // which the GPU has finished (waited for above).
        g.viewRingPos = RingStart(g.frame);
        ResetViewCaches();
        g_interp.frameStartShared = g.sharedState;

        // A frame rate change from the in-game menu: switch between plain
        // frames and in-between frames with nothing in flight.
        if (int64_t req = g_frameRateRequest.exchange(-1); req >= 0)
        {
            uint32_t fr = uint32_t(req);
            bool on = fr != 30;
            g_interp.rate = fr;
            if (on != g_interp.on)
            {
                WaitIdle(); // (drains the presenter too)
                if (on && !InitPresenter())
                    LOG("[d3d12] in-between frames unavailable; staying at 30 fps");
                else
                {
                    g_interp.on = on;
                    g_interp.prev.clear();
                    g_interp.cur.clear();
                    g_interp.prevData.clear();
                    g_interp.curData.clear();
                    g_interp.occurrences.clear();
                    g_interp.replayable = true;
                    // Every slot is free now (the GPU is idle).
                    for (uint32_t i = 0; i < kFrames; i++)
                    {
                        g_interp.slotPending[i] = false;
                        g_interp.slotFence[i] = 0;
                        g.frameFence[i] = 0;
                    }
                }
            }
            if (fr) LOG("[d3d12] frame rate set to %u", fr);
            else LOG("[d3d12] frame rate set to unlimited");
        }
        if (g.frameNumber % 600 == 0)
            SavePipelineLibrary();

        // Release textures not used for a while (after the GPU is done with them).
        for (auto it = g_retired.begin(); it != g_retired.end();)
        {
            if (it->frame + kFrames + 1 <= g.frameNumber)
            {
                if (it->srv != UINT32_MAX) g.cpuViewFree.push_back(it->srv);
                it = g_retired.erase(it);
            }
            else ++it;
        }
        if (g.frameNumber % 120 == 0)
        {
            for (auto it = g_textures.begin(); it != g_textures.end();)
            {
                if (it->second->lastUse + 600 < g.frameNumber)
                {
                    g_retired.push_back({ it->second->res, it->second->srv, g.frameNumber });
                    it = g_textures.erase(it);
                }
                else ++it;
            }
        }

        if (!g_interp.on && (cw != g.swapW || ch != g.swapH))
        {
            WaitIdle();
            for (auto& b : g.backBuffers) b.Reset();
            if (SUCCEEDED(g.swapChain->ResizeBuffers(kFrames, cw, ch, DXGI_FORMAT_R8G8B8A8_UNORM, g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
            {
                g.swapW = cw;
                g.swapH = ch;
            }
            CreateSwapChainBuffers();
        }
    }
}

#endif
