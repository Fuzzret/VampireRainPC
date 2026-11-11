// HLSL sources for the Direct3D 12 backend, compiled at runtime with
// D3DCompile (shader model 5.1). Translated game shaders are inserted into
// the VS/PS templates in place of "XE_BODY".
//
// Layout of the per-draw system constants (b0), in uint4 slots. Must match
// d3d12_backend.cpp (enum SysSlot).
//   0  vp_scale.xyz, vte_flags         (bit0 xy_fmt, bit1 z_fmt, bit2 w0_fmt, bit3 clip)
//   1  vp_offset.xyz, index_endian     (0 none, 1 swap16, 2 swap32)
//   2  index_offset, index_min, index_max, vs_const_size
//   3  ps_const_size, interp_count, param_gen (bit0 on, bits 8+ register), edram_pitch_tiles
//   4  msaa (bit0 x, bit1 y), rt_mask, rt_flags, depth_flags
//      rt_flags: bit i = RT i written, bit 4+i = blending, bit 8 = color pass
//      depth_flags: bit0 depth/stencil used, bit1 D24FS8
//   5  rt_base[4]
//   6  rt_format[4]
//   7  rt_blend[4] (RB_BLENDCONTROL values)
//   8  blend constant (float4)
//   9  depth_base, RB_DEPTHCONTROL, RB_STENCILREFMASK, RB_STENCILREFMASK_BF
//   10 alpha_func, alpha_ref (float bits), alpha_test, persp (unused)
//   11..42 tex_info[32]: swizzle, sign bits (2 per component), exp_adjust (int), flags (bit0 BGR remap)
//   43..74 tex_size[32]: uv scale x, uv scale y (guest size / host size), 1/width, 1/height (float)
//   75..82 sampler[32]: index of each fetch slot's sampler in the sampler heap (4 per slot group)
//   83 HUD fit: x flag (1 all vertices, 2 only flat ones with w = 1), y/z offset to the centred 1280x720 box (float), w window offset x (float)
//   84 HUD fit: render width, render height, 1280, window offset y (float)
#pragma once

namespace d3d12src
{
    // Constants, memory helpers and EDRAM / format helpers shared by all stages.
    // Resources of the draw root signature (VS/GS/PS).
    inline const char* kDrawDecls = R"HLSL(
cbuffer XeSystem : register(b0) { uint4 xe_sys[85]; };
cbuffer XeFloatConsts : register(b1) { float4 xe_cf[256]; };
cbuffer XeMiscConsts : register(b2) { uint4 xe_fetch[48]; uint4 xe_bools[2]; uint4 xe_loops[8]; };
ByteAddressBuffer xe_shared : register(t0);
)HLSL";

    inline const char* kCommon = R"HLSL(

