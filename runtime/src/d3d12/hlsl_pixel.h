// Pixel shader template (translated shader + emulated output merger writing
// the EDRAM through a rasterizer-ordered view), compute shaders for resolves
// and texture untiling, and the presentation shaders.
#pragma once

namespace d3d12src
{
    inline const char* kPixel = R"HLSL(
RasterizerOrderedByteAddressBuffer xe_edram : register(u0);

struct XePSIn
{
    float4 t[16] : TEXCOORD;
    noperspective float2 depth : XEDEPTH;
    float psize : PSIZE;
    float4 pos : SV_Position;
};

bool xe_compare(uint func, uint a, uint b)
{
    switch (func)
    {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    }
    return true;
}
bool xe_comparef(uint func, float a, float b)
{
    switch (func)
    {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    }
    return true;
}
uint xe_stencil_op(uint op, uint s, uint ref)
{
    switch (op)
    {
    case 1u: return 0u;
    case 2u: return ref;
    case 3u: return min(s + 1u, 255u);
    case 4u: return s > 0u ? s - 1u : 0u;
    case 5u: return (~s) & 0xFFu;
    case 6u: return (s + 1u) & 0xFFu;
    case 7u: return (s - 1u) & 0xFFu;
    }
    return s;
}
float xe_blend_factor(uint f, uint c, float4 src, float4 dst, float4 k)
{
    switch (f)
    {
    case 0u: return 0.0;
    case 1u: return 1.0;
    case 4u: return src[c];
    case 5u: return 1.0 - src[c];
    case 6u: return src.w;
    case 7u: return 1.0 - src.w;
    case 8u: return dst[c];
    case 9u: return 1.0 - dst[c];
    case 10u: return dst.w;
    case 11u: return 1.0 - dst.w;
    case 12u: return k[c];
    case 13u: return 1.0 - k[c];
    case 14u: return k.w;
    case 15u: return 1.0 - k.w;
    case 16u: return c == 3u ? 1.0 : min(src.w, 1.0 - dst.w);
    }
    return 0.0;
}
float xe_blend_op(uint op, float s, float d)
{
    switch (op)
    {
    case 1u: return s - d;
    case 2u: return min(s, d);
    case 3u: return max(s, d);
    case 4u: return d - s;
    }
    return s + d;
}
uint xe_float24(float f)
{
    f = clamp(f, 0.0, 1.99999988);
    if (f < exp2(-15.0)) return uint(f * exp2(35.0)) & 0xFFFFFu;
    uint bits = asuint(f);
    uint e = ((bits >> 23) & 0xFFu) - 127u + 15u;
    uint m = (bits >> 3) & 0xFFFFFu;
    return min((e << 20) | m, 0xFFFFFFu);
}

void main(XePSIn input, bool front_face : SV_IsFrontFace)
{
    float4 r[XE_TEMPS];
    [unroll] for (uint ri = 0u; ri < XE_TEMPS; ri++) r[ri] = float4(0.0, 0.0, 0.0, 0.0);
    uint interp_count = min(xe_sys[3].y, XE_TEMPS);
    [unroll] for (uint ii = 0u; ii < 16u; ii++)
        if (ii < interp_count) r[ii] = input.t[ii];
    uint param_gen = xe_sys[3].z;
    if ((param_gen & 1u) != 0u)
    {
        uint pr = (param_gen >> 8) & 63u;
        if (pr < XE_TEMPS)
        {
            float2 gp = input.pos.xy / float(XE_RES_SCALE); // guest pixel position
            r[pr] = float4(front_face ? gp.x : -gp.x, gp.y, 0.0, 0.0);
        }
    }
    bool xe_p = false, xe_kill = false;
    int xe_a0 = 0;
    float xe_ps = 0.0;
    uint xe_vf_addr = 0u;
    int xe_ld = 0, xe_cd = 0;
    uint xe_lc[4] = { 0u, 0u, 0u, 0u }, xe_li[4] = { 0u, 0u, 0u, 0u }, xe_cr[4] = { 0u, 0u, 0u, 0u };
    int xe_lstart[4] = { 0, 0, 0, 0 }, xe_lstep[4] = { 0, 0, 0, 0 };
    float4 xe_o_c[4];
    [unroll] for (uint ci = 0u; ci < 4u; ci++) xe_o_c[ci] = float4(0.0, 0.0, 0.0, 0.0);
    float4 xe_o_depth = float4(input.depth.x, 0.0, 0.0, 0.0);
    float4 xe_o_pos, xe_o_psize;
    float4 xe_o_t[16];

#define xe_aL xe_loop_address(xe_ld, xe_li, xe_lstart, xe_lstep)
XE_BODY
#undef xe_aL

    if (xe_kill)
        discard;

    // ---- Output merger (emulated, writes the EDRAM) ----
    uint4 om = xe_sys[4];
    uint rt_flags = om.z, depth_flags = om.w;
    uint4 alpha = xe_sys[10];
    if (alpha.z != 0u && !xe_comparef(alpha.x, xe_o_c[0].w, asfloat(alpha.y)))
        discard;

    uint pitch = xe_sys[3].w;
    uint msaa_x = om.x & 1u, msaa_y = (om.x >> 1) & 1u;
    // Guest pixel and the host sub-pixel within it (render resolution scale).
    uint hx = uint(input.pos.x), hy = uint(input.pos.y);
    uint px = hx / XE_RES_SCALE, py = hy / XE_RES_SCALE;
    uint subx = hx % XE_RES_SCALE, suby = hy % XE_RES_SCALE;
    float z = xe_o_depth.x;
    uint4 dinfo = xe_sys[9];
    uint dctl = dinfo.y;

    for (uint sy = 0u; sy <= msaa_y; sy++)
    {
        for (uint sx = 0u; sx <= msaa_x; sx++)
        {
            uint xs = ((px << msaa_x) + sx) * XE_RES_SCALE + subx, ys = ((py << msaa_y) + sy) * XE_RES_SCALE + suby;
            if ((depth_flags & 1u) != 0u)
            {
                uint daddr = xe_edram_index(dinfo.x, pitch, xs, ys, false) * 4u;
                uint dv = xe_edram.Load(daddr);
                uint stored = dv >> 8, stencil = dv & 0xFFu;
                uint depth = (depth_flags & 2u) != 0u ? xe_float24(z) : uint(saturate(z) * 16777215.0 + 0.5);
                bool z_enable = (dctl & 2u) != 0u, z_write = (dctl & 4u) != 0u;
                bool ds_pass = true;
                uint new_stencil = stencil;
                if ((dctl & 1u) != 0u)
                {
                    bool back = !front_face && (dctl & 0x80u) != 0u;
                    uint srm = back ? dinfo.w : dinfo.z;
                    uint sref = srm & 0xFFu, smask = (srm >> 8) & 0xFFu, swmask = (srm >> 16) & 0xFFu;
                    uint sfunc = back ? (dctl >> 20) & 7u : (dctl >> 8) & 7u;
                    uint op_fail = back ? (dctl >> 23) & 7u : (dctl >> 11) & 7u;
                    uint op_zpass = back ? (dctl >> 26) & 7u : (dctl >> 14) & 7u;
                    uint op_zfail = back ? (dctl >> 29) & 7u : (dctl >> 17) & 7u;
                    bool s_pass = xe_compare(sfunc, sref & smask, stencil & smask);
                    bool z_pass = !z_enable || xe_compare((dctl >> 4) & 7u, depth, stored);
                    uint res = xe_stencil_op(!s_pass ? op_fail : (!z_pass ? op_zfail : op_zpass), stencil, sref);
                    new_stencil = (stencil & ~swmask) | (res & swmask);
                    ds_pass = s_pass && z_pass;
                }
                else if (z_enable)
                    ds_pass = xe_compare((dctl >> 4) & 7u, depth, stored);
                uint new_depth = (ds_pass && z_enable && z_write) ? depth : stored;
                uint nv = (new_depth << 8) | (new_stencil & 0xFFu);
                if (nv != dv)
                    xe_edram.Store(daddr, nv);
                if (!ds_pass)
                    continue;
            }
            if ((rt_flags & 0x100u) == 0u)
                continue;
            [unroll] for (uint i = 0u; i < 4u; i++)
            {
                if ((rt_flags & (1u << i)) == 0u)
                    continue;
                uint fmt = xe_sys[6][i];
                bool is64 = xe_rt_is64(fmt);
                uint addr = xe_edram_index(xe_sys[5][i], pitch, xs, ys, is64) * 4u;
                uint2 old = is64 ? xe_edram.Load2(addr) : uint2(xe_edram.Load(addr), 0u);
                float4 dst = xe_decode_rt(fmt, old);
                bool gamma = fmt == 1u;
                if (gamma) dst.xyz = pow(saturate(dst.xyz), 2.2);
                float4 src = xe_o_c[i];
                float4 res = src;
                if ((rt_flags & (16u << i)) != 0u)
                {
                    uint b = xe_sys[7][i];
                    float4 k = asfloat(xe_sys[8]);
                    [unroll] for (uint c = 0u; c < 3u; c++)
                        res[c] = xe_blend_op((b >> 5) & 7u, src[c] * xe_blend_factor(b & 31u, c, src, dst, k),
                                             dst[c] * xe_blend_factor((b >> 8) & 31u, c, src, dst, k));
                    res.w = xe_blend_op((b >> 21) & 7u, src.w * xe_blend_factor((b >> 16) & 31u, 3u, src, dst, k),
                                        dst.w * xe_blend_factor((b >> 24) & 31u, 3u, src, dst, k));
                }
                uint mask = (om.y >> (4u * i)) & 15u;
                [unroll] for (uint c2 = 0u; c2 < 4u; c2++)
                    if ((mask & (1u << c2)) == 0u) res[c2] = dst[c2];
                if (gamma) res.xyz = pow(saturate(res.xyz), 1.0 / 2.2);
                uint2 nv2 = xe_encode_rt(fmt, res);
                if (is64) xe_edram.Store2(addr, nv2);
                else xe_edram.Store(addr, nv2.x);
            }
        }
    }
}
)HLSL";

    // Loop address helper (aL), shared by VS and PS.
    inline const char* kLoopAddress = R"HLSL(
