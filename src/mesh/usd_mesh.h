#pragma once

// First: windows.h, included by later headers, defines `interface`, a
// token of the Hydra material network schema.
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/utils.h>

// STD Lib deps
#include <iostream>
#include <fstream>
#include <atomic>
#include <OpenImageIO/hash.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <array>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <mutex>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <tbb/parallel_for.h>

// Open USD Deps
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/imaging/geomUtil/capsuleMeshGenerator.h>
#include <pxr/imaging/geomUtil/coneMeshGenerator.h>
#include <pxr/imaging/geomUtil/cuboidMeshGenerator.h>
#include <pxr/imaging/geomUtil/cylinderMeshGenerator.h>
#include <pxr/imaging/geomUtil/planeMeshGenerator.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/stringUtils.h>

// Hydra / hdMtlx deps for UsdShade -> MaterialX recovery
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/usdImaging/usdImaging/materialParamUtils.h>

// MaterialX deps
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include "../materialx/captured_material.h"

PXR_NAMESPACE_USING_DIRECTIVE
namespace mx = MaterialX;

// internal deps
#include "../geometry.h"
#include "../hydra/fallback_material.h"
#include "../mdl_material.h"
#include "../materialx/usd_network.h"
#include "../materialx/material_service.h"
#include "../load_profile.h"
#include "../materialx/prepared_template.h"
#include "../materialx/cached_document.h"
#include "../materialx/preview_surface.h"
#include "../render_defaults.h"
#include "mesh_conversion.h"

// ============================================================================
// MaterialX material recovery from USD stages
// ============================================================================

// The material bound for the first of `purposes` with a binding (the empty
// token is allPurpose), as Hydra resolves UsdRenderSettings
// materialBindingPurposes. Empty `purposes`: full, then allPurpose.
inline UsdShadeMaterial ComputeBoundMaterialFor(UsdPrim const& prim,
                                                std::vector<TfToken> const& purposes)
{
    static const std::vector<TfToken> defaults{UsdShadeTokens->full, UsdShadeTokens->allPurpose};
    for (TfToken const& purpose : purposes.empty() ? defaults : purposes) {
        UsdRelationship binding;
        UsdShadeMaterial material = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(purpose, &binding);
        if (!material) continue;
        if (purpose.IsEmpty()) return material;
        // ComputeBoundMaterial falls back to allPurpose bindings; here only
        // bindings of this purpose count (material:binding:<purpose> or
        // material:binding:collection:<purpose>:<name>).
        const std::vector<std::string> parts = TfStringSplit(binding.GetName(), ":");
        const bool collection = parts.size() > 2 && parts[2] == "collection";
        const std::string bound = collection ? (parts.size() == 5 ? parts[3] : std::string())
                                             : (parts.size() == 3 ? parts[2] : std::string());
        if (bound == purpose.GetString()) return material;
    }
    return UsdShadeMaterial(UsdPrim());
}

// The calling thread's logger shows "mdl" debug output (PRISM_MDL_VERBOSE).
inline bool UsdMaterialVerbose() { return prism::logging::Debug("mdl"); }

/// A bound material prim as the loader compiles it: its composed UsdShade
/// network as a Hydra material data source (which the material service of
/// the Hydra delegate compiles), or a .mtlx file referenced into the stage
/// that has no such network.
struct UsdMaterialSource {
    HdContainerDataSourceHandle dataSource; // prim data source with a `material` schema
    std::string mtlxFilePath;               // otherwise: the referenced .mtlx file
    std::string materialName;               // and its material node
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Check if a material prim's subtree was composed from a .mtlx file
/// reference. If so, return the resolved file path.
static std::optional<std::string>
FindReferencedMtlxFile(const UsdPrim& materialPrim)
{
    auto primSpec = materialPrim.GetPrimStack();
    for (const auto& spec : primSpec) {
        SdfReferenceListOp refs;
        if (spec->HasField(SdfFieldKeys->References, &refs)) {

            auto processRefs = [&](const std::vector<SdfReference>& refList) -> std::optional<std::string> {
                for (const SdfReference& ref : refList) {
                    const std::string& assetPath = ref.GetAssetPath();
                    if (assetPath.ends_with(".mtlx")) {
                        SdfLayerHandle layer = spec->GetLayer();
                        if (layer) {
                            std::string resolved = layer->ComputeAbsolutePath(assetPath);
                            if (!resolved.empty()) return resolved;
                        }
                        return assetPath;
                    }
                }
                return std::nullopt;
            };

            if (auto r = processRefs(refs.GetExplicitItems()))  return r;
            if (auto r = processRefs(refs.GetPrependedItems())) return r;
            if (auto r = processRefs(refs.GetAppendedItems()))  return r;
        }
    }
    return std::nullopt;
}

/// Walk up the prim's ancestor chain looking for a .mtlx reference.
/// The reference is typically authored on a container Scope above the
/// individual material prims (e.g. /Pawn/Materials rather than
/// /Pawn/Materials/M_Pawn_Body_B).
static std::optional<std::string>
FindReferencedMtlxFileInAncestors(const UsdPrim& prim)
{
    UsdPrim current = prim;
    while (current.IsValid()) {
        if (auto result = FindReferencedMtlxFile(current))
            return result;
        current = current.GetParent();
    }
    return std::nullopt;
}

/// Check whether a material prim has MaterialX-originated UsdShade nodes
/// by looking at the "mtlx" render context surface output.
static bool
HasMtlxSurfaceOutput(const UsdShadeMaterial& material)
{
    const TfTokenVector contextVector = { TfToken("mtlx") };
    UsdShadeShader surfaceSource = material.ComputeSurfaceSource(contextVector);
    return static_cast<bool>(surfaceSource);
}

// ---------------------------------------------------------------------------
// Collection: the materials bound to supported geometry
// ---------------------------------------------------------------------------

// The material prim data source of a composed UsdShade network: the network
// as UsdImaging builds it (surface and displacement terminals), under the
// mtlx render context, which the material service reads first.
inline HdContainerDataSourceHandle
UsdMaterialDataSource(const UsdShadeMaterial& material, UsdTimeCode time)
{
    const TfToken mtlxContext("mtlx");
    const TfTokenVector contexts{mtlxContext};
    const UsdShadeShader surface = material.ComputeSurfaceSource(contexts);
    if (!surface) return nullptr;
    HdMaterialNetworkMap networkMap;
    UsdImagingBuildHdMaterialNetworkFromTerminal(surface.GetPrim(),
        HdMaterialTerminalTokens->surface, contexts, contexts, &networkMap, time);
    // Displacement, which the material service merges into the material
    // (AddDisplacementShader) and geometry is displaced by at load time.
    if (const UsdShadeShader displacement = material.ComputeDisplacementSource(contexts))
        UsdImagingBuildHdMaterialNetworkFromTerminal(displacement.GetPrim(),
            HdMaterialTerminalTokens->displacement, contexts, contexts, &networkMap, time);
    const auto terminal = networkMap.map.find(HdMaterialTerminalTokens->surface);
    if (terminal == networkMap.map.end() || terminal->second.nodes.empty()) return nullptr;
    const HdDataSourceBaseHandle network =
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(networkMap);
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,
        HdMaterialSchema::BuildRetained(1, &mtlxContext, &network));
}

static std::map<SdfPath, UsdMaterialSource>
CollectBoundMaterials(const UsdStageRefPtr& stage,
                      std::vector<TfToken> const& bindingPurposes = {},
                      UsdTimeCode time = UsdTimeCode::Default())
{
    std::map<SdfPath, UsdMaterialSource> result;

    // Discover bindings on supported geometry first. Instance proxies and
    // point-instancer prototype subtrees remain included; no camera/frustum
    // test is involved, so offscreen shadow/reflection materials stay live.
    std::set<SdfPath> required;
    for (const UsdPrim& geometry : UsdPrimRange(stage->GetPseudoRoot(), UsdTraverseInstanceProxies())) {
        if (!geometry.IsA<UsdGeomMesh>() && !geometry.IsA<UsdGeomSphere>() && !geometry.IsA<UsdGeomCube>() &&
            !geometry.IsA<UsdGeomCylinder>() && !geometry.IsA<UsdGeomCone>() && !geometry.IsA<UsdGeomCapsule>() &&
            !geometry.IsA<UsdGeomPlane>() && !geometry.IsA<UsdGeomSubset>()) continue;
        if (auto material = ComputeBoundMaterialFor(geometry, bindingPurposes))
            required.insert(material.GetPath());
    }
    for (const SdfPath& materialPath : required) {
        const UsdPrim prim = stage->GetPrimAtPath(materialPath);
        UsdShadeMaterial material(prim);
        if (!material) continue;

        // The composed UsdShade network, as the Hydra delegate reads it; .mtlx
        // files referenced into the stage arrive this way too (usdMtlx).
        if (HasMtlxSurfaceOutput(material)) {
            if (auto source = UsdMaterialDataSource(material, time)) {
                if (UsdMaterialVerbose())
                    std::cout << "[MtlxCollect] UsdShade material: " << materialPath << "\n";
                result[materialPath].dataSource = std::move(source);
                continue;
            }
        }

        // A .mtlx file referenced into the stage, read directly.
        if (auto mtlxFile = FindReferencedMtlxFileInAncestors(prim)) {
            UsdMaterialSource source;
            source.mtlxFilePath = *mtlxFile;
            source.materialName = materialPath.GetName();
            if (UsdMaterialVerbose()) std::cout << "[MtlxCollect] Found referenced .mtlx: "
                      << source.mtlxFilePath << " at " << materialPath << "\n";
            result[materialPath] = std::move(source);
        }
    }
    return result;
}

