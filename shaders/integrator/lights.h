#pragma once

// Lights of the shared path tracer: the analytic UsdLux shapes and emissive
// triangles, in one record type shared by the CPU backend and the GPU kernels
// (built by src/device/light_table.h). Same C++/HLSL subset as types.h.
//
// Next-event estimation picks one entry of the scene's light table with a
// power-proportional alias table; the dome (all dome lights together) is one
// more entry after the records. Analytic shapes are not in the acceleration
// structure: camera and BSDF rays pass through them and collect their
// emission (intersectLight), and shadow rays ignore them. Emissive triangles
// are ordinary geometry; their selection density per unit area is a per
// material constant, so a BSDF hit needs no light index for MIS.

#include "types.h"

static const uint kLightSphere = 1u;
static const uint kLightRect = 2u;
static const uint kLightDisk = 3u;
static const uint kLightCylinder = 4u;
static const uint kLightDistant = 5u;
static const uint kLightPoint = 6u;
static const uint kLightTriangle = 7u;

// LightRecord.info.y
static const uint kLightCameraVisible = 1u;
static const uint kLightTextured = 2u;
static const uint kLightTwoSided = 4u;

static const uint kLightTextureNone = 0xffffffffu;

// World-space light. Field use by type:
//   sphere    p0 center, w radius
//   rect      p0 center; u, v half extents (local X, Y); n emitting normal (-Z)
//   disk      p0 center; u, v radius vectors (local X, Y); n emitting normal (-Z)
//   cylinder  p0 center; u half length along the axis (local X); v radius vector (local Y)
//   distant   n direction the light travels (-Z); u.w cos of the half angle
//   point     p0 position; radiance is the intensity
//   triangle  p0, u, v vertices; n front normal
// v.w is the area of area lights, n.w the selection probability, radiance
// the emitted radiance (irradiance for a parallel distant light).
struct LightRecord {
    uint4 info;      // x: type, y: flags, z: texture (rect), w: light-link mask
    float4 p0;
    float4 u;
    float4 v;
    float4 n;
    float4 radiance;
};

struct LightSample {
    float3 wi;
    float3 radiance;
    float pdf;       // solid angle, without the selection probability; 1 for delta lights
    float distance;  // to the sampled point; infinity for distant lights
    bool delta;
    bool valid;
};

// A BSDF or camera ray crossing an analytic light.
struct LightHit {
    bool hit;
    float t;
    float3 radiance;
    float pdf;       // solid angle density of sampling this point from the ray origin
};

inline float3 xyz(float4 v) { return float3(v.x, v.y, v.z); }

inline LightSample invalidLightSample()
{
    const LightSample s = {(float3)0.0f, (float3)0.0f, 0.0f, 0.0f, false, false};
    return s;
}

inline LightHit noLightHit()
{
    const LightHit h = {false, 0.0f, (float3)0.0f, 0.0f};
    return h;
}

inline bool isDeltaLight(LightRecord light)
{
    return light.info.x == kLightPoint || (light.info.x == kLightDistant && light.u.w >= 1.0f);
}

struct Frame {
    float3 t;
    float3 b;
};

inline Frame frameAround(float3 n)
{
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float c = n.x * n.y * a;
    const Frame f = {float3(1.0f + s * n.x * n.x * a, s * c, -s * n.x), float3(c, s + n.y * n.y * a, -n.y)};
    return f;
}

// Uniform direction in the cone around `axis` with cos(half angle) cosMax.
inline float3 sampleCone(float3 axis, float cosMax, float2 u)
{
    const float cosTheta = 1.0f - u.x * (1.0f - cosMax);
    const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * kPi * u.y;
    const Frame f = frameAround(axis);
    return normalize(f.t * (cos(phi) * sinTheta) + f.b * (sin(phi) * sinTheta) + axis * cosTheta);
}

inline float conePdf(float oneMinusCosMax)
{
    return oneMinusCosMax > 0.0f ? 1.0f / (2.0f * kPi * oneMinusCosMax) : 0.0f;
}

// 1 - cos of the half angle a sphere of radius r subtends at squared distance d2.
inline float sphereOneMinusCos(float r, float d2)
{
    const float sin2 = min(1.0f, r * r / d2);
    return sin2 / (1.0f + sqrt(max(0.0f, 1.0f - sin2)));
}

inline float2 concentricDisk(float2 u)
{
    const float a = 2.0f * u.x - 1.0f;
    const float b = 2.0f * u.y - 1.0f;
    if (a == 0.0f && b == 0.0f) return float2(0.0f, 0.0f);
    float r;
    float phi;
    if (abs(a) > abs(b)) {
        r = a;
        phi = (kPi / 4.0f) * (b / a);
    } else {
        r = b;
        phi = (kPi / 2.0f) - (kPi / 4.0f) * (a / b);
    }
    return float2(r * cos(phi), r * sin(phi));
}

