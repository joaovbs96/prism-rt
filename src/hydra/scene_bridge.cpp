#include "scene_bridge.h"
#include "fallback_material.h"

#include "materials.h"
#include "environment_cache.h"
#include "render_buffer.h"
#include "refinement_tiles.h"
#include "triangulation.h"
#include "../mesh/subdivision.h"

#include "../environment.h"
#include "../lights.h"
#include "../load_profile.h"
#include "../output/aov.h"
#include "../batched_spheres.h"
#include "../camera_rays.h"
#include "../camera_ray_diagnostics.h"
#include "../geometry.h"
#include "../material.h"
#include "../materialx/resolved_resource.h"
#include "../mdl/context.h"
#include "../progress.h"
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
#include "pxr/usd/usdLux/blackbody.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/imaging/hd/categoriesSchema.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/geomSubsetSchema.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/subdivisionTagsSchema.h"
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
#include <tbb/task_group.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <tuple>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <deque>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <optional>
#include <queue>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::hydra {
namespace {

void _WarnUnsupportedAov(TfToken const &name)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warned;
    std::lock_guard lock(mutex);
    if (warned.insert(name.GetString()).second)
        TF_WARN("PrismRT: AOV '%s' is not supported", name.GetText());
}

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

struct _LightTokenSet {
    TfToken treatAsPoint{"treatAsPoint"};
    // Hydra's light:cameraVisibility (HdsiDomeLightCameraVisibilitySceneIndex
    // writes it for domes); UsdImaging reads it from the light prim's
    // `cameraVisibility` attribute.
    TfToken cameraVisibility{"cameraVisibility"};
};
_LightTokenSet const &_LightTokens()
{
    static const _LightTokenSet tokens;
    return tokens;
}

HdContainerDataSourceHandle _MakeStaticPrimCopy(
    TfToken const &primType, HdContainerDataSourceHandle const &source)
{
    if (!source || !HdPrimTypeIsLight(primType))
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
            HdLightTokens->exposure, HdLightTokens->textureFile,
            HdLightTokens->textureFormat, HdLightTokens->normalize,
            HdLightTokens->enableColorTemperature, HdLightTokens->colorTemperature,
            HdLightTokens->radius, HdLightTokens->width, HdLightTokens->height,
            HdLightTokens->length, HdLightTokens->angle, _LightTokens().treatAsPoint,
            _LightTokens().cameraVisibility, HdLightTokens->diffuse, HdLightTokens->specular,
            HdTokens->lightLink, HdTokens->shadowLink};
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

// RenderSettings::materialBindingPurposes of the latest SetSettings: read by
// every snapshot build and refresh, which do not carry the settings.
std::atomic<std::shared_ptr<const TfTokenVector>> &_BindingPurposes()
{
    static std::atomic<std::shared_ptr<const TfTokenVector>> purposes{
        std::make_shared<const TfTokenVector>(RenderSettings{}.materialBindingPurposes)};
    return purposes;
}

SdfPath _MaterialPath(HdContainerDataSourceHandle const &ds)
{
    HdMaterialBindingsSchema bindings = HdMaterialBindingsSchema::GetFromParent(ds);
    if (!bindings) return {};
    const auto purposes = _BindingPurposes().load();
    for (TfToken const &purpose : *purposes) {
        if (auto p = bindings.GetMaterialBinding(purpose).GetPath())
            return p->GetTypedValue(0.0f);
    }
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

using CanonicalPrimIds = std::unordered_map<PrimKey, int, PrimKeyHash>;

struct InstanceLevel {
    SdfPath instancerPath;
    int sourceIndex = -1;
    int ordinal = -1;
    int radix = 0;
    int duplicateRank = 0;
    bool operator==(InstanceLevel const &) const = default;
};

struct PlacementIdentityLevel {
    SdfPath instancerPath;
    int sourceIndex, duplicateRank;
    bool operator==(PlacementIdentityLevel const &) const = default;
};
struct PlacementKey {
    SdfPath prototypePath;
    std::vector<PlacementIdentityLevel> chain;
    bool operator==(PlacementKey const &) const = default;
};
struct PlacementKeyHash {
    size_t operator()(PlacementKey const &key) const {
        size_t value = SdfPath::Hash{}(key.prototypePath);
        auto combine = [&](size_t part) {
            value ^= part + size_t{0x9e3779b9} + (value << 6) + (value >> 2);
        };
        for (auto const &level : key.chain) {
            combine(SdfPath::Hash{}(level.instancerPath));
            combine(std::hash<int>{}(level.sourceIndex));
            combine(std::hash<int>{}(level.duplicateRank));
        }
        return value;
    }
};
PlacementKey _PlacementKey(SdfPath const &path, std::vector<InstanceLevel> const &chain) {
    PlacementKey key{path, {}};
    for (auto const &level : chain)
        key.chain.push_back({level.instancerPath, level.sourceIndex, level.duplicateRank});
    return key;
}

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
    std::atomic<uint64_t> lastEnabled{0};
    ~PrototypeStorage() {
        if (scene) rtcReleaseScene(scene);
        if (geometry) {
            if (geometry->geom) rtcReleaseGeometry(geometry->geom);
            delete geometry->transform;
            geometry->transform = nullptr;
        }
    }
};
// Everything mesh conversion derives from a prim data source except the Mesh
// itself. It is small enough to cache beside the shared storage, so a reused
// conversion still replays its warnings and material-subset metadata.
struct MeshConversionInfo {
    SharedFaceTable coarseFaces;
    size_t faceCount = 0;
    size_t geometryHash = 0;
    size_t malformedFaceCount = 0;
    size_t degenerateFaceCount = 0;
    // Warnings after the motion check apply only once topology was readable.
    bool topologyRead = false;
    bool motionBlur = false;
    bool multipleUvSets = false;
    bool subdivisionControlHull = false;
    int subdivisionLevel = 0;
    bool missingPoints = false;
    bool unsupportedPoints = false;
};

// Geometry allocates its transform as a raw pointer; free it with a mesh that
// is discarded because identical storage already exists.
struct ConvertedMeshDeleter {
    void operator()(Mesh *mesh) const
    {
        if (!mesh) return;
        delete mesh->transform;
        delete mesh;
    }
};

struct MeshConversion {
    MeshConversionInfo info;
    // Null when no triangle survived conversion.
    std::unique_ptr<Mesh, ConvertedMeshDeleter> mesh;
};

struct PrototypePool {
    std::atomic<uint64_t> topLevelSceneBuilds{0};
    std::atomic<uint64_t> topLevelSceneCommits{0};
    std::atomic<uint64_t> structuralPlacementDetachCalls{0};
    EnvironmentCache environmentCache;
    std::shared_ptr<ColorTexture> whiteEnvironmentTexture =
        std::make_shared<ColorTexture>(Color3(1.0f));
    std::mutex mutex;
    std::unordered_map<size_t, std::vector<std::weak_ptr<PrototypeStorage>>> buckets;
    std::unordered_set<SdfPath, SdfPath::Hash> warnedPointPaths;
    // Captured prim data sources are immutable, so an unchanged handle means
    // an unchanged conversion. Each entry holds its handle so the address
    // cannot be reused while it serves as the key.
    struct ConversionEntry {
        HdContainerDataSourceHandle source;
        std::weak_ptr<PrototypeStorage> storage;
        MeshConversionInfo info;
        uint64_t lastBuild = 0;
        // The material that displaced the geometry (null: none).
        Material const *displacer = nullptr;
    };
    std::unordered_map<HdDataSourceBase const *, ConversionEntry> conversions;
    uint64_t conversionBuilds = 0;
    std::atomic<uint64_t> residencyClock{0};
    std::atomic<uint64_t> hiddenGeometryBudgetBytes{0};
    std::atomic<uint64_t> hiddenGeometryEvictions{0};
    std::atomic<uint64_t> hiddenGeometryRestorations{0};
    struct Residency {
        std::weak_ptr<PrototypeStorage> storage;
        size_t bytes = 0;
        bool enabled = false;
        std::vector<SdfPath> paths;
    };
    // Immutable manifests allow accounting across banks and pinned historical
    // frames without reading a snapshot while a worker is editing it.
    using ResidencyManifest = std::unordered_map<PrototypeStorage *, Residency>;
    std::unordered_map<void const *, ResidencyManifest> residencyManifests;
    std::unordered_set<SdfPath, SdfPath::Hash> evictedPaths;
    std::unordered_set<PrototypeStorage *> currentEnabledStorage;
    std::unordered_map<PrototypeStorage *, std::weak_ptr<PrototypeStorage>> selectedEvictions;
    mutable uint64_t usageRevision = ~uint64_t{0};
    mutable uint64_t cachedHiddenResidentBytes = 0;
    mutable uint64_t cachedHiddenPinnedBytes = 0;
};

// A mesh prim's conversion resolved to prototype storage. Snapshot builds
// prepare these before the walk; prototype refreshes create one inline.
struct PreparedMesh {
    MeshConversionInfo info;
    // Null when conversion produced no triangles.
    std::shared_ptr<PrototypeStorage> storage;
    bool builtStorage = false;
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
    // Cached diagnostics: recomputed with snapshot construction or geometry
    // refreshes, never while polling GetStats().
    size_t meshCount = 0;
    int64_t triangleCount = 0;
    size_t livePrimCount = 0;
    size_t primPathSlotCount = 0;
    std::shared_ptr<PrototypePool> prototypePool = std::make_shared<PrototypePool>();
    std::unordered_map<SdfPath, std::shared_ptr<Material>, SdfPath::Hash>
        resolvedMaterials;
    // Spare banks are built from the published snapshot's resolved
    // materials instead of a MaterialService; a path missing from that seed
    // is recorded rather than compiled.
    bool materialsSeeded = false;
    bool materialSeedMissed = false;
    std::unique_ptr<ColorTexture> fallbackTexture;
    std::unique_ptr<Diffuse> fallbackMaterial;
    std::vector<std::unique_ptr<ColorTexture>> displayColorTextures;
    std::vector<std::unique_ptr<Diffuse>> displayColorMaterials;
    std::unordered_map<SdfPath, Material *, SdfPath::Hash> displayMaterialsByPath;
    std::unique_ptr<Texture<Color3>> environmentTexture;
    std::unique_ptr<Environment> environment;
    // With several domes: each of them; `environment` is then their composite.
    std::vector<std::unique_ptr<Environment>> domeEnvironments;
    // Light/shadow-link categories of the lights, as bits (SceneLight).
    std::unordered_map<TfToken, uint32_t, TfToken::HashFunctor> lightLinkBits, shadowLinkBits;
    std::unordered_map<unsigned int, int> geomToPrimId;
    std::unordered_map<unsigned int, SharedFaceTable> geomToCoarseFaces;
    std::unordered_map<unsigned int, int> geomToInstanceId;
    std::unordered_map<unsigned int, std::vector<InstanceLevel>>
        geomToInstanceChain;
    std::unordered_map<unsigned int, bool> placementEnabled;
    uint64_t enabledPlacementCount = 0;
    std::unordered_map<SdfPath, uint64_t, SdfPath::Hash> enabledPrototypePlacements;
    std::unordered_map<unsigned int, GfMatrix4d> placementMatrices;
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
    CanonicalPrimIds primIds;
    std::shared_ptr<const CanonicalPrimIds> canonicalIds;
    bool primIdentityDirty = true;
    size_t estimatedGeometryBytes = 0;
    uint64_t instanceCount = 0;
    uint64_t prototypeBuildCount = 0;
    uint64_t prototypeConversions = 0;
    uint64_t prototypeConversionCacheHits = 0;
    // Meshes converted alongside material preparation and converted again
    // because a material turned out to displace (see _PrepareMeshes).
    uint64_t speculativeMeshRetries = 0;
    double meshConversionSeconds = 0.0;
    double embreeGeometryCommitSeconds = 0.0;
    double embreeSceneCommitSeconds = 0.0;
    double sceneTraversalSeconds = 0.0;
    size_t domeCount = 0;
    std::unordered_set<std::string> warnedFeatures;
    std::unordered_set<SdfPath, SdfPath::Hash> warnedMotionPaths;
    std::unordered_map<SdfPath, Prototype, SdfPath::Hash> prototypes;
    // Canonical prim identities survive eviction; resident Prototype records
    // and every owning geometry alias are removed together.
    std::unordered_set<SdfPath, SdfPath::Hash> evictedPrototypes;
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
    // Persistent identities avoid reconstructing a hash table for every
    // position-array notice. Input snapshots belong to this bank's generation.
    std::unordered_map<PlacementKey, unsigned int, PlacementKeyHash> placementIds;
    std::unordered_map<SdfPath, std::unordered_map<int, std::unordered_set<unsigned int>>,
        SdfPath::Hash> instanceSourcePlacements;
    std::unordered_map<SdfPath, HdContainerDataSourceHandle, SdfPath::Hash> instancerInputs;
    std::unordered_map<SdfPath, std::unordered_set<SdfPath, SdfPath::Hash>, SdfPath::Hash>
        prototypeDependencies;
    std::unordered_map<SdfPath, std::unordered_set<SdfPath, SdfPath::Hash>, SdfPath::Hash>
        prototypeDependencyPaths, prototypeInstancerPaths, instancerPrototypeUsers;
    uint64_t materialPrototypesVisited = 0;
    uint64_t materialBindingsUpdated = 0;
    uint64_t lastCanonicalRecordsVisited = 0;
    uint64_t lastPickingRecordsVisited = 0;
    std::unordered_set<int> changedPickingPrims;
    uint64_t lastCountRecordsVisited = 0;
    uint64_t lastInputMetadataPrototypesVisited = 0;
    uint64_t lastDependencyPlacementsVisited = 0;
    double lastCanonicalMilliseconds = 0;
    double lastPickingMilliseconds = 0;
    double lastCountsMilliseconds = 0;
    double lastDependencyMilliseconds = 0;
    double lastInputMetadataMilliseconds = 0;
    double lastInputComparisonMilliseconds = 0;
    double lastPlacementReconcileMilliseconds = 0;
    uint64_t lastPlacementRecordsVisited = 0;
    uint64_t lastInstanceArrayElementsScanned = 0;
    uint64_t lastPlacementTopologyAttached = 0;
    uint64_t lastPlacementTopologyDetached = 0;
    uint64_t lastVisibilityPlacementsUpdated = 0;
    uint64_t lastPlacementTransformsUpdated = 0;
    uint64_t lastPrototypePlacementsRepointed = 0;
    uint64_t lastPrototypePathsUpdated = 0;
    uint64_t lastStructuralPathsUpdated = 0;
    uint64_t lastStructuralPlacementsAttached = 0;
    uint64_t lastStructuralPlacementsDetached = 0;
    uint64_t sphereBatchRebuilds = 0;
    uint64_t sphereBatchEntriesUpdated = 0;
    uint64_t sphereBatchConservativeFallbacks = 0;
    uint64_t pickingTopologyFailures = 0;

    ~Snapshot()
    {
        {
            std::lock_guard lock(prototypePool->mutex);
            prototypePool->residencyManifests.erase(this);
            ++prototypePool->residencyClock;
        }
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

struct AccumulateMilliseconds {
    double &total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~AccumulateMilliseconds() {
        total += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
};

int _EnsurePrimId(Snapshot *snapshot, PrimKey const &key)
{
    auto existing = snapshot->primIds.find(key);
    int id = existing == snapshot->primIds.end() ? -1 : existing->second;
    if (existing != snapshot->primIds.end()) return id;
    snapshot->primIdentityDirty = true;
    if (id < 0 && snapshot->canonicalIds) {
        auto canonical = snapshot->canonicalIds->find(key);
        if (canonical != snapshot->canonicalIds->end()) id = canonical->second;
    }
    if (id < 0) id = static_cast<int>(snapshot->primPaths.size());
    const size_t slots = std::max(snapshot->primPaths.size(), static_cast<size_t>(id + 1));
    snapshot->primPaths.resize(slots); snapshot->instancerPaths.resize(slots);
    snapshot->primPaths[id] = key.path; snapshot->instancerPaths[id] = key.instancerPath;
    snapshot->primIds[key] = id;
    snapshot->livePrimCount = snapshot->primIds.size();
    snapshot->primPathSlotCount = snapshot->primPaths.size();
    return id;
}

void _RefreshCachedCounts(Snapshot *snapshot)
{
    if (!snapshot) return;
    AccumulateMilliseconds timing{snapshot->lastCountsMilliseconds};
    snapshot->lastCountRecordsVisited += snapshot->geometries.size() + snapshot->primPaths.size();
    snapshot->meshCount = 0;
    snapshot->triangleCount = 0;
    for (auto const &geometry : snapshot->geometries) {
        if (Mesh const *mesh = dynamic_cast<Mesh *>(geometry.get())) {
            ++snapshot->meshCount;
            snapshot->triangleCount += static_cast<int64_t>(
                mesh->indices.size() / 3);
        }
    }
    snapshot->livePrimCount = static_cast<size_t>(std::count_if(
        snapshot->primPaths.begin(), snapshot->primPaths.end(),
        [](SdfPath const &path) { return !path.IsEmpty(); }));
    snapshot->primPathSlotCount = snapshot->primPaths.size();
}

bool _CanonicalizePrimIds(Snapshot *snapshot, CanonicalPrimIds *registry,
    std::shared_ptr<const CanonicalPrimIds> const &registryView = {})
{
    if (!snapshot) return false;
    // Registry IDs are append-only. A bank whose identities did not change
    // already uses those IDs, even when another generation reserved more slots.
    if (!snapshot->primIdentityDirty && snapshot->canonicalIds &&
        snapshot->canonicalIds->size() == registry->size()) {
        snapshot->primPathSlotCount = snapshot->primPaths.size();
        return false;
    }
    AccumulateMilliseconds timing{snapshot->lastCanonicalMilliseconds};
    snapshot->lastCanonicalRecordsVisited += snapshot->primIds.size() +
        snapshot->geomToPrimId.size();
    if (registry->empty()) {
        *registry = snapshot->primIds;
    } else {
        std::vector<PrimKey> missing;
        for (auto const &[key, id] : snapshot->primIds)
            if (!registry->contains(key)) missing.push_back(key);
        std::sort(missing.begin(), missing.end(), [](PrimKey const &a, PrimKey const &b) {
            return a.path != b.path ? a.path < b.path : a.instancerPath < b.instancerPath;
        });
        for (auto const &key : missing) registry->emplace(key, static_cast<int>(registry->size()));
    }
    std::vector<int> remap(snapshot->primPaths.size(), -1);
    std::vector<SdfPath> paths(registry->size()), instancers(registry->size());
    for (auto &[key, id] : snapshot->primIds) {
        const int canonical = registry->at(key);
        if (id >= 0 && static_cast<size_t>(id) < remap.size()) remap[id] = canonical;
        id = canonical; paths[id] = key.path; instancers[id] = key.instancerPath;
    }
    auto mapId = [&](int id) { return id >= 0 && static_cast<size_t>(id) < remap.size() ? remap[id] : -1; };
    for (auto &[geom, id] : snapshot->geomToPrimId) id = mapId(id);
    for (auto &[geom, ids] : snapshot->sphereBatchPrimIds) for (int &id : ids) id = mapId(id);
    for (auto &batch : snapshot->sphereBatches) for (int &id : batch.primIds) id = mapId(id);
    for (auto &entry : snapshot->pendingSpheres) entry.primId = mapId(entry.primId);
    const bool changed = paths != snapshot->primPaths || instancers != snapshot->instancerPaths;
    snapshot->primPaths = std::move(paths); snapshot->instancerPaths = std::move(instancers);
    snapshot->canonicalIds = registryView && registryView->size() == registry->size()
        ? registryView : std::make_shared<const CanonicalPrimIds>(*registry);
    snapshot->primIdentityDirty = false;
    snapshot->livePrimCount = snapshot->primIds.size();
    snapshot->primPathSlotCount = snapshot->primPaths.size();
    return changed;
}

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
    if (materialPath.IsEmpty() ||
        (!materials && !snapshot->materialsSeeded)) return {};
    auto const found = snapshot->resolvedMaterials.find(materialPath);
    if (found != snapshot->resolvedMaterials.end()) return found->second;
    if (!materials) {
        // A seeded build must never differ from the snapshot it copies.
        snapshot->materialSeedMissed = true;
        return {};
    }
    HdSceneIndexPrim const materialPrim = sceneIndex->GetPrim(materialPath);
    std::shared_ptr<Material> resolved = materials->Resolve(
        materialPrim.dataSource, materialPath);
    snapshot->resolvedMaterials.emplace(materialPath, resolved);
    return resolved;
}

// The material bound to a mesh when it displaces it (the geometry then
// depends on it). Serial: resolution fills the snapshot's material table.
Material const *_DisplacingMaterial(HdSceneIndexBaseRefPtr const &sceneIndex,
                                    HdContainerDataSourceHandle const &ds,
                                    MaterialService *materials, Snapshot *snapshot)
{
    const SdfPath materialPath = _MaterialPath(ds);
    if (materialPath.IsEmpty()) return nullptr;
    std::shared_ptr<Material> material = _ResolveMaterial(sceneIndex, materialPath, materials, snapshot);
    return material && material->hasDisplacement() ? material.get() : nullptr;
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
    snapshot->scene->disabled_placements.erase(id);
    snapshot->placementEnabled[id] = true;
    snapshot->placementMatrices[id] = world;
    for (SdfPath ancestor = path; !ancestor.IsEmpty() && ancestor != SdfPath::AbsoluteRootPath();
         ancestor = ancestor.GetParentPath()) snapshot->placementDependencies[ancestor].insert(id);
    ++snapshot->enabledPlacementCount;
    ++snapshot->enabledPrototypePlacements[path];
    snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - commitStart).count();
    return id;
}

void _SetInstanceTransform(Snapshot *snapshot, unsigned int id,
                           GfMatrix4d const &world,
                           Snapshot::Prototype const &prototype)
{
    auto const previous = snapshot->placementMatrices.find(id);
    if (previous != snapshot->placementMatrices.end() && previous->second == world) return;
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
    snapshot->placementMatrices[id] = world;
    ++snapshot->lastPlacementTransformsUpdated;
}

void _TrackPrototypeEnabled(Snapshot *snapshot, unsigned int id, bool enabled)
{
    auto prim = snapshot->geomToPrimId.find(id);
    if (prim == snapshot->geomToPrimId.end() || prim->second < 0 ||
        static_cast<size_t>(prim->second) >= snapshot->primPaths.size()) return;
    SdfPath const &path = snapshot->primPaths[prim->second];
    if (enabled) ++snapshot->enabledPrototypePlacements[path];
    else if (auto count = snapshot->enabledPrototypePlacements.find(path);
             count != snapshot->enabledPrototypePlacements.end()) {
        if (--count->second == 0) snapshot->enabledPrototypePlacements.erase(count);
    }
}

void _SetInstanceEnabled(Snapshot *snapshot, unsigned int id, bool enabled)
{
    auto state = snapshot->placementEnabled.find(id);
    const bool changed = state == snapshot->placementEnabled.end() ||
        state->second != enabled;
    if (!changed) return;
    RTCGeometry instance = rtcGetGeometry(snapshot->scene->scene, id);
    if (!instance) return;
    if (enabled) rtcEnableGeometry(instance);
    else rtcDisableGeometry(instance);
    rtcCommitGeometry(instance);
    if (enabled) snapshot->scene->disabled_placements.erase(id);
    else snapshot->scene->disabled_placements.insert(id);
    if (enabled) {
        ++snapshot->enabledPlacementCount;
        _TrackPrototypeEnabled(snapshot, id, true);
    } else if (state != snapshot->placementEnabled.end() && state->second) {
        --snapshot->enabledPlacementCount;
        _TrackPrototypeEnabled(snapshot, id, false);
    }
    snapshot->placementEnabled[id] = enabled;
    if (changed) ++snapshot->lastVisibilityPlacementsUpdated;
}

// Update only one placement's memberships, using its old maps before removal.
void _IndexPlacementDependency(Snapshot *snapshot, unsigned int id, bool attach)
{
    auto prim = snapshot->geomToPrimId.find(id);
    if (prim == snapshot->geomToPrimId.end() || prim->second < 0 ||
        static_cast<size_t>(prim->second) >= snapshot->primPaths.size()) return;
    AccumulateMilliseconds timing{snapshot->lastDependencyMilliseconds};
    ++snapshot->lastDependencyPlacementsVisited;
    const int primId = prim->second;
    auto ancestors = [&](SdfPath path) {
        while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath()) {
            if (attach) snapshot->placementDependencies[path].insert(id);
            else if (auto it = snapshot->placementDependencies.find(path);
                     it != snapshot->placementDependencies.end()) {
                it->second.erase(id);
                if (it->second.empty()) snapshot->placementDependencies.erase(it);
            }
            path = path.GetParentPath();
        }
    };
    ancestors(snapshot->primPaths[primId]);
    ancestors(snapshot->instancerPaths[primId]);
    auto chain = snapshot->geomToInstanceChain.find(id);
    auto key = _PlacementKey(snapshot->primPaths[primId],
        chain == snapshot->geomToInstanceChain.end() ? std::vector<InstanceLevel>{} : chain->second);
    if (attach) snapshot->placementIds[key] = id;
    else snapshot->placementIds.erase(key);
    if (chain != snapshot->geomToInstanceChain.end())
        for (InstanceLevel const &level : chain->second) {
            ancestors(level.instancerPath);
            if (attach)
                snapshot->instanceSourcePlacements[level.instancerPath][level.sourceIndex].insert(id);
            else if (auto instancer = snapshot->instanceSourcePlacements.find(level.instancerPath);
                     instancer != snapshot->instanceSourcePlacements.end()) {
                auto source = instancer->second.find(level.sourceIndex);
                if (source != instancer->second.end()) {
                    source->second.erase(id);
                    if (source->second.empty()) instancer->second.erase(source);
                }
                if (instancer->second.empty()) snapshot->instanceSourcePlacements.erase(instancer);
            }
        }
}

void _IndexPlacementDependencies(Snapshot *snapshot)
{
    snapshot->placementDependencies.clear();
    snapshot->placementIds.clear();
    snapshot->instanceSourcePlacements.clear();
    snapshot->instancerInputs.clear();
    snapshot->prototypeDependencies.clear();
    for (auto const &[id, primId] : snapshot->geomToPrimId)
        _IndexPlacementDependency(snapshot, id, true);
}

size_t _RetainedGeometryBytes(Geometry const *geometry)
{
    if (auto const *mesh = dynamic_cast<Mesh const *>(geometry))
        return mesh->vertices.size() * sizeof(Float3) +
            mesh->normals.size() * sizeof(Float3) +
            mesh->texcoords.size() * sizeof(Float2) +
            mesh->indices.size() * sizeof(unsigned int) +
            mesh->tangents.size() * sizeof(float);
    return dynamic_cast<Sphere const *>(geometry) ? 4 * sizeof(float) : 0;
}

PrototypePool::ResidencyManifest _ResidencyManifest(Snapshot const &snapshot)
{
    PrototypePool::ResidencyManifest result;
    for (auto const &[path, prototype] : snapshot.prototypes) {
        auto &entry = result[prototype.storage.get()];
        entry.storage = prototype.storage;
        entry.bytes = _RetainedGeometryBytes(prototype.geometry);
        entry.paths.push_back(path);
        entry.enabled |= snapshot.enabledPrototypePlacements.contains(path);
    }
    return result;
}

struct HiddenGeometryUsage {
    uint64_t residentBytes = 0;
    uint64_t pinnedBytes = 0;
};

std::unordered_set<PrototypeStorage *> _EnabledPrototypeStorage(Snapshot const &snapshot)
{
    std::unordered_set<PrototypeStorage *> result;
    for (auto const &[path, prototype] : snapshot.prototypes)
        if (snapshot.enabledPrototypePlacements.contains(path))
            result.insert(prototype.storage.get());
    return result;
}

void _SetCurrentResidency(Snapshot const &snapshot)
{
    auto enabled = _EnabledPrototypeStorage(snapshot);
    std::lock_guard lock(snapshot.prototypePool->mutex);
    snapshot.prototypePool->currentEnabledStorage = std::move(enabled);
    ++snapshot.prototypePool->residencyClock;
}

// Caller holds the pool mutex. Weak entries do not extend any geometry lifetime.
PrototypePool::ResidencyManifest _CombinedResidency(PrototypePool const &pool,
    std::unordered_set<PrototypeStorage *> const &enabledStorage)
{
    PrototypePool::ResidencyManifest result;
    for (auto const &[owner, manifest] : pool.residencyManifests)
        for (auto const &[storage, entry] : manifest) {
            if (entry.storage.expired()) continue;
            auto &combined = result[storage];
            combined.storage = entry.storage;
            combined.bytes = entry.bytes;
            combined.enabled = enabledStorage.contains(storage);
            combined.paths.insert(combined.paths.end(), entry.paths.begin(), entry.paths.end());
        }
    return result;
}

HiddenGeometryUsage _HiddenGeometryUsage(PrototypePool const &pool)
{
    const uint64_t revision = pool.residencyClock.load();
    if (pool.usageRevision == revision)
        return {pool.cachedHiddenResidentBytes, pool.cachedHiddenPinnedBytes};
    HiddenGeometryUsage usage;
    for (auto const &[storage, entry] : _CombinedResidency(pool, pool.currentEnabledStorage)) {
        if (entry.enabled) continue;
        usage.residentBytes += entry.bytes;
        auto selected = pool.selectedEvictions.find(storage);
        if (selected != pool.selectedEvictions.end() && !selected->second.expired())
            usage.pinnedBytes += entry.bytes;
    }
    pool.cachedHiddenResidentBytes = usage.residentBytes;
    pool.cachedHiddenPinnedBytes = usage.pinnedBytes;
    pool.usageRevision = revision;
    return usage;
}

// Only call on an unpublished bank. Detachment releases Embree's instance
// ownership before removing the last C++ aliases to a prototype scene.
bool _ApplyHiddenGeometryBudget(Snapshot *snapshot, bool useCurrentEligibility = false)
{
    if (!snapshot || !snapshot->scene) return false;
    auto manifest = _ResidencyManifest(*snapshot);
    auto enabledStorage = _EnabledPrototypeStorage(*snapshot);
    PrototypePool &pool = *snapshot->prototypePool;
    std::unordered_set<PrototypeStorage *> evictions;
    std::vector<std::shared_ptr<PrototypeStorage>> evictionOwners;
    {
        std::lock_guard lock(pool.mutex);
        if (useCurrentEligibility) enabledStorage = pool.currentEnabledStorage;
        const uint64_t tick = ++pool.residencyClock;
        pool.residencyManifests[snapshot] = manifest;
        auto combined = _CombinedResidency(pool, enabledStorage);
        for (auto const &[storage, entry] : manifest)
            if (combined.at(storage).enabled)
                storage->lastEnabled.store(tick, std::memory_order_relaxed);
        const uint64_t budget = pool.hiddenGeometryBudgetBytes.load();
        if (!budget) return false;
        uint64_t hiddenBytes = 0;
        for (auto const &[storage, entry] : combined)
            if (!entry.enabled) hiddenBytes += entry.bytes;
        struct Candidate {
            PrototypeStorage *storage;
            size_t bytes;
            uint64_t lastEnabled;
            SdfPath path;
            bool alreadyEvicted;
        };
        std::vector<Candidate> candidates;
        for (auto const &[storage, entry] : manifest) {
            if (combined.at(storage).enabled) continue;
            auto const path = *std::min_element(entry.paths.begin(), entry.paths.end());
            const bool alreadyEvicted = std::any_of(entry.paths.begin(), entry.paths.end(),
                [&](SdfPath const &p) { return pool.evictedPaths.contains(p); });
            candidates.push_back({storage, entry.bytes,
                storage->lastEnabled.load(std::memory_order_relaxed), path, alreadyEvicted});
        }
        std::sort(candidates.begin(), candidates.end(), [](auto const &a, auto const &b) {
            if (a.alreadyEvicted != b.alreadyEvicted) return a.alreadyEvicted;
            return a.lastEnabled != b.lastEnabled ? a.lastEnabled < b.lastEnabled : a.path < b.path;
        });
        for (auto const &candidate : candidates) {
            if (hiddenBytes <= budget && !candidate.alreadyEvicted) continue;
            evictions.insert(candidate.storage);
            evictionOwners.push_back(manifest.at(candidate.storage).storage.lock());
            pool.selectedEvictions[candidate.storage] = manifest.at(candidate.storage).storage;
            hiddenBytes -= std::min<uint64_t>(hiddenBytes, candidate.bytes);
            for (auto const &path : manifest.at(candidate.storage).paths)
                pool.evictedPaths.insert(path);
        }
    }
    if (evictions.empty()) return false;
    std::unordered_set<SdfPath, SdfPath::Hash> paths;
    for (auto *storage : evictions)
        for (auto const &path : manifest.at(storage).paths) paths.insert(path);
    std::vector<unsigned int> detach;
    for (auto const &[id, primId] : snapshot->geomToPrimId)
        if (primId >= 0 && static_cast<size_t>(primId) < snapshot->primPaths.size() &&
            paths.contains(snapshot->primPaths[primId])) detach.push_back(id);
    for (unsigned int id : detach) {
        rtcDetachGeometry(snapshot->scene->scene, id);
        if (auto enabled = snapshot->placementEnabled.find(id);
            enabled != snapshot->placementEnabled.end() && enabled->second) {
            --snapshot->enabledPlacementCount;
            _TrackPrototypeEnabled(snapshot, id, false);
        }
        snapshot->scene->geometry.erase(id);
        snapshot->scene->intersection_geometry.erase(id);
        snapshot->scene->placement_materials.erase(id);
        snapshot->scene->placement_transforms.erase(id);
        auto instance = snapshot->geomToInstanceId.find(id);
        if (instance != snapshot->geomToInstanceId.end() && instance->second >= 0)
            --snapshot->instanceCount;
        snapshot->geomToPrimId.erase(id);
        snapshot->geomToCoarseFaces.erase(id);
        snapshot->geomToInstanceId.erase(id);
        snapshot->geomToInstanceChain.erase(id);
        snapshot->placementEnabled.erase(id);
        snapshot->placementMatrices.erase(id);
    }
    for (SdfPath const &path : paths) {
        auto prototype = snapshot->prototypes.find(path);
        auto bucket = snapshot->prototypeHashBuckets.find(prototype->second.geometryHash);
        if (bucket != snapshot->prototypeHashBuckets.end()) {
            std::erase(bucket->second, path);
            if (bucket->second.empty()) snapshot->prototypeHashBuckets.erase(bucket);
        }
        snapshot->prototypes.erase(prototype);
        snapshot->evictedPrototypes.insert(path);
        snapshot->displayMaterialsByPath.erase(path);
    }
    for (auto *storage : evictions) {
        Geometry const *const retainedGeometry = storage->geometry.get();
        snapshot->estimatedGeometryBytes -= std::min(snapshot->estimatedGeometryBytes,
            manifest.at(storage).bytes);
        std::erase_if(snapshot->geometries, [retainedGeometry](auto const &geometry) {
            return geometry.get() == retainedGeometry;
        });
    }
    pool.hiddenGeometryEvictions.fetch_add(paths.size());
    _IndexPlacementDependencies(snapshot);
    _RefreshCachedCounts(snapshot);
    {
        std::lock_guard lock(pool.mutex);
        std::erase_if(pool.conversions, [&](auto const &entry) {
            auto storage = entry.second.storage.lock();
            return !storage || evictions.contains(storage.get());
        });
        std::erase_if(pool.selectedEvictions, [](auto const &entry) {
            return entry.second.expired();
        });
        pool.residencyManifests[snapshot] = _ResidencyManifest(*snapshot);
    }
    return !detach.empty();
}

// A named primvar of a mesh for materials (geompropvalue), as floats.
struct _ExtraPrimvar {
    std::string name;
    int components = 0;
    TfToken interpolation;
    std::vector<float> values;
};

bool _PrimvarFloats(VtValue const &value, std::vector<float> *out, int *components)
{
    auto take = [&](auto const &array, int n) {
        out->clear();
        out->reserve(array.size() * size_t(n));
        for (auto const &element : array) {
            if constexpr (std::is_arithmetic_v<std::decay_t<decltype(element)>>) {
                out->push_back(float(element));
            } else {
                for (int c = 0; c < n; ++c) out->push_back(float(element[c]));
            }
        }
        *components = n;
        return !out->empty();
    };
    if (value.IsHolding<VtFloatArray>()) return take(value.UncheckedGet<VtFloatArray>(), 1);
    if (value.IsHolding<VtDoubleArray>()) return take(value.UncheckedGet<VtDoubleArray>(), 1);
    if (value.IsHolding<VtIntArray>()) return take(value.UncheckedGet<VtIntArray>(), 1);
    if (value.IsHolding<VtVec2fArray>()) return take(value.UncheckedGet<VtVec2fArray>(), 2);
    if (value.IsHolding<VtVec2dArray>()) return take(value.UncheckedGet<VtVec2dArray>(), 2);
    if (value.IsHolding<VtVec3fArray>()) return take(value.UncheckedGet<VtVec3fArray>(), 3);
    if (value.IsHolding<VtVec3dArray>()) return take(value.UncheckedGet<VtVec3dArray>(), 3);
    if (value.IsHolding<VtVec4fArray>()) return take(value.UncheckedGet<VtVec4fArray>(), 4);
    if (value.IsHolding<VtVec4dArray>()) return take(value.UncheckedGet<VtVec4dArray>(), 4);
    if (value.IsHolding<float>()) return take(std::array<float, 1>{value.UncheckedGet<float>()}, 1);
    if (value.IsHolding<double>()) return take(std::array<double, 1>{value.UncheckedGet<double>()}, 1);
    if (value.IsHolding<int>()) return take(std::array<int, 1>{value.UncheckedGet<int>()}, 1);
    if (value.IsHolding<GfVec2f>()) return take(std::array<GfVec2f, 1>{value.UncheckedGet<GfVec2f>()}, 2);
    if (value.IsHolding<GfVec3f>()) return take(std::array<GfVec3f, 1>{value.UncheckedGet<GfVec3f>()}, 3);
    if (value.IsHolding<GfVec4f>()) return take(std::array<GfVec4f, 1>{value.UncheckedGet<GfVec4f>()}, 4);
    return false;
}