uint xe_swap32(uint v, uint endian)
{
    if (endian == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == 2u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (endian == 3u) return (v << 16) | (v >> 16);
    return v;
}

// EDRAM layout: 2048 tiles of 80x16 samples (40x16 for 64bpp formats). At a
// render resolution scale S every sample is S x S host samples, so tiles are
// 80S x 16S; xs/ys are host (scaled) sample coordinates.
uint xe_edram_index(uint base, uint pitch_tiles, uint xs, uint ys, bool is64)
{
    const uint th = 16u * XE_RES_SCALE, ts = 1280u * XE_RES_SCALE * XE_RES_SCALE;
    if (!is64)
    {
        const uint tw = 80u * XE_RES_SCALE;
        uint tile = (base + (ys / th) * pitch_tiles + xs / tw) & 2047u;
        return tile * ts + (ys % th) * tw + xs % tw;
    }
    const uint tw64 = 40u * XE_RES_SCALE;
    uint tile64 = (base + (ys / th) * pitch_tiles * 2u + xs / tw64) & 2047u;
    return tile64 * ts + ((ys % th) * tw64 + xs % tw64) * 2u;
}

float xe_unorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
uint xe_to_unorm(float f, uint bits) { return uint(saturate(f) * float((1u << bits) - 1u) + 0.5); }
float xe_7e3_to_float(uint v)
{
    uint e = (v >> 7) & 7u, m = v & 127u;
    return e == 0u ? float(m) * exp2(-9.0) : float(m | 128u) * exp2(float(e) - 10.0);
}
uint xe_float_to_7e3(float f)
{
    f = clamp(f, 0.0, 31.875);
    if (f < 0.015625) return uint(f * 512.0 + 0.5);
    uint bits = asuint(f);
    int e = int((bits >> 23) & 0xFFu) - 127;      // f = 1.m * 2^e, e in [-6, 4]
    uint mant = ((bits & 0x7FFFFFu) + 0x8000u) >> 16; // round to 7 bits
    uint ex = uint(e + 3);
    if (mant == 128u) { mant = 0u; ex++; }
    if (ex > 7u) return 0x3FFu;
    return (ex << 7) | mant;
}
float xe_snorm16(uint v) { return float(int(v << 16) >> 16) * (32.0 / 32767.0); }
uint xe_to_snorm16(float f) { return uint(int(round(clamp(f, -32.0, 32.0) * (32767.0 / 32.0)))) & 0xFFFFu; }

bool xe_rt_is64(uint fmt) { return fmt == 5u || fmt == 7u || fmt == 15u; }

float4 xe_decode_rt(uint fmt, uint2 v)
{
    switch (fmt)
    {
    case 0u: case 1u:
        return float4(xe_unorm(v.x & 0xFFu, 8u), xe_unorm((v.x >> 8) & 0xFFu, 8u), xe_unorm((v.x >> 16) & 0xFFu, 8u), xe_unorm(v.x >> 24, 8u));
    case 2u: case 10u:
        return float4(xe_unorm(v.x & 0x3FFu, 10u), xe_unorm((v.x >> 10) & 0x3FFu, 10u), xe_unorm((v.x >> 20) & 0x3FFu, 10u), xe_unorm(v.x >> 30, 2u));
    case 3u: case 12u:
        return float4(xe_7e3_to_float(v.x & 0x3FFu), xe_7e3_to_float((v.x >> 10) & 0x3FFu), xe_7e3_to_float((v.x >> 20) & 0x3FFu), xe_unorm(v.x >> 30, 2u));
    case 4u:
        return float4(xe_snorm16(v.x & 0xFFFFu), xe_snorm16(v.x >> 16), 0.0, 1.0);
    case 5u:
        return float4(xe_snorm16(v.x & 0xFFFFu), xe_snorm16(v.x >> 16), xe_snorm16(v.y & 0xFFFFu), xe_snorm16(v.y >> 16));
    case 6u:
        return float4(f16tof32(v.x), f16tof32(v.x >> 16), 0.0, 1.0);
    case 7u:
        return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16));
    case 14u:
        return float4(asfloat(v.x), 0.0, 0.0, 1.0);
    case 15u:
        return float4(asfloat(v.x), asfloat(v.y), 0.0, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 0.0);
}

uint2 xe_encode_rt(uint fmt, float4 c)
{
    switch (fmt)
    {
    case 0u: case 1u:
        return uint2(xe_to_unorm(c.x, 8u) | (xe_to_unorm(c.y, 8u) << 8) | (xe_to_unorm(c.z, 8u) << 16) | (xe_to_unorm(c.w, 8u) << 24), 0u);
    case 2u: case 10u:
        return uint2(xe_to_unorm(c.x, 10u) | (xe_to_unorm(c.y, 10u) << 10) | (xe_to_unorm(c.z, 10u) << 20) | (xe_to_unorm(c.w, 2u) << 30), 0u);
    case 3u: case 12u:
        return uint2(xe_float_to_7e3(c.x) | (xe_float_to_7e3(c.y) << 10) | (xe_float_to_7e3(c.z) << 20) | (xe_to_unorm(c.w, 2u) << 30), 0u);
    case 4u:
        return uint2(xe_to_snorm16(c.x) | (xe_to_snorm16(c.y) << 16), 0u);
    case 5u:
        return uint2(xe_to_snorm16(c.x) | (xe_to_snorm16(c.y) << 16), xe_to_snorm16(c.z) | (xe_to_snorm16(c.w) << 16));
    case 6u:
        return uint2(f32tof16(c.x) | (f32tof16(c.y) << 16), 0u);
    case 7u:
        return uint2(f32tof16(c.x) | (f32tof16(c.y) << 16), f32tof16(c.z) | (f32tof16(c.w) << 16));
    case 14u:
        return uint2(asuint(c.x), 0u);
    case 15u:
        return uint2(asuint(c.x), asuint(c.y));
    }
    return uint2(0u, 0u);
}

