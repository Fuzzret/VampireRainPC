// Software rasterizer (reference path). Runs the game's vertex and pixel
// shaders through Xenia's shader interpreter and writes into the emulated
// EDRAM, following the Xenos pipeline: index fetch -> vertex shader ->
// clip/viewport -> triangle setup -> pixel shader -> alpha test -> depth/
// stencil -> blend -> EDRAM.
//
// It is slow (every pixel is interpreted) but exact enough to check output.
// Rows are split across worker threads; each worker processes every triangle
// of a draw in order for its own rows, so per-pixel ordering is preserved.
#include "raster.h"
#include "kernel.h"
#include "memory.h"

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace xe::gpu;

namespace gpu
{
    uint32_t* Edram();
    uint32_t EdramIndex(uint32_t baseTiles, uint32_t pitchTiles32, uint32_t xs, uint32_t ys, bool is64);
    void DecodeColor(xenos::ColorRenderTargetFormat fmt, const uint32_t* s, float out[4]);
    void EncodeColor(xenos::ColorRenderTargetFormat fmt, const float in[4], uint32_t* d);
}

namespace raster
{
    namespace
    {
        xe::Memory g_memory(nullptr);

        uint8_t* Phys(uint32_t p) { return g_base + mem::PHYSICAL_BASE + (p & 0x1FFFFFFF); }

        // ------------------------------------------------------------------
        // Shaders
        // ------------------------------------------------------------------
        struct Shader
        {
            std::vector<uint32_t> ucode; // host byte order
        };
        std::unordered_map<uint64_t, std::shared_ptr<Shader>> g_shaders;
        std::shared_ptr<Shader> g_vs, g_ps;

        // ------------------------------------------------------------------
        // Textures: decoded to RGBA float on first use, cached per frame.
        // ------------------------------------------------------------------
        struct Texture
        {
            uint32_t width = 0, height = 0;
            std::vector<float> texels; // RGBA (fetch components X..W, before swizzle)
        };

        std::mutex g_texMutex;
        std::unordered_map<uint64_t, std::shared_ptr<Texture>> g_textures;

