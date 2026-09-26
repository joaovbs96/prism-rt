#include "scene_bridge.h"

#include "materials.h"
#include "render_buffer.h"
#include "triangulation.h"

#include "../environment.h"
#include "../geometry.h"
#include "../material.h"
#include "../mdl/context.h"
#include "../rng.h"
#include "../scene.h"
#include "../texture.h"

#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/geomSubsetSchema.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/xformSchema.h"

#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <optional>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::hydra {
namespace {

template<class T>
bool _ReadArray(HdSampledDataSourceHandle const &source, VtArray<T> *out)
{
    if (!source) return false;
    VtValue value = source->GetValue(0.0f);
    if (!value.IsHolding<VtArray<T>>()) return false;
    *out = value.UncheckedGet<VtArray<T>>();
    return true;
}

GfMatrix4d _LocalTransform(HdContainerDataSourceHandle const &ds, bool *reset)
{
    HdXformSchema schema = HdXformSchema::GetFromParent(ds);
    *reset = false;
    if (!schema) return GfMatrix4d(1.0);
    if (auto r = schema.GetResetXformStack()) *reset = r->GetTypedValue(0.0f);
    if (auto m = schema.GetMatrix()) return m->GetTypedValue(0.0f);
    return GfMatrix4d(1.0);
}

GfMatrix4d _WorldTransform(HdSceneIndexBaseRefPtr const &scene,
                           SdfPath path)
{
    GfMatrix4d result(1.0);
    std::vector<GfMatrix4d> local;
    while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath()) {
        HdSceneIndexPrim prim = scene->GetPrim(path);
        bool reset = false;
        GfMatrix4d matrix = _LocalTransform(prim.dataSource, &reset);
        local.push_back(matrix);
        if (reset) break;
        path = path.GetParentPath();
    }
    // Gf matrices transform row vectors, so local-to-world is child * parent.
    for (GfMatrix4d const &matrix : local) result *= matrix;
    return result;
}

GfMatrix4d _RelativeTransform(HdSceneIndexBaseRefPtr const &scene,
                              SdfPath path, SdfPath const &stopParent)
{
    GfMatrix4d result(1.0);
    while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath() &&
           path != stopParent) {
        HdSceneIndexPrim const prim = scene->GetPrim(path);
        bool reset = false;
        GfMatrix4d const matrix = _LocalTransform(prim.dataSource, &reset);
        result *= matrix;
        if (reset) break;
        path = path.GetParentPath();
    }
    return result;
}

bool _Visible(HdContainerDataSourceHandle const &ds)
{
    HdVisibilitySchema s = HdVisibilitySchema::GetFromParent(ds);
    if (auto v = s.GetVisibility()) return v->GetTypedValue(0.0f);
    return true;
}

bool _WorldVisible(HdSceneIndexBaseRefPtr const &scene, SdfPath path)
{
    while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath()) {
        if (!_Visible(scene->GetPrim(path).dataSource)) return false;
        path = path.GetParentPath();
    }
    return true;
}

bool _PurposeMatches(HdContainerDataSourceHandle const &ds,
                     TfTokenVector const &renderTags)
{
    HdPurposeSchema purpose = HdPurposeSchema::GetFromParent(ds);
    const TfToken value = purpose ? purpose.ResolvePurposeValue()
                                  : HdRenderTagTokens->geometry;
    if (renderTags.empty())
        return value == HdRenderTagTokens->geometry ||
               value == HdRenderTagTokens->render;
    return std::find(renderTags.begin(), renderTags.end(), value) != renderTags.end();
}

SdfPath _MaterialPath(HdContainerDataSourceHandle const &ds)
{
    HdMaterialBindingsSchema bindings = HdMaterialBindingsSchema::GetFromParent(ds);
    HdMaterialBindingSchema binding = bindings.GetMaterialBinding(HdTokens->full);
    if (!binding) binding = bindings.GetMaterialBinding();
    if (auto p = binding.GetPath()) return p->GetTypedValue(0.0f);
    return {};
}

struct PrimvarData {
    TfToken interpolation;
    VtValue value;
};

struct PrimKey {
    SdfPath path;
    SdfPath instancerPath;
    bool operator==(PrimKey const &) const = default;
};

struct PrimKeyHash {
    size_t operator()(PrimKey const &key) const
    {
        size_t const a = SdfPath::Hash{}(key.path);
        size_t const b = SdfPath::Hash{}(key.instancerPath);
        return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    }
};

struct InstanceLevel {
    SdfPath instancerPath;
    int sourceIndex = -1;
    int ordinal = -1;
    int radix = 0;
};

PrimvarData _Primvar(HdPrimvarsSchema const &primvars, TfToken const &name)
{
    PrimvarData result;
    HdPrimvarSchema p = primvars.GetPrimvar(name);
    if (!p) return result;
    if (auto i = p.GetInterpolation()) result.interpolation = i->GetTypedValue(0.0f);
    if (auto v = p.GetFlattenedPrimvarValue()) result.value = v->GetValue(0.0f);
    return result;
}

size_t _PrimvarIndex(TfToken const &interpolation, size_t pointIndex,
                     size_t cornerIndex, size_t faceIndex)
{
    if (interpolation == HdPrimvarSchemaTokens->faceVarying) return cornerIndex;
    if (interpolation == HdPrimvarSchemaTokens->uniform) return faceIndex;
    if (interpolation == HdPrimvarSchemaTokens->constant) return 0;
    return pointIndex;
}

struct Snapshot {
    struct Prototype {
        RTCScene scene = nullptr;
        Mesh *mesh = nullptr;
        std::vector<int> coarseFaces;
        SceneManager::PlacementMaterialBinding binding;
        size_t geometryHash = 0;
    };
    RTCDevice device = nullptr;
    std::unique_ptr<SceneManager> scene;
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::unordered_map<SdfPath, std::shared_ptr<Material>, SdfPath::Hash>
        resolvedMaterials;
    std::unique_ptr<ColorTexture> fallbackTexture;
    std::unique_ptr<Diffuse> fallbackMaterial;
    std::vector<std::unique_ptr<ColorTexture>> displayColorTextures;
    std::vector<std::unique_ptr<Diffuse>> displayColorMaterials;
    std::unordered_map<SdfPath, Material *, SdfPath::Hash> displayMaterialsByPath;
    std::unique_ptr<Texture<Color3>> environmentTexture;
    std::unique_ptr<Environment> environment;
    std::unordered_map<unsigned int, int> geomToPrimId;
    std::unordered_map<unsigned int, std::vector<int>> geomToCoarseFaces;
    std::unordered_map<unsigned int, int> geomToInstanceId;
    std::unordered_map<unsigned int, std::vector<InstanceLevel>>
        geomToInstanceChain;
    std::vector<SdfPath> primPaths;
    std::vector<SdfPath> instancerPaths;
    std::unordered_map<PrimKey, int, PrimKeyHash> primIds;
    size_t estimatedGeometryBytes = 0;
    uint64_t instanceCount = 0;
    uint64_t prototypeBuildCount = 0;
    double meshConversionSeconds = 0.0;
    double embreeGeometryCommitSeconds = 0.0;
    double embreeSceneCommitSeconds = 0.0;
    double sceneTraversalSeconds = 0.0;
    size_t domeCount = 0;
    std::unordered_set<std::string> warnedFeatures;
    std::unordered_set<SdfPath, SdfPath::Hash> warnedMotionPaths;
    std::unordered_map<SdfPath, Prototype, SdfPath::Hash> prototypes;

    ~Snapshot()
    {
        if (scene) {
            scene->geometry.clear();
            scene->intersection_geometry.clear();
            scene->placement_materials.clear();
            for (auto const &mesh : meshes) {
                if (mesh->geom) rtcReleaseGeometry(mesh->geom);
                delete mesh->transform;
                mesh->transform = nullptr;
            }
            if (scene->scene) rtcReleaseScene(scene->scene);
        }
        scene.reset();
        std::unordered_set<RTCScene> releasedScenes;
        for (auto const &[path, prototype] : prototypes)
            if (prototype.scene && releasedScenes.insert(prototype.scene).second)
                rtcReleaseScene(prototype.scene);
        prototypes.clear();
        meshes.clear();
    }
};

bool _IsSampled(HdSampledDataSourceHandle const &source)
{
    if (!source) return false;
    std::vector<HdSampledDataSource::Time> samples;
    return source->GetContributingSampleTimesForInterval(
        -1.0f, 1.0f, &samples) && samples.size() > 1;
}

std::shared_ptr<Material> _ResolveMaterial(
    HdSceneIndexBaseRefPtr const &sceneIndex, SdfPath const &materialPath,
    MaterialService *materials, Snapshot *snapshot)
{
    if (materialPath.IsEmpty() || !materials) return {};
    auto const found = snapshot->resolvedMaterials.find(materialPath);
    if (found != snapshot->resolvedMaterials.end()) return found->second;
    HdSceneIndexPrim const materialPrim = sceneIndex->GetPrim(materialPath);
    std::shared_ptr<Material> resolved = materials->Resolve(
        materialPrim.dataSource, materialPath);
    snapshot->resolvedMaterials.emplace(materialPath, resolved);
    return resolved;
}

Float3 _ToFloat3(GfVec3f const &v) { return {v[0], v[1], v[2]}; }

unsigned int _AttachPrototype(Snapshot *snapshot, SdfPath const &path,
                              Snapshot::Prototype const &prototype,
                              GfMatrix4d const &world)
{
    const auto commitStart = std::chrono::steady_clock::now();
    RTCGeometry instance = rtcNewGeometry(snapshot->device,
                                           RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(instance, prototype.scene);
    float transform[16];
    // GfMatrix uses row-vector notation; transpose into Embree's column-major
    // affine convention.
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            transform[column * 4 + row] = static_cast<float>(world[column][row]);
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                            transform);
    rtcCommitGeometry(instance);
    const unsigned int id = rtcAttachGeometry(snapshot->scene->scene, instance);
    rtcReleaseGeometry(instance);
    snapshot->scene->geometry.emplace(id, prototype.mesh);
    snapshot->scene->intersection_geometry.emplace(id, prototype.mesh->geom);
    SceneManager::PlacementTransform placement;
    const GfMatrix4d normal = world.GetInverse().GetTranspose();
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        placement.normal[row * 3 + column] = static_cast<float>(normal[row][column]);
        placement.tangent[row * 3 + column] = static_cast<float>(world[row][column]);
    }
    placement.mirrored = world.GetDeterminant() < 0.0;
    snapshot->scene->placement_transforms.emplace(id, placement);
    snapshot->scene->placement_materials.emplace(id, prototype.binding);
    snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - commitStart).count();
    return id;
}