// Area-sampled point q with normal nq: the sample seen from p.
inline LightSample areaSample(LightRecord light, float3 p, float3 q, float3 nq, float3 radiance)
{
    LightSample s = invalidLightSample();
    const float3 d = q - p;
    const float dist2 = dot(d, d);
    if (!(dist2 > 0.0f)) return s;
    const float dist = sqrt(dist2);
    s.wi = d / dist;
    float cosLight = -dot(nq, s.wi);
    if ((light.info.y & kLightTwoSided) != 0u) cosLight = abs(cosLight);
    if (!(cosLight > 0.0f) || !(light.v.w > 0.0f)) return s;
    s.pdf = dist2 / (cosLight * light.v.w);
    s.radiance = radiance;
    s.distance = dist;
    s.delta = false;
    s.valid = isfinite(s.pdf);
    return s;
}

// Texture coordinates of a point on a rect light, in image space (top row
// first): UsdLuxRectLight puts the texture's (0, 0) at local (+X, +Y).
inline float2 rectTexcoord(float a, float b) { return float2(0.5f * (1.0f - a), 0.5f * (1.0f + b)); }

// Samples light `light` from shading point p. The radiance of textured rect
// lights still has to be multiplied by the texture at `texcoord`.
struct LightSampleTextured {
    LightSample sample;
    float2 texcoord;
};

inline LightSampleTextured sampleLight(LightRecord light, float3 p, float2 u)
{
    LightSampleTextured result;
    result.sample = invalidLightSample();
    result.texcoord = float2(0.0f, 0.0f);
    const uint type = light.info.x;
    const float3 radiance = xyz(light.radiance);
    if (type == kLightSphere) {
        const float3 c = xyz(light.p0);
        const float r = light.p0.w;
        const float3 dc = c - p;
        const float d2 = dot(dc, dc);
        if (!(d2 > r * r)) return result; // inside: the surface emits outward only
        const float oneMinusCos = sphereOneMinusCos(r, d2);
        const float3 axis = dc / sqrt(d2);
        const float3 wi = sampleCone(axis, 1.0f - oneMinusCos, u);
        // Nearest root of |p + t wi - c| = r.
        const float b = dot(dc, wi);
        const float t = b - sqrt(max(0.0f, r * r - (d2 - b * b)));
        LightSample s = invalidLightSample();
        s.wi = wi;
        s.radiance = radiance;
        s.pdf = conePdf(oneMinusCos);
        s.distance = max(0.0f, t);
        s.delta = false;
        s.valid = s.pdf > 0.0f && isfinite(s.pdf);
        result.sample = s;
        return result;
    }
    if (type == kLightRect) {
        const float a = 2.0f * u.x - 1.0f;
        const float b = 2.0f * u.y - 1.0f;
        const float3 q = xyz(light.p0) + xyz(light.u) * a + xyz(light.v) * b;
        result.sample = areaSample(light, p, q, xyz(light.n), radiance);
        result.texcoord = rectTexcoord(a, b);
        return result;
    }
    if (type == kLightDisk) {
        const float2 d = concentricDisk(u);
        const float3 q = xyz(light.p0) + xyz(light.u) * d.x + xyz(light.v) * d.y;
        result.sample = areaSample(light, p, q, xyz(light.n), radiance);
        return result;
    }
    if (type == kLightCylinder) {
        const float3 axis = xyz(light.u);
        const float3 r1 = xyz(light.v);
        const float3 r2 = normalize(cross(axis, r1)) * length(r1);
        const float phi = 2.0f * kPi * u.y;
        const float3 radial = r1 * cos(phi) + r2 * sin(phi);
        const float3 q = xyz(light.p0) + axis * (2.0f * u.x - 1.0f) + radial;
        result.sample = areaSample(light, p, q, normalize(radial), radiance);
        return result;
    }
    if (type == kLightTriangle) {
        const float su = sqrt(u.x);
        const float b0 = 1.0f - su;
        const float b1 = u.y * su;
        const float3 q = xyz(light.p0) * b0 + xyz(light.u) * b1 + xyz(light.v) * (1.0f - b0 - b1);
        result.sample = areaSample(light, p, q, xyz(light.n), radiance);
        return result;
    }
    if (type == kLightDistant) {
        LightSample s = invalidLightSample();
        const float3 toLight = -normalize(xyz(light.n));
        s.distance = infinity();
        s.radiance = radiance;
        if (light.u.w >= 1.0f) {
            s.wi = toLight;
            s.pdf = 1.0f;
            s.delta = true;
        } else {
            s.wi = sampleCone(toLight, light.u.w, u);
            s.pdf = conePdf(1.0f - light.u.w);
            s.delta = false;
        }
        s.valid = s.pdf > 0.0f;
        result.sample = s;
        return result;
    }
    if (type == kLightPoint) {
        LightSample s = invalidLightSample();
        const float3 d = xyz(light.p0) - p;
        const float dist2 = dot(d, d);
        if (!(dist2 > 0.0f)) return result;
        s.distance = sqrt(dist2);
        s.wi = d / s.distance;
        s.radiance = radiance / dist2;
        s.pdf = 1.0f;
        s.delta = true;
        s.valid = true;
        result.sample = s;
        return result;
    }
    return result;
}

