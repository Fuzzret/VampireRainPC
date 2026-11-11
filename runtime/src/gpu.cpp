// GPU core (API independent): register state, EDRAM, clears, resolves and the
// front buffer. Formats and layouts follow Xenia (third_party/xenia, BSD).
//
// EDRAM is 10 MB split into 2048 tiles of 80x16 samples x 32 bits. A render
// target lives at a tile base with a pitch in tiles; 64bpp targets use tiles
// of 40x16 samples x 64 bits. Rendering writes here; a "resolve" (an
// edram_mode = kCopy draw) copies a rectangle out to a texture in guest memory
// (always tiled) and optionally clears it. VdSwap then shows a texture.
#include "gpu.h"
#include "kernel.h"
#include "memory.h"
#include "raster.h"
#include "d3d12/d3d12_backend.h"

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/xenos.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace xe::gpu;

// Defined in Xenia's registers.cc, which isn't vendored.
const Register reg::RB_COLOR_INFO::rt_register_indices[4] = {
    XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO };

namespace gpu
{
    namespace
    {
        RegisterFile g_regs;
        uint32_t g_edram[xenos::kEdramSizeBytes / 4];

        // Per-frame statistics (logged for the first frames).
        struct Stats { uint32_t draws = 0, resolves = 0, clears = 0, shaders = 0; } g_stats;
        uint64_t g_frame = 0;

        uint8_t* Phys(uint32_t p) { return GuestToHost(mem::PHYSICAL_BASE | (p & 0x1FFFFFFF)); }

        uint32_t Swap32(uint32_t v, xenos::Endian128 e)
        {
            switch (e)
            {
            case xenos::Endian128::k8in16: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
            case xenos::Endian128::k8in32: return __builtin_bswap32(v);
            case xenos::Endian128::k16in32: return (v << 16) | (v >> 16);
            default: return v;
            }
        }
    }

    RegisterFile& Regs() { return g_regs; }
    uint32_t* Edram() { return g_edram; }

    // -----------------------------------------------------------------------
    // EDRAM addressing
    // -----------------------------------------------------------------------
    // Returns the dword index of sample (xs, ys) of a surface.
    uint32_t EdramIndex(uint32_t baseTiles, uint32_t pitchTiles32, uint32_t xs, uint32_t ys, bool is64)
    {
        if (!is64)
        {
            uint32_t tile = (baseTiles + (ys >> 4) * pitchTiles32 + xs / 80) & (xenos::kEdramTileCount - 1);
            return tile * 1280 + (ys & 15) * 80 + xs % 80;
        }
        uint32_t tile = (baseTiles + (ys >> 4) * pitchTiles32 * 2 + xs / 40) & (xenos::kEdramTileCount - 1);
        return tile * 1280 + ((ys & 15) * 40 + xs % 40) * 2;
    }

    // -----------------------------------------------------------------------
    // Format conversion (render target <-> RGBA float)
    // -----------------------------------------------------------------------
    namespace
    {
        float Half(uint16_t h)
        {
            uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
            float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
            return s ? -v : v;
        }
        uint16_t ToHalf(float f)
        {
            // Round to nearest, clamping to the largest finite value.
            if (!(f == f)) return 0x7E00;
            uint32_t s = std::signbit(f) ? 0x8000 : 0;
            f = std::fabs(f);
            if (f >= 65504.0f) return uint16_t(s | 0x7BFF);
            if (f < 6.1035156e-05f) return uint16_t(s | uint32_t(std::lround(f * 16777216.0f)));
            int e;
            float m = std::frexp(f, &e); // f = m * 2^e, m in [0.5, 1)
            uint32_t mant = uint32_t(std::lround((m * 2.0f - 1.0f) * 1024.0f));
            uint32_t exp = uint32_t(e - 1 + 15);
            if (mant == 1024) { mant = 0; exp++; }
            return uint16_t(s | (exp << 10) | mant);
        }
        float Unorm(uint32_t v, uint32_t bits) { return float(v) / float((1u << bits) - 1); }
        uint32_t ToUnorm(float f, uint32_t bits)
        {
            f = std::clamp(f, 0.0f, 1.0f);
            return uint32_t(f * float((1u << bits) - 1) + 0.5f);
        }
        // 7e3 float (k_2_10_10_10_FLOAT color channels).
        float Float7e3(uint32_t v)
        {
            uint32_t e = (v >> 7) & 7, m = v & 127;
            return e == 0 ? std::ldexp(float(m), -9) : std::ldexp(float(m | 128), int(e) - 10);
        }
        uint32_t To7e3(float f)
        {
            f = std::clamp(f, 0.0f, 31.875f);
            if (f < 0.015625f) return uint32_t(f * 512.0f + 0.5f);
            int e;
            float m = std::frexp(f, &e);
            uint32_t mant = uint32_t(std::lround((m * 2.0f - 1.0f) * 128.0f));
            uint32_t exp = uint32_t(e - 1 + 3);
            if (mant == 128) { mant = 0; exp++; }
            if (exp > 7) return 0x3FF;
            return (exp << 7) | mant;
        }
    }

