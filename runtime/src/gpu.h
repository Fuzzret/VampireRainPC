// GPU core: state tracking, EDRAM, resolves and presenting. API independent.
// The command processor (video.cpp) owns the PM4 stream and calls in here.
#pragma once
#include <cstdint>

namespace xe::gpu { class RegisterFile; }

namespace gpu
{
    xe::gpu::RegisterFile& Regs();

    void Init(const char* dataDir);

    // Called by the command processor.
    void OnDraw(uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize, const uint32_t* immediate, uint32_t immediateCount);
    void OnShaderLoad(uint32_t type, uint32_t guestAddr, uint32_t sizeDwords);
    void OnSwap(uint32_t frontBufferPhysical, uint32_t width, uint32_t height);

    // Presenter (window on Windows, frame dumps elsewhere).
    extern bool g_softwareGpu; // --software-gpu: skip Direct3D 12

    void PresenterInit(const char* dataDir);
    void* PresenterWindow();            // native window (HWND), or nullptr
    void PresenterSetExternal(bool on); // a GPU swap chain owns the window
    void PresenterSetStatus(const wchar_t* text); // text shown before the first frame (nullptr: none)
    void Present(const uint32_t* bgra, uint32_t width, uint32_t height);
}