// The primvars of a mesh other than its geometry (points, normals, texture
// coordinates), whose size fits their interpolation.
std::vector<_ExtraPrimvar> _ExtraPrimvars(HdPrimvarsSchema const &primvars, size_t points, size_t faces,
                                          size_t corners)
{
    static const std::unordered_set<std::string> skipped = {
        "points", "normals", "widths", "velocities", "accelerations", "angularVelocities"};
    constexpr int kMaxComponents = 64;
    std::vector<_ExtraPrimvar> result;
    int total = 0;
    TfTokenVector names = primvars.GetPrimvarNames();
    std::sort(names.begin(), names.end());
    for (TfToken const &name : names) {
        const std::string text = name.GetString();
        if (skipped.count(text) || text.find(':') != std::string::npos) continue;
        PrimvarData data = _Primvar(primvars, name);
        // Constant display colors drive the fallback material, so their edits
        // stay material edits.
        if ((text == "displayColor" || text == "displayOpacity") &&
            data.interpolation == HdPrimvarSchemaTokens->constant) continue;
        _ExtraPrimvar extra;
        extra.name = text;
        extra.interpolation = data.interpolation;
        if (!_PrimvarFloats(data.value, &extra.values, &extra.components)) continue;
        const size_t count = extra.values.size() / size_t(extra.components);
        size_t expected = 0;
        if (extra.interpolation == HdPrimvarSchemaTokens->constant) expected = 1;
        else if (extra.interpolation == HdPrimvarSchemaTokens->uniform) expected = faces;
        else if (extra.interpolation == HdPrimvarSchemaTokens->faceVarying) expected = corners;
        else expected = points; // vertex, varying
        if (extra.interpolation == HdPrimvarSchemaTokens->constant ? count < 1 : count != expected) continue;
        if (total + extra.components > kMaxComponents) break;
        total += extra.components;
        result.push_back(std::move(extra));
    }
    return result;
}

// Uniform subdivision of subdivision surfaces: the highest level, and the
// faces one mesh may refine to (meshes denser than that use lower levels, and
// their control hulls beyond it). PRISM_SUBDIVISION_LEVEL (0 renders control
// hulls) and PRISM_SUBDIVISION_MAX_FACES override them.
struct _SubdivisionSettings {
    int level = 2;
    size_t maxFaces = size_t(1) << 16;
    // Meshes whose material displaces them (PRISM_DISPLACEMENT_LEVEL,
    // PRISM_DISPLACEMENT_MAX_FACES).
    int displacementLevel = 4;
    size_t displacementMaxFaces = size_t(1) << 18;
    // Faces all refined prototypes of one scene build may add up to
    // (PRISM_SUBDIVISION_BUDGET_FACES). Beyond it the meshes that refine to
    // the most faces drop levels first. Each face costs a few hundred bytes
    // once triangulated, with normals, UVs, tangents and its Embree BVH.
    size_t budgetFaces = size_t(24) << 20;
};

_SubdivisionSettings const &_Subdivision()
{
    static const _SubdivisionSettings settings = [] {
        _SubdivisionSettings result;
        if (const char *value = std::getenv("PRISM_SUBDIVISION_LEVEL"); value && *value)
            result.level = std::clamp(std::atoi(value), 0, 6);
        if (const char *value = std::getenv("PRISM_SUBDIVISION_MAX_FACES"); value && *value)
            result.maxFaces = size_t(std::max(1ll, std::atoll(value)));
        if (const char *value = std::getenv("PRISM_DISPLACEMENT_LEVEL"); value && *value)
            result.displacementLevel = std::clamp(std::atoi(value), 0, 6);
        if (const char *value = std::getenv("PRISM_DISPLACEMENT_MAX_FACES"); value && *value)
            result.displacementMaxFaces = size_t(std::max(1ll, std::atoll(value)));
        if (const char *value = std::getenv("PRISM_SUBDIVISION_BUDGET_FACES"); value && *value)
            result.budgetFaces = size_t(std::max(1ll, std::atoll(value)));
        return result;
    }();
    return settings;
}

// Moves the vertices of a converted mesh by its material's displacement.
// Vertices at one position (seams of normals or texture coordinates) move
// together, so the surface stays closed; normals and tangents are rebuilt.
void _Displace(Mesh &mesh, Material const &material, bool flip)
{
    const size_t count = mesh.vertices.size();
    if (!count) return;
    // A triangle and corner per vertex, for primvar lookups at the vertex.
    std::vector<std::pair<unsigned, int>> corner(count, {0u, -1});
    for (size_t t = 0; t * 3 + 2 < mesh.indices.size(); ++t)
        for (int k = 0; k < 3; ++k)
            if (corner[mesh.indices[3 * t + k]].second < 0) corner[mesh.indices[3 * t + k]] = {unsigned(t), k};
    std::vector<Float3> offsets(count, Float3(0.0f));
    tbb::parallel_for(size_t(0), count, [&](size_t i) {
        RTCRayHit miss{};
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit hit(miss);
        hit.p = mesh.vertices[i];
        hit.n = hit.n_g = mesh.normals[i].is_zero() ? Float3(0.0f, 0.0f, 1.0f) : mesh.normals[i].normalize();
        hit.o = hit.p + hit.n;
        hit.d = -hit.n;
        hit.front_face = true;
        hit.u = mesh.texcoords.size() == count ? mesh.texcoords[i].u() : 0.0f;
        hit.v = mesh.texcoords.size() == count ? mesh.texcoords[i].v() : 0.0f;
        if (mesh.tangents.size() == 4 * count) {
            hit.tangent = Float3(mesh.tangents[4 * i], mesh.tangents[4 * i + 1], mesh.tangents[4 * i + 2]);
            hit.bitangent_sign = mesh.tangents[4 * i + 3] < 0.0f ? -1.0f : 1.0f;
        }
        if (corner[i].second >= 0) {
            hit.primvarMesh = &mesh;
            hit.primid = int(corner[i].first);
            hit.bary_u = corner[i].second == 1 ? 1.0f : 0.0f;
            hit.bary_v = corner[i].second == 2 ? 1.0f : 0.0f;
        }
        offsets[i] = material.displacement(hit);
    });
    struct PositionHash {
        size_t operator()(std::array<uint32_t, 3> const &p) const {
            return (size_t(p[0]) * 73856093u) ^ (size_t(p[1]) * 19349663u) ^ (size_t(p[2]) * 83492791u);
        }
    };
    std::unordered_map<std::array<uint32_t, 3>, unsigned, PositionHash> groupOf;
    std::vector<unsigned> group(count);
    std::vector<Float3> sum;
    std::vector<int> members;
    for (size_t i = 0; i < count; ++i) {
        const Float3 &p = mesh.vertices[i];
        const std::array<uint32_t, 3> key{std::bit_cast<uint32_t>(p.x()), std::bit_cast<uint32_t>(p.y()),
                                         std::bit_cast<uint32_t>(p.z())};
        auto [found, inserted] = groupOf.emplace(key, unsigned(sum.size()));
        if (inserted) { sum.push_back(Float3(0.0f)); members.push_back(0); }
        group[i] = found->second;
        sum[found->second] += offsets[i];
        ++members[found->second];
    }
    for (size_t i = 0; i < count; ++i) mesh.vertices[i] += sum[group[i]] / float(members[group[i]]);
    // Smooth area-weighted normals per position (the winding faces outward
    // after the conversion's orientation handling).
    std::vector<Float3> normals(sum.size(), Float3(0.0f));
    for (size_t t = 0; t * 3 + 2 < mesh.indices.size(); ++t) {
        const unsigned *tri = &mesh.indices[3 * t];
        const Float3 n = cross(mesh.vertices[tri[1]] - mesh.vertices[tri[0]],
                               mesh.vertices[tri[2]] - mesh.vertices[tri[0]]);
        for (int k = 0; k < 3; ++k) normals[group[tri[k]]] += n;
    }
    (void)flip;
    for (size_t i = 0; i < count; ++i) {
        const Float3 n = normals[group[i]];
        if (n.length() > 0.0f) {
            Float3 unit = n.normalize();
            // Keep the side of the original normal.
            if (dot(unit, mesh.normals[i]) < 0.0f) unit = -unit;
            mesh.normals[i] = unit;
        }
    }
    mesh.tangents.clear();
    mesh.ensureTangents();
}

// The faces a mesh refines to at each subdivision level up to the one its
// conversion would use (index = level); a single entry, its own face count,
// for meshes that do not refine. Reads only the topology.
std::vector<size_t> _RefinementFaces(HdContainerDataSourceHandle const &dataSource,
                                     bool displaced)
{
    HdMeshSchema meshSchema = HdMeshSchema::GetFromParent(dataSource);
    HdMeshTopologySchema topology = meshSchema.GetTopology();
    VtIntArray counts;
    if (!meshSchema || !topology || !_ReadArray(topology.GetFaceVertexCounts(), &counts))
        return {};
    std::vector<size_t> faces{counts.size()};
    TfToken scheme("none");
    if (auto authored = meshSchema.GetSubdivisionScheme()) {
        TfToken const value = authored->GetTypedValue(0.0f);
        if (!value.IsEmpty()) scheme = value;
    }
    if (displaced && scheme == TfToken("none")) scheme = TfToken("bilinear");
    if (scheme == TfToken("none")) return faces;
    _SubdivisionSettings const &subdivision = _Subdivision();
    const auto kind = scheme == TfToken("loop") ? prism::subdivision::Scheme::Loop
        : scheme == TfToken("bilinear") ? prism::subdivision::Scheme::Bilinear
                                        : prism::subdivision::Scheme::CatmullClark;
    const int top = displaced ? subdivision.displacementLevel : subdivision.level;
    const size_t maxFaces = displaced ? subdivision.displacementMaxFaces : subdivision.maxFaces;
    std::span<const int> countSpan(counts.cdata(), counts.size());
    for (int level = 1; level <= top; ++level) {
        const size_t refined = prism::subdivision::RefinedFaceCount(countSpan, kind, level);
        if (refined > maxFaces) break;
        faces.push_back(refined);
    }
    return faces;
}

// Highest subdivision level for each mesh such that all of them together
// stay within the scene's refinement budget. Meshes are lowered one level
// at a time, always the one currently refining to the most faces, so dense
// meshes (whose control hulls are already fine) give up refinement first.
// Fixed entries (already converted prototypes) count but are not lowered.
std::vector<int> _BudgetSubdivisionLevels(
    std::vector<std::vector<size_t>> const &faces, std::vector<bool> const &fixed,
    size_t budget, size_t *plannedFaces, size_t *requestedFaces)
{
    std::vector<int> levels(faces.size(), 0);
    size_t total = 0;
    std::priority_queue<std::pair<size_t, size_t>> largest;
    for (size_t i = 0; i < faces.size(); ++i) {
        if (faces[i].empty()) continue;
        levels[i] = int(faces[i].size()) - 1;
        total += faces[i].back();
        if (!fixed[i] && levels[i] > 0) largest.emplace(faces[i].back(), i);
    }
    if (requestedFaces) *requestedFaces = total;
    while (total > budget && !largest.empty()) {
        const size_t i = largest.top().second;
        largest.pop();
        total -= faces[i][levels[i]];
        --levels[i];
        total += faces[i][levels[i]];
        if (levels[i] > 0) largest.emplace(faces[i][levels[i]], i);
    }
    if (plannedFaces) *plannedFaces = total;
    return levels;
}

// Pure: reads only the prim's data source and touches no Snapshot, pool,
// Embree or material state, so snapshot builds run it on TBB workers. The
// caller emits the warnings recorded in MeshConversionInfo. levelCap, when
// not negative, limits the subdivision level (see _BudgetSubdivisionLevels).
MeshConversion _ConvertMesh(HdContainerDataSourceHandle const &dataSource,
                            Material const *displacer = nullptr, int levelCap = -1)
{
    MeshConversion result;
    MeshConversionInfo &info = result.info;
    HdMeshSchema meshSchema = HdMeshSchema::GetFromParent(dataSource);
    HdMeshTopologySchema topology = meshSchema.GetTopology();
    HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(dataSource);
    if (!meshSchema || !topology || !primvars) return result;

    HdXformSchema const xform = HdXformSchema::GetFromParent(dataSource);
    HdPrimvarSchema const pointsPrimvar =
        primvars.GetPrimvar(HdPrimvarsSchemaTokens->points);
    info.motionBlur = (xform && _IsSampled(xform.GetMatrix())) ||
        (pointsPrimvar && _IsSampled(pointsPrimvar.GetPrimvarValue()));

    VtIntArray readCounts, readIndices, readHoles;
    if (!_ReadArray(topology.GetFaceVertexCounts(), &readCounts) ||
        !_ReadArray(topology.GetFaceVertexIndices(), &readIndices)) return result;
    _ReadArray(topology.GetHoleIndices(), &readHoles);
    PrimvarData pointsPv = _Primvar(primvars, HdPrimvarsSchemaTokens->points);
    VtVec3fArray const *pointsFloat = nullptr;
    VtVec3dArray const *pointsDouble = nullptr;
    if (pointsPv.value.IsHolding<VtVec3fArray>()) {
        pointsFloat = &pointsPv.value.UncheckedGet<VtVec3fArray>();
    } else if (pointsPv.value.IsHolding<VtVec3dArray>()) {
        pointsDouble = &pointsPv.value.UncheckedGet<VtVec3dArray>();
    } else {
        info.missingPoints = pointsPv.value.IsEmpty();
        info.unsupportedPoints = !pointsPv.value.IsEmpty();
        return result;
    }
    size_t pointCount = pointsFloat ? pointsFloat->size() : pointsDouble->size();
    auto pointAt = [&](size_t i) -> GfVec3d {
        return pointsFloat ? GfVec3d((*pointsFloat)[i])
                           : GfVec3d(GfVec3f((*pointsDouble)[i]));
    };
    // Const views: indexing a non-const VtArray detaches (copies) the buffer
    // it still shares with the captured data source.
    VtIntArray const coarseCounts = std::move(readCounts);
    VtIntArray const coarseIndices = std::move(readIndices);
    VtIntArray const coarseHoles = std::move(readHoles);
    if (pointCount == 0) {
        info.missingPoints = true;
        return result;
    }
    info.topologyRead = true;
    info.faceCount = coarseCounts.size();

    PrimvarData normalPv = _Primvar(primvars, HdPrimvarsSchemaTokens->normals);
    PrimvarData uvPv = _Primvar(primvars, TfToken("st"));
    PrimvarData const alternateUvPv = _Primvar(primvars, TfToken("uv"));
    info.multipleUvSets = !uvPv.value.IsEmpty() && !alternateUvPv.value.IsEmpty();
    if (uvPv.value.IsEmpty()) uvPv = alternateUvPv;
    std::vector<_ExtraPrimvar> extras = _ExtraPrimvars(primvars, pointCount, coarseCounts.size(),
                                                       coarseIndices.size());
    int extraStride = 0;
    int pointDataStride = 0;
    std::vector<int> extraOffsets, extraPointOffsets;
    for (auto const &extra : extras) {
        extraOffsets.push_back(extraStride);
        extraStride += extra.components;
        const bool perPoint = extra.interpolation == HdPrimvarSchemaTokens->vertex ||
            extra.interpolation == HdPrimvarSchemaTokens->varying;
        extraPointOffsets.push_back(perPoint ? pointDataStride : -1);
        if (perPoint) pointDataStride += extra.components;
    }

    // Subdivision surfaces become their limit surface at a uniform level;
    // their normals come from the limit (authored ones do not apply).
    // UsdImaging always provides the scheme (USD's fallback is catmullClark);
    // scene indices that omit it describe polygon meshes.
    TfToken scheme("none");
    if (auto authored = meshSchema.GetSubdivisionScheme()) {
        TfToken const value = authored->GetTypedValue(0.0f);
        if (!value.IsEmpty()) scheme = value;
    }
    prism::subdivision::Output refined;
    VtIntArray refinedCounts, refinedIndices;
    VtVec3fArray refinedPoints;
    // Displaced surfaces are diced finer; polygon meshes then refine bilinearly.
    const bool displaced = displacer && displacer->hasDisplacement();
    if (displaced && scheme == TfToken("none")) scheme = TfToken("bilinear");
    if (scheme != TfToken("none")) {
        _SubdivisionSettings const &subdivision = _Subdivision();
        prism::subdivision::Input input;
        input.scheme = scheme == TfToken("loop") ? prism::subdivision::Scheme::Loop
            : scheme == TfToken("bilinear") ? prism::subdivision::Scheme::Bilinear
                                            : prism::subdivision::Scheme::CatmullClark;
        input.level = displaced ? subdivision.displacementLevel : subdivision.level;
        const size_t maxFaces = displaced ? subdivision.displacementMaxFaces : subdivision.maxFaces;
        std::span<const int> countSpan(coarseCounts.cdata(), coarseCounts.size());
        while (input.level > 0 &&
               prism::subdivision::RefinedFaceCount(countSpan, input.scheme, input.level) > maxFaces)
            --input.level;
        if (levelCap >= 0) input.level = std::min(input.level, levelCap);
        info.subdivisionLevel = input.level;
        if (input.level > 0) {
            std::vector<float> points(pointCount * 3);
            for (size_t i = 0; i < pointCount; ++i) {
                const GfVec3d p = pointAt(i);
                points[3 * i] = float(p[0]); points[3 * i + 1] = float(p[1]); points[3 * i + 2] = float(p[2]);
            }
            input.points = points;
            input.faceVertexCounts = countSpan;
            input.faceVertexIndices = std::span<const int>(coarseIndices.cdata(), coarseIndices.size());
            input.holes = std::span<const int>(coarseHoles.cdata(), coarseHoles.size());
            HdSubdivisionTagsSchema const tags = meshSchema.GetSubdivisionTags();
            VtIntArray creaseIndices, creaseLengths, cornerIndices;
            VtFloatArray creaseSharpnesses, cornerSharpnesses;
            if (tags) {
                _ReadArray(tags.GetCreaseIndices(), &creaseIndices);
                _ReadArray(tags.GetCreaseLengths(), &creaseLengths);
                _ReadArray(tags.GetCreaseSharpnesses(), &creaseSharpnesses);
                _ReadArray(tags.GetCornerIndices(), &cornerIndices);
                _ReadArray(tags.GetCornerSharpnesses(), &cornerSharpnesses);
                if (auto boundary = tags.GetInterpolateBoundary()) {
                    TfToken const value = boundary->GetTypedValue(0.0f);
                    if (value == TfToken("none")) input.boundary = prism::subdivision::BoundaryInterpolation::None;
                    else if (value == TfToken("edgeOnly"))
                        input.boundary = prism::subdivision::BoundaryInterpolation::EdgeOnly;
                }
                if (auto rule = tags.GetFaceVaryingLinearInterpolation()) {
                    using F = prism::subdivision::FaceVaryingInterpolation;
                    TfToken const value = rule->GetTypedValue(0.0f);
                    if (value == TfToken("none")) input.faceVarying = F::None;
                    else if (value == TfToken("cornersOnly")) input.faceVarying = F::CornersOnly;
                    else if (value == TfToken("cornersPlus2")) input.faceVarying = F::CornersPlus2;
                    else if (value == TfToken("boundaries")) input.faceVarying = F::Boundaries;
                    else if (value == TfToken("all")) input.faceVarying = F::All;
                }
                if (auto rule = tags.GetTriangleSubdivisionRule())
                    input.smoothTriangles = rule->GetTypedValue(0.0f) == TfToken("smooth");
            }
            input.creaseIndices = std::span<const int>(creaseIndices.cdata(), creaseIndices.size());
            input.creaseLengths = std::span<const int>(creaseLengths.cdata(), creaseLengths.size());
            input.creaseSharpnesses = std::span<const float>(creaseSharpnesses.cdata(), creaseSharpnesses.size());
            input.cornerIndices = std::span<const int>(cornerIndices.cdata(), cornerIndices.size());
            input.cornerSharpnesses = std::span<const float>(cornerSharpnesses.cdata(), cornerSharpnesses.size());
            std::vector<float> uvs;
            const bool faceVaryingUvs = uvPv.interpolation == HdPrimvarSchemaTokens->faceVarying;
            const bool vertexUvs = uvPv.interpolation == HdPrimvarSchemaTokens->vertex ||
                uvPv.interpolation == HdPrimvarSchemaTokens->varying;
            auto appendUvs = [&](auto const &values, size_t expected) {
                if (values.size() != expected) return;
                uvs.reserve(expected * 2);
                for (auto const &uv : values) {
                    uvs.push_back(float(uv[0]));
                    uvs.push_back(float(uv[1]));
                }
            };
            if (faceVaryingUvs || vertexUvs) {
                const size_t expected = faceVaryingUvs ? coarseIndices.size() : pointCount;
                if (uvPv.value.IsHolding<VtVec2fArray>()) appendUvs(uvPv.value.UncheckedGet<VtVec2fArray>(), expected);
                else if (uvPv.value.IsHolding<VtVec2dArray>())
                    appendUvs(uvPv.value.UncheckedGet<VtVec2dArray>(), expected);
            }
            input.uvs = uvs;
            input.uvPerPoint = vertexUvs;
            std::vector<float> pointData;
            if (pointDataStride) {
                pointData.resize(pointCount * size_t(pointDataStride));
                for (size_t e = 0; e < extras.size(); ++e) {
                    if (extraPointOffsets[e] < 0) continue;
                    const int n = extras[e].components;
                    for (size_t p = 0; p < pointCount; ++p)
                        for (int c = 0; c < n; ++c)
                            pointData[p * pointDataStride + extraPointOffsets[e] + c] = extras[e].values[p * n + c];
                }
                input.pointData = pointData;
                input.pointDataStride = pointDataStride;
            }
            refined = prism::subdivision::Refine(input);
        }
        if (refined.valid) {
            refinedCounts.assign(refined.faceVertexCounts.begin(), refined.faceVertexCounts.end());
            refinedIndices.assign(refined.faceVertexIndices.begin(), refined.faceVertexIndices.end());
            refinedPoints.resize(refined.points.size() / 3);
            for (size_t i = 0; i < refinedPoints.size(); ++i)
                refinedPoints[i] = GfVec3f(refined.points[3 * i], refined.points[3 * i + 1], refined.points[3 * i + 2]);
            pointsFloat = &refinedPoints;
            pointsDouble = nullptr;
            pointCount = refinedPoints.size();
            normalPv = PrimvarData();
            if (!refined.uvs.empty()) {
                VtVec2fArray refinedUvs(refined.uvs.size() / 2);
                for (size_t i = 0; i < refinedUvs.size(); ++i)
                    refinedUvs[i] = GfVec2f(refined.uvs[2 * i], refined.uvs[2 * i + 1]);
                uvPv.value = VtValue(std::move(refinedUvs));
                uvPv.interpolation = HdPrimvarSchemaTokens->faceVarying;
            } else if (uvPv.interpolation != HdPrimvarSchemaTokens->constant) {
                uvPv = PrimvarData();
            }
        } else {
            info.subdivisionControlHull = true;
        }
    }
    VtIntArray const &counts = refined.valid ? refinedCounts : coarseCounts;
    VtIntArray const &indices = refined.valid ? refinedIndices : coarseIndices;
    static const VtIntArray noHoles;
    VtIntArray const &holes = refined.valid ? noHoles : coarseHoles;
    // The authored face of a (refined) face.
    auto coarseFace = [&](size_t face) {
        return refined.valid ? size_t(refined.coarseFaces[face]) : face;
    };
    // Face-varying primvars of refined meshes: the mean of each authored face.
    std::vector<std::vector<float>> faceMeans(extras.size());
    if (refined.valid) {
        for (size_t e = 0; e < extras.size(); ++e) {
            if (extras[e].interpolation != HdPrimvarSchemaTokens->faceVarying) continue;
            const int n = extras[e].components;
            faceMeans[e].assign(coarseCounts.size() * size_t(n), 0.0f);
            size_t corner = 0;
            for (size_t f = 0; f < coarseCounts.size(); ++f) {
                const int count = std::max(0, coarseCounts[f]);
                for (int k = 0; k < count && corner + k < coarseIndices.size(); ++k)
                    for (int c = 0; c < n; ++c)
                        faceMeans[e][f * n + c] += extras[e].values[(corner + k) * n + c] / float(std::max(1, count));
                corner += size_t(count);
            }
        }
    }
    // Values of every extra primvar at one corner of a (refined) face.
    auto appendExtras = [&](std::vector<float> &out, size_t point, size_t cornerIndex, size_t face) {
        for (size_t e = 0; e < extras.size(); ++e) {
            auto const &extra = extras[e];
            const int n = extra.components;
            const float *source = nullptr;
            if (extraPointOffsets[e] >= 0) {
                source = refined.valid
                    ? refined.pointData.data() + point * pointDataStride + extraPointOffsets[e]
                    : extra.values.data() + point * n;
            } else if (extra.interpolation == HdPrimvarSchemaTokens->faceVarying) {
                source = refined.valid ? faceMeans[e].data() + coarseFace(face) * n
                                       : extra.values.data() + cornerIndex * n;
            } else if (extra.interpolation == HdPrimvarSchemaTokens->uniform) {
                source = extra.values.data() + coarseFace(face) * n;
            } else {
                source = extra.values.data();
            }
            out.insert(out.end(), source, source + n);
        }
    };
    VtVec3fArray const *authoredNormalsFloat = nullptr;
    VtVec3dArray const *authoredNormalsDouble = nullptr;
    if (normalPv.value.IsHolding<VtVec3fArray>()) {
        authoredNormalsFloat = &normalPv.value.UncheckedGet<VtVec3fArray>();
    } else if (normalPv.value.IsHolding<VtVec3dArray>()) {
        authoredNormalsDouble = &normalPv.value.UncheckedGet<VtVec3dArray>();
    }
    const size_t authoredNormalCount = authoredNormalsFloat
        ? authoredNormalsFloat->size() : (authoredNormalsDouble ? authoredNormalsDouble->size() : 0);
    auto authoredNormalAt = [&](size_t i) -> GfVec3d {
        return authoredNormalsFloat ? GfVec3d((*authoredNormalsFloat)[i])
            : GfVec3d(GfVec3f((*authoredNormalsDouble)[i]));
    };
    VtVec2fArray const *authoredUvsFloat = nullptr;
    VtVec2dArray const *authoredUvsDouble = nullptr;
    if (uvPv.value.IsHolding<VtVec2fArray>()) {
        authoredUvsFloat = &uvPv.value.UncheckedGet<VtVec2fArray>();
    } else if (uvPv.value.IsHolding<VtVec2dArray>()) {
        authoredUvsDouble = &uvPv.value.UncheckedGet<VtVec2dArray>();
    }
    const size_t authoredUvCount = authoredUvsFloat
        ? authoredUvsFloat->size() : (authoredUvsDouble ? authoredUvsDouble->size() : 0);

    TfToken orientation = HdMeshTopologySchemaTokens->rightHanded;
    if (auto o = topology.GetOrientation()) orientation = o->GetTypedValue(0.0f);
    const bool flip = orientation == HdMeshTopologySchemaTokens->leftHanded;
    const GfMatrix4d geometryTransform(1.0);
    const bool mirrored = geometryTransform.GetDeterminant() < 0.0;
    const GfMatrix4d normalMatrix = geometryTransform.GetInverse().GetTranspose();
    std::unordered_set<int> holeSet(holes.begin(), holes.end());

    // Match the standalone USD path when normals are missing, zero, or only
    // partially authored: accumulate one area-weighted coarse-face normal at
    // each referenced point. Keep this in prototype space; placement applies
    // the inverse transpose later.
    std::vector<GfVec3d> generatedNormals(pointCount, GfVec3d(0.0));
    size_t generatedCorner = 0;
    size_t triangleEstimate = 0;
    for (size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count >= 3) triangleEstimate += count - 2;
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
            valid &= pi >= 0 && static_cast<size_t>(pi) < pointCount;
        }
        if (valid) {
            const int p0 = indices[generatedCorner];
            const int p1 = indices[generatedCorner + 1];
            const int p2 = indices[generatedCorner + 2];
            GfVec3d contribution = GfCross(
                pointAt(p1) - pointAt(p0), pointAt(p2) - pointAt(p0));
            if (flip) contribution = -contribution;
            for (int k = 0; k < count; ++k)
                generatedNormals[indices[generatedCorner + k]] += contribution;
        }
        generatedCorner += count;
    }
    if (refined.valid) {
        // Limit normals follow the winding of the authored faces.
        for (size_t i = 0; i < pointCount && 3 * i + 2 < refined.normals.size(); ++i) {
            GfVec3d n(refined.normals[3 * i], refined.normals[3 * i + 1], refined.normals[3 * i + 2]);
            if (n.GetLengthSq() > 1e-20) generatedNormals[i] = flip ? -n : n;
        }
    }

    std::unique_ptr<Mesh, ConvertedMeshDeleter> mesh(new Mesh());
    mesh->primvarStride = extraStride;
    for (size_t e = 0; e < extras.size(); ++e)
        mesh->primvars.push_back({extras[e].name, extras[e].components, extraOffsets[e]});
    mesh->vertices.reserve(3 * triangleEstimate);
    mesh->normals.reserve(3 * triangleEstimate);
    mesh->texcoords.reserve(3 * triangleEstimate);
    mesh->indices.reserve(3 * triangleEstimate);
    std::vector<int> coarseFaces;
    coarseFaces.reserve(triangleEstimate);
    // Reused across faces so ordinary faces allocate nothing.
    std::vector<GfVec3d> facePoints;
    std::vector<std::array<int, 3>> triangles;
    size_t corner = 0;
    for (size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count < 3 || corner + count > indices.size()) {
            ++info.malformedFaceCount;
            corner += std::max(0, count);
            continue;
        }
        if (holeSet.count(static_cast<int>(face))) { corner += count; continue; }
        facePoints.clear();
        bool validFace = true;
        for (int k = 0; k < count; ++k) {
            int const pi = indices[corner + k];
            if (pi < 0 || static_cast<size_t>(pi) >= pointCount) {
                validFace = false;
                break;
            }
            facePoints.emplace_back(pointAt(pi));
        }
        if (!validFace) {
            ++info.malformedFaceCount;
            corner += count;
            continue;
        }
        triangles.resize(count - 2);
        size_t const triangleCount = TriangulatePolygon(
            facePoints.data(), facePoints.size(), triangles.data());
        if (triangleCount == 0) {
            ++info.degenerateFaceCount;
            corner += count;
            continue;
        }
        for (size_t t = 0; t < triangleCount; ++t) {
            std::array<int, 3> const &triangle = triangles[t];
            int local[3] = {triangle[0], triangle[1], triangle[2]};
            if (flip) std::swap(local[1], local[2]);
            GfVec3d wp[3];
            size_t first = mesh->vertices.size();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                wp[v] = geometryTransform.Transform(pointAt(pi));
                mesh->vertices.emplace_back(static_cast<float>(wp[v][0]),
                                            static_cast<float>(wp[v][1]),
                                            static_cast<float>(wp[v][2]));
                const size_t ui = _PrimvarIndex(uvPv.interpolation, pi, ci, coarseFace(face));
                if (ui < authoredUvCount) {
                    if (authoredUvsFloat) {
                        mesh->texcoords.emplace_back((*authoredUvsFloat)[ui][0],
                                                     (*authoredUvsFloat)[ui][1]);
                    } else {
                        mesh->texcoords.emplace_back(
                            static_cast<float>((*authoredUvsDouble)[ui][0]),
                            static_cast<float>((*authoredUvsDouble)[ui][1]));
                    }
                }
                else mesh->texcoords.emplace_back(0.0f, 0.0f);
                if (extraStride) appendExtras(mesh->primvarValues, size_t(pi), ci, face);
            }
            const GfVec3d geometric = GfCross(wp[1] - wp[0], wp[2] - wp[0]).GetNormalized();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                const size_t ni = _PrimvarIndex(normalPv.interpolation, pi, ci, coarseFace(face));
                GfVec3d n = geometric;
                if (ni < authoredNormalCount) {
                    GfVec3d authored = authoredNormalAt(ni);
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
            coarseFaces.push_back(static_cast<int>(coarseFace(face)));
        }
        corner += count;
    }
    if (mesh->indices.empty()) return result;

    mesh->ensureTangents();
    // Merge only identical complete attribute tuples after tangent generation.
    // Keeping tangent sign in the key preserves mirrored UV seams and avoids
    // changing interpolated frames when positions/normals happen to match.
    struct VertexHash {
        size_t operator()(std::array<uint32_t,14> const& value) const {
            size_t h = 0;
            for (auto bits : value) h ^= std::hash<uint32_t>{}(bits) + size_t{0x9e3779b9} + (h<<6) + (h>>2);
            return h;
        }
    };
    std::unordered_map<std::array<uint32_t,14>, unsigned, VertexHash> uniqueVertices;
    // Named primvars take part in the merge through a 64-bit digest, checked
    // against the values on a match.
    const size_t extraCount = size_t(mesh->primvarStride);
    std::vector<float> compactExtras;
    compactExtras.reserve(extraCount * std::min(mesh->vertices.size(), pointCount));
    uniqueVertices.reserve(mesh->vertices.size());
    // Shared corners usually make the compact arrays far smaller than the
    // expanded ones; the authored point count is the closest cheap guess.
    const size_t compactEstimate = std::min(mesh->vertices.size(), pointCount);
    std::vector<Float3> compactPositions, compactNormals;
    std::vector<Float2> compactUvs;
    std::vector<float> compactTangents;
    compactPositions.reserve(compactEstimate);
    compactNormals.reserve(compactEstimate);
    compactUvs.reserve(compactEstimate);
    compactTangents.reserve(4 * compactEstimate);
    std::vector<unsigned> remap(mesh->vertices.size());
    for (size_t i=0; i<mesh->vertices.size(); ++i) {
        const auto& p=mesh->vertices[i]; const auto& n=mesh->normals[i];
        const auto& uv=mesh->texcoords[i];
        const float values[12]={p.x(),p.y(),p.z(),n.x(),n.y(),n.z(),uv.u(),uv.v(),
            mesh->tangents[4*i],mesh->tangents[4*i+1],mesh->tangents[4*i+2],mesh->tangents[4*i+3]};
        std::array<uint32_t,14> key{};
        for (size_t c=0;c<12;++c) key[c]=std::bit_cast<uint32_t>(values[c]);
        float const* extra = extraCount ? mesh->primvarValues.data() + i * extraCount : nullptr;
        if (extra) {
            uint64_t digest = 1469598103934665603ull;
            for (size_t c=0;c<extraCount;++c) digest=(digest^std::bit_cast<uint32_t>(extra[c]))*1099511628211ull;
            key[12]=uint32_t(digest); key[13]=uint32_t(digest>>32);
        }
        auto [found,inserted]=uniqueVertices.emplace(key,static_cast<unsigned>(compactPositions.size()));
        bool fresh=inserted;
        if (!inserted && extra &&
            !std::equal(extra, extra + extraCount, compactExtras.data() + size_t(found->second) * extraCount))
            fresh=true; // digest collision: keep the vertex apart
        remap[i]=fresh ? static_cast<unsigned>(compactPositions.size()) : found->second;
        if (fresh) {
            compactPositions.push_back(p); compactNormals.push_back(n); compactUvs.push_back(uv);
            compactTangents.insert(compactTangents.end(),values+8,values+12);
            if (extra) compactExtras.insert(compactExtras.end(), extra, extra + extraCount);
        }
    }
    for (auto& index : mesh->indices) index=remap[index];
    mesh->vertices=std::move(compactPositions); mesh->normals=std::move(compactNormals);
    mesh->texcoords=std::move(compactUvs); mesh->tangents=std::move(compactTangents);
    mesh->primvarValues=std::move(compactExtras);
    if (displaced) _Displace(*mesh, *displacer, flip);
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
    for (auto const &primvar : mesh->primvars) hashCombine(geometryHash, primvar.name);
    for (float const value : mesh->primvarValues) hashCombine(geometryHash, value);
    for (unsigned int const index : mesh->indices) hashCombine(geometryHash, index);
    info.geometryHash = geometryHash;
    info.coarseFaces = SharedFaceTable(std::move(coarseFaces));
    result.mesh = std::move(mesh);
    return result;
}

bool _EqualMeshGeometry(Mesh const &mesh, Mesh const *other)
{
    if (!other || mesh.vertices.size() != other->vertices.size() ||
        mesh.normals.size() != other->normals.size() ||
        mesh.texcoords.size() != other->texcoords.size() ||
        mesh.tangents != other->tangents ||
        mesh.indices != other->indices) return false;
    auto same3 = [](Float3 const &a, Float3 const &b) {
        return a.x()==b.x() && a.y()==b.y() && a.z()==b.z(); };
    for (size_t i=0;i<mesh.vertices.size();++i)
        if (!same3(mesh.vertices[i], other->vertices[i])) return false;
    for (size_t i=0;i<mesh.normals.size();++i)
        if (!same3(mesh.normals[i], other->normals[i])) return false;
    for (size_t i=0;i<mesh.texcoords.size();++i)
        if (mesh.texcoords[i].u()!=other->texcoords[i].u() ||
            mesh.texcoords[i].v()!=other->texcoords[i].v()) return false;
    return true;
}

std::shared_ptr<PrototypeStorage> _FindPooledStorage(
    PrototypePool &pool, size_t geometryHash, Mesh const &mesh)
{
    std::lock_guard lock(pool.mutex);
    auto& entries = pool.buckets[geometryHash];
    for (auto it=entries.begin();it!=entries.end();) {
        auto candidate=it->lock();
        if (!candidate) { it=entries.erase(it); continue; }
        if (_EqualMeshGeometry(mesh, dynamic_cast<Mesh*>(candidate->geometry.get()))) return candidate;
        ++it;
    }
    return {};
}

// Owns one prototype scene from creation to commit, so distinct prototypes
// can be built concurrently without a lock.
void _BuildPrototypeScene(RTCDevice device, PrototypeStorage &storage,
                          MeshConversion &conversion)
{
    storage.scene = rtcNewScene(device);
    conversion.mesh->setupEmbree(device, storage.scene);
    rtcCommitScene(storage.scene);
    storage.geometry.reset(conversion.mesh.release());
}