    void DecodeColor(xenos::ColorRenderTargetFormat fmt, const uint32_t* s, float out[4])
    {
        uint32_t v = s[0];
        switch (fmt)
        {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
            for (int i = 0; i < 4; i++) out[i] = Unorm((v >> (8 * i)) & 0xFF, 8);
            break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
            for (int i = 0; i < 3; i++) out[i] = Unorm((v >> (10 * i)) & 0x3FF, 10);
            out[3] = Unorm(v >> 30, 2);
            break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            for (int i = 0; i < 3; i++) out[i] = Float7e3((v >> (10 * i)) & 0x3FF);
            out[3] = Unorm(v >> 30, 2);
            break;
        case xenos::ColorRenderTargetFormat::k_16_16:
            out[0] = float(int16_t(v)) * (32.0f / 32767.0f);
            out[1] = float(int16_t(v >> 16)) * (32.0f / 32767.0f);
            out[2] = 0; out[3] = 1;
            break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            for (int i = 0; i < 4; i++) out[i] = float(int16_t(s[i >> 1] >> (16 * (i & 1)))) * (32.0f / 32767.0f);
            break;
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
            out[0] = Half(uint16_t(v)); out[1] = Half(uint16_t(v >> 16)); out[2] = 0; out[3] = 1;
            break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
            for (int i = 0; i < 4; i++) out[i] = Half(uint16_t(s[i >> 1] >> (16 * (i & 1))));
            break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
            memcpy(&out[0], &v, 4); out[1] = 0; out[2] = 0; out[3] = 1;
            break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
            memcpy(&out[0], &s[0], 4); memcpy(&out[1], &s[1], 4); out[2] = 0; out[3] = 1;
            break;
        default:
            out[0] = out[1] = out[2] = out[3] = 0;
            break;
        }
    }

    void EncodeColor(xenos::ColorRenderTargetFormat fmt, const float in[4], uint32_t* d)
    {
        switch (fmt)
        {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
            d[0] = ToUnorm(in[0], 8) | (ToUnorm(in[1], 8) << 8) | (ToUnorm(in[2], 8) << 16) | (ToUnorm(in[3], 8) << 24);
            break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
            d[0] = ToUnorm(in[0], 10) | (ToUnorm(in[1], 10) << 10) | (ToUnorm(in[2], 10) << 20) | (ToUnorm(in[3], 2) << 30);
            break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            d[0] = To7e3(in[0]) | (To7e3(in[1]) << 10) | (To7e3(in[2]) << 20) | (ToUnorm(in[3], 2) << 30);
            break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        {
            auto sn = [](float f) { return uint32_t(uint16_t(int16_t(std::lround(std::clamp(f, -32.0f, 32.0f) * (32767.0f / 32.0f))))); };
            d[0] = sn(in[0]) | (sn(in[1]) << 16);
            break;
        }
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        {
            auto sn = [](float f) { return uint32_t(uint16_t(int16_t(std::lround(std::clamp(f, -32.0f, 32.0f) * (32767.0f / 32.0f))))); };
            d[0] = sn(in[0]) | (sn(in[1]) << 16);
            d[1] = sn(in[2]) | (sn(in[3]) << 16);
            break;
        }
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
            d[0] = ToHalf(in[0]) | (uint32_t(ToHalf(in[1])) << 16);
            break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
            d[0] = ToHalf(in[0]) | (uint32_t(ToHalf(in[1])) << 16);
            d[1] = ToHalf(in[2]) | (uint32_t(ToHalf(in[3])) << 16);
            break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
            memcpy(&d[0], &in[0], 4);
            break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
            memcpy(&d[0], &in[0], 4); memcpy(&d[1], &in[1], 4);
            break;
        default:
            d[0] = 0;
            break;
        }
    }

