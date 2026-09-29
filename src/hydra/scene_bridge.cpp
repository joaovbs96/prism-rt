#include "scene_bridge.h"

#include "materials.h"
#include "render_buffer.h"
#include "triangulation.h"

#include "../environment.h"
#include "../batched_spheres.h"
#include "../camera_rays.h"
#include "../camera_ray_diagnostics.h"
#include "../geometry.h"
#include "../material.h"
#include "../materialx/resolved_resource.h"
#include "../mdl/context.h"
#include "../rng.h"
#include "../scene.h"
#include "../texture.h"

#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/multiInterval.h"
#include "pxr/base/gf/dualQuatd.h"
#include "pxr/base/gf/dualQuatf.h"
#include "pxr/base/gf/dualQuath.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/geomSubsetSchema.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/sphereSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/xformSchema.h"

#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <deque>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <optional>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::hydra {
namespace {

// Nothing is displayed before the first snapshot, so the host has no
// responsiveness to protect and capture can finish in far fewer frames.
constexpr double kInitialCaptureBudgetMilliseconds = 50.0;
constexpr double kSteadyCaptureBudgetMilliseconds = 3.0;

bool _CapturePrimData(TfToken const &primType)
{
    std::string type = primType.GetString();
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return type.find("task") == std::string::npos &&
        type.find("renderbuffer") == std::string::npos &&
        type.find("camera") == std::string::npos &&
        type.find("rendersettings") == std::string::npos &&
        type.find("selection") == std::string::npos &&
        type.find("system") == std::string::npos;
}

template<class T>
HdDataSourceBaseHandle _RetainIf(VtValue const &value)
{
    return value.IsHolding<T>()
        ? HdDataSourceBaseHandle(
            HdRetainedTypedSampledDataSource<T>::New(
                value.UncheckedGet<T>()))
        : HdDataSourceBaseHandle();
}

HdDataSourceBaseHandle _RetainSampled(
    HdSampledDataSourceHandle const &source)
{
    VtValue value = source->GetValue(0.0f);
#define PRISM_RETAIN_VALUE(type) \
    if (auto retained = _RetainIf<type>(value)) return retained
    PRISM_RETAIN_VALUE(bool);
    PRISM_RETAIN_VALUE(int);
    PRISM_RETAIN_VALUE(float);
    PRISM_RETAIN_VALUE(double);
    PRISM_RETAIN_VALUE(std::string);
    PRISM_RETAIN_VALUE(TfToken);
    PRISM_RETAIN_VALUE(SdfPath);
    PRISM_RETAIN_VALUE(SdfAssetPath);
    PRISM_RETAIN_VALUE(ArResolverContext);
    PRISM_RETAIN_VALUE(GfMatrix4d);
    PRISM_RETAIN_VALUE(GfVec2f);
    PRISM_RETAIN_VALUE(GfVec3f);
    PRISM_RETAIN_VALUE(GfVec3d);
    PRISM_RETAIN_VALUE(GfVec4f);
    PRISM_RETAIN_VALUE(VtIntArray);
    PRISM_RETAIN_VALUE(VtFloatArray);
    PRISM_RETAIN_VALUE(VtDoubleArray);
    PRISM_RETAIN_VALUE(VtBoolArray);
    PRISM_RETAIN_VALUE(VtTokenArray);
    PRISM_RETAIN_VALUE(VtStringArray);
    PRISM_RETAIN_VALUE(VtVec2fArray);
    PRISM_RETAIN_VALUE(VtVec3fArray);
    PRISM_RETAIN_VALUE(VtVec3dArray);
    PRISM_RETAIN_VALUE(VtVec4fArray);
    PRISM_RETAIN_VALUE(VtMatrix4dArray);
    PRISM_RETAIN_VALUE(VtQuatfArray);
#undef PRISM_RETAIN_VALUE
    if (auto retained = _RetainIf<VtArray<SdfPath>>(value)) return retained;
    return HdRetainedSampledDataSource::New(std::move(value));
}

HdDataSourceBaseHandle _MakeStaticCopy(HdDataSourceBaseHandle const &source)
{
    if (!source) return {};
    if (auto container = HdContainerDataSource::Cast(source)) {
        TfTokenVector const names = container->GetNames();
        std::vector<HdDataSourceBaseHandle> values;
        values.reserve(names.size());
        for (TfToken const &name : names)
            values.push_back(_MakeStaticCopy(container->Get(name)));
        return HdRetainedContainerDataSource::New(
            names.size(), names.data(), values.data());
    }
    if (auto vector = HdVectorDataSource::Cast(source)) {
        std::vector<HdDataSourceBaseHandle> values(vector->GetNumElements());
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = _MakeStaticCopy(vector->GetElement(i));
        return HdRetainedSmallVectorDataSource::New(
            values.size(), values.data());
    }
    if (auto sampled = HdSampledDataSource::Cast(source))
        return _RetainSampled(sampled);
    // Blocks and other immutable marker sources carry no scene-backed state.
    return source;
}

HdContainerDataSourceHandle _MakeStaticPrimCopy(
    TfToken const &primType, HdContainerDataSourceHandle const &source)
{
    if (!source || primType != HdPrimTypeTokens->domeLight)
        return HdContainerDataSource::Cast(_MakeStaticCopy(source));

    // The legacy light data source resolves authored shader inputs from
    // Get(name), but intentionally omits them from GetNames(). Preserve the
    // inputs consumed by _SetDomeEnvironment while we still own the live DS.
    TfTokenVector names = source->GetNames();
    if (std::find(names.begin(), names.end(), HdLightSchemaTokens->light) ==
        names.end())
        names.push_back(HdLightSchemaTokens->light);
    std::vector<HdDataSourceBaseHandle> values;
    values.reserve(names.size());
    for (TfToken const &name : names) {
        HdDataSourceBaseHandle child = source->Get(name);
        if (name != HdLightSchemaTokens->light) {
            values.push_back(_MakeStaticCopy(child));
            continue;
        }
        HdContainerDataSourceHandle light = HdContainerDataSource::Cast(child);
        if (!light) {
            values.push_back(_MakeStaticCopy(child));
            continue;
        }
        TfTokenVector lightNames = light->GetNames();
        TfToken const consumed[] = {
            HdLightTokens->color, HdLightTokens->intensity,
            HdLightTokens->exposure, HdLightTokens->textureFile};
        for (TfToken const &token : consumed) {
            if (light->Get(token) &&
                std::find(lightNames.begin(), lightNames.end(), token) ==
                    lightNames.end())
                lightNames.push_back(token);
        }
        std::vector<HdDataSourceBaseHandle> lightValues;
        lightValues.reserve(lightNames.size());
        for (TfToken const &lightName : lightNames)
            lightValues.push_back(_MakeStaticCopy(light->Get(lightName)));
        values.push_back(HdRetainedContainerDataSource::New(
            lightNames.size(), lightNames.data(), lightValues.data()));
    }
    return HdRetainedContainerDataSource::New(
        names.size(), names.data(), values.data());
}

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

// Coarse-face metadata is immutable and shared by all placements of a prototype.
class SharedFaceTable {
public:
    SharedFaceTable() = default;
    SharedFaceTable(std::vector<int> values)
        : values(std::make_shared<const std::vector<int>>(std::move(values))) {}
    size_t size() const { return values ? values->size() : 0; }
    int operator[](size_t i) const { return (*values)[i]; }
private:
    std::shared_ptr<const std::vector<int>> values;
};

struct PrototypeStorage {
    std::unique_ptr<Geometry> geometry;
    RTCScene scene = nullptr;
    ~PrototypeStorage() {
        if (scene) rtcReleaseScene(scene);
        if (geometry) {
            if (geometry->geom) rtcReleaseGeometry(geometry->geom);
            delete geometry->transform;
            geometry->transform = nullptr;
        }
    }
};
struct PrototypePool {
    std::mutex mutex;
    std::unordered_map<size_t, std::vector<std::weak_ptr<PrototypeStorage>>> buckets;
};

struct Snapshot {
    struct PendingSphere {
        SdfPath path;
        int primId = -1;
        Float3 center;
        float radius = 1.0f;
        Material *material = nullptr;
    };
    struct SphereBatch {
        std::shared_ptr<BatchedSpheres> geometry;
        unsigned int geomId = RTC_INVALID_GEOMETRY_ID;
        std::vector<SdfPath> paths;
        std::vector<int> primIds;
    };
    struct SphereBatchLocation { size_t batch = 0; size_t entry = 0; };
    struct Prototype {
        RTCScene scene = nullptr;
        Geometry *geometry = nullptr;
        SharedFaceTable coarseFaces;
        SceneManager::PlacementMaterialBinding binding;
        size_t geometryHash = 0;
        std::shared_ptr<PrototypeStorage> storage;
    };
    RTCDevice device = nullptr;
    std::unique_ptr<SceneManager> scene;
    std::vector<std::shared_ptr<Geometry>> geometries;
    std::shared_ptr<PrototypePool> prototypePool = std::make_shared<PrototypePool>();
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
    std::unordered_map<unsigned int, SharedFaceTable> geomToCoarseFaces;
    std::unordered_map<unsigned int, int> geomToInstanceId;
    std::unordered_map<unsigned int, std::vector<InstanceLevel>>
        geomToInstanceChain;
    std::unordered_map<unsigned int, bool> placementEnabled;
    bool sphereBatching = false;
    std::vector<PendingSphere> pendingSpheres;
    std::vector<SphereBatch> sphereBatches;
    std::unordered_map<SdfPath, SphereBatchLocation, SdfPath::Hash>
        sphereBatchLocations;
    std::unordered_map<SdfPath, std::unordered_set<size_t>, SdfPath::Hash>
        sphereBatchDependencies;
    std::unordered_map<unsigned int, std::vector<int>> sphereBatchPrimIds;
    std::vector<SdfPath> primPaths;
    std::vector<SdfPath> instancerPaths;
    std::shared_ptr<const PickingTable> picking;
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
    // Values are paths, not pointers: unordered-map growth and prototype edits
    // cannot invalidate bucket references. Stale paths are checked on lookup.
    std::unordered_map<size_t, std::vector<SdfPath>> prototypeHashBuckets;
    std::unordered_map<SdfPath,
        std::unordered_set<SdfPath, SdfPath::Hash>, SdfPath::Hash>
        materialUsers;
    std::unordered_map<SdfPath,
        std::unordered_set<SdfPath, SdfPath::Hash>, SdfPath::Hash>
        prototypeMaterials;
    std::unordered_map<SdfPath, std::unordered_set<unsigned int>, SdfPath::Hash>
        placementDependencies;
    uint64_t materialPrototypesVisited = 0;
    uint64_t materialBindingsUpdated = 0;
    uint64_t lastPlacementRecordsVisited = 0;
    uint64_t lastVisibilityPlacementsUpdated = 0;
    uint64_t sphereBatchRebuilds = 0;
    uint64_t sphereBatchEntriesUpdated = 0;
    uint64_t sphereBatchConservativeFallbacks = 0;
    uint64_t pickingTopologyFailures = 0;

    ~Snapshot()
    {
        if (scene) {
            scene->geometry.clear();
            scene->intersection_geometry.clear();
            scene->placement_materials.clear();
            if (scene->scene) rtcReleaseScene(scene->scene);
        }
        scene.reset();
        prototypes.clear();
        geometries.clear();
    }
};

void _RecordMaterialUser(Snapshot *snapshot, SdfPath const &material,
                         SdfPath const &prototype)
{
    if (!snapshot || material.IsEmpty() || prototype.IsEmpty()) return;
    snapshot->materialUsers[material].insert(prototype);
    snapshot->prototypeMaterials[prototype].insert(material);
}

void _RemovePrototypeMaterialUsers(Snapshot *snapshot,
                                   SdfPath const &prototype)
{
    auto reverse = snapshot->prototypeMaterials.find(prototype);
    if (reverse == snapshot->prototypeMaterials.end()) return;
    for (SdfPath const &material : reverse->second) {
        auto users = snapshot->materialUsers.find(material);
        if (users == snapshot->materialUsers.end()) continue;
        users->second.erase(prototype);
        if (users->second.empty()) snapshot->materialUsers.erase(users);
    }
    snapshot->prototypeMaterials.erase(reverse);
}

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
    snapshot->scene->geometry.emplace(id, prototype.geometry);
    snapshot->scene->intersection_geometry.emplace(id, prototype.geometry->geom);
    SceneManager::PlacementTransform placement;
    const GfMatrix4d normal = world.GetInverse().GetTranspose();
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        placement.normal[row * 3 + column] = static_cast<float>(normal[row][column]);
        placement.tangent[row * 3 + column] = static_cast<float>(world[row][column]);
    }
    placement.mirrored = world.GetDeterminant() < 0.0;
    placement.flipOrientation = placement.mirrored &&
        prototype.geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE;
    snapshot->scene->placement_transforms.emplace(id, placement);
    snapshot->scene->placement_materials.emplace(id, prototype.binding);
    snapshot->placementEnabled[id] = true;
    snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - commitStart).count();
    return id;
}

void _SetInstanceTransform(Snapshot *snapshot, unsigned int id,
                           GfMatrix4d const &world,
                           Snapshot::Prototype const &prototype)
{
    RTCGeometry instance = rtcGetGeometry(snapshot->scene->scene, id);
    if (!instance) return;
    float transform[16];
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            transform[column * 4 + row] = static_cast<float>(world[column][row]);
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                            transform);
    rtcCommitGeometry(instance);
    SceneManager::PlacementTransform placement;
    GfMatrix4d const normal = world.GetInverse().GetTranspose();
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            placement.normal[row * 3 + column] =
                static_cast<float>(normal[row][column]);
            placement.tangent[row * 3 + column] =
                static_cast<float>(world[row][column]);
        }
    placement.mirrored = world.GetDeterminant() < 0.0;
    placement.flipOrientation = placement.mirrored &&
        prototype.geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE;
    snapshot->scene->placement_transforms[id] = placement;
}

void _SetInstanceEnabled(Snapshot *snapshot, unsigned int id, bool enabled)
{
    auto state = snapshot->placementEnabled.find(id);
    const bool changed = state == snapshot->placementEnabled.end() ||
        state->second != enabled;
    RTCGeometry instance = rtcGetGeometry(snapshot->scene->scene, id);
    if (!instance) return;
    if (enabled) rtcEnableGeometry(instance);
    else rtcDisableGeometry(instance);
    rtcCommitGeometry(instance);
    snapshot->placementEnabled[id] = enabled;
    if (changed) ++snapshot->lastVisibilityPlacementsUpdated;
}