int xe_loop_address(int ld, uint li[4], int lstart[4], int lstep[4])
{
    if (ld <= 0) return 0;
    int v = int(li[ld - 1]) * lstep[ld - 1] + lstart[ld - 1];
    return clamp(v, -256, 256);
}
)HLSL";

    // ---- Compute: resolve (EDRAM -> guest memory) and clear ----
    // Root constants (b0), 32 dwords:
    //  0 x0, 1 y0, 2 width, 3 height (pixels)
    //  4 src base (tiles), 5 pitch tiles, 6 src format (RT or 0xFF00|depth), 7 msaa (bit0 x, bit1 y, bits 2.. samples log2)
    //  8 dest base (bytes, physical), 9 dest pitch (pixels, aligned 32), 10 dest height aligned, 11 dest format
    //  12 dest endian, 13 flags (bit0 raw, bit1 swap RB, bit2 array, bit3 64bpp src, bit4 depth src), 14 slice, 15 sample select
    //  16 exp bias scale (float), 17 dest bytes per pixel log2
    //  18 clear lo, 19 clear hi
    inline const char* kResolve = R"HLSL(
cbuffer XeResolve : register(b0) { uint4 rc[8]; };
RWByteAddressBuffer xe_edram_rw : register(u0);
RWByteAddressBuffer xe_shared_rw : register(u1);
// Render-resolution-scaled copies of resolved surfaces (same layout as guest
// memory, S times wider and taller). Declared after the staging buffer (u2).
RWByteAddressBuffer xe_scaled_rw : register(u3);