    // Writes one pixel of a resolve destination format. Returns bytes per pixel.
    static uint32_t EncodeDest(xenos::ColorFormat fmt, const float c[4], uint32_t out[4])
    {
        switch (fmt)
        {
        case xenos::ColorFormat::k_8_8_8_8:
        case xenos::ColorFormat::k_8_8_8_8_A:
        case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16:
            out[0] = ToUnorm(c[0], 8) | (ToUnorm(c[1], 8) << 8) | (ToUnorm(c[2], 8) << 16) | (ToUnorm(c[3], 8) << 24);
            return 4;
        case xenos::ColorFormat::k_2_10_10_10:
        case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16:
            out[0] = ToUnorm(c[0], 10) | (ToUnorm(c[1], 10) << 10) | (ToUnorm(c[2], 10) << 20) | (ToUnorm(c[3], 2) << 30);
            return 4;
        case xenos::ColorFormat::k_5_6_5:
            out[0] = ToUnorm(c[2], 5) | (ToUnorm(c[1], 6) << 5) | (ToUnorm(c[0], 5) << 11);
            return 2;
        case xenos::ColorFormat::k_8:
        case xenos::ColorFormat::k_8_A:
        case xenos::ColorFormat::k_8_B:
            out[0] = ToUnorm(c[0], 8);
            return 1;
        case xenos::ColorFormat::k_8_8:
            out[0] = ToUnorm(c[0], 8) | (ToUnorm(c[1], 8) << 8);
            return 2;
        case xenos::ColorFormat::k_16:
            out[0] = ToUnorm(c[0], 16);
            return 2;
        case xenos::ColorFormat::k_16_16:
            out[0] = ToUnorm(c[0], 16) | (ToUnorm(c[1], 16) << 16);
            return 4;
        case xenos::ColorFormat::k_16_16_16_16:
            out[0] = ToUnorm(c[0], 16) | (ToUnorm(c[1], 16) << 16);
            out[1] = ToUnorm(c[2], 16) | (ToUnorm(c[3], 16) << 16);
            return 8;
        case xenos::ColorFormat::k_16_FLOAT:
            out[0] = ToHalf(c[0]);
            return 2;
        case xenos::ColorFormat::k_16_16_FLOAT:
            out[0] = ToHalf(c[0]) | (uint32_t(ToHalf(c[1])) << 16);
            return 4;
        case xenos::ColorFormat::k_16_16_16_16_FLOAT:
            out[0] = ToHalf(c[0]) | (uint32_t(ToHalf(c[1])) << 16);
            out[1] = ToHalf(c[2]) | (uint32_t(ToHalf(c[3])) << 16);
            return 8;
        case xenos::ColorFormat::k_32_FLOAT:
            memcpy(&out[0], &c[0], 4);
            return 4;
        case xenos::ColorFormat::k_32_32_FLOAT:
            memcpy(&out[0], &c[0], 8);
            return 8;
        case xenos::ColorFormat::k_32_32_32_32_FLOAT:
            memcpy(&out[0], &c[0], 16);
            return 16;
        default:
            out[0] = ToUnorm(c[0], 8) | (ToUnorm(c[1], 8) << 8) | (ToUnorm(c[2], 8) << 16) | (ToUnorm(c[3], 8) << 24);
            return 4;
        }
    }