// Byte offset of block (x, y) in a 2D tiled surface (pitch in blocks, a
// multiple of 32). Same as Xenia's texture_address::Tiled2D.
uint xe_tiled2d(uint x, uint y, uint pitch, uint bpb_log2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oib = (outer | inner) << bpb_log2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oib & 15u) | (((oib >> 4) & 1u) << 5) |
           (((oib >> 5) & 7u) << 8) | ((oib >> 8) << 12);
}
uint xe_tiled3d(uint x, uint y, uint z, uint pitch, uint height, uint bpb_log2)
{
    uint outer = ((((z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5)) + (x >> 5)) << 7;
    uint inner = ((z & 3u) << 5) | (((y >> 1) & 3u) << 3) | (x & 7u);
    uint oib = (outer | inner) << bpb_log2;
    uint bank = ((y >> 3) ^ (z >> 2)) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (bank << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oib & 15u) | (((oib >> 4) & 1u) << 5) |
           (((oib >> 5) & 7u) << 8) | ((oib >> 8) << 12);
}
)HLSL";

    // The dialect used by translated shaders (HLSL side of prelude_cpp.h).
    inline const char* kDialect = R"HLSL(
#define XE_LOOP [loop]
#define XE_FLT_MAX 3.402823466e+38

float xe_flush(float v) { return v; }
float4 xe_swz(float4 v, uint a, uint b, uint c, uint d) { return float4(v[a], v[b], v[c], v[d]); }
float4 xe_rep(float v) { return float4(v, v, v, v); }
float xe_mul1(float a, float b) { return (a != 0.0 && b != 0.0) ? a * b : 0.0; }
float4 xe_mul(float4 a, float4 b) { return float4(xe_mul1(a.x, b.x), xe_mul1(a.y, b.y), xe_mul1(a.z, b.z), xe_mul1(a.w, b.w)); }
float xe_dp4(float4 a, float4 b) { float s = xe_mul1(a.x, b.x); s += xe_mul1(a.y, b.y); s += xe_mul1(a.z, b.z); s += xe_mul1(a.w, b.w); return s; }
float xe_dp3(float4 a, float4 b) { float s = xe_mul1(a.x, b.x); s += xe_mul1(a.y, b.y); s += xe_mul1(a.z, b.z); return s; }
float xe_dp2(float4 a, float4 b) { float s = xe_mul1(a.x, b.x); s += xe_mul1(a.y, b.y); return s; }
float4 xe_max(float4 a, float4 b) { return float4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w); }
float4 xe_min(float4 a, float4 b) { return float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }
float4 xe_seq(float4 a, float4 b) { return float4(a.x == b.x ? 1.0 : 0.0, a.y == b.y ? 1.0 : 0.0, a.z == b.z ? 1.0 : 0.0, a.w == b.w ? 1.0 : 0.0); }
float4 xe_sgt(float4 a, float4 b) { return float4(a.x > b.x ? 1.0 : 0.0, a.y > b.y ? 1.0 : 0.0, a.z > b.z ? 1.0 : 0.0, a.w > b.w ? 1.0 : 0.0); }
float4 xe_sge(float4 a, float4 b) { return float4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0); }
float4 xe_sne(float4 a, float4 b) { return float4(a.x != b.x ? 1.0 : 0.0, a.y != b.y ? 1.0 : 0.0, a.z != b.z ? 1.0 : 0.0, a.w != b.w ? 1.0 : 0.0); }
float4 xe_cndeq(float4 a, float4 b, float4 c) { return float4(a.x == 0.0 ? b.x : c.x, a.y == 0.0 ? b.y : c.y, a.z == 0.0 ? b.z : c.z, a.w == 0.0 ? b.w : c.w); }
float4 xe_cndge(float4 a, float4 b, float4 c) { return float4(a.x >= 0.0 ? b.x : c.x, a.y >= 0.0 ? b.y : c.y, a.z >= 0.0 ? b.z : c.z, a.w >= 0.0 ? b.w : c.w); }
float4 xe_cndgt(float4 a, float4 b, float4 c) { return float4(a.x > 0.0 ? b.x : c.x, a.y > 0.0 ? b.y : c.y, a.z > 0.0 ? b.z : c.z, a.w > 0.0 ? b.w : c.w); }
float xe_max4(float4 v)
{
    if (v.x >= v.y && v.x >= v.z && v.x >= v.w) return v.x;
    if (v.y >= v.z && v.y >= v.w) return v.y;
    if (v.z >= v.w) return v.z;
    return v.w;
}
float4 xe_cube(float4 v)
{
    float x = v.z, y = v.w, z = v.x;
    float ax = abs(x), ay = abs(y), az = abs(z);
    float4 res;
    if (az >= ax && az >= ay) res = float4(-y, z < 0.0 ? -x : x, z, z < 0.0 ? 5.0 : 4.0);
    else if (ay >= ax) res = float4(y < 0.0 ? -z : z, x, y, y < 0.0 ? 3.0 : 2.0);
    else res = float4(-y, x < 0.0 ? z : -z, x, x < 0.0 ? 1.0 : 0.0);
    res.z *= 2.0;
    return res;
}
bool xe_isfinite(float v) { return !isinf(v) && !isnan(v); }
float xe_logc(float v) { float l = log2(v); return (isinf(l) && l < 0.0) ? -XE_FLT_MAX : l; }
float xe_clampinf(float v) { return isinf(v) ? (v < 0.0 ? -XE_FLT_MAX : XE_FLT_MAX) : v; }
float xe_zeroinf(float v) { return isinf(v) ? (v < 0.0 ? -0.0 : 0.0) : v; }
float xe_half(uint h)
{
    uint e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0u ? float(m) * exp2(-24.0) : float(m | 1024u) * exp2(float(e) - 25.0);
    return (h & 0x8000u) != 0u ? -v : v;
}

