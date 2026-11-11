// Direct3D 12 GPU backend. Emulates the Xenos the same way as the software
// rasterizer (raster.cpp) - the EDRAM is a GPU buffer written by pixel
// shaders through a rasterizer-ordered view, and guest memory is mirrored in
// a 512 MB GPU buffer - but runs everything on the host GPU.
//
// Only built on Windows; elsewhere Active() is always false.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xe::gpu { class RegisterFile; }

namespace d3d12
{
    // Creates the device and swap chain for the window. Returns false (and the
    // caller falls back to the software renderer) if D3D12 or rasterizer
    // ordered views aren't available.
    bool Init(void* hwnd, const std::string& cacheDir);
    bool Active();

    void LoadShader(uint32_t type, const std::vector<uint32_t>& ucode, uint64_t key);
    void Draw(const xe::gpu::RegisterFile& regs, uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize);
    // Rectangle in pixels, already clamped and aligned by the caller.
    void Resolve(const xe::gpu::RegisterFile& regs, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    // Logs every draw and resolve of the next frame (F12).
    void RequestCapture();
    // True while the requested frame is being logged.
    bool Capturing();
    // The CPU may have written memory that following commands use.
    void NoteCpuSync();
    // The game renders at width x height (other aspect ratios) while its HUD
    // stays laid out for 1280x720; HUD draws are fitted into a centred box.
    void SetRenderSize(uint32_t width, uint32_t height);
    // Changes the shown frame rate while running (30, 60, 120, 144, 0 =
    // unlimited); takes effect at the next frame.
    void SetFrameRate(uint32_t fps);
    // Texture mods: save the next frame's textures to mods\dump (F11), and
    // reload mods\textures (F10).
    void RequestTextureDump();
    void ReloadModTextures();
    void Swap(const xe::gpu::RegisterFile& regs, uint32_t frontBufferPhysical, uint32_t width, uint32_t height);
}
