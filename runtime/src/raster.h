// Software rasterizer: runs Xenos shaders (Xenia's interpreter) and writes
// pixels into the emulated EDRAM. Reference path; the Direct3D 12 backend
// replaces it for speed.
#pragma once
#include <cstdint>

namespace xe::gpu { class RegisterFile; }

namespace raster
{
    void Init();
    void LoadShader(uint32_t type, uint32_t guestAddr, uint32_t sizeDwords);
    void NewFrame(); // drops the per-frame texture cache
    void Draw(const xe::gpu::RegisterFile& regs, uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize);
}