uint xe_swap32c(uint v, uint endian)
{
    if (endian == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == 2u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (endian == 3u) return (v << 16) | (v >> 16);
    return v;
}

uint xe_edram_index_c(uint base, uint pitch_tiles, uint xs, uint ys, bool is64)
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

// Writes a pixel of 1..16 bytes. Sub-dword pixels are merged atomically.
void xe_store_dest(uint addr, uint4 v, uint bpp_log2, uint endian)
{
    if (bpp_log2 == 0u)
    {
        uint shift = (addr & 3u) * 8u;
        xe_shared_rw.InterlockedAnd(addr & ~3u, ~(0xFFu << shift));
        xe_shared_rw.InterlockedOr(addr & ~3u, (v.x & 0xFFu) << shift);
        return;
    }
    if (bpp_log2 == 1u)
    {
        uint h = v.x & 0xFFFFu;
        if (endian == 1u || endian == 2u) h = ((h & 0xFFu) << 8) | (h >> 8);
        uint shift = (addr & 2u) * 8u;
        xe_shared_rw.InterlockedAnd(addr & ~3u, ~(0xFFFFu << shift));
        xe_shared_rw.InterlockedOr(addr & ~3u, h << shift);
        return;
    }
    uint4 s = uint4(xe_swap32c(v.x, endian), xe_swap32c(v.y, endian), xe_swap32c(v.z, endian), xe_swap32c(v.w, endian));
    if (bpp_log2 == 2u) xe_shared_rw.Store(addr, s.x);
    else if (bpp_log2 == 3u) xe_shared_rw.Store2(addr, s.xy);
    else xe_shared_rw.Store4(addr, s);
}

// xe_store_dest for the scaled-surface buffer.
void xe_store_scaled(uint addr, uint4 v, uint bpp_log2, uint endian)
{
    if (bpp_log2 == 0u)
    {
        uint shift = (addr & 3u) * 8u;
        xe_scaled_rw.InterlockedAnd(addr & ~3u, ~(0xFFu << shift));
        xe_scaled_rw.InterlockedOr(addr & ~3u, (v.x & 0xFFu) << shift);
        return;
    }
    if (bpp_log2 == 1u)
    {
        uint h = v.x & 0xFFFFu;
        if (endian == 1u || endian == 2u) h = ((h & 0xFFu) << 8) | (h >> 8);
        uint shift = (addr & 2u) * 8u;
        xe_scaled_rw.InterlockedAnd(addr & ~3u, ~(0xFFFFu << shift));
        xe_scaled_rw.InterlockedOr(addr & ~3u, h << shift);
        return;
    }
    uint4 s = uint4(xe_swap32c(v.x, endian), xe_swap32c(v.y, endian), xe_swap32c(v.z, endian), xe_swap32c(v.w, endian));
    if (bpp_log2 == 2u) xe_scaled_rw.Store(addr, s.x);
    else if (bpp_log2 == 3u) xe_scaled_rw.Store2(addr, s.xy);
    else xe_scaled_rw.Store4(addr, s);
}

uint4 xe_encode_dest(uint fmt, float4 c)
{
    switch (fmt)
    {
    case 6u: case 14u: case 50u:   // 8_8_8_8, 8_8_8_8_A, AS_16_16_16_16
        return uint4(xe_to_unorm(c.x, 8u) | (xe_to_unorm(c.y, 8u) << 8) | (xe_to_unorm(c.z, 8u) << 16) | (xe_to_unorm(c.w, 8u) << 24), 0u, 0u, 0u);
    case 7u: case 54u:             // 2_10_10_10
        return uint4(xe_to_unorm(c.x, 10u) | (xe_to_unorm(c.y, 10u) << 10) | (xe_to_unorm(c.z, 10u) << 20) | (xe_to_unorm(c.w, 2u) << 30), 0u, 0u, 0u);
    case 4u:                       // 5_6_5
        return uint4(xe_to_unorm(c.z, 5u) | (xe_to_unorm(c.y, 6u) << 5) | (xe_to_unorm(c.x, 5u) << 11), 0u, 0u, 0u);
    case 2u: case 8u: case 9u:     // 8
        return uint4(xe_to_unorm(c.x, 8u), 0u, 0u, 0u);
    case 10u:                      // 8_8
        return uint4(xe_to_unorm(c.x, 8u) | (xe_to_unorm(c.y, 8u) << 8), 0u, 0u, 0u);
    case 24u:                      // 16
        return uint4(xe_to_unorm(c.x, 16u), 0u, 0u, 0u);
    case 25u:                      // 16_16
        return uint4(xe_to_unorm(c.x, 16u) | (xe_to_unorm(c.y, 16u) << 16), 0u, 0u, 0u);
    case 26u:                      // 16_16_16_16
        return uint4(xe_to_unorm(c.x, 16u) | (xe_to_unorm(c.y, 16u) << 16), xe_to_unorm(c.z, 16u) | (xe_to_unorm(c.w, 16u) << 16), 0u, 0u);
    case 30u:                      // 16_FLOAT
        return uint4(f32tof16(c.x), 0u, 0u, 0u);
    case 31u:                      // 16_16_FLOAT
        return uint4(f32tof16(c.x) | (f32tof16(c.y) << 16), 0u, 0u, 0u);
    case 32u:                      // 16_16_16_16_FLOAT
        return uint4(f32tof16(c.x) | (f32tof16(c.y) << 16), f32tof16(c.z) | (f32tof16(c.w) << 16), 0u, 0u);
    case 36u:                      // 32_FLOAT
        return uint4(asuint(c.x), 0u, 0u, 0u);
    case 37u:                      // 32_32_FLOAT
        return uint4(asuint(c.x), asuint(c.y), 0u, 0u);
    case 38u:                      // 32_32_32_32_FLOAT
        return asuint(c);
    }
    return uint4(xe_to_unorm(c.x, 8u) | (xe_to_unorm(c.y, 8u) << 8) | (xe_to_unorm(c.z, 8u) << 16) | (xe_to_unorm(c.w, 8u) << 24), 0u, 0u, 0u);
}

// Reads guest pixel (x, y) of the resolve source - host sub-sample (subx, suby)
// of each guest sample - converted to the destination format's bits.
uint4 xe_resolve_pixel(uint x, uint y, uint subx, uint suby)
{
    uint flags = rc[3].y;
    bool src64 = (flags & 8u) != 0u, is_depth = (flags & 16u) != 0u;
    uint msaa_x = rc[1].w & 1u, msaa_y = (rc[1].w >> 1) & 1u, samples = 1u << ((rc[1].w >> 2) & 3u);
    uint sel = rc[3].w;
    uint bpp_log2 = rc[4].y;
    uint4 outv = uint4(0u, 0u, 0u, 0u);
    if ((flags & 1u) != 0u)
    {
        uint s = sel < 4u ? min(sel, samples - 1u) : 0u;
        uint sxs = (x << msaa_x) + (msaa_x != 0u ? (s >> 1) : 0u);
        uint sys = (y << msaa_y) + (msaa_y != 0u ? (s & 1u) : 0u);
        uint a = xe_edram_index_c(rc[1].x, rc[1].y, sxs * XE_RES_SCALE + subx, sys * XE_RES_SCALE + suby, src64) * 4u;
        outv.x = xe_edram_rw.Load(a);
        if (src64) outv.y = xe_edram_rw.Load(a + 4u);
        if (!is_depth && (flags & 2u) != 0u && bpp_log2 == 2u && rc[1].z <= 1u)
            outv.x = (outv.x & 0xFF00FF00u) | ((outv.x & 0xFFu) << 16) | ((outv.x >> 16) & 0xFFu);
    }
    else
    {
        float4 c = float4(0.0, 0.0, 0.0, 0.0);
        uint n = 0u;
        for (uint s2 = 0u; s2 < samples; s2++)
        {
            if (sel < 4u && s2 != min(sel, samples - 1u)) continue;
            uint sxs2 = (x << msaa_x) + (msaa_x != 0u ? (s2 >> 1) : 0u);
            uint sys2 = (y << msaa_y) + (msaa_y != 0u ? (s2 & 1u) : 0u);
            uint a2 = xe_edram_index_c(rc[1].x, rc[1].y, sxs2 * XE_RES_SCALE + subx, sys2 * XE_RES_SCALE + suby, src64) * 4u;
            uint2 v = uint2(xe_edram_rw.Load(a2), src64 ? xe_edram_rw.Load(a2 + 4u) : 0u);
            c += xe_decode_rt(rc[1].z, v);
            n++;
        }
        c = c / float(max(n, 1u)) * asfloat(rc[4].x);
        if ((flags & 2u) != 0u) c = c.zyxw;
        outv = xe_encode_dest(rc[2].w, c);
    }
    return outv;
}

[numthreads(8, 8, 1)]
void resolve_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= rc[0].z || id.y >= rc[0].w) return;
    uint x = rc[0].x + id.x, y = rc[0].y + id.y;
    uint flags = rc[3].y;
    uint bpp_log2 = rc[4].y;
    // Unscaled copy: the top-left host sample of each guest sample.
    uint4 outv = xe_resolve_pixel(x, y, 0u, 0u);
    uint offset = (flags & 4u) != 0u ? xe_tiled3d(x, y, rc[3].z, rc[2].y, rc[2].z, bpp_log2)
                                     : xe_tiled2d(x, y, rc[2].y, bpp_log2);
    xe_store_dest((rc[2].x + offset) & 0x1FFFFFFFu, outv, bpp_log2, rc[3].x);
}