bool _AppendMesh(HdSceneIndexBaseRefPtr const &sceneIndex,
                 SdfPath const &path, HdSceneIndexPrim const &prim,
                 MaterialService *materials, Snapshot *snapshot,
                 GfMatrix4d const &world, SdfPath const &instancerPath,
                 int instanceId, std::vector<InstanceLevel> const &instanceChain,
                 TfTokenVector const &renderTags)
{
    if (!_WorldVisible(sceneIndex, path) ||
        !_PurposeMatches(prim.dataSource, renderTags)) return false;
    HdMeshSchema meshSchema = HdMeshSchema::GetFromParent(prim.dataSource);
    HdMeshTopologySchema topology = meshSchema.GetTopology();
    HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
    if (!meshSchema || !topology || !primvars) return false;

    if (instanceId >= 0) {
        auto const cached = snapshot->prototypes.find(path);
        if (cached != snapshot->prototypes.end()) {
            const unsigned int geomId = _AttachPrototype(
                snapshot, path, cached->second, world);
            PrimKey const primKey{path, instancerPath};
            auto [it, inserted] = snapshot->primIds.emplace(
                primKey, static_cast<int>(snapshot->primPaths.size()));
            if (inserted) {
                snapshot->primPaths.push_back(path);
                snapshot->instancerPaths.push_back(instancerPath);
            }
            snapshot->geomToPrimId.emplace(geomId, it->second);
            snapshot->geomToCoarseFaces.emplace(
                geomId, cached->second.coarseFaces);
            snapshot->geomToInstanceId.emplace(geomId, instanceId);
            if (!instanceChain.empty())
                snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
            ++snapshot->instanceCount;
            return true;
        }
    }
    const auto conversionStart = std::chrono::steady_clock::now();

    if (snapshot->warnedMotionPaths.insert(path).second) {
        HdXformSchema const xform = HdXformSchema::GetFromParent(prim.dataSource);
        HdPrimvarSchema const pointsPrimvar =
            primvars.GetPrimvar(HdPrimvarsSchemaTokens->points);
        if ((xform && _IsSampled(xform.GetMatrix())) ||
            (pointsPrimvar && _IsSampled(pointsPrimvar.GetPrimvarValue()))) {
            TF_WARN("PrismRT: motion blur authored on <%s> is not supported; using time 0",
                    path.GetText());
        }
    }

    VtIntArray counts, indices, holes;
    VtVec3fArray points;
    if (!_ReadArray(topology.GetFaceVertexCounts(), &counts) ||
        !_ReadArray(topology.GetFaceVertexIndices(), &indices)) return false;
    _ReadArray(topology.GetHoleIndices(), &holes);
    PrimvarData pointsPv = _Primvar(primvars, HdPrimvarsSchemaTokens->points);
    if (!pointsPv.value.IsHolding<VtVec3fArray>()) return false;
    points = pointsPv.value.UncheckedGet<VtVec3fArray>();
    if (points.empty()) return false;

    PrimvarData normalPv = _Primvar(primvars, HdPrimvarsSchemaTokens->normals);
    PrimvarData uvPv = _Primvar(primvars, TfToken("st"));
    PrimvarData const alternateUvPv = _Primvar(primvars, TfToken("uv"));
    if (!uvPv.value.IsEmpty() && !alternateUvPv.value.IsEmpty() &&
        snapshot->warnedFeatures.insert("multipleUvSets").second) {
        TF_WARN("PrismRT: multiple UV sets are not supported; using primvar 'st'");
    }
    if (uvPv.value.IsEmpty()) uvPv = alternateUvPv;
    VtVec3fArray authoredNormals;
    VtVec2fArray authoredUvs;
    if (normalPv.value.IsHolding<VtVec3fArray>())
        authoredNormals = normalPv.value.UncheckedGet<VtVec3fArray>();
    if (uvPv.value.IsHolding<VtVec2fArray>())
        authoredUvs = uvPv.value.UncheckedGet<VtVec2fArray>();

    TfToken orientation = HdMeshTopologySchemaTokens->rightHanded;
    if (auto o = topology.GetOrientation()) orientation = o->GetTypedValue(0.0f);
    if (auto scheme = meshSchema.GetSubdivisionScheme()) {
        TfToken const value = scheme->GetTypedValue(0.0f);
        if (!value.IsEmpty() && value != TfToken("none") &&
            snapshot->warnedFeatures.insert("subdivisionControlHull").second)
            TF_WARN("PrismRT: subdivision refinement is unsupported; subdivision meshes are rendered as control hulls (first seen at <%s>)",
                    path.GetText());
    }
    const bool flip = orientation == HdMeshTopologySchemaTokens->leftHanded;
    const GfMatrix4d geometryTransform(1.0);
    const bool mirrored = geometryTransform.GetDeterminant() < 0.0;
    const GfMatrix4d normalMatrix = geometryTransform.GetInverse().GetTranspose();
    std::unordered_set<int> holeSet(holes.begin(), holes.end());

    auto mesh = std::make_unique<Mesh>();
    // Geometry predates RAII and allocates this raw pointer in-class. Keep it
    // guarded while conversion can still fail; Snapshot takes over only once
    // the fully built mesh is committed below.
    std::unique_ptr<TransformMatrix> transformOwner(mesh->transform);
    std::vector<int> coarseFaces;
    size_t corner = 0;
    size_t malformedFaceCount = 0;
    size_t degenerateFaceCount = 0;
    for (size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count < 3 || corner + count > indices.size()) {
            ++malformedFaceCount;
            corner += std::max(0, count);
            continue;
        }
        if (holeSet.count(static_cast<int>(face))) { corner += count; continue; }
        std::vector<GfVec3d> facePoints;
        facePoints.reserve(count);
        bool validFace = true;
        for (int k = 0; k < count; ++k) {
            int const pi = indices[corner + k];
            if (pi < 0 || static_cast<size_t>(pi) >= points.size()) {
                validFace = false;
                break;
            }
            facePoints.emplace_back(points[pi]);
        }
        if (!validFace) {
            ++malformedFaceCount;
            corner += count;
            continue;
        }
        std::vector<std::array<int, 3>> const triangles =
            TriangulatePolygon(facePoints);
        if (triangles.empty()) {
            ++degenerateFaceCount;
            corner += count;
            continue;
        }
        for (std::array<int, 3> const &triangle : triangles) {
            int local[3] = {triangle[0], triangle[1], triangle[2]};
            if (flip) std::swap(local[1], local[2]);
            GfVec3d wp[3];
            size_t first = mesh->vertices.size();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                wp[v] = geometryTransform.Transform(GfVec3d(points[pi]));
                mesh->vertices.emplace_back(static_cast<float>(wp[v][0]),
                                            static_cast<float>(wp[v][1]),
                                            static_cast<float>(wp[v][2]));
                const size_t ui = _PrimvarIndex(uvPv.interpolation, pi, ci, face);
                if (ui < authoredUvs.size()) mesh->texcoords.emplace_back(
                    authoredUvs[ui][0], authoredUvs[ui][1]);
                else mesh->texcoords.emplace_back(0.0f, 0.0f);
            }
            const GfVec3d geometric = GfCross(wp[1] - wp[0], wp[2] - wp[0]).GetNormalized();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                const size_t ni = _PrimvarIndex(normalPv.interpolation, pi, ci, face);
                GfVec3d n = geometric;
                if (ni < authoredNormals.size()) {
                    n = normalMatrix.TransformDir(GfVec3d(authoredNormals[ni])).GetNormalized();
                    if (flip != mirrored) n = -n;
                }
                mesh->normals.emplace_back(static_cast<float>(n[0]),
                                           static_cast<float>(n[1]),
                                           static_cast<float>(n[2]));
            }
            mesh->indices.push_back(static_cast<unsigned int>(first));
            mesh->indices.push_back(static_cast<unsigned int>(first + 1));
            mesh->indices.push_back(static_cast<unsigned int>(first + 2));
            coarseFaces.push_back(static_cast<int>(face));
        }
        corner += count;
    }
    if (malformedFaceCount)
        TF_WARN("PrismRT: mesh <%s> skipped %zu malformed faces",
                path.GetText(), malformedFaceCount);
    if (degenerateFaceCount)
        TF_WARN("PrismRT: mesh <%s> skipped %zu degenerate or non-triangulable faces",
                path.GetText(), degenerateFaceCount);
    if (mesh->indices.empty()) return false;

    Material *material = snapshot->fallbackMaterial.get();
    PrimvarData displayColor = _Primvar(primvars, TfToken("displayColor"));
    if (displayColor.value.IsHolding<VtVec3fArray>()) {
        VtVec3fArray const &colors =
            displayColor.value.UncheckedGet<VtVec3fArray>();
        if (!colors.empty()) {
            auto const existing = snapshot->displayMaterialsByPath.find(path);
            if (existing != snapshot->displayMaterialsByPath.end()) {
                material = existing->second;
            } else {
                GfVec3f const &value = colors.front();
                auto texture = std::make_unique<ColorTexture>(
                    Color3(value[0], value[1], value[2]));
                auto displayMaterial = std::make_unique<Diffuse>(texture.get());
                material = displayMaterial.get();
                snapshot->displayColorTextures.push_back(std::move(texture));
                snapshot->displayColorMaterials.push_back(std::move(displayMaterial));
                snapshot->displayMaterialsByPath.emplace(path, material);
            }
        }
    }
    const SdfPath materialPath = _MaterialPath(prim.dataSource);
    if (std::shared_ptr<Material> resolved = _ResolveMaterial(
            sceneIndex, materialPath, materials, snapshot))
        material = resolved.get();

    // Hydra 2 represents authored material subsets as geomSubset children of
    // the mesh.  Mesh::getMaterial indexes the Embree triangle id, so expand
    // each coarse face assignment to every triangle emitted by the fan above.
    // Faces outside an authored subset retain the mesh's base material.
    std::unordered_map<int, Material *> faceMaterials;
    for (SdfPath const &childPath : sceneIndex->GetChildPrimPaths(path)) {
        HdSceneIndexPrim const child = sceneIndex->GetPrim(childPath);
        HdGeomSubsetSchema const subset =
            HdGeomSubsetSchema::GetFromParent(child.dataSource);
        if (!subset) continue;
        if (auto type = subset.GetType()) {
            if (type->GetTypedValue(0.0f) !=
                HdGeomSubsetSchemaTokens->typeFaceSet) continue;
        }
        VtIntArray faces;
        if (!_ReadArray(subset.GetIndices(), &faces)) continue;
        const SdfPath subsetMaterialPath = _MaterialPath(child.dataSource);
        if (subsetMaterialPath.IsEmpty() || !materials) continue;
        std::shared_ptr<Material> resolved = _ResolveMaterial(
            sceneIndex, subsetMaterialPath, materials, snapshot);
        if (!resolved) continue;
        Material *const subsetMaterial = resolved.get();
        for (int const face : faces) {
            if (face >= 0 && static_cast<size_t>(face) < counts.size())
                faceMaterials[face] = subsetMaterial;
        }
    }
    if (faceMaterials.empty()) {
        mesh->material_override = material;
    } else {
        mesh->material_override = nullptr;
        std::unordered_map<Material *, int> materialIds;
        auto materialId = [&](Material *m) {
            auto const found = materialIds.find(m);
            if (found != materialIds.end()) return found->second;
            int const id = static_cast<int>(materialIds.size());
            materialIds.emplace(m, id);
            mesh->materials.emplace(id, m);
            return id;
        };
        int const baseMaterialId = materialId(material);
        std::unordered_map<int, int> faceMaterialIds;
        for (auto const &[face, faceMaterial] : faceMaterials)
            faceMaterialIds.emplace(face, materialId(faceMaterial));
        std::vector<int> const triangleMaterialIds = ExpandFaceAssignments(
            coarseFaces, faceMaterialIds, baseMaterialId);
        for (size_t triangle = 0; triangle < triangleMaterialIds.size(); ++triangle) {
            mesh->material_mapping.emplace(static_cast<int>(triangle),
                                           triangleMaterialIds[triangle]);
        }
    }
    unsigned int geomId = RTC_INVALID_GEOMETRY_ID;
    SceneManager::PlacementMaterialBinding placementBinding;
    placementBinding.fallback = material;
    if (mesh->material_override) placementBinding.fallback = mesh->material_override;
    else for (auto const &[triangle, materialId] : mesh->material_mapping) {
        auto it = mesh->materials.find(materialId);
        if (it != mesh->materials.end())
            placementBinding.triangles.emplace(triangle, it->second);
    }
    snapshot->meshConversionSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - conversionStart).count();
    mesh->ensureTangents();
    auto hashCombine = [](size_t &seed, auto const &value) {
        const size_t valueHash = std::hash<std::decay_t<decltype(value)>>{}(value);
        seed ^= valueHash + size_t{0x9e3779b9} + (seed << 6) + (seed >> 2);
    };
    size_t geometryHash = 0;
    hashCombine(geometryHash, mesh->vertices.size());
    hashCombine(geometryHash, mesh->normals.size());
    hashCombine(geometryHash, mesh->texcoords.size());
    hashCombine(geometryHash, mesh->tangents.size());
    hashCombine(geometryHash, mesh->indices.size());
    for (Float3 const &v : mesh->vertices) {
        hashCombine(geometryHash, v.x()); hashCombine(geometryHash, v.y());
        hashCombine(geometryHash, v.z());
    }
    for (Float3 const &n : mesh->normals) {
        hashCombine(geometryHash, n.x()); hashCombine(geometryHash, n.y());
        hashCombine(geometryHash, n.z());
    }
    for (Float2 const &uv : mesh->texcoords) {
        hashCombine(geometryHash, uv.u()); hashCombine(geometryHash, uv.v());
    }
    for (float const value : mesh->tangents) hashCombine(geometryHash, value);
    for (unsigned int const index : mesh->indices) hashCombine(geometryHash, index);
    auto equalGeometry = [&](Mesh const *other) {
        if (!other || mesh->vertices.size() != other->vertices.size() ||
            mesh->normals.size() != other->normals.size() ||
            mesh->texcoords.size() != other->texcoords.size() ||
            mesh->tangents != other->tangents ||
            mesh->indices != other->indices) return false;
        auto same3 = [](Float3 const &a, Float3 const &b) {
            return a.x()==b.x() && a.y()==b.y() && a.z()==b.z(); };
        for (size_t i=0;i<mesh->vertices.size();++i)
            if (!same3(mesh->vertices[i], other->vertices[i])) return false;
        for (size_t i=0;i<mesh->normals.size();++i)
            if (!same3(mesh->normals[i], other->normals[i])) return false;
        for (size_t i=0;i<mesh->texcoords.size();++i)
            if (mesh->texcoords[i].u()!=other->texcoords[i].u() ||
                mesh->texcoords[i].v()!=other->texcoords[i].v()) return false;
        return true;
    };
    Snapshot::Prototype const *shared = nullptr;
    for (auto const &[otherPath, other] : snapshot->prototypes)
        if (other.geometryHash == geometryHash && equalGeometry(other.mesh)) {
            shared = &other;
            break;
        }
    RTCScene prototypeScene = nullptr;
    Mesh *prototypeMesh = nullptr;
    if (shared) {
        prototypeScene = shared->scene;
        prototypeMesh = shared->mesh;
    } else {
        const auto prototypeCommitStart = std::chrono::steady_clock::now();
        prototypeScene = rtcNewScene(snapshot->device);
        mesh->setupEmbree(*snapshot->scene->device, prototypeScene);
        rtcCommitScene(prototypeScene);
        snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - prototypeCommitStart).count();
        prototypeMesh = mesh.get();
    }
    Snapshot::Prototype prototype{prototypeScene, prototypeMesh, coarseFaces,
                                  placementBinding, geometryHash};
    snapshot->prototypes.emplace(path, prototype);
    geomId = _AttachPrototype(snapshot, path, prototype, world);
    PrimKey const primKey{path, instancerPath};
    auto const existingPrim = snapshot->primIds.find(primKey);
    int primId = existingPrim == snapshot->primIds.end()
        ? -1 : existingPrim->second;
    if (primId < 0) {
        primId = static_cast<int>(snapshot->primPaths.size());
        snapshot->primPaths.push_back(path);
        snapshot->instancerPaths.push_back(instancerPath);
        snapshot->primIds.emplace(primKey, primId);
    }
    snapshot->geomToPrimId.emplace(geomId, primId);
    snapshot->geomToCoarseFaces.emplace(geomId, std::move(coarseFaces));
    snapshot->geomToInstanceId.emplace(geomId, instanceId);
    if (!instanceChain.empty())
        snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
    if (!shared) snapshot->estimatedGeometryBytes +=
        mesh->vertices.size() * sizeof(Float3) +
        mesh->normals.size() * sizeof(Float3) +
        mesh->texcoords.size() * sizeof(Float2) +
        mesh->indices.size() * sizeof(unsigned int) +
        mesh->tangents.size() * sizeof(float);
    if (instanceId >= 0) ++snapshot->instanceCount;
    if (!shared) {
        ++snapshot->prototypeBuildCount;
        snapshot->meshes.push_back(std::move(mesh));
        transformOwner.release();
    }
    return true;
}

