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
// are ordinary geometry; the selection density per unit area of those of
// constant emission is a per-material constant, so a BSDF hit needs no light
// index for MIS. Triangles of varying emission (kLightVaryingEmission) have
// their own densities (backend.emitterPdfArea), and the light sample asks the
// backend for the emission at the sampled point (backend.emitterRadiance).

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
// Triangles: the emission varies over the surface. radiance is unused: the
// backend evaluates the material at the sample (info.z: the primitive; p0.w:
// the placement (CPU) or instance (GPU) as float bits).
static const uint kLightVaryingEmission = 8u;
// Triangles: u and v hold the primitive's third and second vertex (placements
// that flip orientation).
static const uint kLightSwappedVertices = 16u;
// Analytic lights with UsdLuxShapingAPI shaping: lobes.z holds the index of
// their shaping block (as float bits; LightShaping).
static const uint kLightShaped = 32u;

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
// the emitted radiance (irradiance for a parallel distant light); radiance.w
// is 1 + the index of the shadow-link category bit (0: every surface casts
// shadows). lobes holds the UsdLux diffuse (x) and specular (y) multipliers:
// the light as diffuse and as glossy BSDF lobes see it (1 for triangles);
// lobes.z the shaping block of kLightShaped lights.
struct LightRecord {
    uint4 info;      // x: type, y: flags, z: texture (rect), w: light-link category bit (0: all)
    float4 p0;
    float4 u;
    float4 v;
    float4 n;
    float4 radiance;
    float4 lobes;
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
    float pdf;       // solid angle density of sampling this point (see intersectLight)
};

inline float3 xyz(float4 v) { return float3(v.x, v.y, v.z); }

inline uint shadowLinkBit(LightRecord light)
{
    return light.radiance.w >= 1.0f ? (1u << (uint(light.radiance.w) - 1u)) : 0u;
}

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

// Uniform sampling of the solid angle a rect light subtends (Urena et al.
// 2013, "An Area-Preserving Parametrization for Spherical Rectangles"). In
// the frame (x, y, z) at the shading point the rectangle spans
// [x0, x1] x [y0, y1] at height z0 < 0.
struct SphericalRect {
    float3 x;
    float3 y;
    float3 z;
    float x0;
    float y0;
    float x1;
    float y1;
    float z0;
    float b0;
    float b1;
    float k;
    float solidAngle;
};

// Below this solid angle the rect is area-sampled: the solid angle loses
// precision (it is a difference of angles near 2 pi) and the two densities
// differ little anyway.
static const float kMinSphericalRectSolidAngle = 1e-3f;

inline SphericalRect sphericalRect(LightRecord light, float3 p)
{
    SphericalRect r;
    const float3 halfU = xyz(light.u);
    const float3 halfV = xyz(light.v);
    const float exl = 2.0f * length(halfU);
    const float eyl = 2.0f * length(halfV);
    r.x = normalize(halfU);
    r.y = normalize(halfV);
    r.z = cross(r.x, r.y);
    const float3 d = xyz(light.p0) - halfU - halfV - p;
    r.z0 = dot(d, r.z);
    if (r.z0 > 0.0f) {
        r.z = -r.z;
        r.z0 = -r.z0;
    }
    r.x0 = dot(d, r.x);
    r.y0 = dot(d, r.y);
    r.x1 = r.x0 + exl;
    r.y1 = r.y0 + eyl;
    const float3 n0 = normalize(float3(0.0f, r.z0, -r.y0));
    const float3 n1 = normalize(float3(-r.z0, 0.0f, r.x1));
    const float3 n2 = normalize(float3(0.0f, -r.z0, r.y1));
    const float3 n3 = normalize(float3(r.z0, 0.0f, -r.x0));
    const float g0 = acos(clamp(-dot(n0, n1), -1.0f, 1.0f));
    const float g1 = acos(clamp(-dot(n1, n2), -1.0f, 1.0f));
    const float g2 = acos(clamp(-dot(n2, n3), -1.0f, 1.0f));
    const float g3 = acos(clamp(-dot(n3, n0), -1.0f, 1.0f));
    r.b0 = n0.z;
    r.b1 = n2.z;
    r.k = 2.0f * kPi - g2 - g3;
    r.solidAngle = g0 + g1 - r.k;
    if (!(r.z0 < 0.0f) || !isfinite(r.solidAngle)) r.solidAngle = 0.0f;
    return r;
}

inline bool useSphericalRect(SphericalRect r) { return r.solidAngle > kMinSphericalRectSolidAngle; }