        uint64_t Hash(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
        {
            auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; i++)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        void SwapBlock(uint8_t* b, uint32_t size, xenos::Endian e)
        {
            switch (e)
            {
            case xenos::Endian::k8in16:
                for (uint32_t i = 0; i + 1 < size; i += 2) std::swap(b[i], b[i + 1]);
                break;
            case xenos::Endian::k8in32:
                for (uint32_t i = 0; i + 3 < size; i += 4) { std::swap(b[i], b[i + 3]); std::swap(b[i + 1], b[i + 2]); }
                break;
            case xenos::Endian::k16in32:
                for (uint32_t i = 0; i + 3 < size; i += 4) { std::swap(b[i], b[i + 2]); std::swap(b[i + 1], b[i + 3]); }
                break;
            default:
                break;
            }
        }

        float Half(uint16_t h)
        {
            uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
            float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
            return s ? -v : v;
        }

        void Rgb565(uint16_t c, float out[3])
        {
            out[0] = float((c >> 11) & 31) / 31.0f;
            out[1] = float((c >> 5) & 63) / 63.0f;
            out[2] = float(c & 31) / 31.0f;
        }

        // Decodes one 4x4 block of a DXT format into 16 RGBA texels.
        void DecodeDxtColor(const uint8_t* b, float out[16][4], bool dxt1)
        {
            uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
            uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
            float pal[4][4];
            Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
            pal[0][3] = pal[1][3] = 1.0f;
            if (!dxt1 || c0 > c1)
            {
                for (int i = 0; i < 3; i++)
                {
                    pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3;
                    pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3;
                }
                pal[2][3] = pal[3][3] = 1.0f;
            }
            else
            {
                for (int i = 0; i < 3; i++) { pal[2][i] = (pal[0][i] + pal[1][i]) / 2; pal[3][i] = 0; }
                pal[2][3] = 1.0f; pal[3][3] = 0.0f;
            }
            for (int t = 0; t < 16; t++)
                memcpy(out[t], pal[(idx >> (t * 2)) & 3], sizeof(float) * 4);
        }

        void DecodeAlphaBlock(const uint8_t* b, float out[16])
        {
            float a0 = b[0] / 255.0f, a1 = b[1] / 255.0f, pal[8] = { a0, a1 };
            if (b[0] > b[1])
                for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            else
            {
                for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                pal[6] = 0; pal[7] = 1;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
            for (int t = 0; t < 16; t++) out[t] = pal[(bits >> (3 * t)) & 7];
        }

        struct FormatInfo { uint32_t blockW, bytes; bool known; };

        FormatInfo GetFormatInfo(xenos::TextureFormat f)
        {
            using F = xenos::TextureFormat;
            switch (f)
            {
            case F::k_8: case F::k_8_A: case F::k_8_B: return { 1, 1, true };
            case F::k_1_5_5_5: case F::k_5_6_5: case F::k_6_5_5: case F::k_8_8: case F::k_4_4_4_4:
            case F::k_16: case F::k_16_FLOAT: case F::k_16_EXPAND: return { 1, 2, true };
            case F::k_8_8_8_8: case F::k_8_8_8_8_A: case F::k_2_10_10_10: case F::k_8_8_8_8_AS_16_16_16_16:
            case F::k_2_10_10_10_AS_16_16_16_16: case F::k_16_16: case F::k_16_16_FLOAT: case F::k_16_16_EXPAND:
            case F::k_32_FLOAT: case F::k_24_8: case F::k_24_8_FLOAT: case F::k_32: case F::k_10_11_11: case F::k_11_11_10:
            case F::k_10_11_11_AS_16_16_16_16: case F::k_11_11_10_AS_16_16_16_16:
                return { 1, 4, true };
            case F::k_16_16_16_16: case F::k_16_16_16_16_FLOAT: case F::k_16_16_16_16_EXPAND: case F::k_32_32_FLOAT:
                return { 1, 8, true };
            case F::k_32_32_32_32_FLOAT: return { 1, 16, true };
            case F::k_DXT1: case F::k_DXT1_AS_16_16_16_16: case F::k_DXT3A: case F::k_DXT5A: return { 4, 8, true };
            case F::k_DXT2_3: case F::k_DXT4_5: case F::k_DXT2_3_AS_16_16_16_16: case F::k_DXT4_5_AS_16_16_16_16:
            case F::k_DXN: case F::k_DXT3A_AS_1_1_1_1: return { 4, 16, true };
            default: return { 1, 4, false };
            }
        }

        // Decodes a block (blockW x blockW texels) of raw, endian-swapped data.
        void DecodeBlock(xenos::TextureFormat f, const uint8_t* b, float out[16][4])
        {
            using F = xenos::TextureFormat;
            auto u8 = [](uint32_t v) { return float(v & 0xFF) / 255.0f; };
            uint32_t d = 0;
            memcpy(&d, b, 4);
            float* o = out[0];
            o[0] = o[1] = o[2] = 0; o[3] = 1;
            switch (f)
            {
            case F::k_8: case F::k_8_A: case F::k_8_B:
                o[0] = o[1] = o[2] = o[3] = u8(b[0]); break;
            case F::k_8_8: o[0] = u8(b[0]); o[1] = u8(b[1]); o[2] = 0; o[3] = 1; break;
            case F::k_5_6_5:
            {
                uint16_t c = uint16_t(d);
                o[0] = float(c & 31) / 31; o[1] = float((c >> 5) & 63) / 63; o[2] = float((c >> 11) & 31) / 31; break;
            }
            case F::k_6_5_5:
            {
                uint16_t c = uint16_t(d);
                o[0] = float(c & 31) / 31; o[1] = float((c >> 5) & 31) / 31; o[2] = float((c >> 10) & 63) / 63; break;
            }
            case F::k_1_5_5_5:
            {
                uint16_t c = uint16_t(d);
                o[0] = float(c & 31) / 31; o[1] = float((c >> 5) & 31) / 31; o[2] = float((c >> 10) & 31) / 31; o[3] = float(c >> 15); break;
            }
            case F::k_4_4_4_4:
            {
                uint16_t c = uint16_t(d);
                for (int i = 0; i < 4; i++) o[i] = float((c >> (4 * i)) & 15) / 15; break;
            }
            case F::k_16: case F::k_16_EXPAND: o[0] = float(uint16_t(d)) / 65535.0f; break;
            case F::k_16_FLOAT: o[0] = Half(uint16_t(d)); break;
            case F::k_8_8_8_8: case F::k_8_8_8_8_A: case F::k_8_8_8_8_AS_16_16_16_16:
                for (int i = 0; i < 4; i++) o[i] = u8(d >> (8 * i)); break;
            case F::k_2_10_10_10: case F::k_2_10_10_10_AS_16_16_16_16:
                for (int i = 0; i < 3; i++) o[i] = float((d >> (10 * i)) & 0x3FF) / 1023.0f;
                o[3] = float(d >> 30) / 3.0f; break;
            case F::k_16_16: case F::k_16_16_EXPAND:
                o[0] = float(d & 0xFFFF) / 65535.0f; o[1] = float(d >> 16) / 65535.0f; break;
            case F::k_16_16_FLOAT: o[0] = Half(uint16_t(d)); o[1] = Half(uint16_t(d >> 16)); break;
            case F::k_32_FLOAT: memcpy(&o[0], &d, 4); break;
            case F::k_24_8: o[0] = float(d >> 8) / 16777215.0f; break;
            case F::k_16_16_16_16: case F::k_16_16_16_16_EXPAND:
                for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, b + 2 * i, 2); o[i] = float(h) / 65535.0f; } break;
            case F::k_16_16_16_16_FLOAT:
                for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, b + 2 * i, 2); o[i] = Half(h); } break;
            case F::k_32_32_FLOAT: memcpy(o, b, 8); break;
            case F::k_32_32_32_32_FLOAT: memcpy(o, b, 16); break;
            case F::k_DXT1: case F::k_DXT1_AS_16_16_16_16:
                DecodeDxtColor(b, out, true); break;
            case F::k_DXT2_3: case F::k_DXT2_3_AS_16_16_16_16:
                DecodeDxtColor(b + 8, out, false);
                for (int t = 0; t < 16; t++) out[t][3] = float((b[t >> 1] >> ((t & 1) * 4)) & 15) / 15.0f;
                break;
            case F::k_DXT4_5: case F::k_DXT4_5_AS_16_16_16_16:
            {
                DecodeDxtColor(b + 8, out, false);
                float a[16];
                DecodeAlphaBlock(b, a);
                for (int t = 0; t < 16; t++) out[t][3] = a[t];
                break;
            }
            case F::k_DXN:
            {
                float x[16], y[16];
                DecodeAlphaBlock(b, x);
                DecodeAlphaBlock(b + 8, y);
                for (int t = 0; t < 16; t++) { out[t][0] = x[t]; out[t][1] = y[t]; out[t][2] = 0; out[t][3] = 1; }
                break;
            }
            case F::k_DXT5A:
            {
                float a[16];
                DecodeAlphaBlock(b, a);
                for (int t = 0; t < 16; t++) out[t][0] = out[t][1] = out[t][2] = out[t][3] = a[t];
                break;
            }
            case F::k_DXT3A:
                for (int t = 0; t < 16; t++) out[t][0] = out[t][1] = out[t][2] = out[t][3] = float((b[t >> 1] >> ((t & 1) * 4)) & 15) / 15.0f;
                break;
            default:
                o[0] = 1; o[1] = 0; o[2] = 1; o[3] = 1; // magenta: unsupported
                break;
            }
        }

        std::shared_ptr<Texture> GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch)
        {
            uint64_t key = Hash(&fetch, 24);
            {
                std::lock_guard lock(g_texMutex);
                auto it = g_textures.find(key);
                if (it != g_textures.end())
                    return it->second;
            }

            auto tex = std::make_shared<Texture>();
            uint32_t w, h;
            if (fetch.dimension == xenos::DataDimension::k1D) { w = fetch.size_1d.width + 1; h = 1; }
            else if (fetch.dimension == xenos::DataDimension::k3D) { w = fetch.size_3d.width + 1; h = fetch.size_3d.height + 1; }
            else { w = fetch.size_2d.width + 1; h = fetch.size_2d.height + 1; }
            w = std::min<uint32_t>(w, 8192);
            h = std::min<uint32_t>(h, 8192);
            tex->width = w;
            tex->height = h;
            tex->texels.assign(size_t(w) * h * 4, 0.0f);

            FormatInfo fi = GetFormatInfo(fetch.format);
            static std::atomic<int> unknownLogged{ 0 };
            if (!fi.known && unknownLogged++ < 20)
                LOG("[raster] unsupported texture format %u", uint32_t(fetch.format));

            uint32_t blocksW = (w + fi.blockW - 1) / fi.blockW, blocksH = (h + fi.blockW - 1) / fi.blockW;
            uint32_t pitchBlocks = (fetch.pitch << 5) / fi.blockW;
            if (pitchBlocks < blocksW) pitchBlocks = blocksW;
            uint32_t bppLog2 = fi.bytes == 1 ? 0 : fi.bytes == 2 ? 1 : fi.bytes == 4 ? 2 : fi.bytes == 8 ? 3 : 4;
            uint32_t pitchTiled = (pitchBlocks + 31) & ~31u;
            uint32_t base = fetch.base_address << 12;

            float block[16][4];
            uint8_t raw[16];
            for (uint32_t by = 0; by < blocksH; by++)
            {
                for (uint32_t bx = 0; bx < blocksW; bx++)
                {
                    uint32_t off = fetch.tiled ? uint32_t(texture_address::Tiled2D(int32_t(bx), int32_t(by), pitchTiled, bppLog2))
                                               : (by * pitchBlocks + bx) * fi.bytes;
                    memcpy(raw, Phys(base + off), fi.bytes);
                    SwapBlock(raw, fi.bytes, fetch.endianness);
                    DecodeBlock(fetch.format, raw, block);
                    for (uint32_t ty = 0; ty < fi.blockW; ty++)
                        for (uint32_t tx = 0; tx < fi.blockW; tx++)
                        {
                            uint32_t x = bx * fi.blockW + tx, y = by * fi.blockW + ty;
                            if (x < w && y < h)
                                memcpy(&tex->texels[(size_t(y) * w + x) * 4], block[ty * fi.blockW + tx], 16);
                        }
                }
            }

            // Signedness and gamma apply per data component.
            xenos::TextureSign signs[4] = { fetch.sign_x, fetch.sign_y, fetch.sign_z, fetch.sign_w };
            bool anySign = false;
            for (auto s : signs) anySign = anySign || s != xenos::TextureSign::kUnsigned;
            if (anySign)
            {
                for (size_t i = 0; i < tex->texels.size(); i += 4)
                    for (int c = 0; c < 4; c++)
                    {
                        float& v = tex->texels[i + c];
                        switch (signs[c])
                        {
                        case xenos::TextureSign::kSigned: v = v * 255.0f / 127.0f; v = v > 1.0f ? v - 255.0f / 127.0f - 1.0f / 127.0f : v; v = std::max(v, -1.0f); break;
                        case xenos::TextureSign::kUnsignedBiased: v = v * 2.0f - 1.0f; break;
                        case xenos::TextureSign::kGamma: v = std::pow(std::max(v, 0.0f), 2.2f); break;
                        default: break;
                        }
                    }
            }
            if (fetch.exp_adjust)
            {
                float scale = std::ldexp(1.0f, fetch.exp_adjust);
                for (float& v : tex->texels) v *= scale;
            }

            if (getenv("VR_DUMP_TEXTURES"))
            {
                char name[96];
                snprintf(name, sizeof(name), "tex_%08X_%ux%u_f%u.ppm", fetch.base_address << 12, w, h, uint32_t(fetch.format));
                if (FILE* f = fopen(name, "wb"))
                {
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                    for (size_t i = 0; i < size_t(w) * h; i++)
                        for (int c = 0; c < 3; c++)
                            fputc(int(std::clamp(tex->texels[i * 4 + c], 0.0f, 1.0f) * 255.0f), f);
                    fclose(f);
                }
            }
            std::lock_guard lock(g_texMutex);
            g_textures[key] = tex;
            return tex;
        }

        float ApplyClamp(float coord, uint32_t size, xenos::ClampMode mode, bool& border)
        {
            float s = float(size);
            switch (mode)
            {
            case xenos::ClampMode::kRepeat:
                coord -= std::floor(coord / s) * s;
                return coord;
            case xenos::ClampMode::kMirroredRepeat:
            {
                float period = 2.0f * s;
                coord -= std::floor(coord / period) * period;
                return coord >= s ? period - coord - 1.0f : coord;
            }
            case xenos::ClampMode::kClampToBorder:
            case xenos::ClampMode::kMirrorClampToBorder:
                if (coord < 0 || coord >= s) border = true;
                return std::clamp(coord, 0.0f, s - 1.0f);
            default:
                if (mode == xenos::ClampMode::kMirrorClampToEdge || mode == xenos::ClampMode::kMirrorClampToHalfway)
                    coord = std::fabs(coord + 0.5f) - 0.5f;
                return std::clamp(coord, 0.0f, s - 1.0f);
            }
        }

        class Sampler : public ShaderInterpreter::TextureSampler
        {
        public:
            const RegisterFile* regs = nullptr;

            void Fetch(const ucode::TextureFetchInstruction& instr, const float* coords, float* result) override
            {
                auto fetch = regs->GetTextureFetch(instr.fetch_constant_index());
                if (fetch.type != xenos::FetchConstantType::kTexture)
                {
                    result[0] = result[1] = result[2] = result[3] = 0;
                    return;
                }
                auto tex = GetTexture(fetch);
                float u = coords[0], v = coords[1];
                if (!instr.unnormalized_coordinates())
                {
                    u *= float(tex->width);
                    v *= float(tex->height);
                }
                u += instr.offset_x();
                v += instr.offset_y();
                xenos::TextureFilter filter = instr.mag_filter();
                if (filter == xenos::TextureFilter::kUseFetchConst)
                    filter = fetch.mag_filter;

                float texel[4] = {};
                bool border = false;
                if (filter == xenos::TextureFilter::kLinear)
                {
                    float fu = u - 0.5f, fv = v - 0.5f;
                    float x0 = std::floor(fu), y0 = std::floor(fv);
                    float ax = fu - x0, ay = fv - y0;
                    for (int j = 0; j < 4; j++)
                    {
                        float wgt = ((j & 1) ? ax : 1 - ax) * ((j & 2) ? ay : 1 - ay);
                        if (wgt == 0) continue;
                        int tx = int(ApplyClamp(x0 + (j & 1), tex->width, fetch.clamp_x, border));
                        int ty = int(ApplyClamp(y0 + ((j >> 1) & 1), tex->height, fetch.clamp_y, border));
                        const float* t = &tex->texels[(size_t(ty) * tex->width + tx) * 4];
                        for (int c = 0; c < 4; c++) texel[c] += t[c] * wgt;
                    }
                }
                else
                {
                    int tx = int(ApplyClamp(std::floor(u), tex->width, fetch.clamp_x, border));
                    int ty = int(ApplyClamp(std::floor(v), tex->height, fetch.clamp_y, border));
                    memcpy(texel, &tex->texels[(size_t(ty) * tex->width + tx) * 4], 16);
                }
                if (border && fetch.clamp_x >= xenos::ClampMode::kClampToBorder)
                {
                    // Border colors: 0 = transparent black, 1 = opaque black, 2 = opaque white.
                    float b = uint32_t(fetch.border_color) == 2 ? 1.0f : 0.0f;
                    texel[0] = texel[1] = texel[2] = b;
                    texel[3] = uint32_t(fetch.border_color) == 0 ? 0.0f : 1.0f;
                }
                // Fetch constant swizzle: 3 bits per output, 0-3 = XYZW, 4 = 0, 5 = 1.
                for (int c = 0; c < 4; c++)
                {
                    uint32_t s = (fetch.swizzle >> (3 * c)) & 7;
                    result[c] = s < 4 ? texel[s] : s == 5 ? 1.0f : 0.0f;
                }
            }
        };

        // ------------------------------------------------------------------
        // Vertices
        // ------------------------------------------------------------------
        constexpr int kMaxInterp = 16;

        struct Vertex
        {
            float clip[4];          // position as exported
            float interp[kMaxInterp][4];
            float pointSize;
        };

        struct ExportCapture : ShaderInterpreter::ExportSink
        {
            float position[4] = { 0, 0, 0, 1 };
            float interp[kMaxInterp][4] = {};
            float color[4][4] = {};
            float depth = 0;
            bool depthWritten = false;
            float pointSize = 0;

            void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override
            {
                uint32_t r = uint32_t(reg);
                float* dst = nullptr;
                if (r < kMaxInterp) dst = isPixel ? color[r & 3] : interp[r];
                else if (r == uint32_t(ucode::ExportRegister::kVSPosition)) dst = position;
                else if (isPixel && r == uint32_t(ucode::ExportRegister::kPSDepth)) { depth = value[0]; depthWritten = true; return; }
                else if (r == uint32_t(ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex)) { if (mask & 1) pointSize = value[0]; return; }
                if (!dst) return;
                for (int i = 0; i < 4; i++)
                    if (mask & (1u << i)) dst[i] = value[i];
            }
            bool isPixel = false;
        };

        // Screen-space vertex ready for rasterization.
        struct SVertex
        {
            float x, y, z, invW;
            float interp[kMaxInterp][4]; // already multiplied by invW
        };

        struct Triangle
        {
            SVertex v[3];
            bool front;
        };

        // ------------------------------------------------------------------
        // Worker pool
        // ------------------------------------------------------------------
        class Pool
        {
        public:
            void Start(unsigned n)
            {
                m_count = n;
                for (unsigned i = 0; i < n; i++)
                    m_threads.emplace_back([this, i] { Run(i); });
            }
            unsigned Count() const { return m_count; }
            // Runs job(worker) on every worker and waits for all of them.
            void Dispatch(const std::function<void(unsigned)>& job)
            {
                std::unique_lock lock(m_mutex);
                m_job = &job;
                m_pending = m_count;
                m_generation++;
                m_cv.notify_all();
                m_doneCv.wait(lock, [&] { return m_pending == 0; });
                m_job = nullptr;
            }
        private:
            void Run(unsigned index)
            {
                uint64_t seen = 0;
                for (;;)
                {
                    const std::function<void(unsigned)>* job;
                    {
                        std::unique_lock lock(m_mutex);
                        m_cv.wait(lock, [&] { return m_generation != seen; });
                        seen = m_generation;
                        job = m_job;
                    }
                    (*job)(index);
                    std::lock_guard lock(m_mutex);
                    if (--m_pending == 0)
                        m_doneCv.notify_all();
                }
            }
            std::vector<std::thread> m_threads;
            std::mutex m_mutex;
            std::condition_variable m_cv, m_doneCv;
            const std::function<void(unsigned)>* m_job = nullptr;
            unsigned m_pending = 0, m_count = 0;
            uint64_t m_generation = 0;
        };
        Pool g_pool;

        // Per-worker interpreter state.
        struct Worker
        {
            std::unique_ptr<ShaderInterpreter> interp;
            Sampler sampler;
            ExportCapture capture;
        };
        std::vector<std::unique_ptr<Worker>> g_workers;

        // ------------------------------------------------------------------
        // Fixed-function helpers
        // ------------------------------------------------------------------
        bool Compare(xenos::CompareFunction f, float a, float b)
        {
            switch (f)
            {
            case xenos::CompareFunction::kNever: return false;
            case xenos::CompareFunction::kLess: return a < b;
            case xenos::CompareFunction::kEqual: return a == b;
            case xenos::CompareFunction::kLessEqual: return a <= b;
            case xenos::CompareFunction::kGreater: return a > b;
            case xenos::CompareFunction::kNotEqual: return a != b;
            case xenos::CompareFunction::kGreaterEqual: return a >= b;
            default: return true;
            }
        }
        bool CompareU(xenos::CompareFunction f, uint32_t a, uint32_t b)
        {
            switch (f)
            {
            case xenos::CompareFunction::kNever: return false;
            case xenos::CompareFunction::kLess: return a < b;
            case xenos::CompareFunction::kEqual: return a == b;
            case xenos::CompareFunction::kLessEqual: return a <= b;
            case xenos::CompareFunction::kGreater: return a > b;
            case xenos::CompareFunction::kNotEqual: return a != b;
            case xenos::CompareFunction::kGreaterEqual: return a >= b;
            default: return true;
            }
        }
        uint32_t StencilOp(xenos::StencilOp op, uint32_t s, uint32_t ref)
        {
            switch (op)
            {
            case xenos::StencilOp::kZero: return 0;
            case xenos::StencilOp::kReplace: return ref;
            case xenos::StencilOp::kIncrementClamp: return std::min(s + 1, 255u);
            case xenos::StencilOp::kDecrementClamp: return s ? s - 1 : 0;
            case xenos::StencilOp::kInvert: return ~s & 0xFF;
            case xenos::StencilOp::kIncrementWrap: return (s + 1) & 0xFF;
            case xenos::StencilOp::kDecrementWrap: return (s - 1) & 0xFF;
            default: return s;
            }
        }
        float Factor(xenos::BlendFactor f, int c, const float* src, const float* dst, const float* constant)
        {
            switch (f)
            {
            case xenos::BlendFactor::kZero: return 0;
            case xenos::BlendFactor::kOne: return 1;
            case xenos::BlendFactor::kSrcColor: return src[c];
            case xenos::BlendFactor::kOneMinusSrcColor: return 1 - src[c];
            case xenos::BlendFactor::kSrcAlpha: return src[3];
            case xenos::BlendFactor::kOneMinusSrcAlpha: return 1 - src[3];
            case xenos::BlendFactor::kDstColor: return dst[c];
            case xenos::BlendFactor::kOneMinusDstColor: return 1 - dst[c];
            case xenos::BlendFactor::kDstAlpha: return dst[3];
            case xenos::BlendFactor::kOneMinusDstAlpha: return 1 - dst[3];
            case xenos::BlendFactor::kConstantColor: return constant[c];
            case xenos::BlendFactor::kOneMinusConstantColor: return 1 - constant[c];
            case xenos::BlendFactor::kConstantAlpha: return constant[3];
            case xenos::BlendFactor::kOneMinusConstantAlpha: return 1 - constant[3];
            case xenos::BlendFactor::kSrcAlphaSaturate: return c == 3 ? 1 : std::min(src[3], 1 - dst[3]);
            default: return 0;
            }
        }
        float BlendOp(xenos::BlendOp op, float s, float d)
        {
            switch (op)
            {
            case xenos::BlendOp::kSubtract: return s - d;
            case xenos::BlendOp::kMin: return std::min(s, d);
            case xenos::BlendOp::kMax: return std::max(s, d);
            case xenos::BlendOp::kRevSubtract: return d - s;
            default: return s + d;
            }
        }
        float LinearToGamma(float v)
        {
            v = std::clamp(v, 0.0f, 1.0f);
            return std::pow(v, 1.0f / 2.2f);
        }
        float GammaToLinear(float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 2.2f); }

        // ------------------------------------------------------------------
        // Draw state captured once per draw
        // ------------------------------------------------------------------
        struct RenderTarget
        {
            bool enabled = false;
            xenos::ColorRenderTargetFormat format{};
            uint32_t base = 0;
            bool is64 = false;
            uint32_t mask = 0;
            reg::RB_BLENDCONTROL blend{};
            bool blending = false;
        };

        struct DrawState
        {
            const RegisterFile* regs;
            uint32_t pitchTiles, msaaX, msaaY;
            int32_t scissor[4]; // x0, y0, x1, y1 (exclusive), pixels
            RenderTarget rt[4];
            bool depthEnabled, depthWrite, stencilEnabled, depthFloat, hasDepth;
            uint32_t depthBase;
            reg::RB_DEPTHCONTROL depthControl;
            reg::RB_STENCILREFMASK stencilFront, stencilBack;
            bool alphaTest;
            xenos::CompareFunction alphaFunc;
            float alphaRef;
            float blendConstant[4];
            uint32_t psRegs, interpCount;
            bool paramGen;
            uint32_t paramGenPos;
            bool perspective;
            bool colorPass;
            bool hasPS;
        };

        // 20e4 float depth used by D24FS8 surfaces.
        uint32_t ToFloat24(float f)
        {
            f = std::clamp(f, 0.0f, 1.99999988f);
            if (f < std::ldexp(1.0f, -15))
                return uint32_t(f * std::ldexp(1.0f, 15 + 20)) & 0xFFFFF;
            int e;
            float m = std::frexp(f, &e); // [0.5,1)
            uint32_t mant = uint32_t((m * 2.0f - 1.0f) * float(1 << 20));
            uint32_t exp = uint32_t(e - 1 + 15);
            return std::min((exp << 20) | (mant & 0xFFFFF), 0xFFFFFFu);
        }

        void ShadePixel(Worker& w, const DrawState& st, const Triangle& tri, float b0, float b1, float b2, int px, int py)
        {
            const SVertex* v = tri.v;
            float z = b0 * v[0].z + b1 * v[1].z + b2 * v[2].z;

            // Interpolants into the pixel shader's input registers.
            float* temps = w.interp->temp_registers();
            float invW = b0 * v[0].invW + b1 * v[1].invW + b2 * v[2].invW;
            float pw = (st.perspective && invW != 0) ? 1.0f / invW : 1.0f;
            for (uint32_t i = 0; i < st.interpCount; i++)
                for (int c = 0; c < 4; c++)
                    temps[i * 4 + c] = (b0 * v[0].interp[i][c] + b1 * v[1].interp[i][c] + b2 * v[2].interp[i][c]) * pw;
            if (st.paramGen && st.paramGenPos < 64)
            {
                float* p = &temps[st.paramGenPos * 4];
                p[0] = tri.front ? float(px) + 0.5f : -(float(px) + 0.5f);
                p[1] = float(py) + 0.5f;
                p[2] = 0; p[3] = 0;
            }

            // Early depth/stencil value read (test happens after the shader so
            // shader depth output and kills work).
            uint32_t* edram = gpu::Edram();
            w.capture = {};
            w.capture.isPixel = true;
            if (st.hasPS)
            {
                w.interp->Execute();
                if (w.interp->killed())
                    return;
            }

            float* color0 = w.capture.color[0];
            if (st.alphaTest && !Compare(st.alphaFunc, color0[3], st.alphaRef))
                return;
            if (w.capture.depthWritten)
                z = w.capture.depth;

            uint32_t samplesX = 1u << st.msaaX, samplesY = 1u << st.msaaY;
            for (uint32_t sy = 0; sy < samplesY; sy++)
            {
                for (uint32_t sx = 0; sx < samplesX; sx++)
                {
                    uint32_t xs = (uint32_t(px) << st.msaaX) + sx, ys = (uint32_t(py) << st.msaaY) + sy;

                    if (st.hasDepth)
                    {
                        uint32_t& d = edram[gpu::EdramIndex(st.depthBase, st.pitchTiles, xs, ys, false)];
                        uint32_t stored = d >> 8, stencil = d & 0xFF;
                        uint32_t depth = st.depthFloat ? ToFloat24(z) : uint32_t(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f);
                        bool pass = true;
                        uint32_t newStencil = stencil;
                        if (st.stencilEnabled)
                        {
                            const auto& sr = tri.front || !st.depthControl.backface_enable ? st.stencilFront : st.stencilBack;
                            auto func = tri.front || !st.depthControl.backface_enable ? st.depthControl.stencilfunc : st.depthControl.stencilfunc_bf;
                            auto opFail = tri.front || !st.depthControl.backface_enable ? st.depthControl.stencilfail : st.depthControl.stencilfail_bf;
                            auto opZFail = tri.front || !st.depthControl.backface_enable ? st.depthControl.stencilzfail : st.depthControl.stencilzfail_bf;
                            auto opPass = tri.front || !st.depthControl.backface_enable ? st.depthControl.stencilzpass : st.depthControl.stencilzpass_bf;
                            bool sPass = CompareU(func, sr.stencilref & sr.stencilmask, stencil & sr.stencilmask);
                            bool zPass = !st.depthEnabled || CompareU(st.depthControl.zfunc, depth, stored);
                            uint32_t result = StencilOp(!sPass ? opFail : !zPass ? opZFail : opPass, stencil, sr.stencilref);
                            newStencil = (stencil & ~sr.stencilwritemask) | (result & sr.stencilwritemask);
                            pass = sPass && zPass;
                        }
                        else if (st.depthEnabled)
                            pass = CompareU(st.depthControl.zfunc, depth, stored);
                        uint32_t newDepth = (pass && st.depthWrite) ? depth : stored;
                        d = (newDepth << 8) | (newStencil & 0xFF);
                        if (!pass)
                            continue;
                    }

                    if (!st.colorPass)
                        continue;
                    for (int i = 0; i < 4; i++)
                    {
                        const RenderTarget& rt = st.rt[i];
                        if (!rt.enabled)
                            continue;
                        uint32_t* d = &edram[gpu::EdramIndex(rt.base, st.pitchTiles, xs, ys, rt.is64)];
                        float src[4];
                        memcpy(src, w.capture.color[i], 16);
                        bool gamma = rt.format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
                        float dst[4];
                        gpu::DecodeColor(rt.format, d, dst);
                        if (gamma)
                            for (int c = 0; c < 3; c++) dst[c] = GammaToLinear(dst[c]);
                        float out[4];
                        if (rt.blending)
                        {
                            for (int c = 0; c < 3; c++)
                                out[c] = BlendOp(rt.blend.color_comb_fcn,
                                                 src[c] * Factor(rt.blend.color_srcblend, c, src, dst, st.blendConstant),
                                                 dst[c] * Factor(rt.blend.color_destblend, c, src, dst, st.blendConstant));
                            out[3] = BlendOp(rt.blend.alpha_comb_fcn,
                                             src[3] * Factor(rt.blend.alpha_srcblend, 3, src, dst, st.blendConstant),
                                             dst[3] * Factor(rt.blend.alpha_destblend, 3, src, dst, st.blendConstant));
                        }
                        else
                            memcpy(out, src, 16);
                        for (int c = 0; c < 4; c++)
                            if (!(rt.mask & (1u << c)))
                                out[c] = dst[c];
                        if (gamma)
                            for (int c = 0; c < 3; c++) out[c] = LinearToGamma(out[c]);
                        gpu::EncodeColor(rt.format, out, d);
                    }
                }
            }
        }

        // Rasterizes the rows of one triangle that belong to this worker.
        void RasterTriangle(Worker& w, const DrawState& st, const Triangle& tri, unsigned worker, unsigned workers)
        {
            const SVertex* v = tri.v;
            float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
            if (area == 0 || !std::isfinite(area))
                return;
            float minX = std::min({ v[0].x, v[1].x, v[2].x }), maxX = std::max({ v[0].x, v[1].x, v[2].x });
            float minY = std::min({ v[0].y, v[1].y, v[2].y }), maxY = std::max({ v[0].y, v[1].y, v[2].y });
            int x0 = std::max(st.scissor[0], int(std::floor(minX))), x1 = std::min(st.scissor[2], int(std::ceil(maxX)) + 1);
            int y0 = std::max(st.scissor[1], int(std::floor(minY))), y1 = std::min(st.scissor[3], int(std::ceil(maxY)) + 1);
            if (x0 >= x1 || y0 >= y1)
                return;

            // Orient edges so that "inside" is positive.
            float sign = area > 0 ? 1.0f : -1.0f;
            float invArea = 1.0f / (area * sign);
            auto edge = [&](const SVertex& a, const SVertex& b, float px, float py) {
                return ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * sign;
            };
            // Top-left rule: an edge owns pixels exactly on it only if it is a
            // top or left edge.
            auto owns = [&](const SVertex& a, const SVertex& b) {
                float dx = (b.x - a.x) * sign, dy = (b.y - a.y) * sign;
                return dy < 0 || (dy == 0 && dx > 0);
            };
            bool own0 = owns(v[1], v[2]), own1 = owns(v[2], v[0]), own2 = owns(v[0], v[1]);

            for (int y = y0; y < y1; y++)
            {
                if (unsigned((y >> 3)) % workers != worker)
                    continue;
                float py = float(y) + 0.5f;
                for (int x = x0; x < x1; x++)
                {
                    float px = float(x) + 0.5f;
                    float e0 = edge(v[1], v[2], px, py), e1 = edge(v[2], v[0], px, py), e2 = edge(v[0], v[1], px, py);
                    if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                    if ((e0 == 0 && !own0) || (e1 == 0 && !own1) || (e2 == 0 && !own2)) continue;
                    ShadePixel(w, st, tri, e0 * invArea, e1 * invArea, e2 * invArea, x, y);
                }
            }
        }

        // Clips a polygon in clip space against one plane: dot(plane, v) >= 0.
        void ClipPolygon(std::vector<Vertex>& poly, const float plane[4], uint32_t interpCount)
        {
            std::vector<Vertex> out;
            size_t n = poly.size();
            for (size_t i = 0; i < n; i++)
            {
                const Vertex& a = poly[i];
                const Vertex& b = poly[(i + 1) % n];
                float da = plane[0] * a.clip[0] + plane[1] * a.clip[1] + plane[2] * a.clip[2] + plane[3] * a.clip[3];
                float db = plane[0] * b.clip[0] + plane[1] * b.clip[1] + plane[2] * b.clip[2] + plane[3] * b.clip[3];
                if (da >= 0)
                    out.push_back(a);
                if ((da >= 0) != (db >= 0))
                {
                    float t = da / (da - db);
                    Vertex m;
                    for (int c = 0; c < 4; c++) m.clip[c] = a.clip[c] + (b.clip[c] - a.clip[c]) * t;
                    for (uint32_t k = 0; k < interpCount; k++)
                        for (int c = 0; c < 4; c++) m.interp[k][c] = a.interp[k][c] + (b.interp[k][c] - a.interp[k][c]) * t;
                    m.pointSize = a.pointSize;
                    out.push_back(m);
                }
            }
            poly.swap(out);
        }
    }

    // ----------------------------------------------------------------------
    void Init()
    {
        g_memory = xe::Memory(g_base + mem::PHYSICAL_BASE);
        unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
        n = n > 2 ? n - 1 : n;
        g_pool.Start(n);
        for (unsigned i = 0; i < n + 1; i++)
        {
            auto w = std::make_unique<Worker>();
            g_workers.push_back(std::move(w));
        }
        LOG("[raster] software rasterizer: %u worker threads", n);
    }

    void LoadShader(uint32_t type, uint32_t guestAddr, uint32_t sizeDwords)
    {
        if (sizeDwords == 0 || sizeDwords > 0x10000)
            return;
        std::vector<uint32_t> code(sizeDwords);
        for (uint32_t i = 0; i < sizeDwords; i++)
            code[i] = Load32(guestAddr + i * 4);
        uint64_t key = Hash(code.data(), code.size() * 4) ^ (uint64_t(type) << 62);
        auto& slot = g_shaders[key];
        if (!slot)
        {
            slot = std::make_shared<Shader>();
            slot->ucode = std::move(code);
            // Debug aid: VR_DUMP_SHADERS=1 saves each new shader's microcode
            // (host byte order) to shaders/ for offline translator tests.
            if (type <= 1 && getenv("VR_DUMP_SHADERS"))
            {
                std::error_code ec;
                std::filesystem::create_directories("shaders", ec);
                char name[64];
                snprintf(name, sizeof(name), "shaders/%s_%016llx.bin", type == 0 ? "vs" : "ps", (unsigned long long)key);
                if (FILE* f = fopen(name, "wb"))
                {
                    fwrite(slot->ucode.data(), 4, slot->ucode.size(), f);
                    fclose(f);
                }
            }
        }
        if (type == 0)
            g_vs = slot;
        else if (type == 1)
            g_ps = slot;
    }

    void NewFrame()
    {
        std::lock_guard lock(g_texMutex);
        g_textures.clear();
    }

    void Draw(const RegisterFile& regs, uint32_t drawInitiator, uint32_t dmaBase, uint32_t dmaSize)
    {
        if (!g_vs)
            return;
        reg::VGT_DRAW_INITIATOR init;
        init.value = drawInitiator;
        auto prim = init.prim_type;
        uint32_t numIndices = init.num_indices;
        if (numIndices == 0)
            return;

        auto modeControl = regs.Get<reg::RB_MODECONTROL>();
        auto surfaceInfo = regs.Get<reg::RB_SURFACE_INFO>();
        auto programCntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
        auto contextMisc = regs.Get<reg::SQ_CONTEXT_MISC>();
        auto scMode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        auto clipCntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
        auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();

        DrawState st{};
        st.regs = &regs;
        st.msaaX = surfaceInfo.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
        st.msaaY = surfaceInfo.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
        st.pitchTiles = xenos::GetSurfacePitchTiles(surfaceInfo.surface_pitch, surfaceInfo.msaa_samples, false);
        st.hasPS = g_ps != nullptr;
        st.colorPass = modeControl.edram_mode == xenos::EdramMode::kColorDepth && g_ps;
        uint32_t colorMask = regs[XE_GPU_REG_RB_COLOR_MASK];
        static const Register kBlendRegs[4] = { XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2, XE_GPU_REG_RB_BLENDCONTROL3 };
        for (int i = 0; i < 4; i++)
        {
            RenderTarget& rt = st.rt[i];
            rt.mask = (colorMask >> (4 * i)) & 15;
            rt.enabled = st.colorPass && rt.mask != 0;
            if (!rt.enabled) continue;
            auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            rt.format = ci.color_format;
            rt.base = ci.color_base;
            rt.is64 = xenos::IsColorRenderTargetFormat64bpp(rt.format);
            rt.blend.value = regs[kBlendRegs[i]];
            rt.blending = !(rt.blend.color_srcblend == xenos::BlendFactor::kOne && rt.blend.color_destblend == xenos::BlendFactor::kZero &&
                            rt.blend.color_comb_fcn == xenos::BlendOp::kAdd && rt.blend.alpha_srcblend == xenos::BlendFactor::kOne &&
                            rt.blend.alpha_destblend == xenos::BlendFactor::kZero && rt.blend.alpha_comb_fcn == xenos::BlendOp::kAdd);
        }
        st.depthControl = regs.Get<reg::RB_DEPTHCONTROL>();
        st.depthEnabled = st.depthControl.z_enable;
        st.depthWrite = st.depthControl.z_enable && st.depthControl.z_write_enable;
        st.stencilEnabled = st.depthControl.stencil_enable;
        st.hasDepth = st.depthEnabled || st.stencilEnabled;
        auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
        st.depthBase = depthInfo.depth_base;
        st.depthFloat = depthInfo.depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
        st.stencilFront.value = regs[XE_GPU_REG_RB_STENCILREFMASK];
        st.stencilBack.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
        auto colorControl = regs.Get<reg::RB_COLORCONTROL>();
        st.alphaTest = colorControl.alpha_test_enable;
        st.alphaFunc = colorControl.alpha_func;
        memcpy(&st.alphaRef, &regs[XE_GPU_REG_RB_ALPHA_REF], 4);
        for (int i = 0; i < 4; i++)
            memcpy(&st.blendConstant[i], &regs[XE_GPU_REG_RB_BLEND_RED + i], 4);
        st.psRegs = programCntl.ps_num_reg + 1;
        st.interpCount = std::min<uint32_t>({ uint32_t(kMaxInterp), st.psRegs, programCntl.vs_export_count + 1u });
        st.paramGen = programCntl.param_gen;
        st.paramGenPos = contextMisc.param_gen_pos;
        st.perspective = !scMode.persp_corr_dis;

        if (!st.colorPass && !st.hasDepth)
            return;

        // Scissor: window scissor (+ window offset), screen scissor, surface.
        auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
        auto wtl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
        auto wbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
        int32_t sx0 = wtl.tl_x, sy0 = wtl.tl_y, sx1 = wbr.br_x, sy1 = wbr.br_y;
        if (!wtl.window_offset_disable)
        {
            sx0 += wo.window_x_offset; sx1 += wo.window_x_offset;
            sy0 += wo.window_y_offset; sy1 += wo.window_y_offset;
        }
        uint32_t stl = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL], sbr = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR];
        sx0 = std::max<int32_t>(sx0, int32_t(stl & 0x7FFF));
        sy0 = std::max<int32_t>(sy0, int32_t((stl >> 16) & 0x7FFF));
        sx1 = std::min<int32_t>(sx1, int32_t(sbr & 0x7FFF));
        sy1 = std::min<int32_t>(sy1, int32_t((sbr >> 16) & 0x7FFF));
        st.scissor[0] = std::max(sx0, 0);
        st.scissor[1] = std::max(sy0, 0);
        st.scissor[2] = std::min<int32_t>(sx1, int32_t(surfaceInfo.surface_pitch));
        st.scissor[3] = std::min<int32_t>(sy1, 4096);
        if (st.scissor[0] >= st.scissor[2] || st.scissor[1] >= st.scissor[3])
            return;

        // --- Indices ---
        std::vector<uint32_t> indices(numIndices);
        uint32_t indexOffset = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
        uint32_t minIndex = regs[XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF, maxIndex = regs[XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
        uint32_t resetIndex = regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
        bool useReset = scMode.multi_prim_ib_ena;
        std::vector<bool> isReset(numIndices, false);
        if (init.source_select == xenos::SourceSelect::kDMA)
        {
            bool is32 = init.index_size == xenos::IndexFormat::kInt32;
            reg::VGT_DMA_SIZE ds;
            ds.value = dmaSize;
            xenos::Endian endian = ds.swap_mode;
            uint32_t addr = dmaBase & ~(is32 ? 3u : 1u);
            for (uint32_t i = 0; i < numIndices; i++)
            {
                uint32_t idx;
                if (is32)
                {
                    uint8_t b[4];
                    memcpy(b, Phys(addr + i * 4), 4);
                    SwapBlock(b, 4, endian);
                    memcpy(&idx, b, 4);
                }
                else
                {
                    uint8_t b[2];
                    memcpy(b, Phys(addr + i * 2), 2);
                    if (endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32) std::swap(b[0], b[1]);
                    idx = uint32_t(b[0]) | (uint32_t(b[1]) << 8);
                }
                if (useReset && (idx & 0xFFFFFF) == (resetIndex & (is32 ? 0xFFFFFF : 0xFFFF)))
                    isReset[i] = true;
                indices[i] = idx;
            }
        }
        else
        {
            for (uint32_t i = 0; i < numIndices; i++)
                indices[i] = i;
        }

        // --- Vertex shader (cached per index) ---
        Worker& mainWorker = *g_workers.back();
        if (!mainWorker.interp)
            mainWorker.interp = std::make_unique<ShaderInterpreter>(regs, g_memory);
        mainWorker.sampler.regs = &regs;
        mainWorker.interp->SetTextureSampler(&mainWorker.sampler);
        mainWorker.interp->SetExportSink(&mainWorker.capture);
        mainWorker.interp->SetShader(xenos::ShaderType::kVertex, g_vs->ucode.data());

        std::unordered_map<uint32_t, uint32_t> cache;
        std::vector<Vertex> verts;
        std::vector<uint32_t> vref(numIndices);
        for (uint32_t i = 0; i < numIndices; i++)
        {
            if (isReset[i]) { vref[i] = UINT32_MAX; continue; }
            uint32_t idx = std::clamp((indices[i] + indexOffset) & 0xFFFFFF, minIndex, std::max(minIndex, maxIndex));
            auto it = cache.find(idx);
            if (it != cache.end()) { vref[i] = it->second; continue; }
            float* t = mainWorker.interp->temp_registers();
            memset(t, 0, sizeof(float) * 4 * 64);
            t[0] = float(idx);
            mainWorker.capture = {};
            mainWorker.capture.isPixel = false;
            mainWorker.interp->Execute();
            Vertex v;
            memcpy(v.clip, mainWorker.capture.position, 16);
            memcpy(v.interp, mainWorker.capture.interp, sizeof(v.interp));
            v.pointSize = mainWorker.capture.pointSize;
            cache[idx] = uint32_t(verts.size());
            vref[i] = uint32_t(verts.size());
            verts.push_back(v);
        }

        // --- Primitive assembly ---
        std::vector<std::array<uint32_t, 3>> tris;
        std::vector<bool> triIsRect;
        auto addTri = [&](uint32_t a, uint32_t b, uint32_t c, bool rect = false) {
            if (vref[a] == UINT32_MAX || vref[b] == UINT32_MAX || vref[c] == UINT32_MAX) return;
            tris.push_back({ vref[a], vref[b], vref[c] });
            triIsRect.push_back(rect);
        };
        // Rectangle lists need a generated 4th vertex.
        auto addRect = [&](uint32_t i) {
            if (vref[i] == UINT32_MAX || vref[i + 1] == UINT32_MAX || vref[i + 2] == UINT32_MAX) return;
            const Vertex* p[3] = { &verts[vref[i]], &verts[vref[i + 1]], &verts[vref[i + 2]] };
            // The longest edge is the diagonal; the 4th vertex mirrors the corner across it.
            auto len = [&](int a, int b) { float dx = p[a]->clip[0] - p[b]->clip[0], dy = p[a]->clip[1] - p[b]->clip[1]; return dx * dx + dy * dy; };
            float l01 = len(0, 1), l12 = len(1, 2), l02 = len(0, 2);
            int corner, a, b;
            if (l01 >= l12 && l01 >= l02) { corner = 2; a = 0; b = 1; }
            else if (l12 >= l01 && l12 >= l02) { corner = 0; a = 1; b = 2; }
            else { corner = 1; a = 0; b = 2; }
            Vertex v4;
            for (int c = 0; c < 4; c++) v4.clip[c] = p[a]->clip[c] + p[b]->clip[c] - p[corner]->clip[c];
            for (int k = 0; k < kMaxInterp; k++)
                for (int c = 0; c < 4; c++) v4.interp[k][c] = p[a]->interp[k][c] + p[b]->interp[k][c] - p[corner]->interp[k][c];
            v4.pointSize = 0;
            uint32_t r4 = uint32_t(verts.size());
            verts.push_back(v4);
            tris.push_back({ vref[i], vref[i + 1], vref[i + 2] });
            triIsRect.push_back(true);
            uint32_t ra = vref[i + a], rb = vref[i + b];
            tris.push_back({ ra, rb, r4 });
            triIsRect.push_back(true);
        };
        switch (prim)
        {
        case xenos::PrimitiveType::kTriangleList:
            for (uint32_t i = 0; i + 2 < numIndices; i += 3) addTri(i, i + 1, i + 2);
            break;
        case xenos::PrimitiveType::kTriangleStrip:
        {
            uint32_t start = 0;
            for (uint32_t i = 0; i < numIndices; i++)
            {
                if (isReset[i]) { start = i + 1; continue; }
                uint32_t k = i - start;
                if (k >= 2) ((k & 1) ? addTri(i - 1, i - 2, i) : addTri(i - 2, i - 1, i));
            }
            break;
        }
        case xenos::PrimitiveType::kTriangleFan:
        {
            uint32_t start = 0;
            for (uint32_t i = 0; i < numIndices; i++)
            {
                if (isReset[i]) { start = i + 1; continue; }
                if (i - start >= 2) addTri(start, i - 1, i);
            }
            break;
        }
        case xenos::PrimitiveType::kRectangleList:
            for (uint32_t i = 0; i + 2 < numIndices; i += 3) addRect(i);
            break;
        case xenos::PrimitiveType::kQuadList:
            for (uint32_t i = 0; i + 3 < numIndices; i += 4) { addTri(i, i + 1, i + 2); addTri(i, i + 2, i + 3); }
            break;
        default:
        {
            static std::atomic<int> logged{ 0 };
            if (logged++ < 10)
                LOG("[raster] primitive type %u not drawn yet", uint32_t(prim));
            return;
        }
        }
        if (tris.empty())
            return;

        // --- Clip, project, set up ---
        bool clipEnabled = !clipCntl.clip_disable && !vte.vtx_xy_fmt;
        float xScale = 1, xOffset = 0, yScale = 1, yOffset = 0, zScale = 1, zOffset = 0;
        auto rf = [&](Register r) { float f; memcpy(&f, &regs[r], 4); return f; };
        if (vte.vport_x_scale_ena) xScale = rf(XE_GPU_REG_PA_CL_VPORT_XSCALE);
        if (vte.vport_x_offset_ena) xOffset = rf(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
        if (vte.vport_y_scale_ena) yScale = rf(XE_GPU_REG_PA_CL_VPORT_YSCALE);
        if (vte.vport_y_offset_ena) yOffset = rf(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
        if (vte.vport_z_scale_ena) zScale = rf(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
        if (vte.vport_z_offset_ena) zOffset = rf(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
        float halfPixel = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
        float winX = 0, winY = 0;
        if (scMode.vtx_window_offset_enable)
        {
            winX = float(wo.window_x_offset);
            winY = float(wo.window_y_offset);
        }

        auto project = [&](const Vertex& v, SVertex& s) {
            float w = v.clip[3];
            if (!vte.vtx_w0_fmt) w = w != 0 ? 1.0f / w : 1.0f; // clear: the shader wrote 1/W
            float invW = w != 0 ? 1.0f / w : 1.0f;
            float x = vte.vtx_xy_fmt ? v.clip[0] : v.clip[0] * invW;
            float y = vte.vtx_xy_fmt ? v.clip[1] : v.clip[1] * invW;
            float z = vte.vtx_z_fmt ? v.clip[2] : v.clip[2] * invW;
            s.x = x * xScale + xOffset + halfPixel + winX;
            s.y = y * yScale + yOffset + halfPixel + winY;
            s.z = z * zScale + zOffset;
            s.invW = (vte.vtx_xy_fmt || !st.perspective) ? 1.0f : invW;
            for (uint32_t k = 0; k < st.interpCount; k++)
                for (int c = 0; c < 4; c++) s.interp[k][c] = v.interp[k][c] * s.invW;
        };

        static const float kPlanes[6][4] = {
            { 0, 0, 1, 0 },  // z >= 0 (D3D near)
            { 0, 0, -1, 1 }, // z <= w
            { 1, 0, 0, 1 }, { -1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, -1, 0, 1 },
        };

        std::vector<Triangle> setup;
        setup.reserve(tris.size());
        for (size_t t = 0; t < tris.size(); t++)
        {
            const auto& tr = tris[t];
            std::vector<Vertex> poly = { verts[tr[0]], verts[tr[1]], verts[tr[2]] };
            if (clipEnabled)
            {
                bool inside = true;
                for (auto& v : poly)
                    for (auto& p : kPlanes)
                        inside = inside && (p[0] * v.clip[0] + p[1] * v.clip[1] + p[2] * v.clip[2] + p[3] * v.clip[3]) >= 0;
                if (!inside)
                {
                    for (auto& p : kPlanes)
                    {
                        ClipPolygon(poly, p, st.interpCount);
                        if (poly.size() < 3) break;
                    }
                    if (poly.size() < 3) continue;
                }
            }
            std::vector<SVertex> sv(poly.size());
            for (size_t i = 0; i < poly.size(); i++)
                project(poly[i], sv[i]);
            float area = (sv[1].x - sv[0].x) * (sv[2].y - sv[0].y) - (sv[2].x - sv[0].x) * (sv[1].y - sv[0].y);
            bool cw = area > 0;
            bool front = scMode.face ? cw : !cw; // face: 0 = front is counter-clockwise
            if (!triIsRect[t] && ((scMode.cull_front && front) || (scMode.cull_back && !front)))
                continue;
            for (size_t i = 1; i + 1 < sv.size(); i++)
            {
                Triangle tri;
                tri.v[0] = sv[0];
                tri.v[1] = sv[i];
                tri.v[2] = sv[i + 1];
                tri.front = front;
                setup.push_back(tri);
            }
        }
        if (setup.empty())
            return;

        // --- Rasterize in parallel (row bands) ---
        unsigned workers = g_pool.Count();
        std::function<void(unsigned)> job = [&](unsigned wi) {
            Worker& w = *g_workers[wi];
            if (!w.interp)
                w.interp = std::make_unique<ShaderInterpreter>(regs, g_memory);
            w.sampler.regs = &regs;
            w.interp->SetTextureSampler(&w.sampler);
            w.interp->SetExportSink(&w.capture);
            if (g_ps)
                w.interp->SetShader(xenos::ShaderType::kPixel, g_ps->ucode.data());
            for (const Triangle& tri : setup)
                RasterTriangle(w, st, tri, wi, workers);
        };
        g_pool.Dispatch(job);
    }
}