struct InstancePlacement {
    GfMatrix4d transform{1.0};
    SdfPath instancerPath;
    int instanceId = -1;
    std::vector<InstanceLevel> chain;
};

template<class T>
bool _ArrayValue(VtValue const &value, int index, T *result)
{
    if (!value.IsHolding<VtArray<T>>() || index < 0) return false;
    VtArray<T> const &a = value.UncheckedGet<VtArray<T>>();
    if (static_cast<size_t>(index) >= a.size()) return false;
    *result = a[index];
    return true;
}

std::vector<InstancePlacement> _InstancePlacements(
    HdSceneIndexBaseRefPtr const &scene, SdfPath const &prototypePath,
    HdContainerDataSourceHandle const &prototypeDataSource, int depth = 0)
{
    std::vector<InstancePlacement> result;
    if (depth > 16) {
        TF_WARN("PrismRT: instancing depth exceeds 16 at <%s>", prototypePath.GetText());
        return result;
    }
    HdInstancedBySchema instancedBy =
        HdInstancedBySchema::GetFromParent(prototypeDataSource);
    if (!instancedBy || !instancedBy.GetPaths()) return result;
    const VtArray<SdfPath> paths = instancedBy.GetPaths()->GetTypedValue(0.0f);
    const VtArray<SdfPath> roots = instancedBy.GetPrototypeRoots()
        ? instancedBy.GetPrototypeRoots()->GetTypedValue(0.0f)
        : VtArray<SdfPath>();
    for (size_t p = 0; p < paths.size(); ++p) {
        HdSceneIndexPrim instancerPrim = scene->GetPrim(paths[p]);
        if (!_WorldVisible(scene, paths[p])) continue;
        HdInstancerTopologySchema topology =
            HdInstancerTopologySchema::GetFromParent(instancerPrim.dataSource);
        if (!topology) continue;
        const SdfPath topologyPrototype =
            p < roots.size() && !roots[p].IsEmpty() ? roots[p] : prototypePath;
        const VtArray<int> indices =
            topology.ComputeInstanceIndicesForProto(topologyPrototype);
        // Keep prototype hierarchy transforms local to this instancing level.
        // The current instancer's world placement is supplied below, either
        // directly or by its recursively computed parent placement.
        GfMatrix4d const prototypeToRoot = _RelativeTransform(
            scene, prototypePath, topologyPrototype.GetParentPath());
        HdPrimvarsSchema primvars =
            HdPrimvarsSchema::GetFromParent(instancerPrim.dataSource);
        const VtValue translations = _Primvar(
            primvars, HdInstancerTokens->instanceTranslations).value;
        const VtValue rotations = _Primvar(
            primvars, HdInstancerTokens->instanceRotations).value;
        const VtValue scales = _Primvar(
            primvars, HdInstancerTokens->instanceScales).value;
        const VtValue transforms = _Primvar(
            primvars, HdInstancerTokens->instanceTransforms).value;
        std::vector<InstancePlacement> local;
        for (size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
            int const sourceIndex = indices[ordinal];
            if (sourceIndex < 0) continue;
            GfMatrix4d transform(1.0);
            GfVec3f v3f;
            GfVec3d v3d;
            if (_ArrayValue(translations, sourceIndex, &v3f) ||
                (_ArrayValue(translations, sourceIndex, &v3d) &&
                 (v3f = GfVec3f(v3d), true))) {
                GfMatrix4d m(1.0); m.SetTranslate(GfVec3d(v3f)); transform = m * transform;
            }
            GfQuatf qf; GfQuath qh; GfQuatd qd; GfVec4f qv;
            GfMatrix4d rotation(1.0);
            bool hasRotation = false;
            if (_ArrayValue(rotations, sourceIndex, &qf)) {
                rotation.SetRotate(GfQuatd(qf)); hasRotation = true;
            } else if (_ArrayValue(rotations, sourceIndex, &qh)) {
                rotation.SetRotate(GfQuatd(qh)); hasRotation = true;
            } else if (_ArrayValue(rotations, sourceIndex, &qd)) {
                rotation.SetRotate(qd); hasRotation = true;
            } else if (_ArrayValue(rotations, sourceIndex, &qv)) {
                rotation.SetRotate(GfQuatd(qv[0], qv[1], qv[2], qv[3])); hasRotation = true;
            }
            if (hasRotation) transform = rotation * transform;
            if (_ArrayValue(scales, sourceIndex, &v3f) ||
                (_ArrayValue(scales, sourceIndex, &v3d) &&
                 (v3f = GfVec3f(v3d), true))) {
                GfMatrix4d m(1.0); m.SetScale(GfVec3d(v3f)); transform = m * transform;
            }
            GfMatrix4d m4d; GfMatrix4f m4f;
            if (_ArrayValue(transforms, sourceIndex, &m4d)) transform = m4d * transform;
            else if (_ArrayValue(transforms, sourceIndex, &m4f))
                transform = GfMatrix4d(m4f) * transform;
            InstancePlacement placement;
            placement.transform = prototypeToRoot * transform;
            placement.instancerPath = paths[p];
            // HdxPickHit::instanceIndex is a mixed-radix ordinal into each
            // prototype's instance-indices array, not the authored source id.
            placement.instanceId = static_cast<int>(ordinal);
            placement.chain.push_back({paths[p], sourceIndex,
                static_cast<int>(ordinal), static_cast<int>(indices.size())});
            local.push_back(std::move(placement));
        }
        const bool hasParent = bool(
            HdInstancedBySchema::GetFromParent(instancerPrim.dataSource));
        std::vector<InstancePlacement> parents = _InstancePlacements(
            scene, paths[p], instancerPrim.dataSource, depth + 1);
        if (!hasParent) {
            GfMatrix4d const instancerWorld = _WorldTransform(scene, paths[p]);
            for (InstancePlacement placement : local) {
                placement.transform *= instancerWorld;
                result.push_back(std::move(placement));
            }
        } else {
            for (InstancePlacement const &parent : parents) {
                for (InstancePlacement const &child : local) {
                    InstancePlacement nested = child;
                    nested.transform = child.transform * parent.transform;
                    nested.instanceId = parent.instanceId *
                        static_cast<int>(indices.size()) + child.instanceId;
                    nested.chain = parent.chain;
                    nested.chain.insert(nested.chain.end(),
                        child.chain.begin(), child.chain.end());
                    // Keep the innermost instancer as the immediate owner of
                    // the prototype; the full outer-to-inner chain is retained
                    // separately for selection translation and diagnostics.
                    result.push_back(std::move(nested));
                }
            }
        }
    }
    return result;
}