// Scaled copy into a surface of the scaled buffer: one thread per host pixel.
// rc[5]: x = surface offset in the buffer (bytes), y = surface pitch (host
// pixels, S x guest), z/w = position of this resolve's (0,0) in the surface
// (guest pixels).
[numthreads(8, 8, 1)]
void resolve_scaled_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= rc[0].z * XE_RES_SCALE || id.y >= rc[0].w * XE_RES_SCALE) return;
    uint hx = rc[0].x * XE_RES_SCALE + id.x, hy = rc[0].y * XE_RES_SCALE + id.y;
    uint x = hx / XE_RES_SCALE, y = hy / XE_RES_SCALE;
    uint4 outv = xe_resolve_pixel(x, y, hx % XE_RES_SCALE, hy % XE_RES_SCALE);
    uint bpp_log2 = rc[4].y;
    uint offset = xe_tiled2d(rc[5].z * XE_RES_SCALE + hx, rc[5].w * XE_RES_SCALE + hy, rc[5].y, bpp_log2);
    xe_store_scaled(rc[5].x + offset, outv, bpp_log2, rc[3].x);
}

[numthreads(8, 8, 1)]
void clear_main(uint3 id : SV_DispatchThreadID)
{
    // Clears samples: rc[0] = x0, y0, width, height in samples; rc[1].x base, .y pitch, .z is64
    if (id.x >= rc[0].z || id.y >= rc[0].w) return;
    uint xs = rc[0].x + id.x, ys = rc[0].y + id.y;
    bool is64 = rc[1].z != 0u;
    uint a = xe_edram_index_c(rc[1].x, rc[1].y, xs, ys, is64) * 4u;
    if (is64) xe_edram_rw.Store2(a, uint2(rc[4].z, rc[4].w));
    else xe_edram_rw.Store(a, rc[4].w);
}