void _IndexPlacementDependencies(Snapshot *snapshot)
{
    snapshot->placementDependencies.clear();
    auto addAncestors = [&](SdfPath path, unsigned int id) {
        while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath()) {
            snapshot->placementDependencies[path].insert(id);
            path = path.GetParentPath();
        }
    };
    for (auto const &[id, primId] : snapshot->geomToPrimId) {
        if (primId < 0 || static_cast<size_t>(primId) >=
                snapshot->primPaths.size()) continue;
        addAncestors(snapshot->primPaths[primId], id);
        if (static_cast<size_t>(primId) < snapshot->instancerPaths.size())
            addAncestors(snapshot->instancerPaths[primId], id);
        auto chain = snapshot->geomToInstanceChain.find(id);
        if (chain != snapshot->geomToInstanceChain.end())
            for (InstanceLevel const &level : chain->second)
                addAncestors(level.instancerPath, id);
    }
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

    // Match the standalone USD path when normals are missing, zero, or only
    // partially authored: accumulate one area-weighted coarse-face normal at
    // each referenced point. Keep this in prototype space; placement applies
    // the inverse transpose later.
    std::vector<GfVec3d> generatedNormals(points.size(), GfVec3d(0.0));
    size_t generatedCorner = 0;
    for (size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count < 3 || generatedCorner + count > indices.size()) {
            generatedCorner += std::max(0, count);
            continue;
        }
        if (holeSet.count(static_cast<int>(face))) {
            generatedCorner += count;
            continue;
        }
        bool valid = true;
        for (int k = 0; k < count; ++k) {
            const int pi = indices[generatedCorner + k];
            valid &= pi >= 0 && static_cast<size_t>(pi) < points.size();
        }
        if (valid) {
            const int p0 = indices[generatedCorner];
            const int p1 = indices[generatedCorner + 1];
            const int p2 = indices[generatedCorner + 2];
            GfVec3d contribution = GfCross(
                GfVec3d(points[p1]) - GfVec3d(points[p0]),
                GfVec3d(points[p2]) - GfVec3d(points[p0]));
            if (flip) contribution = -contribution;
            for (int k = 0; k < count; ++k)
                generatedNormals[indices[generatedCorner + k]] += contribution;
        }
        generatedCorner += count;
    }

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
                    GfVec3d authored(authoredNormals[ni]);
                    if (authored.GetLengthSq() > 1e-20 &&
                        std::isfinite(authored.GetLengthSq())) {
                        n = normalMatrix.TransformDir(authored).GetNormalized();
                        if (flip != mirrored) n = -n;
                    } else if (generatedNormals[pi].GetLengthSq() > 1e-20 &&
                               std::isfinite(generatedNormals[pi].GetLengthSq())) {
                        n = generatedNormals[pi].GetNormalized();
                    }
                } else if (generatedNormals[pi].GetLengthSq() > 1e-20 &&
                           std::isfinite(generatedNormals[pi].GetLengthSq())) {
                    n = generatedNormals[pi].GetNormalized();
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
    if (!materialPath.IsEmpty())
        _RecordMaterialUser(snapshot, materialPath, path);
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
        _RecordMaterialUser(snapshot, subsetMaterialPath, path);
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
    // Merge only identical complete attribute tuples after tangent generation.
    // Keeping tangent sign in the key preserves mirrored UV seams and avoids
    // changing interpolated frames when positions/normals happen to match.
    struct VertexHash {
        size_t operator()(std::array<uint32_t,12> const& value) const {
            size_t h = 0;
            for (auto bits : value) h ^= std::hash<uint32_t>{}(bits) + size_t{0x9e3779b9} + (h<<6) + (h>>2);
            return h;
        }
    };
    std::unordered_map<std::array<uint32_t,12>, unsigned, VertexHash> uniqueVertices;
    std::vector<Float3> compactPositions, compactNormals;
    std::vector<Float2> compactUvs;
    std::vector<float> compactTangents;
    std::vector<unsigned> remap(mesh->vertices.size());
    for (size_t i=0; i<mesh->vertices.size(); ++i) {
        const auto& p=mesh->vertices[i]; const auto& n=mesh->normals[i];
        const auto& uv=mesh->texcoords[i];
        const float values[12]={p.x(),p.y(),p.z(),n.x(),n.y(),n.z(),uv.u(),uv.v(),
            mesh->tangents[4*i],mesh->tangents[4*i+1],mesh->tangents[4*i+2],mesh->tangents[4*i+3]};
        std::array<uint32_t,12> key{};
        for (size_t c=0;c<key.size();++c) key[c]=std::bit_cast<uint32_t>(values[c]);
        auto [found,inserted]=uniqueVertices.emplace(key,static_cast<unsigned>(compactPositions.size()));
        remap[i]=found->second;
        if (inserted) {
            compactPositions.push_back(p); compactNormals.push_back(n); compactUvs.push_back(uv);
            compactTangents.insert(compactTangents.end(),values+8,values+12);
        }
    }
    for (auto& index : mesh->indices) index=remap[index];
    mesh->vertices=std::move(compactPositions); mesh->normals=std::move(compactNormals);
    mesh->texcoords=std::move(compactUvs); mesh->tangents=std::move(compactTangents);
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
    auto& bucket = snapshot->prototypeHashBuckets[geometryHash];
    for (auto const& candidatePath : bucket) {
        const auto found = snapshot->prototypes.find(candidatePath);
        if (found != snapshot->prototypes.end() && found->second.geometryHash == geometryHash &&
            equalGeometry(dynamic_cast<Mesh *>(found->second.geometry))) {
            shared = &found->second;
            break;
        }
    }
    std::shared_ptr<PrototypeStorage> storage = shared ? shared->storage : nullptr;
    if (!storage) {
        std::lock_guard lock(snapshot->prototypePool->mutex);
        auto& entries = snapshot->prototypePool->buckets[geometryHash];
        for (auto it=entries.begin();it!=entries.end();) {
            auto candidate=it->lock();
            if (!candidate) { it=entries.erase(it); continue; }
            if (equalGeometry(dynamic_cast<Mesh*>(candidate->geometry.get()))) { storage=std::move(candidate); break; }
            ++it;
        }
    }
    const bool builtStorage = !storage;
    if (!storage) {
        const auto prototypeCommitStart = std::chrono::steady_clock::now();
        storage = std::make_shared<PrototypeStorage>();
        storage->scene = rtcNewScene(snapshot->device);
        mesh->setupEmbree(*snapshot->scene->device, storage->scene);
        rtcCommitScene(storage->scene);
        transformOwner.release();
        storage->geometry = std::move(mesh);
        snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - prototypeCommitStart).count();
        std::lock_guard lock(snapshot->prototypePool->mutex);
        snapshot->prototypePool->buckets[geometryHash].push_back(storage);
    }
    RTCScene prototypeScene = storage->scene;
    Mesh* prototypeMesh = static_cast<Mesh*>(storage->geometry.get());
    Snapshot::Prototype prototype{prototypeScene, prototypeMesh, coarseFaces,
                                  placementBinding, geometryHash, storage};
    snapshot->prototypes.emplace(path, prototype);
    if (std::find(bucket.begin(), bucket.end(), path) == bucket.end()) bucket.push_back(path);
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
    snapshot->geomToCoarseFaces.emplace(geomId, prototype.coarseFaces);
    snapshot->geomToInstanceId.emplace(geomId, instanceId);
    if (!instanceChain.empty())
        snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
    if (instanceId >= 0) ++snapshot->instanceCount;
    if (!shared) {
        snapshot->estimatedGeometryBytes +=
            prototypeMesh->vertices.size() * sizeof(Float3) +
            prototypeMesh->normals.size() * sizeof(Float3) +
            prototypeMesh->texcoords.size() * sizeof(Float2) +
            prototypeMesh->indices.size() * sizeof(unsigned int) +
            prototypeMesh->tangents.size() * sizeof(float);
        if (builtStorage) ++snapshot->prototypeBuildCount;
        snapshot->geometries.emplace_back(storage, prototypeMesh);
    }
    return true;
}

Material *_BaseMaterial(HdSceneIndexBaseRefPtr const &sceneIndex,
                        SdfPath const &path,
                        HdSceneIndexPrim const &prim,
                        MaterialService *materials, Snapshot *snapshot)
{
    Material *material = snapshot->fallbackMaterial.get();
    HdPrimvarsSchema primvars =
        HdPrimvarsSchema::GetFromParent(prim.dataSource);
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
                snapshot->displayColorMaterials.push_back(
                    std::move(displayMaterial));
                snapshot->displayMaterialsByPath.emplace(path, material);
            }
        }
    }
    if (std::shared_ptr<Material> resolved = _ResolveMaterial(
            sceneIndex, _MaterialPath(prim.dataSource), materials, snapshot))
        material = resolved.get();
    return material;
}

bool _BatchableSphereTransform(GfMatrix4d const &world, Float3 *center,
                               float *scale)
{
    const double s = world[0][0];
    if (!(s > 0.0) || !std::isfinite(s)) return false;
    constexpr double tolerance = 1.0e-9;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            if (!std::isfinite(world[row][column])) return false;
            const double expected = row == column ? s : 0.0;
            if (std::abs(world[row][column] - expected) > tolerance)
                return false;
        }
    if (std::abs(world[0][3]) > tolerance ||
        std::abs(world[1][3]) > tolerance ||
        std::abs(world[2][3]) > tolerance ||
        std::abs(world[3][3] - 1.0) > tolerance) return false;
    GfVec3d const p = world.Transform(GfVec3d(0.0));
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
        !std::isfinite(p[2])) return false;
    *center = Float3(static_cast<float>(p[0]), static_cast<float>(p[1]),
                     static_cast<float>(p[2]));
    *scale = static_cast<float>(s);
    return *scale > 0.0f && std::isfinite(*scale) &&
        std::isfinite(center->x()) && std::isfinite(center->y()) &&
        std::isfinite(center->z());
}

void _IndexSphereBatchDependency(Snapshot *snapshot, SdfPath path,
                                 size_t batch)
{
    while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath()) {
        snapshot->sphereBatchDependencies[path].insert(batch);
        path = path.GetParentPath();
    }
}

void _FlushSphereBatches(Snapshot *snapshot)
{
    constexpr size_t maxBatchSize = 1024;
    for (size_t begin = 0; begin < snapshot->pendingSpheres.size();
         begin += maxBatchSize) {
        const size_t end = std::min(begin + maxBatchSize,
                                    snapshot->pendingSpheres.size());
        auto batch = std::shared_ptr<BatchedSpheres>(
            new BatchedSpheres(), [](BatchedSpheres *geometry) {
                if (geometry->geom) rtcReleaseGeometry(geometry->geom);
                delete geometry->transform;
                geometry->transform = nullptr;
                delete geometry;
            });
        Snapshot::SphereBatch record;
        record.geometry = batch;
        for (size_t i = begin; i < end; ++i) {
            Snapshot::PendingSphere const &entry = snapshot->pendingSpheres[i];
            batch->entries.push_back({entry.center, entry.radius, entry.material});
            record.paths.push_back(entry.path);
            record.primIds.push_back(entry.primId);
        }
        record.geomId = batch->setupEmbree(
            *snapshot->scene->device, snapshot->scene->scene);
        const size_t batchIndex = snapshot->sphereBatches.size();
        snapshot->scene->geometry.emplace(record.geomId, batch.get());
        snapshot->sphereBatchPrimIds.emplace(record.geomId, record.primIds);
        auto &binding = snapshot->scene->placement_materials[record.geomId];
        for (size_t entry = 0; entry < batch->entries.size(); ++entry) {
            binding.triangles[static_cast<unsigned int>(entry)] =
                batch->entries[entry].material;
            snapshot->sphereBatchLocations[record.paths[entry]] =
                {batchIndex, entry};
            _IndexSphereBatchDependency(snapshot, record.paths[entry], batchIndex);
        }
        snapshot->estimatedGeometryBytes +=
            batch->entries.size() * 4 * sizeof(float);
        snapshot->geometries.emplace_back(batch);
        snapshot->sphereBatches.push_back(std::move(record));
    }
    snapshot->pendingSpheres.clear();
}

int _PrimIdForHit(Snapshot const *snapshot, RayHit const &hit)
{
    auto batch = snapshot->sphereBatchPrimIds.find(
        static_cast<unsigned int>(hit.geomid));
    if (batch != snapshot->sphereBatchPrimIds.end() && hit.primid >= 0 &&
        static_cast<size_t>(hit.primid) < batch->second.size())
        return batch->second[hit.primid];
    auto placement = snapshot->geomToPrimId.find(
        static_cast<unsigned int>(hit.geomid));
    return placement == snapshot->geomToPrimId.end() ? -1 : placement->second;
}

size_t _EnvironmentImportanceBytes(Snapshot const *snapshot)
{
    if (!snapshot || !snapshot->environment) return 0;
    auto const *map = dynamic_cast<EnvironmentMap const *>(
        snapshot->environment.get());
    if (!map) return 0;
    return map->texelProbability.capacity() * sizeof(float) +
        map->probability.capacity() * sizeof(float) +
        map->alias.capacity() * sizeof(size_t);
}

void _BuildPickingInstanceTopologies(Snapshot *snapshot,
                                     PickingTable *picking)
{
    picking->instanceTopologies.resize(picking->primPaths.size());
    std::vector<bool> incompatible(picking->primPaths.size(), false);
    for (auto const &[geomId, chain] : snapshot->geomToInstanceChain) {
        auto prim = snapshot->geomToPrimId.find(geomId);
        if (prim == snapshot->geomToPrimId.end() || prim->second < 0 ||
            static_cast<size_t>(prim->second) >=
                picking->instanceTopologies.size()) continue;
        const size_t primId = static_cast<size_t>(prim->second);
        auto &owned = picking->instanceTopologies[primId];
        if (owned.empty()) {
            owned.resize(chain.size());
            for (size_t level = 0; level < chain.size(); ++level) {
                if (chain[level].radix <= 0) {
                    incompatible[primId] = true;
                    break;
                }
                owned[level].instancerPath = chain[level].instancerPath;
                owned[level].sourceIndicesByOrdinal.assign(
                    static_cast<size_t>(chain[level].radix), -1);
            }
        }
        if (owned.size() != chain.size()) {
            incompatible[primId] = true;
            continue;
        }
        for (size_t level = 0; level < chain.size(); ++level) {
            InstanceLevel const &value = chain[level];
            auto &destination = owned[level];
            if (destination.instancerPath != value.instancerPath ||
                value.ordinal < 0 || value.radix <= 0 ||
                destination.sourceIndicesByOrdinal.size() !=
                    static_cast<size_t>(value.radix) ||
                static_cast<size_t>(value.ordinal) >=
                    destination.sourceIndicesByOrdinal.size()) {
                incompatible[primId] = true;
                continue;
            }
            int &source = destination.sourceIndicesByOrdinal[value.ordinal];
            if (source >= 0 && source != value.sourceIndex)
                incompatible[primId] = true;
            else
                source = value.sourceIndex;
        }
    }
    for (size_t primId = 0; primId < picking->instanceTopologies.size(); ++primId) {
        auto &levels = picking->instanceTopologies[primId];
        if (!incompatible[primId]) continue;
        TF_WARN("PrismRT: incompatible instance selection mapping for <%s>; "
                "instance highlighting is disabled for this published frame",
                picking->primPaths[primId].GetText());
        levels.clear();
        snapshot->pickingTopologyFailures++;
    }
}