float4 xe_C(int i)
{
    if (i < 0 || uint(i) > XE_CONST_SIZE) return float4(0.0, 0.0, 0.0, 0.0);
    float4 c = xe_cf[uint(i) & 255u];
#ifdef XE_INTERP
    // In-between frames (frame rates above 30): blend from this draw's
    // constants in the previous game frame (xe_cfp, the same values when the
    // draw has no match) towards the current ones. xe_interp.x is 1 for real
    // frames.
    if (xe_interp.x < 1.0)
    {
        float4 p = xe_cfp[uint(i) & 255u];
        if (any(p != c)) c = lerp(p, c, xe_interp.x);
    }
#endif
    return c;
}
bool xe_B(uint i) { return ((xe_bools[i >> 7][(i >> 5) & 3u] >> (i & 31u)) & 1u) != 0u; }
uint xe_L(uint i) { return xe_loops[i >> 2][i & 3u]; }
uint xe_fetch_dword(uint d) { return xe_fetch[d >> 2][d & 3u]; }
uint xe_vfetch_base(uint fc) { return xe_fetch_dword(fc * 2u) >> 2; }
uint xe_vfetch_dword(uint fc, uint address)
{
    uint d0 = xe_fetch_dword(fc * 2u), d1 = xe_fetch_dword(fc * 2u + 1u);
    uint base = d0 >> 2, size = (d1 >> 2) & 0xFFFFFFu;
    if (address < base || address >= base + size) return 0u;
    return xe_swap32(xe_shared.Load((address & 0x07FFFFFFu) * 4u), d1 & 3u);
}
)HLSL";

    // Texture sampling for pixel and vertex shaders.
    inline const char* kTextures = R"HLSL(