template<class T>
T _ContainerValue(HdContainerDataSourceHandle const &container,
                  TfToken const &name, T const &fallback)
{
    if (!container) return fallback;
    if (HdSampledDataSourceHandle source =
            HdSampledDataSource::Cast(container->Get(name))) {
        VtValue value = source->GetValue(0.0f);
        if (value.IsHolding<T>()) return value.UncheckedGet<T>();
    }
    return fallback;
}

void _SetDomeEnvironment(HdSceneIndexBaseRefPtr const &scene,
                         SdfPath const &path, HdSceneIndexPrim const &prim,
                         Snapshot *snapshot)
{
    if (!_WorldVisible(scene, path)) return;
    if (snapshot->domeCount++ > 0 &&
        snapshot->warnedFeatures.insert("multipleDomes").second) {
        TF_WARN("PrismRT: multiple dome lights are not yet combined; using the last dome");
    }
    HdLightSchema schema = HdLightSchema::GetFromParent(prim.dataSource);
    HdContainerDataSourceHandle light = schema.GetContainer();
    if (!light) return;
    const GfVec3f color = _ContainerValue(
        light, HdLightTokens->color, GfVec3f(1.0f));
    const float intensity = _ContainerValue(light, HdLightTokens->intensity, 1.0f);
    const float exposure = _ContainerValue(light, HdLightTokens->exposure, 0.0f);
    const float scale = intensity * std::exp2(exposure);
    const Color3 multiplier(color[0] * scale, color[1] * scale, color[2] * scale);
    const SdfAssetPath asset = _ContainerValue(
        light, HdLightTokens->textureFile, SdfAssetPath());
    std::string resolved = asset.GetResolvedPath();
    if (resolved.empty() && !asset.GetAssetPath().empty())
        resolved = ArGetResolver().Resolve(asset.GetAssetPath()).GetPathString();
    try {
        if (!resolved.empty() && std::filesystem::exists(resolved)) {
            snapshot->environmentTexture =
                std::make_unique<ImageTexture<Color3>>(resolved);
        } else {
            snapshot->environmentTexture =
                std::make_unique<ColorTexture>(Color3(1.0f));
            if (!asset.GetAssetPath().empty())
                TF_WARN("PrismRT: could not resolve dome texture '%s'", asset.GetAssetPath().c_str());
        }
        const GfMatrix4d worldToDome = _WorldTransform(scene, path).GetInverse();
        std::array<float, 9> linear{};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                linear[row * 3 + column] = static_cast<float>(
                    worldToDome[column][row]);
        snapshot->environment = std::make_unique<TransformedEnvironmentMap>(
            snapshot->environmentTexture.get(), multiplier, linear);
        snapshot->scene->environment = snapshot->environment.get();
    } catch (std::exception const &e) {
        TF_WARN("PrismRT: failed to load dome environment: %s", e.what());
    }
}

void _Walk(HdSceneIndexBaseRefPtr const &scene, SdfPath const &path,
           MaterialService *materials, Snapshot *snapshot,
           TfTokenVector const &renderTags)
{
    HdSceneIndexPrim prim = scene->GetPrim(path);
    if (prim.primType == HdPrimTypeTokens->mesh) {
        const GfMatrix4d meshWorld = _WorldTransform(scene, path);
        const bool isInstanced = bool(
            HdInstancedBySchema::GetFromParent(prim.dataSource));
        std::vector<InstancePlacement> placements =
            _InstancePlacements(scene, path, prim.dataSource);
        if (placements.empty() && !isInstanced) {
            _AppendMesh(scene, path, prim, materials, snapshot,
                        meshWorld, SdfPath(), -1, {}, renderTags);
        } else {
            for (InstancePlacement const &placement : placements)
                _AppendMesh(scene, path, prim, materials, snapshot,
                            placement.transform,
                            placement.instancerPath, placement.instanceId,
                            placement.chain, renderTags);
        }
    }
    if (prim.primType == HdPrimTypeTokens->domeLight)
        _SetDomeEnvironment(scene, path, prim, snapshot);
    else if (HdLightSchema::GetFromParent(prim.dataSource) &&
             snapshot->warnedFeatures.insert("analyticLights").second) {
        TF_WARN("PrismRT: analytic USD lights are not supported; only dome lights are rendered");
    }
    static const std::unordered_set<std::string> unsupportedPrimTypes{
        "basisCurves", "nurbsCurves", "points", "volume", "tetMesh"};
    std::string const type = prim.primType.GetString();
    if (unsupportedPrimTypes.count(type) &&
        snapshot->warnedFeatures.insert("primType:" + type).second) {
        TF_WARN("PrismRT: prim type '%s' is not supported and will be skipped",
                type.c_str());
    }
    for (SdfPath const &child : scene->GetChildPrimPaths(path))
        _Walk(scene, child, materials, snapshot, renderTags);
}

void _CollectMaterialRequests(HdSceneIndexBaseRefPtr const &scene,
                              SdfPath const &path,
                              std::unordered_set<SdfPath, SdfPath::Hash> *seen,
                              std::vector<MaterialService::Request> *requests)
{
    HdSceneIndexPrim const prim = scene->GetPrim(path);
    const SdfPath materialPath = _MaterialPath(prim.dataSource);
    if (!materialPath.IsEmpty() && seen->insert(materialPath).second) {
        requests->push_back({scene->GetPrim(materialPath).dataSource,
                             materialPath});
    }
    for (SdfPath const &child : scene->GetChildPrimPaths(path))
        _CollectMaterialRequests(scene, child, seen, requests);
}

std::shared_ptr<Snapshot> _BuildSnapshot(HdSceneIndexBaseRefPtr const &scene,
                                         MaterialService *materials,
                                         RenderSettings const &settings,
                                         TfTokenVector const &renderTags,
                                         RTCDevice sharedDevice)
{
    auto result = std::make_shared<Snapshot>();
    result->device = sharedDevice;
    if (!result->device) return {};
    result->scene = std::make_unique<SceneManager>(result->device);
    result->scene->max_depth = settings.maxDepth;
    result->scene->rr_min_depth = settings.rouletteDepth;
    result->scene->environmentSampling =
        settings.environmentSampling == RenderSettings::EnvironmentSampling::Mis;
    result->fallbackTexture = std::make_unique<ColorTexture>(Color3(0.5f));
    result->fallbackMaterial = std::make_unique<Diffuse>(result->fallbackTexture.get());
    auto blackEnvironment = std::make_unique<ColorTexture>(Color3(0.0f));
    result->environment = std::make_unique<ConstantColorBackground>(
        blackEnvironment.get());
    result->environmentTexture = std::move(blackEnvironment);
    result->scene->environment = result->environment.get();
    if (materials) {
        std::unordered_set<SdfPath, SdfPath::Hash> seen;
        std::vector<MaterialService::Request> requests;
        _CollectMaterialRequests(scene, SdfPath::AbsoluteRootPath(),
                                 &seen, &requests);
        materials->Prefetch(requests);
    }
    const auto traversalStart = std::chrono::steady_clock::now();
    _Walk(scene, SdfPath::AbsoluteRootPath(), materials, result.get(), renderTags);
    result->sceneTraversalSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - traversalStart).count();
    const auto sceneCommitStart = std::chrono::steady_clock::now();
    result->scene->commitScene();
    result->embreeSceneCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sceneCommitStart).count();
    return result;
}

bool _UpdatePlacements(HdSceneIndexBaseRefPtr const &index, Snapshot *snapshot)
{
    if (!snapshot || !snapshot->scene) return false;
    RTCScene replacement = rtcNewScene(snapshot->device);
    if (!replacement) return false;
    auto oldPrim = snapshot->geomToPrimId;
    auto oldFaces = snapshot->geomToCoarseFaces;
    auto oldInstances = snapshot->geomToInstanceId;
    auto oldChains = snapshot->geomToInstanceChain;
    auto oldGeometry = snapshot->scene->geometry;
    snapshot->geomToPrimId.clear();
    snapshot->geomToCoarseFaces.clear();
    snapshot->geomToInstanceId.clear();
    snapshot->geomToInstanceChain.clear();
    snapshot->scene->geometry.clear();
    snapshot->scene->intersection_geometry.clear();
    snapshot->scene->placement_transforms.clear();
    snapshot->scene->placement_materials.clear();
    snapshot->instanceCount = 0;

    RTCScene oldScene = snapshot->scene->scene;
    snapshot->scene->scene = replacement;
    for (auto &[path, prototype] : snapshot->prototypes) {
        HdSceneIndexPrim prim = index->GetPrim(path);
        if (!_WorldVisible(index, path)) continue;
        std::vector<InstancePlacement> placements =
            _InstancePlacements(index, path, prim.dataSource);
        if (placements.empty() &&
            !HdInstancedBySchema::GetFromParent(prim.dataSource)) {
            placements.push_back({_WorldTransform(index, path), {}, -1, {}});
        }
        for (InstancePlacement const &placement : placements) {
            const unsigned int id = _AttachPrototype(
                snapshot, path, prototype, placement.transform);
            PrimKey key{path, placement.instancerPath};
            auto found = snapshot->primIds.find(key);
            if (found == snapshot->primIds.end()) continue;
            snapshot->geomToPrimId.emplace(id, found->second);
            snapshot->geomToCoarseFaces.emplace(id, prototype.coarseFaces);
            snapshot->geomToInstanceId.emplace(id, placement.instanceId);
            snapshot->geomToInstanceChain.emplace(id, placement.chain);
            if (placement.instanceId >= 0) ++snapshot->instanceCount;
        }
    }
    const auto commitStart = std::chrono::steady_clock::now();
    rtcCommitScene(replacement);
    snapshot->embreeSceneCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - commitStart).count();
    rtcReleaseScene(oldScene);
    return true;
}