bool _RefreshSphereBatches(
    HdSceneIndexBaseRefPtr const &index,
    std::unordered_set<SdfPath, SdfPath::Hash> const &dirtyPaths,
    MaterialService *materials, Snapshot *snapshot,
    TfTokenVector const &renderTags,
    std::unordered_set<SdfPath, SdfPath::Hash> *handledPaths)
{
    if (!snapshot->sphereBatching || dirtyPaths.empty()) return true;
    std::unordered_set<size_t> affected;
    for (SdfPath const &path : dirtyPaths) {
        auto found = snapshot->sphereBatchDependencies.find(path);
        if (found == snapshot->sphereBatchDependencies.end()) continue;
        affected.insert(found->second.begin(), found->second.end());
        if (handledPaths && snapshot->sphereBatchLocations.count(path))
            handledPaths->insert(path);
    }
    for (size_t batchIndex : affected) {
        if (batchIndex >= snapshot->sphereBatches.size()) return false;
        Snapshot::SphereBatch &record = snapshot->sphereBatches[batchIndex];
        if (!record.geometry || record.paths.size() != record.geometry->entries.size())
            return false;
        for (size_t entry = 0; entry < record.paths.size(); ++entry) {
            SdfPath const &path = record.paths[entry];
            HdSceneIndexPrim const prim = index->GetPrim(path);
            if (prim.primType != HdPrimTypeTokens->sphere ||
                !_WorldVisible(index, path) ||
                !_PurposeMatches(prim.dataSource, renderTags) ||
                HdInstancedBySchema::GetFromParent(prim.dataSource))
                return false;
            HdSphereSchema schema = HdSphereSchema::GetFromParent(prim.dataSource);
            if (!schema) return false;
            double radius = schema.GetRadius()
                ? schema.GetRadius()->GetTypedValue(0.0f) : 1.0;
            Float3 center;
            float scale = 1.0f;
            if (!(radius > 0.0) || !std::isfinite(radius) ||
                !_BatchableSphereTransform(
                    _WorldTransform(index, path), &center, &scale))
                return false;
            const float worldRadius = static_cast<float>(radius) * scale;
            if (!(worldRadius > 0.0f) || !std::isfinite(worldRadius))
                return false;
            snapshot->displayMaterialsByPath.erase(path);
            Material *material = _BaseMaterial(
                index, path, prim, materials, snapshot);
            record.geometry->entries[entry] = {center, worldRadius, material};
            snapshot->scene->placement_materials[record.geomId].triangles[
                static_cast<unsigned int>(entry)] = material;
            ++snapshot->sphereBatchEntriesUpdated;
        }
        float *vertices = static_cast<float *>(rtcGetGeometryBufferData(
            record.geometry->geom, RTC_BUFFER_TYPE_VERTEX, 0));
        if (!vertices) return false;
        for (size_t entry = 0; entry < record.geometry->entries.size(); ++entry) {
            auto const &sphere = record.geometry->entries[entry];
            vertices[4 * entry] = sphere.center.x();
            vertices[4 * entry + 1] = sphere.center.y();
            vertices[4 * entry + 2] = sphere.center.z();
            vertices[4 * entry + 3] = sphere.radius;
        }
        rtcUpdateGeometryBuffer(record.geometry->geom,
                                RTC_BUFFER_TYPE_VERTEX, 0);
        rtcCommitGeometry(record.geometry->geom);
        record.geometry->updateAABB();
        ++snapshot->sphereBatchRebuilds;
    }
    if (!affected.empty()) rtcCommitScene(snapshot->scene->scene);
    return true;
}

void _AttachSphereBatches(Snapshot *snapshot)
{
    snapshot->sphereBatchPrimIds.clear();
    for (Snapshot::SphereBatch &record : snapshot->sphereBatches) {
        if (!record.geometry || !record.geometry->geom) continue;
        record.geomId = rtcAttachGeometry(
            snapshot->scene->scene, record.geometry->geom);
        snapshot->scene->geometry[record.geomId] = record.geometry.get();
        snapshot->sphereBatchPrimIds[record.geomId] = record.primIds;
        auto &binding = snapshot->scene->placement_materials[record.geomId];
        for (size_t entry = 0; entry < record.geometry->entries.size(); ++entry)
            binding.triangles[static_cast<unsigned int>(entry)] =
                record.geometry->entries[entry].material;
    }
}

bool _AppendSphere(HdSceneIndexBaseRefPtr const &sceneIndex,
                   SdfPath const &path, HdSceneIndexPrim const &prim,
                   MaterialService *materials, Snapshot *snapshot,
                   GfMatrix4d const &world, SdfPath const &instancerPath,
                   int instanceId,
                   std::vector<InstanceLevel> const &instanceChain,
                   TfTokenVector const &renderTags)
{
    if (!_WorldVisible(sceneIndex, path) ||
        !_PurposeMatches(prim.dataSource, renderTags)) return false;

    if (instanceId >= 0) {
        auto const cached = snapshot->prototypes.find(path);
        if (cached != snapshot->prototypes.end()) {
            const unsigned int geomId = _AttachPrototype(
                snapshot, path, cached->second, world);
            PrimKey const key{path, instancerPath};
            auto [it, inserted] = snapshot->primIds.emplace(
                key, static_cast<int>(snapshot->primPaths.size()));
            if (inserted) {
                snapshot->primPaths.push_back(path);
                snapshot->instancerPaths.push_back(instancerPath);
            }
            snapshot->geomToPrimId.emplace(geomId, it->second);
            snapshot->geomToCoarseFaces.emplace(geomId, std::vector<int>{});
            snapshot->geomToInstanceId.emplace(geomId, instanceId);
            if (!instanceChain.empty())
                snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
            ++snapshot->instanceCount;
            return true;
        }
    }

    HdSphereSchema schema = HdSphereSchema::GetFromParent(prim.dataSource);
    if (!schema) return false;
    double radius = 1.0;
    if (auto source = schema.GetRadius()) radius = source->GetTypedValue(0.0f);
    if (!(radius > 0.0) || !std::isfinite(radius)) {
        TF_WARN("PrismRT: sphere <%s> has invalid radius %g and will be skipped",
                path.GetText(), radius);
        return false;
    }
    const float sphereRadius = static_cast<float>(radius);
    if (!(sphereRadius > 0.0f) || !std::isfinite(sphereRadius)) {
        TF_WARN("PrismRT: sphere <%s> radius is outside renderer range",
                path.GetText());
        return false;
    }
    const SdfPath sphereMaterialPath = _MaterialPath(prim.dataSource);
    if (!sphereMaterialPath.IsEmpty())
        _RecordMaterialUser(snapshot, sphereMaterialPath, path);
    Material *material = _BaseMaterial(
        sceneIndex, path, prim, materials, snapshot);
    if (snapshot->sphereBatching && instanceId < 0) {
        Float3 center;
        float scale = 1.0f;
        if (_BatchableSphereTransform(world, &center, &scale)) {
            const float worldRadius = sphereRadius * scale;
            if (worldRadius > 0.0f && std::isfinite(worldRadius)) {
                PrimKey const key{path, {}};
                auto [it, inserted] = snapshot->primIds.emplace(
                    key, static_cast<int>(snapshot->primPaths.size()));
                if (inserted) {
                    snapshot->primPaths.push_back(path);
                    snapshot->instancerPaths.emplace_back();
                }
                snapshot->pendingSpheres.push_back(
                    {path, it->second, center, worldRadius, material});
                return true;
            }
        }
    }
    size_t geometryHash = std::hash<int>{}(
        static_cast<int>(RTC_GEOMETRY_TYPE_SPHERE_POINT));
    geometryHash ^= std::hash<float>{}(sphereRadius) + size_t{0x9e3779b9} +
        (geometryHash << 6) + (geometryHash >> 2);

    Snapshot::Prototype const *shared = nullptr;
    auto& sphereBucket = snapshot->prototypeHashBuckets[geometryHash];
    for (auto const& otherPath : sphereBucket) {
        const auto otherIt = snapshot->prototypes.find(otherPath);
        if (otherIt == snapshot->prototypes.end()) continue;
        const auto& other = otherIt->second;
        Sphere const *otherSphere = dynamic_cast<Sphere *>(other.geometry);
        if (other.geometryHash == geometryHash && otherSphere &&
            otherSphere->r == sphereRadius) {
            shared = &other;
            break;
        }
    }
    std::shared_ptr<PrototypeStorage> storage = shared ? shared->storage : nullptr;
    if (!storage) {
        std::lock_guard lock(snapshot->prototypePool->mutex);
        auto& entries=snapshot->prototypePool->buckets[geometryHash];
        for(auto it=entries.begin();it!=entries.end();) {
            auto candidate=it->lock();
            if(!candidate) { it=entries.erase(it); continue; }
            auto sphere=dynamic_cast<Sphere*>(candidate->geometry.get());
            if(sphere && sphere->r==sphereRadius) { storage=std::move(candidate); break; }
            ++it;
        }
    }
    if (!storage) {
        storage=std::make_shared<PrototypeStorage>();
        auto sphere=std::make_unique<Sphere>(Float3(0.0f),sphereRadius,material);
        storage->scene=rtcNewScene(snapshot->device);
        sphere->setupEmbree(*snapshot->scene->device,storage->scene);
        rtcCommitScene(storage->scene);
        storage->geometry=std::move(sphere);
        ++snapshot->prototypeBuildCount;
        std::lock_guard lock(snapshot->prototypePool->mutex);
        snapshot->prototypePool->buckets[geometryHash].push_back(storage);
    }
    RTCScene prototypeScene=storage->scene;
    auto* prototypeSphere=static_cast<Sphere*>(storage->geometry.get());
    if (!shared) snapshot->estimatedGeometryBytes += 4*sizeof(float);
    SceneManager::PlacementMaterialBinding binding;
    binding.fallback = material;
    Snapshot::Prototype prototype{prototypeScene, prototypeSphere, {},
                                  binding, geometryHash, storage};
    snapshot->prototypes.emplace(path, prototype);
    if (std::find(sphereBucket.begin(), sphereBucket.end(), path) == sphereBucket.end()) sphereBucket.push_back(path);
    const unsigned int geomId = _AttachPrototype(
        snapshot, path, prototype, world);
    PrimKey const key{path, instancerPath};
    auto [it, inserted] = snapshot->primIds.emplace(
        key, static_cast<int>(snapshot->primPaths.size()));
    if (inserted) {
        snapshot->primPaths.push_back(path);
        snapshot->instancerPaths.push_back(instancerPath);
    }
    snapshot->geomToPrimId.emplace(geomId, it->second);
    snapshot->geomToCoarseFaces.emplace(geomId, std::vector<int>{});
    snapshot->geomToInstanceId.emplace(geomId, instanceId);
    if (!instanceChain.empty())
        snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
    if (instanceId >= 0) ++snapshot->instanceCount;
    if (!shared) {
        snapshot->geometries.emplace_back(storage, prototypeSphere);
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
           TfTokenVector const &renderTags,
           std::function<bool()> const &cancelled)
{
    if (cancelled && cancelled()) return;
    HdSceneIndexPrim prim = scene->GetPrim(path);
    if (prim.primType == HdPrimTypeTokens->mesh ||
        prim.primType == HdPrimTypeTokens->sphere) {
        const GfMatrix4d world = _WorldTransform(scene, path);
        const bool isInstanced = bool(
            HdInstancedBySchema::GetFromParent(prim.dataSource));
        std::vector<InstancePlacement> placements =
            _InstancePlacements(scene, path, prim.dataSource);
        if (placements.empty() && !isInstanced) {
            if (prim.primType == HdPrimTypeTokens->mesh)
                _AppendMesh(scene, path, prim, materials, snapshot,
                            world, SdfPath(), -1, {}, renderTags);
            else
                _AppendSphere(scene, path, prim, materials, snapshot,
                              world, SdfPath(), -1, {}, renderTags);
        } else {
            for (InstancePlacement const &placement : placements) {
                if (cancelled && cancelled()) return;
                if (prim.primType == HdPrimTypeTokens->mesh)
                    _AppendMesh(scene, path, prim, materials, snapshot,
                                placement.transform, placement.instancerPath,
                                placement.instanceId, placement.chain, renderTags);
                else
                    _AppendSphere(scene, path, prim, materials, snapshot,
                                  placement.transform, placement.instancerPath,
                                  placement.instanceId, placement.chain, renderTags);
            }
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
        _Walk(scene, child, materials, snapshot, renderTags, cancelled);
}

void _CollectMaterialRequests(HdSceneIndexBaseRefPtr const &scene,
                              SdfPath const &path,
                              std::unordered_set<SdfPath, SdfPath::Hash> *seen,
                              std::vector<MaterialService::Request> *requests,
                              TfTokenVector const &renderTags,
                              std::function<bool()> const& cancelled)
{
    if (cancelled && cancelled()) return;
    HdSceneIndexPrim const prim = scene->GetPrim(path);
    auto add = [&](HdContainerDataSourceHandle const& source) {
        const SdfPath materialPath = _MaterialPath(source);
        if (!materialPath.IsEmpty() && seen->insert(materialPath).second)
            requests->push_back({scene->GetPrim(materialPath).dataSource, materialPath});
    };
    // Only supported scene geometry needs shading preparation. This follows
    // authored visibility/purpose, never camera visibility or screen bounds.
    if ((prim.primType == HdPrimTypeTokens->mesh || prim.primType == HdPrimTypeTokens->sphere)
        && _WorldVisible(scene, path) && _PurposeMatches(prim.dataSource, renderTags)) {
        add(prim.dataSource);
        if (prim.primType == HdPrimTypeTokens->mesh) {
            for (auto const& childPath : scene->GetChildPrimPaths(path)) {
                auto child = scene->GetPrim(childPath);
                auto subset = HdGeomSubsetSchema::GetFromParent(child.dataSource);
                if (!subset) continue;
                if (auto type = subset.GetType())
                    if (type->GetTypedValue(0.0f) != HdGeomSubsetSchemaTokens->typeFaceSet) continue;
                VtIntArray faces;
                if (_ReadArray(subset.GetIndices(), &faces) && !faces.empty()) add(child.dataSource);
            }
        }
    }
    for (SdfPath const &child : scene->GetChildPrimPaths(path))
        _CollectMaterialRequests(scene, child, seen, requests, renderTags, cancelled);
}

std::shared_ptr<Snapshot> _BuildSnapshot(HdSceneIndexBaseRefPtr const &scene,
                                         MaterialService *materials,
                                         RenderSettings const &settings,
                                         TfTokenVector const &renderTags,
                                         RTCDevice sharedDevice,
                                         std::shared_ptr<PrototypePool> const& pool = {},
                                         std::function<bool()> cancelled = {})
{
    auto result = std::make_shared<Snapshot>();
    result->device = sharedDevice;
    if (pool) result->prototypePool = pool;
    if (!result->device) return {};
    result->scene = std::make_unique<SceneManager>(result->device);
    result->scene->max_depth = settings.maxDepth;
    result->scene->rr_min_depth = settings.rouletteDepth;
    result->scene->environmentSampling =
        settings.environmentSampling == RenderSettings::EnvironmentSampling::Mis;
    result->scene->textureFiltering = settings.textureFiltering;
    result->sphereBatching = settings.sphereBatching;
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
                                 &seen, &requests, renderTags, cancelled);
        materials->Prefetch(requests, cancelled);
    }
    if (cancelled && cancelled()) return {};
    const auto traversalStart = std::chrono::steady_clock::now();
    _Walk(scene, SdfPath::AbsoluteRootPath(), materials, result.get(),
          renderTags, cancelled);
    if (cancelled && cancelled()) return {};
    _FlushSphereBatches(result.get());
    _IndexPlacementDependencies(result.get());
    result->sceneTraversalSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - traversalStart).count();
    const auto sceneCommitStart = std::chrono::steady_clock::now();
    if (cancelled && cancelled()) return {};
    result->scene->commitScene();
    result->embreeSceneCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sceneCommitStart).count();
    return result;
}

