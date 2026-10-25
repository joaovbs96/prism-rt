#pragma once

// The HLSL vector types and intrinsics used by code shared between C++ and
// HLSL (shaders/integrator), with HLSL semantics. C++ only.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

using uint = uint32_t;

struct float2 {
    float x = 0, y = 0;
    float2() = default;
    float2(float v) : x(v), y(v) {}
    float2(float x_, float y_) : x(x_), y(y_) {}
};

struct float3 {
    float x = 0, y = 0, z = 0;
    float3() = default;
    float3(float v) : x(v), y(v), z(v) {}
    float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct float4 {
    float x = 0, y = 0, z = 0, w = 0;
    float4() = default;
    float4(float v) : x(v), y(v), z(v), w(v) {}
    float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    float4(float3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

struct uint4 {
    uint x = 0, y = 0, z = 0, w = 0;
};

inline float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
inline float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
inline float2 operator*(float2 a, float2 b) { return {a.x * b.x, a.y * b.y}; }

inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float3 operator/(float3 a, float3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline float3 operator-(float3 a) { return {-a.x, -a.y, -a.z}; }
inline float3& operator+=(float3& a, float3 b) { return a = a + b; }
inline float3& operator-=(float3& a, float3 b) { return a = a - b; }
inline float3& operator*=(float3& a, float3 b) { return a = a * b; }
inline float3& operator/=(float3& a, float3 b) { return a = a / b; }

inline float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline float4 operator-(float4 a, float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline float4 operator*(float4 a, float4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
inline float4 operator/(float4 a, float4 b) { return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}; }

inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(float3 a, float3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(float3 a) { return std::sqrt(dot(a, a)); }
inline float3 normalize(float3 a) { return a / float3(length(a)); }

using std::abs;
// isfinite: the global one from <cmath> already has HLSL semantics.
using std::acos;
using std::atan2;
using std::cos;
using std::exp;
using std::log;
using std::floor;
using std::pow;
using std::round;
using std::sin;
using std::sqrt;
using std::tan;
using std::trunc;

inline float min(float a, float b) { return std::fmin(a, b); }
inline float max(float a, float b) { return std::fmax(a, b); }
inline int min(int a, int b) { return std::min(a, b); }
inline int max(int a, int b) { return std::max(a, b); }
inline uint min(uint a, uint b) { return std::min(a, b); }
inline uint max(uint a, uint b) { return std::max(a, b); }
inline float3 min(float3 a, float3 b) { return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)}; }
inline float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float3 exp(float3 v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }
inline float frac(float v) { return v - std::floor(v); }
inline float asfloat(uint v) { return std::bit_cast<float>(v); }
inline uint asuint(float v) { return std::bit_cast<uint>(v); }
