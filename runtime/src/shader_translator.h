// Xenos shader microcode -> structured source code.
//
// The output is a function body written in a small dialect that is valid both
// as HLSL (compiled on Windows for Direct3D 12) and as C++ (with
// shader_prelude_cpp.h), so every game shader can be checked against Xenia's
// interpreter on any machine. The semantics follow the interpreter
// (third_party/xenia/xenia/gpu/shader_interpreter.cc) instruction by
// instruction, including Direct3D 9 multiplication rules (0 * x = 0).
//
// Names the body expects from its prelude:
//   r[XE_TEMPS]           float4 temporaries (inputs are preloaded)
//   xe_p, xe_a0, xe_ps    predicate, address register, previous scalar
//   xe_kill               set by kill instructions (pixel shaders)
//   xe_o_pos, xe_o_t[16], xe_o_psize     vertex shader exports
//   xe_o_c[4], xe_o_depth                pixel shader exports
//   xe_C(i), xe_B(i), xe_L(i)            float / bool / loop constants
//   xe_vfetch_base(fc), xe_vfetch_dword(fc, address)   vertex memory
//   xe_tfetch(fc, dimension, coords, offset, unnormalized)
//   math helpers (xe_mul, xe_dp4, ...)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xlat
{
    enum class ShaderType { kVertex, kPixel };

    struct TranslatedShader
    {
        std::string body;
        bool ok = false;
        std::string error;
        uint32_t textureFetchMask = 0;    // texture fetch constants used
        uint32_t textureDimension[32] = {}; // xenos::FetchOpDimension per used slot
        uint32_t vertexFetchMask[3] = {}; // vertex fetch constants used (bit per index 0..95)
        uint32_t exportMask = 0;          // VS: interpolators written; PS: colors written
        bool writesPointSize = false;
        bool writesDepth = false;
        bool usesKill = false;
        uint32_t tempCount = 0;           // XE_TEMPS needed
        uint32_t floatConstCount = 0;     // float constants read: [0, n) (256 if any are addressed)
    };

    TranslatedShader Translate(ShaderType type, const uint32_t* ucode, uint32_t dwordCount);
}
