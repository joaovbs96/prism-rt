#pragma once

// The USD loaders' side of mesh conversion (src/mesh/mesh_conversion.h): the
// mesh prim as Vt arrays (shared with Hydra data sources or read from a
// UsdGeomMesh, no copies), and its std-only MeshSource view for the core.

#include "../internal_export.h"
#include "../mesh/mesh_conversion.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>

#include <optional>
#include <utility>
#include <vector>

namespace prism::usd {

// A primvar's value (indices applied) and interpolation token (constant,
// uniform, varying, vertex, faceVarying).
struct UsdPrimvarData {
    pxr::TfToken interpolation;
    pxr::VtValue value;
};

// The mesh prim as both USD loaders read it. Empty tokens are unauthored.
struct PRISM_USD_INTERNAL_API UsdMeshSource {
    pxr::VtIntArray faceVertexCounts, faceVertexIndices, holeIndices;
    UsdPrimvarData points;   // VtVec3fArray or VtVec3dArray
    UsdPrimvarData normals;  // primvars:normals over normals
    UsdPrimvarData uvs;      // primvar st, else uv
    bool multipleUvSets = false;
    // The other primvars (inherited constant ones included), for the mesh's
    // named primvars (geompropvalue lookups); points, normals and
    // namespaced names are skipped.
    std::vector<std::pair<pxr::TfToken, UsdPrimvarData>> primvars;
    pxr::TfToken subdivisionScheme;  // empty: none (a polygon mesh)
    pxr::TfToken orientation;        // empty: rightHanded
    std::optional<bool> doubleSided; // unauthored: single-sided
    // Subdivision tags.
    pxr::VtIntArray creaseIndices, creaseLengths, cornerIndices;
    pxr::VtFloatArray creaseSharpnesses, cornerSharpnesses;
    pxr::TfToken interpolateBoundary, faceVaryingLinearInterpolation, triangleSubdivisionRule;
    bool motionBlur = false;

    // Views of these arrays (valid while this source lives, unchanged).
    prism::mesh::MeshSource View() const;
};

// A view of a primvar value: arrays of float, double and int scalars and
// vectors (2-4), and single float, double, int and GfVec2f/3f/4f values;
// other types are Unsupported.
PRISM_USD_INTERNAL_API prism::mesh::PrimvarView View(pxr::VtValue const& value, pxr::TfToken const& interpolation = {});
inline prism::mesh::PrimvarView View(UsdPrimvarData const& data) { return View(data.value, data.interpolation); }

// Values of a primvar as floats (prism::mesh::PrimvarFloats of its view).
PRISM_USD_INTERNAL_API bool PrimvarFloats(pxr::VtValue const& value, std::vector<float>* out, int* components);

// prism::mesh::PrimvarIndex for an interpolation token.
inline size_t PrimvarIndex(pxr::TfToken const& interpolation, size_t point, size_t corner, size_t face)
{
    return prism::mesh::PrimvarIndex(prism::mesh::InterpolationFromName(interpolation.GetString()), point,
                                     corner, face);
}

// prism::mesh::ConvertMesh of the source's view.
inline prism::mesh::MeshConversion ConvertMesh(UsdMeshSource const& source,
                                               prism::mesh::SubdivisionSettings const& subdivision,
                                               Material const* displacer = nullptr, int levelCap = -1)
{
    return prism::mesh::ConvertMesh(source.View(), subdivision, displacer, levelCap);
}

} // namespace prism::usd