bool _PlacementEligible(HdSceneIndexBaseRefPtr const &index,
    SdfPath const &path, HdContainerDataSourceHandle const &data,
    std::vector<InstanceLevel> const &chain, TfTokenVector const &tags)
{
    if (!_WorldVisible(index, path) || !_PurposeMatches(data, tags)) return false;
    return std::all_of(chain.begin(), chain.end(), [&](InstanceLevel const &level) {
        return _WorldVisible(index, level.instancerPath);
    });
}

bool _AppendMesh(HdSceneIndexBaseRefPtr const &sceneIndex,
                 SdfPath const &path, HdSceneIndexPrim const &prim,
                 MaterialService *materials, Snapshot *snapshot,
                 GfMatrix4d const &world, SdfPath const &instancerPath,
                 int instanceId, std::vector<InstanceLevel> const &instanceChain,
                 TfTokenVector const &renderTags,
                 PreparedMesh const *prepared = nullptr, bool attachPlacement = true)
{
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
            const int primId = _EnsurePrimId(snapshot, primKey);
            snapshot->geomToPrimId.emplace(geomId, primId);
            snapshot->geomToCoarseFaces.emplace(
                geomId, cached->second.coarseFaces);
            snapshot->geomToInstanceId.emplace(geomId, instanceId);
            if (!instanceChain.empty())
                snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
            _SetInstanceEnabled(snapshot, geomId, _PlacementEligible(
                sceneIndex, path, prim.dataSource, instanceChain, renderTags));
            ++snapshot->instanceCount;
            return true;
        }
    }

    PreparedMesh inlineMesh;
    if (!prepared) {
        // Prototype refreshes touch a few prims, so they convert inline.
        const auto conversionStart = std::chrono::steady_clock::now();
        MeshConversion conversion = _ConvertMesh(
            prim.dataSource, _DisplacingMaterial(sceneIndex, prim.dataSource, materials, snapshot));
        snapshot->meshConversionSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - conversionStart).count();
        inlineMesh.info = conversion.info;
        if (conversion.mesh) {
            inlineMesh.storage = _FindPooledStorage(
                *snapshot->prototypePool, conversion.info.geometryHash,
                *conversion.mesh);
            if (!inlineMesh.storage) {
                const auto prototypeCommitStart = std::chrono::steady_clock::now();
                inlineMesh.storage = std::make_shared<PrototypeStorage>();
                _BuildPrototypeScene(snapshot->device, *inlineMesh.storage,
                                     conversion);
                inlineMesh.builtStorage = true;
                snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - prototypeCommitStart).count();
                std::lock_guard lock(snapshot->prototypePool->mutex);
                snapshot->prototypePool->buckets[conversion.info.geometryHash]
                    .push_back(inlineMesh.storage);
            }
        }
        prepared = &inlineMesh;
    }
    MeshConversionInfo const &info = prepared->info;

    if (snapshot->warnedMotionPaths.insert(path).second && info.motionBlur) {
        TF_WARN("PrismRT: motion blur authored on <%s> is not supported; using time 0",
                path.GetText());
    }
    if (info.missingPoints || info.unsupportedPoints) {
        bool shouldWarn = false;
        {
            std::lock_guard lock(snapshot->prototypePool->mutex);
            shouldWarn = snapshot->prototypePool->warnedPointPaths.insert(path).second;
        }
        if (shouldWarn) {
            TF_WARN("PrismRT: mesh <%s> skipped because points are %s",
                path.GetText(), info.missingPoints ? "missing" : "an unsupported type");
        }
        return false;
    }
    if (!info.topologyRead) return false;
    if (info.multipleUvSets &&
        snapshot->warnedFeatures.insert("multipleUvSets").second) {
        TF_WARN("PrismRT: multiple UV sets are not supported; using primvar 'st'");
    }
    if (info.subdivisionControlHull &&
        snapshot->warnedFeatures.insert("subdivisionControlHull").second)
        TF_WARN("PrismRT: some subdivision surfaces are rendered as control hulls "
                "(PRISM_SUBDIVISION_LEVEL 0, over PRISM_SUBDIVISION_MAX_FACES, or invalid topology; "
                "first seen at <%s>)", path.GetText());
    if (info.malformedFaceCount)
        TF_WARN("PrismRT: mesh <%s> skipped %zu malformed faces",
                path.GetText(), info.malformedFaceCount);
    if (info.degenerateFaceCount)
        TF_WARN("PrismRT: mesh <%s> skipped %zu degenerate or non-triangulable faces",
                path.GetText(), info.degenerateFaceCount);
    if (!prepared->storage) return false;

    // displayColor / displayOpacity of any interpolation: constant values
    // become the material's; others are mesh primvars (_ExtraPrimvars) the
    // material reads at each hit, their first value standing in where the
    // mesh could not keep them (an unexpected size).
    Material *material = snapshot->fallbackMaterial.get();
    PrimvarData opacityPv = _Primvar(primvars, TfToken("displayOpacity"));
    float opacity = 1.0f;
    bool primvarOpacity = false;
    if (opacityPv.value.IsHolding<VtFloatArray>()) {
        auto const &values = opacityPv.value.UncheckedGet<VtFloatArray>();
        if (!values.empty()) {
            opacity = values.front();
            primvarOpacity = opacityPv.interpolation != HdPrimvarSchemaTokens->constant;
        }
    }
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
                auto displayMaterial = std::make_unique<FallbackMaterial>(texture.get(), opacity,
                    displayColor.interpolation != HdPrimvarSchemaTokens->constant, primvarOpacity);
                material = displayMaterial.get();
                snapshot->displayColorTextures.push_back(std::move(texture));
                snapshot->displayColorMaterials.push_back(std::move(displayMaterial));
                snapshot->displayMaterialsByPath.emplace(path, material);
            }
        }
    }
    if (material == snapshot->fallbackMaterial.get() &&
        (primvarOpacity || NormalizeFallbackOpacity(opacity) < 1.0f)) {
        auto const existing = snapshot->displayMaterialsByPath.find(path);
        if (existing != snapshot->displayMaterialsByPath.end()) {
            material = existing->second;
        } else {
            auto displayMaterial = std::make_unique<FallbackMaterial>(
                snapshot->fallbackTexture.get(), opacity, false, primvarOpacity);
            material = displayMaterial.get();
            snapshot->displayColorMaterials.push_back(std::move(displayMaterial));
            snapshot->displayMaterialsByPath.emplace(path, material);
        }
    }
    const SdfPath materialPath = _MaterialPath(prim.dataSource);
    if (!materialPath.IsEmpty())
        _RecordMaterialUser(snapshot, materialPath, path);
    if (std::shared_ptr<Material> resolved = _ResolveMaterial(
            sceneIndex, materialPath, materials, snapshot))
        material = resolved.get();

    // Hydra 2 represents authored material subsets as geomSubset children of
    // the mesh. Bindings index the Embree triangle id, so expand each coarse
    // face assignment to every triangle emitted by triangulation. Faces
    // outside an authored subset retain the mesh's base material.
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
        if (subsetMaterialPath.IsEmpty() ||
            (!materials && !snapshot->materialsSeeded)) continue;
        _RecordMaterialUser(snapshot, subsetMaterialPath, path);
        std::shared_ptr<Material> resolved = _ResolveMaterial(
            sceneIndex, subsetMaterialPath, materials, snapshot);
        if (!resolved) continue;
        Material *const subsetMaterial = resolved.get();
        for (int const face : std::as_const(faces)) {
            if (face >= 0 && static_cast<size_t>(face) < info.faceCount)
                faceMaterials[face] = subsetMaterial;
        }
    }
    SceneManager::PlacementMaterialBinding placementBinding;
    placementBinding.fallback = material;
    // Storage may be shared with other snapshots, so materials live only in
    // this placement binding, never on the mesh.
    if (!faceMaterials.empty()) {
        for (size_t triangle = 0; triangle < info.coarseFaces.size(); ++triangle) {
            auto const found = faceMaterials.find(info.coarseFaces[triangle]);
            placementBinding.triangles.emplace(static_cast<int>(triangle),
                found == faceMaterials.end() ? material : found->second);
        }
    }

    std::shared_ptr<PrototypeStorage> storage = prepared->storage;
    Snapshot::Prototype const *shared = nullptr;
    auto& bucket = snapshot->prototypeHashBuckets[info.geometryHash];
    for (auto const& candidatePath : bucket) {
        const auto found = snapshot->prototypes.find(candidatePath);
        if (found != snapshot->prototypes.end() &&
            found->second.geometryHash == info.geometryHash &&
            (found->second.storage == storage || _EqualMeshGeometry(
                *static_cast<Mesh *>(storage->geometry.get()),
                dynamic_cast<Mesh *>(found->second.geometry)))) {
            shared = &found->second;
            break;
        }
    }
    if (shared) storage = shared->storage;
    RTCScene prototypeScene = storage->scene;
    Mesh* prototypeMesh = static_cast<Mesh*>(storage->geometry.get());
    Snapshot::Prototype prototype{prototypeScene, prototypeMesh, info.coarseFaces,
                                  placementBinding, info.geometryHash, storage};
    snapshot->prototypes.emplace(path, prototype);
    if (std::find(bucket.begin(), bucket.end(), path) == bucket.end()) bucket.push_back(path);
    if (attachPlacement) {
        const unsigned int geomId = _AttachPrototype(snapshot, path, prototype, world);
        PrimKey const primKey{path, instancerPath};
        const int primId = _EnsurePrimId(snapshot, primKey);
        snapshot->geomToPrimId.emplace(geomId, primId);
        snapshot->geomToCoarseFaces.emplace(geomId, prototype.coarseFaces);
        snapshot->geomToInstanceId.emplace(geomId, instanceId);
        if (!instanceChain.empty())
            snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
        if (instanceId >= 0) ++snapshot->instanceCount;
        _SetInstanceEnabled(snapshot, geomId, _PlacementEligible(
            sceneIndex, path, prim.dataSource, instanceChain, renderTags));
    }
    if (!shared) {
        snapshot->estimatedGeometryBytes +=
            prototypeMesh->vertices.size() * sizeof(Float3) +
            prototypeMesh->normals.size() * sizeof(Float3) +
            prototypeMesh->texcoords.size() * sizeof(Float2) +
            prototypeMesh->indices.size() * sizeof(unsigned int) +
            prototypeMesh->tangents.size() * sizeof(float);
        if (prepared->builtStorage) ++snapshot->prototypeBuildCount;
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
    PrimvarData opacityPv = _Primvar(primvars, TfToken("displayOpacity"));
    float opacity = 1.0f;
    if (opacityPv.interpolation == HdPrimvarSchemaTokens->constant &&
        opacityPv.value.IsHolding<VtFloatArray>()) {
        auto const &values = opacityPv.value.UncheckedGet<VtFloatArray>();
        if (!values.empty()) opacity = values.front();
    }
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
                auto displayMaterial = std::make_unique<FallbackMaterial>(texture.get(), opacity);
                material = displayMaterial.get();
                snapshot->displayColorTextures.push_back(std::move(texture));
                snapshot->displayColorMaterials.push_back(
                    std::move(displayMaterial));
                snapshot->displayMaterialsByPath.emplace(path, material);
            }
        }
    }
    if (material == snapshot->fallbackMaterial.get() &&
        NormalizeFallbackOpacity(opacity) < 1.0f) {
        auto const existing = snapshot->displayMaterialsByPath.find(path);
        if (existing != snapshot->displayMaterialsByPath.end()) {
            material = existing->second;
        } else {
            auto displayMaterial = std::make_unique<FallbackMaterial>(
                snapshot->fallbackTexture.get(), opacity);
            material = displayMaterial.get();
            snapshot->displayColorMaterials.push_back(std::move(displayMaterial));
            snapshot->displayMaterialsByPath.emplace(path, material);
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

template<class Hit>
int _PrimIdForHit(Snapshot const *snapshot, Hit const &hit)
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
    if (auto const* cached = dynamic_cast<CachedTransformedEnvironment const*>(snapshot->environment.get()))
        return cached->importanceBytes();
    auto const *map = dynamic_cast<EnvironmentMap const *>(
        snapshot->environment.get());
    if (!map) return 0;
    return map->texelProbability.capacity() * sizeof(float) +
        map->probability.capacity() * sizeof(float) +
        map->alias.capacity() * sizeof(size_t);
}

void _BuildPickingInstanceTopologies(Snapshot *snapshot,
                                     PickingTable *picking,
                                     std::unordered_set<int> const *affected = nullptr)
{
    AccumulateMilliseconds timing{snapshot->lastPickingMilliseconds};
    std::vector<unsigned int> geometryIds;
    if (affected) {
        picking->instanceTopologies.resize(picking->primPaths.size());
        picking->logicalPlacementCountsByPrim.resize(picking->primPaths.size(), 0);
        for (int primId : *affected) {
            if (primId < 0 || static_cast<size_t>(primId) >= picking->primPaths.size()) continue;
            picking->instanceTopologies[primId].clear();
            picking->logicalPlacementCountsByPrim[primId] = 0;
            auto dependencies = snapshot->placementDependencies.find(picking->primPaths[primId]);
            if (dependencies == snapshot->placementDependencies.end()) continue;
            for (unsigned int id : dependencies->second) {
                auto prim = snapshot->geomToPrimId.find(id);
                if (prim != snapshot->geomToPrimId.end() && prim->second == primId)
                    geometryIds.push_back(id);
            }
        }
    } else {
        picking->instanceTopologies.resize(picking->primPaths.size());
        picking->logicalPlacementCountsByPrim.assign(picking->primPaths.size(), 0);
        for (auto const &[id, prim] : snapshot->geomToPrimId) geometryIds.push_back(id);
    }
    snapshot->lastPickingRecordsVisited += geometryIds.size() +
        (affected ? affected->size() : snapshot->primPaths.size());
    for (unsigned int geomId : geometryIds) {
        const int primId = snapshot->geomToPrimId.at(geomId);
        if (primId >= 0 && static_cast<size_t>(primId) <
                picking->logicalPlacementCountsByPrim.size())
            ++picking->logicalPlacementCountsByPrim[primId];
    }
    if (!affected) for (auto const &[geomId, primIds] : snapshot->sphereBatchPrimIds) {
        (void)geomId;
        for (int primId : primIds)
            if (primId >= 0 && static_cast<size_t>(primId) <
                    picking->logicalPlacementCountsByPrim.size())
                ++picking->logicalPlacementCountsByPrim[primId];
    }
    std::vector<bool> incompatible(picking->primPaths.size(), false);
    for (unsigned int geomId : geometryIds) {
        auto chainIt = snapshot->geomToInstanceChain.find(geomId);
        if (chainIt == snapshot->geomToInstanceChain.end()) continue;
        auto const &chain = chainIt->second;
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
    std::unordered_set<SdfPath, SdfPath::Hash> *handledPaths, bool deferCommit = false)
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
    if (!affected.empty() && !deferCommit) {
        rtcCommitScene(snapshot->scene->scene);
        ++snapshot->prototypePool->topLevelSceneCommits;
    }
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
                   TfTokenVector const &renderTags, bool attachPlacement = true)
{

    if (instanceId >= 0) {
        auto const cached = snapshot->prototypes.find(path);
        if (cached != snapshot->prototypes.end()) {
            const unsigned int geomId = _AttachPrototype(
                snapshot, path, cached->second, world);
            PrimKey const key{path, instancerPath};
            const int primId = _EnsurePrimId(snapshot, key);
            snapshot->geomToPrimId.emplace(geomId, primId);
            snapshot->geomToCoarseFaces.emplace(geomId, std::vector<int>{});
            snapshot->geomToInstanceId.emplace(geomId, instanceId);
            if (!instanceChain.empty())
                snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
            _SetInstanceEnabled(snapshot, geomId, _PlacementEligible(
                sceneIndex, path, prim.dataSource, instanceChain, renderTags));
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
    if (attachPlacement && snapshot->sphereBatching && instanceId < 0 &&
        _PlacementEligible(sceneIndex, path, prim.dataSource, instanceChain, renderTags)) {
        Float3 center;
        float scale = 1.0f;
        if (_BatchableSphereTransform(world, &center, &scale)) {
            const float worldRadius = sphereRadius * scale;
            if (worldRadius > 0.0f && std::isfinite(worldRadius)) {
                PrimKey const key{path, {}};
                const int primId = _EnsurePrimId(snapshot, key);
                snapshot->pendingSpheres.push_back(
                    {path, primId, center, worldRadius, material});
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
    if (attachPlacement) {
        const unsigned int geomId = _AttachPrototype(
            snapshot, path, prototype, world);
        PrimKey const key{path, instancerPath};
        const int primId = _EnsurePrimId(snapshot, key);
        snapshot->geomToPrimId.emplace(geomId, primId);
        snapshot->geomToCoarseFaces.emplace(geomId, std::vector<int>{});
        snapshot->geomToInstanceId.emplace(geomId, instanceId);
        if (!instanceChain.empty())
            snapshot->geomToInstanceChain.emplace(geomId, instanceChain);
        if (instanceId >= 0) ++snapshot->instanceCount;
        _SetInstanceEnabled(snapshot, geomId, _PlacementEligible(
            sceneIndex, path, prim.dataSource, instanceChain, renderTags));
    }
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
        std::unordered_map<int, int> duplicateOccurrences;
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
                static_cast<int>(ordinal), static_cast<int>(indices.size()),
                duplicateOccurrences[sourceIndex]++});
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

// Cache the captured instancers and the prototype relationships, including
// zero-placement prototypes. Only retained data sources reach this worker.
void _CachePlacementInputs(HdSceneIndexBaseRefPtr const &index, Snapshot *snapshot,
    std::unordered_set<SdfPath, SdfPath::Hash> const *affected = nullptr)
{
    AccumulateMilliseconds timing{snapshot->lastInputMetadataMilliseconds};
    std::unordered_set<SdfPath, SdfPath::Hash> all;
    if (!affected) {
        snapshot->instancerInputs.clear();
        snapshot->prototypeDependencies.clear();
        snapshot->prototypeDependencyPaths.clear();
        snapshot->prototypeInstancerPaths.clear();
        snapshot->instancerPrototypeUsers.clear();
        for (auto const &[path, storage] : snapshot->prototypes) all.insert(path);
        affected = &all;
    }
    std::unordered_map<SdfPath, HdContainerDataSourceHandle, SdfPath::Hash> captured;
    auto input = [&](SdfPath const &path) {
        auto [it, added] = captured.try_emplace(path);
        if (added) it->second = index->GetPrim(path).dataSource;
        return it->second;
    };
    for (auto const &prototype : *affected) {
        ++snapshot->lastInputMetadataPrototypesVisited;
        if (auto previous = snapshot->prototypeDependencyPaths.find(prototype);
            previous != snapshot->prototypeDependencyPaths.end()) {
            for (auto const &path : previous->second) {
                auto it = snapshot->prototypeDependencies.find(path);
                if (it == snapshot->prototypeDependencies.end()) continue;
                it->second.erase(prototype);
                if (it->second.empty()) snapshot->prototypeDependencies.erase(it);
            }
            snapshot->prototypeDependencyPaths.erase(previous);
        }
        if (auto previous = snapshot->prototypeInstancerPaths.find(prototype);
            previous != snapshot->prototypeInstancerPaths.end()) {
            for (auto const &path : previous->second) {
                auto it = snapshot->instancerPrototypeUsers.find(path);
                if (it == snapshot->instancerPrototypeUsers.end()) continue;
                it->second.erase(prototype);
                if (it->second.empty()) {
                    snapshot->instancerPrototypeUsers.erase(it);
                    snapshot->instancerInputs.erase(path);
                }
            }
            snapshot->prototypeInstancerPaths.erase(previous);
        }
        if (!snapshot->prototypes.contains(prototype)) continue;
        auto ancestors = [&](SdfPath path) {
            while (!path.IsEmpty() && path != SdfPath::AbsoluteRootPath()) {
                snapshot->prototypeDependencies[path].insert(prototype);
                snapshot->prototypeDependencyPaths[prototype].insert(path);
                path = path.GetParentPath();
            }
        };
        ancestors(prototype);
        std::unordered_set<SdfPath, SdfPath::Hash> visited;
        std::function<void(SdfPath const &, int)> collect = [&](SdfPath const &path, int depth) {
            if (depth > 16 || !visited.insert(path).second) return;
            auto const by = HdInstancedBySchema::GetFromParent(input(path));
            if (!by || !by.GetPaths()) return;
            for (auto const &instancer : by.GetPaths()->GetTypedValue(0.0f)) {
                snapshot->instancerInputs[instancer] = input(instancer);
                snapshot->instancerPrototypeUsers[instancer].insert(prototype);
                snapshot->prototypeInstancerPaths[prototype].insert(instancer);
                ancestors(instancer);
                collect(instancer, depth + 1);
            }
        };
        collect(prototype, 0);
    }
}

bool _SamePlacementData(HdDataSourceBaseHandle const &a, HdDataSourceBaseHandle const &b,
    uint64_t *scanned)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (auto ac = HdContainerDataSource::Cast(a)) {
        auto bc = HdContainerDataSource::Cast(b);
        if (!bc) return false;
        auto names = ac->GetNames(), other = bc->GetNames();
        std::sort(names.begin(), names.end()); std::sort(other.begin(), other.end());
        if (names != other) return false;
        for (auto const &name : names)
            if (!_SamePlacementData(ac->Get(name), bc->Get(name), scanned)) return false;
        return true;
    }
    if (auto av = HdVectorDataSource::Cast(a)) {
        auto bv = HdVectorDataSource::Cast(b);
        if (!bv || av->GetNumElements() != bv->GetNumElements()) return false;
        for (size_t i = 0; i < av->GetNumElements(); ++i)
            if (!_SamePlacementData(av->GetElement(i), bv->GetElement(i), scanned)) return false;
        return true;
    }
    auto av = HdSampledDataSource::Cast(a), bv = HdSampledDataSource::Cast(b);
    if (!av || !bv) return false;
    auto const left = av->GetValue(0.0f), right = bv->GetValue(0.0f);
    if (left.IsArrayValued()) *scanned += left.GetArraySize();
    return left == right;
}

bool _InstanceTransformToken(TfToken const &name)
{
    return name == HdInstancerTokens->instanceTranslations ||
        name == HdInstancerTokens->instanceRotations ||
        name == HdInstancerTokens->instanceScales ||
        name == HdInstancerTokens->instanceTransforms;
}

template<class T>
bool _ChangedInstanceArray(VtValue const &a, VtValue const &b,
    std::unordered_set<int> *changed, uint64_t *scanned)
{
    if (!a.IsHolding<VtArray<T>>() || !b.IsHolding<VtArray<T>>()) return false;
    auto const &left = a.UncheckedGet<VtArray<T>>(), &right = b.UncheckedGet<VtArray<T>>();
    const size_t count = std::max(left.size(), right.size());
    *scanned += count;
    for (size_t i = 0; i < count; ++i)
        if (i >= left.size() || i >= right.size() || left[i] != right[i])
            changed->insert(static_cast<int>(i));
    return true;
}

bool _ChangedInstanceInputs(HdContainerDataSourceHandle const &oldSource,
    HdContainerDataSourceHandle const &newSource, std::unordered_set<int> *changed,
    uint64_t *scanned)
{
    if (!oldSource || !newSource) return false;
    auto names = oldSource->GetNames(), other = newSource->GetNames();
    std::sort(names.begin(), names.end()); std::sort(other.begin(), other.end());
    if (names != other) return false;
    for (auto const &name : names)
        if (name != HdPrimvarsSchemaTokens->primvars &&
            !_SamePlacementData(oldSource->Get(name), newSource->Get(name), scanned)) return false;
    auto oldVars = HdPrimvarsSchema::GetFromParent(oldSource);
    auto newVars = HdPrimvarsSchema::GetFromParent(newSource);
    if (!oldVars || !newVars) return false;
    names = oldVars.GetContainer()->GetNames(); other = newVars.GetContainer()->GetNames();
    std::sort(names.begin(), names.end()); std::sort(other.begin(), other.end());
    if (names != other) return false;
    for (auto const &name : names) {
        if (!_InstanceTransformToken(name)) {
            if (!_SamePlacementData(oldVars.GetContainer()->Get(name),
                    newVars.GetContainer()->Get(name), scanned)) return false;
            continue;
        }
        auto const a = _Primvar(oldVars, name), b = _Primvar(newVars, name);
        if (a.interpolation != b.interpolation) return false;
        if (a.value.IsEmpty() && b.value.IsEmpty()) continue;
        if (!(_ChangedInstanceArray<GfVec3f>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfVec3d>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfQuatf>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfQuath>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfQuatd>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfVec4f>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfMatrix4f>(a.value, b.value, changed, scanned) ||
              _ChangedInstanceArray<GfMatrix4d>(a.value, b.value, changed, scanned))) return false;
    }
    return true;
}

GfMatrix4d _InstanceSourceTransform(HdContainerDataSourceHandle const &source, int index)
{
    auto primvars = HdPrimvarsSchema::GetFromParent(source);
    auto translations = _Primvar(primvars, HdInstancerTokens->instanceTranslations).value;
    auto rotations = _Primvar(primvars, HdInstancerTokens->instanceRotations).value;
    auto scales = _Primvar(primvars, HdInstancerTokens->instanceScales).value;
    auto transforms = _Primvar(primvars, HdInstancerTokens->instanceTransforms).value;
    GfMatrix4d result(1.0), matrix(1.0);
    GfVec3f v; GfVec3d vd;
    if (_ArrayValue(translations, index, &v) ||
        (_ArrayValue(translations, index, &vd) && (v = GfVec3f(vd), true))) {
        matrix.SetTranslate(GfVec3d(v)); result = matrix * result;
    }
    GfQuatf q; GfQuath qh; GfQuatd qd; GfVec4f qv;
    matrix.SetIdentity();
    if (_ArrayValue(rotations, index, &q)) matrix.SetRotate(GfQuatd(q));
    else if (_ArrayValue(rotations, index, &qh)) matrix.SetRotate(GfQuatd(qh));
    else if (_ArrayValue(rotations, index, &qd)) matrix.SetRotate(qd);
    else if (_ArrayValue(rotations, index, &qv))
        matrix.SetRotate(GfQuatd(qv[0], qv[1], qv[2], qv[3]));
    result = matrix * result;
    if (_ArrayValue(scales, index, &v) ||
        (_ArrayValue(scales, index, &vd) && (v = GfVec3f(vd), true))) {
        matrix.SetScale(GfVec3d(v)); result = matrix * result;
    }
    GfMatrix4d md; GfMatrix4f mf;
    if (_ArrayValue(transforms, index, &md)) result = md * result;
    else if (_ArrayValue(transforms, index, &mf)) result = GfMatrix4d(mf) * result;
    return result;
}

bool _PlacementTransformFromChain(HdSceneIndexBaseRefPtr const &index,
    SdfPath prototype, std::vector<InstanceLevel> const &chain, GfMatrix4d *result)
{
    *result = GfMatrix4d(1.0);
    if (chain.empty()) { *result = _WorldTransform(index, prototype); return true; }
    for (auto level = chain.rbegin(); level != chain.rend(); ++level) {
        auto by = HdInstancedBySchema::GetFromParent(index->GetPrim(prototype).dataSource);
        if (!by || !by.GetPaths()) return false;
        auto const paths = by.GetPaths()->GetTypedValue(0.0f);
        auto const roots = by.GetPrototypeRoots() ? by.GetPrototypeRoots()->GetTypedValue(0.0f) : VtArray<SdfPath>();
        auto found = std::find(paths.begin(), paths.end(), level->instancerPath);
        if (found == paths.end()) return false;
        size_t const slot = static_cast<size_t>(found - paths.begin());
        SdfPath const root = slot < roots.size() && !roots[slot].IsEmpty() ? roots[slot] : prototype;
        *result *= _RelativeTransform(index, prototype, root.GetParentPath()) *
            _InstanceSourceTransform(index->GetPrim(level->instancerPath).dataSource, level->sourceIndex);
        prototype = level->instancerPath;
    }
    *result *= _WorldTransform(index, chain.front().instancerPath);
    return true;
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
    HdLightSchema schema = HdLightSchema::GetFromParent(prim.dataSource);
    HdContainerDataSourceHandle light = schema.GetContainer();
    if (!light) return;
    const TfToken format = _ContainerValue(light, HdLightTokens->textureFormat, TfToken());
    if (!format.IsEmpty() && format != TfToken("automatic") && format != TfToken("latlong") &&
        snapshot->warnedFeatures.insert("domeTextureFormat").second)
        TF_WARN("PrismRT: dome texture format '%s' is not supported; reading it as lat-long",
                format.GetText());
    GfVec3f color = _ContainerValue(
        light, HdLightTokens->color, GfVec3f(1.0f));
    // As for the other lights: the blackbody colour (linear Rec.709) tints it.
    if (_ContainerValue(light, HdLightTokens->enableColorTemperature, false))
        color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(
            _ContainerValue(light, HdLightTokens->colorTemperature, 6500.0f)));
    const float intensity = _ContainerValue(light, HdLightTokens->intensity, 1.0f);
    const float exposure = _ContainerValue(light, HdLightTokens->exposure, 0.0f);
    const float scale = intensity * std::exp2(exposure);
    // UsdLux colours are linear Rec.709; the texture converts on preparation.
    const Color3 multiplier = prism::color::Active()->FromRec709(Color3(color[0], color[1], color[2])) * scale;
    const SdfAssetPath asset = _ContainerValue(
        light, HdLightTokens->textureFile, SdfAssetPath());
    std::string resolved = asset.GetResolvedPath();
    if (resolved.empty() && !asset.GetAssetPath().empty())
        resolved = ArGetResolver().Resolve(asset.GetAssetPath()).GetPathString();
    try {
        if (!resolved.empty() && std::filesystem::exists(resolved)) {
            snapshot->environmentTexture =
                std::make_unique<ImageTexture<Color3>>(resolved, prism::color::kLightColorSpace);
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
        std::shared_ptr<Texture<Color3>> texture = snapshot->prototypePool->whiteEnvironmentTexture;
        if (auto* image = dynamic_cast<ImageTexture<Color3>*>(snapshot->environmentTexture.get()))
            texture = std::make_shared<ImageTexture<Color3>>(*image);
        auto local = snapshot->prototypePool->environmentCache.Get(std::move(texture), multiplier);
        auto dome = std::make_unique<CachedTransformedEnvironment>(std::move(local), linear);
        dome->diffuseFactor = _ContainerValue(light, HdLightTokens->diffuse, 1.0f);
        dome->specularFactor = _ContainerValue(light, HdLightTokens->specular, 1.0f);
        // Several domes light the scene together (CompositeEnvironment).
        if (++snapshot->domeCount == 1) {
            snapshot->environment = std::move(dome);
        } else {
            if (snapshot->domeCount == 2)
                snapshot->domeEnvironments.push_back(std::move(snapshot->environment));
            snapshot->domeEnvironments.push_back(std::move(dome));
            std::vector<const Environment *> parts;
            for (auto const &part : snapshot->domeEnvironments) parts.push_back(part.get());
            snapshot->environment = std::make_unique<CompositeEnvironment>(std::move(parts));
        }
        snapshot->scene->environment = snapshot->environment.get();
    } catch (std::exception const &e) {
        TF_WARN("PrismRT: failed to load dome environment: %s", e.what());
    }
}

// UsdLux sphere, rect, disk, cylinder and distant lights.
void _AddAnalyticLight(HdSceneIndexBaseRefPtr const &scene,
                       SdfPath const &path, HdSceneIndexPrim const &prim,
                       Snapshot *snapshot)
{
    prism::LightParameters params;
    TfToken const &type = prim.primType;
    if (type == HdPrimTypeTokens->sphereLight) params.shape = prism::LightShape::Sphere;
    else if (type == HdPrimTypeTokens->rectLight) params.shape = prism::LightShape::Rect;
    else if (type == HdPrimTypeTokens->diskLight) params.shape = prism::LightShape::Disk;
    else if (type == HdPrimTypeTokens->cylinderLight) params.shape = prism::LightShape::Cylinder;
    else if (type == HdPrimTypeTokens->distantLight) params.shape = prism::LightShape::Distant;
    else {
        if (snapshot->warnedFeatures.insert("lightType:" + type.GetString()).second)
            TF_WARN("PrismRT: light type '%s' is not supported and will be skipped", type.GetText());
        return;
    }
    if (!_WorldVisible(scene, path)) return;
    HdContainerDataSourceHandle light =
        HdLightSchema::GetFromParent(prim.dataSource).GetContainer();
    if (!light) return;
    GfVec3f color = _ContainerValue(light, HdLightTokens->color, GfVec3f(1.0f));
    if (_ContainerValue(light, HdLightTokens->enableColorTemperature, false)) {
        const GfVec3f temperature = UsdLuxBlackbodyTemperatureAsRgb(
            _ContainerValue(light, HdLightTokens->colorTemperature, 6500.0f));
        color = GfCompMult(color, temperature);
    }
    params.color = Color3(color[0], color[1], color[2]);
    params.intensity = _ContainerValue(light, HdLightTokens->intensity,
        params.shape == prism::LightShape::Distant ? 50000.0f : 1.0f);
    params.exposure = _ContainerValue(light, HdLightTokens->exposure, 0.0f);
    params.normalize = _ContainerValue(light, HdLightTokens->normalize, false);
    params.radius = _ContainerValue(light, HdLightTokens->radius, 0.5f);
    params.width = _ContainerValue(light, HdLightTokens->width, 1.0f);
    params.height = _ContainerValue(light, HdLightTokens->height, 1.0f);
    params.length = _ContainerValue(light, HdLightTokens->length, 1.0f);
    params.angle = _ContainerValue(light, HdLightTokens->angle, 0.53f);
    params.treatAsPoint = _ContainerValue(light, _LightTokens().treatAsPoint, false);
    params.cameraVisible = _ContainerValue(light, _LightTokens().cameraVisibility, false);
    params.diffuse = _ContainerValue(light, HdLightTokens->diffuse, 1.0f);
    params.specular = _ContainerValue(light, HdLightTokens->specular, 1.0f);
    const GfMatrix4d world = _WorldTransform(scene, path);
    std::array<double, 16> matrix{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) matrix[row * 4 + column] = world[row][column];
    prism::SceneLight result = prism::MakeSceneLight(params, matrix);
    // Light linking (HdsiLightLinkingSceneIndex): a category per non-trivial
    // collection, at most 32 of each kind.
    auto categoryBit = [&](TfToken const &category, auto &bits) -> uint32_t {
        if (category.IsEmpty()) return 0u;
        auto found = bits.find(category);
        if (found != bits.end()) return found->second;
        if (bits.size() >= 32) {
            if (snapshot->warnedFeatures.insert("linkCategories").second)
                TF_WARN("PrismRT: more than 32 light or shadow link collections; further ones are ignored");
            return 0u;
        }
        const uint32_t bit = 1u << bits.size();
        bits.emplace(category, bit);
        return bit;
    };
    result.lightLinkBit = categoryBit(_ContainerValue(light, HdTokens->lightLink, TfToken()),
                                      snapshot->lightLinkBits);
    result.shadowLinkBit = categoryBit(_ContainerValue(light, HdTokens->shadowLink, TfToken()),
                                       snapshot->shadowLinkBits);
    if (params.shape == prism::LightShape::Rect) {
        const SdfAssetPath asset = _ContainerValue(light, HdLightTokens->textureFile, SdfAssetPath());
        std::string resolved = asset.GetResolvedPath();
        if (resolved.empty() && !asset.GetAssetPath().empty())
            resolved = ArGetResolver().Resolve(asset.GetAssetPath()).GetPathString();
        if (!resolved.empty() && std::filesystem::exists(resolved)) {
            try {
                result.texture = std::make_shared<ImageTexture<Color3>>(resolved, prism::color::kLightColorSpace);
            } catch (std::exception const &e) {
                TF_WARN("PrismRT: failed to load light texture '%s': %s", resolved.c_str(), e.what());
            }
        } else if (!asset.GetAssetPath().empty()) {
            TF_WARN("PrismRT: could not resolve light texture '%s'", asset.GetAssetPath().c_str());
        }
    }
    snapshot->scene->lights.push_back(std::move(result));
}

// Mesh prototypes converted ahead of the serial walk, which consumes them in
// traversal order. Instanced placements are kept so they are expanded once.
struct MeshPreparation {
    std::unordered_map<SdfPath, PreparedMesh, SdfPath::Hash> meshes;
    std::unordered_map<SdfPath, std::vector<InstancePlacement>, SdfPath::Hash>
        placements;
};

bool _HasEligiblePlacement(HdSceneIndexBaseRefPtr const &scene,
    SdfPath const &path, HdSceneIndexPrim const &prim, TfTokenVector const &tags)
{
    if (!_PlacementEligible(scene, path, prim.dataSource, {}, tags)) return false;
    if (!HdInstancedBySchema::GetFromParent(prim.dataSource)) return true;
    auto const placements = _InstancePlacements(scene, path, prim.dataSource);
    return std::any_of(placements.begin(), placements.end(), [&](auto const &placement) {
        return _PlacementEligible(scene, path, prim.dataSource, placement.chain, tags);
    });
}

void _CollectMeshes(HdSceneIndexBaseRefPtr const &scene, SdfPath const &path,
                    TfTokenVector const &renderTags,
                    std::function<bool()> const &cancelled,
                    MeshPreparation *preparation,
                    std::vector<std::pair<SdfPath, HdContainerDataSourceHandle>>
                        *candidates, Snapshot *snapshot)
{
    if (cancelled && cancelled()) return;
    HdSceneIndexPrim const prim = scene->GetPrim(path);
    if (prim.primType == HdPrimTypeTokens->mesh) {
        if (snapshot->evictedPrototypes.contains(path) &&
            !_HasEligiblePlacement(scene, path, prim, renderTags)) {
            for (SdfPath const &child : scene->GetChildPrimPaths(path))
                _CollectMeshes(scene, child, renderTags, cancelled, preparation,
                               candidates, snapshot);
            return;
        }
        // Preparation owns supported prototypes even without enabled placements.
        if (HdInstancedBySchema::GetFromParent(prim.dataSource))
            preparation->placements.emplace(path,
                _InstancePlacements(scene, path, prim.dataSource));
        HdMeshSchema const meshSchema =
            HdMeshSchema::GetFromParent(prim.dataSource);
        if (meshSchema && meshSchema.GetTopology() &&
            HdPrimvarsSchema::GetFromParent(prim.dataSource))
            candidates->emplace_back(path, prim.dataSource);
    }
    for (SdfPath const &child : scene->GetChildPrimPaths(path))
        _CollectMeshes(scene, child, renderTags, cancelled, preparation,
                       candidates, snapshot);
}

// Whether the material bound to a mesh can displace it: only a network with a
// connected displacement terminal can. Reads the network, compiles nothing.
bool _MaterialMayDisplace(HdSceneIndexBaseRefPtr const &scene,
                          HdContainerDataSourceHandle const &ds)
{
    const SdfPath materialPath = _MaterialPath(ds);
    if (materialPath.IsEmpty()) return false;
    return MaterialService::MayDisplace(scene->GetPrim(materialPath).dataSource);
}

// Conversion and Embree prototype builds are independent per prim, so they
// run on TBB workers. Sharing is resolved serially in traversal order, which
// keeps storage choice, prim ids and statistics identical to a serial walk.
//
// With awaitMaterials, the batch's materials are still being prepared: when
// no mesh's material can displace, meshes convert meanwhile, and the
// displacers resolved afterwards must confirm that none displaces (otherwise
// the speculative work is discarded and the meshes convert again). It
// returns false when the build was cancelled.
MeshPreparation _PrepareMeshes(HdSceneIndexBaseRefPtr const &scene,
                               MaterialService *materials,
                               Snapshot *snapshot,
                               TfTokenVector const &renderTags,
                               std::function<bool()> const &cancelled,
                               std::function<bool()> const &awaitMaterials = {})
{
    MeshPreparation preparation;
    std::vector<std::pair<SdfPath, HdContainerDataSourceHandle>> candidates;
    _CollectMeshes(scene, SdfPath::AbsoluteRootPath(), renderTags, cancelled,
                   &preparation, &candidates, snapshot);
    if (cancelled && cancelled()) return preparation;

    // Meshes whose material may displace wait for the materials; the others
    // convert meanwhile, against a refinement budget planned with those
    // displacing. Resolving the materials afterwards must reproduce that plan
    // for every converted mesh, or the meshes convert again.
    std::vector<uint8_t> deferred(candidates.size(), 0);
    bool speculative = bool(awaitMaterials);
    if (speculative) {
        size_t count = 0;
        for (size_t i = 0; i < candidates.size(); ++i)
            count += deferred[i] = _MaterialMayDisplace(scene, candidates[i].second);
        speculative = count < candidates.size();
        if (!speculative) std::fill(deferred.begin(), deferred.end(), 0);
    }
    if (awaitMaterials && !speculative && !awaitMaterials()) return preparation;

    PrototypePool &pool = *snapshot->prototypePool;
    // Displacing materials, resolved before the parallel conversion.
    std::vector<Material const *> displacers(candidates.size(), nullptr);
    for (size_t i = 0; !speculative && i < candidates.size(); ++i)
        displacers[i] = _DisplacingMaterial(scene, candidates[i].second, materials, snapshot);
    uint64_t build = 0;
    // Prototypes converted earlier with the same material are reused.
    auto lookUp = [&](size_t i) {
        auto const entry = pool.conversions.find(candidates[i].second.get());
        std::shared_ptr<PrototypeStorage> storage =
            entry != pool.conversions.end() && entry->second.displacer == displacers[i]
                ? entry->second.storage.lock() : nullptr;
        if (!storage) return false;
        entry->second.lastBuild = build;
        preparation.meshes[candidates[i].first] =
            {entry->second.info, std::move(storage), false};
        return true;
    };
    std::vector<size_t> misses;
    {
        std::lock_guard lock(pool.mutex);
        build = ++pool.conversionBuilds;
        for (size_t i = 0; i < candidates.size(); ++i)
            if (!deferred[i] && !lookUp(i)) misses.push_back(i);
    }

    // Refinement budget over every prototype of the build. Cache hits keep
    // the level they were converted at.
    std::vector<std::vector<size_t>> refinementFaces(candidates.size());
    std::vector<bool> fixedLevel(candidates.size(), true);
    for (size_t const i : misses) fixedLevel[i] = false;
    for (size_t i = 0; i < candidates.size(); ++i) if (deferred[i]) fixedLevel[i] = false;
    auto refine = [&](size_t i) {
        refinementFaces[i] = _RefinementFaces(candidates[i].second, deferred[i] && speculative
            ? true : displacers[i] && displacers[i]->hasDisplacement());
        if (fixedLevel[i]) {
            auto const found = preparation.meshes.find(candidates[i].first);
            const size_t level = found != preparation.meshes.end()
                ? size_t(std::max(0, found->second.info.subdivisionLevel)) : 0;
            if (refinementFaces[i].size() > level + 1) refinementFaces[i].resize(level + 1);
        }
    };
    tbb::parallel_for(size_t(0), candidates.size(), [&](size_t i) {
        if (cancelled && cancelled()) return;
        refine(i);
    });
    if (cancelled && cancelled()) return preparation;
    size_t requestedFaces = 0, plannedFaces = 0;
    std::vector<int> levelCaps = _BudgetSubdivisionLevels(refinementFaces, fixedLevel,
        _Subdivision().budgetFaces, &plannedFaces, &requestedFaces);

    std::vector<MeshConversion> conversions(misses.size());
    auto convert = [&](std::vector<size_t> const &indices, std::vector<MeshConversion> &out) {
        prism::progress::AddTotal(prism::Phase::Loading, indices.size());
        const auto conversionStart = std::chrono::steady_clock::now();
        tbb::parallel_for(size_t(0), indices.size(), [&](size_t i) {
            if (cancelled && cancelled()) return;
            out[i] = _ConvertMesh(candidates[indices[i]].second, displacers[indices[i]],
                                  levelCaps[indices[i]]);
            prism::progress::Add(prism::Phase::Loading);
        });
        // Wall time of the parallel phase, covering the whole pure conversion
        // including tangents, vertex de-duplication and hashing.
        snapshot->meshConversionSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - conversionStart).count();
    };
    convert(misses, conversions);
    if (cancelled && cancelled()) return preparation;

    if (speculative) {
        if (!awaitMaterials()) return preparation;
        for (size_t i = 0; i < candidates.size(); ++i)
            displacers[i] = _DisplacingMaterial(scene, candidates[i].second, materials, snapshot);
        bool confirmed = true;
        for (size_t i = 0; i < candidates.size(); ++i)
            confirmed &= deferred[i] || !displacers[i];
        std::vector<size_t> later;
        if (confirmed) {
            std::lock_guard lock(pool.mutex);
            for (size_t i = 0; i < candidates.size(); ++i)
                if (deferred[i] && lookUp(i)) fixedLevel[i] = true;
                else if (deferred[i]) later.push_back(i);
        }
        if (confirmed) {
            speculative = false;
            for (size_t i = 0; i < candidates.size(); ++i) if (deferred[i]) refine(i);
            const std::vector<int> planned = levelCaps;
            levelCaps = _BudgetSubdivisionLevels(refinementFaces, fixedLevel,
                _Subdivision().budgetFaces, &plannedFaces, &requestedFaces);
            for (size_t const i : misses) confirmed &= levelCaps[i] == planned[i];
        }
        if (!confirmed) {
            // Nothing of this batch has entered the pool yet.
            ++snapshot->speculativeMeshRetries;
            return _PrepareMeshes(scene, materials, snapshot, renderTags, cancelled);
        }
        std::vector<MeshConversion> laterConversions(later.size());
        convert(later, laterConversions);
        if (cancelled && cancelled()) return preparation;
        // Back into candidate order, as the walk below expects.
        std::vector<size_t> merged;
        std::vector<MeshConversion> mergedConversions;
        merged.reserve(misses.size() + later.size());
        mergedConversions.reserve(misses.size() + later.size());
        for (size_t a = 0, b = 0; a < misses.size() || b < later.size();) {
            if (b >= later.size() || (a < misses.size() && misses[a] < later[b])) {
                merged.push_back(misses[a]);
                mergedConversions.push_back(std::move(conversions[a++]));
            } else {
                merged.push_back(later[b]);
                mergedConversions.push_back(std::move(laterConversions[b++]));
            }
        }
        misses = std::move(merged);
        conversions = std::move(mergedConversions);
    }
    snapshot->prototypeConversions = misses.size();
    snapshot->prototypeConversionCacheHits = candidates.size() - misses.size();
    if (plannedFaces < requestedFaces) {
        static std::once_flag warned;
        std::call_once(warned, [&] {
            TF_WARN("PrismRT: subdivision refines to %zu faces, over the %zu-face budget "
                    "(PRISM_SUBDIVISION_BUDGET_FACES); the densest meshes use lower levels (%zu faces)",
                    requestedFaces, _Subdivision().budgetFaces, plannedFaces);
        });
    }

    // Identical geometry shares one storage, first within this batch (cache
    // hits included), then with the pool. Storage built by this batch keeps
    // builtStorage so whichever path attaches it first counts the build.
    struct BatchStorage {
        Mesh const *mesh = nullptr;
        std::shared_ptr<PrototypeStorage> storage;
        bool built = false;
    };
    std::unordered_map<size_t, std::vector<BatchStorage>> batch;
    std::vector<std::pair<std::shared_ptr<PrototypeStorage>, MeshConversion *>>
        builds;
    size_t nextMiss = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        SdfPath const &path = candidates[i].first;
        if (nextMiss >= misses.size() || misses[nextMiss] != i) {
            PreparedMesh const &hit = preparation.meshes[path];
            batch[hit.info.geometryHash].push_back({
                static_cast<Mesh const *>(hit.storage->geometry.get()),
                hit.storage, false});
            continue;
        }
        MeshConversion &conversion = conversions[nextMiss++];
        PreparedMesh prepared{conversion.info};
        if (conversion.mesh) {
            auto &sameHash = batch[conversion.info.geometryHash];
            for (BatchStorage const &candidate : sameHash) {
                if (_EqualMeshGeometry(*conversion.mesh, candidate.mesh)) {
                    prepared.storage = candidate.storage;
                    prepared.builtStorage = candidate.built;
                    break;
                }
            }
            if (!prepared.storage) {
                prepared.storage = _FindPooledStorage(
                    pool, conversion.info.geometryHash, *conversion.mesh);
                if (prepared.storage) {
                    sameHash.push_back({static_cast<Mesh const *>(
                        prepared.storage->geometry.get()), prepared.storage, false});
                } else {
                    prepared.storage = std::make_shared<PrototypeStorage>();
                    prepared.builtStorage = true;
                    sameHash.push_back({conversion.mesh.get(), prepared.storage, true});
                    builds.emplace_back(prepared.storage, &conversion);
                }
            }
        }
        preparation.meshes[path] = std::move(prepared);
    }

    const RTCDevice device = snapshot->device;
    const auto commitStart = std::chrono::steady_clock::now();
    tbb::parallel_for(size_t(0), builds.size(), [&](size_t i) {
        if (cancelled && cancelled()) return;
        _BuildPrototypeScene(device, *builds[i].first, *builds[i].second);
    });
    // Wall time of the parallel prototype build phase.
    snapshot->embreeGeometryCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - commitStart).count();
    if (cancelled && cancelled()) return preparation;
    std::lock_guard lock(pool.mutex);
    for (auto const &[storage, conversion] : builds)
        pool.buckets[conversion->info.geometryHash].push_back(storage);
    for (size_t const i : misses) {
        auto const &[path, source] = candidates[i];
        PreparedMesh const &prepared = preparation.meshes[path];
        if (!prepared.storage) continue;
        pool.conversions[source.get()] =
            {source, prepared.storage, prepared.info, build, displacers[i]};
    }
    // Keep only what this build used; a full re-capture creates new handles,
    // so older entries could never hit again.
    for (auto it = pool.conversions.begin(); it != pool.conversions.end();) {
        if (it->second.lastBuild != build || it->second.storage.expired())
            it = pool.conversions.erase(it);
        else
            ++it;
    }
    return preparation;
}

void _Walk(HdSceneIndexBaseRefPtr const &scene, SdfPath const &path,
           MaterialService *materials, Snapshot *snapshot,
           TfTokenVector const &renderTags,
           std::function<bool()> const &cancelled,
           MeshPreparation *preparation = nullptr, bool traverseChildren = true)
{
    if (cancelled && cancelled()) return;
    HdSceneIndexPrim prim = scene->GetPrim(path);
    if (prim.primType == HdPrimTypeTokens->mesh ||
        prim.primType == HdPrimTypeTokens->sphere) {
        const GfMatrix4d world = _WorldTransform(scene, path);
        const bool isInstanced = bool(
            HdInstancedBySchema::GetFromParent(prim.dataSource));
        std::vector<InstancePlacement> placements;
        bool placementsPrepared = false;
        PreparedMesh const *prepared = nullptr;
        if (preparation && prim.primType == HdPrimTypeTokens->mesh) {
            auto const storedPlacements = preparation->placements.find(path);
            if (storedPlacements != preparation->placements.end()) {
                placements = std::move(storedPlacements->second);
                placementsPrepared = true;
            }
            auto const storedMesh = preparation->meshes.find(path);
            if (storedMesh != preparation->meshes.end())
                prepared = &storedMesh->second;
        }
        if (!placementsPrepared)
            placements = _InstancePlacements(scene, path, prim.dataSource);
        if (snapshot->evictedPrototypes.contains(path)) {
            const bool enabled = placements.empty()
                ? !isInstanced && _PlacementEligible(scene, path, prim.dataSource, {}, renderTags)
                : std::any_of(placements.begin(), placements.end(), [&](auto const &placement) {
                    return _PlacementEligible(scene, path, prim.dataSource, placement.chain, renderTags);
                });
            if (!enabled) {
                if (!isInstanced) _EnsurePrimId(snapshot, {path, {}});
                for (auto const &placement : placements)
                    _EnsurePrimId(snapshot, {path, placement.instancerPath});
                if (traverseChildren)
                    for (SdfPath const &child : scene->GetChildPrimPaths(path))
                        _Walk(scene, child, materials, snapshot, renderTags, cancelled,
                              preparation);
                return;
            }
            snapshot->evictedPrototypes.erase(path);
            {
                std::lock_guard lock(snapshot->prototypePool->mutex);
                snapshot->prototypePool->evictedPaths.erase(path);
            }
            ++snapshot->prototypePool->hiddenGeometryRestorations;
        }
        if (placements.empty() && !isInstanced) {
            if (prim.primType == HdPrimTypeTokens->mesh)
                _AppendMesh(scene, path, prim, materials, snapshot,
                            world, SdfPath(), -1, {}, renderTags, prepared);
            else
                _AppendSphere(scene, path, prim, materials, snapshot,
                              world, SdfPath(), -1, {}, renderTags);
        } else if (placements.empty()) {
            if (prim.primType == HdPrimTypeTokens->mesh)
                _AppendMesh(scene, path, prim, materials, snapshot,
                    world, {}, -1, {}, renderTags, prepared, false);
            else
                _AppendSphere(scene, path, prim, materials, snapshot,
                    world, {}, -1, {}, renderTags, false);
        } else {
            for (InstancePlacement const &placement : placements) {
                if (cancelled && cancelled()) return;
                if (prim.primType == HdPrimTypeTokens->mesh)
                    _AppendMesh(scene, path, prim, materials, snapshot,
                                placement.transform, placement.instancerPath,
                                placement.instanceId, placement.chain, renderTags,
                                prepared);
                else
                    _AppendSphere(scene, path, prim, materials, snapshot,
                                  placement.transform, placement.instancerPath,
                                  placement.instanceId, placement.chain, renderTags);
            }
        }
    }
    if (prim.primType == HdPrimTypeTokens->domeLight)
        _SetDomeEnvironment(scene, path, prim, snapshot);
    else if (HdPrimTypeIsLight(prim.primType) && HdLightSchema::GetFromParent(prim.dataSource))
        _AddAnalyticLight(scene, path, prim, snapshot);
    static const std::unordered_set<std::string> unsupportedPrimTypes{
        "basisCurves", "nurbsCurves", "points", "volume", "tetMesh"};
    std::string const type = prim.primType.GetString();
    if (unsupportedPrimTypes.count(type) &&
        snapshot->warnedFeatures.insert("primType:" + type).second) {
        TF_WARN("PrismRT: prim type '%s' is not supported and will be skipped",
                type.c_str());
    }
    if (traverseChildren)
        for (SdfPath const &child : scene->GetChildPrimPaths(path))
            _Walk(scene, child, materials, snapshot, renderTags, cancelled,
                  preparation);
}

bool _RestoreEvictedPrototypes(HdSceneIndexBaseRefPtr const &scene,
    MaterialService *materials, Snapshot *snapshot, TfTokenVector const &tags,
    std::function<bool()> const &cancelled)
{
    std::vector<SdfPath> paths(snapshot->evictedPrototypes.begin(),
                               snapshot->evictedPrototypes.end());
    std::sort(paths.begin(), paths.end());
    bool restored = false;
    for (SdfPath const &path : paths) {
        if (cancelled && cancelled()) return restored;
        auto const prim = scene->GetPrim(path);
        if ((prim.primType != HdPrimTypeTokens->mesh &&
             prim.primType != HdPrimTypeTokens->sphere) ||
            !_HasEligiblePlacement(scene, path, prim, tags)) continue;
        snapshot->displayMaterialsByPath.erase(path);
        _Walk(scene, path, materials, snapshot, tags, cancelled, nullptr, false);
        restored = true;
    }
    if (restored) {
        _FlushSphereBatches(snapshot);
        _IndexPlacementDependencies(snapshot);
        _CachePlacementInputs(scene, snapshot);
        _RefreshCachedCounts(snapshot);
    }
    return restored;
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
    // Retained supported geometry needs materials before its first visibility-on.
    if (prim.primType == HdPrimTypeTokens->mesh || prim.primType == HdPrimTypeTokens->sphere) {
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

void _ApplyLinkMasks(HdSceneIndexBaseRefPtr const &index, Snapshot *snapshot);

std::shared_ptr<Snapshot> _BuildSnapshot(HdSceneIndexBaseRefPtr const &scene,
                                         MaterialService *materials,
                                         RenderSettings const &settings,
                                         TfTokenVector const &renderTags,
                                         RTCDevice sharedDevice,
                                         std::shared_ptr<PrototypePool> const& pool = {},
                                         std::function<bool()> cancelled = {},
                                         decltype(Snapshot::resolvedMaterials) const
                                             *seedMaterials = nullptr)
{
    auto result = std::make_shared<Snapshot>();
    result->device = sharedDevice;
    if (pool) result->prototypePool = pool;
    {
        std::lock_guard lock(result->prototypePool->mutex);
        result->evictedPrototypes = result->prototypePool->evictedPaths;
    }
    std::erase_if(result->evictedPrototypes, [&](SdfPath const &path) {
        auto const type = scene->GetPrim(path).primType;
        return type != HdPrimTypeTokens->mesh && type != HdPrimTypeTokens->sphere;
    });
    if (seedMaterials) {
        result->resolvedMaterials = *seedMaterials;
        result->materialsSeeded = true;
    }
    if (!result->device) return {};
    result->scene = std::make_unique<SceneManager>(result->device);
    ++result->prototypePool->topLevelSceneBuilds;
    result->scene->max_depth = settings.maxDepth;
    result->scene->rr_min_depth = settings.rouletteDepth;
    result->scene->environmentSampling =
        settings.environmentSampling == RenderSettings::EnvironmentSampling::Mis;
    result->scene->textureFiltering = settings.textureFiltering;
    result->scene->textureStreaming = settings.textureStreaming;
    result->sphereBatching = settings.sphereBatching;
    result->fallbackTexture = std::make_unique<ColorTexture>(Color3(0.5f));
    result->fallbackMaterial = std::make_unique<Diffuse>(result->fallbackTexture.get());
    auto blackEnvironment = std::make_unique<ColorTexture>(Color3(0.0f));
    result->environment = std::make_unique<ConstantColorBackground>(
        blackEnvironment.get());
    result->environmentTexture = std::move(blackEnvironment);
    result->scene->environment = result->environment.get();
    // Materials are prepared on this thread while a TBB task prepares the
    // meshes, which wait for the materials only where one may displace them.
    std::promise<void> materialsPrepared;
    std::shared_future<void> materialsReady = materialsPrepared.get_future().share();
    std::exception_ptr materialError;
    tbb::task_group meshJob;
    struct JoinMeshJob {
        tbb::task_group &job;
        ~JoinMeshJob() { try { job.wait(); } catch (...) {} }
    } joinMeshJob{meshJob};
    MeshPreparation preparation;
    const auto traversalStart = std::chrono::steady_clock::now();
    auto prepareMeshes = [&, resolverContext = ArGetResolver().GetCurrentContext()] {
        ArResolverContextBinder binder(resolverContext);
        prism::load_profile::Scope meshProfile("snapshot.meshes");
        std::function<bool()> awaitMaterials;
        if (materials) awaitMaterials = [&] {
            materialsReady.wait();
            return !materialError && !(cancelled && cancelled());
        };
        preparation = _PrepareMeshes(scene, materials, result.get(), renderTags,
                                     cancelled, awaitMaterials);
        if (prism::load_profile::Enabled())
            meshProfile.detail("conversions=" + std::to_string(result->prototypeConversions) +
                " cached=" + std::to_string(result->prototypeConversionCacheHits) +
                " retries=" + std::to_string(result->speculativeMeshRetries) +
                " convert_s=" + std::to_string(result->meshConversionSeconds) +
                " embree_prototypes_s=" + std::to_string(result->embreeGeometryCommitSeconds));
    };
    if (materials) {
        meshJob.run(prepareMeshes);
        try {
            prism::load_profile::Scope profile("snapshot.materials");
            std::unordered_set<SdfPath, SdfPath::Hash> seen;
            std::vector<MaterialService::Request> requests;
            _CollectMaterialRequests(scene, SdfPath::AbsoluteRootPath(),
                                     &seen, &requests, renderTags, cancelled);
            materials->Prefetch(requests, cancelled);
        } catch (...) {
            materialError = std::current_exception();
        }
        materialsPrepared.set_value();
        meshJob.wait();
        if (materialError) std::rethrow_exception(materialError);
    } else {
        materialsPrepared.set_value();
        prepareMeshes();
    }
    if (cancelled && cancelled()) return {};
    std::optional<prism::load_profile::Scope> walkProfile(std::in_place, "snapshot.walk");
    _Walk(scene, SdfPath::AbsoluteRootPath(), materials, result.get(),
          renderTags, cancelled, &preparation);
    walkProfile.reset();
    if (cancelled && cancelled()) return {};
    _FlushSphereBatches(result.get());
    _ApplyLinkMasks(scene, result.get());
    _IndexPlacementDependencies(result.get());
    _ApplyHiddenGeometryBudget(result.get());
    _CachePlacementInputs(scene, result.get());
    result->sceneTraversalSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - traversalStart).count();
    const auto sceneCommitStart = std::chrono::steady_clock::now();
    if (cancelled && cancelled()) return {};
    prism::load_profile::Scope commitProfile("snapshot.embree_commit");
    result->scene->commitScene();
    ++result->prototypePool->topLevelSceneCommits;
    result->embreeSceneCommitSeconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sceneCommitStart).count();
    // Later refreshes of this bank resolve through their job's service.
    result->materialsSeeded = false;
    _RefreshCachedCounts(result.get());
    return result;
}

bool _UpdatePlacements(HdSceneIndexBaseRefPtr const &index, Snapshot *snapshot,
                       TfTokenVector const &renderTags,
                       std::unordered_set<SdfPath, SdfPath::Hash> const &dirtyPaths = {},
                       std::function<bool()> const &cancelled = {},
                       bool *rebuiltAll = nullptr,
                       bool *needsFullSnapshot = nullptr,
                       bool *topologyChanged = nullptr,
                       bool forceRebuild = false, bool deferCommit = false,
                       std::string *fallbackReason = nullptr)
{
    if (!snapshot || !snapshot->scene) return false;
    AccumulateMilliseconds timing{snapshot->lastPlacementReconcileMilliseconds};
    if (rebuiltAll) *rebuiltAll = false;
    if (needsFullSnapshot) *needsFullSnapshot = false;
    if (topologyChanged) *topologyChanged = false;
    auto fail = [&](char const *reason) {
        if (needsFullSnapshot) *needsFullSnapshot = true;
        if (rebuiltAll) *rebuiltAll = true;
        if (fallbackReason) *fallbackReason = reason;
        return false;
    };
    if (forceRebuild) return fail("placement topology requires coherent rebuild");
    auto commit = [&] {
        if (cancelled && cancelled()) return false;
        if (!deferCommit) {
            auto const start = std::chrono::steady_clock::now();
            rtcCommitScene(snapshot->scene->scene);
            ++snapshot->prototypePool->topLevelSceneCommits;
            snapshot->embreeSceneCommitSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
        }
        return true;
    };

    // Hydra array notices carry no element ranges. Compare captured values once,
    // then compose matrices only for changed source indices and their dependents.
    bool arrayOnly = !dirtyPaths.empty();
    std::unordered_set<unsigned int> changedIds;
    for (auto const &path : dirtyPaths) {
        auto old = snapshot->instancerInputs.find(path);
        auto current = index->GetPrim(path);
        std::unordered_set<int> sources;
        if (old == snapshot->instancerInputs.end() ||
            current.primType != HdPrimTypeTokens->instancer ||
            ![&] {
                AccumulateMilliseconds comparison{snapshot->lastInputComparisonMilliseconds};
                return _ChangedInstanceInputs(old->second, current.dataSource, &sources,
                    &snapshot->lastInstanceArrayElementsScanned);
            }()) {
            arrayOnly = false; break;
        }
        auto dependencies = snapshot->instanceSourcePlacements.find(path);
        if (dependencies == snapshot->instanceSourcePlacements.end()) continue;
        for (int source : sources)
            if (auto ids = dependencies->second.find(source); ids != dependencies->second.end())
                changedIds.insert(ids->second.begin(), ids->second.end());
    }
    if (arrayOnly) {
        std::vector<std::pair<unsigned int, GfMatrix4d>> matrices;
        matrices.reserve(changedIds.size());
        for (unsigned int id : changedIds) {
            if (cancelled && cancelled()) return false;
            auto const prim = snapshot->geomToPrimId.find(id);
            auto const chain = snapshot->geomToInstanceChain.find(id);
            if (prim == snapshot->geomToPrimId.end() || prim->second < 0 ||
                static_cast<size_t>(prim->second) >= snapshot->primPaths.size() ||
                chain == snapshot->geomToInstanceChain.end()) return fail("placement cache identity missing");
            GfMatrix4d matrix(1.0);
            if (!_PlacementTransformFromChain(index, snapshot->primPaths[prim->second], chain->second, &matrix))
                return fail("placement cache hierarchy changed");
            matrices.emplace_back(id, matrix);
        }
        for (auto const &[id, matrix] : matrices) {
            auto prototype = snapshot->prototypes.find(snapshot->primPaths[snapshot->geomToPrimId.at(id)]);
            if (prototype == snapshot->prototypes.end() || !rtcGetGeometry(snapshot->scene->scene, id))
                return fail("placement cache geometry missing");
            ++snapshot->lastPlacementRecordsVisited;
            _SetInstanceTransform(snapshot, id, matrix, prototype->second);
        }
        for (auto const &path : dirtyPaths)
            snapshot->instancerInputs[path] = index->GetPrim(path).dataSource;
        return commit();
    }

    if (snapshot->prototypeDependencies.empty()) _CachePlacementInputs(index, snapshot);
    std::unordered_set<SdfPath, SdfPath::Hash> prototypes;
    if (dirtyPaths.empty()) {
        for (auto const &[path, prototype] : snapshot->prototypes) prototypes.insert(path);
    } else {
        for (auto const &path : dirtyPaths) {
            auto dependencies = snapshot->prototypeDependencies.find(path);
            if (dependencies != snapshot->prototypeDependencies.end())
                prototypes.insert(dependencies->second.begin(), dependencies->second.end());
            if (snapshot->prototypes.contains(path)) prototypes.insert(path);
            auto affected = snapshot->placementDependencies.find(path);
            if (affected != snapshot->placementDependencies.end())
                for (unsigned int id : affected->second) {
                    auto prim = snapshot->geomToPrimId.find(id);
                    if (prim != snapshot->geomToPrimId.end() && prim->second >= 0 &&
                        static_cast<size_t>(prim->second) < snapshot->primPaths.size())
                        prototypes.insert(snapshot->primPaths[prim->second]);
                }
        }
    }

    bool changedTopology = false;
    for (auto const &path : prototypes) {
        if (cancelled && cancelled()) return false;
        auto prototype = snapshot->prototypes.find(path);
        // Evicted prototypes have logical records but no physical placements;
        // the residency pass restores them before updates when they become visible.
        if (prototype == snapshot->prototypes.end()) continue;
        auto const prim = index->GetPrim(path);
        auto placements = _InstancePlacements(index, path, prim.dataSource);
        if (placements.empty() && !HdInstancedBySchema::GetFromParent(prim.dataSource))
            placements.push_back({_WorldTransform(index, path), {}, -1, {}});
        std::unordered_set<unsigned int> oldIds, surviving;
        if (auto dependencies = snapshot->placementDependencies.find(path);
            dependencies != snapshot->placementDependencies.end())
            for (unsigned int id : dependencies->second) {
                auto existing = snapshot->geomToPrimId.find(id);
                if (existing != snapshot->geomToPrimId.end() && existing->second >= 0 &&
                    static_cast<size_t>(existing->second) < snapshot->primPaths.size() &&
                    snapshot->primPaths[existing->second] == path) oldIds.insert(id);
            }
        std::unordered_set<PlacementKey, PlacementKeyHash> desired;
        for (auto const &placement : placements)
            if (!desired.insert(_PlacementKey(path, placement.chain)).second)
                return fail("ambiguous duplicate placement identity");

        for (auto const &placement : placements) {
            if (cancelled && cancelled()) return false;
            auto key = _PlacementKey(path, placement.chain);
            auto match = snapshot->placementIds.find(key);
            unsigned int id;
            if (match != snapshot->placementIds.end() && oldIds.contains(match->second)) {
                id = match->second;
                if (!rtcGetGeometry(snapshot->scene->scene, id))
                    return fail("placement instance geometry missing");
                surviving.insert(id);
            } else {
                id = _AttachPrototype(snapshot, path, prototype->second, placement.transform);
                snapshot->geomToCoarseFaces[id] = prototype->second.coarseFaces;
                snapshot->placementIds[std::move(key)] = id;
                if (placement.instanceId >= 0) ++snapshot->instanceCount;
                ++snapshot->lastPlacementTopologyAttached;
                changedTopology = true;
            }
            ++snapshot->lastPlacementRecordsVisited;
            const int primId = _EnsurePrimId(snapshot, {path, placement.instancerPath});
            auto oldInstance = snapshot->geomToInstanceId.find(id);
            auto oldChain = snapshot->geomToInstanceChain.find(id);
            const bool membershipChanged = oldInstance == snapshot->geomToInstanceId.end() ||
                oldInstance->second != placement.instanceId ||
                oldChain == snapshot->geomToInstanceChain.end() || oldChain->second != placement.chain;
            const bool dependencyChanged = oldChain == snapshot->geomToInstanceChain.end() ||
                _PlacementKey(path, oldChain->second) != _PlacementKey(path, placement.chain);
            if (membershipChanged) {
                if (auto old = snapshot->geomToPrimId.find(id); old != snapshot->geomToPrimId.end())
                    snapshot->changedPickingPrims.insert(old->second);
                snapshot->changedPickingPrims.insert(primId);
                if (dependencyChanged) _IndexPlacementDependency(snapshot, id, false);
                changedTopology = true;
            }
            snapshot->geomToPrimId[id] = primId;
            snapshot->geomToInstanceId[id] = placement.instanceId;
            snapshot->geomToInstanceChain[id] = placement.chain;
            if (dependencyChanged) _IndexPlacementDependency(snapshot, id, true);
            _SetInstanceEnabled(snapshot, id, _PlacementEligible(
                index, path, prim.dataSource, placement.chain, renderTags));
            _SetInstanceTransform(snapshot, id, placement.transform, prototype->second);
        }
        for (unsigned int id : oldIds) {
            if (surviving.contains(id)) continue;
            if (snapshot->placementEnabled[id]) {
                --snapshot->enabledPlacementCount;
                _TrackPrototypeEnabled(snapshot, id, false);
            }
            if (snapshot->geomToInstanceId[id] >= 0) --snapshot->instanceCount;
            snapshot->changedPickingPrims.insert(snapshot->geomToPrimId.at(id));
            _IndexPlacementDependency(snapshot, id, false);
            rtcDetachGeometry(snapshot->scene->scene, id);
            snapshot->scene->geometry.erase(id);
            snapshot->scene->intersection_geometry.erase(id);
            snapshot->scene->placement_transforms.erase(id);
            snapshot->scene->placement_materials.erase(id);
            snapshot->geomToPrimId.erase(id);
            snapshot->geomToCoarseFaces.erase(id);
            snapshot->geomToInstanceId.erase(id);
            snapshot->geomToInstanceChain.erase(id);
            snapshot->placementEnabled.erase(id);
            snapshot->placementMatrices.erase(id);
            ++snapshot->lastPlacementTopologyDetached;
            changedTopology = true;
        }
    }
    if (changedTopology) {
        if (topologyChanged) *topologyChanged = true;
        _CachePlacementInputs(index, snapshot, &prototypes);
    } else {
        // Topology and dependency membership are unchanged. Retain the bank's
        // metadata and refresh only captured inputs named by this notice.
        for (auto const &path : dirtyPaths)
            if (snapshot->instancerInputs.contains(path))
                snapshot->instancerInputs[path] = index->GetPrim(path).dataSource;
    }
    return commit();
}

bool _GeometryLeafType(TfToken const &type) {
    return type == HdPrimTypeTokens->mesh || type == HdPrimTypeTokens->sphere;
}

bool _StructuralType(TfToken const &type)
{
    return type.IsEmpty() || type == TfToken("scope") || type == TfToken("xform") ||
        type == HdPrimTypeTokens->instancer || type == HdPrimTypeTokens->geomSubset ||
        _GeometryLeafType(type);
}

bool _EligibleStructuralChanges(HdSceneIndexBaseRefPtr const &oldScene,
    HdSceneIndexBaseRefPtr const &newScene, std::vector<StructuralChange> const &changes,
    bool sphereBatching, std::string *reason,
    std::unordered_set<SdfPath,SdfPath::Hash> const *nativeRoots = nullptr)
{
    (void)nativeRoots;
    auto fail = [&](char const *why) { if (reason) *reason = why; return false; };
    if (changes.empty()) return fail("untyped structural notice");
    for (auto const &change : changes) {
        if (change.operation == StructuralChange::Operation::Unknown)
            return fail("structural unknown operation");
        if (change.operation == StructuralChange::Operation::Rename &&
            (change.newPath.IsEmpty() || change.newPath == SdfPath::AbsoluteRootPath() ||
             change.newPath.HasPrefix(change.path) || change.path.HasPrefix(change.newPath)))
            return fail("structural rename lacks a valid old/new path pair");
        if ((change.operation == StructuralChange::Operation::Remove ||
             change.operation == StructuralChange::Operation::Replace) &&
            change.previousType.IsEmpty() && !change.previousHadChildren)
            return fail("structural previous type is unknown");
        if (change.path.IsEmpty() || change.path == SdfPath::AbsoluteRootPath())
            return fail("structural root edit");
        if (!_StructuralType(change.previousType) || !_StructuralType(change.newType))
            return fail("structural unsupported prim type");
        if (!change.previousType.IsEmpty() && !change.newType.IsEmpty() &&
            change.previousType != change.newType &&
            !(_GeometryLeafType(change.previousType) && _GeometryLeafType(change.newType)))
            return fail("structural unsupported primitive transition");
        for (auto const &scene : {oldScene, newScene}) {
            if (!scene) continue;
            std::vector<SdfPath> pending{change.path};
            if (change.operation == StructuralChange::Operation::Rename) pending.push_back(change.newPath);
            while (!pending.empty()) {
                const auto path = pending.back(); pending.pop_back();
                const auto prim = scene->GetPrim(path);
                if (!_StructuralType(prim.primType))
                    return fail("structural subtree contains unsupported prim type");
                if (sphereBatching && prim.primType == HdPrimTypeTokens->sphere &&
                    _WorldVisible(scene,path)) {
                    Float3 center; float scale;
                    if (_BatchableSphereTransform(_WorldTransform(scene,path),&center,&scale))
                        return fail("structural sphere batch membership requires full rebuild");
                }
                const auto children = scene->GetChildPrimPaths(path);
                pending.insert(pending.end(),children.begin(),children.end());
            }
        }
        if (change.previousType == HdPrimTypeTokens->geomSubset ||
            change.newType == HdPrimTypeTokens->geomSubset) {
            const auto parent = change.path.GetParentPath();
            const bool parentTransaction = std::any_of(changes.begin(),changes.end(),
                [&](auto const &other) { return parent.HasPrefix(other.path) && other.path != change.path; });
            const auto parentType = newScene->GetPrim(parent).primType;
            if (parentType != HdPrimTypeTokens->mesh && (!parentTransaction || !parentType.IsEmpty()))
                return fail("structural subset parent missing or unsupported");
        }
    }
    return true;
}

void _RefreshMaterials(HdSceneIndexBaseRefPtr const &index, MaterialService *materials,
    Snapshot *snapshot, std::unordered_set<SdfPath, SdfPath::Hash> const &paths);

bool _RefreshPrototypes(HdSceneIndexBaseRefPtr const &index,
    std::unordered_set<SdfPath, SdfPath::Hash> const &paths, MaterialService *materials,
    Snapshot *snapshot, TfTokenVector const &renderTags,
    std::vector<std::shared_ptr<PrototypeStorage>> *displacedStorage);

bool _RekeyStructuralRenames(HdSceneIndexBaseRefPtr const &index,
    std::vector<StructuralChange> const &changes, Snapshot *snapshot,
    MaterialService *materials, TfTokenVector const &tags,
    std::vector<std::shared_ptr<PrototypeStorage>> *retired,
    std::function<bool()> const &cancel, bool preflightOnly = false)
{
    if (std::none_of(changes.begin(),changes.end(),[](auto const &change) {
            return change.operation == StructuralChange::Operation::Rename;
        })) return true;
    auto remapPath = [&](SdfPath path) {
        for (auto const &change : changes)
            if (change.operation == StructuralChange::Operation::Rename && path.HasPrefix(change.path))
                path = path.ReplacePrefix(change.path,change.newPath);
        return path;
    };
    struct IdentityMove { PrimKey oldKey, newKey; int oldId; };
    std::vector<IdentityMove> moves;
    std::unordered_set<PrimKey,PrimKeyHash> destinations;
    std::unordered_set<SdfPath,SdfPath::Hash> movedPrototypes, affected;
    // Preflight every identity and destination before changing the inactive bank.
    for (auto const &[key,id] : snapshot->primIds) {
        PrimKey destination{remapPath(key.path),remapPath(key.instancerPath)};
        if (!destinations.insert(destination).second) return false;
        if (destination != key) moves.push_back({key,destination,id});
    }
    std::unordered_set<SdfPath,SdfPath::Hash> prototypeDestinations;
    for (auto const &[path,prototype] : snapshot->prototypes) {
        const auto destination = remapPath(path);
        if (!prototypeDestinations.insert(destination).second) return false;
        if (destination == path) continue;
        if (!_GeometryLeafType(index->GetPrim(destination).primType)) return false;
        movedPrototypes.insert(destination);
    }
    if (preflightOnly || (moves.empty() && movedPrototypes.empty())) return true;
    if (cancel && cancel()) return false;
    for (auto const &move : moves) {
        snapshot->primIds.erase(move.oldKey);
        snapshot->primPaths[move.oldId] = {};
        snapshot->instancerPaths[move.oldId] = {};
        affected.insert(move.newKey.path);
    }
    snapshot->primIdentityDirty = true;
    std::unordered_map<int,int> remapIds;
    for (auto const &move : moves)
        remapIds.emplace(move.oldId,_EnsurePrimId(snapshot,move.newKey));
    for (auto &[id,primId] : snapshot->geomToPrimId)
        if (auto found = remapIds.find(primId); found != remapIds.end()) primId = found->second;
    for (auto &[id,chain] : snapshot->geomToInstanceChain)
        for (auto &level : chain) {
            const auto destination = remapPath(level.instancerPath);
            if (destination != level.instancerPath) {
                level.instancerPath = destination;
                affected.insert(snapshot->primPaths[snapshot->geomToPrimId.at(id)]);
            }
        }
    auto rekeyMap = [&](auto &map) {
        auto previous = std::move(map);
        map.clear();
        for (auto &[path,value] : previous) map.emplace(remapPath(path),std::move(value));
    };
    rekeyMap(snapshot->prototypes);
    rekeyMap(snapshot->enabledPrototypePlacements);
    rekeyMap(snapshot->displayMaterialsByPath);
    rekeyMap(snapshot->resolvedMaterials);
    rekeyMap(snapshot->prototypeMaterials);
    rekeyMap(snapshot->materialUsers);
    auto rekeySet = [&](auto &paths) {
        auto previous = std::move(paths); paths.clear();
        for (auto const &path : previous) paths.insert(remapPath(path));
    };
    for (auto &[path,users] : snapshot->materialUsers) rekeySet(users);
    for (auto &[path,materials] : snapshot->prototypeMaterials) rekeySet(materials);
    rekeySet(snapshot->evictedPrototypes);
    {
        std::lock_guard lock(snapshot->prototypePool->mutex);
        rekeySet(snapshot->prototypePool->evictedPaths);
        for (auto const &path : snapshot->evictedPrototypes)
            snapshot->prototypePool->evictedPaths.insert(path);
    }
    for (auto &[hash,paths] : snapshot->prototypeHashBuckets)
        for (auto &path : paths) path = remapPath(path);
    _IndexPlacementDependencies(snapshot);
    _CachePlacementInputs(index,snapshot);
    // Conversion compatibility uses the existing pool's exact geometry check;
    // compatible storage and Embree placement handles survive the path move.
    if (!movedPrototypes.empty() && !_RefreshPrototypes(index,movedPrototypes,
            materials,snapshot,tags,retired)) return false;
    affected.insert(movedPrototypes.begin(),movedPrototypes.end());
    if (!affected.empty()) {
        _RefreshMaterials(index,materials,snapshot,affected);
        if (!_UpdatePlacements(index,snapshot,tags,affected,cancel,
                nullptr,nullptr,nullptr,false,true)) return false;
    }
    snapshot->lastStructuralPathsUpdated += moves.size();
    return true;
}

bool _UpdateStructuralLeaves(HdSceneIndexBaseRefPtr const &retained,
    std::vector<StructuralChange> const &changes, MaterialService *materials,
    Snapshot *snapshot, TfTokenVector const &tags,
    std::vector<std::shared_ptr<PrototypeStorage>> *retired, std::function<bool()> const &cancel)
{
    std::unordered_set<SdfPath, SdfPath::Hash> paths;
    std::unordered_set<SdfPath, SdfPath::Hash> subsetParents;
    std::unordered_set<SdfPath, SdfPath::Hash> externalPrototypes;
    const bool hasRenames = std::any_of(changes.begin(),changes.end(),[](auto const &change) {
        return change.operation == StructuralChange::Operation::Rename;
    });
    if (hasRenames) {
        if (!_RekeyStructuralRenames(retained,changes,snapshot,materials,tags,retired,cancel)) return false;
        // Bank replay can combine a cold rename with a later visibility edit.
        // The pre-transaction restore saw the old path; resolve eligibility
        // again after rekeying, against the final captured destination.
        _RestoreEvictedPrototypes(retained,materials,snapshot,tags,cancel);
        if (cancel && cancel()) return false;
    }
    for (auto const &change : changes) {
        const bool rename = change.operation == StructuralChange::Operation::Rename;
        auto root = rename ? change.newPath : change.path;
        if (rename) for (auto const &later : changes)
            if (later.operation == StructuralChange::Operation::Rename && root.HasPrefix(later.path))
                root = root.ReplacePrefix(later.path,later.newPath);
        if (!rename) {
            if (auto deps = snapshot->prototypeDependencies.find(root);
                deps != snapshot->prototypeDependencies.end())
                externalPrototypes.insert(deps->second.begin(),deps->second.end());
        }
        // Removed descendants no longer exist in the captured index. Their
        // retained identity records are the authoritative removal inventory.
        if (!rename) {
            for (auto const &[key,id] : snapshot->primIds)
                if (key.path.HasPrefix(root)) paths.insert(key.path);
            for (auto const &[path,prototype] : snapshot->prototypes)
                if (path.HasPrefix(root)) paths.insert(path);
            for (auto const &path : snapshot->evictedPrototypes)
                if (path.HasPrefix(root)) paths.insert(path);
        }
        std::vector<SdfPath> pending{root};
        while (!pending.empty()) {
            const auto path = pending.back(); pending.pop_back();
            const auto prim = retained->GetPrim(path);
            if (_GeometryLeafType(prim.primType) && (!rename ||
                (!snapshot->prototypes.contains(path) && !snapshot->evictedPrototypes.contains(path))))
                paths.insert(path);
            if (prim.primType == HdPrimTypeTokens->geomSubset)
                subsetParents.insert(path.GetParentPath());
            const auto children = retained->GetChildPrimPaths(path);
            pending.insert(pending.end(),children.begin(),children.end());
        }
        if (change.previousType == HdPrimTypeTokens->geomSubset ||
            change.newType == HdPrimTypeTokens->geomSubset)
        {
            subsetParents.insert(root.GetParentPath());
            ++snapshot->lastStructuralPathsUpdated;
        }
    }
    for (auto const &path : paths) externalPrototypes.erase(path);
    for (auto const &path : paths) {
        if (cancel && cancel()) return false;
        ++snapshot->lastStructuralPathsUpdated;
        snapshot->evictedPrototypes.erase(path);
        {
            std::lock_guard lock(snapshot->prototypePool->mutex);
            snapshot->prototypePool->evictedPaths.erase(path);
        }
        std::vector<PrimKey> removedKeys;
        for (auto const &[key, id] : snapshot->primIds)
            if (key.path == path) removedKeys.push_back(key);
        std::vector<unsigned int> ids;
        if (auto deps = snapshot->placementDependencies.find(path); deps != snapshot->placementDependencies.end())
            for (unsigned int id : deps->second) {
                auto found = snapshot->geomToPrimId.find(id);
                if (found != snapshot->geomToPrimId.end() && found->second >= 0 &&
                    snapshot->primPaths[found->second] == path) ids.push_back(id);
            }
        if (snapshot->sphereBatchLocations.contains(path)) return false;
        for (unsigned int id : ids) {
            _IndexPlacementDependency(snapshot, id, false);
            if (snapshot->geomToInstanceId[id] >= 0) --snapshot->instanceCount;
            rtcDetachGeometry(snapshot->scene->scene, id);
            if (snapshot->placementEnabled.contains(id) && snapshot->placementEnabled[id]) {
                --snapshot->enabledPlacementCount;
                _TrackPrototypeEnabled(snapshot, id, false);
            }
            snapshot->scene->geometry.erase(id); snapshot->scene->intersection_geometry.erase(id);
            snapshot->scene->placement_materials.erase(id); snapshot->scene->placement_transforms.erase(id);
            snapshot->geomToPrimId.erase(id); snapshot->geomToCoarseFaces.erase(id);
            snapshot->geomToInstanceId.erase(id); snapshot->geomToInstanceChain.erase(id);
            snapshot->placementEnabled.erase(id); snapshot->placementMatrices.erase(id);
            for (SdfPath ancestor = path; !ancestor.IsEmpty() && ancestor != SdfPath::AbsoluteRootPath();
                 ancestor = ancestor.GetParentPath()) {
                auto dep = snapshot->placementDependencies.find(ancestor);
                if (dep != snapshot->placementDependencies.end()) {
                    dep->second.erase(id);
                    if (dep->second.empty()) snapshot->placementDependencies.erase(dep);
                }
            }
            ++snapshot->lastStructuralPlacementsDetached;
            ++snapshot->prototypePool->structuralPlacementDetachCalls;
        }
        for (auto const &key : removedKeys) {
            const int primId = snapshot->primIds.at(key);
            snapshot->primPaths[primId] = {}; snapshot->instancerPaths[primId] = {};
            snapshot->primIds.erase(key);
            snapshot->primIdentityDirty = true;
        }
        auto prototype = snapshot->prototypes.find(path);
        if (prototype != snapshot->prototypes.end()) {
            auto storage = prototype->second.storage;
            const auto hash = prototype->second.geometryHash;
            retired->push_back(storage);
            snapshot->prototypes.erase(prototype);
            auto bucket = snapshot->prototypeHashBuckets.find(hash);
            if (bucket != snapshot->prototypeHashBuckets.end()) {
                std::erase(bucket->second,path);
                if (bucket->second.empty()) snapshot->prototypeHashBuckets.erase(bucket);
            }
            const bool shared = std::any_of(snapshot->prototypes.begin(),snapshot->prototypes.end(),
                [&](auto const &item) { return item.second.storage == storage; });
            if (!shared) {
                std::erase_if(snapshot->geometries,[&](auto const &geometry) { return geometry.get() == storage->geometry.get(); });
                size_t bytes = 4*sizeof(float);
                if (auto mesh = dynamic_cast<Mesh *>(storage->geometry.get()))
                    bytes = mesh->vertices.size()*sizeof(Float3)+mesh->normals.size()*sizeof(Float3)+
                        mesh->texcoords.size()*sizeof(Float2)+mesh->indices.size()*sizeof(unsigned int)+
                        mesh->tangents.size()*sizeof(float);
                snapshot->estimatedGeometryBytes -= std::min(snapshot->estimatedGeometryBytes,bytes);
            }
        }
        if (auto users = snapshot->prototypeMaterials.find(path); users != snapshot->prototypeMaterials.end()) {
            for (auto const &material : users->second) {
                auto reverse = snapshot->materialUsers.find(material);
                if (reverse != snapshot->materialUsers.end()) {
                    reverse->second.erase(path);
                    if (reverse->second.empty()) snapshot->materialUsers.erase(reverse);
                }
            }
            snapshot->prototypeMaterials.erase(users);
        }
        snapshot->displayMaterialsByPath.erase(path);
        const auto prim = retained->GetPrim(path);
        const size_t before = snapshot->geomToPrimId.size();
        if (_GeometryLeafType(prim.primType))
            _Walk(retained, path, materials, snapshot, tags, cancel, nullptr, false);
        const size_t attached = snapshot->geomToPrimId.size()-before;
        snapshot->lastStructuralPlacementsAttached += attached;

    }
    if (!subsetParents.empty()) _RefreshMaterials(retained, materials, snapshot, subsetParents);
    _IndexPlacementDependencies(snapshot);
    _CachePlacementInputs(retained, snapshot);
    if (!externalPrototypes.empty() && !_UpdatePlacements(retained,snapshot,tags,
            externalPrototypes,cancel,nullptr,nullptr,nullptr,false,true)) return false;
    return true;
}

bool _RefreshPrototypes(HdSceneIndexBaseRefPtr const &index,
                        std::unordered_set<SdfPath, SdfPath::Hash> const &paths,
                        MaterialService *materials, Snapshot *snapshot,
                        TfTokenVector const &renderTags,
                        std::vector<std::shared_ptr<PrototypeStorage>> *displacedStorage)
{
    if (!snapshot || !snapshot->scene || paths.empty()) return false;
    for (SdfPath const &path : paths) {
        // The retained source has already been recaptured. Keep cold geometry
        // cold until its first eligible placement, then convert the latest data.
        if (snapshot->evictedPrototypes.contains(path)) continue;
        std::vector<unsigned int> dependentIds;
        if (auto dependencies = snapshot->placementDependencies.find(path);
            dependencies != snapshot->placementDependencies.end())
            for (unsigned int id : dependencies->second) {
                auto prim = snapshot->geomToPrimId.find(id);
                if (prim != snapshot->geomToPrimId.end() && prim->second >= 0 &&
                    static_cast<size_t>(prim->second) < snapshot->primPaths.size() &&
                    snapshot->primPaths[prim->second] == path) dependentIds.push_back(id);
            }
        RTCGeometryType oldType = RTC_GEOMETRY_TYPE_INSTANCE;
        // displayColor is a material input carried by a geometry primvar.  A
        // prototype refresh must recapture its value instead of reusing the
        // path cache populated by the previous data source.
        snapshot->displayMaterialsByPath.erase(path);
        _RemovePrototypeMaterialUsers(snapshot, path);
        auto found = snapshot->prototypes.find(path);
        if (found != snapshot->prototypes.end()) {
            Geometry *const oldGeometry = found->second.geometry;
            oldType = oldGeometry->type;
            displacedStorage->push_back(found->second.storage);
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
            prim.primType != HdPrimTypeTokens->sphere) {
            if (oldType != RTC_GEOMETRY_TYPE_INSTANCE || !dependentIds.empty()) return false;
            continue;
        }
        if (prim.primType == HdPrimTypeTokens->mesh)
            _AppendMesh(index, path, prim, materials, snapshot,
                _WorldTransform(index, path), {}, -1, {}, renderTags, nullptr, false);
        else
            _AppendSphere(index, path, prim, materials, snapshot,
                _WorldTransform(index, path), {}, -1, {}, renderTags, false);
        auto replacement = snapshot->prototypes.find(path);
        if (replacement == snapshot->prototypes.end() ||
            oldType == RTC_GEOMETRY_TYPE_INSTANCE ||
            (oldType != RTC_GEOMETRY_TYPE_INSTANCE && oldType != replacement->second.geometry->type))
            return false;
        ++snapshot->lastPrototypePathsUpdated;
        for (unsigned int id : dependentIds) {
            RTCGeometry instance = rtcGetGeometry(snapshot->scene->scene, id);
            if (!instance) return false;
            rtcSetGeometryInstancedScene(instance, replacement->second.scene);
            rtcCommitGeometry(instance);
            snapshot->scene->geometry[id] = replacement->second.geometry;
            snapshot->scene->intersection_geometry[id] = replacement->second.geometry->geom;
            snapshot->scene->placement_materials[id] = replacement->second.binding;
            snapshot->geomToCoarseFaces[id] = replacement->second.coarseFaces;
            ++snapshot->lastPrototypePlacementsRepointed;
        }
    }
    return true;
}

// The light/shadow-link categories of every placement, from the categories
// HdsiLightLinkingSceneIndex gives geometry.
void _ApplyLinkMasks(HdSceneIndexBaseRefPtr const &index, Snapshot *snapshot)
{
    SceneManager &scene = *snapshot->scene;
    scene.placement_link_masks.clear();
    scene.lightLinking = !snapshot->lightLinkBits.empty() || !snapshot->shadowLinkBits.empty();
    if (!scene.lightLinking) return;
    std::unordered_map<SdfPath, std::pair<uint32_t, uint32_t>, SdfPath::Hash> byPath;
    for (auto const &[id, primId] : snapshot->geomToPrimId) {
        if (primId < 0 || size_t(primId) >= snapshot->primPaths.size()) continue;
        SdfPath const &path = snapshot->primPaths[primId];
        auto cached = byPath.find(path);
        if (cached == byPath.end()) {
            std::pair<uint32_t, uint32_t> masks{0u, 0u};
            HdCategoriesSchema categories = HdCategoriesSchema::GetFromParent(index->GetPrim(path).dataSource);
            if (categories) {
                for (auto const &[category, bit] : snapshot->lightLinkBits)
                    if (categories.IsIncludedInCategory(category)) masks.first |= bit;
                for (auto const &[category, bit] : snapshot->shadowLinkBits)
                    if (categories.IsIncludedInCategory(category)) masks.second |= bit;
            }
            cached = byPath.emplace(path, masks).first;
        }
        scene.placement_link_masks[int(id)] = cached->second;
    }
}

void _RefreshEnvironment(HdSceneIndexBaseRefPtr const &index,
                         SdfPath const &path, Snapshot *snapshot)
{
    HdSceneIndexPrim const prim = index->GetPrim(path);
    if (prim.primType == HdPrimTypeTokens->domeLight)
        _SetDomeEnvironment(index, path, prim, snapshot);
    else if (HdPrimTypeIsLight(prim.primType) && HdLightSchema::GetFromParent(prim.dataSource))
        _AddAnalyticLight(index, path, prim, snapshot);
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
        snapshot->displayMaterialsByPath.erase(path);
        Material *base = _BaseMaterial(index, path, prim, materials, snapshot);
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


class PublishedSnapshotPickScene final : public PublishedPickScene {
public:
    PublishedSnapshotPickScene(std::shared_ptr<const Snapshot> snapshot,
        CameraState const& camera, unsigned int width, unsigned int height,
        std::shared_ptr<prism::device::RenderDevice> renderDevice)
        : snapshot(std::move(snapshot)), camera(camera),
          parameters(_CameraParameters(camera)), width(width), height(height),
          device(this->snapshot->device), renderDevice(std::move(renderDevice))
    { rtcRetainDevice(device); }
    ~PublishedSnapshotPickScene() override {
        // Release all Embree scenes/geometries before the explicit device pin.
        snapshot.reset();
        rtcReleaseDevice(device);
    }
    bool PickPixel(unsigned int x, unsigned int y, PublishedPickHit* result) const override {
        if (!result || x>=width || y>=height) return false;
        const RTCRay ray=prism::MakeCameraRayWithLens(parameters,
            static_cast<float>(x)+.5f,static_cast<float>(y)+.5f,width,height,nullptr);
        RTCRayHit raw=renderDevice->Intersect(*snapshot->scene, prism::PathRay{ray,{}});
        if(raw.hit.geomID==RTC_INVALID_GEOMETRY_ID) return false;
        const unsigned int placementId=raw.hit.instID[0]!=RTC_INVALID_GEOMETRY_ID
            ? raw.hit.instID[0] : raw.hit.geomID;
        auto geometry=snapshot->scene->geometry.find(placementId);
        if(geometry==snapshot->scene->geometry.end()) return false;
        Geometry* geom=geometry->second;
        RTCGeometry interpolation=geom->geom;
        if(auto leaf=snapshot->scene->intersection_geometry.find(placementId);
           leaf!=snapshot->scene->intersection_geometry.end()) interpolation=leaf->second;
        const auto placement=snapshot->scene->placement_transforms.find(placementId);
        Float3 localAnalyticNormal;
        Float3 const* analyticNormal=nullptr;
        const auto transform=[](Float3 const& v,std::array<float,9> const& m) {
            return Float3(v.x()*m[0]+v.y()*m[3]+v.z()*m[6],
                          v.x()*m[1]+v.y()*m[4]+v.z()*m[7],
                          v.x()*m[2]+v.y()*m[5]+v.z()*m[8]);
        };
        if(placement!=snapshot->scene->placement_transforms.end()) {
            Float3 normal(raw.hit.Ng_x,raw.hit.Ng_y,raw.hit.Ng_z);
            if(geom->type==RTC_GEOMETRY_TYPE_SPHERE_POINT) {
                localAnalyticNormal=normal; analyticNormal=&localAnalyticNormal;
            }
            normal=transform(normal,placement->second.normal);
            if(placement->second.flipOrientation ||
               (placement->second.mirrored && geom->type==RTC_GEOMETRY_TYPE_TRIANGLE))
                normal=-normal;
            raw.hit.Ng_x=normal.x(); raw.hit.Ng_y=normal.y(); raw.hit.Ng_z=normal.z();
        }
        RayHit hit(raw,&interpolation,geom->type,analyticNormal);
        hit.geomid=static_cast<int>(placementId);
        if(placement!=snapshot->scene->placement_transforms.end()) {
            if(geom->type==RTC_GEOMETRY_TYPE_TRIANGLE)
                hit.n=transform(hit.n,placement->second.normal).normalize();
            if(placement->second.flipOrientation ||
               (placement->second.mirrored && geom->type==RTC_GEOMETRY_TYPE_TRIANGLE))
                hit.n=-hit.n;
        }
        result->primId=_PrimIdForHit(snapshot.get(),hit);
        result->instanceId=-1; result->elementId=-1;
        if(auto instance=snapshot->geomToInstanceId.find(placementId);
           instance!=snapshot->geomToInstanceId.end()) result->instanceId=instance->second;
        if(auto faces=snapshot->geomToCoarseFaces.find(placementId);
           faces!=snapshot->geomToCoarseFaces.end() && hit.primid>=0 &&
           static_cast<size_t>(hit.primid)<faces->second.size())
            result->elementId=faces->second[hit.primid];
        result->worldPoint=GfVec3d(hit.p.x(),hit.p.y(),hit.p.z());
        result->worldNormal=GfVec3f(hit.n.x(),hit.n.y(),hit.n.z());
        GfVec4d clip(hit.p.x(),hit.p.y(),hit.p.z(),1.0);
        clip=clip*camera.worldToView*camera.projection;
        if(std::abs(clip[3])<=1e-12) return false;
        result->normalizedDepth=static_cast<float>(.5*clip[2]/clip[3]+.5);
        return result->primId>=0;
    }
private:
    std::shared_ptr<const Snapshot> snapshot;
    CameraState camera;
    prism::CameraRayParameters parameters;
    unsigned int width,height;
    RTCDevice device;
    std::shared_ptr<prism::device::RenderDevice> renderDevice;
};

struct PreviewResult {
    unsigned int width = 0;
    unsigned int height = 0;
    bool published = false;
    double publicationMilliseconds = 0.0;
    // Tracing time without scene preparation (GPU devices upload the scene
    // on their first render).
    double traceMilliseconds = 0.0;
    uint64_t publishedBytes = 0;
    uint64_t tracedPixels = 0;
    uint64_t boundAovCount = 0;
};

using prism::output::AovKind;

AovKind _AovKind(TfToken const &name)
{
    return prism::output::AovKindFromName(name.GetString());
}

bool _IntegerAov(AovKind kind) { return prism::output::IntegerAov(kind); }

size_t _AovComponents(AovKind kind)
{
    return static_cast<size_t>(prism::output::AovComponents(kind));
}

// Convert while the center-ray hit is local; only requested final outputs are
// retained. Preview and full resolution deliberately share these semantics.
void _ResolvePrimaryAov(AovKind kind, RayHit const &hit, CameraState const &camera,
                       Snapshot const *snapshot, float *values, int *id)
{
    const bool valid = hit.geomid != RTC_INVALID_GEOMETRY_ID;
    if (_IntegerAov(kind)) {
        *id = -1;
        if (!valid) return;
        if (kind == AovKind::PrimId) *id = _PrimIdForHit(snapshot, hit);
        else if (kind == AovKind::InstanceId) {
            auto it = snapshot->geomToInstanceId.find(hit.geomid);
            if (it != snapshot->geomToInstanceId.end()) *id = it->second;
        } else {
            auto it = snapshot->geomToCoarseFaces.find(hit.geomid);
            if (it != snapshot->geomToCoarseFaces.end() && hit.primid >= 0 &&
                static_cast<size_t>(hit.primid) < it->second.size()) *id = it->second[hit.primid];
        }
    } else {
        prism::output::ResolveGeometricAov(kind, hit, camera.worldToView, camera.projection, values);
    }
}

PreviewResult _RenderPreview(std::shared_ptr<Snapshot> const &snapshot,
    CameraState const &camera, RenderSettings const &settings,
    HdRenderPassAovBindingVector const &bindings,
    unsigned int width, unsigned int height, HdRenderThread *renderThread,
    std::shared_ptr<const PublishedFrame> const &frame,
    std::atomic<uint64_t> &colorTracedPixels,
    prism::device::RenderDevice &renderDevice)
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
        AovKind kind = AovKind::Unknown;
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
        target.kind = _AovKind(binding.aovName);
        if (binding.aovName == HdAovTokens->color) {
            target.components = 4; target.floats.resize(pixels * 4);
        } else if (binding.aovName == HdAovTokens->normal ||
                   binding.aovName == HdAovTokens->Neye ||
                   binding.aovName == HdAovTokens->Peye ||
                   binding.aovName == TfToken("primvars:st")) {
            target.components = binding.aovName == TfToken("primvars:st") ? 2 : 3;
            target.floats.resize(pixels * target.components);
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
            _WarnUnsupportedAov(binding.aovName);
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

    prism::device::PreviewRequest request;
    request.camera = _CameraParameters(camera);
    request.width = width;
    request.height = height;
    request.previewWidth = result.width;
    request.previewHeight = result.height;
    request.seed = static_cast<unsigned int>(settings.seed);
    request.shading.sampleClamp = settings.sampleClamp;
    request.shading.nee = {settings.neeProbabilityMin, settings.neeFromDepth, settings.spatialLights};
    request.shading.exposureScale = camera.linearExposureScale;
    request.shading.transparentEscapes =
        snapshot->domeCount == 0 || !settings.domeLightCameraVisibility;
    request.tracePrimary = needPrimary;
    request.colorPixels = &colorTracedPixels;
    request.stopRequested = [renderThread] {
        return renderThread && renderThread->IsStopRequested();
    };
    request.traceMilliseconds = &result.traceMilliseconds;
    request.writePixel = [&](unsigned int px, unsigned int py, Color3 const &color,
                             float coverage, RayHit const *primary) {
        const size_t pixel =
            (static_cast<size_t>(result.height - 1 - py) * result.width + px);
        for (Target &target : targets) {
            if (target.kind == AovKind::Color) {
                float *dst = target.floats.data() + pixel * 4;
                dst[0] = color.x(); dst[1] = color.y();
                dst[2] = color.z(); dst[3] = coverage;
                continue;
            }
            if (!primary) continue;
            _ResolvePrimaryAov(target.kind, *primary, camera, snapshot.get(),
                target.floats.empty() ? nullptr : target.floats.data() + pixel * target.components,
                target.ints.empty() ? nullptr : target.ints.data() + pixel);
        }
    };
    if (!renderDevice.RenderPreview(*snapshot->scene, request)) return result;
    auto const publicationStart = std::chrono::steady_clock::now();
    for (Target const &target : targets) {
        if (!target.ints.empty())
            target.buffer->PublishPreviewInt(result.width, result.height,
                                              target.ints, target.components, frame);
        else
            target.buffer->PublishPreviewFloat(result.width, result.height,
                                                target.floats, target.components, frame,
                                                target.kind == AovKind::Color);
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
        std::vector<StructuralChange> structuralChanges;
        bool structuralEligible = false;
        std::string structuralReason;
    };
    struct BuildResult {
        uint64_t generation = 0;
        std::shared_ptr<Snapshot> snapshot;
        std::shared_ptr<Snapshot> spareSnapshot;
        std::unique_ptr<MaterialService> materialService;
        double seconds = 0.0;
        std::string error;
        // The build ran out of memory; rebuilding the same scene would too.
        bool outOfMemory = false;
        bool reusedBank = false;
        bool residencyUnchanged = false;
        bool pinnedBankFallback = false;
        bool visibilityRecoveryRebuild = false;
        bool sphereBatchFallback = false;
        bool structuralLeafUpdate = false;
        bool structuralFallback = false;
        CanonicalPrimIds canonicalSeed;
        bool prototypeUpdateFallback = false;
        bool placementTopologyFallback = false;
        std::string updateFallbackReason;
        uint64_t prototypeBuilds = 0;
        // A rebuilt snapshot is published before its spare bank exists; the
        // spare job reports its bank in spareSnapshot with spare set.
        bool needsSpare = false;
        bool spare = false;
        // Set only when the spare bank was built to the end. A cancel raised
        // after that point does not make the bank any less complete.
        bool spareComplete = false;
    };

    Impl(HdSceneIndexBaseRefPtr index, HdRenderThread *thread)
        : sceneIndex(std::move(index)), renderThread(thread), device(rtcNewDevice(nullptr))
    {
        if (device)
            rtcSetDeviceMemoryMonitorFunction(device, &Impl::MemoryMonitor, this);
        // PRISM_DEVICE selects the initial device; a prism:device render setting
        // overrides it.
        if (const char *value = std::getenv("PRISM_DEVICE"); value && *value) {
            if (!prism::device::ParseDevicePreference(value, &settings.device))
                TF_WARN("PrismRT: ignoring PRISM_DEVICE=%s (expected auto, cpu, cpu-reference, vulkan or metal)", value);
        }
        // PRISM_GPU_PREPARATION_PREVIEW=0/1 sets the initial value of
        // prism:gpuPreparationPreview, which a render setting overrides.
        if (const char *value = std::getenv("PRISM_GPU_PREPARATION_PREVIEW"); value && *value)
            settings.gpuPreparationPreview = std::strcmp(value, "0") != 0;
        if (const char *value = std::getenv("PRISM_GPU_PREPARATION_PREVIEW_MAX_MIB"); value && *value)
            settings.gpuPreparationPreviewMaxMiB = std::max(0, std::atoi(value));
        if (const char *value = std::getenv("PRISM_GPU_PREPARATION_PREVIEW_DELAY_MS"); value && *value)
            gpuPreviewDelayMs = std::max(0, std::atoi(value));
        if (const char *value = std::getenv("PRISM_GPU_PREPARATION_PREVIEW_THREADS"); value && *value)
            gpuPreviewThreads = std::max(0, std::atoi(value));
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
                // Overlaps scene capture; the build job's services share its
                // key, so their first Prefetch waits for it instead.
                materialService->Prewarm();
            }
        } catch (std::exception const &e) {
            TF_WARN("PrismRT: MDL initialization failed: %s", e.what());
        }
    }

    // Waits for a device job, returning false (and leaving the job running)
    // as soon as the host asks the render worker to stop.
    template <class T>
    bool WaitForDeviceJob(std::future<T> &job) const
    {
        while (job.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
            if (renderThread->IsStopRequested()) return false;
        return true;
    }

    // Loading is active from the first scene edit until its snapshot is
    // published (a spare bank job does not count). Call with mutex held.
    void RefreshLoadingPhaseLocked()
    {
        progress.SetActive(prism::Phase::Loading, sceneDirty || captureActive ||
            (buildFuture.valid() && !buildIsSpare));
    }

    // Describes a created device for the host; `note` is CreateDevice's
    // explanation when it fell back to the CPU.
    static std::string DescribeDevice(prism::device::RenderDevice const &device,
        prism::device::DevicePreference preference, std::string const &note)
    {
        const auto info = device.Info();
        std::string name = info.name;
        for (std::string_view suffix : {" (Vulkan)", " (Metal)"})
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                name.resize(name.size() - suffix.size());
        if (info.kind == prism::device::DeviceKind::CpuReference) return "cpu-reference";
        if (info.kind != prism::device::DeviceKind::Cpu)
            return std::string(prism::device::ToString(info.kind)) + ": " + name;
        if (note.empty()) return "cpu";
        // "no GPU device available (vulkan: reason); using the CPU device"
        std::string reason = note;
        const auto open = note.find('('), close = note.rfind(");");
        if (open != std::string::npos && close != std::string::npos && close > open)
            reason = note.substr(open + 1, close - open - 1);
        std::string kind = prism::device::ToString(preference);
        if (reason.compare(0, kind.size() + 2, kind + ": ") == 0) reason.erase(0, kind.size() + 2);
        if (kind == "auto") kind = "GPU";
        else if (!kind.empty()) kind[0] = char(std::toupper(static_cast<unsigned char>(kind[0])));
        if (reason.size() > 60) reason = reason.substr(0, 57) + "...";
        return "cpu (" + kind + " fallback: " + reason + ")";
    }

    // Returns the render device for the requested preference, creating it
    // on first use or when the preference changed. Render worker only;
    // returns null if a stop was requested before the device was ready.
    std::shared_ptr<prism::device::RenderDevice> AcquireRenderDevice(
        prism::device::DevicePreference preference)
    {
        {
            std::lock_guard lock(mutex);
            if (renderDevice && renderDevicePreference == preference) return renderDevice;
        }
        if (deviceCreation.valid() && deviceCreationPreference != preference) {
            if (!WaitForDeviceJob(deviceCreation)) {
                ++devicePreparationsInterrupted;
                return nullptr;
            }
            try { (void)deviceCreation.get(); }
            catch (...) {}
        }
        if (!deviceCreation.valid()) {
            deviceCreationPreference = preference;
            progress.Begin(prism::Phase::Device);
            deviceCreation = std::async(std::launch::async, [preference, this] {
                prism::load_profile::Scope profile("device.create");
                CreatedDevice created;
                created.device = prism::device::CreateDevice(preference, &created.note);
                progress.End(prism::Phase::Device);
                return created;
            });
        }
        if (!WaitForDeviceJob(deviceCreation)) {
            ++devicePreparationsInterrupted;
            return nullptr;
        }
        CreatedDevice created;
        try {
            created = deviceCreation.get();
        } catch (std::exception const &e) {
            TF_WARN("PrismRT: render device creation failed: %s", e.what());
        }
        if (!created.device) created.device = prism::device::CreateDevice(
            prism::device::DevicePreference::Cpu, &created.note);
        if (!created.note.empty()) TF_WARN("PrismRT: %s", created.note.c_str());
        const auto info = created.device->Info();
        progress.SetDevice(DescribeDevice(*created.device, preference, created.note));
        TF_STATUS("PrismRT: rendering on %s device: %s",
            prism::device::ToString(info.kind), info.name.c_str());
        std::lock_guard lock(mutex);
        renderDevice = created.device;
        renderDevicePreference = preference;
        renderDeviceScene = nullptr;
        ++deviceSceneEpoch;
        return renderDevice;
    }

    // Advances the device's per-scene preparation for the displayed snapshot
    // without blocking: finishes a completed job and, once no job is running,
    // starts the one for the current epoch. Render worker only. True when the
    // device is prepared for the snapshot.
    bool AdvanceDevicePreparation(std::shared_ptr<prism::device::RenderDevice> const &device,
                                  std::shared_ptr<Snapshot> const &prepared)
    {
        uint64_t epoch;
        {
            std::lock_guard lock(mutex);
            epoch = deviceSceneEpoch;
            if (devicePreparedEpoch == epoch) return true;
        }
        if (devicePreparation.valid()) {
            // A superseded job still owns the device; it cannot be cancelled.
            if (devicePreparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return false;
            try {
                devicePreparation.get();
            } catch (std::exception const &e) {
                TF_WARN("PrismRT: render device preparation failed: %s", e.what());
            } catch (...) {}
            ++devicePreparations;
            std::lock_guard lock(mutex);
            epoch = deviceSceneEpoch;
            if (deviceSceneEpoch == devicePreparationEpoch) {
                devicePreparedEpoch = devicePreparationEpoch;
                return true;
            }
        }
        devicePreparationEpoch = epoch;
        devicePreparationStart = std::chrono::steady_clock::now();
        // The job owns the snapshot, so a published replacement cannot
        // free or (as an inactive bank) edit the scene it is reading.
        devicePreparation = std::async(std::launch::async,
            [this, device, prepared] {
                const auto start = std::chrono::steady_clock::now();
                prism::load_profile::Scope profile("device.prepare");
                // Test hook: stretches the preparation to exercise the CPU
                // preview that covers it.
                if (const char *delay = std::getenv("PRISM_TEST_GPU_PREPARATION_DELAY_MS");
                    delay && *delay)
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
                device->Prepare(*prepared->scene);
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count();
                std::lock_guard lock(mutex);
                devicePreparationSeconds = seconds;
            });
        return false;
    }

    // Runs the device's per-scene preparation for the displayed snapshot as
    // a job and waits for it. Render worker only; false if a stop was
    // requested first.
    bool PrepareRenderDevice(std::shared_ptr<prism::device::RenderDevice> const &device,
                             std::shared_ptr<Snapshot> const &prepared)
    {
        while (!AdvanceDevicePreparation(device, prepared)) {
            if (devicePreparation.valid() && !WaitForDeviceJob(devicePreparation)) {
                ++devicePreparationsInterrupted;
                return false;
            }
        }
        return true;
    }

    enum class PreparationOutcome { Stopped, Ready, Preview };

    // Like PrepareRenderDevice, but a GPU device that is still preparing
    // after gpuPreviewDelayMs yields Preview: the caller renders on
    // previewDevice and polls AdvanceDevicePreparation to hand over.
    PreparationOutcome PrepareOrPreview(std::shared_ptr<prism::device::RenderDevice> const &device,
                                        std::shared_ptr<Snapshot> const &prepared,
                                        RenderSettings const &renderSettings)
    {
        if (AdvanceDevicePreparation(device, prepared)) return PreparationOutcome::Ready;
        const auto kind = device->Info().kind;
        const int64_t limit = int64_t(renderSettings.gpuPreparationPreviewMaxMiB) << 20;
        const bool eligible = renderSettings.gpuPreparationPreview &&
            (kind == prism::device::DeviceKind::Vulkan || kind == prism::device::DeviceKind::Metal) &&
            (limit <= 0 || embreeLiveBytes.load(std::memory_order_relaxed) <= limit);
        if (!eligible)
            return PrepareRenderDevice(device, prepared) ? PreparationOutcome::Ready
                                                         : PreparationOutcome::Stopped;
        // Fast (cached) preparations never start the CPU preview.
        const auto deadline = devicePreparationStart + std::chrono::milliseconds(gpuPreviewDelayMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (renderThread->IsStopRequested()) {
                ++devicePreparationsInterrupted;
                return PreparationOutcome::Stopped;
            }
            if (AdvanceDevicePreparation(device, prepared)) return PreparationOutcome::Ready;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (AdvanceDevicePreparation(device, prepared)) return PreparationOutcome::Ready;
        if (!previewDevice)
            previewDevice = prism::device::CreateDevice(prism::device::DevicePreference::Cpu);
        return PreparationOutcome::Preview;
    }

    static constexpr const char *kGpuPreviewDevice = "cpu (preview while the GPU prepares)";

    // Marks the CPU preview as the displayed device.
    void BeginGpuPreview()
    {
        if (gpuPreviewActive.exchange(true)) return;
        progress.SetDevice(kGpuPreviewDevice);
    }

    // Restores the GPU's device line after a handover (unless the device
    // reported something more specific, such as a CPU fallback).
    void EndGpuPreview(prism::device::RenderDevice const &gpu, prism::device::DevicePreference preference)
    {
        if (!gpuPreviewActive.exchange(false)) return;
        if (progress.Read().device == kGpuPreviewDevice)
            progress.SetDevice(DescribeDevice(gpu, preference, {}));
    }

    // Builds the second bank for the snapshot just published, from the same
    // retained scene and inputs as the job that produced it. It copies that
    // snapshot's resolved materials, so it never prefetches or compiles and
    // binds exactly the materials the displayed snapshot uses.
    void LaunchSpareLocked(uint64_t generation)
    {
        std::optional<decltype(Snapshot::resolvedMaterials)> seed;
        if (materialService && snapshot) seed = snapshot->resolvedMaterials;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        buildCancel = cancel;
        buildingGeneration = generation;
        buildIsSpare = true;
        buildFuture = std::async(std::launch::async,
            [generation, retained = jobScene, tags = jobRenderTags,
             settings = jobSettings, resolverContext = jobResolverContext,
             device = device, prototypePool = prototypePool,
             canonicalSeed = canonicalPrimIds,
             cancel = std::move(cancel), seed = std::move(seed)]() {
                BuildResult result;
                result.generation = generation;
                result.spare = true;
                result.canonicalSeed = canonicalSeed;
                auto const start = std::chrono::steady_clock::now();
                prism::load_profile::Scope profile("snapshot.spare");
                try {
                    ArResolverContextBinder binder(resolverContext);
                    result.spareSnapshot = _BuildSnapshot(
                        retained, nullptr, settings, tags, device,
                        prototypePool, [cancel] {
                            return cancel->load(std::memory_order_relaxed);
                        }, seed ? &*seed : nullptr);
                    // _BuildSnapshot returns null whenever it saw the cancel
                    // flag before committing, so a snapshot here is whole.
                    if (result.spareSnapshot &&
                        result.spareSnapshot->materialSeedMissed)
                        result.error = "spare bank needs an unresolved material";
                    result.spareComplete = result.spareSnapshot &&
                        result.error.empty();
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

    void MaintainHiddenGeometryLocked()
    {
        // Published snapshots (including frame-owned picking scenes) remain
        // immutable. Policy changes wait for an exclusive inactive bank.
        const auto budget = prototypePool->hiddenGeometryBudgetBytes.load();
        const auto revision = prototypePool->residencyClock.load();
        if (!budget || !inactiveSnapshot || inactiveSnapshot.use_count() != 1 ||
            (hiddenMaintenanceBank == inactiveSnapshot.get() &&
             hiddenMaintenanceBudget == budget && hiddenMaintenanceRevision == revision)) return;
        if (_ApplyHiddenGeometryBudget(inactiveSnapshot.get(), true)) {
            rtcCommitScene(inactiveSnapshot->scene->scene);
            ++prototypePool->topLevelSceneCommits;
        }
        hiddenMaintenanceBank = inactiveSnapshot.get();
        hiddenMaintenanceBudget = budget;
        hiddenMaintenanceRevision = prototypePool->residencyClock.load();
    }

    ~Impl()
    {
        // Device jobs own their device and snapshot; let them finish before
        // the snapshots' shared Embree device is released.
        if (deviceCreation.valid()) {
            try { (void)deviceCreation.get(); }
            catch (...) {}
        }
        if (devicePreparation.valid()) {
            try { devicePreparation.get(); }
            catch (...) {}
        }
        if (buildCancel) buildCancel->store(true, std::memory_order_relaxed);
        if (buildFuture.valid()) {
            try { (void)buildFuture.get(); }
            catch (...) {}
        }
        // A retained published pick frame can outlive this bridge. Remove the
        // callback's Impl userdata after all bridge workers have quiesced.
        if (device) rtcSetDeviceMemoryMonitorFunction(device, nullptr, nullptr);
        snapshot.reset();
        inactiveSnapshot.reset();
        // PrototypeStorage releases Embree scenes/geometries. Expire every
        // strong snapshot owner before releasing their shared RTCDevice.
        prototypePool.reset();
        if (device) rtcReleaseDevice(device);
    }

    // Declared first so it outlives every worker that reports to it.
    prism::Progress progress;
    HdSceneIndexBaseRefPtr sceneIndex;
    HdRenderThread *renderThread;
    RTCDevice device = nullptr;
    // Executes the per-pixel work; recreated when the device setting changes.
    std::shared_ptr<prism::device::RenderDevice> renderDevice;
    prism::device::DevicePreference renderDevicePreference = prism::device::DevicePreference::Auto;
    SceneManager const *renderDeviceScene = nullptr;
    uint64_t renderDeviceGeneration = 0;
    // Integrator settings the render device last saw: GPU devices copy them
    // into their scene data, so a change must reach them like an edit.
    std::tuple<int, int, bool, int, bool> renderDeviceSettings{};
    // Device creation and per-scene device preparation (GPU kernel
    // compilation, scene and texture upload) can take tens of seconds. They
    // run as jobs the render worker waits on while polling for a stop, so a
    // host StopRender never waits for them; an interrupted job keeps running
    // and the next Render resumes waiting on it. Only the render worker
    // touches these futures (and ~Impl, after the worker has stopped).
    struct CreatedDevice {
        std::shared_ptr<prism::device::RenderDevice> device;
        std::string note;
    };
    std::future<CreatedDevice> deviceCreation;
    prism::device::DevicePreference deviceCreationPreference = prism::device::DevicePreference::Auto;
    std::future<void> devicePreparation;
    uint64_t devicePreparationEpoch = 0;
    std::chrono::steady_clock::time_point devicePreparationStart;
    // CPU preview while a GPU device prepares (render worker, except the
    // atomics, which statistics read).
    std::shared_ptr<prism::device::RenderDevice> previewDevice;
    int gpuPreviewDelayMs = 1000;
    int gpuPreviewThreads = 0; // 0: a fraction of the cores
    std::atomic<bool> gpuPreviewActive{false};
    std::atomic<uint64_t> gpuPreviewSamples{0};
    std::atomic<uint64_t> gpuPreviewHandovers{0};
    std::atomic<uint64_t> gpuPreviewHandoverSample{0};
    // Accumulation restarts after texture streaming made textures finer.
    std::atomic<uint64_t> textureStreamingRestarts{0};
    // Advanced (under mutex) whenever the device or its view of the scene
    // changes; devicePreparedEpoch records the last epoch fully prepared.
    uint64_t deviceSceneEpoch = 1;
    uint64_t devicePreparedEpoch = 0;
    std::atomic<uint64_t> devicePreparationsInterrupted{0};
    std::atomic<uint64_t> devicePreparations{0};
    double devicePreparationSeconds = 0.0;
    std::atomic<int64_t> embreeLiveBytes{0};
    std::atomic<int64_t> embreePeakBytes{0};
    mutable std::mutex mutex;
    bool sceneDirty = true;
    bool stateDirty = true;
    bool resumeRequested = false;
    unsigned int dirtyKinds = static_cast<unsigned int>(SceneDirtyKind::Unknown);
    std::unordered_set<SdfPath, SdfPath::Hash> dirtyPaths;
    RenderSettings settings;
    CameraState camera;
    HdRenderPassAovBindingVector bindings;
    std::vector<uint64_t> bindingAllocationRevisions;
    std::vector<unsigned int> committedRngStates;
    // Adaptive sampling: per-pixel RGB sums of all and of even-numbered
    // samples and the sample count (7 floats per pixel), and the pixels
    // still sampled.
    std::vector<float> adaptiveSums;
    prism::device::ActivePixels adaptiveActive;
    unsigned tileWidth = 0, tileHeight = 0;
    std::vector<RefinementTile> refinementTiles;
    TfTokenVector renderTags;
    unsigned int width = 0, height = 0;
    std::shared_ptr<Snapshot> snapshot;
    std::shared_ptr<Snapshot> inactiveSnapshot;
    Snapshot const *hiddenMaintenanceBank = nullptr;
    uint64_t hiddenMaintenanceBudget = 0;
    uint64_t hiddenMaintenanceRevision = 0;
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
    // When Prepare last saw the camera change; lighting-only edits wait for
    // the camera to rest (see Prepare).
    std::chrono::steady_clock::time_point lastCameraChange{};
    uint64_t deferredEnvironmentCaptures = 0;
    // The running build converts edits of lights alone.
    bool buildLightingOnly = false;
    // The displayed snapshot brought lighting held back during camera motion:
    // its quiet interval before refinement counts from that motion.
    bool refineFromCameraChange = false;
    std::deque<DirtyRecord> dirtyJournal;
    std::vector<StructuralChange> dirtyStructuralChanges;
    CanonicalPrimIds canonicalPrimIds;
    std::shared_ptr<const CanonicalPrimIds> canonicalRegistryView;
    bool canonicalIdsInitialized = false;
    bool dirtyStructuralSafe = true;
    std::unordered_set<SdfPath,SdfPath::Hash> nativePrototypeRoots;
    bool captureNativeRootsDirty = false;
    uint64_t captureGeneration = 0;
    size_t lastPlacementDirtyPathCount = 0;
    size_t lastPrototypeDirtyPathCount = 0;
    size_t lastMaterialDirtyPathCount = 0;
    bool captureActive = false;
    HdRetainedSceneIndexRefPtr captureScene;
    HdRetainedSceneIndexRefPtr retainedSceneCache;
    std::deque<SdfPath> captureQueue;
    std::unordered_set<SdfPath,SdfPath::Hash> captureQueued;
    struct StaticPrimRecord {
        TfToken primType;
        HdContainerDataSourceHandle dataSource;
    };
    std::unordered_map<SdfPath, StaticPrimRecord, SdfPath::Hash> staticPrims;
    bool staticPrimsComplete = false;
    bool captureFull = true;
    bool dirtyAllPaths = false;
    TfTokenVector captureRenderTags;
    unsigned int captureDirtyKinds = 0;
    std::unordered_set<SdfPath, SdfPath::Hash> captureDirtyPaths;
    ArResolverContext captureResolverContext;
    std::future<BuildResult> buildFuture;
    std::shared_ptr<std::atomic<bool>> buildCancel;
    uint64_t buildingGeneration = 0;
    uint64_t discardedBuilds = 0;
    // Failed builds of requestedGeneration. After an out-of-memory failure, or
    // a second failure, the scene is not rebuilt until it changes again;
    // generationFailure (the snapshotError statistic) then says why.
    uint64_t failedGeneration = 0;
    unsigned failedBuildAttempts = 0;
    std::string generationFailure;
    uint64_t reusedSnapshotBuilds = 0;
    uint64_t prototypeBuilds = 0;
    uint64_t capturedPrims = 0;
    double captureMilliseconds = 0.0;
    double captureBudgetMilliseconds = 0.0;
    std::chrono::steady_clock::time_point startTime;
    std::atomic<uint64_t> samples{0};
    uint64_t snapshotBuilds = 0;
    double snapshotBuildSeconds = 0.0;
    // Inputs of the last launched job, reused by its follow-up spare job.
    HdSceneIndexBaseRefPtr jobScene;
    TfTokenVector jobRenderTags;
    RenderSettings jobSettings;
    ArResolverContext jobResolverContext;
    bool buildIsSpare = false;
    std::chrono::steady_clock::time_point constructionTime =
        std::chrono::steady_clock::now();
    double firstPublishSeconds = 0.0;
    double spareSnapshotSeconds = 0.0;
    uint64_t spareSnapshotsInstalled = 0;
    uint64_t spareSnapshotsDiscarded = 0;
    uint64_t sparePrototypeConversions = 0;
    uint64_t sparePrototypeConversionCacheHits = 0;
    double renderSeconds = 0.0;
    uint64_t previewPublishes = 0;
    uint64_t refinementPublishes = 0;
    uint64_t firstPassPrimaryScratchBytes = 0;
    std::atomic<uint64_t> colorTracedPixels{0};
    std::atomic<uint64_t> geometricOnlyPixels{0};
    uint64_t colorPassScratchBytes = 0;
    uint64_t colorRngStorageBytes = 0;
    double lastPassTraceMilliseconds = 0;
    double lastPassCommitMilliseconds = 0;
    double lastPassRayWorkerMilliseconds = 0;
    double lastPassAovWorkerMilliseconds = 0;
    double lastPassWaitMilliseconds = 0;
    double lastPassPayloadMilliseconds = 0;
    double lastPassPublicationMilliseconds = 0;
    double previewMilliseconds = 0.0;
    // Tracing plus publication of the last preview: what adaptive preview
    // sizes against.
    double previewCostMilliseconds = 0.0;
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
    uint64_t pinnedSnapshotFallbacks = 0;
    uint64_t sphereBatchFallbacks = 0;
    uint64_t structuralLeafUpdates = 0;
    uint64_t structuralFallbacks = 0;
    uint64_t prototypeUpdateFallbacks = 0;
    uint64_t placementTopologyFallbacks = 0;
    bool profileBufferWrites = false;
    std::string lastRebuildReason = "initial population";
};

SceneBridge::SceneBridge(HdSceneIndexBaseRefPtr sceneIndex, HdRenderThread *thread)
    : _impl(std::make_unique<Impl>(std::move(sceneIndex), thread))
{
    // A new delegate starts from the default colour management, whatever an
    // earlier one in this process selected.
    try {
        prism::color::Activate(_impl->settings.color);
    } catch (std::exception const &error) {
        TF_WARN("PrismRT: %s", error.what());
    }
}
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
    MarkSceneDirtyBatch({{kind, paths, {}}});
}

void SceneBridge::MarkSceneDirtyBatch(std::vector<SceneDirtyNotice> const &notices)
{
    if (notices.empty()) return;
    struct Group {
        std::unordered_set<SdfPath, SdfPath::Hash> paths;
        bool allPaths = false;
        std::string reason;
        std::vector<StructuralChange> structuralChanges;
    };
    std::map<SceneDirtyKind, Group> groups;
    for (auto const &notice : notices) {
        auto &group = groups[notice.kind];
        group.allPaths |= notice.paths.empty();
        group.paths.insert(notice.paths.begin(), notice.paths.end());
        group.structuralChanges.insert(group.structuralChanges.end(),
            notice.structuralChanges.begin(),notice.structuralChanges.end());
        if (notice.kind == SceneDirtyKind::Structure && notice.structuralChanges.empty())
            group.structuralChanges.push_back({StructuralChange::Operation::Unknown,
                notice.paths.empty() ? SdfPath{} : notice.paths.front(),{},{},true});
        if (group.reason.empty())
            group.reason = !notice.reason.empty() ? notice.reason :
                (notice.paths.empty() ? "unknown dirty locator" :
                                       notice.paths.front().GetString());
    }
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    if (_impl->buildCancel)
        _impl->buildCancel->store(true, std::memory_order_relaxed);
    _impl->sceneDirty = true;
    ++_impl->requestedGeneration;
    for (auto &[kind, group] : groups) {
        if (group.allPaths) group.paths.clear();
        _impl->dirtyAllPaths |= group.allPaths;
        std::string structuralReason;
        const bool structuralEligible = kind == SceneDirtyKind::Structure &&
            _EligibleStructuralChanges(_impl->retainedSceneCache,_impl->sceneIndex,
                group.structuralChanges,_impl->settings.sphereBatching,&structuralReason,&_impl->nativePrototypeRoots);
        _impl->dirtyJournal.push_back({_impl->requestedGeneration,
            static_cast<unsigned int>(kind), group.paths, group.structuralChanges,
            structuralEligible,structuralReason});
        if (kind == SceneDirtyKind::Structure) {
            _impl->dirtyStructuralSafe &= structuralEligible;
            _impl->dirtyStructuralChanges.insert(_impl->dirtyStructuralChanges.end(),
                group.structuralChanges.begin(),group.structuralChanges.end());
            if (_impl->canonicalIdsInitialized) {
                SdfPathVector reserve;
                for (auto const &change : group.structuralChanges)
                    if (_GeometryLeafType(change.newType)) reserve.push_back(
                        change.operation == StructuralChange::Operation::Rename ? change.newPath : change.path);
                std::sort(reserve.begin(),reserve.end());
                for (auto const &path : reserve) {
                    PrimKey key{path,{}};
                    if (!_impl->canonicalPrimIds.contains(key))
                        _impl->canonicalPrimIds.emplace(key,static_cast<int>(_impl->canonicalPrimIds.size()));
                }
            }
        }
        _impl->dirtyKinds |= static_cast<unsigned int>(kind);
        _impl->dirtyPaths.insert(group.paths.begin(), group.paths.end());
        switch (kind) {
        case SceneDirtyKind::Placement: ++_impl->placementInvalidations; break;
        case SceneDirtyKind::Prototype: ++_impl->prototypeInvalidations; break;
        case SceneDirtyKind::Material: ++_impl->materialInvalidations; break;
        case SceneDirtyKind::Environment: ++_impl->environmentInvalidations; break;
        default: ++_impl->conservativeRebuilds; break;
        }
        if (_impl->snapshot && _impl->materialService) {
            // Keep resource invalidation attached to its own dirty kind.
            if (kind == SceneDirtyKind::Material) {
                if (group.paths.empty())
                    _impl->materialService->InvalidateAssociations({}, true);
                for (auto const &path : group.paths)
                    _impl->materialService->InvalidateResources(path, true);
            } else if (kind == SceneDirtyKind::Structure || kind == SceneDirtyKind::Unknown) {
                if (group.paths.empty())
                    _impl->materialService->InvalidateAssociations({}, true);
                for (auto const &path : group.paths)
                    _impl->materialService->InvalidateAssociations(path, true);
            }
        }
    }
    auto reason = groups.find(SceneDirtyKind::Unknown);
    if (reason == groups.end()) reason = groups.find(SceneDirtyKind::Structure);
    if (reason == groups.end()) reason = groups.begin();
    _impl->lastRebuildReason = reason->second.reason;
    if (prism::load_profile::Enabled()) {
        std::string kinds;
        for (auto const &[kind, group] : groups)
            kinds += std::to_string(static_cast<unsigned int>(kind)) + ":" +
                std::to_string(group.paths.size()) + (group.allPaths ? "(all)" : "") + ",";
        prism::load_profile::Mark("scene.dirty",
            "generation=" + std::to_string(_impl->requestedGeneration) +
            " building=" + std::to_string(_impl->buildingGeneration) +
            " kinds=" + kinds + " reason=" + reason->second.reason);
    }
    _converged.store(false);
}

void SceneBridge::RestartAccumulation()
{
    _impl->renderThread->StopRender();
    std::lock_guard lock(_impl->mutex);
    _impl->samples.store(0);
    _impl->stateDirty = true;
    for (HdRenderPassAovBinding const &binding : _impl->bindings)
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            buffer->SetConverged(false);
    _impl->startTime = std::chrono::steady_clock::now();
    _impl->renderSeconds = 0.0;
    _converged.store(false);
}

void SceneBridge::SetSettings(RenderSettings const &requested)
{
    if (requested.hiddenGeometryBudgetMiB < 0) {
        TF_WARN("PrismRT: hidden geometry budget must be non-negative; retaining the previous settings");
        return;
    }
    RenderSettings settings = requested;
    {
        // Colour management is process-wide (prism::color::Active): an
        // unusable config, working space or view keeps the previous one.
        RenderSettings const previous = GetSettings();
        if (settings.color != previous.color) {
            try {
                prism::color::Activate(settings.color);
            } catch (std::exception const &error) {
                TF_WARN("PrismRT: %s; retaining the previous colour management", error.what());
                settings.color = previous.color;
            }
        }
    }
    bool imageChanged = false;
    bool budgetChanged = false;
    bool colorChanged = false;
    bool bindingsChanged = false;
    {
        std::lock_guard lock(_impl->mutex);
        RenderSettings const &current = _impl->settings;
        if (current.samples == settings.samples &&
            current.maxDepth == settings.maxDepth &&
            current.rouletteDepth == settings.rouletteDepth &&
            current.seed == settings.seed &&
            current.sampleClamp == settings.sampleClamp &&
            current.adaptiveThreshold == settings.adaptiveThreshold &&
            current.adaptiveMinSamples == settings.adaptiveMinSamples &&
            current.neeProbabilityMin == settings.neeProbabilityMin &&
            current.neeFromDepth == settings.neeFromDepth &&
            current.spatialLights == settings.spatialLights &&
            current.adaptivePreview == settings.adaptivePreview &&
            current.previewTargetMs == settings.previewTargetMs &&
            current.previewMaxDimension == settings.previewMaxDimension &&
            current.refinementDelayMs == settings.refinementDelayMs &&
            current.resourceRevision == settings.resourceRevision &&
            current.sphereBatching == settings.sphereBatching &&
            current.hiddenGeometryBudgetMiB == settings.hiddenGeometryBudgetMiB &&
            current.textureFiltering == settings.textureFiltering &&
            current.textureStreaming == settings.textureStreaming &&
            current.textureStreamingPrepass == settings.textureStreamingPrepass &&
            current.domeLightCameraVisibility == settings.domeLightCameraVisibility &&
            current.environmentSampling == settings.environmentSampling &&
            current.gpuPreparationPreview == settings.gpuPreparationPreview &&
            current.gpuPreparationPreviewMaxMiB == settings.gpuPreparationPreviewMaxMiB &&
            current.device == settings.device && current.color == settings.color &&
            current.materialBindingPurposes == settings.materialBindingPurposes)
            return;
        bindingsChanged = current.materialBindingPurposes != settings.materialBindingPurposes;
        // Display and view do not touch the linear colour AOV.
        colorChanged = current.color.config != settings.color.config ||
            current.color.workingSpace != settings.color.workingSpace;
        imageChanged = current.maxDepth != settings.maxDepth ||
            current.rouletteDepth != settings.rouletteDepth ||
            current.seed != settings.seed ||
            current.sampleClamp != settings.sampleClamp ||
            current.adaptiveThreshold != settings.adaptiveThreshold ||
            current.adaptiveMinSamples != settings.adaptiveMinSamples ||
            current.neeProbabilityMin != settings.neeProbabilityMin ||
            current.neeFromDepth != settings.neeFromDepth ||
            current.spatialLights != settings.spatialLights ||
            current.resourceRevision != settings.resourceRevision ||
            current.sphereBatching != settings.sphereBatching ||
            current.textureFiltering != settings.textureFiltering ||
            current.textureStreaming != settings.textureStreaming ||
            current.environmentSampling != settings.environmentSampling ||
            current.domeLightCameraVisibility != settings.domeLightCameraVisibility ||
            current.device != settings.device || colorChanged || bindingsChanged;
        budgetChanged = current.samples != settings.samples;
        if (budgetChanged && !imageChanged && _converged.load() &&
            _impl->samples.load() != 0 &&
            std::none_of(_impl->bindings.begin(), _impl->bindings.end(),
                [](auto const &binding) {
                    return binding.aovName == HdAovTokens->color &&
                        dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer);
                })) {
            // A completed deterministic output pass has no color sample budget.
            budgetChanged = false;
        }
    }
    // Budget changes quiesce a staged pass without discarding committed work.
    // Presentation controls are read at preview boundaries and need no stop.
    if (imageChanged || budgetChanged) _impl->renderThread->StopRender();
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
    if (bindingsChanged) {
        _BindingPurposes().store(
            std::make_shared<const TfTokenVector>(settings.materialBindingPurposes));
        // Every prim may bind another material.
        if (_impl->snapshot) {
            _impl->sceneDirty = true;
            if (_impl->buildCancel)
                _impl->buildCancel->store(true, std::memory_order_relaxed);
            ++_impl->requestedGeneration;
            _impl->dirtyJournal.push_back({
                _impl->requestedGeneration,
                static_cast<unsigned int>(SceneDirtyKind::Unknown), {}});
            _impl->dirtyKinds |= static_cast<unsigned int>(SceneDirtyKind::Unknown);
            ++_impl->materialInvalidations;
            ++_impl->conservativeRebuilds;
            _impl->lastRebuildReason = "material binding purposes changed";
        }
    }
    // A new working space changes every converted colour: materials (their
    // generated colour transforms), lights, domes and their textures.
    if (_impl->snapshot && colorChanged) {
        if (_impl->materialService) _impl->materialService->InvalidateAll();
        _impl->sceneDirty = true;
        if (_impl->buildCancel)
            _impl->buildCancel->store(true, std::memory_order_relaxed);
        ++_impl->requestedGeneration;
        _impl->dirtyJournal.push_back({
            _impl->requestedGeneration,
            static_cast<unsigned int>(SceneDirtyKind::Unknown), {}});
        _impl->dirtyKinds |= static_cast<unsigned int>(SceneDirtyKind::Unknown);
        ++_impl->materialInvalidations;
        ++_impl->environmentInvalidations;
        ++_impl->conservativeRebuilds;
        _impl->lastRebuildReason = "colour management changed";
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
    _impl->prototypePool->hiddenGeometryBudgetBytes.store(
        static_cast<uint64_t>(settings.hiddenGeometryBudgetMiB) * 1024u * 1024u);
    _impl->MaintainHiddenGeometryLocked();
    _impl->stateDirty |= imageChanged;
    _impl->resumeRequested |= budgetChanged;
    if (imageChanged || budgetChanged) {
        for (auto const &binding : _impl->bindings)
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->SetConverged(false);
        _converged.store(false);
    }
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
    bool resumeRequested = false;
    std::vector<uint64_t> allocationRevisions;
    for (auto const &binding : bindings) {
        auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer);
        allocationRevisions.push_back(buffer ? buffer->GetAllocationRevision() : 0);
    }
    bool hasPreparationWork = false;
    bool spareOnly = false;
    // Buffers that have not shown a frame still hold their zero allocation,
    // which the host blends as transparent black over its background.
    bool clearNeeded = false;
    for (HdRenderPassAovBinding const &binding : bindings)
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            clearNeeded |= buffer->NeedsInitialClear();
    {
        std::lock_guard lock(_impl->mutex);
        _impl->MaintainHiddenGeometryLocked();
        cameraChanged = _impl->camera.worldToView != camera.worldToView ||
            _impl->camera.projection != camera.projection ||
            _impl->camera.nearClip != camera.nearClip ||
            _impl->camera.farClip != camera.farClip ||
            _impl->camera.fStop != camera.fStop ||
            _impl->camera.focalLength != camera.focalLength ||
            _impl->camera.focusDistance != camera.focusDistance ||
            _impl->camera.orthographic != camera.orthographic ||
            _impl->camera.linearExposureScale != camera.linearExposureScale ||
            _impl->width != width || _impl->height != height;
        auto renderBindings = bindings;
        auto previousBindings = _impl->bindings;
        // Color clear values belong to viewport presentation, not traced RGBA.
        for (auto &b : renderBindings) if (b.aovName == HdAovTokens->color) b.clearValue = VtValue();
        for (auto &b : previousBindings) if (b.aovName == HdAovTokens->color) b.clearValue = VtValue();
        bindingsChanged = previousBindings != renderBindings ||
            _impl->bindingAllocationRevisions != allocationRevisions;
        renderTagsChanged = _impl->renderTags != renderTags;
        stateChanged = _impl->stateDirty;
        resumeRequested = _impl->resumeRequested;
        hasPreparationWork = _impl->sceneDirty || _impl->captureActive ||
            _impl->buildFuture.valid();
        spareOnly = !_impl->sceneDirty && !_impl->captureActive &&
            _impl->buildFuture.valid() && _impl->buildIsSpare;
        if (!_impl->sceneDirty && !_impl->stateDirty && !resumeRequested && !cameraChanged &&
            !bindingsChanged && !renderTagsChanged && !hasPreparationWork &&
            !clearNeeded)
            return false;
    }

    // Only state that changes the currently displayed frame needs to quiesce
    // the render worker here. Scene notices already request a stop in
    // MarkSceneDirty; capture-only Prepare calls leave the old shaded snapshot
    // available for navigation.
    if (cameraChanged || bindingsChanged || renderTagsChanged || clearNeeded) {
        auto const stopStart = std::chrono::steady_clock::now();
        _impl->renderThread->StopRender();
        std::lock_guard lock(_impl->mutex);
        _impl->stopRenderMilliseconds = 1000.0 * std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stopStart).count();
    }
    // The worker is stopped here, and only Prepare's caller restarts it, so it
    // cannot be writing these buffers. Each buffer is cleared once per
    // allocation and never after it has shown a frame.
    if (clearNeeded)
        for (HdRenderPassAovBinding const &binding : bindings)
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->ApplyInitialClear(binding.clearValue);

    {
        std::lock_guard lock(_impl->mutex);
        if (renderTagsChanged && _impl->snapshot) {
            if (_impl->buildCancel)
                _impl->buildCancel->store(true, std::memory_order_relaxed);
            _impl->sceneDirty = true;
            ++_impl->requestedGeneration;
            _impl->dirtyKinds |= static_cast<unsigned int>(
                SceneDirtyKind::Placement);
            _impl->dirtyJournal.push_back({
                _impl->requestedGeneration,
                static_cast<unsigned int>(SceneDirtyKind::Placement), {}});
            _impl->lastRebuildReason = "render tags changed";
        }
        _impl->camera = camera;
        if (cameraChanged) {
            ++_impl->cameraRevision;
            _impl->lastCameraChange = std::chrono::steady_clock::now();
            _impl->refineFromCameraChange = false;
        }
        _impl->bindings = bindings;
        _impl->bindingAllocationRevisions = std::move(allocationRevisions);
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
            _impl->buildIsSpare = false;
            hasCompleted = true;
        }
    }
    if (hasCompleted && completed.spare) {
        // Installing the spare bank leaves the displayed snapshot, its
        // accumulation and the material service alone, so the render worker
        // keeps running. A finished spare is installed even when edits
        // arrived after it: it matches activeGeneration, and the journal
        // records newer than it stay for the next job to replay. Only a spare
        // cut short by a cancel, or a failed one, is dropped; the next edit
        // then takes the missing-bank path.
        // The spare job read the retained scene that incremental capture
        // mutates in place. Capture starts only once buildFuture has been
        // consumed here, so the job has finished reading before any capture
        // writes, and this install precedes the bank decision for the newer
        // generation's job.
        std::lock_guard lock(_impl->mutex);
        _impl->spareSnapshotSeconds = completed.seconds;
        if (completed.spareSnapshot) {
            _impl->sparePrototypeConversions =
                completed.spareSnapshot->prototypeConversions;
            _impl->sparePrototypeConversionCacheHits =
                completed.spareSnapshot->prototypeConversionCacheHits;
        }
        if (completed.spareComplete && completed.error.empty() &&
            completed.generation == _impl->activeGeneration) {
            if (_CanonicalizePrimIds(completed.spareSnapshot.get(),&completed.canonicalSeed))
                completed.spareSnapshot->picking.reset();
            _impl->inactiveSnapshot = std::move(completed.spareSnapshot);
            _impl->inactiveGeneration = completed.generation;
            while (!_impl->dirtyJournal.empty() &&
                   _impl->dirtyJournal.front().generation <=
                       _impl->inactiveGeneration)
                _impl->dirtyJournal.pop_front();
            ++_impl->spareSnapshotsInstalled;
        } else {
            ++_impl->spareSnapshotsDiscarded;
        }
    } else if (hasCompleted) {
        bool publish = false;
        {
            std::lock_guard lock(_impl->mutex);
            publish = completed.snapshot && completed.error.empty() &&
                completed.generation == _impl->requestedGeneration &&
                !_impl->sceneDirty;
            prism::load_profile::Mark(publish ? "snapshot.published" : "snapshot.discarded",
                "generation=" + std::to_string(completed.generation) +
                " requested=" + std::to_string(_impl->requestedGeneration) +
                " reason=" + _impl->lastRebuildReason);
            if (!publish) ++_impl->discardedBuilds;
            if (publish) {
                _impl->failedBuildAttempts = 0;
                _impl->generationFailure.clear();
            } else if (completed.generation == _impl->requestedGeneration &&
                !_impl->sceneDirty) {
                if (_impl->failedGeneration != completed.generation) {
                    _impl->failedGeneration = completed.generation;
                    _impl->failedBuildAttempts = 0;
                }
                ++_impl->failedBuildAttempts;
                if (!completed.error.empty() &&
                    (completed.outOfMemory || _impl->failedBuildAttempts >= 2)) {
                    // Rebuilding would fail the same way and, when out of
                    // memory, keep the process at its peak indefinitely.
                    // Report the failure and wait for the scene to change.
                    _impl->generationFailure = completed.error;
                    _impl->lastRebuildReason = "background generation failed";
                    TF_RUNTIME_ERROR("PrismRT: scene preparation failed (%s); "
                        "the scene is not rebuilt until it changes", completed.error.c_str());
                } else {
                    _impl->sceneDirty = true;
                    _impl->lastRebuildReason = "background generation failed";
                }
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
            if (completed.needsSpare) {
                // The replaced snapshot predates a rebuild; the spare job
                // launched below provides the bank instead.
                _impl->inactiveSnapshot.reset();
                _impl->inactiveGeneration = 0;
            } else if (_impl->snapshot) {
                _impl->inactiveSnapshot = std::move(_impl->snapshot);
                _impl->inactiveGeneration = _impl->activeGeneration;
            }
            const bool idsChanged = _CanonicalizePrimIds(completed.snapshot.get(),&_impl->canonicalPrimIds,
                _impl->canonicalRegistryView);
            _impl->canonicalRegistryView = completed.snapshot->canonicalIds;
            _impl->canonicalIdsInitialized = true;
            if (idsChanged) {
                auto picking = std::make_shared<PickingTable>();
                picking->primPaths = completed.snapshot->primPaths;
                picking->instancerPaths = completed.snapshot->instancerPaths;
                _BuildPickingInstanceTopologies(completed.snapshot.get(),picking.get());
                if (_impl->snapshot && _impl->snapshot->picking && *_impl->snapshot->picking == *picking)
                    completed.snapshot->picking = _impl->snapshot->picking;
                else completed.snapshot->picking = std::move(picking);
            }
            _impl->snapshot = std::move(completed.snapshot);
            if (!completed.residencyUnchanged) _SetCurrentResidency(*_impl->snapshot);
            _impl->activeGeneration = completed.generation;
            while (!_impl->dirtyJournal.empty() &&
                   _impl->dirtyJournal.front().generation <=
                       _impl->inactiveGeneration)
                _impl->dirtyJournal.pop_front();
            _impl->materialService = std::move(completed.materialService);
            _impl->snapshotBuildSeconds = completed.seconds;
            if (completed.reusedBank) ++_impl->reusedSnapshotBuilds;
            else ++_impl->snapshotBuilds;
            if (completed.pinnedBankFallback) {
                ++_impl->pinnedSnapshotFallbacks;
                ++_impl->conservativeRebuilds;
                _impl->lastRebuildReason = "inactive snapshot retained by a published frame";
            }
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
            if (completed.structuralLeafUpdate) ++_impl->structuralLeafUpdates;
            if (completed.structuralFallback) ++_impl->structuralFallbacks;
            if (completed.prototypeUpdateFallback) ++_impl->prototypeUpdateFallbacks;
            if (completed.placementTopologyFallback) ++_impl->placementTopologyFallbacks;
            if (!completed.updateFallbackReason.empty())
                _impl->lastRebuildReason = completed.updateFallbackReason;
            _impl->prototypeBuilds += completed.prototypeBuilds;
            if (_impl->firstPublishSeconds == 0.0)
                _impl->firstPublishSeconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() -
                        _impl->constructionTime).count();
            // Occupying buildFuture keeps capture gated and preparation
            // pending until the spare bank is installed or dropped.
            if (completed.needsSpare)
                _impl->LaunchSpareLocked(completed.generation);
            published = true;
            // Lighting that waited for the camera to rest: the camera has
            // already been quiet, so refinement need not wait a full
            // interval again after this snapshot's preview.
            _impl->refineFromCameraChange = _impl->buildLightingOnly &&
                std::chrono::steady_clock::now() - _impl->lastCameraChange < std::chrono::seconds(1);
            _impl->progress.SetError({});
        } else if (!completed.error.empty()) {
            TF_WARN("PrismRT: background snapshot generation failed: %s",
                    completed.error.c_str());
            _impl->progress.SetError("scene conversion failed: " + completed.error);
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
        // Edits of lights alone wait while the camera moves. A host light
        // that follows the camera (usdview's default camera light) changes
        // with every camera move; converting and uploading each of those
        // scenes restarted the render worker before any preview of the moving
        // camera could finish. Previews keep the displayed lighting until the
        // camera rests, then the latest lighting is captured once.
        const auto holdLighting = std::chrono::milliseconds(
            std::clamp(_impl->settings.refinementDelayMs / 2, 0, 50));
        const bool deferLighting = _impl->snapshot &&
            _impl->dirtyKinds == static_cast<unsigned int>(SceneDirtyKind::Environment) &&
            std::chrono::steady_clock::now() - _impl->lastCameraChange < holdLighting;
        if (deferLighting && _impl->sceneDirty && !_impl->captureActive &&
            !_impl->buildFuture.valid())
            ++_impl->deferredEnvironmentCaptures;
        if (_impl->sceneDirty && !_impl->captureActive &&
            !_impl->buildFuture.valid() && !deferLighting) {
            _impl->captureActive = true;
            _impl->captureGeneration = _impl->requestedGeneration;
            _impl->captureQueue.clear();
            _impl->captureQueued.clear();
            _impl->captureNativeRootsDirty = false;
            std::string structuralReason;
            const bool leafStructure = (_impl->dirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Structure)) != 0 &&
                _impl->dirtyStructuralSafe &&
                _EligibleStructuralChanges(_impl->retainedSceneCache,_impl->sceneIndex,
                    _impl->dirtyStructuralChanges,_impl->settings.sphereBatching,&structuralReason,&_impl->nativePrototypeRoots);
            const unsigned int unsafeCapture = static_cast<unsigned int>(SceneDirtyKind::Unknown) |
                (leafStructure ? 0u : static_cast<unsigned int>(SceneDirtyKind::Structure));
            if (!leafStructure && !structuralReason.empty()) _impl->lastRebuildReason = structuralReason;
            _impl->captureFull = !_impl->staticPrimsComplete ||
                _impl->dirtyAllPaths ||
                (_impl->dirtyKinds & unsafeCapture) != 0;
            if (_impl->captureFull) {
                _impl->captureScene = HdRetainedSceneIndex::New();
                _impl->staticPrims.clear();
                _impl->staticPrimsComplete = false;
                _impl->captureQueue.push_back(SdfPath::AbsoluteRootPath());
                _impl->captureQueued.insert(SdfPath::AbsoluteRootPath());
            } else {
                _impl->captureScene = _impl->retainedSceneCache;
                for (SdfPath const &path : _impl->dirtyPaths)
                    if (_impl->captureQueued.insert(path).second)
                        _impl->captureQueue.push_back(path);
            }
            _impl->captureRenderTags = _impl->renderTags;
            _impl->captureDirtyKinds = _impl->dirtyKinds;
            _impl->captureDirtyPaths = _impl->dirtyPaths;
            _impl->captureResolverContext =
                ArGetResolver().GetCurrentContext();
            _impl->capturedPrims = 0;
            prism::load_profile::Mark("capture.start",
                "generation=" + std::to_string(_impl->captureGeneration) +
                (_impl->captureFull ? " full" : " incremental") +
                " kinds=" + std::to_string(_impl->dirtyKinds) +
                " paths=" + std::to_string(_impl->dirtyPaths.size()));
        }
        _impl->RefreshLoadingPhaseLocked();
    }
    auto const captureStart = std::chrono::steady_clock::now();
    for (;;) {
        std::lock_guard lock(_impl->mutex);
        if (!_impl->captureActive) break;
        if (_impl->captureGeneration != _impl->requestedGeneration) {
            _impl->captureActive = false;
            _impl->captureScene.Reset();
            _impl->captureQueue.clear();
            _impl->captureQueued.clear();
            break;
        }
        if (!_impl->captureQueue.empty()) {
            captureWorked = true;
            SdfPath const path = _impl->captureQueue.front();
            _impl->captureQueue.pop_front();
            HdSceneIndexPrim const prim = _impl->sceneIndex->GetPrim(path);
            auto prior = _impl->staticPrims.find(path);
            if (prim.primType == HdPrimTypeTokens->instancer ||
                HdInstancedBySchema::GetFromParent(prim.dataSource) ||
                (prior != _impl->staticPrims.end() &&
                 (prior->second.primType == HdPrimTypeTokens->instancer ||
                  HdInstancedBySchema::GetFromParent(prior->second.dataSource))))
                _impl->captureNativeRootsDirty = true;
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
                std::erase_if(_impl->staticPrims, [&](auto const &record) {
                    return record.first.HasPrefix(path);
                });
                _impl->captureNativeRootsDirty = true;
                // HdRetainedSceneIndex owns its own prim table. Erasing the
                // parallel record cache alone leaves the removed subtree
                // visible to the background converter.
                _impl->captureScene->RemovePrims({path});
            }
            if (_impl->captureFull ||
                (_impl->captureDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Structure))) {
                for (SdfPath const &child :
                     _impl->sceneIndex->GetChildPrimPaths(path))
                    if (_impl->captureQueued.insert(child).second)
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
            if (_impl->captureFull || _impl->captureNativeRootsDirty) {
                _impl->nativePrototypeRoots.clear();
                for (auto const &[path,prim] : _impl->staticPrims) {
                    auto topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
                    if (auto prototypes = topology.GetPrototypes())
                        for (auto const &root : prototypes->GetTypedValue(0.0f))
                            _impl->nativePrototypeRoots.insert(root);
                    auto by = HdInstancedBySchema::GetFromParent(prim.dataSource);
                    if (auto roots = by.GetPrototypeRoots())
                        for (auto const &root : roots->GetTypedValue(0.0f))
                            _impl->nativePrototypeRoots.insert(root);
                }
            }
            _impl->retainedSceneCache = _impl->captureScene;
            const uint64_t generation = _impl->captureGeneration;
            prism::load_profile::Mark("capture.done",
                "generation=" + std::to_string(generation) +
                " prims=" + std::to_string(_impl->capturedPrims) +
                " capture_ms=" + std::to_string(int(_impl->captureMilliseconds)));
            HdSceneIndexBaseRefPtr retained = _impl->captureScene;
            TfTokenVector tags = _impl->captureRenderTags;
            RenderSettings jobSettings = _impl->settings;
            unsigned int jobDirtyKinds = _impl->captureDirtyKinds;
            auto jobDirtyPaths = _impl->captureDirtyPaths;
            decltype(jobDirtyPaths) jobPlacementPaths, jobPrototypePaths, jobMaterialPaths;
            std::vector<StructuralChange> jobStructuralChanges;
            bool jobStructuralSafe = true;
            std::string jobStructuralReason;
            if (!_impl->canonicalRegistryView ||
                _impl->canonicalRegistryView->size() != _impl->canonicalPrimIds.size())
                _impl->canonicalRegistryView = std::make_shared<const CanonicalPrimIds>(_impl->canonicalPrimIds);
            auto canonicalSeed = _impl->canonicalRegistryView;
            ArResolverContext resolverContext = _impl->captureResolverContext;
            RTCDevice device = _impl->device;
            std::shared_ptr<PrototypePool> prototypePool =
                _impl->prototypePool;
            std::shared_ptr<Snapshot> bank = std::move(
                _impl->inactiveSnapshot);
            std::shared_ptr<const PickingTable> publishedPicking =
                _impl->snapshot ? _impl->snapshot->picking : nullptr;
            const bool pinnedBankFallback = bank && bank.use_count() > 1;
            if (pinnedBankFallback) bank.reset();
            const uint64_t bankGeneration = _impl->inactiveGeneration;
            {
                // Even without a bank, each kind must keep its own paths.
                // With a bank, replay every generation it has not yet seen.
                const uint64_t replayAfter = bank ? bankGeneration : _impl->activeGeneration;
                jobDirtyKinds = 0;
                jobDirtyPaths.clear();
                jobPlacementPaths.clear();
                jobPrototypePaths.clear();
                jobMaterialPaths.clear();
                bool allPlacements = false, allPrototypes = false, allMaterials = false;
                for (auto const &record : _impl->dirtyJournal) {
                    if (record.generation <= replayAfter ||
                        record.generation > generation) continue;
                    jobDirtyKinds |= record.kinds;
                    if (record.kinds & static_cast<unsigned int>(SceneDirtyKind::Structure)) {
                        jobStructuralSafe &= record.structuralEligible;
                        if (!record.structuralReason.empty()) jobStructuralReason = record.structuralReason;
                        if (record.structuralChanges.empty())
                            jobStructuralChanges.push_back({StructuralChange::Operation::Unknown,{},{},{},true});
                        else jobStructuralChanges.insert(jobStructuralChanges.end(),
                            record.structuralChanges.begin(),record.structuralChanges.end());
                    }
                    jobDirtyPaths.insert(
                        record.paths.begin(), record.paths.end());
                    if (record.kinds & static_cast<unsigned int>(
                            SceneDirtyKind::Placement)) {
                        allPlacements |= record.paths.empty();
                        jobPlacementPaths.insert(
                            record.paths.begin(), record.paths.end());
                    }
                    if (record.kinds & static_cast<unsigned int>(
                            SceneDirtyKind::Prototype)) {
                        allPrototypes |= record.paths.empty();
                        jobPrototypePaths.insert(
                            record.paths.begin(), record.paths.end());
                    }
                    if (record.kinds & static_cast<unsigned int>(
                            SceneDirtyKind::Material)) {
                        allMaterials |= record.paths.empty();
                        jobMaterialPaths.insert(
                            record.paths.begin(), record.paths.end());
                    }
                }
                if (allPlacements) jobPlacementPaths.clear();
                if (allPrototypes) {
                    // Prototype refresh interprets an empty set as no work,
                    // so enumerate captured geometry plus old bank records.
                    // Retaining old paths lets a now-unsupported prim drop
                    // its stale prototype too.
                    jobPrototypePaths.clear();
                    for (auto const &[path, prim] : _impl->staticPrims)
                        if (prim.primType == HdPrimTypeTokens->mesh ||
                            prim.primType == HdPrimTypeTokens->sphere)
                            jobPrototypePaths.insert(path);
                    if (bank) {
                        for (auto const &[path, prototype] : bank->prototypes)
                            jobPrototypePaths.insert(path);
                        for (auto const &[path, location] : bank->sphereBatchLocations)
                            jobPrototypePaths.insert(path);
                    }
                }
                if (allMaterials) jobMaterialPaths.clear();
            }
            _impl->lastPlacementDirtyPathCount = jobPlacementPaths.size();
            _impl->lastPrototypeDirtyPathCount = jobPrototypePaths.size();
            _impl->lastMaterialDirtyPathCount = jobMaterialPaths.size();
            _impl->inactiveGeneration = 0;
            std::string mx = std::getenv("PRISM_MATERIALX_LIBRARY_PATH") ?
                std::getenv("PRISM_MATERIALX_LIBRARY_PATH") : "";
            std::string mdl = std::getenv("PRISM_MATERIALX_MDL") ?
                std::getenv("PRISM_MATERIALX_MDL") : "";
            _impl->captureActive = false;
            _impl->captureScene.Reset();
            _impl->sceneDirty = false;
            _impl->dirtyKinds = 0;
            _impl->dirtyAllPaths = false;
            _impl->dirtyPaths.clear();
            _impl->dirtyStructuralChanges.clear();
            _impl->dirtyStructuralSafe = true;
            _impl->buildingGeneration = generation;
            _impl->jobScene = retained;
            _impl->jobRenderTags = tags;
            _impl->jobSettings = jobSettings;
            _impl->jobResolverContext = resolverContext;
            _impl->buildIsSpare = false;
            _impl->buildLightingOnly =
                jobDirtyKinds == static_cast<unsigned int>(SceneDirtyKind::Environment);
            auto cancel = std::make_shared<std::atomic<bool>>(false);
            _impl->buildCancel = cancel;
            _impl->buildFuture = std::async(std::launch::async,
                [generation, associationScope = reinterpret_cast<uint64_t>(_impl->sceneIndex.operator->()),
                 retained = std::move(retained),
                 tags = std::move(tags), jobSettings, resolverContext,
                 jobDirtyKinds, canonicalSeed = std::move(canonicalSeed),
                 jobStructuralChanges = std::move(jobStructuralChanges),
                 jobStructuralSafe, jobStructuralReason = std::move(jobStructuralReason),
                 jobPlacementPaths = std::move(jobPlacementPaths),
                 jobPrototypePaths = std::move(jobPrototypePaths),
                 jobMaterialPaths = std::move(jobMaterialPaths),
                 bank = std::move(bank), device, pinnedBankFallback,
                 publishedPicking = std::move(publishedPicking),
                 prototypePool = std::move(prototypePool),
                 cancel = std::move(cancel),
                 mx = std::move(mx), mdl = std::move(mdl)]() mutable {
                    SceneBridge::Impl::BuildResult result;
                    result.generation = generation;
                    result.pinnedBankFallback = pinnedBankFallback;
                    auto const start = std::chrono::steady_clock::now();
                    prism::load_profile::Scope profile("snapshot.build");
                    profile.detail("generation=" + std::to_string(generation) +
                                   (bank ? " bank" : " full"));
                    try {
                        bool placementTopologyChanged = false;
                        bool sphereBatchTopologyChanged = false;
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
                        std::string structuralReason = jobStructuralReason;
                        bool leafStructure = jobStructuralSafe &&
                            (jobDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Structure)) != 0 &&
                            _EligibleStructuralChanges(retained,retained,jobStructuralChanges,
                                jobSettings.sphereBatching,&structuralReason);
                        if (leafStructure && bank) for (auto const &change : jobStructuralChanges) {
                            for (auto const &[path,location] : bank->sphereBatchLocations)
                                if (path.HasPrefix(change.path) ||
                                    (!change.newPath.IsEmpty() && path.HasPrefix(change.newPath))) leafStructure = false;
                        }
                        if (leafStructure && bank && !_RekeyStructuralRenames(retained,
                                jobStructuralChanges,bank.get(),nullptr,tags,nullptr,{},true)) {
                            leafStructure = false;
                            structuralReason = "structural rename destination conflicts with retained identities";
                        }
                        const unsigned int unsafeKinds = static_cast<unsigned int>(SceneDirtyKind::Unknown) |
                            (leafStructure ? 0u : static_cast<unsigned int>(SceneDirtyKind::Structure));
                        result.structuralFallback = (jobDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Structure)) && !leafStructure;
                        if (result.structuralFallback) result.updateFallbackReason = structuralReason.empty()
                            ? "structural bank topology requires full rebuild" : structuralReason;
                        if (cancel->load(std::memory_order_relaxed)) {
                            return result;
                        } else if (bank && (jobDirtyKinds & unsafeKinds) == 0) {
                            const uint64_t priorPrototypeBuilds =
                                bank->prototypeBuildCount;
                            bank->canonicalIds = canonicalSeed;
                            bank->primPaths.resize(std::max(bank->primPaths.size(),canonicalSeed->size()));
                            bank->instancerPaths.resize(bank->primPaths.size());
                            bank->lastStructuralPathsUpdated = 0;
                            bank->lastStructuralPlacementsAttached = 0;
                            bank->lastStructuralPlacementsDetached = 0;
                            bank->lastCanonicalRecordsVisited = 0;
                            bank->lastPickingRecordsVisited = 0;
                            bank->changedPickingPrims.clear();
                            bank->lastCountRecordsVisited = 0;
                            bank->lastInputMetadataPrototypesVisited = 0;
                            bank->lastDependencyPlacementsVisited = 0;
                            bank->lastCanonicalMilliseconds = 0;
                            bank->lastPickingMilliseconds = 0;
                            bank->lastCountsMilliseconds = 0;
                            bank->lastDependencyMilliseconds = 0;
                            bank->lastInputMetadataMilliseconds = 0;
                            bank->lastInputComparisonMilliseconds = 0;
                            bank->lastPlacementReconcileMilliseconds = 0;
                            bank->lastPlacementRecordsVisited = 0;
                            bank->lastInstanceArrayElementsScanned = 0;
                            bank->lastPlacementTopologyAttached = 0;
                            bank->lastPlacementTopologyDetached = 0;
                            bank->lastVisibilityPlacementsUpdated = 0;
                            bank->lastPlacementTransformsUpdated = 0;
                            bank->lastPrototypePlacementsRepointed = 0;
                            bank->lastPrototypePathsUpdated = 0;
                            const bool restoredPrototypes = _RestoreEvictedPrototypes(
                                retained, result.materialService.get(), bank.get(), tags,
                                [cancel] { return cancel->load(std::memory_order_relaxed); });
                            std::vector<std::shared_ptr<PrototypeStorage>> displacedStorage;
                            struct BankCleanup {
                                std::shared_ptr<Snapshot> *bank;
                                ~BankCleanup() { bank->reset(); }
                            } bankCleanup{&bank};
                            for (auto path = jobPrototypePaths.begin(); path != jobPrototypePaths.end();) {
                                if (retained->GetPrim(*path).primType == HdPrimTypeTokens->instancer) {
                                    jobPlacementPaths.insert(*path);
                                    jobDirtyKinds |= static_cast<unsigned int>(SceneDirtyKind::Placement);
                                    path = jobPrototypePaths.erase(path);
                                } else ++path;
                            }
                            bool placementRebuiltAll = false;
                            bool placementNeedsFullSnapshot = false;
                            bool batchNeedsFullSnapshot = false;
                            const bool hadGlobalPlacement = jobPlacementPaths.empty() &&
                                (jobDirtyKinds & static_cast<unsigned int>(SceneDirtyKind::Placement));
                            if (leafStructure) {
                                result.structuralLeafUpdate = true;
                                placementTopologyChanged = true;
                                if (!_UpdateStructuralLeaves(retained,jobStructuralChanges,
                                    result.materialService.get(),bank.get(),tags,&displacedStorage,[cancel] {
                                        return cancel->load(std::memory_order_relaxed);
                                    })) {
                                    if (cancel->load(std::memory_order_relaxed)) return result;
                                    bank = _BuildSnapshot(retained,result.materialService.get(),jobSettings,tags,
                                        device,prototypePool,[cancel] { return cancel->load(std::memory_order_relaxed); });
                                    if (!bank) return result;
                                    batchNeedsFullSnapshot = true; placementRebuiltAll = true;
                                    result.structuralLeafUpdate = false; result.structuralFallback = true;
                                    result.updateFallbackReason = "structural leaf update failed; full rebuild required";
                                }
                                for (auto const &change : jobStructuralChanges) {
                                    jobPrototypePaths.erase(change.path); jobPlacementPaths.erase(change.path);
                                }
                                if (!hadGlobalPlacement && jobPlacementPaths.empty())
                                    jobDirtyKinds &= ~static_cast<unsigned int>(SceneDirtyKind::Placement);
                            }
                            std::unordered_set<SdfPath, SdfPath::Hash>
                                batchDirtyPaths = jobPrototypePaths;
                            batchDirtyPaths.insert(jobPlacementPaths.begin(),
                                                   jobPlacementPaths.end());
                            if (jobPlacementPaths.empty() &&
                                (jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Placement)) != 0) {
                                for (auto const &dependency :
                                     bank->sphereBatchDependencies)
                                    batchDirtyPaths.insert(dependency.first);
                            }
                            std::unordered_set<SdfPath, SdfPath::Hash>
                                handledBatchPaths;
                            if (!batchNeedsFullSnapshot && !_RefreshSphereBatches(
                                    retained, batchDirtyPaths,
                                    result.materialService.get(), bank.get(),
                                    tags, &handledBatchPaths, true)) {
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
                            sphereBatchTopologyChanged =
                                !handledBatchPaths.empty() || batchNeedsFullSnapshot;
                            if (!batchNeedsFullSnapshot &&
                                jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Prototype))
                            {
                                if (!jobPrototypePaths.empty() && !_RefreshPrototypes(
                                    retained, jobPrototypePaths, result.materialService.get(),
                                    bank.get(), tags, &displacedStorage)) {
                                    bank = _BuildSnapshot(retained, result.materialService.get(),
                                        jobSettings, tags, device, prototypePool, [cancel] {
                                            return cancel->load(std::memory_order_relaxed);
                                        });
                                    if (!bank) return result;
                                    batchNeedsFullSnapshot = true;
                                    placementRebuiltAll = true;
                                    result.prototypeUpdateFallback = true;
                                    result.updateFallbackReason = "prototype conversion or geometry type requires full rebuild";
                                }
                            }
                            if (cancel->load(std::memory_order_relaxed))
                                return result;
                            // Replay placement edits after prototype repointing,
                            // then commit the complete inactive scene once.
                            if (!batchNeedsFullSnapshot &&
                                jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Placement)) {
                                bool const placementUpdated = _UpdatePlacements(retained, bank.get(),
                                  tags, jobPlacementPaths, [cancel] {
                                    return cancel->load(
                                        std::memory_order_relaxed);
                                }, &placementRebuiltAll,
                                    &placementNeedsFullSnapshot,
                                    &placementTopologyChanged, false, true,
                                    &result.updateFallbackReason);
                                if (!placementUpdated && !cancel->load(std::memory_order_relaxed)) {
                                    placementNeedsFullSnapshot = true;
                                    placementRebuiltAll = true;
                                    if (result.updateFallbackReason.empty())
                                        result.updateFallbackReason = "placement update failed; full rebuild required";
                                }
                            }
                            if (cancel->load(std::memory_order_relaxed))
                                return result;
                            result.placementTopologyFallback = placementRebuiltAll && !batchNeedsFullSnapshot;
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
                                bank->domeEnvironments.clear();
                                bank->scene->lights.clear();
                                bank->lightLinkBits.clear();
                                bank->shadowLinkBits.clear();
                                _RefreshEnvironment(
                                    retained, SdfPath::AbsoluteRootPath(), bank.get());
                                _ApplyLinkMasks(retained, bank.get());
                            }
                            result.residencyUnchanged =
                                jobDirtyKinds == static_cast<unsigned int>(SceneDirtyKind::Placement) &&
                                !restoredPrototypes && !placementTopologyChanged && !sphereBatchTopologyChanged &&
                                !placementNeedsFullSnapshot && !batchNeedsFullSnapshot &&
                                bank->lastVisibilityPlacementsUpdated == 0 &&
                                prototypePool->hiddenGeometryBudgetBytes.load() == 0;
                            const bool retentionDetached = !result.residencyUnchanged &&
                                _ApplyHiddenGeometryBudget(bank.get());
                            placementTopologyChanged |= restoredPrototypes || retentionDetached;
                            if (!batchNeedsFullSnapshot && !placementNeedsFullSnapshot &&
                                (retentionDetached || (jobDirtyKinds & (static_cast<unsigned int>(SceneDirtyKind::Prototype) |
                                    static_cast<unsigned int>(SceneDirtyKind::Placement) |
                                    static_cast<unsigned int>(SceneDirtyKind::Structure))) != 0)) {
                                if (cancel->load(std::memory_order_relaxed)) return result;
                                auto const commitStart = std::chrono::steady_clock::now();
                                rtcCommitScene(bank->scene->scene);
                                ++bank->prototypePool->topLevelSceneCommits;
                                bank->embreeSceneCommitSeconds += std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - commitStart).count();
                            }
                            result.snapshot = std::move(bank);
                            const bool rebuiltSnapshot =
                                placementNeedsFullSnapshot ||
                                batchNeedsFullSnapshot;
                            result.reusedBank = !rebuiltSnapshot;
                            if (result.reusedBank &&
                                (jobDirtyKinds & (static_cast<unsigned int>(
                                    SceneDirtyKind::Prototype) | static_cast<unsigned int>(SceneDirtyKind::Structure))) != 0)
                                _RefreshCachedCounts(result.snapshot.get());
                            result.visibilityRecoveryRebuild =
                                placementNeedsFullSnapshot;
                            result.prototypeBuilds = rebuiltSnapshot
                                ? result.snapshot->prototypeBuildCount
                                : result.snapshot->prototypeBuildCount -
                                    priorPrototypeBuilds;
                            // A visibility restore with no retained
                            // placement IDs rebuilt the full TLAS. Keep the
                            // second bank at the same state so the next
                            // unrelated edit cannot republish an older hidden
                            // scene; Prepare builds it after publication.
                            result.needsSpare = placementRebuiltAll &&
                                result.snapshot &&
                                !cancel->load(std::memory_order_relaxed);
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
                            // Keep a synchronized mutable bank so the first
                            // local edit after a structural rebuild does not
                            // require another full conversion. Prepare builds
                            // it after publication so the first image does
                            // not wait for it.
                            result.needsSpare = result.snapshot &&
                                !cancel->load(std::memory_order_relaxed);
                        }
                        if (result.snapshot && result.snapshot->scene) {
                            auto const previousPicking =
                                result.snapshot->picking;
                            auto const fallbackPicking = publishedPicking
                                ? publishedPicking : previousPicking;
                            const unsigned int nonTopologyKinds =
                                jobDirtyKinds & ~static_cast<unsigned int>(
                                    SceneDirtyKind::Placement);
                            const bool placementOnly =
                                (jobDirtyKinds & static_cast<unsigned int>(
                                    SceneDirtyKind::Placement)) != 0 &&
                                nonTopologyKinds == 0;
                            const bool onlyMaterialEnvironment =
                                jobDirtyKinds != 0 &&
                                (jobDirtyKinds & ~(
                                    static_cast<unsigned int>(SceneDirtyKind::Material) |
                                    static_cast<unsigned int>(SceneDirtyKind::Environment))) == 0;
                            const bool canReusePickingDirectly = result.reusedBank &&
                                fallbackPicking &&
                                (onlyMaterialEnvironment ||
                                 (placementOnly && !placementTopologyChanged &&
                                  !sphereBatchTopologyChanged));
                            if (canReusePickingDirectly) {
                                result.snapshot->picking = fallbackPicking;
                            } else {
                                const bool partialPicking = result.reusedBank && placementOnly &&
                                    !sphereBatchTopologyChanged && fallbackPicking;
                                auto picking = partialPicking
                                    ? std::make_shared<PickingTable>(*fallbackPicking)
                                    : std::make_shared<PickingTable>();
                                picking->primPaths = result.snapshot->primPaths;
                                picking->instancerPaths =
                                    result.snapshot->instancerPaths;
                                _BuildPickingInstanceTopologies(
                                    result.snapshot.get(), picking.get(), partialPicking
                                        ? &result.snapshot->changedPickingPrims : nullptr);
                                if (publishedPicking &&
                                    *publishedPicking == *picking)
                                    result.snapshot->picking = publishedPicking;
                                else if (previousPicking &&
                                    *previousPicking == *picking)
                                    result.snapshot->picking = previousPicking;
                                else
                                    result.snapshot->picking = std::move(picking);
                            }
                            result.snapshot->scene->max_depth =
                                jobSettings.maxDepth;
                            result.snapshot->scene->rr_min_depth =
                                jobSettings.rouletteDepth;
                            result.snapshot->scene->environmentSampling =
                                jobSettings.environmentSampling ==
                                    RenderSettings::EnvironmentSampling::Mis;
                            result.snapshot->scene->textureFiltering =
                                jobSettings.textureFiltering;
                            result.snapshot->scene->textureStreaming =
                                jobSettings.textureStreaming;
                        }
                    } catch (std::bad_alloc const &) {
                        // Release the partial scene before reporting.
                        result.snapshot.reset();
                        result.error = "out of memory while preparing the scene";
                        result.outOfMemory = true;
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
    _impl->RefreshLoadingPhaseLocked();
    if (spareOnly && !stateChanged && !resumeRequested && !cameraChanged && !bindingsChanged &&
        !renderTagsChanged && !clearNeeded && !published && !captureWorked &&
        !_impl->sceneDirty && !_impl->captureActive) {
        // Only the spare bank advanced. The displayed frame is unchanged, so
        // neither restart the render worker nor clear its convergence.
        _impl->prepareMilliseconds = 1000.0 * std::chrono::duration<double>(
            std::chrono::steady_clock::now() - prepareStart).count();
        return false;
    }
    if ((stateChanged || published) && _impl->snapshot &&
        _impl->snapshot->scene) {
        _impl->snapshot->scene->max_depth = _impl->settings.maxDepth;
        _impl->snapshot->scene->rr_min_depth = _impl->settings.rouletteDepth;
        _impl->snapshot->scene->environmentSampling =
            _impl->settings.environmentSampling ==
                RenderSettings::EnvironmentSampling::Mis;
        _impl->snapshot->scene->textureFiltering =
            _impl->settings.textureFiltering;
        _impl->snapshot->scene->textureStreaming =
            _impl->settings.textureStreaming;
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
    _impl->resumeRequested = false;
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
    std::shared_ptr<prism::device::RenderDevice> renderDevice;
    {
        std::lock_guard lock(_impl->mutex);
        settings = _impl->settings;
    }
    // Device creation runs outside the bridge mutex, which Hydra's Execute
    // and render statistics take on the host thread.
    renderDevice = _impl->AcquireRenderDevice(settings.device);
    if (!renderDevice) return;
    {
        std::lock_guard lock(_impl->mutex);
        snapshot = _impl->snapshot; camera = _impl->camera;
        bindings = _impl->bindings; settings = _impl->settings;
        // Published edits replace the snapshot or advance its generation;
        // camera changes do neither.
        const auto deviceSettings = snapshot && snapshot->scene
            ? std::make_tuple(snapshot->scene->max_depth, snapshot->scene->rr_min_depth,
                  snapshot->scene->environmentSampling, int(snapshot->scene->textureFiltering),
                  snapshot->scene->textureStreaming)
            : std::tuple<int, int, bool, int, bool>{};
        if (snapshot && snapshot->scene &&
            (_impl->renderDeviceScene != snapshot->scene.get() ||
             _impl->renderDeviceGeneration != _impl->activeGeneration ||
             _impl->renderDeviceSettings != deviceSettings)) {
            renderDevice->SceneChanged(*snapshot->scene);
            _impl->progress.End(prism::Phase::Rendering);
            _impl->renderDeviceScene = snapshot->scene.get();
            _impl->renderDeviceGeneration = _impl->activeGeneration;
            _impl->renderDeviceSettings = deviceSettings;
            ++_impl->deviceSceneEpoch;
        }
        width = _impl->width; height = _impl->height;
        generation = _impl->activeGeneration;
        cameraRevision = _impl->cameraRevision;
        // A spare bank job never changes what is displayed.
        preparing = _impl->sceneDirty || _impl->captureActive ||
            (_impl->buildFuture.valid() && !_impl->buildIsSpare);
    }
    // A host can bind one destination more than once. Each physical output
    // must be accumulated and published once: a second Publish would swap in
    // recycled old pixels. Distinct destinations still share AOV staging.
    HdRenderPassAovBindingVector uniqueBindings;
    uniqueBindings.reserve(bindings.size());
    for (auto const &binding : bindings) {
        if (binding.renderBuffer && std::any_of(uniqueBindings.begin(), uniqueBindings.end(),
                [&](auto const &other) {
                    return binding.renderBuffer == other.renderBuffer && binding.aovName == other.aovName;
                })) continue;
        uniqueBindings.push_back(binding);
    }
    bindings = std::move(uniqueBindings);
    bool hasColor = false, hasGeometric = false;
    for (auto const &binding : bindings) {
        if (!dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer)) continue;
        const AovKind kind = _AovKind(binding.aovName);
        hasColor |= kind == AovKind::Color;
        hasGeometric |= kind != AovKind::Color && kind != AovKind::Unknown;
    }
    const int sampleCount = hasColor ? std::max(1, settings.samples) : 1;
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
    // Kernel compilation and scene upload happen here, not inside the
    // preview or pass, so a host stop never waits for them.
    // While a GPU device is still preparing, the CPU device renders instead
    // (activeDevice) and the loop below hands over once the GPU is ready.
    prism::device::RenderDevice *activeDevice = renderDevice.get();
    bool previewingGpu = false;
    if (hasColor || hasGeometric) {
        using Outcome = Impl::PreparationOutcome;
        const Outcome outcome = _impl->PrepareOrPreview(renderDevice, snapshot, settings);
        if (outcome == Outcome::Stopped) return;
        if (outcome == Outcome::Preview) {
            previewingGpu = true;
            activeDevice = _impl->previewDevice.get();
            _impl->BeginGpuPreview();
        }
    }
    if (!previewingGpu) _impl->EndGpuPreview(*renderDevice, settings.device);
    _impl->progress.Begin(prism::Phase::Rendering);
    for (HdRenderPassAovBinding const &binding : bindings) {
        TfToken const &name = binding.aovName;
        const bool supported = name == HdAovTokens->color ||
            name == HdAovTokens->normal || name == HdAovTokens->Neye ||
            name == HdAovTokens->Peye || name == TfToken("primvars:st") ||
            name == HdAovTokens->depth || name == HdAovTokens->cameraDepth ||
            name == HdAovTokens->primId || name == HdAovTokens->elementId ||
            name == HdAovTokens->instanceId;
        if (!supported) _WarnUnsupportedAov(name);
    }
    const int firstSample = static_cast<int>(_impl->samples.load());
    std::shared_ptr<const PublishedFrame> frame;
    if (firstSample != 0) {
        std::lock_guard lock(_impl->mutex);
        frame = _impl->lastPublishedFrame;
    }
    if (!frame) {
        auto initialFrame = std::make_shared<PublishedFrame>();
        initialFrame->generation = generation;
        initialFrame->cameraRevision = cameraRevision;
        initialFrame->picking = snapshot->picking;
        initialFrame->pickScene = std::make_shared<PublishedSnapshotPickScene>(
            snapshot, camera, width, height, renderDevice);
        initialFrame->camera = camera;
        initialFrame->width = width;
        initialFrame->height = height;
        frame = std::move(initialFrame);
    }
    if (firstSample == 0 && hasColor) {
        // Preview rendering runs on Hydra's render worker.  Prepare only captures
        // the raw AOV bindings after StopRender has quiesced their previous user;
        // buffer destruction and resize also stop this worker before changing
        // storage, so no preparation worker ever dereferences a render buffer.
        RenderSettings previewSettings = settings;
        const auto deviceKind = activeDevice->Info().kind;
        const bool gpuPreview = deviceKind == prism::device::DeviceKind::Metal ||
            deviceKind == prism::device::DeviceKind::Vulkan;
        // The automatic maximum keeps CPU previews coarse; GPU devices afford
        // far more pixels within the target.
        const int maxDimension = settings.previewMaxDimension > 0
            ? std::clamp(settings.previewMaxDimension, 16, 1024) : (gpuPreview ? 512 : 96);
        previewSettings.previewMaxDimension = maxDimension;
        if (settings.adaptivePreview) {
            std::lock_guard lock(_impl->mutex);
            if (_impl->previewCostMilliseconds <= 0.0) {
                // Without a measurement, start below the maximum.
                previewSettings.previewMaxDimension = std::min(maxDimension, gpuPreview ? 256 : 96);
            } else {
                const double ratio = std::sqrt(
                    static_cast<double>(std::clamp(settings.previewTargetMs, 1, 1000)) /
                    _impl->previewCostMilliseconds);
                previewSettings.previewMaxDimension = std::clamp(
                    static_cast<int>(std::lround(std::max(
                        _impl->previewWidth, _impl->previewHeight) *
                        std::clamp(ratio, 0.5, 2.0))),
                    16, maxDimension);
            }
        }
        auto const previewStart = std::chrono::steady_clock::now();
        PreviewResult const preview = _RenderPreview(
            snapshot, camera, previewSettings, bindings, width, height,
            _impl->renderThread, frame, _impl->colorTracedPixels, *activeDevice);
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
            if (preview.published)
                _impl->previewCostMilliseconds = preview.traceMilliseconds + preview.publicationMilliseconds;
            if (preview.published) {
                if (_impl->previewPublishes++ == 0)
                    prism::load_profile::Mark("frame.first_preview",
                                              "generation=" + std::to_string(generation));
                _impl->lastPublishedFrame = frame;
            }
        }
        // Do not let full-resolution work contend with camera interaction.  Each
        // new camera state cancels this wait in at most two milliseconds and starts
        // a fresh quiet period; refinement begins automatically after input stops.
        auto const quietStart = std::chrono::steady_clock::now();
        for (;;) {
            if (_impl->renderThread->IsStopRequested()) return;
            int delay;
            auto start = quietStart;
            {
                std::lock_guard lock(_impl->mutex);
                delay = _impl->settings.refinementDelayMs;
                // Lighting held back during camera motion was published after
                // the camera came to rest: the quiet interval started then.
                if (_impl->refineFromCameraChange)
                    start = std::min(quietStart, _impl->lastCameraChange);
            }
            if (std::chrono::steady_clock::now() >= start +
                std::chrono::milliseconds(std::clamp(delay, 0, 2000))) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (_impl->renderThread->IsStopRequested()) return;
    }
    if (firstSample == 0) {
        for (HdRenderPassAovBinding const &binding : bindings)
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->BeginFrame(binding.clearValue, _AovKind(binding.aovName) == AovKind::Unknown);
        // Full-resolution RNG setup belongs to the render worker.  Camera motion
        // only pays for the bounded preview above on the UI/Execute thread.
        if (hasColor)
            _impl->committedRngStates.resize(static_cast<size_t>(width) * height);
        else
            std::vector<unsigned int>().swap(_impl->committedRngStates);
        if (hasColor && settings.adaptiveThreshold > 0.0f) {
            _impl->adaptiveSums.assign(static_cast<size_t>(width) * height * 7, 0.0f);
            _impl->adaptiveActive.Reset(width, height);
        } else {
            std::vector<float>().swap(_impl->adaptiveSums);
            _impl->adaptiveActive = {};
        }
    }
    prism::CameraRayParameters const cameraParameters =
        _CameraParameters(camera);
    prism::DumpCameraRayDiagnostics(
        cameraParameters, width, height, settings.seed, "hydra");
    // Offline renders: a device that streams textures loads the levels this
    // view asks for before the first sample, and keeps them for the whole
    // accumulation (deterministic images).
    if (firstSample == 0 && firstSample < sampleCount && hasColor && !previewingGpu &&
        settings.textureStreamingPrepass) {
        prism::device::PassRequest settle;
        settle.camera = cameraParameters;
        settle.width = width;
        settle.height = height;
        settle.seed = static_cast<unsigned int>(settings.seed);
        settle.shading.sampleClamp = settings.sampleClamp;
        settle.shading.nee = {settings.neeProbabilityMin, settings.neeFromDepth, settings.spatialLights};
        settle.shading.exposureScale = camera.linearExposureScale;
        settle.shading.transparentEscapes =
            snapshot->domeCount == 0 || !settings.domeLightCameraVisibility;
        prism::load_profile::Scope settleProfile("frame.texture_settle");
        activeDevice->SettleTextures(*snapshot->scene, settle);
        if (_impl->renderThread->IsStopRequested()) return;
    }
    struct ResolvedBinding { PrismRenderBuffer *buffer; AovKind kind; size_t plane; };
    struct AovPlane {
        AovKind kind;
        size_t components;
        std::vector<float> floats;
        std::vector<int> ints;
    };
    std::vector<ResolvedBinding> resolvedBindings;
    std::vector<AovPlane> primaryPlanes;
    const size_t scratchPixels = firstSample < sampleCount && (hasColor || hasGeometric)
        ? static_cast<size_t>(width) * height : 0;
    for (auto const &binding : bindings) {
        auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer);
        const AovKind kind = _AovKind(binding.aovName);
        size_t plane = 0;
        if (firstSample == 0 && buffer && kind != AovKind::Color && kind != AovKind::Unknown) {
            auto found = std::find_if(primaryPlanes.begin(), primaryPlanes.end(),
                [kind](auto const &p) { return p.kind == kind; });
            plane = static_cast<size_t>(found - primaryPlanes.begin());
            if (found == primaryPlanes.end()) {
                AovPlane staging{kind, _AovComponents(kind), {}, {}};
                if (_IntegerAov(kind)) staging.ints.resize(scratchPixels);
                else staging.floats.resize(scratchPixels * staging.components);
                primaryPlanes.push_back(std::move(staging));
            }
        }
        resolvedBindings.push_back({buffer, kind, plane});
    }
    std::vector<Color3> passColors(hasColor ? scratchPixels : 0);
    std::vector<float> passCoverage(hasColor ? scratchPixels : 0);
    std::vector<unsigned int> rngStates(hasColor ? scratchPixels : 0);
    {
        std::lock_guard lock(_impl->mutex);
        _impl->firstPassPrimaryScratchBytes = 0;
        for (auto const &plane : primaryPlanes)
            _impl->firstPassPrimaryScratchBytes += plane.floats.capacity() * sizeof(float) +
                plane.ints.capacity() * sizeof(int);
        _impl->colorPassScratchBytes = passColors.capacity() * sizeof(Color3) +
            passCoverage.capacity() * sizeof(float);
        _impl->colorRngStorageBytes = (rngStates.capacity() +
            _impl->committedRngStates.capacity()) * sizeof(unsigned int);
    }
    if (!hasColor && !hasGeometric) {
        if (_impl->renderThread->IsStopRequested()) return;
        if (firstSample == 0) {
            for (auto const &binding : resolvedBindings)
                if (binding.buffer) binding.buffer->Publish(frame);
            _impl->samples.store(1);
        }
        if (_impl->renderThread->IsStopRequested()) return;
        for (auto const &binding : resolvedBindings)
            if (binding.buffer) binding.buffer->SetConverged(!preparing);
        {
            std::lock_guard lock(_impl->mutex);
            _impl->lastPublishedFrame = frame;
            _impl->renderSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _impl->startTime).count();
        }
        _converged.store(!preparing);
        return;
    }
    struct DisplayTarget {
        PrismRenderBuffer *buffer;
        PrismRenderBuffer::ReadView base;
        std::vector<std::shared_ptr<const PrismRenderBuffer::PresentationTile>> tiles;
    };
    std::vector<DisplayTarget> displays;
    struct DisplayCleanup {
        std::vector<ResolvedBinding> const &targets;
        ~DisplayCleanup() { for (auto &t : targets) if (t.buffer) t.buffer->ClearPresentationPixels(); }
    } displayCleanup{resolvedBindings};
    if (_impl->tileWidth != width || _impl->tileHeight != height) {
        _impl->refinementTiles = MakeRefinementTiles(width, height);
        _impl->tileWidth = width; _impl->tileHeight = height;
    }
    auto const &tiles = _impl->refinementTiles;
    std::vector<prism::device::PixelTile> deviceTiles;
    deviceTiles.reserve(tiles.size());
    for (auto const &tile : tiles)
        deviceTiles.push_back({tile.x, tile.y, tile.width, tile.height, tile.index});
    uint64_t presentationRevision = 0;
    prism::device::AdaptiveSettings adaptive;
    adaptive.threshold = _impl->adaptiveSums.empty() ? 0.0f : settings.adaptiveThreshold;
    adaptive.minSamples = settings.adaptiveMinSamples;
    // Pixels traced by the current pass; converged pixels keep their value.
    auto traced = [&](size_t pixel) {
        return !adaptive.Enabled() || _impl->adaptiveActive.Active(pixel);
    };
    // Set once texture streaming restarted the accumulation: the geometric
    // outputs of the first pass are already published.
    bool restarted = false;
    for (int sample = firstSample; sample < sampleCount; ++sample) {
        if (adaptive.Enabled() && _impl->adaptiveActive.Count() == 0) break;
        if (_impl->renderThread->IsStopRequested()) return;
        while (_impl->renderThread->IsPauseRequested()) {
            if (_impl->renderThread->IsStopRequested()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Pass boundary: continue on the GPU from this sample index. The
        // per-pixel RNG state and the accumulated average carry over, so the
        // image keeps converging instead of restarting.
        if (previewingGpu) {
            // Test hook: the CPU preview renders at most this many samples
            // and then waits here for the GPU, so the handover happens
            // however long the GPU's preparation takes.
            if (const char *limit = std::getenv("PRISM_TEST_GPU_PREVIEW_MAX_SAMPLES");
                limit && *limit && std::atoi(limit) > 0 && sample >= std::atoi(limit)) {
                while (!_impl->AdvanceDevicePreparation(renderDevice, snapshot)) {
                    if (_impl->renderThread->IsStopRequested()) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (_impl->AdvanceDevicePreparation(renderDevice, snapshot)) {
                previewingGpu = false;
                activeDevice = renderDevice.get();
                _impl->gpuPreviewHandoverSample.store(static_cast<uint64_t>(sample));
                _impl->gpuPreviewHandovers.fetch_add(1);
                _impl->EndGpuPreview(*renderDevice, settings.device);
            } else {
                _impl->gpuPreviewSamples.fetch_add(1);
            }
        }
        // Texture streaming: once the levels the first samples asked for are
        // resident, the accumulation starts again (the device bounds how
        // often). Geometric outputs are kept: they do not sample textures.
        if (!previewingGpu && sample > 0 && hasColor &&
            activeDevice->AccumulationOutdated(*snapshot->scene)) {
            for (HdRenderPassAovBinding const &binding : bindings)
                if (binding.aovName == HdAovTokens->color)
                    if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                        buffer->BeginFrame(binding.clearValue, false);
            if (!_impl->adaptiveSums.empty()) {
                std::fill(_impl->adaptiveSums.begin(), _impl->adaptiveSums.end(), 0.0f);
                _impl->adaptiveActive.Reset(width, height);
            }
            _impl->samples.store(0);
            _impl->textureStreamingRestarts.fetch_add(1);
            prism::load_profile::Mark("frame.texture_restart", "sample=" + std::to_string(sample));
            restarted = true;
            sample = 0;
        }
        if (sample != 0)
            std::copy(_impl->committedRngStates.begin(),
                      _impl->committedRngStates.end(), rngStates.begin());
        displays.clear();
        for (auto const &binding : bindings) {
            if (binding.aovName != HdAovTokens->color) continue;
            auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer);
            if (!buffer || std::any_of(displays.begin(), displays.end(),
                    [buffer](auto const &t) { return t.buffer == buffer; })) continue;
            auto base = buffer->CapturePublished();
            if (base.format != HdFormatFloat32Vec4 || base.frame != frame || !base.pixels) continue;
            displays.push_back({buffer, base, {}});
            displays.back().tiles.resize(tiles.size());
        }
        const auto traceStart = std::chrono::steady_clock::now();
        using Clock = std::chrono::steady_clock;
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::vector<std::atomic<bool>> ready(tiles.size());
        for (auto &flag : ready) flag.store(false, std::memory_order_relaxed);
        std::vector<bool> emitted(tiles.size(), false);
        std::atomic<uint64_t> rayNanoseconds{0}, aovNanoseconds{0};
        std::atomic<bool> abandonPass{false};
        std::mutex completionMutex;
        std::condition_variable completion;
        bool finished = false;
        double waitMilliseconds = 0, payloadMilliseconds = 0, publicationMilliseconds = 0;
        prism::device::PassRequest passRequest;
        passRequest.camera = cameraParameters;
        passRequest.width = width;
        passRequest.height = height;
        passRequest.seed = static_cast<unsigned int>(settings.seed);
        passRequest.sample = sample;
        passRequest.shading.sampleClamp = settings.sampleClamp;
        passRequest.shading.nee = {settings.neeProbabilityMin, settings.neeFromDepth, settings.spatialLights};
        passRequest.shading.exposureScale = camera.linearExposureScale;
        passRequest.shading.transparentEscapes =
            snapshot->domeCount == 0 || !settings.domeLightCameraVisibility;
        passRequest.tiles = deviceTiles;
        passRequest.activePixels = adaptive.Enabled() ? _impl->adaptiveActive.Mask() : nullptr;
        passRequest.traceColor = hasColor;
        passRequest.tracePrimary = sample == 0 && !primaryPlanes.empty();
        passRequest.rngStates = rngStates.data();
        passRequest.colors = passColors.data();
        passRequest.coverage = passCoverage.data();
        passRequest.abandoned = [&] {
            return abandonPass.load(std::memory_order_relaxed) ||
                _impl->renderThread->IsStopRequested();
        };
        passRequest.paused = [&] { return _impl->renderThread->IsPauseRequested(); };
        passRequest.resolvePrimary = [&](unsigned x, unsigned y, RayHit const &hit) {
            const size_t pixel = static_cast<size_t>(y)*width+x;
            for (auto &plane : primaryPlanes)
                _ResolvePrimaryAov(plane.kind, hit, camera, snapshot.get(),
                    plane.floats.empty() ? nullptr : plane.floats.data()+pixel*plane.components,
                    plane.ints.empty() ? nullptr : plane.ints.data()+pixel);
        };
        passRequest.tileFinished = [&](size_t position) {
            ready[position].store(true, std::memory_order_release);
        };
        // Reserve execution capacity for the host and coordinator.
        passRequest.concurrency = std::max(1, tbb::this_task_arena::max_concurrency() - 2);
        // The CPU preview shares the machine with the GPU preparation (DXC and
        // driver compiles). Measured on 28 threads, 7 or more preview threads
        // slowed a 35 s preparation by 20-60%, 4 or fewer did not: keep an
        // eighth of the threads.
        if (previewingGpu)
            passRequest.concurrency = std::max(1, std::min(passRequest.concurrency,
                _impl->gpuPreviewThreads > 0 ? _impl->gpuPreviewThreads
                    : std::max(2, tbb::this_task_arena::max_concurrency() / 8)));
        passRequest.colorPixels = &_impl->colorTracedPixels;
        passRequest.geometricOnlyPixels = &_impl->geometricOnlyPixels;
        passRequest.rayNanoseconds = &rayNanoseconds;
        passRequest.primaryNanoseconds = &aovNanoseconds;
        // One pass-wide device job replaces the barrier after each 64 tiles.
        // The coordinator alone owns presentation packets. Acquire/release
        // completion flags make only fully written tiles visible to it.
        prism::device::RenderDevice *const device = activeDevice;
        auto worker = std::async(std::launch::async, [&] {
            auto finish = [&] {
                { std::lock_guard lock(completionMutex); finished = true; }
                completion.notify_one();
            };
            try {
                device->RenderPass(*snapshot->scene, passRequest);
            } catch (...) { finish(); throw; }
            finish();
        });
        struct DrainPass {
            std::atomic<bool> &abandon;
            std::future<void> &worker;
            ~DrainPass() {
                // Packing/allocation can throw while workers are paused.
                // Unblock them before the future's destructor waits, and
                // before any of the captured pass storage is destroyed.
                abandon.store(true, std::memory_order_relaxed);
                if (worker.valid()) worker.wait();
            }
        } drainPass{abandonPass, worker};
        auto nextDisplay = traceStart + std::chrono::milliseconds(100);
        for (;;) {
            const auto waitStart = Clock::now();
            bool done;
            {
                std::unique_lock lock(completionMutex);
                completion.wait_until(lock, nextDisplay, [&] { return finished; });
                done = finished;
            }
            waitMilliseconds += milliseconds(Clock::now()-waitStart);
            if (done) break;
            if (!_impl->renderThread->IsStopRequested() && !displays.empty()) {
                const auto payloadStart = Clock::now();
                std::vector<size_t> completed;
                for (size_t i = 0; i < tiles.size(); ++i)
                    if (!emitted[i] && ready[i].load(std::memory_order_acquire)) {
                        completed.push_back(i); emitted[i] = true;
                    }
                // Packing a publication is bounded memory work. Running it
                // here avoids entering the render scheduler while the host
                // is waiting for the next immutable display packet.
                for (size_t j = 0; j != completed.size(); ++j) {
                    auto const &tile = tiles[completed[j]];
                    for (auto &target : displays) {
                        auto payload = std::make_shared<PrismRenderBuffer::PresentationTile>();
                        payload->x = tile.x; payload->y = height - tile.y - tile.height;
                        payload->width = tile.width; payload->height = tile.height;
                        payload->rgba.resize(static_cast<size_t>(tile.width) * tile.height * 4);
                        auto const *previous = reinterpret_cast<float const *>(target.base.pixels->data());
                        for (unsigned y = 0; y < tile.height; ++y) {
                            for (unsigned x = 0; x < tile.width; ++x) {
                                const size_t pixel = static_cast<size_t>(tile.y + y) * width + tile.x + x;
                                const size_t offset = (static_cast<size_t>(height - 1 - tile.y - y) * width + tile.x + x) * 4;
                                float *dst = payload->rgba.data() + (static_cast<size_t>(tile.height - 1 - y) * tile.width + x) * 4;
                                auto const &c = passColors[pixel];
                                const float values[4] = {c.x(), c.y(), c.z(), passCoverage[pixel]};
                                const bool fresh = traced(pixel);
                                for (size_t component = 0; component < 4; ++component)
                                    dst[component] = !fresh ? previous[offset + component] :
                                        sample == 0 ? values[component] :
                                        (previous[offset + component] * sample + values[component]) / (sample + 1);
                            }
                        }
                        target.tiles[tile.index] = std::move(payload);
                    }
                }
                payloadMilliseconds += milliseconds(Clock::now()-payloadStart);
                if (!completed.empty() && !_impl->renderThread->IsStopRequested()) {
                    const auto publicationStart = Clock::now();
                    ++presentationRevision;
                    for (auto &target : displays) {
                        auto packet = std::make_shared<PrismRenderBuffer::PresentationSnapshot>();
                        packet->base = target.base;
                        packet->pass = static_cast<uint64_t>(sample);
                        packet->revision = presentationRevision;
                        packet->tiles = target.tiles;
                        target.buffer->SetPresentationSnapshot(std::move(packet));
                    }
                    { std::lock_guard lock(_impl->mutex); ++_impl->refinementPublishes; }
                    publicationMilliseconds += milliseconds(Clock::now()-publicationStart);
                }
            }
            // Keep cadence anchored to the pass, rather than adding payload
            // construction time to every interval. If packing overran an
            // interval, consume new completions immediately instead of queuing
            // a backlog of timer events.
            nextDisplay = std::max(nextDisplay + std::chrono::milliseconds(100), Clock::now());
        }
        // Also propagates worker errors; never release worker-owned memory
        // while cancellation or an exception is still draining the TBB job.
        worker.get();
        if (_impl->renderThread->IsStopRequested()) return;
        const auto commitStart = std::chrono::steady_clock::now();
        // Release previous raw image references so publication can recycle its storage.
        // The buffer keeps the immutable display override alive until Publish.
        displays.clear();
        // Commit is bounded buffer work: finish it even if a stop arrives.
        // The next RNG state and sample count advance only with publication.
        tbb::parallel_for(tbb::blocked_range2d<unsigned int>(0, height, 8, 0, width, 8),
            [&](tbb::blocked_range2d<unsigned int> const &range) {
                std::vector<uint64_t> tileWriteNanoseconds(
                    _impl->profileBufferWrites ? bindings.size() : 0, 0);
                for (unsigned int y = range.rows().begin(); y != range.rows().end(); ++y) {
                    for (unsigned int x = range.cols().begin(); x != range.cols().end(); ++x) {
                        const size_t pixel = static_cast<size_t>(y) * width + x;
                        // Hydra's render buffers are addressed bottom-up. Camera sampling
                        // remains top-down so the generated rays match standalone PrismRT.
                        const unsigned int bufferY = height - 1 - y;
                        for (size_t bindingIndex = 0;
                             bindingIndex < bindings.size(); ++bindingIndex) {
                            auto const &resolved = resolvedBindings[bindingIndex];
                            const AovKind kind = resolved.kind;
                            if ((sample != 0 || restarted) && kind != AovKind::Color)
                                continue;
                            auto *buffer = resolved.buffer;
                            if (!buffer) continue;
                            const auto writeStart = _impl->profileBufferWrites
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
                            if (kind == AovKind::Color) {
                                if (!traced(pixel)) continue;
                                Color3 const &c = passColors[pixel];
                                float v[4] = {c.x(), c.y(), c.z(), passCoverage[pixel]};
                                buffer->WriteFloat(x, bufferY, v, 4, true);
                            } else if (kind != AovKind::Unknown && sample == 0) {
                                auto const &plane = primaryPlanes[resolved.plane];
                                if (_IntegerAov(kind))
                                    buffer->WriteInt(x, bufferY, plane.ints.data() + pixel, 1);
                                else
                                    buffer->WriteFloat(x, bufferY,
                                        plane.floats.data() + pixel * plane.components, plane.components, false);
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
                        if (auto *buffer = resolvedBindings[i].buffer)
                            buffer->RecordWriteNanoseconds(tileWriteNanoseconds[i]);
                    }
                }
            });
        for (HdRenderPassAovBinding const &binding : bindings) {
            // Geometric AOVs are deterministic and only written on sample 0.
            // Color changes every pass as accumulation progresses.
            if ((sample != 0 || restarted) && binding.aovName != HdAovTokens->color) continue;
            if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
                buffer->Publish(frame);
        }
        {
            std::lock_guard lock(_impl->mutex);
            _impl->lastPublishedFrame = frame;
            _impl->lastPassTraceMilliseconds = std::chrono::duration<double,std::milli>(commitStart-traceStart).count();
            _impl->lastPassCommitMilliseconds = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-commitStart).count();
            _impl->lastPassRayWorkerMilliseconds = rayNanoseconds.load()*1e-6;
            _impl->lastPassAovWorkerMilliseconds = aovNanoseconds.load()*1e-6;
            _impl->lastPassWaitMilliseconds = waitMilliseconds;
            _impl->lastPassPayloadMilliseconds = payloadMilliseconds;
            _impl->lastPassPublicationMilliseconds = publicationMilliseconds;
        }
        if (adaptive.Enabled()) {
            float *sums = _impl->adaptiveSums.data();
            tbb::parallel_for(size_t(0), passColors.size(), [&](size_t pixel) {
                if (!traced(pixel)) return;
                Color3 const &c = passColors[pixel];
                for (int channel = 0; channel < 3; ++channel) {
                    sums[7 * pixel + channel] += c[channel];
                    if (sample % 2 == 0) sums[7 * pixel + 3 + channel] += c[channel];
                }
                sums[7 * pixel + 6] += 1.0f;
            });
            if (sample + 1 < sampleCount && adaptive.CheckAfter(sample + 1)) {
                const float scale = prism::device::ImageScale(passColors.size(), [&](size_t pixel) {
                    const float* p = sums + 7 * pixel;
                    return p[6] > 0.0f ? (p[0] + p[1] + p[2]) / p[6] : 0.0f;
                });
                _impl->adaptiveActive.Update(adaptive.threshold, [&](size_t pixel) {
                    return prism::device::AdaptiveError(sums + 7 * pixel, sums + 7 * pixel + 3, sample + 1, scale);
                });
            }
        }
        _impl->committedRngStates.swap(rngStates);
        _impl->samples.store(static_cast<uint64_t>(sample + 1));
        if (sample == 0 && !restarted) {
            std::vector<AovPlane>().swap(primaryPlanes);
            prism::load_profile::Mark("frame.full_sample", "generation=" + std::to_string(generation));
        }
    }
    if (_impl->renderThread->IsStopRequested()) return;
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

prism::Progress::Snapshot SceneBridge::GetProgress() const
{
    return _impl->progress.Read();
}

VtDictionary SceneBridge::GetProgressStats() const
{
    const auto progress = _impl->progress.Read();
    using prism::Phase;
    auto count = [&](Phase phase, bool total) {
        auto const &state = progress.phases[size_t(phase)];
        return VtValue(static_cast<int>(std::min<uint64_t>(
            total ? state.total : state.done, std::numeric_limits<int>::max())));
    };
    VtDictionary result;
    result["prismPhase"] = VtValue(progress.phase);
    result["prismPhaseDetail"] = VtValue(progress.detail);
    result["prismPhaseSeconds"] = VtValue(progress.phaseSeconds);
    result["prismDevice"] = VtValue(progress.device);
    result["prismError"] = VtValue(progress.error);
    // True while the CPU device renders a preview for a GPU that is preparing.
    result["prismGpuPreview"] = VtValue(_impl->gpuPreviewActive.load(std::memory_order_relaxed));
    std::string active;
    for (size_t i = 0; i < progress.phases.size(); ++i) {
        if (!progress.phases[i].active) continue;
        if (!active.empty()) active += ',';
        active += prism::PhaseName(Phase(i));
    }
    result["prismActivePhases"] = VtValue(active);
    result["prismMeshesDone"] = count(Phase::Loading, false);
    result["prismMeshesTotal"] = count(Phase::Loading, true);
    result["prismMaterialsDone"] = count(Phase::Materials, false);
    result["prismMaterialsTotal"] = count(Phase::Materials, true);
    result["prismTexturesDone"] = count(Phase::Textures, false);
    result["prismTexturesTotal"] = count(Phase::Textures, true);
    result["prismUploadDone"] = count(Phase::Upload, false);
    result["prismUploadTotal"] = count(Phase::Upload, true);
    return result;
}

VtDictionary SceneBridge::GetStats() const
{
    // Progress first and lock-free: it must stay current while the bridge
    // mutex below is busy.
    VtDictionary result = GetProgressStats();
    const int64_t completed = static_cast<int64_t>(_impl->samples.load());
    const bool converged = _converged.load();
    result["completedSamples"] = VtValue(completed);
    result["converged"] = VtValue(converged);
    std::lock_guard lock(_impl->mutex);
    // Standard Hydra progress keys that hosts such as usdview read:
    // numCompletedSamples (int) and percentDone (double, 0-100). A converged
    // pass reports 100 even below the target (adaptive early-out).
    const int targetSamples = std::max(1, _impl->settings.samples);
    result["numCompletedSamples"] = VtValue(static_cast<int>(
        std::min<int64_t>(completed, std::numeric_limits<int>::max())));
    result["targetSamples"] = VtValue(targetSamples);
    result["percentDone"] = VtValue(converged ? 100.0 :
        std::clamp(100.0 * static_cast<double>(completed) / targetSamples, 0.0, 100.0));
    result["refinementPublishes"] = VtValue(static_cast<int64_t>(_impl->refinementPublishes));
    result["devicePreparing"] = VtValue(_impl->devicePreparedEpoch != _impl->deviceSceneEpoch);
    result["devicePreparations"] = VtValue(static_cast<int64_t>(_impl->devicePreparations.load()));
    result["devicePreparationsInterrupted"] = VtValue(
        static_cast<int64_t>(_impl->devicePreparationsInterrupted.load()));
    result["devicePreparationSeconds"] = VtValue(_impl->devicePreparationSeconds);
    result["deferredEnvironmentCaptures"] = VtValue(
        static_cast<int64_t>(_impl->deferredEnvironmentCaptures));
    result["gpuPreviewPasses"] = VtValue(static_cast<int64_t>(_impl->gpuPreviewSamples.load()));
    result["gpuPreviewHandovers"] = VtValue(static_cast<int64_t>(_impl->gpuPreviewHandovers.load()));
    result["gpuPreviewHandoverSample"] = VtValue(static_cast<int64_t>(_impl->gpuPreviewHandoverSample.load()));
    // GPU texture streaming (see docs/texture_service.md#gpu-texture-streaming).
    if (_impl->renderDevice) {
        const prism::device::TextureStreamingStats streaming = _impl->renderDevice->TextureStreaming();
        constexpr double mib = 1.0 / 1048576.0;
        result["prismTextureStreaming"] = VtValue(streaming.active);
        result["prismTextureResidentMiB"] = VtValue(double(streaming.residentBytes) * mib);
        result["prismTextureBudgetMiB"] = VtValue(double(streaming.budgetBytes) * mib);
        result["prismTextureMemoryMiB"] = VtValue(double(streaming.imageBytes) * mib);
        result["prismDeviceMemoryMiB"] = VtValue(double(streaming.deviceBytes) * mib);
        result["prismTexturesStreamed"] = VtValue(static_cast<int>(streaming.images));
        result["prismTexturesRequested"] = VtValue(static_cast<int>(streaming.requested));
        result["prismTexturesPending"] = VtValue(static_cast<int>(streaming.pending));
        result["prismTexturesBelowRequested"] = VtValue(static_cast<int>(streaming.belowRequested));
        result["textureStreamingUploads"] = VtValue(static_cast<int64_t>(streaming.uploads));
        result["textureStreamingUploadedMiB"] = VtValue(double(streaming.uploadedBytes) * mib);
        result["textureStreamingFeedbackRounds"] = VtValue(static_cast<int64_t>(streaming.feedbackRounds));
    }
    result["textureStreamingRestarts"] = VtValue(static_cast<int64_t>(_impl->textureStreamingRestarts.load()));
    result["firstPassPrimaryScratchBytes"] = VtValue(static_cast<int64_t>(_impl->firstPassPrimaryScratchBytes));
    result["colorTracedPixels"] = VtValue(static_cast<int64_t>(_impl->colorTracedPixels.load()));
    result["geometricOnlyPixels"] = VtValue(static_cast<int64_t>(_impl->geometricOnlyPixels.load()));
    result["colorPassScratchBytes"] = VtValue(static_cast<int64_t>(_impl->colorPassScratchBytes));
    result["colorRngStorageBytes"] = VtValue(static_cast<int64_t>(_impl->colorRngStorageBytes));
    result["lastPassTraceMilliseconds"] = VtValue(_impl->lastPassTraceMilliseconds);
    result["lastPassCommitMilliseconds"] = VtValue(_impl->lastPassCommitMilliseconds);
    result["lastPassPublicationMilliseconds"] = VtValue(_impl->lastPassPublicationMilliseconds);
    result["lastPassPayloadMilliseconds"] = VtValue(_impl->lastPassPayloadMilliseconds);
    result["lastPassWaitMilliseconds"] = VtValue(_impl->lastPassWaitMilliseconds);
    result["lastPassAovWorkerMilliseconds"] = VtValue(_impl->lastPassAovWorkerMilliseconds);
    result["lastPassRayWorkerMilliseconds"] = VtValue(_impl->lastPassRayWorkerMilliseconds);
    result["meshCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->meshCount : 0));
    result["triangleCount"] = VtValue(
        _impl->snapshot ? _impl->snapshot->triangleCount : int64_t{0});
    result["primCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->livePrimCount : 0));
    result["primPathSlotCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->primPathSlotCount : 0));
    result["snapshotBuilds"] = VtValue(static_cast<int64_t>(
        _impl->snapshotBuilds));
    result["topLevelSceneBuilds"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->topLevelSceneBuilds.load()));
    result["topLevelSceneCommits"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->topLevelSceneCommits.load()));
    result["lastCanonicalRecordsVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastCanonicalRecordsVisited : 0));
    result["lastPickingRecordsVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPickingRecordsVisited : 0));
    result["lastCountRecordsVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastCountRecordsVisited : 0));
    result["lastInputMetadataPrototypesVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastInputMetadataPrototypesVisited : 0));
    result["lastDependencyPlacementsVisited"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastDependencyPlacementsVisited : 0));
    result["lastCanonicalMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastCanonicalMilliseconds : 0.0);
    result["lastPickingMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastPickingMilliseconds : 0.0);
    result["lastCountsMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastCountsMilliseconds : 0.0);
    result["lastDependencyMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastDependencyMilliseconds : 0.0);
    result["lastInputMetadataMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastInputMetadataMilliseconds : 0.0);
    result["lastInputComparisonMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastInputComparisonMilliseconds : 0.0);
    result["lastPlacementReconcileMilliseconds"] = VtValue(
        _impl->snapshot ? _impl->snapshot->lastPlacementReconcileMilliseconds : 0.0);
    result["lastPlacementTransformsUpdated"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPlacementTransformsUpdated : 0));
    result["lastPrototypePlacementsRepointed"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPrototypePlacementsRepointed : 0));
    result["lastPrototypePathsUpdated"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPrototypePathsUpdated : 0));
    result["placementTopologyFallbacks"] = VtValue(static_cast<int64_t>(_impl->placementTopologyFallbacks));
    result["structuralLeafUpdates"] = VtValue(static_cast<int64_t>(_impl->structuralLeafUpdates));
    result["structuralPlacementDetachCalls"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->structuralPlacementDetachCalls.load()));
    result["structuralFallbacks"] = VtValue(static_cast<int64_t>(_impl->structuralFallbacks));
    result["lastStructuralPathsUpdated"] = VtValue(static_cast<int64_t>(_impl->snapshot ? _impl->snapshot->lastStructuralPathsUpdated : 0));
    result["lastStructuralPlacementsAttached"] = VtValue(static_cast<int64_t>(_impl->snapshot ? _impl->snapshot->lastStructuralPlacementsAttached : 0));
    result["lastStructuralPlacementsDetached"] = VtValue(static_cast<int64_t>(_impl->snapshot ? _impl->snapshot->lastStructuralPlacementsDetached : 0));
    result["prototypeUpdateFallbacks"] = VtValue(static_cast<int64_t>(_impl->prototypeUpdateFallbacks));
    result["enabledPlacementCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->enabledPlacementCount : 0));
    {
        std::lock_guard poolLock(_impl->prototypePool->mutex);
        auto const usage = _HiddenGeometryUsage(*_impl->prototypePool);
        const uint64_t budget = _impl->prototypePool->hiddenGeometryBudgetBytes.load();
        result["hiddenGeometryBudgetBytes"] = VtValue(static_cast<int64_t>(budget));
        result["hiddenGeometryResidentBytes"] = VtValue(static_cast<int64_t>(usage.residentBytes));
        result["hiddenGeometryPinnedBytes"] = VtValue(static_cast<int64_t>(usage.pinnedBytes));
        result["hiddenGeometryBudgetOverageBytes"] = VtValue(static_cast<int64_t>(
            budget && usage.residentBytes > budget ? usage.residentBytes - budget : 0));
    }
    result["hiddenGeometryEvictions"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->hiddenGeometryEvictions.load()));
    result["hiddenGeometryRestorations"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->hiddenGeometryRestorations.load()));
    result["evictedPrototypeCount"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->evictedPrototypes.size() : 0));
    result["snapshotBuildSeconds"] = VtValue(_impl->snapshotBuildSeconds);
    result["firstPublishSeconds"] = VtValue(_impl->firstPublishSeconds);
    result["spareSnapshotSeconds"] = VtValue(_impl->spareSnapshotSeconds);
    // wait_for does not consume the result; Prepare still installs it.
    result["spareSnapshotReady"] = VtValue(
        _impl->buildIsSpare && _impl->buildFuture.valid() &&
        _impl->buildFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready);
    result["spareSnapshotsInstalled"] = VtValue(static_cast<int64_t>(
        _impl->spareSnapshotsInstalled));
    result["spareSnapshotsDiscarded"] = VtValue(static_cast<int64_t>(
        _impl->spareSnapshotsDiscarded));
    result["sparePrototypeConversions"] = VtValue(static_cast<int64_t>(
        _impl->sparePrototypeConversions));
    result["sparePrototypeConversionCacheHits"] = VtValue(static_cast<int64_t>(
        _impl->sparePrototypeConversionCacheHits));
    const auto textureStats = _impl->textureService->stats();
    result["processTextureSourceBytesHashed"] = VtValue(static_cast<int64_t>(textureStats.sourceBytesHashed));
    result["processTextureHashCalls"] = VtValue(static_cast<int64_t>(textureStats.resourceHashCalls));
    result["processTextureSourceWaitSeconds"] = VtValue(textureStats.sourceWaitNanoseconds / 1.0e9);
    result["processTextureContentWaitSeconds"] = VtValue(textureStats.contentWaitNanoseconds / 1.0e9);
    result["processTextureColdPreparations"] = VtValue(static_cast<int64_t>(textureStats.coldPreparations));
    result["processTextureDiskHits"] = VtValue(static_cast<int64_t>(textureStats.diskCacheHits));
    result["processTextureMemoryHits"] = VtValue(static_cast<int64_t>(textureStats.memoryCacheHits));
    result["processTextureInspectSeconds"] = VtValue(textureStats.inspectNanoseconds / 1.0e9);
    result["processTextureDecodeSeconds"] = VtValue(textureStats.decodeNanoseconds / 1.0e9);
    result["processTexturePixelSeconds"] = VtValue(textureStats.pixelNanoseconds / 1.0e9);
    result["processTextureMipSeconds"] = VtValue(textureStats.mipNanoseconds / 1.0e9);
    result["processTexturePublishSeconds"] = VtValue(textureStats.publishNanoseconds / 1.0e9);
    result["processTextureTrimSeconds"] = VtValue(textureStats.trimNanoseconds / 1.0e9);
    result["processTextureLockWaitSeconds"] = VtValue(textureStats.lockWaitNanoseconds / 1.0e9);
    result["processTextureAheadRequests"] = VtValue(static_cast<int64_t>(textureStats.aheadRequests));
    result["processTextureAheadPreparations"] = VtValue(static_cast<int64_t>(textureStats.aheadPreparations));
    result["processTextureAheadMemoryHits"] = VtValue(static_cast<int64_t>(textureStats.aheadMemoryHits));
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
    result["previewCostMilliseconds"] = VtValue(_impl->previewCostMilliseconds);
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
    result["lastInstanceArrayElementsScanned"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastInstanceArrayElementsScanned : 0));
    result["lastPlacementTopologyAttached"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPlacementTopologyAttached : 0));
    result["lastPlacementTopologyDetached"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->lastPlacementTopologyDetached : 0));
    result["lastVisibilityPlacementsUpdated"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ?
            _impl->snapshot->lastVisibilityPlacementsUpdated : 0));
    result["prototypeBuildCount"] = VtValue(static_cast<int64_t>(
        _impl->prototypeBuilds));
    result["prototypeConversions"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->prototypeConversions : 0));
    result["prototypeConversionCacheHits"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->prototypeConversionCacheHits : 0));
    result["speculativeMeshRetries"] = VtValue(static_cast<int64_t>(
        _impl->snapshot ? _impl->snapshot->speculativeMeshRetries : 0));
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
    result["lastPlacementDirtyPathCount"] = VtValue(static_cast<int64_t>(_impl->lastPlacementDirtyPathCount));
    result["lastPrototypeDirtyPathCount"] = VtValue(static_cast<int64_t>(_impl->lastPrototypeDirtyPathCount));
    result["lastMaterialDirtyPathCount"] = VtValue(static_cast<int64_t>(_impl->lastMaterialDirtyPathCount));
    result["prototypeInvalidations"] = VtValue(static_cast<int64_t>(_impl->prototypeInvalidations));
    result["materialInvalidations"] = VtValue(static_cast<int64_t>(_impl->materialInvalidations));
    result["environmentInvalidations"] = VtValue(static_cast<int64_t>(_impl->environmentInvalidations));
    result["environmentDistributionBuilds"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->environmentCache.BuildCount()));
    result["environmentDistributionCacheHits"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->environmentCache.HitCount()));
    result["sharedEnvironmentImportanceBytes"] = VtValue(static_cast<int64_t>(
        _impl->prototypePool->environmentCache.LiveImportanceBytes()));
    result["conservativeRebuilds"] = VtValue(static_cast<int64_t>(_impl->conservativeRebuilds));
    result["pinnedSnapshotFallbacks"] = VtValue(static_cast<int64_t>(_impl->pinnedSnapshotFallbacks));
    result["visibilityRecoveryRebuilds"] = VtValue(static_cast<int64_t>(
        _impl->visibilityRecoveryRebuilds));
    result["lastRebuildReason"] = VtValue(_impl->lastRebuildReason);
    // Why the current scene could not be prepared; empty while it can be.
    result["snapshotError"] = VtValue(
        _impl->failedGeneration == _impl->requestedGeneration
            ? _impl->generationFailure : std::string());
    uint64_t writeNs = 0, publishNs = 0, mapNs = 0, presentationCopyBytes = 0;
    std::unordered_set<PrismRenderBuffer *> measuredBuffers;
    for (auto const &binding : _impl->bindings) {
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer)) {
            if (!measuredBuffers.insert(buffer).second) continue;
            writeNs += buffer->GetWriteNanoseconds();
            publishNs += buffer->GetPublishNanoseconds();
            mapNs += buffer->GetMapWaitNanoseconds();
            presentationCopyBytes += buffer->GetPresentationCopyBytes();
        }
    }
    result["bufferWriteProfilingEnabled"] = VtValue(
        _impl->profileBufferWrites);
    result["presentationCopyBytes"] = VtValue(static_cast<int64_t>(presentationCopyBytes));
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
        result["materialPrewarmSeconds"] = VtValue(stats.prewarmSeconds);
        result["materialPrewarmWaitSeconds"] = VtValue(stats.prewarmWaitSeconds);
    }
    return result;
}

} // namespace prism::hydra
