#pragma once

// The AOVs PrismRT can output, shared by the Hydra delegate (render buffers),
// the render-product writers of both frontends and the RenderVar mapping
// (docs/hydra.md, "Render products"). To add an AOV: add a kind and a row to
// Aovs(), produce it in the device or in ResolveGeometricAov, and map it in
// the Hydra bridge. The light AOVs (albedo, diffuse, specular, emission) come
// from the path tracer with the colour (LightAovPixel, src/device/device.h).

#include "../ray.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>

namespace prism::output {

enum class AovKind {
    Color, Depth, CameraDepth, Normal, Neye, Peye, St,
    PrimId, ElementId, InstanceId,
    Albedo, Diffuse, Specular, Emission, Pworld,
    Unknown
};

struct AovInfo {
    AovKind kind;
    // The Hydra AOV name (HdAovTokens), also a RenderVar's raw sourceName.
    std::string_view name;
    int components;
    bool integer;
    // From the primary (pixel-centre) hit only; no path tracing.
    bool geometric;
    // Produced by both frontends. Prim, instance and element ids come from
    // each frontend's own tables (the delegate's render index; the CLI's
    // placements, numbered as the delegate numbers them).
    bool standalone;
};

inline std::span<const AovInfo> Aovs()
{
    static constexpr std::array<AovInfo, 15> table{{
        {AovKind::Color, "color", 4, false, false, true},
        {AovKind::Depth, "depth", 1, false, true, true},
        {AovKind::CameraDepth, "cameraDepth", 1, false, true, true},
        {AovKind::Normal, "normal", 3, false, true, true},
        {AovKind::Neye, "Neye", 3, false, true, true},
        {AovKind::Peye, "Peye", 3, false, true, true},
        {AovKind::St, "primvars:st", 2, false, true, true},
        {AovKind::PrimId, "primId", 1, true, true, true},
        {AovKind::ElementId, "elementId", 1, true, true, true},
        {AovKind::InstanceId, "instanceId", 1, true, true, true},
        {AovKind::Albedo, "albedo", 3, false, false, true},
        {AovKind::Diffuse, "diffuse", 3, false, false, true},
        {AovKind::Specular, "specular", 3, false, false, true},
        {AovKind::Emission, "emission", 3, false, false, true},
        {AovKind::Pworld, "Pworld", 3, false, true, true},
    }};
    return table;
}

inline AovInfo const *FindAov(AovKind kind)
{
    for (auto const &info : Aovs())
        if (info.kind == kind) return &info;
    return nullptr;
}

inline AovKind AovKindFromName(std::string_view name)
{
    for (auto const &info : Aovs())
        if (info.name == name) return info.kind;
    // Other renderers' name of the world-space position.
    if (name == "P") return AovKind::Pworld;
    return AovKind::Unknown;
}

// The AOVs the path tracer gathers with the colour (LightAovPixel): RGB,
// accumulated over the same samples.
inline bool LightAov(AovKind kind)
{
    return kind == AovKind::Albedo || kind == AovKind::Diffuse || kind == AovKind::Specular ||
        kind == AovKind::Emission;
}

inline int AovComponents(AovKind kind)
{
    auto const *info = FindAov(kind);
    return info ? info->components : 0;
}

inline bool IntegerAov(AovKind kind)
{
    auto const *info = FindAov(kind);
    return info && info->integer;
}

// The value of a geometric, non-integer AOV for a pixel-centre hit (misses
// included): depth (NDC z in [0, 1], 1 on a miss), camera depth (distance
// along -Z in eye space, +inf on a miss), world/eye normals, eye and world
// positions (0 on a miss) and the hit's barycentric st. `values` holds
// AovComponents(kind) floats.
inline void ResolveGeometricAov(AovKind kind, RayHit const &hit,
                                pxr::GfMatrix4d const &worldToView,
                                pxr::GfMatrix4d const &projection, float *values)
{
    const bool valid = hit.geomid != RTC_INVALID_GEOMETRY_ID;
    if (kind == AovKind::Depth || kind == AovKind::CameraDepth) {
        values[0] = kind == AovKind::Depth ? 1.0f : std::numeric_limits<float>::infinity();
        if (!valid) return;
        pxr::GfVec4d view(hit.p.x(), hit.p.y(), hit.p.z(), 1.0);
        view = view * worldToView;
        if (kind == AovKind::CameraDepth) values[0] = static_cast<float>(-view[2]);
        else {
            const pxr::GfVec4d clip = view * projection;
            if (std::abs(clip[3]) > 1e-12) values[0] = static_cast<float>(.5 * clip[2] / clip[3] + .5);
        }
    } else if (kind == AovKind::Peye) {
        values[0] = values[1] = values[2] = 0;
        if (valid) {
            const pxr::GfVec3d eye = worldToView.Transform(pxr::GfVec3d(hit.p.x(), hit.p.y(), hit.p.z()));
            for (size_t i = 0; i < 3; ++i) values[i] = static_cast<float>(eye[i]);
        }
    } else if (kind == AovKind::Pworld) {
        values[0] = valid ? hit.p.x() : 0;
        values[1] = valid ? hit.p.y() : 0;
        values[2] = valid ? hit.p.z() : 0;
    } else if (kind == AovKind::St) {
        values[0] = valid ? hit.u : 0; values[1] = valid ? hit.v : 0;
    } else if (kind == AovKind::Normal || kind == AovKind::Neye) {
        Float3 normal = hit.n;
        if (kind == AovKind::Neye && valid) {
            const pxr::GfVec3d eye = worldToView.TransformDir(
                pxr::GfVec3d(normal.x(), normal.y(), normal.z())).GetNormalized();
            normal = Float3(static_cast<float>(eye[0]), static_cast<float>(eye[1]), static_cast<float>(eye[2]));
        }
        values[0] = normal.x(); values[1] = normal.y(); values[2] = normal.z();
    }
}

} // namespace prism::output