    static uint32_t DestBytesPerPixel(xenos::ColorFormat fmt)
    {
        uint32_t tmp[4];
        const float zero[4] = {};
        return EncodeDest(fmt, zero, tmp);
    }

    static void StoreDest(uint8_t* p, const uint32_t* v, uint32_t bpp, xenos::Endian128 endian)
    {
        switch (bpp)
        {
        case 1: *p = uint8_t(v[0]); break;
        case 2:
        {
            uint16_t h = uint16_t(v[0]);
            if (endian == xenos::Endian128::k8in16 || endian == xenos::Endian128::k8in32)
                h = __builtin_bswap16(h);
            memcpy(p, &h, 2);
            break;
        }
        default:
            for (uint32_t i = 0; i < bpp / 4; i++)
            {
                uint32_t w = Swap32(v[i], endian);
                memcpy(p + i * 4, &w, 4);
            }
            if (endian == xenos::Endian128::k8in64 && bpp >= 8)
            {
                for (uint32_t i = 0; i < bpp; i += 8)
                {
                    uint64_t q;
                    memcpy(&q, p + i, 8);
                    q = __builtin_bswap64(q);
                    memcpy(p + i, &q, 8);
                }
            }
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Resolve (copy EDRAM -> memory, optional clear)
    // -----------------------------------------------------------------------
    static void Resolve()
    {
        auto copyControl = g_regs.Get<reg::RB_COPY_CONTROL>();
        auto surfaceInfo = g_regs.Get<reg::RB_SURFACE_INFO>();
        auto depthInfo = g_regs.Get<reg::RB_DEPTH_INFO>();

        // The rectangle comes from three CPU-written vertices in fetch constant 0.
        xenos::xe_gpu_vertex_fetch_t fetch = g_regs.GetVertexFetch(0);
        if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6)
        {
            LOG("[gpu] resolve: unexpected vertex fetch (type %u size %u)", uint32_t(fetch.type), uint32_t(fetch.size));
            return;
        }
        const uint8_t* vtx = Phys(fetch.address * 4);
        float halfPixel = g_regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
        int32_t fx[6];
        for (int i = 0; i < 6; i++)
        {
            uint32_t raw;
            memcpy(&raw, vtx + i * 4, 4);
            raw = Swap32(raw, xenos::Endian128(uint32_t(fetch.endian)));
            float f;
            memcpy(&f, &raw, 4);
            fx[i] = int32_t(std::floor((f + halfPixel) * 256.0f + 0.5f));
        }
        int32_t x0 = (std::min({ fx[0], fx[2], fx[4] }) + 127) >> 8;
        int32_t y0 = (std::min({ fx[1], fx[3], fx[5] }) + 127) >> 8;
        int32_t x1 = (std::max({ fx[0], fx[2], fx[4] }) + 127) >> 8;
        int32_t y1 = (std::max({ fx[1], fx[3], fx[5] }) + 127) >> 8;

        if (g_regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
        {
            auto wo = g_regs.Get<reg::PA_SC_WINDOW_OFFSET>();
            x0 += wo.window_x_offset; x1 += wo.window_x_offset;
            y0 += wo.window_y_offset; y1 += wo.window_y_offset;
        }
        auto tl = g_regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
        auto br = g_regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
        int32_t sx0 = tl.tl_x, sy0 = tl.tl_y, sx1 = br.br_x, sy1 = br.br_y;
        if (!tl.window_offset_disable)
        {
            auto wo = g_regs.Get<reg::PA_SC_WINDOW_OFFSET>();
            sx0 += wo.window_x_offset; sx1 += wo.window_x_offset;
            sy0 += wo.window_y_offset; sy1 += wo.window_y_offset;
        }
        x0 = std::clamp(x0, std::max(sx0, 0), std::max(sx1, 0));
        x1 = std::clamp(x1, std::max(sx0, 0), std::max(sx1, 0));
        y0 = std::clamp(y0, std::max(sy0, 0), std::max(sy1, 0));
        y1 = std::clamp(y1, std::max(sy0, 0), std::max(sy1, 0));
        x0 &= ~7; y0 &= ~7;
        x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
        int32_t pitchAligned = int32_t(surfaceInfo.surface_pitch & ~7u);
        x1 = std::min(x1, pitchAligned);
        x0 = std::min(x0, x1);
        if (x0 >= x1 || y0 >= y1)
            return;

        g_stats.resolves++;
        if (d3d12::Active())
        {
            if (g_regs.Get<reg::RB_COPY_CONTROL>().color_clear_enable || g_regs.Get<reg::RB_COPY_CONTROL>().depth_clear_enable)
                g_stats.clears++;
            d3d12::Resolve(g_regs, x0, y0, x1, y1);
            return;
        }
        uint32_t msaaX = surfaceInfo.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
        uint32_t msaaY = surfaceInfo.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
        uint32_t pitchTiles = xenos::GetSurfacePitchTiles(surfaceInfo.surface_pitch, surfaceInfo.msaa_samples, false);
        bool isDepth = copyControl.copy_src_select >= 4;

        // --- Copy ---
        if (copyControl.copy_command == xenos::CopyCommand::kRaw || copyControl.copy_command == xenos::CopyCommand::kConvert)
        {
            auto destInfo = g_regs.Get<reg::RB_COPY_DEST_INFO>();
            auto destPitch = g_regs.Get<reg::RB_COPY_DEST_PITCH>();
            uint32_t destBase = g_regs[XE_GPU_REG_RB_COPY_DEST_BASE];
            uint32_t pitchA = (destPitch.copy_dest_pitch + 31) & ~31u;
            uint32_t heightA = (destPitch.copy_dest_height + 31) & ~31u;

            xenos::ColorRenderTargetFormat srcFormat = xenos::ColorRenderTargetFormat::k_8_8_8_8;
            uint32_t srcBase = depthInfo.depth_base;
            bool src64 = false;
            if (!isDepth)
            {
                auto ci = g_regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copyControl.copy_src_select & 3]);
                srcFormat = ci.color_format;
                srcBase = ci.color_base;
                src64 = xenos::IsColorRenderTargetFormat64bpp(srcFormat);
            }
            xenos::ColorFormat destFormat = destInfo.copy_dest_format;
            uint32_t bpp = isDepth ? 4 : DestBytesPerPixel(destFormat);
            uint32_t bppLog2 = bpp >= 16 ? 4 : bpp >= 8 ? 3 : bpp >= 4 ? 2 : bpp >= 2 ? 1 : 0;
            bool raw = isDepth || xenos::IsColorResolveFormatBitwiseEquivalent(srcFormat, destFormat);
            float expScale = std::ldexp(1.0f, destInfo.copy_dest_exp_bias);
            uint32_t sampleSel = uint32_t(copyControl.copy_sample_select);
            uint32_t samples = 1u << uint32_t(surfaceInfo.msaa_samples);

            for (int32_t y = y0; y < y1; y++)
            {
                for (int32_t x = x0; x < x1; x++)
                {
                    // Sample positions: 2x = (0,0),(0,1); 4x = (0,0),(0,1),(1,0),(1,1)
                    auto sampleIndex = [&](uint32_t s) {
                        uint32_t sx = (uint32_t(x) << msaaX) + (msaaX ? (s >> 1) : 0);
                        uint32_t sy = (uint32_t(y) << msaaY) + (msaaY ? (s & 1) : 0);
                        return EdramIndex(srcBase, pitchTiles, sx, sy, src64);
                    };
                    uint32_t out[4] = {};
                    if (raw)
                    {
                        uint32_t s = sampleSel < 4 ? std::min(sampleSel, samples - 1) : 0;
                        const uint32_t* src = &g_edram[sampleIndex(s)];
                        out[0] = src[0];
                        if (bpp == 8) out[1] = src[1];
                        if (!isDepth && destInfo.copy_dest_swap && bpp == 4 && srcFormat <= xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
                            out[0] = (out[0] & 0xFF00FF00) | ((out[0] & 0xFF) << 16) | ((out[0] >> 16) & 0xFF);
                    }
                    else
                    {
                        float c[4] = {}, t[4];
                        uint32_t n = 0;
                        for (uint32_t s = 0; s < samples; s++)
                        {
                            if (sampleSel < 4 && s != std::min(sampleSel, samples - 1))
                                continue;
                            DecodeColor(srcFormat, &g_edram[sampleIndex(s)], t);
                            for (int i = 0; i < 4; i++) c[i] += t[i];
                            n++;
                        }
                        for (int i = 0; i < 4; i++) c[i] = c[i] / float(n ? n : 1) * expScale;
                        if (destInfo.copy_dest_swap)
                            std::swap(c[0], c[2]);
                        EncodeDest(destFormat, c, out);
                    }
                    uint32_t offset;
                    if (destInfo.copy_dest_array)
                        offset = uint32_t(texture_address::Tiled3D(x, y, destInfo.copy_dest_slice, pitchA, heightA, bppLog2));
                    else
                        offset = uint32_t(texture_address::Tiled2D(x, y, pitchA, bppLog2));
                    StoreDest(Phys(destBase + offset), out, bpp, destInfo.copy_dest_endian);
                }
            }
        }

        // Textures decoded from memory a resolve just wrote are stale.
        raster::NewFrame();

        // --- Clear ---
        if (copyControl.color_clear_enable && !isDepth)
        {
            g_stats.clears++;
            auto ci = g_regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copyControl.copy_src_select & 3]);
            bool is64 = xenos::IsColorRenderTargetFormat64bpp(ci.color_format);
            uint32_t hi = g_regs[XE_GPU_REG_RB_COLOR_CLEAR], lo = g_regs[XE_GPU_REG_RB_COLOR_CLEAR_LO];
            for (uint32_t ys = uint32_t(y0) << msaaY; ys < uint32_t(y1) << msaaY; ys++)
                for (uint32_t xs = uint32_t(x0) << msaaX; xs < uint32_t(x1) << msaaX; xs++)
                {
                    uint32_t* d = &g_edram[EdramIndex(ci.color_base, pitchTiles, xs, ys, is64)];
                    if (is64) { d[0] = lo; d[1] = hi; }
                    else d[0] = hi;
                }
        }
        if (copyControl.depth_clear_enable)
        {
            g_stats.clears++;
            uint32_t value = g_regs[XE_GPU_REG_RB_DEPTH_CLEAR];
            for (uint32_t ys = uint32_t(y0) << msaaY; ys < uint32_t(y1) << msaaY; ys++)
                for (uint32_t xs = uint32_t(x0) << msaaX; xs < uint32_t(x1) << msaaX; xs++)
                    g_edram[EdramIndex(depthInfo.depth_base, pitchTiles, xs, ys, false)] = value;
        }
    }

    // -----------------------------------------------------------------------
    // Command processor hooks
    // -----------------------------------------------------------------------
    void OnDraw(uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize, const uint32_t* immediate, uint32_t immediateCount)
    {
        g_regs[XE_GPU_REG_VGT_DRAW_INITIATOR] = drawInitiator;
        auto mode = g_regs.Get<reg::RB_MODECONTROL>().edram_mode;
        if (mode == xenos::EdramMode::kCopy)
        {
            Resolve();
            return;
        }
        g_stats.draws++;
        if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly)
            return;
        if (d3d12::Active())
            d3d12::Draw(g_regs, drawInitiator, dmaBase, dmaSize);
        else
            raster::Draw(g_regs, drawInitiator, dmaBase, dmaSize);
    }

    void OnShaderLoad(uint32_t type, uint32_t guestAddr, uint32_t sizeDwords)
    {
        g_stats.shaders++;
        if (!d3d12::Active())
        {
            raster::LoadShader(type, guestAddr, sizeDwords);
            return;
        }
        if (sizeDwords == 0 || sizeDwords > 0x10000 || type > 1)
            return;
        std::vector<uint32_t> code(sizeDwords);
        for (uint32_t i = 0; i < sizeDwords; i++)
            code[i] = Load32(guestAddr + i * 4);
        uint64_t key = 1469598103934665603ull;
        for (uint32_t w : code)
            for (int b = 0; b < 4; b++)
                key = (key ^ ((w >> (8 * b)) & 0xFF)) * 1099511628211ull;
        key ^= uint64_t(type) << 62;
        d3d12::LoadShader(type, code, key);
    }

    void OnSwap(uint32_t frontBufferPhysical, uint32_t width, uint32_t height)
    {
        g_frame++;
        if (g_frame <= 10 || g_frame % 600 == 0)
            LOG("[gpu] frame %llu: %u draws, %u resolves, %u clears, %u shader loads; front buffer %08X %ux%u",
                (unsigned long long)g_frame, g_stats.draws, g_stats.resolves, g_stats.clears, g_stats.shaders,
                frontBufferPhysical, width, height);
        g_stats = {};
        if (d3d12::Active())
        {
            d3d12::Swap(g_regs, frontBufferPhysical, width, height);
            return;
        }
        raster::NewFrame();

        // The front buffer texture's fetch constant was written to fetch slot 0.
        auto fetch = g_regs.GetTextureFetch(0);
        width = std::min<uint32_t>(width, 8192);
        height = std::min<uint32_t>(height, 8192);
        uint32_t pitch = fetch.pitch << 5;
        if (pitch < width)
            pitch = (width + 31) & ~31u;
        auto endian = xenos::Endian128(uint32_t(fetch.endianness));
        bool is1010102 = fetch.format == xenos::TextureFormat::k_2_10_10_10 ||
                         fetch.format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        static std::vector<uint32_t> image;
        image.resize(size_t(width) * height);
        const uint8_t* base = Phys(frontBufferPhysical);
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t off = fetch.tiled ? uint32_t(texture_address::Tiled2D(int32_t(x), int32_t(y), pitch, 2)) : (y * pitch + x) * 4;
                uint32_t v;
                memcpy(&v, base + off, 4);
                v = Swap32(v, endian);
                uint32_t c[4];
                if (is1010102)
                {
                    c[0] = ((v >> 0) & 0x3FF) >> 2; c[1] = ((v >> 10) & 0x3FF) >> 2; c[2] = ((v >> 20) & 0x3FF) >> 2; c[3] = 255;
                }
                else
                {
                    c[0] = v & 0xFF; c[1] = (v >> 8) & 0xFF; c[2] = (v >> 16) & 0xFF; c[3] = v >> 24;
                }
                // Apply the fetch constant swizzle (the front buffer may be stored BGRA).
                auto pick = [&](uint32_t k) { uint32_t sel = (fetch.swizzle >> (3 * k)) & 7; return sel < 4 ? c[sel] : (sel == 5 ? 255u : 0u); };
                uint32_t r = pick(0), g = pick(1), b = pick(2);
                image[size_t(y) * width + x] = 0xFF000000u | (r << 16) | (g << 8) | b; // BGRA in memory
            }
        }
        Present(image.data(), width, height);
    }

    bool g_softwareGpu = false;

    void Init(const char* dataDir)
    {
        memset(&g_regs, 0, sizeof(g_regs));
        PresenterInit(dataDir);
        if (!g_softwareGpu)
        {
            if (void* window = PresenterWindow())
            {
                if (!d3d12::Init(window, std::string(dataDir) + "/shadercache"))
                    LOG("[gpu] Direct3D 12 unavailable, using the software renderer");
            }
        }
        if (d3d12::Active())
            PresenterSetExternal(true);
        else
            raster::Init();
    }
}