bool _UpdatePlacements(HdSceneIndexBaseRefPtr const &index, Snapshot *snapshot,
                       std::unordered_set<SdfPath, SdfPath::Hash> const &dirtyPaths = {},
                       std::function<bool()> const &cancelled = {},
                       bool *rebuiltAll = nullptr,
                       bool *needsFullSnapshot = nullptr)
{
    if (!snapshot || !snapshot->scene) return false;
    if (rebuiltAll) *rebuiltAll = false;
    if (needsFullSnapshot) *needsFullSnapshot = false;
    snapshot->lastPlacementRecordsVisited = 0;
    snapshot->lastVisibilityPlacementsUpdated = 0;
    if (!dirtyPaths.empty()) {
        std::unordered_set<unsigned int> affected;
        for (SdfPath const &path : dirtyPaths) {
            auto found = snapshot->placementDependencies.find(path);
            if (found != snapshot->placementDependencies.end())
                affected.insert(found->second.begin(), found->second.end());
        }
        if (!affected.empty()) {
            std::unordered_map<SdfPath, std::vector<unsigned int>, SdfPath::Hash>
                idsByPrototype;
            for (unsigned int id : affected) {
                auto prim = snapshot->geomToPrimId.find(id);
                if (prim == snapshot->geomToPrimId.end() || prim->second < 0 ||
                    static_cast<size_t>(prim->second) >= snapshot->primPaths.size())
                    return _UpdatePlacements(index, snapshot, {}, cancelled,
                                             rebuiltAll, needsFullSnapshot);
                idsByPrototype[snapshot->primPaths[prim->second]].push_back(id);
            }
            bool canUpdateInPlace = true;
            for (auto &[prototypePath, ids] : idsByPrototype) {
                if (cancelled && cancelled()) return false;
                HdSceneIndexPrim const prim = index->GetPrim(prototypePath);
                std::vector<InstancePlacement> placements;
                if (_WorldVisible(index, prototypePath)) {
                    placements = _InstancePlacements(
                        index, prototypePath, prim.dataSource);
                    if (placements.empty() &&
                        !HdInstancedBySchema::GetFromParent(prim.dataSource))
                        placements.push_back({
                            _WorldTransform(index, prototypePath), {}, -1, {}});
                }
                bool hiddenByDirtyPath = !_WorldVisible(index, prototypePath);
                if (!hiddenByDirtyPath) {
                    for (SdfPath const &dirtyPath : dirtyPaths) {
                        auto dependencies = snapshot->placementDependencies.find(
                            dirtyPath);
                        if (dependencies == snapshot->placementDependencies.end() ||
                            _WorldVisible(index, dirtyPath)) continue;
                        if (std::any_of(ids.begin(), ids.end(),
                                [&](unsigned int id) {
                                    return dependencies->second.count(id) != 0;
                                })) {
                            hiddenByDirtyPath = true;
                            break;
                        }
                    }
                }
                auto oldKey = [&](unsigned int id) {
                    int instanceId = -1;
                    auto instance = snapshot->geomToInstanceId.find(id);
                    if (instance != snapshot->geomToInstanceId.end())
                        instanceId = instance->second;
                    SdfPath instancerPath;
                    auto primId = snapshot->geomToPrimId.find(id);
                    if (primId != snapshot->geomToPrimId.end() &&
                        primId->second >= 0 && static_cast<size_t>(primId->second) <
                            snapshot->instancerPaths.size())
                        instancerPath = snapshot->instancerPaths[primId->second];
                    std::string key = instancerPath.GetString() + "#" +
                        std::to_string(instanceId);
                    auto chain = snapshot->geomToInstanceChain.find(id);
                    if (chain != snapshot->geomToInstanceChain.end())
                        for (InstanceLevel const &level : chain->second)
                            key += "|" + level.instancerPath.GetString() + ":" +
                                std::to_string(level.sourceIndex) + ":" +
                                std::to_string(level.ordinal) + ":" +
                                std::to_string(level.radix);
                    return key;
                };
                std::sort(ids.begin(), ids.end(), [&](unsigned int a, unsigned int b) {
                    return oldKey(a) < oldKey(b);
                });
                std::sort(placements.begin(), placements.end(),
                    [](InstancePlacement const &a, InstancePlacement const &b) {
                        auto key = [](InstancePlacement const &placement) {
                            std::string result = placement.instancerPath.GetString() +
                                "#" + std::to_string(placement.instanceId);
                            for (InstanceLevel const &level : placement.chain)
                                result += "|" + level.instancerPath.GetString() +
                                    ":" + std::to_string(level.sourceIndex) +
                                    ":" + std::to_string(level.ordinal) +
                                    ":" + std::to_string(level.radix);
                            return result;
                        };
                        return key(a) < key(b);
                    });
                auto prototype = snapshot->prototypes.find(prototypePath);
                if (prototype == snapshot->prototypes.end()) {
                    canUpdateInPlace = false;
                    break;
                }
                snapshot->lastPlacementRecordsVisited += ids.size();
                if (hiddenByDirtyPath && placements.empty()) {
                    for (unsigned int id : ids)
                        _SetInstanceEnabled(snapshot, id, false);
                    continue;
                }
                if (ids.size() != placements.size()) {
                    canUpdateInPlace = false;
                    break;
                }
                for (size_t i = 0; i < ids.size(); ++i) {
                    _SetInstanceEnabled(snapshot, ids[i], true);
                    _SetInstanceTransform(
                        snapshot, ids[i], placements[i].transform,
                        prototype->second);
                }
            }
            if (canUpdateInPlace) {
                if (cancelled && cancelled()) return false;
                auto const commitStart = std::chrono::steady_clock::now();
                rtcCommitScene(snapshot->scene->scene);
                snapshot->embreeSceneCommitSeconds +=
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - commitStart).count();
                return true;
            }
        }
        if (affected.empty()) {
            // Hidden prototypes have no top-level placement records. A later
            // visibility-on notice therefore cannot be recovered by rebuilding
            // the TLAS from Snapshot::prototypes: the prototype itself may have
            // been removed by an intervening geometry edit. Ask the caller for
            // a complete immutable-scene rebuild.
            if (needsFullSnapshot) *needsFullSnapshot = true;
            return false;
        }
    }
    if (rebuiltAll) *rebuiltAll = true;
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
    snapshot->placementEnabled.clear();
    snapshot->scene->geometry.clear();
    snapshot->scene->intersection_geometry.clear();
    snapshot->scene->placement_transforms.clear();
    snapshot->scene->placement_materials.clear();
    snapshot->instanceCount = 0;

    RTCScene oldScene = snapshot->scene->scene;
    struct SceneRelease {
        RTCScene scene = nullptr;
        ~SceneRelease() { if (scene) rtcReleaseScene(scene); }
    } oldSceneRelease{oldScene};
    snapshot->scene->scene = replacement;
    for (auto &[path, prototype] : snapshot->prototypes) {
        if (cancelled && cancelled()) return false;
        HdSceneIndexPrim prim = index->GetPrim(path);
        if (!_WorldVisible(index, path)) continue;
        std::vector<InstancePlacement> placements =
            _InstancePlacements(index, path, prim.dataSource);
        if (placements.empty() &&
            !HdInstancedBySchema::GetFromParent(prim.dataSource)) {
            placements.push_back({_WorldTransform(index, path), {}, -1, {}});
        }
        for (InstancePlacement const &placement : placements) {
            if (cancelled && cancelled()) return false;
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
    _AttachSphereBatches(snapshot);
    const auto commitStart = std::chrono::steady_clock::now();
    if (cancelled && cancelled()) return false;
    rtcCommitScene(replacement);
    _IndexPlacementDependencies(snapshot);
    snapshot->embreeSceneCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - commitStart).count();
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
    snapshot->placementEnabled.clear();
    rtcReleaseScene(oldTop);

    for (SdfPath const &path : paths) {
        // displayColor is a material input carried by a geometry primvar.  A
        // prototype refresh must recapture its value instead of reusing the
        // path cache populated by the previous data source.
        snapshot->displayMaterialsByPath.erase(path);
        _RemovePrototypeMaterialUsers(snapshot, path);
        auto found = snapshot->prototypes.find(path);
        if (found != snapshot->prototypes.end()) {
            Geometry *const oldGeometry = found->second.geometry;
            RTCScene const oldPrototypeScene = found->second.scene;
            auto bucketIt = snapshot->prototypeHashBuckets.find(
                found->second.geometryHash);
            if (bucketIt != snapshot->prototypeHashBuckets.end()) {
                auto &bucket = bucketIt->second;
                bucket.erase(std::remove(bucket.begin(), bucket.end(), path),
                             bucket.end());
                if (bucket.empty())
                    snapshot->prototypeHashBuckets.erase(bucketIt);
            }
            snapshot->prototypes.erase(found);
            const bool stillShared = std::any_of(
                snapshot->prototypes.begin(), snapshot->prototypes.end(),
                [&](auto const &entry) {
                    return entry.second.geometry == oldGeometry; });
            auto geometryIt = std::find_if(
                snapshot->geometries.begin(), snapshot->geometries.end(),
                [&](std::shared_ptr<Geometry> const &geometry) {
                    return geometry.get() == oldGeometry; });
            if (!stillShared && geometryIt != snapshot->geometries.end()) {
                if (Mesh const *mesh = dynamic_cast<Mesh *>((*geometryIt).get()))
                    snapshot->estimatedGeometryBytes -=
                        mesh->vertices.size() * sizeof(Float3) +
                        mesh->normals.size() * sizeof(Float3) +
                        mesh->texcoords.size() * sizeof(Float2) +
                        mesh->indices.size() * sizeof(unsigned int) +
                        mesh->tangents.size() * sizeof(float);
                else if (dynamic_cast<Sphere *>((*geometryIt).get()))
                    snapshot->estimatedGeometryBytes -= 4 * sizeof(float);
                snapshot->geometries.erase(geometryIt);
            }
            // PrototypeStorage releases Embree resources after both scene
            // versions and their in-flight readers have released ownership.
        }

        HdSceneIndexPrim const prim = index->GetPrim(path);
        if (prim.primType != HdPrimTypeTokens->mesh &&
            prim.primType != HdPrimTypeTokens->sphere) continue;
        std::vector<InstancePlacement> placements =
            _InstancePlacements(index, path, prim.dataSource);
        const bool instanced = bool(
            HdInstancedBySchema::GetFromParent(prim.dataSource));
        if (placements.empty() && !instanced) {
            if (prim.primType == HdPrimTypeTokens->mesh)
                _AppendMesh(index, path, prim, materials, snapshot,
                            _WorldTransform(index, path), {}, -1, {}, renderTags);
            else
                _AppendSphere(index, path, prim, materials, snapshot,
                              _WorldTransform(index, path), {}, -1, {}, renderTags);
        } else {
            for (InstancePlacement const &placement : placements) {
                if (prim.primType == HdPrimTypeTokens->mesh)
                    _AppendMesh(index, path, prim, materials, snapshot,
                                placement.transform, placement.instancerPath,
                                placement.instanceId, placement.chain, renderTags);
                else
                    _AppendSphere(index, path, prim, materials, snapshot,
                                  placement.transform, placement.instancerPath,
                                  placement.instanceId, placement.chain, renderTags);
            }
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
                       MaterialService *materials, Snapshot *snapshot,
                       std::unordered_set<SdfPath, SdfPath::Hash> const &paths = {})
{
    if (!snapshot) return;
    std::unordered_set<SdfPath, SdfPath::Hash> affectedPrototypes;
    std::unordered_set<SdfPath, SdfPath::Hash> affectedBatchedSpheres;
    if (paths.empty()) {
        snapshot->resolvedMaterials.clear();
    } else {
        for (SdfPath const &dirtyPath : paths) {
            snapshot->resolvedMaterials.erase(dirtyPath);
            if (snapshot->prototypes.count(dirtyPath))
                affectedPrototypes.insert(dirtyPath);
            auto users = snapshot->materialUsers.find(dirtyPath);
            if (users != snapshot->materialUsers.end()) {
                affectedPrototypes.insert(
                    users->second.begin(), users->second.end());
                for (SdfPath const &path : users->second)
                    if (snapshot->sphereBatchLocations.count(path))
                        affectedBatchedSpheres.insert(path);
            }
            if (snapshot->sphereBatchLocations.count(dirtyPath))
                affectedBatchedSpheres.insert(dirtyPath);
        }
        for (SdfPath const &prototypePath : affectedPrototypes)
            _RemovePrototypeMaterialUsers(snapshot, prototypePath);
    }
    auto refreshBatchedSphere = [&](SdfPath const &path) {
        auto location = snapshot->sphereBatchLocations.find(path);
        if (location == snapshot->sphereBatchLocations.end() ||
            location->second.batch >= snapshot->sphereBatches.size()) return;
        Snapshot::SphereBatch &batch =
            snapshot->sphereBatches[location->second.batch];
        if (!batch.geometry ||
            location->second.entry >= batch.geometry->entries.size()) return;
        _RemovePrototypeMaterialUsers(snapshot, path);
        snapshot->displayMaterialsByPath.erase(path);
        HdSceneIndexPrim const prim = index->GetPrim(path);
        const SdfPath materialPath = _MaterialPath(prim.dataSource);
        if (!materialPath.IsEmpty())
            _RecordMaterialUser(snapshot, materialPath, path);
        Material *material = _BaseMaterial(index, path, prim, materials, snapshot);
        batch.geometry->entries[location->second.entry].material = material;
        snapshot->scene->placement_materials[batch.geomId].triangles[
            static_cast<unsigned int>(location->second.entry)] = material;
        ++snapshot->materialBindingsUpdated;
        ++snapshot->sphereBatchEntriesUpdated;
    };
    if (paths.empty()) {
        for (auto const &[path, location] : snapshot->sphereBatchLocations)
            refreshBatchedSphere(path);
    } else {
        for (SdfPath const &path : affectedBatchedSpheres)
            refreshBatchedSphere(path);
    }
    auto refreshPrototype = [&](SdfPath const &path,
                                Snapshot::Prototype &prototype) {
        ++snapshot->materialPrototypesVisited;
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
        const SdfPath materialPath = _MaterialPath(prim.dataSource);
        if (!materialPath.IsEmpty())
            _RecordMaterialUser(snapshot, materialPath, path);
        if (auto resolved = _ResolveMaterial(
                index, materialPath, materials, snapshot))
            base = resolved.get();
        std::unordered_map<int, Material *> faceMaterials;
        for (SdfPath const &childPath : index->GetChildPrimPaths(path)) {
            HdSceneIndexPrim const child = index->GetPrim(childPath);
            HdGeomSubsetSchema const subset =
                HdGeomSubsetSchema::GetFromParent(child.dataSource);
            if (!subset) continue;
            VtIntArray faces;
            if (!_ReadArray(subset.GetIndices(), &faces)) continue;
            const SdfPath subsetMaterialPath = _MaterialPath(child.dataSource);
            if (!subsetMaterialPath.IsEmpty())
                _RecordMaterialUser(snapshot, subsetMaterialPath, path);
            if (auto resolved = _ResolveMaterial(
                    index, subsetMaterialPath, materials, snapshot))
                for (int face : faces) if (face >= 0)
                    faceMaterials[face] = resolved.get();
        }
        prototype.binding = {};
        prototype.binding.fallback = base;
        if (faceMaterials.empty()) {
            return;
        }
        for (size_t triangle = 0; triangle < prototype.coarseFaces.size(); ++triangle) {
            auto material = faceMaterials.find(prototype.coarseFaces[triangle]);
            if (material != faceMaterials.end())
                prototype.binding.triangles.emplace(
                    static_cast<int>(triangle), material->second);
        }
    };
    if (paths.empty()) {
        for (auto &[path, prototype] : snapshot->prototypes)
            refreshPrototype(path, prototype);
    } else {
        for (SdfPath const &path : affectedPrototypes) {
            auto prototype = snapshot->prototypes.find(path);
            if (prototype != snapshot->prototypes.end())
                refreshPrototype(path, prototype->second);
        }
    }
    if (paths.empty()) {
        for (auto const &[geomId, primId] : snapshot->geomToPrimId) {
            if (primId < 0 || static_cast<size_t>(primId) >=
                    snapshot->primPaths.size()) continue;
            auto prototype = snapshot->prototypes.find(
                snapshot->primPaths[primId]);
            if (prototype != snapshot->prototypes.end()) {
                snapshot->scene->placement_materials[geomId] =
                    prototype->second.binding;
                ++snapshot->materialBindingsUpdated;
            }
        }
    } else {
        for (SdfPath const &path : affectedPrototypes) {
            auto prototype = snapshot->prototypes.find(path);
            auto placements = snapshot->placementDependencies.find(path);
            if (prototype == snapshot->prototypes.end() ||
                placements == snapshot->placementDependencies.end()) continue;
            for (unsigned int geomId : placements->second) {
                auto prim = snapshot->geomToPrimId.find(geomId);
                if (prim == snapshot->geomToPrimId.end() || prim->second < 0 ||
                    static_cast<size_t>(prim->second) >=
                        snapshot->primPaths.size() ||
                    snapshot->primPaths[prim->second] != path) continue;
                snapshot->scene->placement_materials[geomId] =
                    prototype->second.binding;
                ++snapshot->materialBindingsUpdated;
            }
        }
    }
}

using RayPair = prism::CameraRayPair;

prism::CameraRayParameters _CameraParameters(CameraState const &camera)
{
    prism::CameraRayParameters parameters;
    parameters.cameraToWorld = camera.worldToView.GetInverse();
    parameters.inverseProjection = camera.projection.GetInverse();
    parameters.nearClip = camera.nearClip;
    parameters.farClip = camera.farClip;
    // HdCamera reports focal length in world units. UsdImaging has already
    // applied GfCamera::FOCAL_LENGTH_UNIT while adapting the USD attribute.
    parameters.apertureRadius = prism::CameraApertureRadius(
        camera.focalLength, camera.fStop);
    parameters.focusDistance = camera.focusDistance;
    parameters.orthographic = camera.orthographic;
    return parameters;
}

struct PreviewResult {
    unsigned int width = 0;
    unsigned int height = 0;
    bool published = false;
    double publicationMilliseconds = 0.0;
    uint64_t publishedBytes = 0;
    uint64_t tracedPixels = 0;
    uint64_t boundAovCount = 0;
};

PreviewResult _RenderPreview(std::shared_ptr<Snapshot> const &snapshot,
    CameraState const &camera, RenderSettings const &settings,
    HdRenderPassAovBindingVector const &bindings,
    unsigned int width, unsigned int height, HdRenderThread *renderThread,
    std::shared_ptr<const PublishedFrame> const &frame)
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
    result.tracedPixels = pixels;
    result.boundAovCount = targets.size();
    for (Target const &target : targets)
        result.publishedBytes += static_cast<uint64_t>(width) * height *
            HdDataSizeOfFormat(target.buffer->GetFormat());

    prism::CameraRayParameters const cameraParameters =
        _CameraParameters(camera);
    std::atomic<bool> cancelled{false};
    tbb::parallel_for(
        tbb::blocked_range2d<unsigned int>(
            0, result.height, 4, 0, result.width, 8),
        [&](tbb::blocked_range2d<unsigned int> const &range) {
    if (cancelled.load(std::memory_order_relaxed)) return;
    for (unsigned int py = range.rows().begin();
         py != range.rows().end(); ++py) {
        if (renderThread && renderThread->IsStopRequested()) {
            cancelled.store(true, std::memory_order_relaxed);
            return;
        }
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
            RayPair rays = prism::GenerateCameraRays(
                cameraParameters, x, y, width, height, seed);
            Color3 color = snapshot->scene->trace(
                prism::PathRay{rays.sampled, rays.footprint},
                0, Color3(1.0f), seed);
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
                        value = _PrimIdForHit(snapshot.get(), *primary);
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

    if (cancelled.load(std::memory_order_relaxed) ||
        (renderThread && renderThread->IsStopRequested())) return result;
    auto const publicationStart = std::chrono::steady_clock::now();
    for (Target const &target : targets) {
        if (!target.ints.empty())
            target.buffer->PublishPreviewInt(result.width, result.height,
                                              target.ints, target.components, frame);
        else
            target.buffer->PublishPreviewFloat(result.width, result.height,
                                                target.floats, target.components, frame);
    }
    result.publicationMilliseconds = 1000.0 * std::chrono::duration<double>(
        std::chrono::steady_clock::now() - publicationStart).count();
    result.published = true;
    return result;
}

} // namespace

class SceneBridge::Impl
{
public:
    static bool MemoryMonitor(void *user, ssize_t bytes, bool post)
    {
        (void)post;
        auto *self = static_cast<Impl *>(user);
        if (!self) return true;
        // Embree reports each allocation delta once, either before or after
        // the operation. `post` is relevant when an application rejects a
        // pre-allocation callback; Prism always permits allocations.
        const int64_t live = self->embreeLiveBytes.fetch_add(
            static_cast<int64_t>(bytes), std::memory_order_relaxed) + bytes;
        int64_t peak = self->embreePeakBytes.load(std::memory_order_relaxed);
        while (live > peak && !self->embreePeakBytes.compare_exchange_weak(
                   peak, live, std::memory_order_relaxed)) {}
        return true;
    }
    struct DirtyRecord {
        uint64_t generation = 0;
        unsigned int kinds = 0;
        std::unordered_set<SdfPath, SdfPath::Hash> paths;
    };
    struct BuildResult {
        uint64_t generation = 0;
        std::shared_ptr<Snapshot> snapshot;
        std::shared_ptr<Snapshot> spareSnapshot;
        std::unique_ptr<MaterialService> materialService;
        double seconds = 0.0;
        std::string error;
        bool reusedBank = false;
        bool visibilityRecoveryRebuild = false;
        bool sphereBatchFallback = false;
        uint64_t prototypeBuilds = 0;
    };

    Impl(HdSceneIndexBaseRefPtr index, HdRenderThread *thread)
        : sceneIndex(std::move(index)), renderThread(thread), device(rtcNewDevice(nullptr))
    {
        if (device)
            rtcSetDeviceMemoryMonitorFunction(device, &Impl::MemoryMonitor, this);
        if (const char *value = std::getenv("PRISM_PROFILE_BUFFERS"))
            profileBufferWrites = value[0] != '\0' && std::strcmp(value, "0") != 0;
        try {
            mdl::Context &context = SharedMdlContext();
            if (context.is_valid()) {
                const char *mx = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
                const char *mdl = std::getenv("PRISM_MATERIALX_MDL");
                materialService = std::make_unique<MaterialService>(
                    &context, mx ? mx : "", mdl ? mdl : "",
                    reinterpret_cast<uint64_t>(sceneIndex.operator->()));
            }
        } catch (std::exception const &e) {
            TF_WARN("PrismRT: MDL initialization failed: %s", e.what());
        }
    }

    ~Impl()
    {
        if (buildCancel) buildCancel->store(true, std::memory_order_relaxed);
        if (buildFuture.valid()) {
            try { (void)buildFuture.get(); }
            catch (...) {}
        }
        snapshot.reset();
        inactiveSnapshot.reset();
        // PrototypeStorage releases Embree scenes/geometries. Expire every
        // strong snapshot owner before releasing their shared RTCDevice.
        prototypePool.reset();
        if (device) rtcReleaseDevice(device);
    }

    HdSceneIndexBaseRefPtr sceneIndex;
    HdRenderThread *renderThread;
    RTCDevice device = nullptr;
    std::atomic<int64_t> embreeLiveBytes{0};
    std::atomic<int64_t> embreePeakBytes{0};
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
    std::shared_ptr<Snapshot> inactiveSnapshot;
    std::shared_ptr<const PublishedFrame> lastPublishedFrame;
    uint64_t activeGeneration = 0;
    uint64_t inactiveGeneration = 0;
    std::unique_ptr<MaterialService> materialService;
    MaterialService::MaterialStats materialWorkTotals;
    std::shared_ptr<prism::TextureService> textureService = prism::TextureService::Acquire();
    std::shared_ptr<PrototypePool> prototypePool =
        std::make_shared<PrototypePool>();
    uint64_t requestedGeneration = 1;
    uint64_t cameraRevision = 0;
    std::deque<DirtyRecord> dirtyJournal;
    uint64_t captureGeneration = 0;
    bool captureActive = false;
    HdRetainedSceneIndexRefPtr captureScene;
    HdRetainedSceneIndexRefPtr retainedSceneCache;
    std::deque<SdfPath> captureQueue;
    struct StaticPrimRecord {
        TfToken primType;
        HdContainerDataSourceHandle dataSource;
    };
    std::unordered_map<SdfPath, StaticPrimRecord, SdfPath::Hash> staticPrims;
    bool staticPrimsComplete = false;
    bool captureFull = true;
    TfTokenVector captureRenderTags;
    unsigned int captureDirtyKinds = 0;
    std::unordered_set<SdfPath, SdfPath::Hash> captureDirtyPaths;
    ArResolverContext captureResolverContext;
    std::future<BuildResult> buildFuture;
    std::shared_ptr<std::atomic<bool>> buildCancel;
    uint64_t buildingGeneration = 0;
    uint64_t discardedBuilds = 0;
    uint64_t reusedSnapshotBuilds = 0;
    uint64_t prototypeBuilds = 0;
    uint64_t capturedPrims = 0;
    double captureMilliseconds = 0.0;
    double captureBudgetMilliseconds = 0.0;
    std::chrono::steady_clock::time_point startTime;
    std::atomic<uint64_t> samples{0};
    uint64_t snapshotBuilds = 0;
    double snapshotBuildSeconds = 0.0;
    double renderSeconds = 0.0;
    uint64_t previewPublishes = 0;
    double previewMilliseconds = 0.0;
    double previewRayWorkMilliseconds = 0.0;
    double previewPublicationMilliseconds = 0.0;
    uint64_t previewPublishedBytes = 0;
    uint64_t previewTracedPixels = 0;
    uint64_t previewBoundAovCount = 0;
    double prepareMilliseconds = 0.0;
    double stopRenderMilliseconds = 0.0;
    unsigned int previewWidth = 0;
    unsigned int previewHeight = 0;
    uint64_t placementInvalidations = 0;
    uint64_t prototypeInvalidations = 0;
    uint64_t materialInvalidations = 0;
    uint64_t environmentInvalidations = 0;
    uint64_t conservativeRebuilds = 0;
    uint64_t visibilityRecoveryRebuilds = 0;
    uint64_t sphereBatchFallbacks = 0;
    bool profileBufferWrites = false;
    std::string lastRebuildReason = "initial population";
};

SceneBridge::SceneBridge(HdSceneIndexBaseRefPtr sceneIndex, HdRenderThread *thread)
    : _impl(std::make_unique<Impl>(std::move(sceneIndex), thread)) {}
SceneBridge::~SceneBridge()
{
    // Render() retains a local snapshot but continues to read Impl state and
    // buffers throughout the callback. Quiesce it before Impl tears either
    // down, including for owners other than PrismTaskDelegate.
    if (_impl && _impl->renderThread)
        _impl->renderThread->StopRender();
    _impl.reset();
}

void SceneBridge::MarkSceneDirty()
{
    MarkSceneDirty(SceneDirtyKind::Unknown);
}

void SceneBridge::MarkSceneDirty(SceneDirtyKind kind, SdfPath const &path)
{
    MarkScenePathsDirty(kind, path.IsEmpty() ? SdfPathVector{} : SdfPathVector{path});
}

void SceneBridge::MarkScenePathsDirty(SceneDirtyKind kind, SdfPathVector const &paths)
{
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    if (_impl->buildCancel)
        _impl->buildCancel->store(true, std::memory_order_relaxed);
    _impl->sceneDirty = true;
    ++_impl->requestedGeneration;
    SceneBridge::Impl::DirtyRecord record;
    record.generation = _impl->requestedGeneration;
    record.kinds = static_cast<unsigned int>(kind);
    record.paths.insert(paths.begin(), paths.end());
    _impl->dirtyJournal.push_back(std::move(record));
    _impl->dirtyKinds |= static_cast<unsigned int>(kind);
    _impl->dirtyPaths.insert(paths.begin(), paths.end());
    switch (kind) {
    case SceneDirtyKind::Placement: ++_impl->placementInvalidations; break;
    case SceneDirtyKind::Prototype: ++_impl->prototypeInvalidations; break;
    case SceneDirtyKind::Material: ++_impl->materialInvalidations; break;
    case SceneDirtyKind::Environment: ++_impl->environmentInvalidations; break;
    default: ++_impl->conservativeRebuilds; break;
    }
    _impl->lastRebuildReason = paths.empty()
        ? "unknown dirty locator" : paths.front().GetString();
    if (_impl->snapshot && _impl->materialService) {
        // Structural edits retain immutable graph/program caches. Only paths
        // actually affected by a notice lose their association shortcut.
        if (kind == SceneDirtyKind::Material) {
            if (paths.empty()) _impl->materialService->InvalidateAssociations({}, true);
            for (auto const& path : paths)
                _impl->materialService->InvalidateResources(path, true);
        } else if (kind == SceneDirtyKind::Structure || kind == SceneDirtyKind::Unknown) {
            if (paths.empty()) _impl->materialService->InvalidateAssociations({}, true);
            for (auto const& path : paths)
                _impl->materialService->InvalidateAssociations(path, true);
        }
    }
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
            current.adaptivePreview == settings.adaptivePreview &&
            current.previewTargetMs == settings.previewTargetMs &&
            current.previewMaxDimension == settings.previewMaxDimension &&
            current.refinementDelayMs == settings.refinementDelayMs &&
            current.resourceRevision == settings.resourceRevision &&
            current.sphereBatching == settings.sphereBatching &&
            current.textureFiltering == settings.textureFiltering &&
            current.environmentSampling == settings.environmentSampling)
            return;
    }
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    if (_impl->materialService)
        _impl->materialService->SetResourceRevision(
            static_cast<uint64_t>(settings.resourceRevision));
    if (_impl->snapshot &&
        _impl->settings.resourceRevision != settings.resourceRevision) {
        if (_impl->materialService) _impl->materialService->InvalidateAll();
        _impl->sceneDirty = true;
        if (_impl->buildCancel)
            _impl->buildCancel->store(true, std::memory_order_relaxed);
        ++_impl->requestedGeneration;
        _impl->dirtyJournal.push_back({
            _impl->requestedGeneration,
            static_cast<unsigned int>(SceneDirtyKind::Material) |
                static_cast<unsigned int>(SceneDirtyKind::Environment),
            {}});
        _impl->dirtyKinds |= static_cast<unsigned int>(SceneDirtyKind::Material) |
            static_cast<unsigned int>(SceneDirtyKind::Environment);
        ++_impl->materialInvalidations;
        ++_impl->environmentInvalidations;
        _impl->lastRebuildReason = "resource revision changed";
    }
    if (_impl->snapshot &&
        _impl->settings.sphereBatching != settings.sphereBatching) {
        _impl->sceneDirty = true;
        if (_impl->buildCancel)
            _impl->buildCancel->store(true, std::memory_order_relaxed);
        ++_impl->requestedGeneration;
        _impl->dirtyJournal.push_back({
            _impl->requestedGeneration,
            static_cast<unsigned int>(SceneDirtyKind::Structure), {}});
        _impl->dirtyKinds |= static_cast<unsigned int>(SceneDirtyKind::Structure);
        ++_impl->conservativeRebuilds;
        _impl->lastRebuildReason = "sphere batching changed";
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
    bool stateChanged = false;
    bool hasPreparationWork = false;
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
        stateChanged = _impl->stateDirty;
        hasPreparationWork = _impl->sceneDirty || _impl->captureActive ||
            _impl->buildFuture.valid();
        if (!_impl->sceneDirty && !_impl->stateDirty && !cameraChanged &&
            !bindingsChanged && !renderTagsChanged && !hasPreparationWork)
            return false;
    }

    // Only state that changes the currently displayed frame needs to quiesce
    // the render worker here. Scene notices already request a stop in
    // MarkSceneDirty; capture-only Prepare calls leave the old shaded snapshot
    // available for navigation.
    if (cameraChanged || bindingsChanged || renderTagsChanged) {
        auto const stopStart = std::chrono::steady_clock::now();
        _impl->renderThread->StopRender();
        std::lock_guard lock(_impl->mutex);
        _impl->stopRenderMilliseconds = 1000.0 * std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stopStart).count();
    }

    {
        std::lock_guard lock(_impl->mutex);
        if (renderTagsChanged) {
            if (_impl->buildCancel)
                _impl->buildCancel->store(true, std::memory_order_relaxed);
            _impl->sceneDirty = true;
            ++_impl->requestedGeneration;
            _impl->dirtyKinds |= static_cast<unsigned int>(
                SceneDirtyKind::Unknown);
            _impl->dirtyJournal.push_back({
                _impl->requestedGeneration,
                static_cast<unsigned int>(SceneDirtyKind::Unknown), {}});
            _impl->lastRebuildReason = "render tags changed";
        }
        _impl->camera = camera;
        if (cameraChanged) ++_impl->cameraRevision;
        _impl->bindings = bindings;
        _impl->renderTags = renderTags;
        _impl->width = width;
        _impl->height = height;
    }

    // Consume a completed worker result without ever waiting for a running
    // conversion. Superseded generations are destroyed without publication.
    SceneBridge::Impl::BuildResult completed;
    bool hasCompleted = false;
    bool published = false;
    {
        std::lock_guard lock(_impl->mutex);
        if (_impl->buildFuture.valid() &&
            _impl->buildFuture.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            completed = _impl->buildFuture.get();
            // These are renderer-lifetime work counters, including obsolete
            // jobs. The historical per-generation timing gauges below remain
            // separate so replacing a service cannot erase measured work.
            if (completed.materialService) {
                const auto work = completed.materialService->GetStats();
                _impl->materialWorkTotals.conversionCalls += work.conversionCalls;
                _impl->materialWorkTotals.generationCalls += work.generationCalls;
                _impl->materialWorkTotals.templateCacheHits += work.templateCacheHits;
                _impl->materialWorkTotals.sdkTranslationCalls += work.sdkTranslationCalls;
                _impl->materialWorkTotals.programCacheHits += work.programCacheHits;
                _impl->materialWorkTotals.bindingCreations += work.bindingCreations;
                _impl->materialWorkTotals.fallbackTranslations += work.fallbackTranslations;
                _impl->materialWorkTotals.programWaitSeconds += work.programWaitSeconds;
                _impl->materialWorkTotals.textureBytes += work.textureBytes;
            }
            _impl->buildingGeneration = 0;
            _impl->buildCancel.reset();
            hasCompleted = true;
        }
    }
    if (hasCompleted) {
        bool publish = false;
        {
            std::lock_guard lock(_impl->mutex);
            publish = completed.snapshot && completed.error.empty() &&
                completed.generation == _impl->requestedGeneration &&
                !_impl->sceneDirty;
            if (!publish) ++_impl->discardedBuilds;
            if (!publish && completed.generation == _impl->requestedGeneration &&
                !_impl->sceneDirty) {
                _impl->sceneDirty = true;
                _impl->lastRebuildReason = "background generation failed";
            }
        }
        if (publish) {
            auto const stopStart = std::chrono::steady_clock::now();
            _impl->renderThread->StopRender();
            std::lock_guard lock(_impl->mutex);
            _impl->stopRenderMilliseconds = 1000.0 *
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - stopStart).count();
            // StopRender above is blocking, so no render callback can retain
            // the displayed snapshot while it becomes the inactive bank.
            if (completed.spareSnapshot) {
                _impl->inactiveSnapshot = std::move(completed.spareSnapshot);
                _impl->inactiveGeneration = completed.generation;
            } else if (_impl->snapshot) {
                _impl->inactiveSnapshot = std::move(_impl->snapshot);
                _impl->inactiveGeneration = _impl->activeGeneration;
            }
            _impl->snapshot = std::move(completed.snapshot);
            _impl->activeGeneration = completed.generation;
            while (!_impl->dirtyJournal.empty() &&
                   _impl->dirtyJournal.front().generation <=
                       _impl->inactiveGeneration)
                _impl->dirtyJournal.pop_front();
            _impl->materialService = std::move(completed.materialService);
            _impl->snapshotBuildSeconds = completed.seconds;
            if (completed.reusedBank) ++_impl->reusedSnapshotBuilds;
            else ++_impl->snapshotBuilds;
            if (completed.visibilityRecoveryRebuild) {
                ++_impl->visibilityRecoveryRebuilds;
                ++_impl->conservativeRebuilds;
                _impl->lastRebuildReason =
                    "visibility restore requires missing prototype records";
            }
            if (completed.sphereBatchFallback) {
                ++_impl->sphereBatchFallbacks;
                ++_impl->conservativeRebuilds;
                _impl->lastRebuildReason =
                    "sphere batch eligibility or topology changed";
            }
            _impl->prototypeBuilds += completed.prototypeBuilds;
            published = true;
        } else if (!completed.error.empty()) {
            TF_WARN("PrismRT: background snapshot generation failed: %s",
                    completed.error.c_str());
        }
    }

    // Start or advance an owner-thread capture. Each call is bounded between
    // prim boundaries: about three milliseconds once a snapshot is displayed,
    // longer before the first one. _MakeStaticCopy retains typed sampled
    // leaves at time zero and recursively severs live handles.
    double captureBudget = kSteadyCaptureBudgetMilliseconds;
    bool captureWorked = false;
    {
        std::lock_guard lock(_impl->mutex);
        if (!_impl->snapshot)
            captureBudget = kInitialCaptureBudgetMilliseconds;
        if (_impl->sceneDirty && !_impl->captureActive &&
            !_impl->buildFuture.valid()) {
            _impl->captureActive = true;
            _impl->captureGeneration = _impl->requestedGeneration;
            _impl->captureQueue.clear();
            const unsigned int unsafeCapture =
                static_cast<unsigned int>(SceneDirtyKind::Structure) |
                static_cast<unsigned int>(SceneDirtyKind::Unknown);
            _impl->captureFull = !_impl->staticPrimsComplete ||
                (_impl->dirtyKinds & unsafeCapture) != 0;
            if (_impl->captureFull) {
                _impl->captureScene = HdRetainedSceneIndex::New();
                _impl->staticPrims.clear();
                _impl->staticPrimsComplete = false;
                _impl->captureQueue.push_back(
                    SdfPath::AbsoluteRootPath());
            } else {
                _impl->captureScene = _impl->retainedSceneCache;
                for (SdfPath const &path : _impl->dirtyPaths)
                    _impl->captureQueue.push_back(path);
            }
            _impl->captureRenderTags = _impl->renderTags;
            _impl->captureDirtyKinds = _impl->dirtyKinds;
            _impl->captureDirtyPaths = _impl->dirtyPaths;
            _impl->captureResolverContext =
                ArGetResolver().GetCurrentContext();
            _impl->capturedPrims = 0;
        }
    }
    auto const captureStart = std::chrono::steady_clock::now();
    for (;;) {
        std::lock_guard lock(_impl->mutex);
        if (!_impl->captureActive) break;
        if (_impl->captureGeneration != _impl->requestedGeneration) {
            _impl->captureActive = false;
            _impl->captureScene.Reset();
            _impl->captureQueue.clear();
            break;
        }
        if (!_impl->captureQueue.empty()) {
            captureWorked = true;
            SdfPath const path = _impl->captureQueue.front();
            _impl->captureQueue.pop_front();
            HdSceneIndexPrim const prim = _impl->sceneIndex->GetPrim(path);
            if (_CapturePrimData(prim.primType) &&
                (!prim.primType.IsEmpty() || prim.dataSource)) {
                _impl->staticPrims[path] = {
                    prim.primType,
                    _MakeStaticPrimCopy(prim.primType, prim.dataSource)};
                auto const &record = _impl->staticPrims[path];
                _impl->captureScene->AddPrims({{
                    path, record.primType, record.dataSource}});
                ++_impl->capturedPrims;
            } else {
                _impl->staticPrims.erase(path);
                // HdRetainedSceneIndex owns its own prim table. Erasing the
                // parallel record cache alone leaves the removed subtree
                // visible to the background converter.
                _impl->captureScene->RemovePrims({path});
            }
            if (_impl->captureFull) {
                for (SdfPath const &child :
                     _impl->sceneIndex->GetChildPrimPaths(path))
                    _impl->captureQueue.push_back(child);
            }
        } else break;
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - captureStart).count() >= captureBudget)
            break;
    }
    {
        std::lock_guard lock(_impl->mutex);
        _impl->captureMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - captureStart).count();
        if (captureWorked) _impl->captureBudgetMilliseconds = captureBudget;
        if (_impl->captureActive && _impl->captureQueue.empty() &&
            _impl->captureGeneration != _impl->requestedGeneration) {
            _impl->captureActive = false;
            _impl->captureScene.Reset();
        }
        if (_impl->captureActive && _impl->captureQueue.empty() &&
            !_impl->buildFuture.valid()) {
            _impl->staticPrimsComplete = true;
            _impl->retainedSceneCache = _impl->captureScene;
            const uint64_t generation = _impl->captureGeneration;
            HdSceneIndexBaseRefPtr retained = _impl->captureScene;
            TfTokenVector tags = _impl->captureRenderTags;
            RenderSettings jobSettings = _impl->settings;
            unsigned int jobDirtyKinds = _impl->captureDirtyKinds;
            auto jobDirtyPaths = _impl->captureDirtyPaths;
            auto jobPlacementPaths =
                (jobDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Placement))
                    ? jobDirtyPaths : decltype(jobDirtyPaths){};
            auto jobPrototypePaths =
                (jobDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Prototype))
                    ? jobDirtyPaths : decltype(jobDirtyPaths){};
            auto jobMaterialPaths =
                (jobDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Material))
                    ? jobDirtyPaths : decltype(jobDirtyPaths){};
            ArResolverContext resolverContext = _impl->captureResolverContext;
            RTCDevice device = _impl->device;
            std::shared_ptr<PrototypePool> prototypePool =
                _impl->prototypePool;
            std::shared_ptr<Snapshot> bank = std::move(
                _impl->inactiveSnapshot);
            const uint64_t bankGeneration = _impl->inactiveGeneration;
            if (bank) {
                jobDirtyKinds = 0;
                jobDirtyPaths.clear();
                jobPlacementPaths.clear();
                jobPrototypePaths.clear();
                jobMaterialPaths.clear();
                for (auto const &record : _impl->dirtyJournal) {
                    if (record.generation <= bankGeneration ||
                        record.generation > generation) continue;
                    jobDirtyKinds |= record.kinds;
                    jobDirtyPaths.insert(
                        record.paths.begin(), record.paths.end());
                    if (record.kinds & static_cast<unsigned int>(
                            SceneDirtyKind::Placement))
                        jobPlacementPaths.insert(
                            record.paths.begin(), record.paths.end());
                    if (record.kinds & static_cast<unsigned int>(
                            SceneDirtyKind::Prototype))
                        jobPrototypePaths.insert(
                            record.paths.begin(), record.paths.end());
                    if (record.kinds & static_cast<unsigned int>(
                            SceneDirtyKind::Material))
                        jobMaterialPaths.insert(
                            record.paths.begin(), record.paths.end());
                }
            }
            _impl->inactiveGeneration = 0;
            std::string mx = std::getenv("PRISM_MATERIALX_LIBRARY_PATH") ?
                std::getenv("PRISM_MATERIALX_LIBRARY_PATH") : "";
            std::string mdl = std::getenv("PRISM_MATERIALX_MDL") ?
                std::getenv("PRISM_MATERIALX_MDL") : "";
            _impl->captureActive = false;
            _impl->captureScene.Reset();
            _impl->sceneDirty = false;
            _impl->dirtyKinds = 0;
            _impl->dirtyPaths.clear();
            _impl->buildingGeneration = generation;
            auto cancel = std::make_shared<std::atomic<bool>>(false);
            _impl->buildCancel = cancel;
            _impl->buildFuture = std::async(std::launch::async,
                [generation, associationScope = reinterpret_cast<uint64_t>(_impl->sceneIndex.operator->()),
                 retained = std::move(retained),
                 tags = std::move(tags), jobSettings, resolverContext,
                 jobDirtyKinds,
                 jobPlacementPaths = std::move(jobPlacementPaths),
                 jobPrototypePaths = std::move(jobPrototypePaths),
                 jobMaterialPaths = std::move(jobMaterialPaths),
                 bank = std::move(bank), device,
                 prototypePool = std::move(prototypePool),
                 cancel = std::move(cancel),
                 mx = std::move(mx), mdl = std::move(mdl)]() mutable {
                    SceneBridge::Impl::BuildResult result;
                    result.generation = generation;
                    auto const start = std::chrono::steady_clock::now();
                    try {
                        ArResolverContextBinder binder(resolverContext);
                        mdl::Context &context = SharedMdlContext();
                        if (context.is_valid()) {
                            result.materialService =
                                std::make_unique<MaterialService>(
                                    &context, mx, mdl, associationScope);
                            result.materialService->SetResourceRevision(
                                static_cast<uint64_t>(
                                    jobSettings.resourceRevision));
                        }
                        const unsigned int unsafeKinds =
                            static_cast<unsigned int>(SceneDirtyKind::Structure) |
                            static_cast<unsigned int>(SceneDirtyKind::Unknown);
                        if (cancel->load(std::memory_order_relaxed)) {
                            return result;
                        } else if (bank && (jobDirtyKinds & unsafeKinds) == 0) {
                            const uint64_t priorPrototypeBuilds =
                                bank->prototypeBuildCount;
                            bool placementRebuiltAll = false;
                            bool placementNeedsFullSnapshot = false;
                            bool batchNeedsFullSnapshot = false;
                            std::unordered_set<SdfPath, SdfPath::Hash>
                                batchDirtyPaths = jobPrototypePaths;
                            batchDirtyPaths.insert(jobPlacementPaths.begin(),
                                                   jobPlacementPaths.end());
                            std::unordered_set<SdfPath, SdfPath::Hash>
                                handledBatchPaths;
                            if (!_RefreshSphereBatches(
                                    retained, batchDirtyPaths,
                                    result.materialService.get(), bank.get(),
                                    tags, &handledBatchPaths)) {
                                bank = _BuildSnapshot(
                                    retained, result.materialService.get(),
                                    jobSettings, tags, device, prototypePool,
                                    [cancel] {
                                        return cancel->load(
                                            std::memory_order_relaxed);
                                    });
                                if (!bank) return result;
                                batchNeedsFullSnapshot = true;
                                placementRebuiltAll = true;
                                result.sphereBatchFallback = true;
                            }
                            for (SdfPath const &path : handledBatchPaths) {
                                jobPrototypePaths.erase(path);
                                jobPlacementPaths.erase(path);
                            }
                            if (!batchNeedsFullSnapshot &&
                                jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Prototype))
                            {
                                _RefreshPrototypes(
                                    retained, jobPrototypePaths,
                                    result.materialService.get(), bank.get(), tags);
                            }
                            if (cancel->load(std::memory_order_relaxed))
                                return result;
                            // Prototype refreshes rebuild the inactive TLAS and
                            // its dependency index. Replay placement and
                            // visibility edits afterward so a bank that is one
                            // generation behind still preserves the latest
                            // enabled state and transforms.
                            if (!batchNeedsFullSnapshot &&
                                jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Placement))
                                _UpdatePlacements(retained, bank.get(),
                                  jobPlacementPaths, [cancel] {
                                    return cancel->load(
                                        std::memory_order_relaxed);
                                }, &placementRebuiltAll,
                                   &placementNeedsFullSnapshot);
                            if (cancel->load(std::memory_order_relaxed))
                                return result;
                            if (!batchNeedsFullSnapshot &&
                                placementNeedsFullSnapshot) {
                                bank = _BuildSnapshot(
                                    retained, result.materialService.get(),
                                    jobSettings, tags, device, prototypePool,
                                    [cancel] {
                                        return cancel->load(
                                            std::memory_order_relaxed);
                                    });
                                placementRebuiltAll = true;
                                if (!bank) return result;
                            }
                            if (jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Material))
                                _RefreshMaterials(
                                    retained, result.materialService.get(), bank.get(),
                                    jobMaterialPaths);
                            if (cancel->load(std::memory_order_relaxed))
                                return result;
                            if (jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Environment)) {
                                bank->domeCount = 0;
                                auto black = std::make_unique<ColorTexture>(
                                    Color3(0.0f));
                                bank->environment =
                                    std::make_unique<ConstantColorBackground>(
                                        black.get());
                                bank->environmentTexture = std::move(black);
                                bank->scene->environment = bank->environment.get();
                                _RefreshEnvironment(
                                    retained, SdfPath::AbsoluteRootPath(), bank.get());
                            }
                            result.snapshot = std::move(bank);
                            const bool rebuiltSnapshot =
                                placementNeedsFullSnapshot ||
                                batchNeedsFullSnapshot;
                            result.reusedBank = !rebuiltSnapshot;
                            result.visibilityRecoveryRebuild =
                                placementNeedsFullSnapshot;
                            result.prototypeBuilds = rebuiltSnapshot
                                ? result.snapshot->prototypeBuildCount
                                : result.snapshot->prototypeBuildCount -
                                    priorPrototypeBuilds;
                            if (placementRebuiltAll && result.snapshot &&
                                !cancel->load(std::memory_order_relaxed)) {
                                // A visibility restore with no retained
                                // placement IDs rebuilt the full TLAS. Keep the
                                // second bank at the same state so the next
                                // unrelated edit cannot republish an older
                                // hidden scene.
                                result.spareSnapshot = _BuildSnapshot(
                                    retained, result.materialService.get(),
                                    jobSettings, tags, device, prototypePool,
                                    [cancel] {
                                        return cancel->load(
                                            std::memory_order_relaxed);
                                    });
                            }
                        } else {
                            result.snapshot = _BuildSnapshot(
                                retained, result.materialService.get(),
                                jobSettings, tags, device, prototypePool,
                                [cancel] {
                                    return cancel->load(
                                        std::memory_order_relaxed);
                                });
                            result.prototypeBuilds = result.snapshot
                                ? result.snapshot->prototypeBuildCount : 0;
                            if (result.snapshot &&
                                !cancel->load(std::memory_order_relaxed)) {
                                // Keep a synchronized mutable bank so the
                                // first local edit after a structural rebuild
                                // does not require another full conversion.
                                result.spareSnapshot = _BuildSnapshot(
                                    retained, result.materialService.get(),
                                    jobSettings, tags, device, prototypePool,
                                    [cancel] {
                                        return cancel->load(
                                            std::memory_order_relaxed);
                                    });
                            }
                        }
                        if (result.snapshot && result.snapshot->scene) {
                            auto picking = std::make_shared<PickingTable>();
                            picking->primPaths = result.snapshot->primPaths;
                            picking->instancerPaths =
                                result.snapshot->instancerPaths;
                            _BuildPickingInstanceTopologies(
                                result.snapshot.get(), picking.get());
                            result.snapshot->picking = std::move(picking);
                            result.snapshot->scene->max_depth =
                                jobSettings.maxDepth;
                            result.snapshot->scene->rr_min_depth =
                                jobSettings.rouletteDepth;
                            result.snapshot->scene->environmentSampling =
                                jobSettings.environmentSampling ==
                                    RenderSettings::EnvironmentSampling::Mis;
                            result.snapshot->scene->textureFiltering =
                                jobSettings.textureFiltering;
                        }
                    } catch (std::exception const &e) {
                        result.error = e.what();
                    } catch (...) {
                        result.error = "unknown exception";
                    }
                    result.seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start).count();
                    return result;
                });
        }
    }

    std::lock_guard lock(_impl->mutex);
    if ((stateChanged || published) && _impl->snapshot &&
        _impl->snapshot->scene) {
        _impl->snapshot->scene->max_depth = _impl->settings.maxDepth;
        _impl->snapshot->scene->rr_min_depth = _impl->settings.rouletteDepth;
        _impl->snapshot->scene->environmentSampling =
            _impl->settings.environmentSampling ==
                RenderSettings::EnvironmentSampling::Mis;
        _impl->snapshot->scene->textureFiltering =
            _impl->settings.textureFiltering;
    }
    if (stateChanged || cameraChanged || bindingsChanged ||
        renderTagsChanged || published) {
        _impl->samples.store(0);
        for (HdRenderPassAovBinding const &binding : _impl->bindings) {
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer)) {
                buffer->SetConverged(false);
            }
        }
        _impl->startTime = std::chrono::steady_clock::now();
        _impl->renderSeconds = 0.0;
    }
    _impl->stateDirty = false;
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
    bool preparing = false;
    uint64_t generation = 0;
    uint64_t cameraRevision = 0;
    unsigned int width, height;
    {
        std::lock_guard lock(_impl->mutex);
        snapshot = _impl->snapshot; camera = _impl->camera;
        bindings = _impl->bindings; settings = _impl->settings;
        width = _impl->width; height = _impl->height;
        generation = _impl->activeGeneration;
        cameraRevision = _impl->cameraRevision;
        preparing = _impl->sceneDirty || _impl->captureActive ||
            _impl->buildFuture.valid();
    }
    if (!snapshot || !snapshot->scene || width == 0 || height == 0) {
        for (HdRenderPassAovBinding const &binding : bindings)
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->SetConverged(!preparing);
        {
            std::lock_guard lock(_impl->mutex);
            _impl->renderSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _impl->startTime).count();
        }
        _converged.store(!preparing); return;
    }
    auto frame = std::make_shared<PublishedFrame>();
    frame->generation = generation;
    frame->cameraRevision = cameraRevision;
    frame->picking = snapshot->picking;
    frame->camera = camera;
    frame->width = width;
    frame->height = height;
    // Preview rendering runs on Hydra's render worker.  Prepare only captures
    // the raw AOV bindings after StopRender has quiesced their previous user;
    // buffer destruction and resize also stop this worker before changing
    // storage, so no preparation worker ever dereferences a render buffer.
    RenderSettings previewSettings = settings;
    if (settings.adaptivePreview) {
        std::lock_guard lock(_impl->mutex);
        if (_impl->previewMilliseconds > 0.0) {
            const double ratio = std::sqrt(
                static_cast<double>(std::clamp(settings.previewTargetMs, 1, 1000)) /
                _impl->previewMilliseconds);
            previewSettings.previewMaxDimension = std::clamp(
                static_cast<int>(std::lround(std::max(
                    _impl->previewWidth, _impl->previewHeight) *
                    std::clamp(ratio, 0.5, 2.0))),
                16, std::clamp(settings.previewMaxDimension, 16, 1024));
        }
    }
    auto const previewStart = std::chrono::steady_clock::now();
    PreviewResult const preview = _RenderPreview(
        snapshot, camera, previewSettings, bindings, width, height,
        _impl->renderThread, frame);
    if (_impl->renderThread->IsStopRequested()) return;
    {
        std::lock_guard lock(_impl->mutex);
        _impl->previewMilliseconds = 1000.0 * std::chrono::duration<double>(
            std::chrono::steady_clock::now() - previewStart).count();
        _impl->previewPublicationMilliseconds =
            preview.publicationMilliseconds;
        _impl->previewRayWorkMilliseconds = std::max(
            0.0, _impl->previewMilliseconds -
                preview.publicationMilliseconds);
        _impl->previewPublishedBytes = preview.publishedBytes;
        _impl->previewTracedPixels = preview.tracedPixels;
        _impl->previewBoundAovCount = preview.boundAovCount;
        _impl->previewWidth = preview.width;
        _impl->previewHeight = preview.height;
        if (preview.published) {
            ++_impl->previewPublishes;
            _impl->lastPublishedFrame = frame;
        }
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
    prism::CameraRayParameters const cameraParameters =
        _CameraParameters(camera);
    prism::DumpCameraRayDiagnostics(
        cameraParameters, width, height, settings.seed, "hydra");
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
                        RayPair rays = prism::GenerateCameraRays(
                            cameraParameters, x, y, width, height, seed);
                        Color3 c = snapshot->scene->trace(
                            prism::PathRay{rays.sampled, rays.footprint},
                            0, Color3(1.0f), seed);
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
                                int id = _PrimIdForHit(snapshot.get(), *primary);
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
                buffer->Publish(frame);
        }
        {
            std::lock_guard lock(_impl->mutex);
            _impl->lastPublishedFrame = frame;
        }
        _impl->samples.store(static_cast<uint64_t>(sample + 1));
    }
    for (HdRenderPassAovBinding const &binding : bindings)
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            buffer->SetConverged(!preparing);
    {
        std::lock_guard lock(_impl->mutex);
        _impl->renderSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - _impl->startTime).count();
    }
    _converged.store(!preparing);
}