// ============================================================================
// Mesh sources: UsdGeomMesh and implicit surfaces as the shared conversion
// (src/mesh/mesh_conversion.h) reads them, the way UsdImaging presents them
// to the Hydra delegate.
// ============================================================================

namespace prism::usd_detail {

inline prism::mesh::PrimvarData ReadPrimvar(pxr::UsdGeomPrimvar const& primvar, pxr::UsdTimeCode time)
{
    prism::mesh::PrimvarData result;
    if (!primvar) return result;
    pxr::VtValue value;
    if (!primvar.ComputeFlattened(&value, time)) return result;
    result.value = std::move(value);
    result.interpolation = primvar.GetInterpolation();
    return result;
}

// The named primvars of a gprim with the constant primvars it inherits
// (UsdImaging flattens inherited primvars into each prim); points, normals
// and namespaced primvars are not among them.
inline void ReadNamedPrimvars(pxr::UsdPrim const& prim, pxr::UsdTimeCode time, prism::mesh::MeshSource* source)
{
    static const std::unordered_set<std::string> geometry = {
        "points", "normals", "widths", "velocities", "accelerations", "angularVelocities"};
    for (pxr::UsdGeomPrimvar const& primvar : pxr::UsdGeomPrimvarsAPI(prim).FindPrimvarsWithInheritance()) {
        const pxr::TfToken name = primvar.GetPrimvarName();
        const std::string& text = name.GetString();
        if (geometry.count(text) || text.find(':') != std::string::npos || !primvar.HasValue()) continue;
        prism::mesh::PrimvarData data = ReadPrimvar(primvar, time);
        if (!data.value.IsEmpty()) source->primvars.emplace_back(name, std::move(data));
    }
}

inline void ReadSurfacePrimvars(pxr::UsdPrim const& prim, pxr::UsdTimeCode time, prism::mesh::MeshSource* source)
{
    pxr::UsdGeomPrimvarsAPI primvars(prim);
    pxr::UsdGeomPrimvar st = primvars.GetPrimvar(pxr::TfToken("st"));
    pxr::UsdGeomPrimvar uv = primvars.GetPrimvar(pxr::TfToken("uv"));
    const bool hasSt = st && st.HasValue(), hasUv = uv && uv.HasValue();
    source->multipleUvSets = hasSt && hasUv;
    if (hasSt) source->uvs = ReadPrimvar(st, time);
    else if (hasUv) source->uvs = ReadPrimvar(uv, time);
    ReadNamedPrimvars(prim, time, source);
}

// A UsdGeomMesh at a time; skinnedPoints replace its points (UsdSkel).
inline std::optional<prism::mesh::MeshSource> MeshSourceFromUsd(pxr::UsdGeomMesh const& mesh, pxr::UsdTimeCode time,
                                                                 pxr::VtVec3fArray const* skinnedPoints = nullptr)
{
    prism::mesh::MeshSource source;
    if (!mesh.GetFaceVertexCountsAttr().Get(&source.faceVertexCounts, time) ||
        !mesh.GetFaceVertexIndicesAttr().Get(&source.faceVertexIndices, time))
        return std::nullopt;
    mesh.GetHoleIndicesAttr().Get(&source.holeIndices, time);
    if (skinnedPoints) {
        source.points.value = pxr::VtValue(*skinnedPoints);
    } else {
        pxr::VtValue points;
        if (mesh.GetPointsAttr().Get(&points, time)) source.points.value = std::move(points);
    }
    source.points.interpolation = pxr::UsdGeomTokens->vertex;
    // primvars:normals takes precedence over normals (UsdGeomPointBased).
    pxr::UsdGeomPrimvar normals = pxr::UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(pxr::UsdGeomTokens->normals);
    if (normals && normals.HasValue()) {
        source.normals = ReadPrimvar(normals, time);
    } else {
        pxr::VtValue value;
        if (mesh.GetNormalsAttr().Get(&value, time)) {
            source.normals.value = std::move(value);
            source.normals.interpolation = mesh.GetNormalsInterpolation();
        }
    }
    ReadSurfacePrimvars(mesh.GetPrim(), time, &source);
    mesh.GetSubdivisionSchemeAttr().Get(&source.subdivisionScheme, time);
    mesh.GetOrientationAttr().Get(&source.orientation, time);
    bool doubleSided = false;
    mesh.GetDoubleSidedAttr().Get(&doubleSided, time);
    source.doubleSided = doubleSided;
    mesh.GetCreaseIndicesAttr().Get(&source.creaseIndices, time);
    mesh.GetCreaseLengthsAttr().Get(&source.creaseLengths, time);
    mesh.GetCreaseSharpnessesAttr().Get(&source.creaseSharpnesses, time);
    mesh.GetCornerIndicesAttr().Get(&source.cornerIndices, time);
    mesh.GetCornerSharpnessesAttr().Get(&source.cornerSharpnesses, time);
    mesh.GetInterpolateBoundaryAttr().Get(&source.interpolateBoundary, time);
    mesh.GetFaceVaryingLinearInterpolationAttr().Get(&source.faceVaryingLinearInterpolation, time);
    mesh.GetTriangleSubdivisionRuleAttr().Get(&source.triangleSubdivisionRule, time);
    source.motionBlur = mesh.GetPointsAttr().GetNumTimeSamples() > 1;
    return source;
}

// The basis that turns the Z-aligned generated shapes to their axis
// (HdsiImplicitSurfaceSceneIndex).
inline pxr::GfMatrix4d ImplicitBasis(pxr::TfToken const& axis)
{
    pxr::GfVec4d u, v, spine;
    if (axis == pxr::UsdGeomTokens->x) {
        u = pxr::GfVec4d::YAxis(); v = pxr::GfVec4d::ZAxis(); spine = pxr::GfVec4d::XAxis();
    } else if (axis == pxr::UsdGeomTokens->y) {
        u = pxr::GfVec4d::ZAxis(); v = pxr::GfVec4d::XAxis(); spine = pxr::GfVec4d::YAxis();
    } else {
        u = pxr::GfVec4d::XAxis(); v = pxr::GfVec4d::YAxis(); spine = pxr::GfVec4d::ZAxis();
    }
    pxr::GfMatrix4d basis;
    basis.SetRow(0, u); basis.SetRow(1, v); basis.SetRow(2, spine); basis.SetRow(3, pxr::GfVec4d::WAxis());
    return basis;
}

inline bool IsImplicitSurface(pxr::UsdPrim const& prim)
{
    return prim.IsA<pxr::UsdGeomCube>() || prim.IsA<pxr::UsdGeomCylinder>() || prim.IsA<pxr::UsdGeomCone>() ||
        prim.IsA<pxr::UsdGeomCapsule>() || prim.IsA<pxr::UsdGeomPlane>();
}

// Cubes, cylinders, cones, capsules and planes as the meshes Hydra makes of
// them (HdsiImplicitSurfaceSceneIndex, GeomUtil generators).
inline std::optional<prism::mesh::MeshSource> ImplicitMeshSource(pxr::UsdPrim const& prim, pxr::UsdTimeCode time)
{
    constexpr size_t kRadial = 10, kCapAxial = 4;
    pxr::PxOsdMeshTopology topology;
    pxr::VtVec3fArray points;
    auto axisOf = [&](pxr::UsdAttribute const& attribute) {
        pxr::TfToken axis = pxr::UsdGeomTokens->z;
        attribute.Get(&axis, time);
        return axis;
    };
    if (pxr::UsdGeomCube cube{prim}) {
        double size = 2.0;
        cube.GetSizeAttr().Get(&size, time);
        topology = pxr::GeomUtilCuboidMeshGenerator::GenerateTopology();
        points.resize(pxr::GeomUtilCuboidMeshGenerator::ComputeNumPoints());
        pxr::GeomUtilCuboidMeshGenerator::GeneratePoints(points.begin(), float(size), float(size), float(size));
    } else if (pxr::UsdGeomCylinder cylinder{prim}) {
        double radius = 1.0, height = 2.0;
        cylinder.GetRadiusAttr().Get(&radius, time);
        cylinder.GetHeightAttr().Get(&height, time);
        const pxr::GfMatrix4d basis = ImplicitBasis(axisOf(cylinder.GetAxisAttr()));
        topology = pxr::GeomUtilCylinderMeshGenerator::GenerateTopology(kRadial);
        points.resize(pxr::GeomUtilCylinderMeshGenerator::ComputeNumPoints(kRadial));
        pxr::GeomUtilCylinderMeshGenerator::GeneratePoints(points.begin(), kRadial, float(radius), float(height), &basis);
    } else if (pxr::UsdGeomCone cone{prim}) {
        double radius = 1.0, height = 2.0;
        cone.GetRadiusAttr().Get(&radius, time);
        cone.GetHeightAttr().Get(&height, time);
        const pxr::GfMatrix4d basis = ImplicitBasis(axisOf(cone.GetAxisAttr()));
        topology = pxr::GeomUtilConeMeshGenerator::GenerateTopology(kRadial);
        points.resize(pxr::GeomUtilConeMeshGenerator::ComputeNumPoints(kRadial));
        pxr::GeomUtilConeMeshGenerator::GeneratePoints(points.begin(), kRadial, float(radius), float(height), &basis);
    } else if (pxr::UsdGeomCapsule capsule{prim}) {
        double radius = 0.5, height = 1.0;
        capsule.GetRadiusAttr().Get(&radius, time);
        capsule.GetHeightAttr().Get(&height, time);
        const pxr::GfMatrix4d basis = ImplicitBasis(axisOf(capsule.GetAxisAttr()));
        topology = pxr::GeomUtilCapsuleMeshGenerator::GenerateTopology(kRadial, kCapAxial);
        points.resize(pxr::GeomUtilCapsuleMeshGenerator::ComputeNumPoints(kRadial, kCapAxial));
        pxr::GeomUtilCapsuleMeshGenerator::GeneratePoints(points.begin(), kRadial, kCapAxial, float(radius),
                                                          float(height), &basis);
    } else if (pxr::UsdGeomPlane plane{prim}) {
        double width = 2.0, length = 2.0;
        plane.GetWidthAttr().Get(&width, time);
        plane.GetLengthAttr().Get(&length, time);
        const pxr::GfMatrix4d basis = ImplicitBasis(axisOf(plane.GetAxisAttr()));
        topology = pxr::GeomUtilPlaneMeshGenerator::GenerateTopology();
        points.resize(pxr::GeomUtilPlaneMeshGenerator::ComputeNumPoints());
        pxr::GeomUtilPlaneMeshGenerator::GeneratePoints(points.begin(), float(width), float(length), &basis);
    } else {
        return std::nullopt;
    }
    prism::mesh::MeshSource source;
    source.faceVertexCounts = topology.GetFaceVertexCounts();
    source.faceVertexIndices = topology.GetFaceVertexIndices();
    source.holeIndices = topology.GetHoleIndices();
    source.subdivisionScheme = topology.GetScheme();
    source.orientation = topology.GetOrientation();
    source.points.value = pxr::VtValue(std::move(points));
    source.points.interpolation = pxr::UsdGeomTokens->vertex;
    ReadSurfacePrimvars(prim, time, &source);
    bool doubleSided = false;
    pxr::UsdGeomGprim(prim).GetDoubleSidedAttr().Get(&doubleSided, time);
    source.doubleSided = doubleSided;
    return source;
}

inline pxr::VtVec3fArray FloatPoints(pxr::VtValue const& value)
{
    if (value.IsHolding<pxr::VtVec3fArray>()) return value.UncheckedGet<pxr::VtVec3fArray>();
    pxr::VtVec3fArray result;
    if (value.IsHolding<pxr::VtVec3dArray>())
        for (auto const& p : value.UncheckedGet<pxr::VtVec3dArray>()) result.push_back(pxr::GfVec3f(p));
    return result;
}

template<class Vec>
inline pxr::VtArray<Vec> FloatVectors(pxr::VtValue const& value)
{
    using Double = std::conditional_t<std::is_same_v<Vec, pxr::GfVec2f>, pxr::GfVec2d, pxr::GfVec3d>;
    if (value.IsHolding<pxr::VtArray<Vec>>()) return value.UncheckedGet<pxr::VtArray<Vec>>();
    pxr::VtArray<Vec> result;
    if (value.IsHolding<pxr::VtArray<Double>>())
        for (auto const& v : value.UncheckedGet<pxr::VtArray<Double>>()) result.push_back(Vec(v));
    return result;
}

/// Transform and normalize a normal vector.
inline Float3 TransformNormal(const pxr::GfVec3f& n, const pxr::GfMatrix4d& normalMatrix, bool flip)
{
    pxr::GfVec3d nd(n[0], n[1], n[2]);
    pxr::GfVec3d tn = normalMatrix.TransformDir(nd);
    if (flip) tn = -tn;
    double len = tn.GetLength();
    if (len > 0.0) tn /= len;
    return Float3(static_cast<float>(tn[0]), static_cast<float>(tn[1]), static_cast<float>(tn[2]));
}

// Fan triangulation of a face given its corner vertex indices; leftHanded
// faces swap the winding.
inline void Triangulate(std::vector<unsigned int>& out, const std::vector<unsigned int>& corners, bool leftHanded)
{
    const int count = static_cast<int>(corners.size());
    for (int i = 1; i + 1 < count; ++i) {
        out.push_back(corners[0]);
        out.push_back(corners[leftHanded ? i + 1 : i]);
        out.push_back(corners[leftHanded ? i : i + 1]);
    }
}

// The standalone loader's polygon conversion of a mesh that does not refine:
// fan triangulation, a vertex per point unless a primvar varies per face or
// face corner (then one per corner). Positions and normals are transformed by
// `world` (identity: prototype space). Normals follow Hydra's conversion:
// generated ones face along the authored winding (leftHanded flips them,
// authored normals too), and a mirroring transform turns them with the
// triangles (SceneManager flips placed prototypes the same way). Returns the
// mesh's coarse face per triangle, or nothing when no triangle is left.
inline bool BuildPolygonMesh(prism::mesh::MeshSource const& source, pxr::GfMatrix4d const& world,
                             Mesh& out, std::vector<int>& coarseFaces)
{
    using pxr::UsdGeomTokens;
    const pxr::VtVec3fArray points = FloatPoints(source.points.value);
    pxr::VtIntArray const& counts = source.faceVertexCounts;
    pxr::VtIntArray const& indices = source.faceVertexIndices;
    if (points.empty() || counts.empty()) return false;
    const bool leftHanded = source.orientation == pxr::TfToken("leftHanded");
    const pxr::GfMatrix4d normalMatrix = world.GetInverse().GetTranspose();
    const bool mirrored = world.GetDeterminant() < 0.0;
    std::unordered_set<int> holes(source.holeIndices.begin(), source.holeIndices.end());
    // Faces with a valid winding: three corners or more, inside the arrays.
    std::vector<uint8_t> valid(counts.size(), 0);
    {
        size_t corner = 0;
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            bool ok = count >= 3 && corner + size_t(count) <= indices.size() && !holes.count(int(face));
            for (int k = 0; ok && k < count; ++k)
                ok = indices[corner + k] >= 0 && size_t(indices[corner + k]) < points.size();
            valid[face] = ok;
            corner += size_t(std::max(0, count));
        }
    }
    const std::vector<prism::mesh::NamedPrimvar> extras =
        prism::mesh::NamedPrimvars(source, points.size(), counts.size(), indices.size());
    const pxr::VtVec2fArray uvs = FloatVectors<pxr::GfVec2f>(source.uvs.value);
    const pxr::VtVec3fArray normals = FloatVectors<pxr::GfVec3f>(source.normals.value);
    const pxr::TfToken& uvInterpolation = source.uvs.interpolation;
    const pxr::TfToken& normalInterpolation = source.normals.interpolation;
    auto perCorner = [](pxr::TfToken const& interpolation) {
        return interpolation == UsdGeomTokens->faceVarying || interpolation == UsdGeomTokens->uniform;
    };
    bool expand = (!uvs.empty() && perCorner(uvInterpolation)) ||
        (!normals.empty() && perCorner(normalInterpolation));
    for (auto const& extra : extras) expand |= perCorner(extra.interpolation);