// The point of the rect for the sample u, relative to the shading point.
inline float3 sampleSphericalRect(SphericalRect r, float2 u)
{
    const float au = u.x * r.solidAngle + r.k;
    const float fu = (cos(au) * r.b0 - r.b1) / sin(au);
    const float cu = clamp((fu > 0.0f ? 1.0f : -1.0f) / sqrt(fu * fu + r.b0 * r.b0), -1.0f, 1.0f);
    const float xu = clamp(-(cu * r.z0) / max(sqrt(1.0f - cu * cu), 1e-7f), r.x0, r.x1);
    const float d2 = xu * xu + r.z0 * r.z0;
    const float h0 = r.y0 / sqrt(d2 + r.y0 * r.y0);
    const float h1 = r.y1 / sqrt(d2 + r.y1 * r.y1);
    const float hv = h0 + u.y * (h1 - h0);
    const float hv2 = hv * hv;
    const float yv = hv2 < 1.0f - 1e-6f ? clamp(hv * sqrt(d2) / sqrt(1.0f - hv2), r.y0, r.y1) : r.y1;
    return r.x * xu + r.y * yv + r.z * r.z0;
}

// Cylinder lights are sampled on the arc a shading point sees (outside the
// cylinder, the points within acos(r / d) of the direction to it, d its
// distance from the axis) and, along the axis, in proportion to the solid
// angle of the axis segment (equiangular). Both depend on the shading point
// only through d and its position along the axis.
struct CylinderFrame {
    float3 axis;      // unit
    float3 e1;        // unit, from the axis toward the shading point
    float3 e2;
    float halfLength;
    float radius;
    float along;      // shading point's coordinate along the axis
    float distance;   // shading point's distance from the axis
    float alpha;      // half angle of the visible arc
    float thetaA;     // angles of the segment's ends seen from the axis point nearest p
    float thetaB;
    bool valid;       // outside the cylinder, non-degenerate
};

inline CylinderFrame cylinderFrame(LightRecord light, float3 p)
{
    CylinderFrame c;
    const float3 axisVector = xyz(light.u);
    c.halfLength = length(axisVector);
    c.radius = length(xyz(light.v));
    c.axis = axisVector / max(c.halfLength, 1e-30f);
    const float3 rel = p - xyz(light.p0);
    c.along = dot(rel, c.axis);
    const float3 perpendicular = rel - c.axis * c.along;
    c.distance = length(perpendicular);
    c.valid = c.halfLength > 0.0f && c.radius > 0.0f && c.distance > c.radius;
    c.e1 = perpendicular / max(c.distance, 1e-30f);
    c.e2 = cross(c.axis, c.e1);
    c.alpha = acos(clamp(c.radius / max(c.distance, 1e-30f), 0.0f, 1.0f));
    c.thetaA = atan2(-c.halfLength - c.along, c.distance);
    c.thetaB = atan2(c.halfLength - c.along, c.distance);
    c.valid = c.valid && c.alpha > 0.0f && c.thetaB > c.thetaA;
    return c;
}

// Density per unit area of the point at axial coordinate t on the visible arc.
inline float cylinderPdfArea(CylinderFrame c, float t)
{
    const float h = t - c.along;
    const float pdfT = c.distance / ((c.thetaB - c.thetaA) * (c.distance * c.distance + h * h));
    return pdfT / (2.0f * c.alpha * c.radius);
}

// Luminance with the working space's weights (SampleSettings::luminance;
// prism::color::ColorManagement::luminanceWeights).
inline float luminanceOf(float3 c, float3 weights) { return max(0.0f, dot(weights, c)); }

// Spatial light selection: about the unoccluded irradiance an analytic light
// brings to p, without the receiver's cosine (its radiance times the solid
// angle it subtends, at most a hemisphere). Zero only where the light cannot
// reach p: behind a one-sided planar light, inside a sphere or cylinder.
inline float lightImportance(LightRecord light, float3 p, float3 luminance)
{
    const float l = luminanceOf(xyz(light.radiance), luminance);
    const uint type = light.info.x;
    if (type == kLightDistant) return light.u.w >= 1.0f ? l : l * 2.0f * kPi * (1.0f - light.u.w);
    const float3 d = p - xyz(light.p0);
    const float d2 = dot(d, d);
    if (type == kLightPoint) return l / max(d2, 1e-20f);
    float solidAngle = 0.0f;
    if (type == kLightSphere) {
        const float r = light.p0.w;
        if (!(d2 > r * r)) return 0.0f;
        solidAngle = 2.0f * kPi * sphereOneMinusCos(r, d2);
    } else if (type == kLightRect || type == kLightDisk) {
        float cosLight = dot(xyz(light.n), d) / sqrt(max(d2, 1e-30f));
        if ((light.info.y & kLightTwoSided) != 0u) cosLight = abs(cosLight);
        if (!(cosLight > 0.0f)) return 0.0f;
        solidAngle = light.v.w * max(cosLight, 0.1f) / max(d2, 1e-20f);
    } else if (type == kLightCylinder) {
        // A thin segment: 2 r (sin(thetaB) - sin(thetaA)) / distance.
        const float3 axisVector = xyz(light.u);
        const float halfLength = length(axisVector);
        const float r = length(xyz(light.v));
        const float along = dot(d, axisVector) / max(halfLength, 1e-30f);
        const float distance2 = max(0.0f, d2 - along * along);
        if (!(distance2 > r * r)) return 0.0f;
        const float b = halfLength - along;
        const float a = -halfLength - along;
        const float sines = b / sqrt(distance2 + b * b) - a / sqrt(distance2 + a * a);
        solidAngle = 2.0f * r * sines / sqrt(distance2);
    }
    return l * min(solidAngle, 2.0f * kPi);
}

