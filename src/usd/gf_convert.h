#pragma once

// Exact conversions between the core's std-only math (src/matrix4d.h) and
// pxr's Gf types, for the USD front ends (prism_usd, hdPrism).

#include "../matrix4d.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>

namespace prism::usd {

inline prism::Matrix4d ToMatrix4d(pxr::GfMatrix4d const& gf)
{
    prism::Matrix4d result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) result.m[r][c] = gf[r][c];
    return result;
}

inline pxr::GfMatrix4d ToGf(prism::Matrix4d const& m)
{
    pxr::GfMatrix4d result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) result[r][c] = m.m[r][c];
    return result;
}

inline prism::Vec3d ToVec3d(pxr::GfVec3d const& v) { return prism::Vec3d(v[0], v[1], v[2]); }
inline pxr::GfVec3d ToGf(prism::Vec3d const& v) { return pxr::GfVec3d(v[0], v[1], v[2]); }

} // namespace prism::usd
