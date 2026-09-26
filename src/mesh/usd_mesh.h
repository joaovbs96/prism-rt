#pragma once

// STD Lib deps
#include <iostream>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <vector>

// Open USD Deps
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
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

PXR_NAMESPACE_USING_DIRECTIVE
namespace mx = MaterialX;

// internal deps
#include "geometry.h"
#include "mdl_material.h"

// ============================================================================
// MaterialX material recovery from USD stages
// ============================================================================

/// Describes how a MaterialX material was found in the USD stage.
enum class MtlxSourceKind {
    ReferencedFile,   // A .mtlx file directly referenced into the stage
    UsdShadeEncoded   // MaterialX nodes encoded as a UsdShade network
};

/// Result of resolving a single material prim's MaterialX content.
struct ResolvedMtlxMaterial {
    SdfPath             materialPath;
    MtlxSourceKind      sourceKind;
    std::string         mtlxFilePath;     // non-empty only for ReferencedFile
    mx::DocumentPtr     document;         // recovered mx::Document (for UsdShade)
    std::string         materialName;     // name of the material node inside the doc
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
// Single-material conversion via Hydra hdMtlx
// ---------------------------------------------------------------------------

static mx::DocumentPtr
RecoverMtlxDocumentFromUsdShade(const UsdPrim& materialPrim,
                                const mx::DocumentPtr& mtlxLibraries)
{
    const TfToken mtlxContext("mtlx");
    const TfTokenVector contextVector    = { mtlxContext };
    const TfTokenVector shaderSourceTypes = { mtlxContext };

    SdfPath materialPath = materialPrim.GetPath();
    const UsdShadeShader surface = UsdShadeMaterial(materialPrim).ComputeSurfaceSource(contextVector);
    if (!surface)
        return nullptr;

    HdMaterialNetworkMap networkMap;
    UsdImagingBuildHdMaterialNetworkFromTerminal(
        surface.GetPrim(),
        HdMaterialTerminalTokens->surface,
        shaderSourceTypes,
        contextVector,
        &networkMap,
        UsdTimeCode::Default()
    );

    bool isVolume = false;
    HdMaterialNetwork2 network2 =
        HdConvertToHdMaterialNetwork2(networkMap, &isVolume);

    auto surfaceTerminalIt = network2.terminals.find(HdMaterialTerminalTokens->surface);
    if (surfaceTerminalIt == network2.terminals.end()) {
        std::cerr << "[MtlxRecover] No surface terminal for " << materialPath << "\n";
        return nullptr;
    }

    const SdfPath& terminalNodePath = surfaceTerminalIt->second.upstreamNode;
    auto terminalNodeIt = network2.nodes.find(terminalNodePath);
    if (terminalNodeIt == network2.nodes.end()) {
        std::cerr << "[MtlxRecover] Terminal node not found for " << materialPath << "\n";
        return nullptr;
    }

    HdMtlxTexturePrimvarData mxHdData;
    return HdMtlxCreateMtlxDocumentFromHdNetwork(
        network2,
        terminalNodeIt->second,
        terminalNodePath,
        materialPath,
        mtlxLibraries,
        &mxHdData
    );
}

// ---------------------------------------------------------------------------
// Batch collection: all MaterialX materials in a stage
// ---------------------------------------------------------------------------

static std::map<SdfPath, ResolvedMtlxMaterial>
CollectMaterialXMaterials(const UsdStageRefPtr& stage,
                          const std::string& mx_libraries_path)
{
    std::map<SdfPath, ResolvedMtlxMaterial> result;

    // Same stdlib instance the material compilations below will use.
    mx::DocumentPtr mtlxStdLibraries = MDLGenerator::shared(mx_libraries_path)->get_stdlib();

    for (const UsdPrim& prim : stage->Traverse()) {
        if (!prim.IsA<UsdShadeMaterial>())
            continue;

        UsdShadeMaterial material(prim);
        SdfPath materialPath = prim.GetPath();

        // --- Case 1: Direct .mtlx file reference ---
        auto mtlxFile = FindReferencedMtlxFileInAncestors(prim);
        if (mtlxFile.has_value()) {
            ResolvedMtlxMaterial resolved;
            resolved.materialPath = materialPath;
            resolved.sourceKind   = MtlxSourceKind::ReferencedFile;
            resolved.mtlxFilePath = mtlxFile.value();
            resolved.materialName = materialPath.GetName();

            std::cout << "[MtlxCollect] Found referenced .mtlx: "
                      << resolved.mtlxFilePath << " at " << materialPath << "\n";

            result[materialPath] = std::move(resolved);
            continue;
        }

        // --- Case 2: UsdShade-encoded MaterialX ---
        if (HasMtlxSurfaceOutput(material)) {
            mx::DocumentPtr doc =
                RecoverMtlxDocumentFromUsdShade(prim, mtlxStdLibraries);

            if (!doc) {
                std::cerr << "[MtlxCollect] Failed to recover document for "
                          << materialPath << "\n";
                continue;
            }

            ResolvedMtlxMaterial resolved;
            resolved.materialPath = materialPath;
            resolved.sourceKind   = MtlxSourceKind::UsdShadeEncoded;
            resolved.document     = doc;

            auto matNodes = doc->getMaterialNodes();
            resolved.materialName = matNodes.empty()
                ? materialPath.GetName()
                : matNodes[0]->getName();

            std::cout << "[MtlxCollect] Recovered UsdShade-encoded MaterialX: "
                      << materialPath << " (material: '"
                      << resolved.materialName << "')\n";

            result[materialPath] = std::move(resolved);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Compile a single resolved material into an MDLMaterial
// ---------------------------------------------------------------------------

static std::unique_ptr<MDLMaterial>
CompileMtlxMaterial(const ResolvedMtlxMaterial& resolved,
                    mdl::Context& mdl_context,
                    const std::string& mx_libraries_path,
                    const std::string& mx_mdl_path)
{
    if (resolved.sourceKind == MtlxSourceKind::ReferencedFile) {
        return std::make_unique<MDLMaterial>(
            resolved.mtlxFilePath,
            mdl_context,
            mx_libraries_path,
            mx_mdl_path,
            resolved.materialName
        );
    }

    if (!resolved.document) {
        std::cerr << "[CompileMtlx] No document for " << resolved.materialPath << "\n";
        return nullptr;
    }

    std::filesystem::path tempDir  = std::filesystem::temp_directory_path();
    // Full prim path, not just the leaf: same-named materials in different
    // scopes must not share a temp file (and thus an MDL module name).
    std::filesystem::path tempMtlx = tempDir /
        (TfMakeValidIdentifier(resolved.materialPath.GetString()) + "_recovered.mtlx");

    try {
        mx::writeToXmlFile(resolved.document, tempMtlx.string());
    } catch (const std::exception& e) {
        std::cerr << "[CompileMtlx] Failed to write temp .mtlx: " << e.what() << "\n";
        return nullptr;
    }

    std::cout << "[CompileMtlx] Wrote recovered document to: " << tempMtlx << "\n";

    return std::make_unique<MDLMaterial>(
        tempMtlx.string(),
        mdl_context,
        mx_libraries_path,
        mx_mdl_path,
        resolved.materialName
    );
}

// ============================================================================
// Per-face-corner vertex layout
//
// USD meshes with faceVarying UVs (the common case) require that geometry is
// expanded so that each face corner becomes its own vertex entry. This struct
// carries the fully-expanded, renderer-ready data for one mesh prim and is
// built by buildMeshData() before anything is written into the global buffers.
// ============================================================================

struct MeshData {
    std::vector<Float3>        positions;
    std::vector<Float3>        normals;
    std::vector<Float2>        texcoords;
    std::vector<unsigned int>  indices;   // into the above, base-0 for this mesh
    int                        mat_id = -1;
};

// ============================================================================
// USDMesh — loads geometry + detects and compiles MaterialX materials
// ============================================================================

class USDMesh : public Mesh {
public:

    // Basic constructor — no MDL/MaterialX support, uses displayColor fallback
    USDMesh(const std::filesystem::path& file_path)
        : USDMesh(file_path, nullptr, "", "")
    {}

    // Full constructor with MDL/MaterialX material support
    USDMesh(const std::filesystem::path& file_path,
            mdl::Context* mdl_context,
            const std::string& mx_libraries_path,
            const std::string& mx_mdl_path)
    {
        pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(file_path.string());
        if (!stage) {
            std::cerr << "Failed to open USD file: " << file_path << "\n";
            return;
        }
        std::cout << "Successfully opened USD stage: " << file_path << "\n";

        // ── Phase 1: Detect and compile MaterialX materials ────────────────
        if (mdl_context && !mx_libraries_path.empty()) {
            std::cout << "[USDMesh] Scanning for MaterialX materials...\n";

            auto mtlxMaterials = CollectMaterialXMaterials(stage, mx_libraries_path);

            // Cache only within this mesh load: SDK context, libraries and loader
            // options are identical here. USD prim paths remain separate bindings.
            // Recovered UsdShade documents may contain different composed values,
            // so they must not be merged merely because their names match.
            using MaterialKey = std::pair<std::string, std::string>;
            std::map<MaterialKey, int> compiled_files;
            size_t reused_materials = 0;

            for (auto& [matPath, resolved] : mtlxMaterials) {
                std::optional<MaterialKey> cache_key;
                if (resolved.sourceKind == MtlxSourceKind::ReferencedFile) {
                    std::error_code error;
                    auto path = std::filesystem::weakly_canonical(resolved.mtlxFilePath, error);
                    if (!error) {
                        cache_key = MaterialKey(path.generic_string(), resolved.materialName);
                        auto cached = compiled_files.find(*cache_key);
                        if (cached != compiled_files.end()) {
                            m_mdl_material_lookup[matPath] = cached->second;
                            ++reused_materials;
                            std::cout << "[USDMesh] Reused MaterialX material '"
                                      << matPath << "' -> mat_id " << cached->second << "\n";
                            continue;
                        }
                    }
                }
                auto mdlMat = CompileMtlxMaterial(
                    resolved, *mdl_context, mx_libraries_path, mx_mdl_path);

                if (mdlMat && mdlMat->is_valid()) {
                    int mat_id = static_cast<int>(m_owned_mdl_materials.size());
                    m_mdl_material_lookup[matPath] = mat_id;
                    materials[mat_id] = mdlMat.get();
                    m_owned_mdl_materials.push_back(std::move(mdlMat));
                    // Never cache failures; ownership remains in the vector and
                    // all reused bindings resolve to the same stable material ID.
                    if (cache_key) compiled_files.emplace(*cache_key, mat_id);

                    std::cout << "[USDMesh] Compiled MaterialX material '"
                              << matPath << "' -> mat_id " << mat_id << "\n";
                } else {
                    std::cerr << "[USDMesh] Failed to compile MaterialX material: "
                              << matPath << "\n";
                }
            }

            std::cout << "[USDMesh] Compiled " << m_owned_mdl_materials.size()
                      << " MaterialX material(s), reused " << reused_materials
                      << ", bound " << m_mdl_material_lookup.size() << " USD material(s)\n";
        }

        // ── Phase 2: Process geometry ──────────────────────────────────────
        pxr::UsdTimeCode timeCode = pxr::UsdTimeCode::Default();
        pxr::UsdGeomXformCache xformCache(timeCode);

        // We do a single traversal. PointInstancers are handled inline;
        // their prototype subtrees are skipped by the traversal iterator
        // so we won't double-process prototype meshes.
        pxr::UsdPrimRange range = stage->Traverse();
        for (auto iter = range.begin(); iter != range.end(); ++iter) {
            const pxr::UsdPrim& prim = *iter;

            if (prim.IsA<pxr::UsdGeomPointInstancer>()) {
                processPointInstancer(
                    pxr::UsdGeomPointInstancer(prim),
                    xformCache, timeCode);

                // Skip into the instancer's subtree — prototypes live there
                // and must not be processed as stand-alone meshes.
                iter.PruneChildren();
                continue;
            }

            if (prim.IsA<pxr::UsdGeomMesh>()) {
                MeshData data = buildMeshData(
                    pxr::UsdGeomMesh(prim), xformCache, timeCode,
                    xformCache.GetLocalToWorldTransform(prim));
                appendMeshData(data);
            }
        }

        std::cout << "Finished processing USD file:\n"
                  << "    Total vertices:  " << vertices.size()  << "\n"
                  << "    Total triangles: " << globalTriangleCount << "\n"
                  << "    Total materials: " << materials.size() << "\n";
    }

private:

    int globalTriangleCount = 0;

    std::vector<std::unique_ptr<MDLMaterial>> m_owned_mdl_materials;
    std::map<SdfPath, int>                    m_mdl_material_lookup;

    // -----------------------------------------------------------------------
    // appendMeshData — commit a fully-built MeshData into the global buffers
    // -----------------------------------------------------------------------

    void appendMeshData(const MeshData& data)
    {
        if (data.positions.empty()) return;

        const size_t vertexOffset = vertices.size();

        // Append geometry
        vertices.insert(vertices.end(),   data.positions.begin(),  data.positions.end());
        normals.insert(normals.end(),     data.normals.begin(),    data.normals.end());
        texcoords.insert(texcoords.end(), data.texcoords.begin(),  data.texcoords.end());

        // Append indices (rebased to global vertex buffer)
        for (unsigned int idx : data.indices) {
            indices.push_back(static_cast<unsigned int>(vertexOffset) + idx);
        }

        // Assign material to every triangle
        size_t triCount = data.indices.size() / 3;
        for (size_t t = 0; t < triCount; ++t) {
            material_mapping[globalTriangleCount++] = data.mat_id;
        }
    }

    // -----------------------------------------------------------------------
    // resolveMaterialId — look up or create a fallback material for a prim
    // -----------------------------------------------------------------------

    int resolveMaterialId(const pxr::UsdPrim& prim,
                          pxr::UsdTimeCode timeCode)
    {
        if (!m_mdl_material_lookup.empty()) {
            pxr::UsdShadeMaterialBindingAPI bindingAPI(prim);
            pxr::UsdShadeMaterial boundMat = bindingAPI.ComputeBoundMaterial();
            if (boundMat) {
                auto it = m_mdl_material_lookup.find(
                    boundMat.GetPrim().GetPath());
                if (it != m_mdl_material_lookup.end())
                    return it->second;
            }
        }

        // Fallback: Diffuse from displayColor
        pxr::UsdGeomPrimvarsAPI primvarsAPI(prim);
        pxr::UsdGeomPrimvar displayColorPrimvar =
            primvarsAPI.GetPrimvar(TfToken("displayColor"));

        Color3 mesh_color(0.7f, 0.7f, 0.7f);
        if (displayColorPrimvar) {
            pxr::VtValue val;
            if (displayColorPrimvar.Get(&val, timeCode) &&
                val.IsHolding<pxr::VtVec3fArray>())
            {
                auto arr = val.Get<pxr::VtVec3fArray>();
                if (!arr.empty())
                    mesh_color = Color3(arr[0][0], arr[0][1], arr[0][2]);
            }
        }

        int mat_id = static_cast<int>(materials.size());
        materials[mat_id] = new Diffuse(new ColorTexture(mesh_color));
        return mat_id;
    }

    // -----------------------------------------------------------------------
    // buildMeshData — two-pass builder
    //
    // Pass 1: read all primvars and decide on the vertex layout.
    //         faceVarying UVs require one vertex per face corner.
    //         vertex/varying UVs keep the original indexed layout.
    //
    // Pass 2: emit expanded geometry into MeshData.
    //
    // The transform is applied here so that the same function works for both
    // regular meshes (world transform from xformCache) and instancer copies
    // (prototype transform composed with the instance transform).
    // -----------------------------------------------------------------------

    MeshData buildMeshData(const pxr::UsdGeomMesh& mesh,
                           pxr::UsdGeomXformCache& xformCache,
                           pxr::UsdTimeCode timeCode,
                           const pxr::GfMatrix4d& worldTransform)
    {
        MeshData out;

        // ── Resolve material ───────────────────────────────────────────────
        out.mat_id = resolveMaterialId(mesh.GetPrim(), timeCode);

        // ── Read raw mesh attributes ───────────────────────────────────────
        pxr::VtArray<pxr::GfVec3f> points;
        pxr::VtArray<int>          faceVertexIndices;
        pxr::VtArray<int>          faceVertexCounts;

        if (!mesh.GetPointsAttr().Get(&points, timeCode) ||
            !mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, timeCode) ||
            !mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, timeCode))
        {
            std::cerr << "[USDMesh] Missing required attributes on "
                      << mesh.GetPrim().GetPath() << "\n";
            return out;
        }

        // ── Compute transform-derived values ───────────────────────────────
        pxr::GfMatrix4d normalMatrix = worldTransform.GetInverse().GetTranspose();
        bool flipNormals = worldTransform.GetDeterminant() < 0.0;

        // ── Pass 1: UV primvar discovery ───────────────────────────────────
        pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh.GetPrim());
        pxr::UsdGeomPrimvar uvPrimvar;
        for (const pxr::UsdGeomPrimvar& pv : primvarsAPI.GetPrimvars()) {
            TfToken name = pv.GetPrimvarName();
            if (name == "st" || name == "uv" || name == "UV") {
                uvPrimvar = pv;
                break;
            }
        }

        pxr::VtArray<pxr::GfVec2f> uvValues;
        pxr::VtArray<int>          uvIndices;
        pxr::TfToken               uvInterpolation;
        bool                       hasUvIndices = false;
        bool                       isFaceVarying = false;

        if (uvPrimvar) {
            uvPrimvar.Get(&uvValues, timeCode);
            hasUvIndices   = uvPrimvar.GetIndices(&uvIndices, timeCode);
            uvInterpolation = uvPrimvar.GetInterpolation();
            isFaceVarying  = (uvInterpolation == pxr::UsdGeomTokens->faceVarying);
        }

        // ── Pass 1: normals discovery ──────────────────────────────────────
        pxr::VtArray<pxr::GfVec3f> meshNormals;
        pxr::TfToken               normalsInterpolation;
        bool                       hasMeshNormals = false;
        bool                       normalsAreFaceVarying = false;

        pxr::UsdAttribute normalsAttr = mesh.GetNormalsAttr();
        if (normalsAttr && normalsAttr.Get(&meshNormals, timeCode)) {
            hasMeshNormals = true;
            pxr::UsdGeomPrimvar normalsPrimvar =
                primvarsAPI.GetPrimvar(pxr::UsdGeomTokens->normals);
            if (normalsPrimvar) {
                normalsInterpolation = normalsPrimvar.GetInterpolation();
                normalsAreFaceVarying =
                    (normalsInterpolation == pxr::UsdGeomTokens->faceVarying);
            }
        }

        // ── Pass 2: emit geometry ──────────────────────────────────────────
        //
        // If UVs (or normals) are faceVarying we must expand: one vertex
        // per face corner. Otherwise we keep the compact indexed layout.
        //
        if (isFaceVarying) {
            buildFaceVaryingLayout(
                out, points, faceVertexIndices, faceVertexCounts,
                uvValues, uvIndices, hasUvIndices,
                meshNormals, normalsAreFaceVarying, hasMeshNormals,
                worldTransform, normalMatrix, flipNormals);
        } else {
            buildIndexedLayout(
                out, points, faceVertexIndices, faceVertexCounts,
                uvValues, uvInterpolation,
                meshNormals, normalsInterpolation, hasMeshNormals,
                worldTransform, normalMatrix, flipNormals);
        }

        return out;
    }

    // -----------------------------------------------------------------------
    // buildFaceVaryingLayout
    //
    // Expands the mesh so every face corner is its own vertex. This is
    // required when UVs (or normals) are authored with faceVarying
    // interpolation, since the same position vertex may need different
    // UV/normal values depending on which face it belongs to.
    // -----------------------------------------------------------------------

    void buildFaceVaryingLayout(
        MeshData& out,
        const pxr::VtArray<pxr::GfVec3f>& points,
        const pxr::VtArray<int>&           faceVertexIndices,
        const pxr::VtArray<int>&           faceVertexCounts,
        const pxr::VtArray<pxr::GfVec2f>& uvValues,
        const pxr::VtArray<int>&           uvIndices,
        bool                               hasUvIndices,
        const pxr::VtArray<pxr::GfVec3f>& meshNormals,
        bool                               normalsAreFaceVarying,
        bool                               hasMeshNormals,
        const pxr::GfMatrix4d&             worldTransform,
        const pxr::GfMatrix4d&             normalMatrix,
        bool                               flipNormals)
    {
        // Pre-compute per-vertex geometric normals for fallback / fan normal
        std::vector<pxr::GfVec3f> geometricNormals =
            computeGeometricNormals(points, faceVertexIndices, faceVertexCounts, flipNormals);

        size_t faceCorner = 0;

        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int fvc = faceVertexCounts[faceIdx];

            // Collect per-corner expanded vertex indices for triangulation
            std::vector<unsigned int> cornerVerts;
            cornerVerts.reserve(fvc);

            for (int c = 0; c < fvc; ++c) {
                int posIdx = faceVertexIndices[faceCorner + c];

                unsigned int expandedIdx = static_cast<unsigned int>(out.positions.size());

                // Position
                pxr::GfVec3d p(points[posIdx][0], points[posIdx][1], points[posIdx][2]);
                pxr::GfVec3d tp = worldTransform.Transform(p);
                out.positions.push_back(Float3(
                    static_cast<float>(tp[0]),
                    static_cast<float>(tp[1]),
                    static_cast<float>(tp[2])));

                // UV
                if (!uvValues.empty()) {
                    int uvIdx = hasUvIndices
                        ? uvIndices[faceCorner + c]
                        : static_cast<int>(faceCorner + c);
                    const pxr::GfVec2f& uv = uvValues[uvIdx];
                    out.texcoords.push_back(Float2(uv[0], uv[1]));
                } else {
                    out.texcoords.push_back(Float2(0.f, 0.f));
                }

                // Normal
                pxr::GfVec3f rawNormal(0.f, 1.f, 0.f);
                if (hasMeshNormals && normalsAreFaceVarying &&
                    (faceCorner + c) < meshNormals.size())
                {
                    rawNormal = meshNormals[faceCorner + c];
                } else if (hasMeshNormals && !normalsAreFaceVarying &&
                           posIdx < static_cast<int>(meshNormals.size()))
                {
                    rawNormal = meshNormals[posIdx];
                } else if (posIdx < static_cast<int>(geometricNormals.size())) {
                    rawNormal = geometricNormals[posIdx];
                }

                out.normals.push_back(
                    transformNormal(rawNormal, normalMatrix, flipNormals));

                cornerVerts.push_back(expandedIdx);
            }

            // Fan triangulate
            triangulate(out.indices, cornerVerts);

            faceCorner += fvc;
        }
    }