    // Area-weighted per-point normals of the authored winding.
    std::vector<pxr::GfVec3f> generated(points.size(), pxr::GfVec3f(0.f));
    {
        size_t corner = 0;
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            if (valid[face]) {
                const int i0 = indices[corner], i1 = indices[corner + 1], i2 = indices[corner + 2];
                pxr::GfVec3f fn = (points[i1] - points[i0]) ^ (points[i2] - points[i0]);
                if (leftHanded) fn = -fn;
                for (int k = 0; k < count; ++k) generated[indices[corner + k]] += fn;
            }
            corner += size_t(std::max(0, count));
        }
        for (auto& n : generated) {
            const float length = n.GetLength();
            n = length > 0.f ? n / length : pxr::GfVec3f(0.f, 1.f, 0.f);
        }
    }
    // The shading normal at a corner: authored where it is usable (negated
    // for leftHanded meshes), otherwise generated.
    auto normalAt = [&](size_t point, size_t corner, size_t face) {
        const size_t index = prism::mesh::PrimvarIndex(normalInterpolation, point, corner, face);
        if (index < normals.size()) {
            const pxr::GfVec3f& n = normals[index];
            const float lengthSq = n.GetLengthSq();
            if (lengthSq > 1e-20f && std::isfinite(lengthSq))
                return TransformNormal(leftHanded ? -n : n, normalMatrix, mirrored);
        }
        return TransformNormal(generated[point], normalMatrix, mirrored);
    };
    auto uvAt = [&](size_t point, size_t corner, size_t face) {
        const size_t index = prism::mesh::PrimvarIndex(uvInterpolation, point, corner, face);
        return index < uvs.size() ? Float2(uvs[index][0], uvs[index][1]) : Float2(0.f, 0.f);
    };
    int stride = 0;
    for (auto const& extra : extras) {
        out.primvars.push_back({extra.name, extra.components, stride});
        stride += extra.components;
    }
    out.primvarStride = stride;
    auto appendExtras = [&](size_t point, size_t corner, size_t face) {
        for (auto const& extra : extras) {
            const size_t n = size_t(extra.components);
            const size_t element = extra.interpolation == UsdGeomTokens->constant ? 0
                : prism::mesh::PrimvarIndex(extra.interpolation, point, corner, face);
            out.primvarValues.insert(out.primvarValues.end(), extra.values.begin() + element * n,
                                     extra.values.begin() + (element + 1) * n);
        }
    };
    auto position = [&](size_t point) {
        const pxr::GfVec3d p = world.Transform(pxr::GfVec3d(points[point][0], points[point][1], points[point][2]));
        return Float3(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    };

    std::vector<unsigned int> corners;
    size_t corner = 0;
    if (expand) {
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            if (valid[face]) {
                corners.clear();
                for (int k = 0; k < count; ++k) {
                    const size_t point = size_t(indices[corner + k]);
                    corners.push_back(static_cast<unsigned int>(out.vertices.size()));
                    out.vertices.push_back(position(point));
                    out.texcoords.push_back(uvAt(point, corner + k, face));
                    out.normals.push_back(normalAt(point, corner + k, face));
                    if (stride) appendExtras(point, corner + k, face);
                }
                const size_t before = out.indices.size();
                Triangulate(out.indices, corners, leftHanded);
                coarseFaces.insert(coarseFaces.end(), (out.indices.size() - before) / 3, int(face));
            }
            corner += size_t(std::max(0, count));
        }
    } else {
        out.vertices.reserve(points.size());
        for (size_t point = 0; point < points.size(); ++point) {
            out.vertices.push_back(position(point));
            out.normals.push_back(normalAt(point, 0, 0));
            out.texcoords.push_back(uvAt(point, 0, 0));
            if (stride) appendExtras(point, 0, 0);
        }
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            if (valid[face]) {
                corners.assign(indices.begin() + corner, indices.begin() + corner + count);
                const size_t before = out.indices.size();
                Triangulate(out.indices, corners, leftHanded);
                coarseFaces.insert(coarseFaces.end(), (out.indices.size() - before) / 3, int(face));
            }
            corner += size_t(std::max(0, count));
        }
    }
    out.single_sided_all = source.doubleSided ? !*source.doubleSided : true;
    return !out.indices.empty();
}

} // namespace prism::usd_detail

