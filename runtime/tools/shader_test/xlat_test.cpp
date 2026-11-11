// Differential test: runs every translated shader (gen.inc) and Xenia's
// interpreter on the same random inputs and compares all outputs.
#include "prelude_cpp.h"

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace xe::gpu;
using namespace xs;

thread_local XeEnv* xs::xe_env = nullptr;

namespace
{
    RegisterFile g_regs;
    std::vector<uint8_t> g_memory(16u << 20);
    bool g_pixel = false;

    float TestSample(uint32_t fc, const float* c)
    {
        return std::sin(c[0] * 1.7f + float(fc)) + c[1] * 0.25f - c[2] * 0.125f;
    }

    struct Sampler : ShaderInterpreter::TextureSampler
    {
        void Fetch(const ucode::TextureFetchInstruction& instr, const float* coords, float* result) override
        {
            float v = TestSample(instr.fetch_constant_index(), coords);
            result[0] = v; result[1] = v * 2.0f; result[2] = -v; result[3] = 0.5f;
        }
    };

    struct Sink : ShaderInterpreter::ExportSink
    {
        float4 pos, t[16], psize, c[4], depth;
        void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override
        {
            uint32_t rg = uint32_t(reg);
            float4* d = nullptr;
            if (!g_pixel)
            {
                if (rg < 16) d = &t[rg];
                else if (rg == uint32_t(ucode::ExportRegister::kVSPosition)) d = &pos;
                else if (rg == uint32_t(ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex)) d = &psize;
            }
            else
            {
                if (rg < 16) d = &c[rg & 3];
                else if (rg == uint32_t(ucode::ExportRegister::kPSDepth)) d = &depth;
            }
            if (!d) return;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) (*d)[i] = value[i];
        }
    };
}

float xs::xe_half(uint h)
{
    return xe::xenos_half_to_float(uint16_t(h));
}

float4 xs::xe_C(int index)
{
    auto bs = g_regs.Get<reg::SQ_VS_CONST>(g_pixel ? XE_GPU_REG_SQ_PS_CONST : XE_GPU_REG_SQ_VS_CONST);
    if (index < 0 || uint32_t(index) > bs.size) return float4();
    index += bs.base;
    if (index >= 512) return float4();
    float4 v;
    memcpy(&v, &g_regs[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * index], 16);
    return v;
}
bool xs::xe_B(uint index) { return (g_regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + (index >> 5)] >> (index & 31)) & 1; }
uint xs::xe_L(uint index) { return g_regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + index]; }
uint xs::xe_vfetch_base(uint fc) { return g_regs.GetVertexFetch(fc).address; }
uint xs::xe_vfetch_dword(uint fc, uint address)
{
    auto f = g_regs.GetVertexFetch(fc);
    if (address < f.address || address >= f.address + f.size) return 0;
    uint32_t v;
    memcpy(&v, &g_memory[size_t(address) * 4 % g_memory.size()], 4);
    return xenos::GpuSwap(v, f.endian);
}
float4 xs::xe_tfetch(uint fc, uint dimension, float4 coords, float4 offset, bool unnormalized)
{
    float c[3] = { coords.x, coords.y, coords.z };
    float v = TestSample(fc, c);
    return float4(v, v * 2.0f, -v, 0.5f);
}

namespace xs
{
#include "gen_macros.h"
#include "gen.inc"
}
#undef r

static bool Close(float a, float b)
{
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == b) return true;
    float d = std::fabs(a - b), m = std::max(std::fabs(a), std::fabs(b));
    return d <= 1e-5f * std::max(m, 1.0f);
}