    // -----------------------------------------------------------------------
    // buildIndexedLayout
    //
    // Keeps the original compact indexed layout for vertex/varying/uniform
    // interpolated data. Normals and UVs are mapped 1:1 with points.
    // -----------------------------------------------------------------------

    void buildIndexedLayout(
        MeshData& out,
        const pxr::VtArray<pxr::GfVec3f>& points,
        const pxr::VtArray<int>&           faceVertexIndices,
        const pxr::VtArray<int>&           faceVertexCounts,
        const pxr::VtArray<pxr::GfVec2f>& uvValues,
        const pxr::TfToken&                uvInterpolation,
        const pxr::VtArray<pxr::GfVec3f>& meshNormals,
        const pxr::TfToken&                normalsInterpolation,
        bool                               hasMeshNormals,
        const pxr::GfMatrix4d&             worldTransform,
        const pxr::GfMatrix4d&             normalMatrix,
        bool                               flipNormals)
    {
        // Positions
        out.positions.reserve(points.size());
        for (const pxr::GfVec3f& p : points) {
            pxr::GfVec3d pd(p[0], p[1], p[2]);
            pxr::GfVec3d tp = worldTransform.Transform(pd);
            out.positions.push_back(Float3(
                static_cast<float>(tp[0]),
                static_cast<float>(tp[1]),
                static_cast<float>(tp[2])));
        }

        // Normals (per-vertex)
        if (hasMeshNormals &&
            (normalsInterpolation == pxr::UsdGeomTokens->vertex ||
             normalsInterpolation == pxr::UsdGeomTokens->varying))
        {
            out.normals.reserve(meshNormals.size());
            for (const pxr::GfVec3f& n : meshNormals) {
                out.normals.push_back(
                    transformNormal(n, normalMatrix, flipNormals));
            }
        } else {
            // Generate geometric normals
            auto geom = computeGeometricNormals(
                points, faceVertexIndices, faceVertexCounts, flipNormals);
            out.normals.reserve(geom.size());
            for (const pxr::GfVec3f& n : geom) {
                out.normals.push_back(
                    transformNormal(n, normalMatrix, flipNormals));
            }
        }

        // UVs
        if (!uvValues.empty() &&
            (uvInterpolation == pxr::UsdGeomTokens->vertex ||
             uvInterpolation == pxr::UsdGeomTokens->varying))
        {
            out.texcoords.reserve(uvValues.size());
            for (const pxr::GfVec2f& uv : uvValues) {
                out.texcoords.push_back(Float2(uv[0], uv[1]));
            }
        }

        // Pad normals / texcoords to match positions
        while (out.normals.size()    < out.positions.size())
            out.normals.push_back(Float3(0.f, 1.f, 0.f));
        while (out.texcoords.size()  < out.positions.size())
            out.texcoords.push_back(Float2(0.f, 0.f));

        // Indices — triangulate faces
        size_t faceCorner = 0;
        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int fvc = faceVertexCounts[faceIdx];
            std::vector<unsigned int> cornerVerts(fvc);
            for (int c = 0; c < fvc; ++c)
                cornerVerts[c] = static_cast<unsigned int>(
                    faceVertexIndices[faceCorner + c]);
            triangulate(out.indices, cornerVerts);
            faceCorner += fvc;
        }
    }

    // -----------------------------------------------------------------------
    // Shared helpers
    // -----------------------------------------------------------------------

    /// Fan-triangulate a face given its corner vertex indices (into MeshData).
    static void triangulate(std::vector<unsigned int>& outIndices,
                            const std::vector<unsigned int>& cornerVerts)
    {
        int fvc = static_cast<int>(cornerVerts.size());
        if (fvc < 3) return;

        if (fvc == 3) {
            outIndices.push_back(cornerVerts[0]);
            outIndices.push_back(cornerVerts[1]);
            outIndices.push_back(cornerVerts[2]);
        } else if (fvc == 4) {
            outIndices.push_back(cornerVerts[0]);
            outIndices.push_back(cornerVerts[1]);
            outIndices.push_back(cornerVerts[2]);
            outIndices.push_back(cornerVerts[0]);
            outIndices.push_back(cornerVerts[2]);
            outIndices.push_back(cornerVerts[3]);
        } else {
            // Fan from vertex 0 for n-gons
            for (int i = 1; i < fvc - 1; ++i) {
                outIndices.push_back(cornerVerts[0]);
                outIndices.push_back(cornerVerts[i]);
                outIndices.push_back(cornerVerts[i + 1]);
            }
        }
    }

    /// Compute area-weighted per-vertex geometric normals.
    static std::vector<pxr::GfVec3f>
    computeGeometricNormals(const pxr::VtArray<pxr::GfVec3f>& points,
                            const pxr::VtArray<int>&           faceVertexIndices,
                            const pxr::VtArray<int>&           faceVertexCounts,
                            bool                               flipNormals)
    {
        std::vector<pxr::GfVec3f> normals(points.size(), pxr::GfVec3f(0.f));

        size_t faceCorner = 0;
        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int fvc = faceVertexCounts[faceIdx];
            if (fvc >= 3) {
                int i0 = faceVertexIndices[faceCorner + 0];
                int i1 = faceVertexIndices[faceCorner + 1];
                int i2 = faceVertexIndices[faceCorner + 2];

                pxr::GfVec3f e1 = points[i1] - points[i0];
                pxr::GfVec3f e2 = points[i2] - points[i0];
                pxr::GfVec3f fn = e1 ^ e2; // cross product, area-weighted

                if (flipNormals) fn = -fn;

                for (int v = 0; v < fvc; ++v)
                    normals[faceVertexIndices[faceCorner + v]] += fn;
            }
            faceCorner += fvc;
        }

        for (auto& n : normals) {
            float len = n.GetLength();
            n = (len > 0.f) ? (n / len) : pxr::GfVec3f(0.f, 1.f, 0.f);
        }
        return normals;
    }

    /// Transform and normalize a normal vector.
    static Float3 transformNormal(const pxr::GfVec3f&    n,
                                  const pxr::GfMatrix4d& normalMatrix,
                                  bool                   flip)
    {
        pxr::GfVec3d nd(n[0], n[1], n[2]);
        pxr::GfVec3d tn = normalMatrix.TransformDir(nd);
        if (flip) tn = -tn;
        double len = tn.GetLength();
        if (len > 0.0) tn /= len;
        return Float3(
            static_cast<float>(tn[0]),
            static_cast<float>(tn[1]),
            static_cast<float>(tn[2]));
    }

    // -----------------------------------------------------------------------
    // PointInstancer support
    //
    // For each instance, compose the instance transform (position + orientation
    // + scale) with the prototype mesh's own local-to-world transform and call
    // buildMeshData with the composed transform. This produces a correct,
    // fully-transformed copy of the prototype geometry at each instance
    // location. Actual GPU instancing can be added later.
    // -----------------------------------------------------------------------

    void processPointInstancer(const pxr::UsdGeomPointInstancer& instancer,
                            pxr::UsdGeomXformCache&           xformCache,
                            pxr::UsdTimeCode                  timeCode)
    {
        pxr::VtArray<pxr::GfVec3f> positions;
        pxr::VtArray<pxr::GfQuath> orientations;
        pxr::VtArray<pxr::GfVec3f> scales;
        pxr::VtArray<int>          protoIndices;

        instancer.GetPositionsAttr().Get(&positions,     timeCode);
        instancer.GetOrientationsAttr().Get(&orientations, timeCode);
        instancer.GetScalesAttr().Get(&scales,           timeCode);
        instancer.GetProtoIndicesAttr().Get(&protoIndices, timeCode);

        if (positions.empty() || protoIndices.empty()) {
            std::cerr << "[PointInstancer] No instances on "
                    << instancer.GetPrim().GetPath() << "\n";
            return;
        }

        pxr::SdfPathVector protoPaths;
        instancer.GetPrototypesRel().GetTargets(&protoPaths);
        if (protoPaths.empty()) {
            std::cerr << "[PointInstancer] No prototypes on "
                    << instancer.GetPrim().GetPath() << "\n";
            return;
        }

        pxr::UsdStageRefPtr stage = instancer.GetPrim().GetStage();
        pxr::GfMatrix4d instancerWorld =
            xformCache.GetLocalToWorldTransform(instancer.GetPrim());

        std::cout << "[PointInstancer] Processing " << protoIndices.size()
                << " instance(s) on " << instancer.GetPrim().GetPath() << "\n";

        for (size_t i = 0; i < protoIndices.size(); ++i) {
            int protoIdx = protoIndices[i];
            if (protoIdx < 0 || protoIdx >= static_cast<int>(protoPaths.size())) {
                std::cerr << "[PointInstancer] Invalid proto index " << protoIdx
                        << " at instance " << i << "\n";
                continue;
            }

            pxr::UsdPrim protoPrim = stage->GetPrimAtPath(protoPaths[protoIdx]);
            if (!protoPrim.IsValid()) {
                std::cerr << "[PointInstancer] Prototype prim not found: "
                        << protoPaths[protoIdx] << "\n";
                continue;
            }

            // Build per-instance local transform: T * R * S
            pxr::GfVec3f pos = (i < positions.size())
                ? positions[i] : pxr::GfVec3f(0.f);
            pxr::GfQuath ori = (i < orientations.size())
                ? orientations[i] : pxr::GfQuath(1.f, 0.f, 0.f, 0.f);
            pxr::GfVec3f scl = (i < scales.size())
                ? scales[i] : pxr::GfVec3f(1.f, 1.f, 1.f);

            pxr::GfMatrix4d scaleMat(1.0);
            scaleMat.SetScale(pxr::GfVec3d(scl[0], scl[1], scl[2]));

            pxr::GfQuatf orif(
                static_cast<float>(ori.GetReal()),
                static_cast<float>(ori.GetImaginary()[0]),
                static_cast<float>(ori.GetImaginary()[1]),
                static_cast<float>(ori.GetImaginary()[2]));
            pxr::GfQuatd orid(
                orif.GetReal(),
                pxr::GfVec3d(orif.GetImaginary()[0],
                            orif.GetImaginary()[1],
                            orif.GetImaginary()[2]));
            pxr::GfMatrix4d rotMat(1.0);
            rotMat.SetRotate(orid);

            pxr::GfMatrix4d transMat(1.0);
            transMat.SetTranslate(pxr::GfVec3d(pos[0], pos[1], pos[2]));

            pxr::GfMatrix4d instanceLocal = scaleMat * rotMat * transMat;

            // The prototype's world transform already has instancerWorld baked in
            // (it's a child of the instancer). Strip it out to get only the
            // proto's own local contribution relative to the instancer.
            pxr::GfMatrix4d protoRelative =
                xformCache.GetLocalToWorldTransform(protoPrim) *
                instancerWorld.GetInverse();

            // Final: instancerWorld brings us from instancer-local to world,
            // instanceLocal places this specific instance, protoRelative applies
            // any local transform the prototype prim itself has.
            pxr::GfMatrix4d finalTransform =
                protoRelative * instanceLocal * instancerWorld;

            processPrimSubtreeWithTransform(
                protoPrim, xformCache, timeCode, finalTransform);
        }
    }

    /// Walk a prim subtree, applying a caller-supplied world transform to every
    /// mesh found. Used to process instancer prototypes with their composed
    /// instance transform rather than the prim's own world transform.
    void processPrimSubtreeWithTransform(const pxr::UsdPrim&       root,
                                         pxr::UsdGeomXformCache&   xformCache,
                                         pxr::UsdTimeCode          timeCode,
                                         const pxr::GfMatrix4d&    worldTransform)
    {
        if (root.IsA<pxr::UsdGeomMesh>()) {
            MeshData data = buildMeshData(
                pxr::UsdGeomMesh(root), xformCache, timeCode, worldTransform);
            appendMeshData(data);
        }

        for (const pxr::UsdPrim& child : root.GetChildren()) {
            // Compose child's local transform on top of the supplied world
            pxr::GfMatrix4d childLocal =
                xformCache.GetLocalToWorldTransform(child) *
                xformCache.GetLocalToWorldTransform(root).GetInverse();
            pxr::GfMatrix4d childWorld = childLocal * worldTransform;

            processPrimSubtreeWithTransform(child, xformCache, timeCode, childWorld);
        }
    }
};