// ============================================================================
// USDMesh — loads geometry + detects and compiles MaterialX materials
//
// Every gprim becomes its own placement, as in the Hydra delegate: polygon
// meshes in world space; refined, displaced and instanced meshes (native
// instances and PointInstancers) as prototypes placed by Embree instances.
// ============================================================================

class USDMesh : public Mesh {
public:
    struct LoadOptions {
        // Material bindings resolve for the first of these purposes that has
        // one (UsdRenderSettings materialBindingPurposes; empty: full, then
        // allPurpose).
        std::vector<pxr::TfToken> bindingPurposes;
        pxr::UsdTimeCode time = pxr::UsdTimeCode::Default();
        // Purposes rendered besides default (UsdRenderSettings
        // includedPurposes); empty: render.
        std::vector<pxr::TfToken> includedPurposes;
        prism::mesh::SubdivisionSettings subdivision = DefaultSubdivision();
        // The renderer's options for the material service (compile policy,
        // workers, cache directory); its MaterialX paths are the
        // constructor's. Default: the environment's, as for the subdivision.
        prism::RendererOptions renderer = prism::RendererOptions::FromEnvironment();
        // The colour management materials convert into, the SSS fallback and
        // the cache directory (the renderer's). Without a colour management:
        // those of `renderer`.
        prism::MaterialSettings materials;
    };

    static prism::mesh::SubdivisionSettings DefaultSubdivision()
    {
        auto const& defaults = prism::defaults::FromEnvironment();
        prism::mesh::SubdivisionSettings result;
        result.level = defaults.subdivisionLevel;
        result.maxFaces = size_t(defaults.subdivisionMaxFaces);
        result.displacementLevel = defaults.displacementLevel;
        result.displacementMaxFaces = size_t(defaults.displacementMaxFaces);
        result.budgetFaces = size_t(defaults.subdivisionBudgetFaces);
        return result;
    }

    // What the render products and light linking need to know about an
    // attached placement (by Embree geometry id).
    struct PlacementIds {
        int primId = -1;
        int instanceId = -1;
        prism::mesh::SharedFaceTable coarseFaces; // elementId per triangle (empty: the triangle)
        pxr::SdfPath path;                        // the prim (an instance proxy for native instances)
    };
    std::unordered_map<unsigned, PlacementIds> attachedIds;

    struct AttachedSphere {
        unsigned int id;
        Sphere* geometry;
        pxr::GfMatrix4d world;
        Material* material;
        PlacementIds ids;
        std::shared_ptr<const InstancePrimvars> instancePrimvars;
    };
    struct AttachedMesh {
        unsigned int id;
        Mesh* mesh;
        bool instanced;               // an Embree instance of the prototype mesh at `world`
        pxr::GfMatrix4d world;
        Material* material;
        std::shared_ptr<const std::unordered_map<int, Material*>> triangleMaterials;
        PlacementIds ids;
        std::shared_ptr<const InstancePrimvars> instancePrimvars;
    };

    size_t sphereCount() const { return m_spherePlacements.size(); }
    size_t meshPlacementCount() const { return m_meshPlacements.size(); }
    // Placements that render (visible, of an included purpose).
    size_t renderablePlacementCount() const
    {
        size_t count = 0;
        for (auto const& p : m_meshPlacements) count += p.enabled;
        for (auto const& p : m_spherePlacements) count += p.enabled;
        return count;
    }

    // Spheres retain their local analytic shape. Affine Embree instances also
    // represent nonuniformly scaled spheres without baking or tessellating them.
    std::vector<AttachedSphere> attachSphereInstances(RTCDevice device, RTCScene target) {
        std::vector<AttachedSphere> result;
        for (const auto& placement : m_spherePlacements) {
            if (!placement.enabled) continue;
            auto& prototype = m_spherePrototypes[placement.radius];
            if (!prototype) prototype = std::make_unique<SpherePrototype>(device, placement.radius);
            RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
            rtcSetGeometryInstancedScene(instance, prototype->scene);
            float matrix[16];
            for (int c=0; c<4; ++c) for (int r=0; r<4; ++r)
                matrix[c*4+r] = float(placement.world[c][r]);
            rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matrix);
            rtcCommitGeometry(instance);
            const unsigned id = rtcAttachGeometry(target, instance);
            rtcReleaseGeometry(instance);
            result.push_back({id, &prototype->geometry, placement.world,
                material_override ? material_override : placement.material,
                placement.ids, placement.instancePrimvars});
        }
        return result;
    }

    // Mesh placements: world-space meshes are attached directly, prototypes
    // once each into a scene of their own that their placements instance.
    std::vector<AttachedMesh> attachMeshPlacements(RTCDevice device, RTCScene target) {
        std::vector<AttachedMesh> result;
        for (const auto& placement : m_meshPlacements) {
            if (!placement.enabled) continue;
            Shape& shape = *m_shapes[placement.shape];
            if (!shape.mesh) continue;
            Material* material = material_override ? material_override : placement.material;
            if (!shape.instanced) {
                const unsigned id = shape.mesh->setupEmbree(device, target);
                result.push_back({id, shape.mesh.get(), false, pxr::GfMatrix4d(1.0), material,
                    material_override ? nullptr : placement.triangleMaterials, placement.ids,
                    placement.instancePrimvars});
                continue;
            }
            if (!shape.scene) {
                shape.scene = rtcNewScene(device);
                shape.mesh->setupEmbree(device, shape.scene);
                rtcCommitScene(shape.scene);
            }
            RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
            rtcSetGeometryInstancedScene(instance, shape.scene);
            float matrix[16];
            // GfMatrix uses row vectors; Embree's column-major affine convention.
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                matrix[c * 4 + r] = float(placement.world[c][r]);
            rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matrix);
            rtcCommitGeometry(instance);
            const unsigned id = rtcAttachGeometry(target, instance);
            rtcReleaseGeometry(instance);
            result.push_back({id, shape.mesh.get(), true, placement.world, material,
                material_override ? nullptr : placement.triangleMaterials, placement.ids,
                placement.instancePrimvars});
        }
        return result;
    }

    // Basic constructor — no MDL/MaterialX support, uses displayColor fallback
    USDMesh(const std::filesystem::path& file_path)
        : USDMesh(file_path, nullptr, "", "")
    {}

    // Full constructor with MDL/MaterialX material support.
    USDMesh(const std::filesystem::path& file_path,
            mdl::Context* mdl_context,
            const std::string& mx_libraries_path,
            const std::string& mx_mdl_path,
            std::vector<pxr::TfToken> bindingPurposes = {})
        : USDMesh(file_path, mdl_context, mx_libraries_path, mx_mdl_path,
                  LoadOptions{std::move(bindingPurposes)})
    {}

    USDMesh(const std::filesystem::path& file_path,
            mdl::Context* mdl_context,
            const std::string& mx_libraries_path,
            const std::string& mx_mdl_path,
            LoadOptions options)
        : m_bindingPurposes(options.bindingPurposes), m_options(std::move(options))
    {
        pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(file_path.string());
        if (!stage) {
            std::cerr << "Failed to open USD file: " << file_path << "\n";
            return;
        }
        std::cout << "Successfully opened USD stage: " << file_path << "\n";
        load(stage, mdl_context, mx_libraries_path, mx_mdl_path);
    }

    USDMesh(pxr::UsdStageRefPtr const& stage,
            mdl::Context* mdl_context,
            const std::string& mx_libraries_path,
            const std::string& mx_mdl_path,
            LoadOptions options)
        : m_bindingPurposes(options.bindingPurposes), m_options(std::move(options))
    {
        if (stage) load(stage, mdl_context, mx_libraries_path, mx_mdl_path);
    }

    ~USDMesh() override
    {
        for (auto& shape : m_shapes) {
            if (shape->scene) rtcReleaseScene(shape->scene);
            if (shape->mesh && shape->mesh->geom) rtcReleaseGeometry(shape->mesh->geom);
        }
    }