SdfPath SceneBridge::GetPrimPath(int primId) const
{
    std::lock_guard lock(_impl->mutex);
    auto const picking = _impl->lastPublishedFrame
        ? _impl->lastPublishedFrame->picking : nullptr;
    if (!picking || primId < 0 ||
        static_cast<size_t>(primId) >= picking->primPaths.size()) return {};
    return picking->primPaths[primId];
}

SdfPath SceneBridge::GetInstancerPath(int primId) const
{
    std::lock_guard lock(_impl->mutex);
    auto const picking = _impl->lastPublishedFrame
        ? _impl->lastPublishedFrame->picking : nullptr;
    if (!picking || primId < 0 ||
        static_cast<size_t>(primId) >= picking->instancerPaths.size()) return {};
    return picking->instancerPaths[primId];
}

std::vector<PrismRenderBuffer *> SceneBridge::GetRenderBuffers() const
{
    std::vector<PrismRenderBuffer *> result;
    std::lock_guard lock(_impl->mutex);
    result.reserve(_impl->bindings.size());
    for (HdRenderPassAovBinding const &binding : _impl->bindings)
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            result.push_back(buffer);
    return result;
}

void SceneBridge::RemoveRenderBuffer(HdRenderBuffer *buffer)
{
    if (!buffer) return;
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    _impl->bindings.erase(std::remove_if(
        _impl->bindings.begin(), _impl->bindings.end(),
        [buffer](HdRenderPassAovBinding const &binding) {
            return binding.renderBuffer == buffer;
        }), _impl->bindings.end());
    _impl->stateDirty = true;
    _converged.store(false);
}

