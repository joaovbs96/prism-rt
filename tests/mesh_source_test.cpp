// The std-only MeshSource (src/mesh/mesh_conversion.h) and its Vt adapter
// (src/usd/usd_mesh_source.h): a mesh described directly by spans converts
// exactly as the same mesh read through Vt arrays, and Vt primvar values map
// to the views the conversion accepted before it was split from USD.

#include "../src/mesh/mesh_conversion.h"
#include "../src/usd/usd_mesh_source.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec2i.h>

#include <bit>
#include <cstring>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int failures = 0;
void Expect(bool ok, char const* what)
{
    if (!ok) { ++failures; std::cerr << "FAILED: " << what << "\n"; }
}

bool SameMesh(Mesh const& a, Mesh const& b)
{
    auto same = [](auto const& x, auto const& y) {
        return x.size() == y.size() &&
            (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return same(a.vertices, b.vertices) && same(a.normals, b.normals) && same(a.texcoords, b.texcoords) &&
        same(a.tangents, b.tangents) && same(a.indices, b.indices) && same(a.primvarValues, b.primvarValues) &&
        a.single_sided_all == b.single_sided_all;
}

} // namespace

int main()
{
    // A cube-ish open box of quads (subdivided) with double points, double
    // face-varying UVs, a per-point float primvar and a constant colour.
    const std::vector<int> counts{4, 4, 4, 3};
    const std::vector<int> indices{0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 6};
    const std::vector<double> points{-1, -1, -0.0, 1, -1, 0, 1, 1, 0, -1, 1, 0,
                                     -1, -1, 2, 1, -1, 2, 1, 1, 2.25, -1, 1, 2};
    std::vector<double> uvs;
    for (size_t i = 0; i < indices.size(); ++i) { uvs.push_back(0.1 * double(i)); uvs.push_back(1.0 - 0.07 * double(i)); }
    const std::vector<float> weight{0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f};
    const float color[3] = {0.25f, 0.5f, 0.75f};

    for (std::string_view scheme : {"none", "catmullClark", "bilinear", ""}) {
        // std-only description
        using View = prism::mesh::PrimvarView;
        using prism::mesh::Interpolation;
        prism::mesh::MeshSource direct;
        direct.faceVertexCounts = counts;
        direct.faceVertexIndices = indices;
        direct.points = {Interpolation::Vertex, View::Type::Double, 3, true, points.data(), points.size() / 3};
        direct.uvs = {Interpolation::FaceVarying, View::Type::Double, 2, true, uvs.data(), uvs.size() / 2};
        direct.primvars.push_back({"weight", {Interpolation::Vertex, View::Type::Float, 1, true, weight.data(), weight.size()}});
        direct.primvars.push_back({"displayColor", {Interpolation::Constant, View::Type::Float, 3, false, color, 1}});
        direct.primvars.push_back({"tint", {Interpolation::Constant, View::Type::Float, 3, false, color, 1}});
        direct.subdivisionScheme = scheme;
        direct.orientation = "leftHanded";

        // the same through Vt
        prism::usd::UsdMeshSource vt;
        vt.faceVertexCounts.assign(counts.begin(), counts.end());
        vt.faceVertexIndices.assign(indices.begin(), indices.end());
        VtVec3dArray vtPoints;
        for (size_t i = 0; i < points.size(); i += 3) vtPoints.push_back(GfVec3d(points[i], points[i + 1], points[i + 2]));
        vt.points = {TfToken("vertex"), VtValue(vtPoints)};
        VtVec2dArray vtUvs;
        for (size_t i = 0; i < uvs.size(); i += 2) vtUvs.push_back(GfVec2d(uvs[i], uvs[i + 1]));
        vt.uvs = {TfToken("faceVarying"), VtValue(vtUvs)};
        vt.primvars.push_back({TfToken("weight"), {TfToken("vertex"), VtValue(VtFloatArray(weight.begin(), weight.end()))}});
        vt.primvars.push_back({TfToken("displayColor"), {TfToken("constant"), VtValue(GfVec3f(color[0], color[1], color[2]))}});
        vt.primvars.push_back({TfToken("tint"), {TfToken("constant"), VtValue(GfVec3f(color[0], color[1], color[2]))}});
        vt.subdivisionScheme = TfToken(std::string(scheme));
        vt.orientation = TfToken("leftHanded");

        prism::mesh::SubdivisionSettings settings;
        const auto a = prism::mesh::ConvertMesh(direct, settings);
        const auto b = prism::usd::ConvertMesh(vt, settings);
        Expect(a.mesh && b.mesh, "both sources convert");
        if (a.mesh && b.mesh) {
            Expect(SameMesh(*a.mesh, *b.mesh), "std and Vt sources give the same mesh");
            Expect(a.info.geometryHash == b.info.geometryHash, "same geometry hash");
            Expect(a.mesh->primvarStride == 4, "named primvars kept (weight, tint; not displayColor)");
        }
        Expect(a.info.subdivisionLevel == b.info.subdivisionLevel, "same subdivision level");
    }

    // Views of Vt values: the types the Vt-based conversion read.
    using Type = prism::mesh::PrimvarView::Type;
    Expect(prism::usd::View(VtValue()).type == Type::Empty, "empty value");
    Expect(prism::usd::View(VtValue(GfVec3d(1, 2, 3))).type == Type::Unsupported, "single GfVec3d unsupported");
    Expect(prism::usd::View(VtValue(VtVec2iArray(2))).type == Type::Unsupported, "int vector arrays unsupported");
    Expect(prism::usd::View(VtValue(GfVec4f(1, 2, 3, 4))).components == 4, "single GfVec4f");
    std::vector<float> floats;
    int components = 0;
    Expect(prism::usd::PrimvarFloats(VtValue(VtIntArray{3, -4}), &floats, &components) && components == 1 &&
               floats == std::vector<float>{3.f, -4.f}, "int array to floats");
    Expect(prism::usd::PrimvarFloats(VtValue(2.5), &floats, &components) && floats == std::vector<float>{2.5f},
           "double to floats");
    Expect(!prism::usd::PrimvarFloats(VtValue(VtFloatArray()), &floats, &components), "empty array has no floats");
    // Unsupported points are reported, missing ones too.
    prism::usd::UsdMeshSource bad;
    bad.faceVertexCounts = {3};
    bad.faceVertexIndices = {0, 1, 2};
    bad.points.value = VtValue(GfVec3f(0.f));
    auto conversion = prism::usd::ConvertMesh(bad, {});
    Expect(conversion.info.unsupportedPoints && !conversion.info.missingPoints, "single point value unsupported");
    bad.points.value = VtValue();
    conversion = prism::usd::ConvertMesh(bad, {});
    Expect(conversion.info.missingPoints && !conversion.info.unsupportedPoints, "missing points");

    if (failures) return 1;
    std::cout << "mesh sources agree\n";
    return 0;
}