int main()
{
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> small(0.0f, 8.0f), wide(-4.0f, 4.0f);
    for (size_t i = 0; i < g_memory.size(); i++) g_memory[i] = uint8_t(rng());
    xe::Memory memory(g_memory.data());

    int failures = 0;
    for (const xs::GenShader& gs : xs::kShaders)
    {
        std::ifstream in(gs.path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        std::vector<uint32_t> ucode(bytes.size() / 4 + 3);
        memcpy(ucode.data(), bytes.data(), bytes.size());
        g_pixel = gs.pixel != 0;

        int shaderFailures = 0;
        for (int trial = 0; trial < 200 && shaderFailures < 3; trial++)
        {
            // Random state.
            memset(&g_regs, 0, sizeof(g_regs));
            g_regs[XE_GPU_REG_SQ_VS_CONST] = 0x000FF000; // base 0, size 255
            g_regs[XE_GPU_REG_SQ_PS_CONST] = 0x000FF100; // base 256, size 255
            for (int i = 0; i < 512 * 4; i++)
            {
                float f = wide(rng);
                if ((rng() & 15) == 0) f = 0.0f;
                if ((rng() & 31) == 0) f = 1.0f;
                memcpy(&g_regs[XE_GPU_REG_SHADER_CONSTANT_000_X + i], &f, 4);
            }
            for (int i = 0; i < 8; i++) g_regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i] = rng();
            for (int i = 0; i < 32; i++)
                g_regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i] = (rng() % 4) | (0u << 8) | (1u << 16);
            for (int i = 0; i < 96; i++)
            {
                // Vertex fetch constants: in-range addresses, random endian.
                uint32_t base = (rng() % (uint32_t(g_memory.size()) / 8)) & ~3u;
                uint32_t* fcw = &g_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i * 2];
                fcw[0] = (base << 2) | 3;            // type vertex
                fcw[1] = (rng() & 3) | (0x10000u << 2); // endian, size
            }

            XeEnv env;
            for (int i = 0; i < 64; i++)
                env.r[i] = float4(small(rng), wide(rng), small(rng), wide(rng));
            XeEnv start = env;

            // Interpreter.
            ShaderInterpreter interp(g_regs, memory);
            Sampler sampler;
            Sink sink;
            interp.SetTextureSampler(&sampler);
            interp.SetExportSink(&sink);
            interp.SetShader(g_pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, ucode.data());
            memcpy(interp.temp_registers(), start.r, sizeof(start.r));
            interp.Execute();

            // Translated code.
            xe_env = &env;
            gs.fn();
            xe_env = nullptr;

            const float* it = interp.temp_registers();
            std::string diff;
            char buf[160];
            for (int i = 0; i < 64 && diff.empty(); i++)
                for (int c = 0; c < 4; c++)
                    if (!Close(env.r[i][c], it[i * 4 + c]))
                    {
                        snprintf(buf, sizeof(buf), "r%d.%c: translated %g interpreter %g", i, "xyzw"[c], env.r[i][c], it[i * 4 + c]);
                        diff = buf;
                        break;
                    }
            auto cmp = [&](const char* name, const float4& a, const float4& b) {
                for (int c = 0; c < 4 && diff.empty(); c++)
                    if (!Close(a[c], b[c]))
                    {
                        snprintf(buf, sizeof(buf), "%s.%c: translated %g interpreter %g", name, "xyzw"[c], a[c], b[c]);
                        diff = buf;
                    }
            };
            if (!g_pixel)
            {
                cmp("oPos", env.o_pos, sink.pos);
                cmp("oPts", env.o_psize, sink.psize);
                for (int i = 0; i < 16; i++) cmp(("o" + std::to_string(i)).c_str(), env.o_t[i], sink.t[i]);
            }
            else
            {
                for (int i = 0; i < 4; i++) cmp(("oC" + std::to_string(i)).c_str(), env.o_c[i], sink.c[i]);
                cmp("oDepth", env.o_depth, sink.depth);
                if (diff.empty() && env.kill != interp.killed())
                    diff = "kill flag differs";
            }
            if (!diff.empty())
            {
                printf("FAIL %s trial %d: %s\n", gs.name, trial, diff.c_str());
                shaderFailures++;
            }
        }
        if (shaderFailures) failures++;
        else printf("ok   %s\n", gs.name);
    }
    printf("%d of %zu shaders differ\n", failures, sizeof(xs::kShaders) / sizeof(xs::kShaders[0]));
    return failures ? 1 : 0;
}