Texture2D<float4> xe_tex[32] : register(t1);
// The same descriptors, for slots sampled as cube maps.
TextureCube<float4> xe_texc[32] : register(t1, space1);
// Every distinct sampler has a permanent place in the heap; xe_sys says
// which one each fetch slot uses.
SamplerState xe_samp[2048] : register(s0);

float4 xe_tfetch_finish(uint fc, float4 t)
{
    uint4 info = xe_sys[11u + fc];
    if ((info.w & 1u) != 0u) t = float4(t.z, t.y, t.x, t.w); // host BGR order -> data XYZ
    float d[4] = { t.x, t.y, t.z, t.w };
    [unroll] for (uint c = 0u; c < 4u; c++)
    {
        uint s = (info.y >> (c * 2u)) & 3u;
        if (s == 1u) { float v = d[c] * 255.0; d[c] = max(-1.0, (v > 127.5 ? v - 256.0 : v) / 127.0); }
        else if (s == 2u) d[c] = d[c] * 2.0 - 1.0;
        else if (s == 3u) d[c] = pow(max(d[c], 0.0), 2.2);
    }
    float scale = exp2(float(int(info.z)));
    float4 o;
    [unroll] for (uint k = 0u; k < 4u; k++)
    {
        uint s = (info.x >> (k * 3u)) & 7u;
        o[k] = s < 4u ? d[s] * scale : (s == 5u ? 1.0 : 0.0);
    }
    return o;
}

// Cube map fetches take the face coordinates the cube instruction made,
// moved to 1..2 (x = S, y = T), and the face index in z. Turn them back into
// a direction: +X (1, -T, -S), -X (-1, -T, S), +Y (S, 1, T), -Y (S, -1, -T),
// +Z (S, -T, 1), -Z (-S, -T, -1).
float3 xe_cube_dir(float4 c)
{
    float s = c.x * 2.0 - 3.0, t = c.y * 2.0 - 3.0;
    uint face = uint(clamp(c.z + 0.5, 0.0, 5.0));
    if (face == 0u) return float3(1.0, -t, -s);
    if (face == 1u) return float3(-1.0, -t, s);
    if (face == 2u) return float3(s, 1.0, t);
    if (face == 3u) return float3(s, -1.0, -t);
    if (face == 4u) return float3(s, -t, 1.0);
    return float3(-s, -t, -1.0);
}

float4 xe_tfetch(uint fc, uint dimension, float4 coords, float4 offset, bool unnormalized)
{
    if (dimension == 3u)
    {
        float3 dir = xe_cube_dir(coords);
#ifdef XE_PS
        return xe_tfetch_finish(fc, xe_texc[fc].Sample(xe_samp[xe_sys[75u + (fc >> 2)][fc & 3u]], dir));
#else
        return xe_tfetch_finish(fc, xe_texc[fc].SampleLevel(xe_samp[xe_sys[75u + (fc >> 2)][fc & 3u]], dir, 0.0));
#endif
    }
    float4 size = asfloat(xe_sys[43u + fc]);
    float2 uv = unnormalized ? coords.xy * size.zw : coords.xy;
    uv = (uv + offset.xy * size.zw) * size.xy;
#ifdef XE_PS
    // Pixel shaders pick the mip level from screen-space derivatives.
    return xe_tfetch_finish(fc, xe_tex[fc].Sample(xe_samp[xe_sys[75u + (fc >> 2)][fc & 3u]], uv));
#else
    return xe_tfetch_finish(fc, xe_tex[fc].SampleLevel(xe_samp[xe_sys[75u + (fc >> 2)][fc & 3u]], uv, 0.0));
#endif
}
)HLSL";

    // HUD drawn for a 1280x720 screen while the game renders at another
    // size (other aspect ratios): positions move into a centred 1280x720 box,
    // and points on the screen's edges go to the real edges (full-screen
    // overlays and edge-to-edge bars still cover the screen).
    inline const char* kFitHud = R"HLSL(