private:
    void load(pxr::UsdStageRefPtr const& stage, mdl::Context* mdl_context,
              const std::string& mx_libraries_path, const std::string& mx_mdl_path)
    {
        // ── Phase 1: Compile the bound materials ───────────────────────────
        // Through the Hydra delegate's material service: conversions and
        // compilations on a worker pool, compatible materials sharing one
        // program through templates, class parameters folded (by
        // PRISM_MDL_COMPILE_MODE), and the process and on-disk caches.
        if (mdl_context && !mx_libraries_path.empty()) {
            ArResolverContextBinder materialResolver(
                stage->GetPathResolverContext());
            prism::RendererOptions serviceOptions = m_options.renderer;
            serviceOptions.materials.materialxLibraries = mx_libraries_path;
            serviceOptions.materials.materialxMdlPath = mx_mdl_path;
            prism::MaterialSettings settings = m_options.materials;
            if (!settings.color) {
                // No renderer's settings: those of the options.
                settings.color = prism::color::ColorManagement::Create(serviceOptions.color);
                settings.sssFallback = serviceOptions.materials.sssFallback;
                settings.generationCache = serviceOptions.materials.generationCache;
                settings.mdlResourceMode = serviceOptions.materials.mdlResourceMode;
            }
            if (settings.cacheDirectory.empty()) settings.cacheDirectory = prism::PersistentCacheRoot();
            prism::materials::MaterialService service(mdl_context, serviceOptions, settings);
            // Loads the MaterialX and MDL libraries while bindings are read.
            service.Prewarm();
            std::cout << "[USDMesh] Scanning for MaterialX materials...\n";
            auto sources = CollectBoundMaterials(stage, m_bindingPurposes, m_options.time);
            std::vector<prism::materials::MaterialService::Request> requests;
            for (auto const& [path, source] : sources)
                if (source.dataSource) requests.push_back({source.dataSource, path});
            service.Prefetch(requests);

            std::map<Material const*, int> ids;
            // Referenced .mtlx files by (canonical path, material name).
            std::map<std::pair<std::string, std::string>, std::shared_ptr<Material>> files;
            size_t failed = 0;
            for (auto const& [path, source] : sources) {
                std::shared_ptr<Material> material;
                if (source.dataSource) {
                    material = service.Resolve(source.dataSource, path);
                } else {
                    std::error_code error;
                    auto canonical = std::filesystem::weakly_canonical(source.mtlxFilePath, error);
                    auto& file = files[{(error ? std::filesystem::path(source.mtlxFilePath) : canonical)
                                            .generic_string(), source.materialName}];
                    if (!file) file = std::make_shared<MDLMaterial>(source.mtlxFilePath, *mdl_context,
                        mx_libraries_path, mx_mdl_path, settings, source.materialName);
                    material = file;
                }
                // As in the delegate: a material that failed to compile
                // renders magenta; one that could not be converted is unbound.
                if (auto* mdl = dynamic_cast<MDLMaterial*>(material.get()); !material || (mdl && !mdl->is_valid())) {
                    std::cerr << "[USDMesh] Failed to compile MaterialX material: " << path << "\n";
                    ++failed;
                    if (!material) continue;
                }
                auto [id, added] = ids.try_emplace(material.get(), int(m_owned_materials.size()));
                if (added) {
                    materials[id->second] = material.get();
                    m_owned_materials.push_back(std::move(material));
                }
                m_mdl_material_lookup[path] = id->second;
                if (UsdMaterialVerbose()) std::cout << "[USDMesh] MaterialX material '"
                          << path << "' -> mat_id " << id->second << "\n";
            }
            const auto stats = service.GetStats();
            std::cout << "[USDMesh] " << m_owned_materials.size() << " MaterialX material(s) for "
                      << m_mdl_material_lookup.size() << " bound USD material(s): "
                      << stats.compilations << " compiled (" << stats.foldedCompilations << " folded), "
                      << stats.templateCacheHits << " template bindings, " << failed << " failed\n";
            if (UsdMaterialVerbose() || prism::load_profile::Enabled())
                std::cout << "[USDMesh] material work: batch " << stats.batchWallSeconds
                          << " s; summed over workers: conversion " << stats.conversionSeconds
                          << " s (resources " << stats.resourceReadSeconds << " s), generation "
                          << stats.generationSeconds << " s, MDL compile " << stats.classCompilationSeconds
                          << " s, native JIT " << stats.nativeJitSeconds << " s\n";
        }

        // ── Phase 2: Process geometry ──────────────────────────────────────
        collectGeometry(stage);
        convertShapes();

        size_t triangles = 0, instanced = 0;
        for (auto const& placement : m_meshPlacements) {
            if (!placement.enabled || !m_shapes[placement.shape]->mesh) continue;
            triangles += m_shapes[placement.shape]->mesh->indices.size() / 3;
            instanced += m_shapes[placement.shape]->instanced;
        }
        std::cout << "Finished processing USD file:\n"
                  << "    Mesh placements: " << m_meshPlacements.size() << " (" << instanced
                  << " instanced), spheres: " << m_spherePlacements.size() << "\n"
                  << "    Total triangles: " << triangles << "\n"
                  << "    Total materials: " << materials.size() << "\n";
    }

    // ── Geometry ────────────────────────────────────────────────────────────

    // One mesh conversion: a world-space polygon mesh, or a prototype shared
    // by the placements of an instanced gprim (per displacing material).
    struct Shape {
        pxr::UsdPrim prim;
        std::optional<prism::mesh::MeshSource> source;
        bool worldSpace = false;            // a single placement baked in world space
        pxr::GfMatrix4d world{1.0};         // for worldSpace
        Material const* displacer = nullptr;
        // Results.
        bool instanced = false;             // placed by Embree instances
        std::unique_ptr<Mesh, prism::mesh::ConvertedMeshDeleter> mesh;
        prism::mesh::SharedFaceTable coarseFaces;
        prism::mesh::MeshConversionInfo info;
        RTCScene scene = nullptr;
        std::vector<size_t> refinementFaces;
        int level = 0;
    };
    struct MeshPlacement {
        size_t shape = 0;
        pxr::GfMatrix4d world{1.0};
        Material* material = nullptr;
        std::shared_ptr<const std::unordered_map<int, Material*>> triangleMaterials;
        std::shared_ptr<const std::unordered_map<int, Material*>> faceMaterials; // by coarse face
        bool enabled = true;
        PlacementIds ids;
        std::shared_ptr<const InstancePrimvars> instancePrimvars;
    };
    // An instance level of a placement: a PointInstancer (the instance's
    // index into its arrays) or a native instance (its instance prim).
    struct InstanceLevel {
        pxr::UsdPrim instancer;
        int sourceIndex = -1;
        bool native = false;
    };
    // A placement of a prim's local space in the world.
    struct Instance {
        pxr::GfMatrix4d toWorld{1.0};
        int instanceId = -1;
        pxr::SdfPath instancerPath;
        std::vector<InstanceLevel> chain;   // outer to inner
        // Native instancing: the prototype root and its instance proxy path.
        pxr::SdfPath prototypeRoot, proxyRoot;
        pxr::SdfPath proxyOf(pxr::SdfPath const& path) const
        {
            return prototypeRoot.IsEmpty() ? path : path.ReplacePrefix(prototypeRoot, proxyRoot);
        }
    };
    struct PointInstancerData {
        pxr::VtVec3fArray positions, scales;
        pxr::VtQuathArray orientations;
        pxr::VtQuatfArray orientationsf;
        pxr::VtIntArray protoIndices;
        std::vector<bool> mask;
        // Instance-rate primvars (vertex or varying interpolation).
        struct Column { std::string name; int components = 0; std::vector<float> values; };
        std::vector<Column> primvars;
    };

    pxr::GfMatrix4d localTransform(pxr::UsdPrim const& prim, bool* resets) const
    {
        *resets = false;
        pxr::UsdGeomXformable xformable(prim);
        if (!xformable) return pxr::GfMatrix4d(1.0);
        pxr::GfMatrix4d local(1.0);
        xformable.GetLocalTransformation(&local, resets, m_options.time);
        return local;
    }

    PointInstancerData const& instancerData(pxr::UsdGeomPointInstancer const& instancer)
    {
        auto [it, added] = m_instancerData.try_emplace(instancer.GetPath());
        if (!added) return it->second;
        PointInstancerData& data = it->second;
        const pxr::UsdTimeCode time = m_options.time;
        instancer.GetPositionsAttr().Get(&data.positions, time);
        instancer.GetScalesAttr().Get(&data.scales, time);
        instancer.GetOrientationsAttr().Get(&data.orientations, time);
        instancer.GetOrientationsfAttr().Get(&data.orientationsf, time);
        instancer.GetProtoIndicesAttr().Get(&data.protoIndices, time);
        // invisibleIds (and inactiveIds): the instances to skip, as Hydra's
        // instancer topology mask does.
        data.mask = instancer.ComputeMaskAtTime(time);
        for (pxr::UsdGeomPrimvar const& primvar : pxr::UsdGeomPrimvarsAPI(instancer.GetPrim()).GetPrimvarsWithValues()) {
            const pxr::TfToken interpolation = primvar.GetInterpolation();
            const std::string name = primvar.GetPrimvarName().GetString();
            if ((interpolation != pxr::UsdGeomTokens->vertex && interpolation != pxr::UsdGeomTokens->varying) ||
                name.find(':') != std::string::npos) continue;
            pxr::VtValue value;
            if (!primvar.ComputeFlattened(&value, time)) continue;
            PointInstancerData::Column column;
            column.name = name;
            if (!prism::mesh::PrimvarFloats(value, &column.values, &column.components) ||
                column.components < 1 || column.components > 4) continue;
            data.primvars.push_back(std::move(column));
        }
        return data;
    }

    // The instance transform of a PointInstancer's source index, composed as
    // the Hydra delegate composes its instancer primvars (scale, rotation,
    // then translation).
    static pxr::GfMatrix4d instanceTransform(PointInstancerData const& data, int index)
    {
        pxr::GfMatrix4d transform(1.0);
        const size_t i = size_t(index);
        if (i < data.positions.size()) {
            pxr::GfMatrix4d m(1.0); m.SetTranslate(pxr::GfVec3d(data.positions[i])); transform = m * transform;
        }
        bool hasRotation = false;
        pxr::GfMatrix4d rotation(1.0);
        if (i < data.orientationsf.size()) {
            rotation.SetRotate(pxr::GfQuatd(data.orientationsf[i])); hasRotation = true;
        } else if (i < data.orientations.size()) {
            rotation.SetRotate(pxr::GfQuatd(data.orientations[i])); hasRotation = true;
        }
        if (hasRotation) transform = rotation * transform;
        if (i < data.scales.size()) {
            pxr::GfMatrix4d m(1.0); m.SetScale(pxr::GfVec3d(data.scales[i])); transform = m * transform;
        }
        return transform;
    }

    // The placements of a prim's local space: one per instance of the
    // PointInstancer prototypes and native prototypes it lies in (nested
    // levels multiply). *instanced is false for prims outside prototypes.
    std::vector<Instance> const& placementsOf(pxr::UsdPrim const& prim, bool* instanced, int depth = 0)
    {
        static const std::vector<Instance> none;
        auto cached = m_placementCache.find(prim.GetPath());
        if (cached != m_placementCache.end()) {
            *instanced = cached->second.first;
            return cached->second.second;
        }
        auto& entry = m_placementCache[prim.GetPath()];
        entry.first = false;
        if (depth > 16) {
            std::cerr << "[USDMesh] instancing depth exceeds 16 at " << prim.GetPath() << "\n";
            return entry.second;
        }
        std::vector<Instance> result;
        for (pxr::UsdPrim root = prim; root && !root.IsPseudoRoot(); root = root.GetParent()) {
            auto roots = m_prototypeRoots.find(root.GetPath());
            if (roots != m_prototypeRoots.end()) {
                entry.first = true;
                // Prototype hierarchy transforms up to (and with) the root.
                pxr::GfMatrix4d toRoot(1.0);
                for (pxr::UsdPrim p = prim; p; p = p.GetParent()) {
                    bool resets = false;
                    toRoot *= localTransform(p, &resets);
                    if (resets || p == root) break;
                }
                for (auto const& [instancerPrim, prototypeIndex] : roots->second) {
                    pxr::UsdGeomPointInstancer instancer(instancerPrim);
                    PointInstancerData const& data = instancerData(instancer);
                    std::vector<int> sources;
                    for (size_t i = 0; i < data.protoIndices.size(); ++i)
                        if (data.protoIndices[i] == prototypeIndex && (i >= data.mask.size() || data.mask[i]))
                            sources.push_back(int(i));
                    bool parentInstanced = false;
                    std::vector<Instance> parents = placementsOf(instancerPrim, &parentInstanced, depth + 1);
                    if (!parentInstanced) {
                        Instance world;
                        world.toWorld = m_xformCache->GetLocalToWorldTransform(instancerPrim);
                        parents = {world};
                    }
                    for (Instance const& parent : parents) {
                        for (size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
                            Instance placement;
                            placement.toWorld = (toRoot * instanceTransform(data, sources[ordinal])) * parent.toWorld;
                            placement.instanceId = parentInstanced
                                ? parent.instanceId * int(sources.size()) + int(ordinal) : int(ordinal);
                            placement.instancerPath = instancerPrim.GetPath();
                            placement.chain = parent.chain;
                            placement.chain.push_back({instancerPrim, sources[ordinal], false});
                            placement.prototypeRoot = parent.prototypeRoot;
                            placement.proxyRoot = parent.proxyRoot;
                            result.push_back(std::move(placement));
                        }
                    }
                }
                break;
            }
            if (root.IsPrototype()) {
                entry.first = true;
                pxr::GfMatrix4d toRoot(1.0);
                for (pxr::UsdPrim p = prim; p && p != root; p = p.GetParent()) {
                    bool resets = false;
                    toRoot *= localTransform(p, &resets);
                    if (resets) break;
                }
                std::vector<pxr::UsdPrim> instances = root.GetInstances();
                std::sort(instances.begin(), instances.end(),
                          [](auto const& a, auto const& b) { return a.GetPath() < b.GetPath(); });
                for (size_t ordinal = 0; ordinal < instances.size(); ++ordinal) {
                    pxr::UsdPrim const& instance = instances[ordinal];
                    bool parentInstanced = false;
                    std::vector<Instance> parents = placementsOf(instance, &parentInstanced, depth + 1);
                    if (!parentInstanced) {
                        Instance world;
                        world.toWorld = m_xformCache->GetLocalToWorldTransform(instance);
                        parents = {world};
                    }
                    for (Instance const& parent : parents) {
                        Instance placement;
                        placement.toWorld = toRoot * parent.toWorld;
                        placement.instanceId = parentInstanced
                            ? parent.instanceId * int(instances.size()) + int(ordinal) : int(ordinal);
                        placement.instancerPath = root.GetPath();
                        placement.chain = parent.chain;
                        placement.prototypeRoot = root.GetPath();
                        placement.proxyRoot = parent.proxyOf(instance.GetPath());
                        // The native level's primvars are read from the proxy.
                        placement.chain.push_back({instance.GetStage()->GetPrimAtPath(placement.proxyRoot), -1, true});
                        result.push_back(std::move(placement));
                    }
                }
                break;
            }
        }
        entry.second = std::move(result);
        *instanced = entry.first;
        return entry.second;
    }

    // Instance-rate primvars of a placement for MDL scene data lookups:
    // every level contributes, inner levels over outer ones. PointInstancer
    // levels give their vertex/varying primvars at the instance's index;
    // native instances the constant primvars the instance inherits (the
    // primvars UsdImaging gives native instancers).
    std::shared_ptr<const InstancePrimvars> instancePrimvars(std::vector<InstanceLevel> const& chain)
    {
        if (chain.empty()) return nullptr;
        auto values = std::make_shared<InstancePrimvars>();
        auto set = [&](InstancePrimvars::Value value) {
            auto existing = std::find_if(values->values.begin(), values->values.end(),
                                         [&](auto const& v) { return v.name == value.name; });
            if (existing != values->values.end()) *existing = std::move(value);
            else values->values.push_back(std::move(value));
        };
        for (auto const& level : chain) {
            if (!level.instancer) continue;
            if (!level.native) {
                PointInstancerData const& data = instancerData(pxr::UsdGeomPointInstancer(level.instancer));
                for (auto const& column : data.primvars) {
                    const size_t n = column.values.size() / size_t(column.components);
                    if (level.sourceIndex < 0 || size_t(level.sourceIndex) >= n) continue;
                    InstancePrimvars::Value value;
                    value.name = column.name;
                    value.components = column.components;
                    std::copy_n(column.values.begin() + size_t(level.sourceIndex) * size_t(column.components),
                                column.components, value.value.begin());
                    set(std::move(value));
                }
                continue;
            }
            for (pxr::UsdGeomPrimvar const& primvar :
                 pxr::UsdGeomPrimvarsAPI(level.instancer).FindPrimvarsWithInheritance()) {
                const std::string name = primvar.GetPrimvarName().GetString();
                if (primvar.GetInterpolation() != pxr::UsdGeomTokens->constant ||
                    name.find(':') != std::string::npos) continue;
                pxr::VtValue value;
                if (!primvar.ComputeFlattened(&value, m_options.time)) continue;
                InstancePrimvars::Value entry;
                entry.name = name;
                std::vector<float> floats;
                if (!prism::mesh::PrimvarFloats(value, &floats, &entry.components) ||
                    entry.components < 1 || entry.components > 4) continue;
                std::copy_n(floats.begin(), entry.components, entry.value.begin());
                set(std::move(entry));
            }
        }
        if (values->values.empty()) return nullptr;
        return values;
    }

    // Whether a placement renders: visible (with every PointInstancer of its
    // chain) and of an included purpose.
    bool eligible(pxr::UsdPrim const& prim, std::vector<InstanceLevel> const& chain) const
    {
        pxr::UsdGeomImageable imageable(prim);
        if (!imageable) return true;
        if (imageable.ComputeVisibility(m_options.time) == pxr::UsdGeomTokens->invisible) return false;
        for (auto const& level : chain)
            if (!level.native && pxr::UsdGeomImageable(level.instancer).ComputeVisibility(m_options.time) ==
                    pxr::UsdGeomTokens->invisible) return false;
        const pxr::TfToken purpose = imageable.ComputePurpose();
        if (purpose == pxr::UsdGeomTokens->default_) return true;
        auto const& included = m_options.includedPurposes;
        if (included.empty()) return purpose == pxr::UsdGeomTokens->render;
        return std::find(included.begin(), included.end(), purpose) != included.end();
    }

    // The prim id of a (prim, instancer) pair: as the Hydra delegate numbers
    // the prims it places, in traversal order.
    int primId(pxr::SdfPath const& path, pxr::SdfPath const& instancer)
    {
        auto [it, added] = m_primIds.try_emplace({path, instancer}, int(m_primIds.size()));
        return it->second;
    }

    // The bound MaterialX material of a prim, or null.
    Material* boundMaterial(pxr::UsdPrim const& prim)
    {
        if (m_mdl_material_lookup.empty()) return nullptr;
        pxr::UsdShadeMaterial bound = ComputeBoundMaterialFor(prim, m_bindingPurposes);
        if (!bound) return nullptr;
        auto it = m_mdl_material_lookup.find(bound.GetPrim().GetPath());
        return it == m_mdl_material_lookup.end() ? nullptr : materials.at(it->second);
    }

    // The fallback material of a gprim without one: displayColor and
    // displayOpacity (inherited constant ones too), one per prim.
    Material* fallbackMaterial(pxr::UsdPrim const& prim)
    {
        auto existing = m_fallbackByPath.find(prim.GetPath());
        if (existing != m_fallbackByPath.end()) return materials.at(existing->second);
        // Without displayColor: the delegate's fallback grey.
        Color3 color(0.5f, 0.5f, 0.5f);
        float opacity = 1.0f;
        bool primvarColor = false, primvarOpacity = false;
        const bool meshLike = !prim.IsA<pxr::UsdGeomSphere>();
        pxr::UsdGeomPrimvarsAPI primvars(prim);
        if (pxr::UsdGeomPrimvar pv = primvars.FindPrimvarWithInheritance(pxr::TfToken("displayColor")); pv) {
            pxr::VtValue value;
            if (pv.ComputeFlattened(&value, m_options.time) && value.IsHolding<pxr::VtVec3fArray>() &&
                !value.UncheckedGet<pxr::VtVec3fArray>().empty()) {
                auto const& c = value.UncheckedGet<pxr::VtVec3fArray>().front();
                color = Color3(c[0], c[1], c[2]);
                primvarColor = pv.GetInterpolation() != pxr::UsdGeomTokens->constant && meshLike;
            }
        }
        if (pxr::UsdGeomPrimvar pv = primvars.FindPrimvarWithInheritance(pxr::TfToken("displayOpacity")); pv) {
            pxr::VtValue value;
            if (pv.ComputeFlattened(&value, m_options.time) && value.IsHolding<pxr::VtFloatArray>() &&
                !value.UncheckedGet<pxr::VtFloatArray>().empty()) {
                const bool constant = pv.GetInterpolation() == pxr::UsdGeomTokens->constant;
                // Spheres take a constant opacity only.
                if (constant || meshLike) {
                    opacity = value.UncheckedGet<pxr::VtFloatArray>().front();
                    primvarOpacity = !constant;
                }
            }
        }
        int mat_id = static_cast<int>(materials.size());
        auto texture = std::make_unique<ColorTexture>(color);
        std::unique_ptr<Diffuse> material;
        if (primvarColor || primvarOpacity || prism::hydra::NormalizeFallbackOpacity(opacity) < 1.0f)
            material = std::make_unique<prism::hydra::FallbackMaterial>(texture.get(), opacity, primvarColor,
                                                                        primvarOpacity);
        else
            material = std::make_unique<Diffuse>(texture.get());
        materials[mat_id] = material.get();
        m_owned_fallback_textures.push_back(std::move(texture));
        m_owned_fallback_materials.push_back(std::move(material));
        m_fallbackByPath.emplace(prim.GetPath(), mat_id);
        return materials.at(mat_id);
    }

    // GeomSubset material bindings of a mesh, by coarse face (faces outside
    // the subsets keep the mesh's material).
    std::shared_ptr<const std::unordered_map<int, Material*>> subsetMaterials(pxr::UsdPrim const& prim)
    {
        if (!prim.IsA<pxr::UsdGeomMesh>()) return nullptr;
        auto faces = std::make_shared<std::unordered_map<int, Material*>>();
        for (pxr::UsdGeomSubset const& subset : pxr::UsdGeomSubset::GetAllGeomSubsets(pxr::UsdGeomImageable(prim))) {
            pxr::TfToken elementType;
            subset.GetElementTypeAttr().Get(&elementType);
            if (elementType != pxr::UsdGeomTokens->face) continue;
            Material* material = boundMaterial(subset.GetPrim());
            if (!material) continue;
            pxr::VtIntArray indices;
            subset.GetIndicesAttr().Get(&indices, m_options.time);
            for (int face : indices) if (face >= 0) (*faces)[face] = material;
        }
        if (faces->empty()) return nullptr;
        return faces;
    }

    size_t shapeFor(pxr::UsdPrim const& prim, bool instanced, pxr::GfMatrix4d const& world,
                    Material const* displacer)
    {
        if (instanced) {
            auto key = std::make_pair(prim.GetPath(), displacer);
            auto found = m_shapeIndex.find(key);
            if (found != m_shapeIndex.end()) return found->second;
            m_shapeIndex.emplace(key, m_shapes.size());
        }
        auto shape = std::make_unique<Shape>();
        shape->prim = prim;
        shape->worldSpace = !instanced;
        shape->world = world;
        shape->displacer = displacer;
        if (prim.IsA<pxr::UsdGeomMesh>()) {
            shape->source = prism::usd_detail::MeshSourceFromUsd(pxr::UsdGeomMesh(prim), m_options.time,
                                                                 skinnedPoints(prim));
            if (!shape->source)
                std::cerr << "[USDMesh] Missing required attributes on " << prim.GetPath() << "\n";
        } else {
            shape->source = prism::usd_detail::ImplicitMeshSource(prim, m_options.time);
        }
        m_shapes.push_back(std::move(shape));
        return m_shapes.size() - 1;
    }

    // Skinned points of a mesh bound to a skeleton (UsdSkel), at the load
    // time; null for other meshes.
    pxr::VtVec3fArray const* skinnedPoints(pxr::UsdPrim const& prim)
    {
        auto found = m_skinnedPoints.find(prim.GetPath());
        return found == m_skinnedPoints.end() ? nullptr : &found->second;
    }

    // UsdSkel linear blend skinning of the meshes under each SkelRoot at the
    // load time (UsdSkelCache; the stage is not edited). Skinned points are
    // computed in skeleton space and brought into each mesh's own space, so
    // the mesh keeps its transform. Blend shapes are not applied.
    void skinMeshes(pxr::UsdStageRefPtr const& stage)
    {
        pxr::UsdSkelCache cache;
        bool warnedBlendShapes = false;
        const auto predicate = pxr::UsdTraverseInstanceProxies();
        for (pxr::UsdPrim const& prim : pxr::UsdPrimRange(stage->GetPseudoRoot(), predicate)) {
            if (!prim.IsA<pxr::UsdSkelRoot>()) continue;
            const pxr::UsdSkelRoot root(prim);
            cache.Populate(root, predicate);
            std::vector<pxr::UsdSkelBinding> bindings;
            cache.ComputeSkelBindings(root, &bindings, predicate);
            for (auto const& binding : bindings) {
                const pxr::UsdSkelSkeletonQuery skeleton = cache.GetSkelQuery(binding.GetSkeleton());
                pxr::VtMatrix4dArray transforms;
                if (!skeleton || !skeleton.ComputeSkinningTransforms(&transforms, m_options.time)) continue;
                const pxr::GfMatrix4d skeletonToWorld =
                    m_xformCache->GetLocalToWorldTransform(binding.GetSkeleton().GetPrim());
                for (pxr::UsdSkelSkinningQuery const& query : binding.GetSkinningTargets()) {
                    const pxr::UsdPrim target = query.GetPrim();
                    if (!target.IsA<pxr::UsdGeomMesh>() || !query.HasJointInfluences()) continue;
                    if (query.HasBlendShapes() && !warnedBlendShapes) {
                        std::cerr << "[USDMesh] UsdSkel blend shapes are not applied (first at "
                                  << target.GetPath() << ")\n";
                        warnedBlendShapes = true;
                    }
                    pxr::VtVec3fArray points;
                    if (!pxr::UsdGeomMesh(target).GetPointsAttr().Get(&points, m_options.time) ||
                        !query.ComputeSkinnedPoints(transforms, &points, m_options.time)) continue;
                    const pxr::GfMatrix4d skeletonToMesh =
                        skeletonToWorld * m_xformCache->GetLocalToWorldTransform(target).GetInverse();
                    for (auto& point : points) point = pxr::GfVec3f(skeletonToMesh.Transform(pxr::GfVec3d(point)));
                    m_skinnedPoints[target.GetPath()] = std::move(points);
                }
            }
        }
    }

    void addGprim(pxr::UsdPrim const& prim)
    {
        const bool isMesh = prim.IsA<pxr::UsdGeomMesh>();
        const bool isSphere = prim.IsA<pxr::UsdGeomSphere>();
        if (!isMesh && !isSphere && !prism::usd_detail::IsImplicitSurface(prim)) return;
        bool instanced = false;
        std::vector<Instance> placements = placementsOf(prim, &instanced);
        if (!instanced) {
            Instance world;
            world.toWorld = m_xformCache->GetLocalToWorldTransform(prim);
            placements = {world};
        }
        for (Instance const& placement : placements) {
            // Native instances resolve bindings, visibility and purpose
            // through the instance proxy of the prototype prim.
            pxr::UsdPrim proxy = placement.prototypeRoot.IsEmpty() ? prim
                : prim.GetStage()->GetPrimAtPath(placement.proxyOf(prim.GetPath()));
            if (!proxy) proxy = prim;
            PlacementIds ids;
            ids.primId = primId(prim.GetPath(), placement.instancerPath);
            ids.instanceId = placement.instanceId;
            ids.path = proxy.GetPath();
            // Hidden placements keep their prim id (the delegate numbers them
            // too) but are not converted.
            if (!eligible(proxy, placement.chain)) continue;
            const bool enabled = true;
            Material* material = boundMaterial(proxy);
            if (!material) material = fallbackMaterial(prim);
            if (isSphere) {
                captureSphere(pxr::UsdGeomSphere(prim), placement.toWorld, material, enabled, ids,
                              instancePrimvars(placement.chain));
                continue;
            }
            Material const* displacer = material->hasDisplacement() ? material : nullptr;
            MeshPlacement mesh;
            mesh.shape = shapeFor(prim, instanced, placement.toWorld, displacer);
            mesh.world = placement.toWorld;
            mesh.material = material;
            mesh.faceMaterials = subsetMaterials(proxy);
            mesh.enabled = enabled;
            mesh.ids = ids;
            mesh.instancePrimvars = instancePrimvars(placement.chain);
            m_meshPlacements.push_back(std::move(mesh));
        }
    }

    void collectGeometry(pxr::UsdStageRefPtr const& stage)
    {
        m_xformCache = std::make_unique<pxr::UsdGeomXformCache>(m_options.time);
        skinMeshes(stage);
        // PointInstancer prototype roots and the instancers listing them,
        // including instancers inside native prototypes.
        std::vector<pxr::UsdPrim> roots{stage->GetPseudoRoot()};
        for (pxr::UsdPrim const& prototype : stage->GetPrototypes()) roots.push_back(prototype);
        for (pxr::UsdPrim const& root : roots) {
            for (pxr::UsdPrim const& prim : pxr::UsdPrimRange(root)) {
                if (!prim.IsA<pxr::UsdGeomPointInstancer>()) continue;
                pxr::SdfPathVector targets;
                pxr::UsdGeomPointInstancer(prim).GetPrototypesRel().GetForwardedTargets(&targets);
                for (size_t i = 0; i < targets.size(); ++i)
                    m_prototypeRoots[targets[i]].emplace_back(prim, int(i));
            }
        }
        // Gprims in traversal order (the delegate numbers prims so), then the
        // prims of native prototypes.
        for (pxr::UsdPrim const& root : roots)
            for (pxr::UsdPrim const& prim : pxr::UsdPrimRange(root)) addGprim(prim);
    }

    // Refinement levels within the scene's budget, then the conversions:
    // shapes that refine or displace by the shared conversion (as
    // prototypes), the others by the polygon conversion.
    void convertShapes()
    {
        std::vector<std::vector<size_t>> faces(m_shapes.size());
        std::vector<bool> fixed(m_shapes.size(), false);
        for (size_t i = 0; i < m_shapes.size(); ++i) {
            Shape& shape = *m_shapes[i];
            if (!shape.source) continue;
            auto const& counts = shape.source->faceVertexCounts;
            faces[i] = prism::mesh::RefinementFaces(std::span<const int>(counts.cdata(), counts.size()),
                shape.source->subdivisionScheme, shape.displacer != nullptr, m_options.subdivision);
        }
        const std::vector<int> levels = prism::mesh::BudgetSubdivisionLevels(
            faces, fixed, m_options.subdivision.budgetFaces, nullptr, nullptr);
        std::atomic<bool> controlHulls{false};
        tbb::parallel_for(size_t(0), m_shapes.size(), [&](size_t i) {
            Shape& shape = *m_shapes[i];
            if (!shape.source) return;
            shape.level = levels[i];
            const pxr::TfToken scheme = prism::mesh::RefinementScheme(shape.source->subdivisionScheme,
                                                                      shape.displacer != nullptr);
            if (shape.level > 0 || shape.displacer) {
                prism::mesh::MeshConversion conversion = prism::mesh::ConvertMesh(
                    *shape.source, m_options.subdivision, shape.displacer, shape.level);
                shape.info = conversion.info;
                shape.coarseFaces = conversion.info.coarseFaces;
                shape.mesh = std::move(conversion.mesh);
                shape.instanced = true;
                if (conversion.info.subdivisionControlHull) controlHulls = true;
            } else {
                if (scheme != pxr::TfToken("none")) controlHulls = true;
                std::unique_ptr<Mesh, prism::mesh::ConvertedMeshDeleter> mesh(new Mesh());
                std::vector<int> coarse;
                if (prism::usd_detail::BuildPolygonMesh(*shape.source,
                        shape.worldSpace ? shape.world : pxr::GfMatrix4d(1.0), *mesh, coarse)) {
                    shape.coarseFaces = prism::mesh::SharedFaceTable(std::move(coarse));
                    shape.mesh = std::move(mesh);
                }
                shape.instanced = !shape.worldSpace;
            }
            shape.source.reset();
        });
        if (controlHulls)
            std::cerr << "[USDMesh] some subdivision surfaces are rendered as control hulls (subdivision level "
                         "0, over the maximum faces, or the refinement budget)\n";
        // Materials of GeomSubsets per triangle, from the coarse faces.
        for (auto& placement : m_meshPlacements) {
            Shape const& shape = *m_shapes[placement.shape];
            if (!shape.mesh) continue;
            if (!placement.faceMaterials) continue;
            auto triangles = std::make_shared<std::unordered_map<int, Material*>>();
            for (size_t t = 0; t < shape.coarseFaces.size(); ++t) {
                auto found = placement.faceMaterials->find(shape.coarseFaces[t]);
                (*triangles)[int(t)] = found == placement.faceMaterials->end() ? placement.material : found->second;
            }
            placement.triangleMaterials = std::move(triangles);
            placement.faceMaterials.reset();
        }
        for (auto& placement : m_meshPlacements) placement.ids.coarseFaces = m_shapes[placement.shape]->coarseFaces;
    }

    struct SpherePlacement {
        float radius;
        pxr::GfMatrix4d world;
        Material* material;
        bool enabled = true;
        PlacementIds ids;
        std::shared_ptr<const InstancePrimvars> instancePrimvars;
    };
    struct SpherePrototype {
        Sphere geometry;
        RTCScene scene;
        SpherePrototype(RTCDevice device, float radius)
            : geometry(Float3(0), radius, nullptr), scene(rtcNewScene(device)) {
            geometry.setupEmbree(device, scene);
            rtcCommitScene(scene);
        }
        ~SpherePrototype() {
            rtcReleaseScene(scene);
            if (geometry.geom) rtcReleaseGeometry(geometry.geom);
            delete geometry.transform;
        }
    };
    std::vector<SpherePlacement> m_spherePlacements;
    std::map<float, std::unique_ptr<SpherePrototype>> m_spherePrototypes;

    void captureSphere(const pxr::UsdGeomSphere& sphere, const pxr::GfMatrix4d& transform, Material* material,
                       bool enabled, PlacementIds ids, std::shared_ptr<const InstancePrimvars> primvars) {
        double radius = 1;
        sphere.GetRadiusAttr().Get(&radius, m_options.time);
        if (!(radius > 0) || !std::isfinite(radius) ||
            !std::isfinite(float(radius)) || std::abs(transform.GetDeterminant()) < 1e-15) {
            std::cerr << "[USDMesh] Invalid analytic sphere: " << sphere.GetPath() << '\n';
            return;
        }
        m_spherePlacements.push_back({float(radius), transform, material, enabled, std::move(ids),
                                      std::move(primvars)});
    }

    std::vector<std::shared_ptr<Material>> m_owned_materials;
    std::vector<std::unique_ptr<ColorTexture>> m_owned_fallback_textures;
    std::vector<std::unique_ptr<Diffuse>> m_owned_fallback_materials;
    std::map<SdfPath, int>                    m_mdl_material_lookup;
    std::map<SdfPath, int>                    m_fallbackByPath;
    std::vector<pxr::TfToken>                 m_bindingPurposes;
    LoadOptions                               m_options;
    std::unique_ptr<pxr::UsdGeomXformCache>   m_xformCache;
    std::vector<std::unique_ptr<Shape>>       m_shapes;
    std::map<std::pair<pxr::SdfPath, Material const*>, size_t> m_shapeIndex;
    std::vector<MeshPlacement>                m_meshPlacements;
    std::map<pxr::SdfPath, std::vector<std::pair<pxr::UsdPrim, int>>> m_prototypeRoots;
    std::unordered_map<pxr::SdfPath, PointInstancerData, pxr::SdfPath::Hash> m_instancerData;
    std::unordered_map<pxr::SdfPath, std::pair<bool, std::vector<Instance>>, pxr::SdfPath::Hash> m_placementCache;
    std::map<std::pair<pxr::SdfPath, pxr::SdfPath>, int> m_primIds;
    std::unordered_map<pxr::SdfPath, pxr::VtVec3fArray, pxr::SdfPath::Hash> m_skinnedPoints;
};
