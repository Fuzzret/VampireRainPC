// Other screen shapes (4:3, 16:10, ultrawide).
//
// The game keeps its render settings in one structure, filled once by
// sub_82210A48: width 1280 (+4), height 720 (+8) and aspect ratio 16:9 (+16,
// float). The Direct3D device's back buffer is created from the width and
// height, and the cameras build their projection from the aspect ratio. So
// the picture's shape is changed here:
//   - wider than 16:9: height 720, wider picture (e.g. 1680x720 for 21:9) -
//     the view widens;
//   - narrower (4:3, 16:10): width 1280, taller picture (e.g. 1280x960) - the
//     view keeps its height and is narrower, like the game on a 4:3 TV.
// The HUD and menus are laid out for 1280x720, and always fit inside the
// picture this way: the renderer moves them into a centred 1280x720 box (see
// d3d12::SetRenderSize). The window shows the game's output at its shape.
#include "kernel.h"
#include "settings.h"
#include "d3d12/d3d12_backend.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint32_t kRenderSettings = 0x82790FF8;
}

PPC_FUNC_IMPL(__imp__sub_82210A48);
PPC_FUNC(sub_82210A48)
{
    __imp__sub_82210A48(ctx, base);
    float aspect = settings::RenderAspect();
    if (std::fabs(aspect - 16.0f / 9.0f) < 0.01f)
        return; // the original shape
    if (Load32(kRenderSettings + 4) != 1280 || Load32(kRenderSettings + 8) != 720)
        return; // not the layout we know
    uint32_t width = 1280, height = 720;
    // Multiples of 16 pixels; between 4:3 (1280x960) and 32:9 (2560x720).
    if (aspect > 16.0f / 9.0f)
        width = std::clamp<uint32_t>(uint32_t(std::lround(720.0f * aspect / 16.0f)) * 16, 1280, 2560);
    else
        height = std::clamp<uint32_t>(uint32_t(std::lround(1280.0f / aspect / 16.0f)) * 16, 720, 960);
    Store32(kRenderSettings + 4, width);
    Store32(kRenderSettings + 8, height);
    StoreF32(kRenderSettings + 16, float(width) / float(height));
    d3d12::SetRenderSize(width, height);
    LOG("[display] rendering at %ux%u (aspect %.3f), HUD in a centred 1280x720 box", width, height, float(width) / float(height));
}
