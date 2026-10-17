// Prism shading dialect: the common subset of C++, GLSL and HLSL used by the
// shared integrator and the GPU backends. Code written against it uses
//   - splat3(v) instead of single-scalar vector constructors,
//   - float2/3/4, int, uint, bool and structs with .x/.y/.z/.w member access
//     (no swizzles, no matrices, no arrays of vectors in expressions),
//   - IN(T)/OUT(T)/INOUT(T) parameter qualifiers (C++ references: never pass
//     the same variable twice),
//   - the prism_* helpers below for functions spelled differently,
//   - f-suffixed float literals and u-suffixed unsigned literals.
// Exactly one of PRISM_SL_CPP (implied by __cplusplus), PRISM_GLSL or
// PRISM_HLSL selects the target.

#ifndef PRISM_SL_H
#define PRISM_SL_H

#if defined(__cplusplus)

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace prism::sl {

using uint = uint32_t;

struct float2 {
    float x = 0, y = 0;
    constexpr float2() = default;
    constexpr explicit float2(float v) : x(v), y(v) {}
    constexpr float2(float x_, float y_) : x(x_), y(y_) {}
};
struct float3 {
    float x = 0, y = 0, z = 0;
    constexpr float3() = default;
    constexpr explicit float3(float v) : x(v), y(v), z(v) {}
    constexpr float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
struct float4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr float4() = default;
    constexpr explicit float4(float v) : x(v), y(v), z(v), w(v) {}
    constexpr float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr float4(float3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

#define PRISM_SL_BINARY(T, op)                                                         \
    constexpr T operator op(T a, T b) { return PRISM_SL_APPLY2(T, a, b, op); }          \
    constexpr T operator op(T a, float b) { return operator op(a, T(b)); }               \
    constexpr T operator op(float a, T b) { return operator op(T(a), b); }               \
    constexpr T& operator op##=(T& a, T b) { return a = a op b; }                        \
    constexpr T& operator op##=(T& a, float b) { return a = a op T(b); }
#define PRISM_SL_APPLY2(T, a, b, op) PRISM_SL_APPLY2_##T(a, b, op)
#define PRISM_SL_APPLY2_float2(a, b, op) float2(a.x op b.x, a.y op b.y)
#define PRISM_SL_APPLY2_float3(a, b, op) float3(a.x op b.x, a.y op b.y, a.z op b.z)
#define PRISM_SL_APPLY2_float4(a, b, op) float4(a.x op b.x, a.y op b.y, a.z op b.z, a.w op b.w)
#define PRISM_SL_VECTOR(T)                                                             \
    PRISM_SL_BINARY(T, +) PRISM_SL_BINARY(T, -) PRISM_SL_BINARY(T, *) PRISM_SL_BINARY(T, /)
PRISM_SL_VECTOR(float2)
PRISM_SL_VECTOR(float3)
PRISM_SL_VECTOR(float4)
constexpr float2 operator-(float2 a) { return float2(-a.x, -a.y); }
constexpr float3 operator-(float3 a) { return float3(-a.x, -a.y, -a.z); }
constexpr float4 operator-(float4 a) { return float4(-a.x, -a.y, -a.z, -a.w); }

inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float3 cross(float3 a, float3 b)
{
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { return a / length(a); }
using std::abs; using std::sqrt; using std::floor; using std::sin; using std::cos; using std::tan;
using std::acos; using std::pow; using std::exp2; using std::log2;
inline float min(float a, float b) { return std::fmin(a, b); }
inline float max(float a, float b) { return std::fmax(a, b); }
inline int min(int a, int b) { return std::min(a, b); }
inline int max(int a, int b) { return std::max(a, b); }
inline uint min(uint a, uint b) { return std::min(a, b); }
inline uint max(uint a, uint b) { return std::max(a, b); }
inline float clamp(float v, float a, float b) { return std::clamp(v, a, b); }
inline int clamp(int v, int a, int b) { return std::clamp(v, a, b); }
inline float3 min(float3 a, float3 b) { return float3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }
inline float3 max(float3 a, float3 b) { return float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
inline float prism_atan2(float y, float x) { return std::atan2(y, x); }
inline bool prism_isfinite(float v) { return std::isfinite(v); }
inline float prism_infinity() { return std::numeric_limits<float>::infinity(); }
inline float prism_asfloat(uint v) { return std::bit_cast<float>(v); }
inline uint prism_asuint(float v) { return std::bit_cast<uint>(v); }
inline float prism_lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float prism_round(float v) { return std::round(v); }
inline float prism_hypot(float x, float y) { return std::hypot(x, y); }
inline float3 splat3(float v) { return float3(v); }

} // namespace prism::sl

#define IN(T) T const&
#define OUT(T) T&
#define INOUT(T) T&
#define PRISM_INLINE inline

#elif defined(PRISM_GLSL)

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define IN(T) in T
#define OUT(T) out T
#define INOUT(T) inout T
#define PRISM_INLINE
#define prism_atan2(y, x) atan(y, x)
#define prism_lerp(a, b, t) mix(a, b, t)
#define prism_asfloat(v) uintBitsToFloat(v)
#define prism_asuint(v) floatBitsToUint(v)
#define prism_round(v) roundEven(v)
bool prism_isfinite(float v) { return !isinf(v) && !isnan(v); }
float prism_infinity() { return uintBitsToFloat(0x7f800000u); }
float prism_hypot(float x, float y) { return sqrt(x * x + y * y); }
float3 splat3(float v) { return vec3(v); }

#else // HLSL

#define IN(T) in T
#define OUT(T) out T
#define INOUT(T) inout T
#define PRISM_INLINE
#define prism_atan2(y, x) atan2(y, x)
#define prism_lerp(a, b, t) lerp(a, b, t)
#define prism_asfloat(v) asfloat(v)
#define prism_asuint(v) asuint(v)
#define prism_round(v) round(v)
bool prism_isfinite(float v) { return isfinite(v); }
float prism_infinity() { return asfloat(0x7f800000u); }
float prism_hypot(float x, float y) { return sqrt(x * x + y * y); }
float3 splat3(float v) { return float3(v, v, v); }

#endif

#define PRISM_PI 3.14159265358979323846f

#endif // PRISM_SL_H