bool _RefreshPrototypes(HdSceneIndexBaseRefPtr const &index,
                        std::unordered_set<SdfPath, SdfPath::Hash> const &paths,
                        MaterialService *materials, Snapshot *snapshot,
                        TfTokenVector const &renderTags)
{
    if (!snapshot || !snapshot->scene || paths.empty()) return false;
    RTCScene oldTop = snapshot->scene->scene;
    snapshot->scene->scene = rtcNewScene(snapshot->device);
    if (!snapshot->scene->scene) {
        snapshot->scene->scene = oldTop;
        return false;
    }
    snapshot->scene->geometry.clear();
    snapshot->scene->intersection_geometry.clear();
    snapshot->scene->placement_transforms.clear();
    snapshot->scene->placement_materials.clear();
    snapshot->geomToPrimId.clear();
    snapshot->geomToCoarseFaces.clear();
    snapshot->geomToInstanceId.clear();
    snapshot->geomToInstanceChain.clear();
    rtcReleaseScene(oldTop);

    for (SdfPath const &path : paths) {
        // displayColor is a material input carried by a geometry primvar.  A
        // prototype refresh must recapture its value instead of reusing the
        // path cache populated by the previous data source.
        snapshot->displayMaterialsByPath.erase(path);
        auto found = snapshot->prototypes.find(path);
        if (found != snapshot->prototypes.end()) {
            Mesh *const oldMesh = found->second.mesh;
            RTCScene const oldPrototypeScene = found->second.scene;
            snapshot->prototypes.erase(found);
            const bool stillShared = std::any_of(
                snapshot->prototypes.begin(), snapshot->prototypes.end(),
                [&](auto const &entry) { return entry.second.mesh == oldMesh; });
            auto meshIt = std::find_if(snapshot->meshes.begin(), snapshot->meshes.end(),
                [&](std::unique_ptr<Mesh> const &mesh) { return mesh.get() == oldMesh; });
            if (!stillShared && meshIt != snapshot->meshes.end()) {
                snapshot->estimatedGeometryBytes -=
                    (*meshIt)->vertices.size() * sizeof(Float3) +
                    (*meshIt)->normals.size() * sizeof(Float3) +
                    (*meshIt)->texcoords.size() * sizeof(Float2) +
                    (*meshIt)->indices.size() * sizeof(unsigned int) +
                    (*meshIt)->tangents.size() * sizeof(float);
                if ((*meshIt)->geom) rtcReleaseGeometry((*meshIt)->geom);
                delete (*meshIt)->transform;
                (*meshIt)->transform = nullptr;
                snapshot->meshes.erase(meshIt);
            }
            if (!stillShared && oldPrototypeScene) rtcReleaseScene(oldPrototypeScene);
        }

        HdSceneIndexPrim const prim = index->GetPrim(path);
        if (prim.primType != HdPrimTypeTokens->mesh) continue;
        std::vector<InstancePlacement> placements =
            _InstancePlacements(index, path, prim.dataSource);
        const bool instanced = bool(
            HdInstancedBySchema::GetFromParent(prim.dataSource));
        if (placements.empty() && !instanced) {
            _AppendMesh(index, path, prim, materials, snapshot,
                        _WorldTransform(index, path), {}, -1, {}, renderTags);
        } else {
            for (InstancePlacement const &placement : placements)
                _AppendMesh(index, path, prim, materials, snapshot,
                            placement.transform, placement.instancerPath,
                            placement.instanceId, placement.chain, renderTags);
        }
    }
    // Recreate one coherent top-level scene from all retained and rebuilt
    // prototypes, discarding temporary attachments made during conversion.
    return _UpdatePlacements(index, snapshot);
}

void _RefreshEnvironment(HdSceneIndexBaseRefPtr const &index,
                         SdfPath const &path, Snapshot *snapshot)
{
    HdSceneIndexPrim const prim = index->GetPrim(path);
    if (prim.primType == HdPrimTypeTokens->domeLight)
        _SetDomeEnvironment(index, path, prim, snapshot);
    for (SdfPath const &child : index->GetChildPrimPaths(path))
        _RefreshEnvironment(index, child, snapshot);
}

void _RefreshMaterials(HdSceneIndexBaseRefPtr const &index,
                       MaterialService *materials, Snapshot *snapshot)
{
    if (!snapshot) return;
    snapshot->resolvedMaterials.clear();
    for (auto &[path, prototype] : snapshot->prototypes) {
        HdSceneIndexPrim const prim = index->GetPrim(path);
        Material *base = snapshot->fallbackMaterial.get();
        HdPrimvarsSchema primvars =
            HdPrimvarsSchema::GetFromParent(prim.dataSource);
        PrimvarData color = _Primvar(primvars, TfToken("displayColor"));
        if (color.value.IsHolding<VtVec3fArray>() &&
            !color.value.UncheckedGet<VtVec3fArray>().empty()) {
            auto found = snapshot->displayMaterialsByPath.find(path);
            if (found != snapshot->displayMaterialsByPath.end())
                base = found->second;
        }
        if (auto resolved = _ResolveMaterial(
                index, _MaterialPath(prim.dataSource), materials, snapshot))
            base = resolved.get();
        std::unordered_map<int, Material *> faceMaterials;
        for (SdfPath const &childPath : index->GetChildPrimPaths(path)) {
            HdSceneIndexPrim const child = index->GetPrim(childPath);
            HdGeomSubsetSchema const subset =
                HdGeomSubsetSchema::GetFromParent(child.dataSource);
            if (!subset) continue;
            VtIntArray faces;
            if (!_ReadArray(subset.GetIndices(), &faces)) continue;
            if (auto resolved = _ResolveMaterial(
                    index, _MaterialPath(child.dataSource), materials, snapshot))
                for (int face : faces) if (face >= 0)
                    faceMaterials[face] = resolved.get();
        }
        prototype.binding = {};
        prototype.binding.fallback = base;
        if (faceMaterials.empty()) {
            continue;
        }
        for (size_t triangle = 0; triangle < prototype.coarseFaces.size(); ++triangle) {
            auto material = faceMaterials.find(prototype.coarseFaces[triangle]);
            if (material != faceMaterials.end())
                prototype.binding.triangles.emplace(
                    static_cast<int>(triangle), material->second);
        }
    }
    for (auto const &[geomId, primId] : snapshot->geomToPrimId) {
        if (primId < 0 || static_cast<size_t>(primId) >= snapshot->primPaths.size())
            continue;
        auto prototype = snapshot->prototypes.find(snapshot->primPaths[primId]);
        if (prototype != snapshot->prototypes.end())
            snapshot->scene->placement_materials[geomId] =
                prototype->second.binding;
    }
}

struct RayPair { RTCRay sampled; RTCRay center; };

RayPair _CameraRays(CameraState const &camera, unsigned int x, unsigned int y,
                    unsigned int width, unsigned int height, unsigned int &seed)
{
    const GfMatrix4d cameraToWorld = camera.worldToView.GetInverse();
    const GfMatrix4d inverseProjection = camera.projection.GetInverse();
    auto makeRay = [&](float sx, float sy, bool lens) {
        const double nx = 2.0 * sx / width - 1.0;
        const double ny = 1.0 - 2.0 * sy / height;
        GfVec4d p(nx, ny, -1.0, 1.0);
        p = p * inverseProjection;
        if (std::abs(p[3]) > 1e-12) p /= p[3];
        GfVec3d originCamera(0.0), directionCamera;
        if (camera.orthographic) {
            originCamera = GfVec3d(p[0], p[1], 0.0);
            directionCamera = GfVec3d(0, 0, -1);
        } else directionCamera = GfVec3d(p[0], p[1], p[2]).GetNormalized();

        if (lens && !camera.orthographic && camera.fStop > 0.0f &&
            camera.focusDistance > 0.0f) {
            Float3 disk = random_in_unit_disk(seed);
            const double radius = camera.focalLength * 0.1 / (2.0 * camera.fStop);
            const double focusT = camera.focusDistance /
                std::max(1e-8, -directionCamera[2]);
            const GfVec3d focus = directionCamera * focusT;
            originCamera = GfVec3d(disk.x() * radius, disk.y() * radius, 0.0);
            directionCamera = (focus - originCamera).GetNormalized();
        }
        GfVec3d o = cameraToWorld.Transform(originCamera);
        GfVec3d d = cameraToWorld.TransformDir(directionCamera).GetNormalized();
        RTCRay ray = initialize_ray(
            Float3(static_cast<float>(o[0]), static_cast<float>(o[1]), static_cast<float>(o[2])),
            Float3(static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2])));
        const float dz = std::max(1e-8f, static_cast<float>(-directionCamera[2]));
        ray.tnear = camera.nearClip / dz;
        ray.tfar = camera.farClip / dz;
        return ray;
    };
    const float sx = static_cast<float>(x) + rnd(seed);
    const float sy = static_cast<float>(y) + rnd(seed);
    return {makeRay(sx, sy, true),
            makeRay(static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f, false)};
}

struct PreviewResult {
    unsigned int width = 0;
    unsigned int height = 0;
    bool published = false;
};

