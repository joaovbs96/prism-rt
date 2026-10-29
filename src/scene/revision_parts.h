#pragma once

// The revision mechanics shared by the public Scene's commits
// (src/scene/scene.cpp) and the Hydra delegate's snapshot banks
// (src/hydra/scene_bridge.cpp), docs/sdk_design.md 3.5 and docs/DECISIONS.md
// (2026-10-29): how a placement becomes an Embree instance and moves, the
// shading transform devices read for it, the light and shadow link category
// bits, and when a spare revision may be edited in place.
//
// Each front end keeps its own identities on top: the scene keys its pools
// by its handles and conversion inputs and places by placement ordinal; the
// delegate keys by Hydra paths and data sources, lets Embree number its
// placements and replays its dirty journal into the spare bank. Matrix4d is
// bit-identical to GfMatrix4d (src/matrix4d.h), so the delegate's Gf
// transforms go through the same arithmetic.
//
// Header-only and stateless: hdPrism compiles it in, nothing is exported.

#include "../matrix4d.h"
#include "../scene.h"

#include <embree4/rtcore.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace prism::revision {

// What devices shade a placement with: normals by the inverse transpose,
// tangents by the matrix itself. A mirroring transform flips the winding of
// triangles (flipWithMirror) but not of spheres, whose reflection changes
// only their UV frame's handedness.
inline SceneManager::PlacementTransform PlacementTransformOf(Matrix4d const& world, bool flipWithMirror)
{
    SceneManager::PlacementTransform placement;
    const Matrix4d normal = world.inverse().transpose();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            placement.normal[size_t(r * 3 + c)] = float(normal[size_t(r)][c]);
            placement.tangent[size_t(r * 3 + c)] = float(world[size_t(r)][c]);
        }
    placement.mirrored = world.determinant() < 0.0;
    placement.flipOrientation = flipWithMirror && placement.mirrored;
    return placement;
}

// An instance's transform (row vectors) in Embree's column-major affine
// convention.
inline void SetInstanceTransform(RTCGeometry instance, Matrix4d const& world)
{
    float matrix[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) matrix[c * 4 + r] = float(world[size_t(c)][r]);
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matrix);
}

// An Embree instance of `prototype` at `world`, attached to `top` as
// geometry `id`, or with the next free id when `id` is
// RTC_INVALID_GEOMETRY_ID. Returns its id.
inline unsigned AttachInstance(RTCDevice device, RTCScene top, RTCScene prototype, Matrix4d const& world,
                               unsigned id = RTC_INVALID_GEOMETRY_ID)
{
    RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(instance, prototype);
    SetInstanceTransform(instance, world);
    rtcCommitGeometry(instance);
    if (id == RTC_INVALID_GEOMETRY_ID) id = rtcAttachGeometry(top, instance);
    else rtcAttachGeometryByID(top, instance, id);
    rtcReleaseGeometry(instance);
    return id;
}

// Moves the instance attached as `id` (no instance there: nothing).
inline bool MoveInstance(RTCScene top, unsigned id, Matrix4d const& world)
{
    RTCGeometry instance = rtcGetGeometry(top, id);
    if (!instance) return false;
    SetInstanceTransform(instance, world);
    rtcCommitGeometry(instance);
    return true;
}

// Light and shadow link categories (UsdLux light linking): a bit per
// distinct category, in the order the lights name them, at most 32 of each
// kind (SceneLight::lightLinkBit, SceneManager::placement_link_masks).
template <class Name, class Hash = std::hash<Name>>
class LinkCategories {
public:
    // The bit of a category (0: none, or a 33rd category, which sets
    // `overflowed`). `named` is false for the empty category.
    uint32_t bit(Name const& name, bool named)
    {
        if (!named) return 0u;
        auto found = m_bits.find(name);
        if (found != m_bits.end()) return found->second;
        if (m_bits.size() >= 32) {
            overflowed = true;
            return 0u;
        }
        const uint32_t bit = 1u << m_bits.size();
        m_bits.emplace(name, bit);
        return bit;
    }
    // The bits of the categories `includes` accepts (a placement's mask).
    template <class Includes>
    uint32_t mask(Includes&& includes) const
    {
        uint32_t bits = 0;
        for (auto const& [name, bit] : m_bits)
            if (includes(name)) bits |= bit;
        return bits;
    }
    bool empty() const { return m_bits.empty(); }
    void clear()
    {
        m_bits.clear();
        overflowed = false;
    }
    bool overflowed = false;

private:
    std::unordered_map<Name, uint32_t, Hash> m_bits;
};

// A spare revision may be edited in place only while nothing but its bank
// holds it: no session renders it and no published frame pins it.
template <class Revision>
bool Exclusive(std::shared_ptr<Revision> const& revision)
{
    return revision && revision.use_count() == 1;
}

} // namespace prism::revision