float2 xe_fit_hud(float2 s)
{
    if (xe_sys[83].x == 0u)
        return s;
    float2 d = asfloat(xe_sys[83].yz);
    float2 win = float2(asfloat(xe_sys[83].w), asfloat(xe_sys[84].w));
    float2 size = asfloat(xe_sys[84].xy);
    float2 ui = s - win; // in the 1280x720 layout
    ui.x = ui.x <= 1.0 ? 0.0 : (ui.x >= 1279.0 ? size.x : ui.x + d.x);
    ui.y = ui.y <= 1.0 ? 0.0 : (ui.y >= 719.0 ? size.y : ui.y + d.y);
    return ui + win;
}
)HLSL";

    // Vertex shader template.
    inline const char* kVertex = R"HLSL(
struct XeVSOut
{
    float4 t[16] : TEXCOORD;
    noperspective float2 depth : XEDEPTH;     // guest screen z, guest w
    float psize : PSIZE;
    float4 pos : SV_Position;
    float2 clip : SV_ClipDistance0;
};

XeVSOut main(uint vertex_id : SV_VertexID)
{
    float4 r[XE_TEMPS];
    [unroll] for (uint ri = 0u; ri < XE_TEMPS; ri++) r[ri] = float4(0.0, 0.0, 0.0, 0.0);
    bool xe_p = false, xe_kill = false;
    int xe_a0 = 0;
    float xe_ps = 0.0;
    uint xe_vf_addr = 0u;
    int xe_ld = 0, xe_cd = 0;
    uint xe_lc[4] = { 0u, 0u, 0u, 0u }, xe_li[4] = { 0u, 0u, 0u, 0u }, xe_cr[4] = { 0u, 0u, 0u, 0u };
    int xe_lstart[4] = { 0, 0, 0, 0 }, xe_lstep[4] = { 0, 0, 0, 0 };
    float4 xe_o_pos = float4(0.0, 0.0, 0.0, 1.0), xe_o_psize = float4(0.0, 0.0, 0.0, 0.0);
    float4 xe_o_t[16];
    [unroll] for (uint ti = 0u; ti < 16u; ti++) xe_o_t[ti] = float4(0.0, 0.0, 0.0, 0.0);
    float4 xe_o_c[4];
    float4 xe_o_depth = float4(0.0, 0.0, 0.0, 0.0);

    // Vertex index: byte swap (the host reads the guest's big-endian index
    // buffer), offset, clamp.
    uint xe_index = vertex_id;
    uint xe_endian = xe_sys[1].w;
    if (xe_endian == 1u) xe_index = ((xe_index & 0xFFu) << 8) | ((xe_index >> 8) & 0xFFu);
    else if (xe_endian == 2u) xe_index = xe_swap32(xe_index, 2u);
    xe_index = (xe_index + xe_sys[2].x) & 0xFFFFFFu;
    xe_index = clamp(xe_index, xe_sys[2].y, max(xe_sys[2].y, xe_sys[2].z));
    r[0].x = float(xe_index);

#define xe_aL xe_loop_address(xe_ld, xe_li, xe_lstart, xe_lstep)
XE_BODY
#undef xe_aL

    XeVSOut o;
    o.t = xe_o_t;
    o.psize = xe_o_psize.x;
    uint vte = xe_sys[0].w;
    float3 scale = asfloat(xe_sys[0].xyz), offs = asfloat(xe_sys[1].xyz);
    float4 p = xe_o_pos;
    // vtx_w0_fmt set: the shader wrote W itself; clear: it wrote 1/W.
    float w = (vte & 4u) != 0u ? p.w : (p.w != 0.0 ? 1.0 / p.w : 1.0);
    // Guest clipping (D3D: 0 <= z <= w) in guest clip space.
    o.clip = (vte & 8u) != 0u ? float2(p.z, w - p.z) : float2(1.0, 1.0);
    if ((vte & 1u) != 0u)
    {
        // Already in screen space; keep w for perspective-correct interpolation,
        // unless it's unusable (screen-space clears often leave w at 0, and a
        // host w <= 0 would clip the whole primitive away).
        if (!(w > 1e-20 && w < 1e30)) w = 1.0;
        float2 sxy = p.xy * scale.xy + offs.xy;
        float sz = ((vte & 2u) != 0u ? p.z : p.z / w) * scale.z + offs.z;
        sxy = xe_fit_hud(sxy);
        o.pos = float4((sxy.x * XE_NDC_K - 1.0) * w, (1.0 - sxy.y * XE_NDC_K) * w, 0.5 * w, w);
        o.depth = float2(sz, w);
    }
    else
    {
        // Screen = (clip / w) * scale + offset; host clip = screen mapped onto
        // the host viewport (XE_NDC_K = 2 / guest pixels it spans), times w.
        float2 hxy = (p.xy * scale.xy + offs.xy * w) * XE_NDC_K - float2(w, w);
        if (xe_sys[83].x == 1u || (xe_sys[83].x == 2u && abs(w - 1.0) < 1e-5))
        {
            // HUD laid out for 1280x720 (2D, w is normally 1): through screen space.
            float2 sxy = xe_fit_hud(p.xy / w * scale.xy + offs.xy);
            hxy = (sxy * XE_NDC_K - 1.0) * w;
        }
        o.pos = float4(hxy.x, -hxy.y, 0.5 * w, w);
        float sz = ((vte & 2u) != 0u ? p.z : (w != 0.0 ? p.z / w : 0.0)) * scale.z + offs.z;
        o.depth = float2(sz, w);
    }
    return o;
}
)HLSL";

    // Geometry shader expanding rectangle lists (3 corners -> 4).
    inline const char* kRectGs = R"HLSL(