// Untile/copy a texture level from guest memory into a linear staging buffer.
// rc[0]: base (bytes), pitch (blocks, aligned 32 when tiled), width blocks, height blocks (this band)
// rc[1]: bpb log2, tiled, endian, dest row pitch (bytes)
// rc[2]: x = first row of this band, y = 1: read the scaled buffer (base is
//        the surface offset), z/w = origin of the image in the source (blocks)
RWByteAddressBuffer xe_staging : register(u2);
uint xe_untile_load(uint a, bool scaled) { return scaled ? xe_scaled_rw.Load(a) : xe_shared_rw.Load(a & 0x1FFFFFFFu); }
[numthreads(8, 8, 1)]
void untile_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= rc[0].z || id.y >= rc[0].w) return;
    uint bpb_log2 = rc[1].x;
    uint bpb = 1u << bpb_log2;
    bool scaled = rc[2].y != 0u;
    uint sx = id.x + rc[2].z, sy = id.y + rc[2].x + rc[2].w; // + origin (packed mips, scaled sub-surfaces)
    uint src = rc[1].y != 0u ? rc[0].x + xe_tiled2d(sx, sy, rc[0].y, bpb_log2)
                             : rc[0].x + (sy * rc[0].y + sx) * bpb;
    uint dst = id.y * rc[1].w + id.x * bpb;
    uint endian = rc[1].z;
    if (scaled)
    {
        // Scaled surfaces only hold uncompressed formats of 1-16 bytes.
        if (bpb_log2 >= 2u)
        {
            for (uint i = 0u; i < bpb; i += 4u)
                xe_staging.Store(dst + i, xe_swap32c(xe_scaled_rw.Load(src + i), endian));
        }
        else
        {
            uint word = xe_scaled_rw.Load(src & ~3u);
            uint v = bpb_log2 == 1u ? (word >> ((src & 2u) * 8u)) & 0xFFFFu : (word >> ((src & 3u) * 8u)) & 0xFFu;
            if (bpb_log2 == 1u && (endian == 1u || endian == 2u)) v = ((v & 0xFFu) << 8) | (v >> 8);
            uint mask = bpb_log2 == 1u ? 0xFFFFu : 0xFFu, shift = (dst & 3u) * 8u;
            xe_staging.InterlockedAnd(dst & ~3u, ~(mask << shift));
            xe_staging.InterlockedOr(dst & ~3u, v << shift);
        }
        return;
    }
    if (bpb_log2 >= 2u)
    {
        for (uint i = 0u; i < bpb; i += 4u)
            xe_staging.Store(dst + i, xe_swap32c(xe_shared_rw.Load(((src + i) & 0x1FFFFFFFu)), endian));
    }
    else
    {
        // 1 or 2 bytes: gather from the containing dword.
        uint a = (src & 0x1FFFFFFFu);
        uint word = xe_shared_rw.Load(a & ~3u);
        uint v;
        if (bpb_log2 == 1u)
        {
            v = (word >> ((a & 2u) * 8u)) & 0xFFFFu;
            if (endian == 1u || endian == 2u) v = ((v & 0xFFu) << 8) | (v >> 8);
            uint shift = (dst & 2u) * 8u;
            xe_staging.InterlockedAnd(dst & ~3u, ~(0xFFFFu << shift));
            xe_staging.InterlockedOr(dst & ~3u, v << shift);
        }
        else
        {
            if (endian == 2u) a ^= 3u;
            else if (endian == 1u) a ^= 1u;
            v = (xe_shared_rw.Load(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu;
            uint shift8 = (dst & 3u) * 8u;
            xe_staging.InterlockedAnd(dst & ~3u, ~(0xFFu << shift8));
            xe_staging.InterlockedOr(dst & ~3u, v << shift8);
        }
    }
}
)HLSL";

    // ---- Presentation ----
    inline const char* kPresent = R"HLSL(