PreviewResult _RenderPreview(std::shared_ptr<Snapshot> const &snapshot,
    CameraState const &camera, RenderSettings const &settings,
    HdRenderPassAovBindingVector const &bindings,
    unsigned int width, unsigned int height)
{
    PreviewResult result;
    if (!snapshot || !snapshot->scene || width == 0 || height == 0)
        return result;

    const unsigned int maxPreviewEdge = static_cast<unsigned int>(
        std::clamp(settings.previewMaxDimension, 16, 1024));
    if (width >= height) {
        result.width = std::min(width, maxPreviewEdge);
        result.height = std::max(1u, static_cast<unsigned int>(
            (static_cast<uint64_t>(height) * result.width + width / 2) / width));
    } else {
        result.height = std::min(height, maxPreviewEdge);
        result.width = std::max(1u, static_cast<unsigned int>(
            (static_cast<uint64_t>(width) * result.height + height / 2) / height));
    }

    struct Target {
        PrismRenderBuffer *buffer = nullptr;
        TfToken name;
        size_t components = 0;
        std::vector<float> floats;
        std::vector<int> ints;
    };
    std::vector<Target> targets;
    bool needPrimary = false;
    const size_t pixels = static_cast<size_t>(result.width) * result.height;
    for (HdRenderPassAovBinding const &binding : bindings) {
        auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer);
        if (!buffer) continue;
        Target target;
        target.buffer = buffer;
        target.name = binding.aovName;
        if (binding.aovName == HdAovTokens->color) {
            target.components = 4; target.floats.resize(pixels * 4);
        } else if (binding.aovName == HdAovTokens->normal ||
                   binding.aovName == HdAovTokens->Neye) {
            target.components = 3; target.floats.resize(pixels * 3);
            needPrimary = true;
        } else if (binding.aovName == HdAovTokens->depth ||
                   binding.aovName == HdAovTokens->cameraDepth) {
            target.components = 1; target.floats.resize(pixels);
            needPrimary = true;
        } else if (binding.aovName == HdAovTokens->primId ||
                   binding.aovName == HdAovTokens->elementId ||
                   binding.aovName == HdAovTokens->instanceId) {
            target.components = 1; target.ints.assign(pixels, -1);
            needPrimary = true;
        } else {
            continue;
        }
        targets.push_back(std::move(target));
    }
    if (targets.empty()) return result;

    tbb::parallel_for(
        tbb::blocked_range2d<unsigned int>(
            0, result.height, 4, 0, result.width, 8),
        [&](tbb::blocked_range2d<unsigned int> const &range) {
    for (unsigned int py = range.rows().begin();
         py != range.rows().end(); ++py) {
        const unsigned int y = std::min(height - 1,
            static_cast<unsigned int>((static_cast<uint64_t>(2 * py + 1) *
                                       height) / (2 * result.height)));
        for (unsigned int px = range.cols().begin();
             px != range.cols().end(); ++px) {
            const unsigned int x = std::min(width - 1,
                static_cast<unsigned int>((static_cast<uint64_t>(2 * px + 1) *
                                           width) / (2 * result.width)));
            unsigned int seed = tea<64>(
                ((py * result.width + px) ^ settings.seed) ^ 0x9e3779b9u,
                (px + 0x85ebca6bu) ^ (py * 0xc2b2ae35u));
            RayPair rays = _CameraRays(camera, x, y, width, height, seed);
            Color3 color = snapshot->scene->trace(
                rays.sampled, 0, Color3(1.0f), seed);
            if (settings.sampleClamp > 0.0f) {
                const float peak = color.max_component();
                if (peak > settings.sampleClamp)
                    color *= settings.sampleClamp / peak;
            }
            std::optional<RayHit> primary;
            if (needPrimary)
                primary.emplace(snapshot->scene->cast_ray(rays.center));
            const size_t pixel =
                (static_cast<size_t>(result.height - 1 - py) * result.width + px);

            for (Target &target : targets) {
                if (target.name == HdAovTokens->color) {
                    float *dst = target.floats.data() + pixel * 4;
                    dst[0] = color.x(); dst[1] = color.y();
                    dst[2] = color.z(); dst[3] = 1.0f;
                    continue;
                }
                if (!primary) continue;
                const bool hit = primary->geomid != RTC_INVALID_GEOMETRY_ID;
                if (target.name == HdAovTokens->depth ||
                    target.name == HdAovTokens->cameraDepth) {
                    float value = target.name == HdAovTokens->depth
                        ? 1.0f : std::numeric_limits<float>::infinity();
                    if (hit) {
                        GfVec4d view(primary->p.x(), primary->p.y(),
                                     primary->p.z(), 1.0);
                        view = view * camera.worldToView;
                        if (target.name == HdAovTokens->cameraDepth) {
                            value = static_cast<float>(-view[2]);
                        } else {
                            GfVec4d clip = view * camera.projection;
                            if (std::abs(clip[3]) > 1e-12)
                                value = static_cast<float>(
                                    0.5 * clip[2] / clip[3] + 0.5);
                        }
                    }
                    target.floats[pixel] = value;
                } else if (target.name == HdAovTokens->normal ||
                           target.name == HdAovTokens->Neye) {
                    Float3 normal = primary->n;
                    if (target.name == HdAovTokens->Neye && hit) {
                        GfVec3d eye = camera.worldToView.TransformDir(
                            GfVec3d(normal.x(), normal.y(), normal.z())).GetNormalized();
                        normal = Float3(static_cast<float>(eye[0]),
                                        static_cast<float>(eye[1]),
                                        static_cast<float>(eye[2]));
                    }
                    float *dst = target.floats.data() + pixel * 3;
                    dst[0] = normal.x(); dst[1] = normal.y(); dst[2] = normal.z();
                } else {
                    int value = -1;
                    if (hit && target.name == HdAovTokens->primId) {
                        auto it = snapshot->geomToPrimId.find(primary->geomid);
                        if (it != snapshot->geomToPrimId.end()) value = it->second;
                    } else if (hit && target.name == HdAovTokens->elementId) {
                        auto it = snapshot->geomToCoarseFaces.find(primary->geomid);
                        if (it != snapshot->geomToCoarseFaces.end() &&
                            primary->primid >= 0 &&
                            static_cast<size_t>(primary->primid) < it->second.size())
                            value = it->second[primary->primid];
                    } else if (hit && target.name == HdAovTokens->instanceId) {
                        auto it = snapshot->geomToInstanceId.find(primary->geomid);
                        if (it != snapshot->geomToInstanceId.end()) value = it->second;
                    }
                    target.ints[pixel] = value;
                }
            }
        }
    }
        });

    for (Target const &target : targets) {
        if (!target.ints.empty())
            target.buffer->PublishPreviewInt(result.width, result.height,
                                              target.ints, target.components);
        else
            target.buffer->PublishPreviewFloat(result.width, result.height,
                                                target.floats, target.components);
    }
    result.published = true;
    return result;
}

} // namespace

class SceneBridge::Impl
{
public:
    Impl(HdSceneIndexBaseRefPtr index, HdRenderThread *thread)
        : sceneIndex(std::move(index)), renderThread(thread), device(rtcNewDevice(nullptr))
    {
        if (const char *value = std::getenv("PRISM_PROFILE_BUFFERS"))
            profileBufferWrites = value[0] != '\0' && std::strcmp(value, "0") != 0;
        try {
            mdl::Context &context = SharedMdlContext();
            if (context.is_valid()) {
                const char *mx = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
                const char *mdl = std::getenv("PRISM_MATERIALX_MDL");
                materialService = std::make_unique<MaterialService>(
                    &context, mx ? mx : "", mdl ? mdl : "");
            }
        } catch (std::exception const &e) {
            TF_WARN("PrismRT: MDL initialization failed: %s", e.what());
        }
    }

    ~Impl()
    {
        snapshot.reset();
        if (device) rtcReleaseDevice(device);
    }

    HdSceneIndexBaseRefPtr sceneIndex;
    HdRenderThread *renderThread;
    RTCDevice device = nullptr;
    mutable std::mutex mutex;
    bool sceneDirty = true;
    bool stateDirty = true;
    unsigned int dirtyKinds = static_cast<unsigned int>(SceneDirtyKind::Unknown);
    std::unordered_set<SdfPath, SdfPath::Hash> dirtyPaths;
    RenderSettings settings;
    CameraState camera;
    HdRenderPassAovBindingVector bindings;
    TfTokenVector renderTags;
    unsigned int width = 0, height = 0;
    std::shared_ptr<Snapshot> snapshot;
    std::unique_ptr<MaterialService> materialService;
    std::chrono::steady_clock::time_point startTime;
    std::atomic<uint64_t> samples{0};
    uint64_t snapshotBuilds = 0;
    double snapshotBuildSeconds = 0.0;
    double renderSeconds = 0.0;
    uint64_t previewPublishes = 0;
    double previewMilliseconds = 0.0;
    double prepareMilliseconds = 0.0;
    double stopRenderMilliseconds = 0.0;
    unsigned int previewWidth = 0;
    unsigned int previewHeight = 0;
    uint64_t placementInvalidations = 0;
    uint64_t prototypeInvalidations = 0;
    uint64_t materialInvalidations = 0;
    uint64_t environmentInvalidations = 0;
    uint64_t conservativeRebuilds = 0;
    bool profileBufferWrites = false;
    std::string lastRebuildReason = "initial population";
};

SceneBridge::SceneBridge(HdSceneIndexBaseRefPtr sceneIndex, HdRenderThread *thread)
    : _impl(std::make_unique<Impl>(std::move(sceneIndex), thread)) {}
SceneBridge::~SceneBridge() = default;

void SceneBridge::MarkSceneDirty()
{
    MarkSceneDirty(SceneDirtyKind::Unknown);
}

void SceneBridge::MarkSceneDirty(SceneDirtyKind kind, SdfPath const &path)
{
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    _impl->sceneDirty = true;
    _impl->dirtyKinds |= static_cast<unsigned int>(kind);
    if (!path.IsEmpty()) _impl->dirtyPaths.insert(path);
    switch (kind) {
    case SceneDirtyKind::Placement: ++_impl->placementInvalidations; break;
    case SceneDirtyKind::Prototype: ++_impl->prototypeInvalidations; break;
    case SceneDirtyKind::Material: ++_impl->materialInvalidations; break;
    case SceneDirtyKind::Environment: ++_impl->environmentInvalidations; break;
    default: ++_impl->conservativeRebuilds; break;
    }
    _impl->lastRebuildReason = path.IsEmpty()
        ? "unknown dirty locator" : path.GetString();
    if (kind == SceneDirtyKind::Material && _impl->materialService)
        _impl->materialService->Invalidate(path, true);
    if ((kind == SceneDirtyKind::Structure || kind == SceneDirtyKind::Unknown) &&
        _impl->materialService)
        _impl->materialService->InvalidateAll();
    _converged.store(false);
}

