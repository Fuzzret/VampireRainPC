// C++ implementation of the shader dialect produced by shader_translator, for
// testing translated shaders on the CPU against Xenia's interpreter.
// The HLSL side of the same names lives in src/d3d12/shaders (prelude).
#pragma once
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#define XE_LOOP
#define XE_FLT_MAX FLT_MAX

namespace xs
{
typedef uint32_t uint;

struct float4
{
    float x, y, z, w;
    float4() : x(0), y(0), z(0), w(0) {}
    float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
inline float4 operator+(float4 a, float4 b) { return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
inline float4 operator-(float4 a, float4 b) { return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
inline float4 operator*(float4 a, float4 b) { return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w }; }
inline float4 operator*(float4 a, float s) { return { a.x * s, a.y * s, a.z * s, a.w * s }; }
inline float4 operator-(float4 a) { return { -a.x, -a.y, -a.z, -a.w }; }

inline float asfloat(uint v) { float f; memcpy(&f, &v, 4); return f; }
inline float xe_flush(float v)
{
    uint b; memcpy(&b, &v, 4);
    b &= (b & 0x7F800000u) ? ~0u : (1u << 31);
    memcpy(&v, &b, 4);
    return v;
}
using std::sqrt; using std::sin; using std::cos; using std::exp2; using std::log2;
inline float floor(float v) { return std::floor(v); }
inline float trunc(float v) { return std::trunc(v); }
inline float abs(float v) { return std::fabs(v); }
inline float4 abs(float4 v) { return { std::fabs(v.x), std::fabs(v.y), std::fabs(v.z), std::fabs(v.w) }; }
inline float4 floor(float4 v) { return { std::floor(v.x), std::floor(v.y), std::floor(v.z), std::floor(v.w) }; }
inline float4 trunc(float4 v) { return { std::trunc(v.x), std::trunc(v.y), std::trunc(v.z), std::trunc(v.w) }; }
inline float saturate(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; } // NaN -> 0
inline float4 saturate(float4 v) { return { saturate(v.x), saturate(v.y), saturate(v.z), saturate(v.w) }; }
inline float clamp(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }
inline float max(float a, float b) { return std::fmax(a, b); }
inline bool xe_isfinite(float v) { return std::isfinite(v); }

inline float4 xe_swz(float4 v, int a, int b, int c, int d) { return { xe_flush(v[a]), xe_flush(v[b]), xe_flush(v[c]), xe_flush(v[d]) }; }
inline float4 xe_rep(float v) { return { v, v, v, v }; }
inline float xe_mul1(float a, float b) { return (a != 0.0f && b != 0.0f) ? a * b : 0.0f; }
inline float4 xe_mul(float4 a, float4 b) { return { xe_mul1(a.x, b.x), xe_mul1(a.y, b.y), xe_mul1(a.z, b.z), xe_mul1(a.w, b.w) }; }
inline float xe_dp4(float4 a, float4 b) { float r = 0; for (int i = 0; i < 4; i++) r += xe_mul1(a[i], b[i]); return r; }
inline float xe_dp3(float4 a, float4 b) { float r = 0; for (int i = 0; i < 3; i++) r += xe_mul1(a[i], b[i]); return r; }
inline float xe_dp2(float4 a, float4 b) { float r = 0; for (int i = 0; i < 2; i++) r += xe_mul1(a[i], b[i]); return r; }
#define XE_CW(name, expr) inline float4 name(float4 a, float4 b) { float4 r; for (int i = 0; i < 4; i++) { float x = a[i], y = b[i]; r[i] = (expr); } return r; }
XE_CW(xe_max, x >= y ? x : y)
XE_CW(xe_min, x < y ? x : y)
XE_CW(xe_seq, float(x == y))
XE_CW(xe_sgt, float(x > y))
XE_CW(xe_sge, float(x >= y))
XE_CW(xe_sne, float(x != y))
#undef XE_CW
inline float4 xe_cndeq(float4 a, float4 b, float4 c) { float4 r; for (int i = 0; i < 4; i++) r[i] = a[i] == 0.0f ? b[i] : c[i]; return r; }
inline float4 xe_cndge(float4 a, float4 b, float4 c) { float4 r; for (int i = 0; i < 4; i++) r[i] = a[i] >= 0.0f ? b[i] : c[i]; return r; }
inline float4 xe_cndgt(float4 a, float4 b, float4 c) { float4 r; for (int i = 0; i < 4; i++) r[i] = a[i] > 0.0f ? b[i] : c[i]; return r; }
inline float xe_max4(float4 v)
{
    if (v.x >= v.y && v.x >= v.z && v.x >= v.w) return v.x;
    if (v.y >= v.z && v.y >= v.w) return v.y;
    if (v.z >= v.w) return v.z;
    return v.w;
}
inline float4 xe_cube(float4 v)
{
    float x = v.z, y = v.w, z = v.x;
    float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    float4 r;
    if (az >= ax && az >= ay) { bool n = z < 0; r = { -y, n ? -x : x, z, n ? 5.0f : 4.0f }; }
    else if (ay >= ax) { bool n = y < 0; r = { n ? -z : z, x, y, n ? 3.0f : 2.0f }; }
    else { bool n = x < 0; r = { -y, n ? z : -z, x, n ? 1.0f : 0.0f }; }
    r.z *= 2.0f;
    return r;
}
inline float xe_logc(float v) { float r = std::log2(v); return r == -INFINITY ? -FLT_MAX : r; }
inline float xe_clampinf(float v) { return v == -INFINITY ? -FLT_MAX : v == INFINITY ? FLT_MAX : v; }
inline float xe_zeroinf(float v) { return v == -INFINITY ? -0.0f : v == INFINITY ? 0.0f : v; }
float xe_half(uint h);

// Environment supplied by the test harness.
struct XeEnv
{
    float4 r[64];
    bool p = false, kill = false;
    int a0 = 0;
    float ps = 0;
    uint vf_addr = 0;
    int ld = 0, cd = 0;
    uint lc[4] = {}, li[4] = {}, cr[4] = {};
    int lstart[4] = {}, lstep[4] = {};
    float4 o_pos, o_t[16], o_psize, o_c[4], o_depth;
};
extern thread_local XeEnv* xe_env;
float4 xe_C(int index);
bool xe_B(uint index);
uint xe_L(uint index);
uint xe_vfetch_base(uint fc);
uint xe_vfetch_dword(uint fc, uint address);
float4 xe_tfetch(uint fc, uint dimension, float4 coords, float4 offset, bool unnormalized);
inline int xe_aL_fn()
{
    XeEnv& e = *xe_env;
    if (e.ld <= 0) return 0;
    int v = int(e.li[e.ld - 1]) * e.lstep[e.ld - 1] + e.lstart[e.ld - 1];
    return v < -256 ? -256 : v > 256 ? 256 : v;
}

}