// The emitting side of an analytic shape along a ray within (tnear, tmax),
// distances in units of the ray's direction (which need not be normalized).
// Distant lights are found on escape (escapedDistantLight) and emissive
// triangles by the acceleration structure. `texcoord` as in sampleLight.
struct LightHitTextured {
    LightHit hit;
    float2 texcoord;
};

inline LightHit planarHit(LightRecord light, float t, float3 direction, float3 radiance)
{
    LightHit h = noLightHit();
    float cosLight = -dot(xyz(light.n), direction);
    if ((light.info.y & kLightTwoSided) != 0u) cosLight = abs(cosLight);
    if (!(cosLight > 0.0f) || !(light.v.w > 0.0f)) return h;
    h.hit = true;
    h.t = t;
    h.radiance = radiance;
    h.pdf = t * t / (cosLight * light.v.w);
    return h;
}

inline LightHitTextured intersectLight(LightRecord light, Ray ray, float tmax)
{
    LightHitTextured result;
    result.hit = noLightHit();
    result.texcoord = float2(0.0f, 0.0f);
    const uint type = light.info.x;
    const float3 o = ray.origin;
    const float scale = length(ray.direction);
    const float3 d = ray.direction / scale;
    // From here on, distances along the normalized direction.
    const float tmin = ray.tnear * scale;
    tmax *= scale;
    const float3 radiance = xyz(light.radiance);
    if (type == kLightSphere) {
        const float3 c = xyz(light.p0);
        const float r = light.p0.w;
        const float3 oc = c - o;
        const float d2 = dot(oc, oc);
        if (!(d2 > r * r)) return result;
        const float b = dot(oc, d);
        const float disc = r * r - (d2 - b * b);
        if (disc < 0.0f) return result;
        const float t = b - sqrt(disc);
        if (!(t > tmin && t < tmax)) return result;
        result.hit.hit = true;
        result.hit.t = t;
        result.hit.radiance = radiance;
        result.hit.pdf = conePdf(sphereOneMinusCos(r, d2));
        return result;
    }
    if (type == kLightRect || type == kLightDisk) {
        const float3 n = xyz(light.n);
        const float denom = dot(n, d);
        if (denom == 0.0f) return result;
        const float t = dot(xyz(light.p0) - o, n) / denom;
        if (!(t > tmin && t < tmax)) return result;
        const float3 rel = o + d * t - xyz(light.p0);
        const float3 u = xyz(light.u);
        const float3 v = xyz(light.v);
        const float a = dot(rel, u) / dot(u, u);
        const float b = dot(rel, v) / dot(v, v);
        if (type == kLightRect ? (abs(a) > 1.0f || abs(b) > 1.0f) : (a * a + b * b > 1.0f)) return result;
        result.hit = planarHit(light, t, d, radiance);
        result.texcoord = rectTexcoord(a, b);
        return result;
    }
    if (type == kLightCylinder) {
        const float3 axisVector = xyz(light.u);
        const float halfLength = length(axisVector);
        const float r = length(xyz(light.v));
        if (!(halfLength > 0.0f) || !(r > 0.0f)) return result;
        const float3 axis = axisVector / halfLength;
        const float3 oc = o - xyz(light.p0);
        const float3 dp = d - axis * dot(d, axis);
        const float3 op = oc - axis * dot(oc, axis);
        const float a = dot(dp, dp);
        const float b = dot(op, dp);
        const float c = dot(op, op) - r * r;
        if (!(a > 0.0f) || c <= 0.0f) return result; // parallel, or inside: emits outward only
        const float disc = b * b - a * c;
        if (disc < 0.0f) return result;
        const float t = (-b - sqrt(disc)) / a;
        if (!(t > tmin && t < tmax)) return result;
        const float3 q = oc + d * t;
        if (abs(dot(q, axis)) > halfLength) return result;
        const float3 nq = normalize(q - axis * dot(q, axis));
        const float cosLight = -dot(nq, d);
        if (!(cosLight > 0.0f) || !(light.v.w > 0.0f)) return result;
        result.hit.hit = true;
        result.hit.t = t;
        result.hit.radiance = radiance;
        result.hit.pdf = t * t / (cosLight * light.v.w);
        return result;
    }
    return result;
}

// An escaped ray seeing the disk of a distant light with a nonzero angle.
inline LightHit escapedDistantLight(LightRecord light, float3 direction)
{
    LightHit h = noLightHit();
    if (light.info.x != kLightDistant || light.u.w >= 1.0f) return h;
    if (dot(normalize(direction), -normalize(xyz(light.n))) < light.u.w) return h;
    h.hit = true;
    h.t = infinity();
    h.radiance = xyz(light.radiance);
    h.pdf = conePdf(1.0f - light.u.w);
    return h;
}

// Alias table over the light table's entries: x the probability of keeping
// the column, y its alias (as float bits), z the entry's selection
// probability. Picks an entry with one uniform number.
struct LightPick {
    uint index;
    float pmf;
};