void SceneBridge::SetSettings(RenderSettings const &settings)
{
    {
        std::lock_guard lock(_impl->mutex);
        RenderSettings const &current = _impl->settings;
        if (current.samples == settings.samples &&
            current.maxDepth == settings.maxDepth &&
            current.rouletteDepth == settings.rouletteDepth &&
            current.seed == settings.seed &&
            current.sampleClamp == settings.sampleClamp &&
            current.previewMaxDimension == settings.previewMaxDimension &&
            current.refinementDelayMs == settings.refinementDelayMs &&
            current.resourceRevision == settings.resourceRevision &&
            current.environmentSampling == settings.environmentSampling)
            return;
    }
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    if (_impl->settings.resourceRevision != settings.resourceRevision) {
        if (_impl->materialService) _impl->materialService->InvalidateAll();
        _impl->sceneDirty = true;
        _impl->dirtyKinds |= static_cast<unsigned int>(SceneDirtyKind::Material) |
            static_cast<unsigned int>(SceneDirtyKind::Environment);
        ++_impl->materialInvalidations;
        ++_impl->environmentInvalidations;
        _impl->lastRebuildReason = "resource revision changed";
    }
    _impl->settings = settings;
    _impl->stateDirty = true;
    _converged.store(false);
}

RenderSettings SceneBridge::GetSettings() const
{
    std::lock_guard lock(_impl->mutex);
    return _impl->settings;
}

bool SceneBridge::Prepare(CameraState const &camera,
                          HdRenderPassAovBindingVector const &bindings,
                          TfTokenVector const &renderTags,
                          unsigned int width, unsigned int height)
{
    auto const prepareStart = std::chrono::steady_clock::now();
    bool cameraChanged = false;
    bool bindingsChanged = false;
    bool renderTagsChanged = false;
    bool shouldPreview = false;
    {
        std::lock_guard lock(_impl->mutex);
        cameraChanged = _impl->camera.worldToView != camera.worldToView ||
            _impl->camera.projection != camera.projection ||
            _impl->camera.nearClip != camera.nearClip ||
            _impl->camera.farClip != camera.farClip ||
            _impl->camera.fStop != camera.fStop ||
            _impl->camera.focalLength != camera.focalLength ||
            _impl->camera.focusDistance != camera.focusDistance ||
            _impl->camera.orthographic != camera.orthographic ||
            _impl->width != width || _impl->height != height;
        bindingsChanged = _impl->bindings != bindings;
        renderTagsChanged = _impl->renderTags != renderTags;
        if (!_impl->sceneDirty && !_impl->stateDirty && !cameraChanged &&
            !bindingsChanged && !renderTagsChanged)
            return false;
        shouldPreview = cameraChanged || bindingsChanged ||
            _impl->sceneDirty || _impl->stateDirty || renderTagsChanged;
    }
    auto const stopStart = std::chrono::steady_clock::now();
    _impl->renderThread->StopRender();
    const double stopMilliseconds = 1000.0 * std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stopStart).count();
    std::lock_guard lock(_impl->mutex);
    _impl->stopRenderMilliseconds = stopMilliseconds;

    const bool placementOnly = _impl->sceneDirty && _impl->snapshot &&
        !renderTagsChanged && _impl->dirtyKinds ==
            static_cast<unsigned int>(SceneDirtyKind::Placement);
    const unsigned int materialEnvironment =
        static_cast<unsigned int>(SceneDirtyKind::Material) |
        static_cast<unsigned int>(SceneDirtyKind::Environment);
    if (placementOnly) {
        _UpdatePlacements(_impl->sceneIndex, _impl->snapshot.get());
    } else if (_impl->sceneDirty && _impl->snapshot && !renderTagsChanged &&
               _impl->dirtyKinds == static_cast<unsigned int>(
                   SceneDirtyKind::Prototype)) {
        _RefreshPrototypes(_impl->sceneIndex, _impl->dirtyPaths,
                           _impl->materialService.get(), _impl->snapshot.get(),
                           renderTags);
    } else if (_impl->sceneDirty && _impl->snapshot && !renderTagsChanged &&
               _impl->dirtyKinds == static_cast<unsigned int>(
                   SceneDirtyKind::Material)) {
        _RefreshMaterials(_impl->sceneIndex, _impl->materialService.get(),
                          _impl->snapshot.get());
    } else if (_impl->sceneDirty && _impl->snapshot && !renderTagsChanged &&
               _impl->dirtyKinds == static_cast<unsigned int>(
                   SceneDirtyKind::Environment)) {
        _impl->snapshot->domeCount = 0;
        auto black = std::make_unique<ColorTexture>(Color3(0.0f));
        _impl->snapshot->environment =
            std::make_unique<ConstantColorBackground>(black.get());
        _impl->snapshot->environmentTexture = std::move(black);
        _impl->snapshot->scene->environment =
            _impl->snapshot->environment.get();
        _RefreshEnvironment(_impl->sceneIndex, SdfPath::AbsoluteRootPath(),
                            _impl->snapshot.get());
    } else if (_impl->sceneDirty && _impl->snapshot && !renderTagsChanged &&
               _impl->dirtyKinds == materialEnvironment) {
        _RefreshMaterials(_impl->sceneIndex, _impl->materialService.get(),
                          _impl->snapshot.get());
        _impl->snapshot->domeCount = 0;
        auto black = std::make_unique<ColorTexture>(Color3(0.0f));
        _impl->snapshot->environment =
            std::make_unique<ConstantColorBackground>(black.get());
        _impl->snapshot->environmentTexture = std::move(black);
        _impl->snapshot->scene->environment =
            _impl->snapshot->environment.get();
        _RefreshEnvironment(_impl->sceneIndex, SdfPath::AbsoluteRootPath(),
                            _impl->snapshot.get());
    } else if (_impl->sceneDirty || !_impl->snapshot || renderTagsChanged) {
        auto const buildStart = std::chrono::steady_clock::now();
        _impl->snapshot = _BuildSnapshot(_impl->sceneIndex,
                                         _impl->materialService.get(),
                                         _impl->settings, renderTags,
                                         _impl->device);
        _impl->snapshotBuildSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - buildStart).count();
        ++_impl->snapshotBuilds;
    }
    if (_impl->snapshot && _impl->snapshot->scene) {
        _impl->snapshot->scene->max_depth = _impl->settings.maxDepth;
        _impl->snapshot->scene->rr_min_depth = _impl->settings.rouletteDepth;
        _impl->snapshot->scene->environmentSampling =
            _impl->settings.environmentSampling ==
                RenderSettings::EnvironmentSampling::Mis;
    }
    _impl->camera = camera;
    _impl->bindings = bindings;
    _impl->renderTags = renderTags;
    _impl->width = width;
    _impl->height = height;
    _impl->samples.store(0);
    for (HdRenderPassAovBinding const &binding : _impl->bindings) {
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer)) {
            buffer->SetConverged(false);
        }
    }
    if (shouldPreview) {
        auto const previewStart = std::chrono::steady_clock::now();
        PreviewResult const preview = _RenderPreview(
            _impl->snapshot, _impl->camera, _impl->settings,
            _impl->bindings, _impl->width, _impl->height);
        _impl->previewMilliseconds = 1000.0 * std::chrono::duration<double>(
            std::chrono::steady_clock::now() - previewStart).count();
        _impl->previewWidth = preview.width;
        _impl->previewHeight = preview.height;
        if (preview.published) ++_impl->previewPublishes;
    }
    _impl->sceneDirty = _impl->stateDirty = false;
    _impl->dirtyKinds = 0;
    _impl->dirtyPaths.clear();
    _impl->startTime = std::chrono::steady_clock::now();
    _impl->renderSeconds = 0.0;
    _converged.store(false);
    _impl->prepareMilliseconds = 1000.0 * std::chrono::duration<double>(
        std::chrono::steady_clock::now() - prepareStart).count();
    return true;
}

