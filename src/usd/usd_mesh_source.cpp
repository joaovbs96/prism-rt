#include "usd_mesh_source.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::usd {
namespace {

using Type = prism::mesh::PrimvarView::Type;

template <class Array>
bool ArrayView(VtValue const& value, Type type, int components, prism::mesh::PrimvarView* view)
{
    if (!value.IsHolding<Array>()) return false;
    // Const access: indexing a non-const VtArray detaches (copies) the
    // buffer it shares with the captured data source.
    Array const& array = value.UncheckedGet<Array>();
    view->type = type;
    view->components = components;
    view->array = true;
    view->data = array.cdata();
    view->size = array.size();
    return true;
}

template <class Single>
bool SingleView(VtValue const& value, Type type, int components, prism::mesh::PrimvarView* view)
{
    if (!value.IsHolding<Single>()) return false;
    view->type = type;
    view->components = components;
    view->array = false;
    view->data = &value.UncheckedGet<Single>();
    view->size = 1;
    return true;
}

std::span<const int> Span(VtIntArray const& array) { return {array.cdata(), array.size()}; }
std::span<const float> Span(VtFloatArray const& array) { return {array.cdata(), array.size()}; }

} // namespace

prism::mesh::PrimvarView View(VtValue const& value, TfToken const& interpolation)
{
    prism::mesh::PrimvarView view;
    view.interpolation = prism::mesh::InterpolationFromName(interpolation.GetString());
    if (value.IsEmpty()) return view;
    static_assert(sizeof(GfVec2f) == 2 * sizeof(float) && sizeof(GfVec3f) == 3 * sizeof(float) &&
                  sizeof(GfVec4f) == 4 * sizeof(float) && sizeof(GfVec2d) == 2 * sizeof(double) &&
                  sizeof(GfVec3d) == 3 * sizeof(double) && sizeof(GfVec4d) == 4 * sizeof(double));
    if (ArrayView<VtFloatArray>(value, Type::Float, 1, &view) ||
        ArrayView<VtDoubleArray>(value, Type::Double, 1, &view) ||
        ArrayView<VtIntArray>(value, Type::Int, 1, &view) ||
        ArrayView<VtVec2fArray>(value, Type::Float, 2, &view) ||
        ArrayView<VtVec2dArray>(value, Type::Double, 2, &view) ||
        ArrayView<VtVec3fArray>(value, Type::Float, 3, &view) ||
        ArrayView<VtVec3dArray>(value, Type::Double, 3, &view) ||
        ArrayView<VtVec4fArray>(value, Type::Float, 4, &view) ||
        ArrayView<VtVec4dArray>(value, Type::Double, 4, &view) ||
        SingleView<float>(value, Type::Float, 1, &view) ||
        SingleView<double>(value, Type::Double, 1, &view) ||
        SingleView<int>(value, Type::Int, 1, &view) ||
        SingleView<GfVec2f>(value, Type::Float, 2, &view) ||
        SingleView<GfVec3f>(value, Type::Float, 3, &view) ||
        SingleView<GfVec4f>(value, Type::Float, 4, &view))
        return view;
    view.type = Type::Unsupported;
    return view;
}

bool PrimvarFloats(VtValue const& value, std::vector<float>* out, int* components)
{
    return prism::mesh::PrimvarFloats(View(value), out, components);
}

prism::mesh::MeshSource UsdMeshSource::View() const
{
    prism::mesh::MeshSource view;
    view.faceVertexCounts = Span(faceVertexCounts);
    view.faceVertexIndices = Span(faceVertexIndices);
    view.holeIndices = Span(holeIndices);
    view.points = prism::usd::View(points);
    view.normals = prism::usd::View(normals);
    view.uvs = prism::usd::View(uvs);
    view.multipleUvSets = multipleUvSets;
    view.primvars.reserve(primvars.size());
    for (auto const& [name, data] : primvars) view.primvars.emplace_back(name.GetString(), prism::usd::View(data));
    view.subdivisionScheme = subdivisionScheme.GetString();
    view.orientation = orientation.GetString();
    view.doubleSided = doubleSided;
    view.creaseIndices = Span(creaseIndices);
    view.creaseLengths = Span(creaseLengths);
    view.cornerIndices = Span(cornerIndices);
    view.creaseSharpnesses = Span(creaseSharpnesses);
    view.cornerSharpnesses = Span(cornerSharpnesses);
    view.interpolateBoundary = interpolateBoundary.GetString();
    view.faceVaryingLinearInterpolation = faceVaryingLinearInterpolation.GetString();
    view.triangleSubdivisionRule = triangleSubdivisionRule.GetString();
    view.motionBlur = motionBlur;
    return view;
}

} // namespace prism::usd