struct XeVSOut
{
    float4 t[16] : TEXCOORD;
    noperspective float2 depth : XEDEPTH;
    float psize : PSIZE;
    float4 pos : SV_Position;
    float2 clip : SV_ClipDistance0;
};

XeVSOut xe_combine(XeVSOut a, XeVSOut b, XeVSOut c)
{
    XeVSOut o;
    [unroll] for (uint i = 0u; i < 16u; i++) o.t[i] = a.t[i] + b.t[i] - c.t[i];
    o.depth = a.depth + b.depth - c.depth;
    o.psize = c.psize;
    o.pos = a.pos + b.pos - c.pos;
    o.clip = a.clip + b.clip - c.clip;
    return o;
}

[maxvertexcount(4)]
void main(triangle XeVSOut v[3], inout TriangleStream<XeVSOut> s)
{
    // The longest edge is the diagonal; the 4th corner mirrors the remaining one.
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float l01 = dot(p0 - p1, p0 - p1), l12 = dot(p1 - p2, p1 - p2), l02 = dot(p0 - p2, p0 - p2);
    // Strip order: corner, diagonal end, diagonal end, generated corner, so
    // both triangles share the diagonal.
    if (l01 >= l12 && l01 >= l02)
    { s.Append(v[2]); s.Append(v[0]); s.Append(v[1]); s.Append(xe_combine(v[0], v[1], v[2])); }
    else if (l12 >= l01 && l12 >= l02)
    { s.Append(v[0]); s.Append(v[1]); s.Append(v[2]); s.Append(xe_combine(v[1], v[2], v[0])); }
    else
    { s.Append(v[1]); s.Append(v[0]); s.Append(v[2]); s.Append(xe_combine(v[0], v[2], v[1])); }
}
)HLSL";
}