void SceneBridge::Render()
{
    std::shared_ptr<Snapshot> snapshot;
    CameraState camera;
    HdRenderPassAovBindingVector bindings;
    RenderSettings settings;
    unsigned int width, height;
    {
        std::lock_guard lock(_impl->mutex);
        snapshot = _impl->snapshot; camera = _impl->camera;
        bindings = _impl->bindings; settings = _impl->settings;
        width = _impl->width; height = _impl->height;
    }
    if (!snapshot || !snapshot->scene || width == 0 || height == 0) {
        for (HdRenderPassAovBinding const &binding : bindings)
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->SetConverged(true);
        {
            std::lock_guard lock(_impl->mutex);
            _impl->renderSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _impl->startTime).count();
        }
        _converged.store(true); return;
    }
    // Do not let full-resolution work contend with camera interaction.  Each
    // new camera state cancels this wait in at most two milliseconds and starts
    // a fresh quiet period; refinement begins automatically after input stops.
    auto const refineAfter = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::clamp(settings.refinementDelayMs, 0, 2000));
    while (std::chrono::steady_clock::now() < refineAfter) {
        if (_impl->renderThread->IsStopRequested()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (_impl->renderThread->IsStopRequested()) return;
    for (HdRenderPassAovBinding const &binding : bindings)
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            buffer->BeginFrame(binding.clearValue);
    // Full-resolution RNG setup belongs to the render worker.  Camera motion
    // only pays for the bounded preview above on the UI/Execute thread.
    std::vector<unsigned int> rngStates(static_cast<size_t>(width) * height);
    tbb::parallel_for(
        tbb::blocked_range2d<unsigned int>(0, height, 16, 0, width, 64),
        [&](tbb::blocked_range2d<unsigned int> const &range) {
            for (unsigned int y = range.rows().begin();
                 y != range.rows().end(); ++y) {
                for (unsigned int x = range.cols().begin();
                     x != range.cols().end(); ++x) {
                    if (_impl->renderThread->IsStopRequested()) return;
                    rngStates[static_cast<size_t>(y) * width + x] =
                        tea<64>((y * width) ^ settings.seed, x);
                }
            }
        });
    if (_impl->renderThread->IsStopRequested()) return;
    const int sampleCount = std::max(1, settings.samples);
    for (int sample = 0; sample < sampleCount; ++sample) {
        if (_impl->renderThread->IsStopRequested()) return;
        while (_impl->renderThread->IsPauseRequested()) {
            if (_impl->renderThread->IsStopRequested()) return;
            std::this_thread::yield();
        }
        tbb::parallel_for(tbb::blocked_range2d<unsigned int>(0, height, 8, 0, width, 8),
            [&](tbb::blocked_range2d<unsigned int> const &range) {
                std::vector<uint64_t> tileWriteNanoseconds(
                    _impl->profileBufferWrites ? bindings.size() : 0, 0);
                for (unsigned int y = range.rows().begin(); y != range.rows().end(); ++y) {
                    if (_impl->renderThread->IsStopRequested()) return;
                    for (unsigned int x = range.cols().begin(); x != range.cols().end(); ++x) {
                        unsigned int &seed = rngStates[
                            static_cast<size_t>(y) * width + x];
                        RayPair rays = _CameraRays(camera, x, y, width, height, seed);
                        Color3 c = snapshot->scene->trace(rays.sampled, 0, Color3(1.0f), seed);
                        if (settings.sampleClamp > 0.0f) {
                            const float peak = c.max_component();
                            if (peak > settings.sampleClamp)
                                c *= settings.sampleClamp / peak;
                        }
                        std::optional<RayHit> primary;
                        if (sample == 0) primary.emplace(
                            snapshot->scene->cast_ray(rays.center));
                        // Hydra's render buffers are addressed bottom-up. Camera sampling
                        // remains top-down so the generated rays match standalone PrismRT.
                        const unsigned int bufferY = height - 1 - y;
                        for (size_t bindingIndex = 0;
                             bindingIndex < bindings.size(); ++bindingIndex) {
                            HdRenderPassAovBinding const &binding = bindings[bindingIndex];
                            if (sample != 0 && binding.aovName != HdAovTokens->color)
                                continue;
                            auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer);
                            if (!buffer) continue;
                            const auto writeStart = _impl->profileBufferWrites
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
                            if (binding.aovName == HdAovTokens->color) {
                                float v[4] = {c.x(), c.y(), c.z(), 1.0f};
                                buffer->WriteFloat(x, bufferY, v, 4, true);
                            } else if (!primary) {
                                continue;
                            } else if (binding.aovName == HdAovTokens->depth ||
                                       binding.aovName == HdAovTokens->cameraDepth) {
                                float v = binding.aovName == HdAovTokens->depth
                                    ? 1.0f : std::numeric_limits<float>::infinity();
                                if (primary->geomid != RTC_INVALID_GEOMETRY_ID) {
                                    GfVec4d view(primary->p.x(), primary->p.y(),
                                                 primary->p.z(), 1.0);
                                    view = view * camera.worldToView;
                                    if (binding.aovName == HdAovTokens->cameraDepth) {
                                        v = static_cast<float>(-view[2]);
                                    } else {
                                        GfVec4d clip = view * camera.projection;
                                        if (std::abs(clip[3]) > 1e-12)
                                            v = static_cast<float>(
                                                0.5 * clip[2] / clip[3] + 0.5);
                                    }
                                }
                                buffer->WriteFloat(x, bufferY, &v, 1, false);
                            } else if (binding.aovName == HdAovTokens->normal ||
                                       binding.aovName == HdAovTokens->Neye) {
                                Float3 n = primary->n;
                                if (binding.aovName == HdAovTokens->Neye &&
                                    primary->geomid != RTC_INVALID_GEOMETRY_ID) {
                                    GfVec3d eye = camera.worldToView.TransformDir(
                                        GfVec3d(n.x(), n.y(), n.z())).GetNormalized();
                                    n = Float3(static_cast<float>(eye[0]),
                                               static_cast<float>(eye[1]),
                                               static_cast<float>(eye[2]));
                                }
                                float v[3] = {n.x(), n.y(), n.z()};
                                buffer->WriteFloat(x, bufferY, v, 3, false);
                            } else if (binding.aovName == HdAovTokens->primId) {
                                int id = -1;
                                auto it = snapshot->geomToPrimId.find(primary->geomid);
                                if (it != snapshot->geomToPrimId.end()) id = it->second;
                                buffer->WriteInt(x, bufferY, &id, 1);
                            } else if (binding.aovName == HdAovTokens->elementId) {
                                int id = -1;
                                auto it = snapshot->geomToCoarseFaces.find(primary->geomid);
                                if (it != snapshot->geomToCoarseFaces.end() &&
                                    primary->primid >= 0 &&
                                    static_cast<size_t>(primary->primid) < it->second.size()) {
                                    id = it->second[primary->primid];
                                }
                                buffer->WriteInt(x, bufferY, &id, 1);
                            } else if (binding.aovName == HdAovTokens->instanceId) {
                                int id = -1;
                                auto it = snapshot->geomToInstanceId.find(primary->geomid);
                                if (it != snapshot->geomToInstanceId.end()) id = it->second;
                                buffer->WriteInt(x, bufferY, &id, 1);
                            }
                            if (_impl->profileBufferWrites) {
                                tileWriteNanoseconds[bindingIndex] +=
                                    static_cast<uint64_t>(std::chrono::duration_cast<
                                        std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - writeStart).count());
                            }
                        }
                    }
                }
                if (_impl->profileBufferWrites) {
                    for (size_t i = 0; i < bindings.size(); ++i) {
                        if (tileWriteNanoseconds[i] == 0) continue;
                        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(
                                bindings[i].renderBuffer))
                            buffer->RecordWriteNanoseconds(tileWriteNanoseconds[i]);
                    }
                }
            });
        // A stop can arrive while TBB is completing the current tiles.  Keep
        // the previous coherent frame instead of publishing that partial pass.
        if (_impl->renderThread->IsStopRequested()) return;
        for (HdRenderPassAovBinding const &binding : bindings) {
            // Geometric AOVs are deterministic and only written on sample 0.
            // Color changes every pass as accumulation progresses.
            if (sample != 0 && binding.aovName != HdAovTokens->color) continue;
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->Publish();
        }
        _impl->samples.store(static_cast<uint64_t>(sample + 1));
    }
    for (HdRenderPassAovBinding const &binding : bindings)
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            buffer->SetConverged(true);
    {
        std::lock_guard lock(_impl->mutex);
        _impl->renderSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - _impl->startTime).count();
    }
    _converged.store(true);
}

SdfPath SceneBridge::GetPrimPath(int primId) const
{
    std::lock_guard lock(_impl->mutex);
    if (!_impl->snapshot || primId < 0 ||
        static_cast<size_t>(primId) >= _impl->snapshot->primPaths.size()) return {};
    return _impl->snapshot->primPaths[primId];
}

SdfPath SceneBridge::GetInstancerPath(int primId) const
{
    std::lock_guard lock(_impl->mutex);
    if (!_impl->snapshot || primId < 0 ||
        static_cast<size_t>(primId) >= _impl->snapshot->instancerPaths.size()) return {};
    return _impl->snapshot->instancerPaths[primId];
}

VtDictionary SceneBridge::GetStats() const
{
    VtDictionary result;
    result["completedSamples"] = VtValue(static_cast<int64_t>(_impl->samples.load()));
    result["converged"] = VtValue(_converged.load());
    std::lock_guard lock(_impl->mutex);
    result["meshCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->meshes.size() : 0));
    int64_t triangles = 0;
    if (_impl->snapshot) {
        for (auto const &mesh : _impl->snapshot->meshes)
            triangles += static_cast<int64_t>(mesh->indices.size() / 3);
    }
    result["triangleCount"] = VtValue(triangles);
    result["primCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->primPaths.size() : 0));
    result["snapshotBuilds"] = VtValue(static_cast<int64_t>(
        _impl->snapshotBuilds));
    result["snapshotBuildSeconds"] = VtValue(_impl->snapshotBuildSeconds);
    result["renderSeconds"] = VtValue(_impl->renderSeconds);
    result["previewPublishes"] = VtValue(static_cast<int64_t>(
        _impl->previewPublishes));
    result["previewMilliseconds"] = VtValue(_impl->previewMilliseconds);
    result["prepareMilliseconds"] = VtValue(_impl->prepareMilliseconds);
    result["stopRenderMilliseconds"] = VtValue(
        _impl->stopRenderMilliseconds);
    result["previewWidth"] = VtValue(static_cast<int64_t>(_impl->previewWidth));
    result["previewHeight"] = VtValue(static_cast<int64_t>(_impl->previewHeight));
    result["instanceCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->instanceCount : 0));
    result["prototypeCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->meshes.size() : 0));
    result["prototypePathCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->prototypes.size() : 0));
    result["prototypeBuildCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->prototypeBuildCount : 0));
    result["meshConversionSeconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->meshConversionSeconds : 0.0);
    result["embreeGeometryCommitSeconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->embreeGeometryCommitSeconds : 0.0);
    result["embreeSceneCommitSeconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->embreeSceneCommitSeconds : 0.0);
    result["sceneTraversalSeconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->sceneTraversalSeconds : 0.0);
    result["estimatedGeometryBytes"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->estimatedGeometryBytes : 0));
    result["placementInvalidations"] = VtValue(static_cast<int64_t>(_impl->placementInvalidations));
    result["prototypeInvalidations"] = VtValue(static_cast<int64_t>(_impl->prototypeInvalidations));
    result["materialInvalidations"] = VtValue(static_cast<int64_t>(_impl->materialInvalidations));
    result["environmentInvalidations"] = VtValue(static_cast<int64_t>(_impl->environmentInvalidations));
    result["conservativeRebuilds"] = VtValue(static_cast<int64_t>(_impl->conservativeRebuilds));
    result["lastRebuildReason"] = VtValue(_impl->lastRebuildReason);
    uint64_t writeNs = 0, publishNs = 0, mapNs = 0;
    for (auto const &binding : _impl->bindings) {
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer)) {
            writeNs += buffer->GetWriteNanoseconds();
            publishNs += buffer->GetPublishNanoseconds();
            mapNs += buffer->GetMapWaitNanoseconds();
        }
    }
    result["bufferWriteProfilingEnabled"] = VtValue(
        _impl->profileBufferWrites);
    result["bufferWriteMilliseconds"] = VtValue(writeNs / 1.0e6);
    result["bufferPublicationMilliseconds"] = VtValue(publishNs / 1.0e6);
    result["bufferMapWaitMilliseconds"] = VtValue(mapNs / 1.0e6);
    if (_impl->materialService) {
        const auto stats = _impl->materialService->GetStats();
        result["materialRequests"] = VtValue(static_cast<int64_t>(stats.requests));
        result["materialPathCacheHits"] = VtValue(static_cast<int64_t>(stats.pathCacheHits));
        result["materialContentCacheHits"] = VtValue(static_cast<int64_t>(stats.contentCacheHits));
        result["materialCompilations"] = VtValue(static_cast<int64_t>(stats.compilations));
        result["materialNativeJits"] = VtValue(static_cast<int64_t>(stats.nativeJits));
        result["materialFailures"] = VtValue(static_cast<int64_t>(stats.failures));
        result["materialConversionSeconds"] = VtValue(stats.conversionSeconds);
        result["materialCompilationSeconds"] = VtValue(stats.compilationSeconds);
        result["materialXGenerationSeconds"] = VtValue(stats.generationSeconds);
        result["materialClassCompilationSeconds"] = VtValue(
            stats.classCompilationSeconds);
        result["materialNativeJitSeconds"] = VtValue(stats.nativeJitSeconds);
        result["materialResourceBytes"] = VtValue(static_cast<int64_t>(stats.textureBytes));
        result["materialResourceReadSeconds"] = VtValue(stats.resourceReadSeconds);
        result["materialBatchWallSeconds"] = VtValue(stats.batchWallSeconds);
        result["materialWorkerCpuSeconds"] = VtValue(stats.workerCpuSeconds);
    }
    return result;
}

} // namespace prism::hydra