Texture2D<float4> xe_front : register(t0);
SamplerState xe_linear : register(s0);
// swizzle (3 bits per output channel), anti-aliasing on, 1 / source size
cbuffer XePresent : register(b0) { uint xe_present_swizzle; uint xe_present_aa; float2 xe_texel; };
struct PresentOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
PresentOut vs_main(uint id : SV_VertexID)
{
    PresentOut o;
    float2 uv = float2((id << 1) & 2u, id & 2u);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
// The front buffer may be stored with channels swapped; the swizzle fixes it.
float3 xe_front_rgb(float2 uv)
{
    float4 t = xe_front.SampleLevel(xe_linear, uv, 0.0);
    float d[4] = { t.x, t.y, t.z, t.w };
    float3 o;
    [unroll] for (uint k = 0u; k < 3u; k++)
    {
        uint s = (xe_present_swizzle >> (k * 3u)) & 7u;
        o[k] = s < 4u ? d[s] : (s == 5u ? 1.0 : 0.0);
    }
    return o;
}
float xe_luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 ps_main(PresentOut i) : SV_Target
{
    float3 c = xe_front_rgb(i.uv);
    if (xe_present_aa == 0u)
        return float4(c, 1.0);
    // Edge smoothing on the final image: where the corners' brightness
    // differs, blur along the edge direction (perpendicular to the gradient),
    // and keep the wider blur only if it stays within the local range.
    float2 t = xe_texel;
    float lNW = xe_luma(xe_front_rgb(i.uv + float2(-1.0, -1.0) * t));
    float lNE = xe_luma(xe_front_rgb(i.uv + float2(1.0, -1.0) * t));
    float lSW = xe_luma(xe_front_rgb(i.uv + float2(-1.0, 1.0) * t));
    float lSE = xe_luma(xe_front_rgb(i.uv + float2(1.0, 1.0) * t));
    float lM = xe_luma(c);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125))
        return float4(c, 1.0);
    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, -8.0, 8.0) * t;
    float3 a = 0.5 * (xe_front_rgb(i.uv + dir * (1.0 / 3.0 - 0.5)) + xe_front_rgb(i.uv + dir * (2.0 / 3.0 - 0.5)));
    float3 b = a * 0.5 + 0.25 * (xe_front_rgb(i.uv - dir * 0.5) + xe_front_rgb(i.uv + dir * 0.5));
    float lB = xe_luma(b);
    return float4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)HLSL";
}