// Texture coordinates of a point on a rect light, in image space (top row
// first): UsdLuxRectLight puts the texture's (0, 0) at local (+X, +Y).
inline float2 rectTexcoord(float a, float b) { return float2(0.5f * (1.0f - a), 0.5f * (1.0f + b)); }

// Samples light `light` from shading point p. The radiance of textured rect
// lights still has to be multiplied by the texture at `texcoord`; for
// triangles, `texcoord` holds the weights of the record's second and third
// vertex at the sampled point.
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
        const SphericalRect sr = sphericalRect(light, p);
        if (useSphericalRect(sr)) {
            const float3 offset = sampleSphericalRect(sr, u);
            const float dist = length(offset);
            const float3 rel = p + offset - xyz(light.p0);
            const float3 axisU = xyz(light.u);
            const float3 axisV = xyz(light.v);
            LightSample s = invalidLightSample();
            s.wi = offset / dist;
            // The emitting side faces the shading point (sr.z points to the rect).
            float facing = -dot(xyz(light.n), s.wi);
            if ((light.info.y & kLightTwoSided) != 0u) facing = abs(facing);
            s.radiance = radiance;
            s.pdf = 1.0f / sr.solidAngle;
            s.distance = dist;
            s.delta = false;
            s.valid = facing > 0.0f && dist > 0.0f && isfinite(dist);
            result.sample = s;
            result.texcoord = rectTexcoord(dot(rel, axisU) / dot(axisU, axisU), dot(rel, axisV) / dot(axisV, axisV));
            return result;
        }
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
        const CylinderFrame c = cylinderFrame(light, p);
        if (!c.valid) return result; // inside: the surface emits outward only
        const float phi = (2.0f * u.y - 1.0f) * c.alpha;
        const float3 normal = c.e1 * cos(phi) + c.e2 * sin(phi);
        const float theta = c.thetaA + u.x * (c.thetaB - c.thetaA);
        const float t = clamp(c.along + c.distance * tan(theta), -c.halfLength, c.halfLength);
        const float3 q = xyz(light.p0) + c.axis * t + normal * c.radius;
        LightSample s = invalidLightSample();
        const float3 d = q - p;
        const float dist2 = dot(d, d);
        if (!(dist2 > 0.0f)) return result;
        const float dist = sqrt(dist2);
        s.wi = d / dist;
        const float cosLight = -dot(normal, s.wi);
        if (!(cosLight > 0.0f)) return result;
        s.pdf = cylinderPdfArea(c, t) * dist2 / cosLight;
        s.radiance = radiance;
        s.distance = dist;
        s.delta = false;
        s.valid = s.pdf > 0.0f && isfinite(s.pdf);
        result.sample = s;
        return result;
    }
    if (type == kLightTriangle) {
        const float su = sqrt(u.x);
        const float b0 = 1.0f - su;
        const float b1 = u.y * su;
        const float3 q = xyz(light.p0) * b0 + xyz(light.u) * b1 + xyz(light.v) * (1.0f - b0 - b1);
        result.sample = areaSample(light, p, q, xyz(light.n), radiance);
        result.texcoord = float2(b1, 1.0f - b0 - b1);
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
// The pdf is that of sampling the point from `from`, the vertex the ray
// scattered at: its origin, unless it has since crossed cutouts.
struct LightHitTextured {
    LightHit hit;
    float2 texcoord;
};

inline LightHit planarHit(LightRecord light, float t, float pdfDistance, float3 direction, float3 radiance)
{
    LightHit h = noLightHit();
    float cosLight = -dot(xyz(light.n), direction);
    if ((light.info.y & kLightTwoSided) != 0u) cosLight = abs(cosLight);
    if (!(cosLight > 0.0f) || !(light.v.w > 0.0f)) return h;
    h.hit = true;
    h.t = t;
    h.radiance = radiance;
    h.pdf = pdfDistance * pdfDistance / (cosLight * light.v.w);
    return h;
}

inline LightHitTextured intersectLight(LightRecord light, Ray ray, float tmax, float3 from)
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
        const float3 fc = c - from;
        result.hit.pdf = conePdf(sphereOneMinusCos(r, dot(fc, fc)));
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
        result.hit = planarHit(light, t, length(rel + xyz(light.p0) - from), d, radiance);
        result.texcoord = rectTexcoord(a, b);
        if (result.hit.hit && type == kLightRect) {
            const SphericalRect sr = sphericalRect(light, from);
            if (useSphericalRect(sr)) result.hit.pdf = 1.0f / sr.solidAngle;
        }
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
        if (!(cosLight > 0.0f)) return result;
        const CylinderFrame frame = cylinderFrame(light, from);
        if (!frame.valid) return result;
        result.hit.hit = true;
        result.hit.t = t;
        result.hit.radiance = radiance;
        const float3 fq = q + xyz(light.p0) - from;
        result.hit.pdf = cylinderPdfArea(frame, dot(q, axis)) * dot(fq, fq) / cosLight;
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
// The barycentrics of a triangle's sampled point (sampleLight's texcoord) in
// the primitive's vertex order: the weights of its second and third vertex.
inline float2 primitiveBarycentrics(LightRecord light, float2 weights)
{
    return (light.info.y & kLightSwappedVertices) != 0u ? float2(weights.y, weights.x) : weights;
}

struct LightPick {
    uint index;
    float pmf;
};

// UsdLuxShapingAPI (usdLux/schema.usda, ShapingAPI): emission scaled by the
// direction e it leaves the light in (unit, from the light toward the
// receiver), relative to the light's primary axis (its local -Z):
//   cone     theta = acos(axis . e); zero beyond the cutoff (shaping:cone:angle),
//            1 - smoothstep(smoothStart, cutoff, theta) with smoothStart =
//            cutoff * (1 - softness) (shaping:cone:softness)
//   focus    f = |axis . e|^focus; the colour lerp(focusTint, 1, f)
//   IES      the profile at e in the light's local frame (iesLookup), its
//            theta rescaled by shaping:ies:angleScale, times iesScale (the
//            peak candela, divided by the profile's mean with
//            shaping:ies:normalize)
// The shaping blocks follow the light table's alias entries (lightAlias
// from lightCount() + 1): four float4 per shaped light, then the IES grids
// (width x height values, then each row's maximum: height values), four
// values per float4:
//   [0] xyz primary axis (world, unit), w focus (0: none)
//   [1] xyz the light's local +X (world, unit; IES phi 0), w cutoff (radians; >= pi: no cone)
//   [2] xyz focus tint, w smoothStart (radians)
//   [3] x angle scale, y IES scale, z the profile's first float4 (as uint
//       bits, from lightCount() + 1), w its width | height << 16 (0: no profile)
struct LightShaping {
    float3 axis;
    float focus;
    float3 xAxis;
    float cutoff;
    float3 tint;
    float smoothStart;
    float angleScale;
    float iesScale;
    uint profile;
    uint profileSize;
};

inline LightShaping lightShapingFrom(float4 a, float4 b, float4 c, float4 d)
{
    LightShaping s;
    s.axis = xyz(a);
    s.focus = a.w;
    s.xAxis = xyz(b);
    s.cutoff = b.w;
    s.tint = xyz(c);
    s.smoothStart = c.w;
    s.angleScale = d.x;
    s.iesScale = d.y;
    s.profile = asuint(d.z);
    s.profileSize = asuint(d.w);
    return s;
}

// 1 - smoothstep(smoothStart, cutoff, theta) inside the cone, 0 outside.
inline float shapingCone(LightShaping s, float theta)
{
    if (!(s.cutoff < kPi)) return 1.0f;
    if (theta > s.cutoff) return 0.0f;
    if (!(s.smoothStart < s.cutoff) || theta <= s.smoothStart) return 1.0f;
    const float t = saturate((theta - s.smoothStart) / (s.cutoff - s.smoothStart));
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// lerp(focusTint, 1, |cos theta|^focus).
inline float3 shapingFocus(LightShaping s, float cosTheta)
{
    if (!(s.focus > 0.0f)) return (float3)1.0f;
    const float f = pow(abs(cosTheta), s.focus);
    return s.tint + ((float3)1.0f - s.tint) * f;
}

// The cone and focus factors along e.
inline float3 shapingConeFocus(LightShaping s, float3 e)
{
    const float cosTheta = clamp(dot(s.axis, e), -1.0f, 1.0f);
    const float cone = shapingCone(s, acos(cosTheta));
    if (!(cone > 0.0f)) return (float3)0.0f;
    return shapingFocus(s, cosTheta) * cone;
}

// shaping:ies:angleScale applied to the light's theta (increasing in theta).
inline float iesTheta(LightShaping s, float theta)
{
    if (s.angleScale > 0.0f) return theta / s.angleScale;
    if (s.angleScale < 0.0f) return (theta - kPi) / -s.angleScale + kPi;
    return theta;
}

// The IES angles (theta, phi) of e in radians: theta from the light's local
// -Z, rescaled by shaping:ies:angleScale; phi from local +X toward +Y.
inline float2 iesAngles(LightShaping s, float3 e)
{
    const float theta = acos(clamp(dot(s.axis, e), -1.0f, 1.0f));
    const float3 yAxis = cross(s.xAxis, s.axis); // local +Y = (+Z) x (+X), +Z = -axis
    float phi = atan2(dot(e, yAxis), dot(e, s.xAxis));
    if (phi < 0.0f) phi += 2.0f * kPi;
    return float2(iesTheta(s, theta), phi);
}

// The angle a sphere of radius r around a light's p0 subtends at distance
// dist (pi from inside it).
inline float lightSpread(float radius, float dist)
{
    return dist > radius ? acos(sqrt(max(0.0f, 1.0f - (radius / dist) * (radius / dist)))) : kPi;
}

// Bilinear lookup in an IES grid of `width` columns over phi in [0, 2 pi)
// (periodic) and `height` rows over theta in [0, pi] (src/lights/ies.h).
struct IesLookup {
    uint i0;
    uint i1;
    uint j0;
    float tx;
    float ty;
    bool valid;
};

inline IesLookup iesGridLookup(float theta, float phi, uint width, uint height)
{
    IesLookup l;
    l.i0 = 0u;
    l.i1 = 0u;
    l.j0 = 0u;
    l.tx = 0.0f;
    l.ty = 0.0f;
    l.valid = false;
    if (!(theta >= 0.0f) || theta > kPi || height < 2u || width < 1u) return l;
    const float y = theta / kPi * float(height - 1u);
    l.j0 = min(uint(y), height - 2u);
    l.ty = min(y - float(l.j0), 1.0f);
    float x = phi / (2.0f * kPi) * float(width);
    x = x - floor(x / float(width)) * float(width);
    const float fx = floor(x);
    l.i0 = min(uint(fx), width - 1u);
    l.tx = clamp(x - fx, 0.0f, 1.0f);
    l.i1 = l.i0 + 1u >= width ? 0u : l.i0 + 1u;
    l.valid = true;
    return l;
}

// A bound on the cone and focus factors over the directions from an
// analytic light of bounding radius `radius` (around its record's p0) toward
// p, over the angle range the light subtends there. Zero only where no point
// of the light reaches p. (PathTracer::iesImportance bounds the profile.)
inline float shapingImportance(LightShaping s, float3 p, float3 center, float radius)
{
    const float3 d = p - center;
    const float dist = length(d);
    if (!(dist > 0.0f)) return 1.0f;
    const float theta = acos(clamp(dot(s.axis, d / dist), -1.0f, 1.0f));
    const float spread = lightSpread(radius, dist);
    const float nearest = max(0.0f, theta - spread);
    const float cone = shapingCone(s, nearest);
    if (!(cone > 0.0f)) return 0.0f;
    const float cosMax = max(abs(cos(nearest)), abs(cos(min(kPi, theta + spread))));
    // lerp(tint, 1, f) grows with f, unless a tint component exceeds 1.
    const float3 focus = shapingFocus(s, cosMax);
    float focusBound = max(focus.x, max(focus.y, focus.z));
    if (s.focus > 0.0f) focusBound = max(focusBound, max(s.tint.x, max(s.tint.y, s.tint.z)));
    return cone * focusBound;
}

// The radius of a sphere around p0 that holds an analytic light.
inline float lightBoundingRadius(LightRecord light)
{
    const uint type = light.info.x;
    if (type == kLightSphere) return light.p0.w;
    if (type == kLightRect) return length(xyz(light.u)) + length(xyz(light.v));
    if (type == kLightDisk) return max(length(xyz(light.u)), length(xyz(light.v)));
    if (type == kLightCylinder) return length(xyz(light.u)) + length(xyz(light.v));
    return 0.0f;
}