VtDictionary SceneBridge::GetStats() const
{
    VtDictionary result;
    result["completedSamples"] = VtValue(static_cast<int64_t>(_impl->samples.load()));
    result["converged"] = VtValue(_converged.load());
    std::lock_guard lock(_impl->mutex);
    result["meshCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? std::count_if(
            _impl->snapshot->geometries.begin(),
            _impl->snapshot->geometries.end(),
            [](std::shared_ptr<Geometry> const &geometry) {
                return dynamic_cast<Mesh *>(geometry.get()) != nullptr;
            }) : 0));
    int64_t triangles = 0;
    if (_impl->snapshot) {
        for (auto const &geometry : _impl->snapshot->geometries)
            if (Mesh const *mesh = dynamic_cast<Mesh *>(geometry.get()))
                triangles += static_cast<int64_t>(mesh->indices.size() / 3);
    }
    result["triangleCount"] = VtValue(triangles);
    result["primCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->primPaths.size() : 0));
    result["snapshotBuilds"] = VtValue(static_cast<int64_t>(
        _impl->snapshotBuilds));
    result["snapshotBuildSeconds"] = VtValue(_impl->snapshotBuildSeconds);
    const auto textureStats = _impl->textureService->stats();
    result["processTextureSourceBytesHashed"] = VtValue(static_cast<int64_t>(textureStats.sourceBytesHashed));
    result["processTextureHashCalls"] = VtValue(static_cast<int64_t>(textureStats.resourceHashCalls));
    result["processTextureSourceWaitSeconds"] = VtValue(textureStats.sourceWaitNanoseconds / 1.0e9);
    result["processTextureContentWaitSeconds"] = VtValue(textureStats.contentWaitNanoseconds / 1.0e9);
    const auto resourceStats = prism::materialx::GetResolvedMaterialResourceStats();
    result["processMaterialResourceReadCalls"] = VtValue(static_cast<int64_t>(resourceStats.sourceReadCalls));
    result["processMaterialResourceHashCalls"] = VtValue(static_cast<int64_t>(resourceStats.sourceHashCalls));
    result["processMaterialResourceBytesRead"] = VtValue(static_cast<int64_t>(resourceStats.sourceBytesRead));
    result["processMaterialResourceWaitSeconds"] = VtValue(resourceStats.waitNanoseconds / 1.0e9);
    result["processPreparationAdmissionWaitSeconds"] = VtValue(
        prism::PreparationAdmission::Shared().admissionWaitNanoseconds() / 1.0e9);

    result["preparationPending"] = VtValue(
        _impl->sceneDirty || _impl->captureActive ||
        _impl->buildFuture.valid());
    result["preparationGeneration"] = VtValue(static_cast<int64_t>(
        _impl->requestedGeneration));
    result["displayedGeneration"] = VtValue(static_cast<int64_t>(
        _impl->activeGeneration));
    result["displayedScenePredatesEdits"] = VtValue(
        bool(_impl->snapshot) &&
        _impl->activeGeneration < _impl->requestedGeneration);
    result["requestedCameraRevision"] = VtValue(static_cast<int64_t>(
        _impl->cameraRevision));
    result["displayedCameraRevision"] = VtValue(static_cast<int64_t>(
        _impl->lastPublishedFrame
            ? _impl->lastPublishedFrame->cameraRevision : 0));
    result["buildingGeneration"] = VtValue(static_cast<int64_t>(
        _impl->buildingGeneration));
    result["capturedPrims"] = VtValue(static_cast<int64_t>(
        _impl->capturedPrims));
    result["captureQueueRemaining"] = VtValue(static_cast<int64_t>(
        _impl->captureQueue.size()));
    result["retainedPrimRecordCount"] = VtValue(static_cast<int64_t>(
        _impl->staticPrims.size()));
    result["captureMilliseconds"] = VtValue(_impl->captureMilliseconds);
    result["captureBudgetMilliseconds"] = VtValue(
        _impl->captureBudgetMilliseconds);
    result["discardedSnapshotBuilds"] = VtValue(static_cast<int64_t>(
        _impl->discardedBuilds));
    result["reusedSnapshotBuilds"] = VtValue(static_cast<int64_t>(
        _impl->reusedSnapshotBuilds));
    result["renderSeconds"] = VtValue(_impl->renderSeconds);
    result["previewPublishes"] = VtValue(static_cast<int64_t>(
        _impl->previewPublishes));
    result["previewMilliseconds"] = VtValue(_impl->previewMilliseconds);
    result["previewRayWorkMilliseconds"] = VtValue(
        _impl->previewRayWorkMilliseconds);
    result["previewPublicationMilliseconds"] = VtValue(
        _impl->previewPublicationMilliseconds);
    result["previewPublishedBytes"] = VtValue(static_cast<int64_t>(
        _impl->previewPublishedBytes));
    result["previewTracedPixels"] = VtValue(static_cast<int64_t>(
        _impl->previewTracedPixels));
    result["previewBoundAovCount"] = VtValue(static_cast<int64_t>(
        _impl->previewBoundAovCount));
    result["prepareMilliseconds"] = VtValue(_impl->prepareMilliseconds);
    result["stopRenderMilliseconds"] = VtValue(
        _impl->stopRenderMilliseconds);
    result["previewWidth"] = VtValue(static_cast<int64_t>(_impl->previewWidth));
    result["previewHeight"] = VtValue(static_cast<int64_t>(_impl->previewHeight));
    result["instanceCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->instanceCount : 0));
    result["prototypeCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->geometries.size() : 0));
    result["prototypePathCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->prototypes.size() : 0));
    int64_t pickingTopologyPrimCount = 0;
    if (_impl->snapshot && _impl->snapshot->picking)
        pickingTopologyPrimCount = static_cast<int64_t>(std::count_if(
            _impl->snapshot->picking->instanceTopologies.begin(),
            _impl->snapshot->picking->instanceTopologies.end(),
            [](auto const &levels) { return !levels.empty(); }));
    result["pickingInstanceTopologyPrimCount"] = VtValue(
        pickingTopologyPrimCount);
    result["pickingInstanceTopologyFailures"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->pickingTopologyFailures : 0));
    result["sphereBatchCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->sphereBatches.size() : 0));
    int64_t sphereBatchEntries = 0;
    if (_impl->snapshot)
        for (auto const &batch : _impl->snapshot->sphereBatches)
            sphereBatchEntries += static_cast<int64_t>(batch.paths.size());
    result["sphereBatchEntryCount"] = VtValue(sphereBatchEntries);
    result["sphereBatchRebuilds"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->sphereBatchRebuilds : 0));
    result["sphereBatchEntriesUpdated"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->sphereBatchEntriesUpdated : 0));
    result["sphereBatchConservativeFallbacks"] = VtValue(
        static_cast<int64_t>(_impl->sphereBatchFallbacks));
    result["materialPrototypesVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->materialPrototypesVisited : 0));
    result["materialBindingsUpdated"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->materialBindingsUpdated : 0));
    result["lastPlacementRecordsVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPlacementRecordsVisited : 0));
    result["lastVisibilityPlacementsUpdated"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ?
            _impl->snapshot->lastVisibilityPlacementsUpdated : 0));
    result["prototypeBuildCount"] = VtValue(static_cast<int64_t>(
        _impl->prototypeBuilds));
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
    result["activeSnapshotGeometryBytes"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->estimatedGeometryBytes : 0));
    result["inactiveSnapshotGeometryBytes"] = VtValue(static_cast<int64_t>(
        _impl->inactiveSnapshot
            ? _impl->inactiveSnapshot->estimatedGeometryBytes : 0));
    result["activeEnvironmentImportanceTableBytes"] = VtValue(
        static_cast<int64_t>(_EnvironmentImportanceBytes(
            _impl->snapshot.get())));
    result["inactiveEnvironmentImportanceTableBytes"] = VtValue(
        static_cast<int64_t>(_EnvironmentImportanceBytes(
            _impl->inactiveSnapshot.get())));
    result["retainedSnapshotCount"] = VtValue(static_cast<int64_t>(
        (_impl->snapshot ? 1 : 0) + (_impl->inactiveSnapshot ? 1 : 0)));
    result["embreeLiveBytes"] = VtValue(static_cast<int64_t>(
        std::max<int64_t>(0, _impl->embreeLiveBytes.load(
            std::memory_order_relaxed))));
    result["embreePeakBytes"] = VtValue(static_cast<int64_t>(
        std::max<int64_t>(0, _impl->embreePeakBytes.load(
            std::memory_order_relaxed))));
    result["placementInvalidations"] = VtValue(static_cast<int64_t>(_impl->placementInvalidations));
    result["prototypeInvalidations"] = VtValue(static_cast<int64_t>(_impl->prototypeInvalidations));
    result["materialInvalidations"] = VtValue(static_cast<int64_t>(_impl->materialInvalidations));
    result["environmentInvalidations"] = VtValue(static_cast<int64_t>(_impl->environmentInvalidations));
    result["conservativeRebuilds"] = VtValue(static_cast<int64_t>(_impl->conservativeRebuilds));
    result["visibilityRecoveryRebuilds"] = VtValue(static_cast<int64_t>(
        _impl->visibilityRecoveryRebuilds));
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
        result["materialConversionCalls"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.conversionCalls));
        result["materialGenerationCalls"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.generationCalls));
        result["materialTemplateCacheHits"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.templateCacheHits));
        result["materialPathCacheHits"] = VtValue(static_cast<int64_t>(stats.pathCacheHits));
        result["materialContentCacheHits"] = VtValue(static_cast<int64_t>(stats.contentCacheHits));
        result["materialEarlyCacheHits"] = VtValue(static_cast<int64_t>(stats.earlyCacheHits));
        result["materialCompilations"] = VtValue(static_cast<int64_t>(stats.compilations));
        result["materialFoldedCompilations"] = VtValue(static_cast<int64_t>(stats.foldedCompilations));
        result["materialNativeJits"] = VtValue(static_cast<int64_t>(stats.nativeJits));
        result["materialSdkTranslationCalls"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.sdkTranslationCalls));
        result["materialProgramCacheHits"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.programCacheHits));
        result["materialBindingCreations"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.bindingCreations));
        result["materialFallbackTranslations"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.fallbackTranslations));
        result["materialProgramWaitSeconds"] = VtValue(_impl->materialWorkTotals.programWaitSeconds);
        result["materialFailures"] = VtValue(static_cast<int64_t>(stats.failures));
        result["sdkTextureFallbacks"] = VtValue(static_cast<int64_t>(
            stats.sdkTextureFallbacks));
        result["customTextureTargets"] = VtValue(static_cast<int64_t>(
            stats.customTextureTargets));
        result["materialConversionSeconds"] = VtValue(stats.conversionSeconds);
        result["materialCompilationSeconds"] = VtValue(stats.compilationSeconds);
        result["materialXGenerationSeconds"] = VtValue(stats.generationSeconds);
        result["materialXPersistentGenerationCacheHits"] =
            VtValue(static_cast<int64_t>(stats.persistentGenerationCacheHits));
        result["materialClassCompilationSeconds"] = VtValue(
            stats.classCompilationSeconds);
        result["materialNativeJitSeconds"] = VtValue(stats.nativeJitSeconds);
        result["materialResourceReadBytesCumulative"] = VtValue(static_cast<int64_t>(_impl->materialWorkTotals.textureBytes));
        result["materialResourceBytes"] = VtValue(static_cast<int64_t>(stats.textureBytes));
        result["materialCacheAccountedBytes"] = VtValue(static_cast<int64_t>(
            stats.cacheAccountedBytes));
        result["materialUnusedCacheBytes"] = VtValue(static_cast<int64_t>(
            stats.unusedCacheBytes));
        result["materialCacheEvictions"] = VtValue(static_cast<int64_t>(
            stats.cacheEvictions));
        result["materialPreparationPeakReservedBytes"] = VtValue(
            static_cast<int64_t>(stats.preparationPeakReservedBytes));
        result["materialResourceReadSeconds"] = VtValue(stats.resourceReadSeconds);
        result["materialBatchWallSeconds"] = VtValue(stats.batchWallSeconds);
        result["materialWorkerCpuSeconds"] = VtValue(stats.workerCpuSeconds);
    }
    return result;
}

} // namespace prism::hydra
