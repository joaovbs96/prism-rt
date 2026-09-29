// Material schema tokens include `interface`, which Windows headers pulled in
// by the renderer define as a macro; include them first.
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/utils.h>
#include "../../src/hydra/scene_bridge.h"
#include "../../src/hydra/render_buffer.h"
#include "../../src/hydra/native_tasks.h"
#include "../../src/hydra/triangulation.h"
#include "../../src/scene.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/purposeSchema.h>
#include <pxr/imaging/hd/renderThread.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>

#include <cstdint>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class OwnerCheckedPoints final
    : public HdTypedSampledDataSource<VtVec3fArray>
{
public:
    OwnerCheckedPoints(VtVec3fArray values, std::thread::id owner,
                       std::atomic<bool> *wrongThread)
        : _values(std::move(values)), _owner(owner),
          _wrongThread(wrongThread) {}
    VtValue GetValue(Time) override
    {
        _Check();
        return VtValue(_values);
    }
    VtVec3fArray GetTypedValue(Time) override
    {
        _Check();
        return _values;
    }
    bool GetContributingSampleTimesForInterval(
        Time, Time, std::vector<Time> *) override { return false; }
private:
    void _Check()
    {
        if (std::this_thread::get_id() != _owner)
            _wrongThread->store(true);
    }
    VtVec3fArray _values;
    std::thread::id _owner;
    std::atomic<bool> *_wrongThread;
};

HdContainerDataSourceHandle FixtureMeshData(
    bool holeSecondFace,
    TfToken const &purpose = HdRenderTagTokens->geometry,
    GfVec3f const &displayColor = GfVec3f(0.7f),
    float pointOffset = 0.0f,
    HdVec3fArrayDataSourceHandle const &pointsOverride = {})
{
    const VtVec3fArray points{{-1 + pointOffset, -1, 0}, {1, -1, 0}, {1, 1, 0},
                              {-1, 1, 0}, {0, 0, 1}};
    HdContainerDataSourceHandle pointsPrimvar = HdPrimvarSchema::Builder()
        .SetPrimvarValue(pointsOverride ? pointsOverride :
            HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->vertex))
        .Build();
    HdContainerDataSourceHandle colorPrimvar = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            VtVec3fArray{displayColor}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->constant))
        .Build();
    HdContainerDataSourceHandle primvars = HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->points, pointsPrimvar,
        TfToken("displayColor"), colorPrimvar);
    HdContainerDataSourceHandle topology = HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(
            VtIntArray{4, 3}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(
            VtIntArray{0, 1, 2, 3, 0, 2, 4}))
        .SetHoleIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(
            holeSecondFace ? VtIntArray{1} : VtIntArray{}))
        .SetOrientation(HdMeshTopologySchema::BuildOrientationDataSource(
            HdMeshTopologySchemaTokens->rightHanded))
        .Build();
    return HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->primvars, primvars,
        HdMeshSchemaTokens->mesh,
        HdMeshSchema::Builder().SetTopology(topology).Build(),
        HdPurposeSchemaTokens->purpose,
        HdPurposeSchema::Builder()
            .SetPurpose(HdRetainedTypedSampledDataSource<TfToken>::New(purpose))
            .Build());
}

HdContainerDataSourceHandle InstancedMeshData(
    bool holeSecondFace = true,
    GfVec3f const &displayColor = GfVec3f(0.7f),
    float pointOffset = 0.0f)
{
    HdContainerDataSourceHandle instancedBy = HdInstancedBySchema::Builder()
        .SetPaths(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
            VtArray<SdfPath>{SdfPath("/Instancer")}))
        .SetPrototypeRoots(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
            VtArray<SdfPath>{SdfPath("/Proto")}))
        .Build();
    return HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(
            HdInstancedBySchemaTokens->instancedBy, instancedBy),
        FixtureMeshData(holeSecondFace, HdRenderTagTokens->geometry,
                        displayColor, pointOffset));
}

HdContainerDataSourceHandle DomeData()
{
    HdContainerDataSourceHandle light = HdRetainedContainerDataSource::New(
        HdLightTokens->color,
        HdRetainedTypedSampledDataSource<GfVec3f>::New(GfVec3f(1.0f)),
        HdLightTokens->intensity,
        HdRetainedTypedSampledDataSource<float>::New(1.0f));
    return HdRetainedContainerDataSource::New(
        HdLightSchemaTokens->light, light);
}

HdContainerDataSourceHandle FixtureSphereData(
    double radius, GfVec3d const &translation = GfVec3d(0.0),
    GfVec3f const &color = GfVec3f(0.7f, 0.3f, 0.1f))
{
    HdContainerDataSourceHandle colorPrimvar = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            VtVec3fArray{color}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->constant))
        .Build();
    GfMatrix4d matrix(1.0);
    matrix.SetTranslate(translation);
    return HdRetainedContainerDataSource::New(
        HdSphereSchemaTokens->sphere,
        HdSphereSchema::Builder()
            .SetRadius(HdRetainedTypedSampledDataSource<double>::New(radius))
            .Build(),
        HdPrimvarsSchemaTokens->primvars,
        HdRetainedContainerDataSource::New(
            TfToken("displayColor"), colorPrimvar),
        HdXformSchemaTokens->xform,
        HdXformSchema::Builder()
            .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix))
            .Build());
}

HdContainerDataSourceHandle FixtureXformData(GfVec3d const &translation)
{
    GfMatrix4d matrix(1.0);
    matrix.SetTranslate(translation);
    return HdRetainedContainerDataSource::New(
        HdXformSchemaTokens->xform,
        HdXformSchema::Builder()
            .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix))
            .Build());
}

HdContainerDataSourceHandle InstancerData(double offset = 1.0,
                                          bool visible = true)
{
    HdDataSourceBaseHandle const instanceIndexArrays[] = {
        HdRetainedTypedSampledDataSource<VtIntArray>::New(VtIntArray{1, 3})};
    HdContainerDataSourceHandle topology = HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
            VtArray<SdfPath>{SdfPath("/Proto")}))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(
            1, instanceIndexArrays))
        .Build();
    HdContainerDataSourceHandle translations = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            VtVec3fArray{{0, 0, 0}, {1, 0, 0}, {0, 0, 0}, {3, 0, 0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->instance))
        .Build();
    HdContainerDataSourceHandle scales = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            VtVec3fArray{{1, 1, 1}, {2, 1, 0.5f}, {1, 1, 1}, {-1, 1, 1}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->instance))
        .Build();
    const float halfAngle = 0.174532925f;
    HdContainerDataSourceHandle rotations = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtArray<GfQuatf>>::New(
            VtArray<GfQuatf>{{1.0f, GfVec3f(0)},
                {std::cos(halfAngle), GfVec3f(0, std::sin(halfAngle), 0)},
                {1.0f, GfVec3f(0)}, {1.0f, GfVec3f(0)}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->instance))
        .Build();
    HdContainerDataSourceHandle primvars = HdRetainedContainerDataSource::New(
        HdInstancerTokens->instanceTranslations, translations,
        HdInstancerTokens->instanceScales, scales,
        HdInstancerTokens->instanceRotations, rotations);
    GfMatrix4d instancerMatrix(1.0);
    instancerMatrix.SetTranslate(GfVec3d(offset, 0, 0));
    HdContainerDataSourceHandle xform = HdXformSchema::Builder()
        .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(
            instancerMatrix))
        .Build();
    return HdRetainedContainerDataSource::New(
        HdInstancerTopologySchemaTokens->instancerTopology, topology,
        HdPrimvarsSchemaTokens->primvars, primvars,
        HdXformSchemaTokens->xform, xform,
        HdVisibilitySchemaTokens->visibility,
        HdVisibilitySchema::Builder()
            .SetVisibility(
                HdRetainedTypedSampledDataSource<bool>::New(visible))
            .Build());
}

HdContainerDataSourceHandle VisibilityMeshData(bool visible, float pointOffset)
{
    return HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(
            HdVisibilitySchemaTokens->visibility,
            HdVisibilitySchema::Builder()
                .SetVisibility(
                    HdRetainedTypedSampledDataSource<bool>::New(visible))
                .Build()),
        FixtureMeshData(false, HdRenderTagTokens->geometry, GfVec3f(0.7f),
                        pointOffset));
}

HdContainerDataSourceHandle PreviewMaterialData(SdfPath const &path,
                                                GfVec3f const &color)
{
    HdMaterialNode preview;
    preview.path = path.AppendChild(TfToken("Preview"));
    preview.identifier = TfToken("UsdPreviewSurface");
    preview.parameters[TfToken("diffuseColor")] = VtValue(color);
    HdMaterialNetworkMap networkMap;
    networkMap.map[TfToken("surface")].nodes = {preview};
    HdDataSourceBaseHandle const network =
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(networkMap);
    TfToken const context = HdMaterialSchemaTokens->universalRenderContext;
    return HdRetainedContainerDataSource::New(
        HdMaterialSchemaTokens->material,
        HdMaterialSchema::BuildRetained(1, &context, &network));
}

HdContainerDataSourceHandle FaceSubsetData(VtIntArray const &faces,
                                           SdfPath const &material)
{
    HdDataSourceBaseHandle const binding = HdMaterialBindingSchema::Builder()
        .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(material))
        .Build();
    TfToken const purpose = HdMaterialBindingsSchemaTokens->allPurpose;
    return HdRetainedContainerDataSource::New(
        HdGeomSubsetSchema::GetSchemaToken(),
        HdGeomSubsetSchema::Builder()
            .SetType(HdGeomSubsetSchema::BuildTypeDataSource(
                HdGeomSubsetSchemaTokens->typeFaceSet))
            .SetIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(faces))
            .Build(),
        HdMaterialBindingsSchema::GetSchemaToken(),
        HdMaterialBindingsSchema::BuildRetained(1, &purpose, &binding));
}

// 64 distinct meshes, two identical paths plus an instanced prototype with
// the same geometry, a mesh whose geom subsets bind two materials, and a
// hidden mesh. Visible mesh paths: 68; distinct geometries: 66.
void AddConversionScene(HdRetainedSceneIndexRefPtr const &scene)
{
    HdRetainedSceneIndex::AddedPrimEntries prims;
    for (char const *parent : {"/Grid", "/Twin", "/Looks"})
        prims.push_back({SdfPath(parent), TfToken("xform"),
                         FixtureXformData(GfVec3d(0.0))});
    for (int i = 0; i < 64; ++i)
        prims.push_back({SdfPath("/Grid/M" + std::to_string(i)),
            HdPrimTypeTokens->mesh,
            FixtureMeshData(false, HdRenderTagTokens->geometry, GfVec3f(0.7f),
                            0.01f * static_cast<float>(i + 1))});
    prims.push_back({SdfPath("/Twin/A"), HdPrimTypeTokens->mesh,
                     FixtureMeshData(true)});
    prims.push_back({SdfPath("/Twin/B"), HdPrimTypeTokens->mesh,
                     FixtureMeshData(true)});
    prims.push_back({SdfPath("/Looks/Red"), HdPrimTypeTokens->material,
                     PreviewMaterialData(SdfPath("/Looks/Red"),
                                         GfVec3f(0.8f, 0.1f, 0.1f))});
    prims.push_back({SdfPath("/Looks/Blue"), HdPrimTypeTokens->material,
                     PreviewMaterialData(SdfPath("/Looks/Blue"),
                                         GfVec3f(0.1f, 0.1f, 0.8f))});
    prims.push_back({SdfPath("/Subset"), HdPrimTypeTokens->mesh,
                     FixtureMeshData(false, HdRenderTagTokens->geometry,
                                     GfVec3f(0.7f), -0.3f)});
    prims.push_back({SdfPath("/Subset/Quad"), HdPrimTypeTokens->geomSubset,
                     FaceSubsetData(VtIntArray{0}, SdfPath("/Looks/Red"))});
    prims.push_back({SdfPath("/Subset/Apex"), HdPrimTypeTokens->geomSubset,
                     FaceSubsetData(VtIntArray{1}, SdfPath("/Looks/Blue"))});
    prims.push_back({SdfPath("/Proto"), TfToken("scope"),
                     HdRetainedContainerDataSource::New(
                         TfToken("present"),
                         HdRetainedTypedSampledDataSource<bool>::New(true))});
    prims.push_back({SdfPath("/Proto/Mesh"), HdPrimTypeTokens->mesh,
                     InstancedMeshData()});
    prims.push_back({SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                     InstancerData()});
    prims.push_back({SdfPath("/Hidden"), HdPrimTypeTokens->mesh,
                     VisibilityMeshData(false, 0.9f)});
    scene->AddPrims(prims);
}

int64_t Stat(VtDictionary const &stats, char const *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<int64_t>()
        ? it->second.UncheckedGet<int64_t>() : -1;
}

double DoubleStat(VtDictionary const &stats, char const *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<double>()
        ? it->second.UncheckedGet<double>() : -1.0;
}

bool BoolStat(VtDictionary const &stats, char const *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<bool>() &&
        it->second.UncheckedGet<bool>();
}

bool PumpPrepare(prism::hydra::SceneBridge &bridge,
                 prism::hydra::CameraState const &camera,
                 HdRenderPassAovBindingVector const &bindings,
                 TfTokenVector const &renderTags,
                 unsigned int width, unsigned int height)
{
    bool changed = bridge.Prepare(
        camera, bindings, renderTags, width, height);
    const int64_t targetGeneration = Stat(
        bridge.GetStats(), "preparationGeneration");
    auto const deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        VtDictionary const stats = bridge.GetStats();
        if (!BoolStat(stats, "preparationPending") &&
            Stat(stats, "displayedGeneration") >= targetGeneration)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        changed |= bridge.Prepare(camera, bindings, renderTags, width, height);
    }
    VtDictionary const stats = bridge.GetStats();
    return changed && !BoolStat(stats, "preparationPending") &&
        Stat(stats, "displayedGeneration") >= targetGeneration;
}

bool Check(bool condition, char const *message)
{
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

template<class Predicate>
bool WaitFor(Predicate predicate)
{
    auto const deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}

template<class T>
std::vector<T> ReadBuffer(PrismRenderBuffer *buffer)
{
    T const *data = static_cast<T const *>(buffer->Map());
    std::vector<T> result;
    if (data) result.assign(data, data + static_cast<size_t>(
        buffer->GetWidth()) * buffer->GetHeight() *
        HdGetComponentCount(buffer->GetFormat()));
    buffer->Unmap();
    return result;
}

Color3 TraceTriangle(bool instanced, bool mirrored, bool *zeroTangent)
{
    RTCDevice device = rtcNewDevice(nullptr);
    SceneManager scene(device);
    ColorTexture albedo(Color3(0.8f, 0.35f, 0.1f));
    Diffuse material(&albedo);
    ColorTexture white(Color3(1.0f));
    ConstantColorBackground environment(&white);
    scene.environment = &environment;
    scene.max_depth = 3;
    scene.rr_min_depth = 100;
    auto mesh = std::make_unique<Mesh>();
    const float sx = mirrored && !instanced ? -1.0f : 1.0f;
    mesh->vertices = {{-sx, -1, 0}, {sx, -1, 0}, {0, 1, 0}};
    const float nz = mirrored && !instanced ? -1.0f : 1.0f;
    mesh->normals = {{0, 0, nz}, {0, 0, nz}, {0, 0, nz}};
    mesh->texcoords = {{0, 0}, {0, 0}, {0, 0}};
    mesh->indices = {0, 1, 2};
    mesh->material_override = &material;
    if (!instanced) {
        const unsigned id = mesh->setupEmbree(device, scene.scene);
        scene.geometry.emplace(id, mesh.get());
    } else {
        RTCScene prototype = rtcNewScene(device);
        mesh->setupEmbree(device, prototype);
        rtcCommitScene(prototype);
        RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
        rtcSetGeometryInstancedScene(instance, prototype);
        const float x = mirrored ? -1.0f : 1.0f;
        const float transform[16] = {x,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                                transform);
        rtcCommitGeometry(instance);
        const unsigned id = rtcAttachGeometry(scene.scene, instance);
        rtcReleaseGeometry(instance);
        scene.geometry.emplace(id, mesh.get());
        scene.intersection_geometry.emplace(id, mesh->geom);
        SceneManager::PlacementTransform placement;
        placement.normal = {x,0,0, 0,1,0, 0,0,1};
        placement.tangent = placement.normal;
        placement.mirrored = mirrored;
        scene.placement_transforms.emplace(id, placement);
        rtcReleaseScene(prototype);
    }
    rtcCommitScene(scene.scene);
    const RTCRay primary = initialize_ray(
        Float3(0, 0, 1), Float3(0, 0, -1));
    const RayHit primaryHit = scene.cast_ray(primary);
    *zeroTangent = primaryHit.tangent.is_zero() &&
        std::isfinite(primaryHit.tangent.x()) &&
        std::isfinite(primaryHit.tangent.y()) &&
        std::isfinite(primaryHit.tangent.z());
    unsigned int seed = 17;
    const Color3 result = scene.trace(primary, 0, Color3(1.0f), seed);
    scene.geometry.clear();
    scene.intersection_geometry.clear();
    scene.placement_transforms.clear();
    if (mesh->geom) rtcReleaseGeometry(mesh->geom);
    delete mesh->transform;
    mesh->transform = nullptr;
    rtcReleaseScene(scene.scene);
    scene.scene = nullptr;
    rtcReleaseDevice(device);
    return result;
}

struct HitSignature {
    Float3 p, n, tangent;
    bool frontFace = false;
};

HitSignature HitTransformedTriangle(bool instanced, bool mirrored)
{
    RTCDevice device = rtcNewDevice(nullptr);
    SceneManager scene(device);
    auto mesh = std::make_unique<Mesh>();
    const std::array<GfVec3d, 3> local{{{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}}};
    GfMatrix4d scale(1), rotation(1), translation(1);
    scale.SetScale(GfVec3d(mirrored ? -1.7 : 1.7, 0.65, 1.2));
    rotation.SetRotate(GfRotation(GfVec3d(0, 1, 0), 31.0));
    translation.SetTranslate(GfVec3d(0.35, -0.2, 0.6));
    const GfMatrix4d world = scale * rotation * translation;
    const GfMatrix4d normalMatrix = world.GetInverse().GetTranspose();
    GfVec3d authoredNormal = normalMatrix.TransformDir(GfVec3d(0, 0, 1)).GetNormalized();
    if (mirrored) authoredNormal = -authoredNormal;
    for (GfVec3d const &point : local) {
        GfVec3d const p = instanced ? point : world.Transform(point);
        mesh->vertices.emplace_back(float(p[0]), float(p[1]), float(p[2]));
        GfVec3d const n = instanced ? GfVec3d(0, 0, 1) : authoredNormal;
        mesh->normals.emplace_back(float(n[0]), float(n[1]), float(n[2]));
    }
    mesh->texcoords = {{0, 0}, {1, 0}, {0.5f, 1}};
    mesh->indices = {0, 1, 2};
    RTCScene prototype = nullptr;
    if (!instanced) {
        const unsigned id = mesh->setupEmbree(device, scene.scene);
        scene.geometry.emplace(id, mesh.get());
    } else {
        prototype = rtcNewScene(device);
        mesh->setupEmbree(device, prototype);
        rtcCommitScene(prototype);
        RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
        rtcSetGeometryInstancedScene(instance, prototype);
        float transform[16];
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                transform[column * 4 + row] = float(world[column][row]);
        rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                                transform);
        rtcCommitGeometry(instance);
        const unsigned id = rtcAttachGeometry(scene.scene, instance);
        rtcReleaseGeometry(instance);
        scene.geometry.emplace(id, mesh.get());
        scene.intersection_geometry.emplace(id, mesh->geom);
        SceneManager::PlacementTransform placement;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column) {
                placement.normal[row * 3 + column] =
                    float(normalMatrix[row][column]);
                placement.tangent[row * 3 + column] = float(world[row][column]);
            }
        placement.mirrored = mirrored;
        scene.placement_transforms.emplace(id, placement);
    }
    rtcCommitScene(scene.scene);
    const GfVec3d a = world.Transform(local[0]);
    const GfVec3d b = world.Transform(local[1]);
    const GfVec3d c = world.Transform(local[2]);
    const GfVec3d center = (a + b + c) / 3.0;
    const GfVec3d geometric = GfCross(b - a, c - a).GetNormalized();
    RayHit const hit = scene.cast_ray(initialize_ray(
        Float3(float(center[0] + 2 * geometric[0]),
               float(center[1] + 2 * geometric[1]),
               float(center[2] + 2 * geometric[2])),
        Float3(float(-geometric[0]), float(-geometric[1]),
               float(-geometric[2]))));
    HitSignature result{hit.p, hit.n, hit.tangent, hit.front_face};
    scene.geometry.clear();
    scene.intersection_geometry.clear();
    scene.placement_transforms.clear();
    if (mesh->geom) rtcReleaseGeometry(mesh->geom);
    delete mesh->transform;
    mesh->transform = nullptr;
    if (prototype) rtcReleaseScene(prototype);
    rtcReleaseScene(scene.scene);
    scene.scene = nullptr;
    rtcReleaseDevice(device);
    return result;
}

RayHit HitTransformedSphere(bool mirrored)
{
    RTCDevice device = rtcNewDevice(nullptr);
    SceneManager scene(device);
    Sphere sphere(Float3(0.0f), 1.0f, nullptr);
    RTCScene prototype = rtcNewScene(device);
    sphere.setupEmbree(device, prototype);
    rtcCommitScene(prototype);

    GfMatrix4d scale(1.0), rotation(1.0), translation(1.0);
    scale.SetScale(GfVec3d(mirrored ? -2.0 : 2.0, 1.25, 0.5));
    rotation.SetRotate(GfRotation(GfVec3d(0, 1, 0), 27.0));
    translation.SetTranslate(GfVec3d(0.5, -0.25, 1.0));
    const GfMatrix4d world = scale * rotation * translation;
    const GfMatrix4d normalMatrix = world.GetInverse().GetTranspose();
    RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(instance, prototype);
    float transform[16];
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            transform[column * 4 + row] = float(world[column][row]);
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                            transform);
    rtcCommitGeometry(instance);
    const unsigned id = rtcAttachGeometry(scene.scene, instance);
    rtcReleaseGeometry(instance);
    scene.geometry.emplace(id, &sphere);
    scene.intersection_geometry.emplace(id, sphere.geom);
    SceneManager::PlacementTransform placement;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            placement.normal[row * 3 + column] =
                float(normalMatrix[row][column]);
            placement.tangent[row * 3 + column] = float(world[row][column]);
        }
    placement.mirrored = mirrored;
    placement.flipOrientation = false;
    scene.placement_transforms.emplace(id, placement);
    rtcCommitScene(scene.scene);

    const GfVec3d surface = world.Transform(GfVec3d(1, 0, 0));
    const GfVec3d outward = normalMatrix.TransformDir(
        GfVec3d(1, 0, 0)).GetNormalized();
    RayHit result = scene.cast_ray(initialize_ray(
        Float3(float(surface[0] + 2 * outward[0]),
               float(surface[1] + 2 * outward[1]),
               float(surface[2] + 2 * outward[2])),
        Float3(float(-outward[0]), float(-outward[1]),
               float(-outward[2]))));
    scene.geometry.clear();
    scene.intersection_geometry.clear();
    scene.placement_transforms.clear();
    rtcReleaseGeometry(sphere.geom);
    sphere.geom = nullptr;
    delete sphere.transform;
    sphere.transform = nullptr;
    rtcReleaseScene(prototype);
    rtcReleaseScene(scene.scene);
    scene.scene = nullptr;
    rtcReleaseDevice(device);
    return result;
}

} // namespace

int main()
{
    const std::string testCache =
        (std::filesystem::current_path() / "scene_bridge_test_cache").string();
#ifdef _WIN32
    _putenv_s("PRISM_CACHE_DIR", testCache.c_str());
#else
    setenv("PRISM_CACHE_DIR", testCache.c_str(), 1);
#endif
    bool flatTangent = false, nativeTangent = false;
    bool flatMirroredTangent = false, nativeMirroredTangent = false;
    const Color3 flat = TraceTriangle(false, false, &flatTangent);
    const Color3 native = TraceTriangle(true, false, &nativeTangent);
    const Color3 flatMirrored = TraceTriangle(
        false, true, &flatMirroredTangent);
    const Color3 nativeMirrored = TraceTriangle(
        true, true, &nativeMirroredTangent);
    bool ok = Check((flat - native).length() < 1e-5f &&
                    (flatMirrored - nativeMirrored).length() < 1e-5f &&
                    flat.max_component() > 0.1f && flatTangent &&
                    nativeTangent && flatMirroredTangent &&
                    nativeMirroredTangent,
                    "native instance transport differs from flattened geometry");
    for (bool mirrored : {false, true}) {
        HitSignature const flattened = HitTransformedTriangle(false, mirrored);
        HitSignature const instanced = HitTransformedTriangle(true, mirrored);
        ok &= Check((flattened.p - instanced.p).length() < 2e-5f &&
                        (flattened.n - instanced.n).length() < 2e-5f &&
                        (flattened.tangent - instanced.tangent).length() < 2e-5f &&
                        flattened.frontFace == instanced.frontFace &&
                        flattened.frontFace,
                    "rotated/nonuniform native hit differs from flattened baseline");
        RayHit const sphere = HitTransformedSphere(mirrored);
        ok &= Check(sphere.geomid != RTC_INVALID_GEOMETRY_ID &&
                        sphere.front_face &&
                        std::abs(sphere.u - 0.5f) < 1e-5f &&
                        std::abs(sphere.v - 0.5f) < 1e-5f &&
                        sphere.n.length() > 0.999f &&
                        sphere.tangent.length() > 0.999f,
                    "analytic sphere local UV or transformed frame is invalid");
    }
    // A fan crosses the missing corner of this concave pentagon.  Validate
    // that the ear-clipped result covers exactly the authored polygon area.
    const std::vector<GfVec3d> concave{{0, 0, 0}, {2, 0, 0}, {2, 2, 0},
                                       {1, 1, 0}, {0, 2, 0}};
    const auto concaveTriangles = prism::hydra::TriangulatePolygon(concave);
    double concaveArea = 0.0;
    for (auto const &triangle : concaveTriangles) {
        concaveArea += 0.5 * GfCross(
            concave[triangle[1]] - concave[triangle[0]],
            concave[triangle[2]] - concave[triangle[0]]).GetLength();
    }

    HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
    scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                      FixtureMeshData(true)},
                     {SdfPath("/Dome"), HdPrimTypeTokens->domeLight,
                      DomeData()}});
    HdRenderThread thread;
    prism::hydra::SceneBridge *activeBridge = nullptr;
    thread.SetRenderCallback([&] {
        if (activeBridge) activeBridge->Render();
    });
    thread.StartThread();
    {
        PrismRenderBuffer buffer(SdfPath("/PublishedColor"), &thread);
        ok &= Check(buffer.Allocate(GfVec3i(2, 1, 1),
                                    HdFormatFloat32Vec4, false),
                    "render buffer allocation failed");
        buffer.BeginFrame(VtValue(GfVec4f(0.0f)));
        float left[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        float right[4] = {0.75f, 0.5f, 0.25f, 1.0f};
        buffer.WriteFloat(0, 0, left, 4, false);
        buffer.WriteFloat(1, 0, right, 4, false);
        buffer.Publish();
        const std::vector<float> published = ReadBuffer<float>(&buffer);

        buffer.BeginFrame(VtValue(GfVec4f(0.0f)));
        float replacement[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        buffer.WriteFloat(0, 0, replacement, 4, false);
        ok &= Check(ReadBuffer<float>(&buffer) == published,
                    "partial replacement frame became visible before publish");
        float replacementBackground[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        buffer.WriteFloat(1, 0, replacementBackground, 4, false);
        buffer.Publish();
        const std::vector<float> replaced = ReadBuffer<float>(&buffer);
        ok &= Check(published.size() == 8 && published[3] == 1.0f &&
                        published[7] == 1.0f && replaced.size() == 8 &&
                        replaced[0] == 1.0f && replaced[1] == 0.0f &&
                        replaced[3] == 1.0f && replaced[4] == 0.0f &&
                        replaced[7] == 0.0f,
                    "complete replacement frame was not published atomically");
    }
    ok &= Check(concaveTriangles.size() == 3 &&
                    std::abs(concaveArea - 3.0) < 1e-9,
                "concave polygon triangulation escaped the authored boundary");
    const std::vector<int> subsetSlots = prism::hydra::ExpandFaceAssignments(
        {0, 0, 1, 2, 2}, {{0, 4}, {2, 9}}, 1);
    ok &= Check(subsetSlots == std::vector<int>({4, 4, 1, 9, 9}),
                "material subset assignment did not preserve coarse faces");
    {
        using Level = prism::hydra::PickingTable::InstanceLevel;
        const std::vector<Level> retainedTopology{
            {SdfPath("/Outer"), {10, 20}},
            {SdfPath("/Inner"), {3, 7, 9}}};
        const std::vector<Level> reorderedLiveTopology{
            {SdfPath("/Outer"), {20, 10}},
            {SdfPath("/Inner"), {9, 3, 7}}};
        const std::map<SdfPath, std::set<int>> sourceSelection{
            {SdfPath("/Outer"), {20}}, {SdfPath("/Inner"), {3}},
            {SdfPath("/UnrelatedInstancer"), {99}}};
        ok &= Check(prism::hydra::TranslatePickingInstanceSelection(
                        retainedTopology, sourceSelection, true) ==
                        std::set<int>{3} &&
                    prism::hydra::TranslatePickingInstanceSelection(
                        reorderedLiveTopology, sourceSelection, true) ==
                        std::set<int>{1},
                    "retained picking topology followed reordered live source indices");
        const std::map<SdfPath, std::set<int>> ordinalSelection{
            {SdfPath("/Outer"), {1}}, {SdfPath("/Inner"), {2}}};
        ok &= Check(prism::hydra::TranslatePickingInstanceSelection(
                        retainedTopology, ordinalSelection, false) ==
                        std::set<int>{5},
                    "nested compact instance ordinals changed flattening semantics");
        const std::vector<Level> topologyWithMaskedHole{
            {SdfPath("/Outer"), {-1, 20}}};
        ok &= Check(prism::hydra::TranslatePickingInstanceSelection(
                        topologyWithMaskedHole,
                        {{SdfPath("/Outer"), {20}}}, true) ==
                        std::set<int>{1},
                    "missing placement metadata changed retained ordinal radix");
    }
    {
        prism::hydra::SceneBridge bridge(scene, &thread);
        activeBridge = &bridge;
        prism::hydra::CameraState camera;
        const TfTokenVector geometryTags{HdRenderTagTokens->geometry};
        ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags, 32, 32),
                    "initial prepare did not rebuild");
        VtDictionary stats = bridge.GetStats();
        ok &= Check(Stat(stats, "meshCount") == 1, "mesh was not populated");
        ok &= Check(Stat(stats, "triangleCount") == 2,
                    "quad triangulation or hole filtering is incorrect");
        ok &= Check(Stat(stats, "snapshotBuilds") == 1,
                    "unexpected initial snapshot count");
        ok &= Check(Stat(stats, "estimatedGeometryBytes") > 0,
                    "geometry memory estimate was not populated");
        ok &= Check(Stat(stats, "instanceCount") == 0,
                    "non-instanced mesh was counted as an instance");
        ok &= Check(DoubleStat(stats, "snapshotBuildSeconds") >= 0.0,
                    "snapshot build timing was not populated");

        // A completed or running older generation must never publish after a
        // newer owner-thread capture. The second authored value is observable
        // in the final topology and the obsolete build is accounted for.
        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false, HdRenderTagTokens->guide)}});
        bridge.MarkSceneDirty();
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false)}});
        bridge.MarkSceneDirty();
        ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags, 32, 32),
                    "latest generation did not finish after supersession");
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "discardedSnapshotBuilds") >= 1 &&
                        Stat(stats, "meshCount") == 1 &&
                        Stat(stats, "triangleCount") == 3,
                    "superseded generation published stale scene data");
        std::atomic<bool> liveSourceReadByWorker{false};
        HdVec3fArrayDataSourceHandle checkedPoints(
            new OwnerCheckedPoints(
                VtVec3fArray{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0},
                             {-1, 1, 0}, {0, 0, 1}},
                std::this_thread::get_id(), &liveSourceReadByWorker));
        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false, HdRenderTagTokens->geometry,
                                          GfVec3f(0.7f), 0.0f,
                                          checkedPoints)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Mesh"));
        ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags, 32, 32),
                    "immutable-source generation did not finish");
        ok &= Check(!liveSourceReadByWorker.load(),
                    "background generation traversed a live sampled data source");
        const int64_t buildsAfterLatest = Stat(
            bridge.GetStats(), "snapshotBuilds");

        camera.projection[0][0] = 2.0;
        ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags, 32, 32),
                    "camera edit did not reset accumulation");
        ok &= Check(Stat(bridge.GetStats(), "snapshotBuilds") ==
                        buildsAfterLatest,
                    "camera-only edit rebuilt geometry");

        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false)}});
        bridge.MarkSceneDirty();
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        ok &= Check(Stat(bridge.GetStats(), "triangleCount") == 3,
                    "topology invalidation did not rebuild triangles");

        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false, HdRenderTagTokens->guide)}});
        bridge.MarkSceneDirty();
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        ok &= Check(Stat(bridge.GetStats(), "meshCount") == 0,
                    "purpose filtering did not exclude guide mesh");

        scene->RemovePrims({SdfPath("/Mesh")});
        bridge.MarkSceneDirty();
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        ok &= Check(Stat(bridge.GetStats(), "meshCount") == 0,
                    "subtree removal left stale geometry");

        scene->AddPrims({
            {SdfPath("/Proto"), TfToken("scope"),
             HdRetainedContainerDataSource::New(
                 TfToken("present"),
                 HdRetainedTypedSampledDataSource<bool>::New(true))},
            {SdfPath("/Proto/Mesh"), HdPrimTypeTokens->mesh,
             InstancedMeshData()},
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData()}});
        bridge.MarkSceneDirty();
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "meshCount") == 1 &&
                        Stat(stats, "instanceCount") == 2,
                    "point-instancer placements did not share a prototype");
        ok &= Check(Stat(stats, "prototypeCount") == 1 &&
                        Stat(stats, "triangleCount") == 2,
                    "native instances duplicated prototype storage");
        ok &= Check(Stat(stats, "pickingInstanceTopologyPrimCount") == 1 &&
                        Stat(stats, "pickingInstanceTopologyFailures") == 0,
                    "published picking metadata lost instance topology");
        const int64_t buildsBeforeTransform = Stat(stats, "snapshotBuilds");
        scene->AddPrims({{SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                          InstancerData(1.25)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Instancer"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeTransform &&
                        Stat(stats, "placementInvalidations") > 0 &&
                        Stat(stats, "prototypeCount") == 1,
                    "transform edit rebuilt prototype geometry");

        scene->AddPrims({{SdfPath("/StaticMesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(true)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                              SdfPath("/StaticMesh"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        const int64_t buildsBeforePrototypeEdit = Stat(stats, "snapshotBuilds");
        const int64_t prototypeBuildsBeforeEdit =
            Stat(stats, "prototypeBuildCount");
        ok &= Check(Stat(stats, "prototypeCount") == 1 &&
                        Stat(stats, "prototypePathCount") == 2,
                    "identical prototype paths did not share geometry storage");
        scene->AddPrims({{SdfPath("/Proto/Mesh"), HdPrimTypeTokens->mesh,
                          InstancedMeshData(false)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Proto/Mesh"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforePrototypeEdit &&
                        Stat(stats, "prototypeBuildCount") ==
                            prototypeBuildsBeforeEdit + 1 &&
                        Stat(stats, "prototypeCount") == 2,
                    "topology edit rebuilt unaffected prototypes");

        // Start from synchronized banks so this isolates the visibility-only
        // journal replay from the preceding prototype edit.
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                              SdfPath("/Proto"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        const int64_t buildsBeforeVisibility = Stat(stats, "snapshotBuilds");
        const int64_t prototypeBuildsBeforeVisibility =
            Stat(stats, "prototypeBuildCount");
        scene->AddPrims({{SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                          InstancerData(1.25, false)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Instancer"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeVisibility &&
                        Stat(stats, "prototypeBuildCount") ==
                            prototypeBuildsBeforeVisibility &&
                        Stat(stats, "lastPlacementRecordsVisited") == 2 &&
                        Stat(stats, "lastVisibilityPlacementsUpdated") == 2,
                    "visibility edit rebuilt or touched an unrelated prototype");
        scene->AddPrims({{SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                          InstancerData(1.25, true)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Instancer"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeVisibility &&
                        Stat(stats, "prototypeBuildCount") ==
                            prototypeBuildsBeforeVisibility &&
                        Stat(stats, "lastPlacementRecordsVisited") == 2,
                    "visibility restore rebuilt or touched an unrelated prototype");
        const int64_t recoveriesBeforeHiddenPrototypeEdit =
            Stat(stats, "visibilityRecoveryRebuilds");

        // A hidden placement can legitimately lose its dependency records
        // when a simultaneous prototype refresh rebuilds the inactive TLAS.
        // Showing it again must take the conservative rebuild path and restore
        // every instance rather than silently treating the empty index as a
        // successful targeted update.
        scene->AddPrims({{SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                          InstancerData(1.25, false)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Instancer"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        scene->AddPrims({{SdfPath("/Proto/Mesh"), HdPrimTypeTokens->mesh,
                          InstancedMeshData(false, GfVec3f(0.6f))}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Proto/Mesh"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        scene->AddPrims({{SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                          InstancerData(1.25, true)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Instancer"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "instanceCount") == 2 &&
                        Stat(stats, "prototypeCount") == 2 &&
                        Stat(stats, "visibilityRecoveryRebuilds") >
                            recoveriesBeforeHiddenPrototypeEdit,
                    "visibility restore after hidden prototype edit lost placements");
        const int64_t buildsBeforeStateEdits = Stat(stats, "snapshotBuilds");
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Material,
                              SdfPath("/Looks/Unused"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Environment,
                              SdfPath("/Dome"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeStateEdits &&
                        Stat(stats, "prototypeCount") == 2 &&
                        Stat(stats, "materialInvalidations") > 0 &&
                        Stat(stats, "environmentInvalidations") > 0,
                    "material or dome edit rebuilt prototype storage");
        auto resourceSettings = bridge.GetSettings();
        ++resourceSettings.resourceRevision;
        bridge.SetSettings(resourceSettings);
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeStateEdits &&
                        Stat(stats, "prototypeCount") == 2,
                    "external resource revision rebuilt prototype geometry");

        const int64_t buildsBeforePoints = Stat(stats, "snapshotBuilds");
        const int64_t prototypeBuildsBeforePoints =
            Stat(stats, "prototypeBuildCount");
        scene->AddPrims({{SdfPath("/StaticMesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(true, HdRenderTagTokens->geometry,
                                          GfVec3f(0.7f), 0.2f)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/StaticMesh"));
        PumpPrepare(bridge, camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforePoints &&
                        Stat(stats, "prototypeBuildCount") ==
                            prototypeBuildsBeforePoints + 1 &&
                        Stat(stats, "prototypeCount") == 2,
                    "points edit rebuilt unaffected prototype geometry");

        constexpr unsigned resolution = 16;
        PrismRenderBuffer colorBuffer(SdfPath("/Color"), &thread);
        PrismRenderBuffer primBuffer(SdfPath("/PrimId"), &thread);
        PrismRenderBuffer instanceBuffer(SdfPath("/InstanceId"), &thread);
        PrismRenderBuffer elementBuffer(SdfPath("/ElementId"), &thread);
        PrismRenderBuffer normalBuffer(SdfPath("/Normal"), &thread);
        PrismRenderBuffer depthBuffer(SdfPath("/Depth"), &thread);
        colorBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                             HdFormatFloat32Vec4, false);
        primBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                            HdFormatInt32, false);
        instanceBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                                HdFormatInt32, false);
        elementBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                               HdFormatInt32, false);
        normalBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                              HdFormatFloat32Vec3, false);
        depthBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                             HdFormatFloat32, false);
        auto binding = [](TfToken const &name, HdRenderBuffer *buffer,
                          VtValue const &clear) {
            HdRenderPassAovBinding result;
            result.aovName = name;
            result.renderBuffer = buffer;
            result.clearValue = clear;
            return result;
        };
        HdRenderPassAovBindingVector aovs{
            binding(HdAovTokens->color, &colorBuffer, VtValue(GfVec4f(0))),
            binding(HdAovTokens->primId, &primBuffer, VtValue(-1)),
            binding(HdAovTokens->instanceId, &instanceBuffer, VtValue(-1)),
            binding(HdAovTokens->elementId, &elementBuffer, VtValue(-1)),
            binding(HdAovTokens->normal, &normalBuffer, VtValue(GfVec3f(0))),
            binding(HdAovTokens->depth, &depthBuffer, VtValue(1.0f))};
        GfFrustum frustum;
        frustum.SetPerspective(100.0, 1.0, 0.1, 100.0);
        camera.projection = frustum.ComputeProjectionMatrix();
        camera.worldToView.SetTranslate(GfVec3d(0, 0, -3));
        camera.nearClip = 0.1f;
        camera.farClip = 100.0f;

        prism::hydra::RenderSettings renderSettings;
        renderSettings.samples = 1000000;
        renderSettings.maxDepth = 1;
        renderSettings.previewMaxDimension = 16;
        renderSettings.refinementDelayMs = 0;
        bridge.SetSettings(renderSettings);
        const int64_t previewsBeforeInitial = Stat(
            bridge.GetStats(), "previewPublishes");
        PumpPrepare(bridge, camera, aovs, geometryTags, resolution, resolution);
        thread.StartRender();
        ok &= Check(WaitFor([&] {
            return Stat(bridge.GetStats(), "previewPublishes") >
                previewsBeforeInitial;
        }), "bounded camera preview was not published by the render worker");
        thread.StopRender();
        stats = bridge.GetStats();
        const std::vector<float> previewColor = ReadBuffer<float>(&colorBuffer);
        bool previewAlpha = previewColor.size() == resolution * resolution * 4;
        for (size_t pixel = 0; previewAlpha && pixel < resolution * resolution;
             ++pixel)
            previewAlpha = previewColor[pixel * 4 + 3] == 1.0f;
        ok &= Check(Stat(stats, "previewPublishes") > 0 &&
                        Stat(stats, "previewWidth") <= 16 &&
                        Stat(stats, "previewHeight") <= 16,
                    "bounded camera preview dimensions are invalid");
        ok &= Check(previewAlpha,
                    "camera preview color buffer was malformed");
        thread.StartRender();
        ok &= Check(WaitFor([&] {
            return Stat(bridge.GetStats(), "completedSamples") > 0;
        }), "progressive render did not start");
        thread.PauseRender();
        const int64_t beforePause = Stat(
            bridge.GetStats(), "completedSamples");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int64_t whilePaused = Stat(
            bridge.GetStats(), "completedSamples");
        ok &= Check(whilePaused <= beforePause + 1,
                    "pause did not stop progressive sampling");
        thread.ResumeRender();
        ok &= Check(WaitFor([&] {
            return Stat(bridge.GetStats(), "completedSamples") > whilePaused;
        }), "resume did not continue progressive sampling");
        thread.StopRender();
        ok &= Check(!bridge.IsConverged() &&
                        Stat(bridge.GetStats(), "completedSamples") <
                            renderSettings.samples,
                    "cancellation incorrectly reported convergence");

        renderSettings.samples = 1;
        // The color-edit check below needs a surface bounce to the dome.
        // Depth one intentionally truncates that contribution to black.
        renderSettings.maxDepth = 2;
        bridge.SetSettings(renderSettings);
        PumpPrepare(bridge, camera, aovs, geometryTags, resolution, resolution);
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "one-sample restart did not converge");
        std::vector<int> const primIds = ReadBuffer<int>(&primBuffer);
        std::vector<int> const instanceIds = ReadBuffer<int>(&instanceBuffer);
        std::vector<int> const elementIds = ReadBuffer<int>(&elementBuffer);
        std::vector<float> const initialColors = ReadBuffer<float>(&colorBuffer);
        std::vector<float> const normals = ReadBuffer<float>(&normalBuffer);
        std::vector<float> const depths = ReadBuffer<float>(&depthBuffer);
        bool foundInstance0 = false, foundInstance1 = false;
        bool validAovs = true;
        bool rotatedNormal = false, mirroredNormal = false;
        bool smoothMissingNormals = false;
        GfVec3f observedRotated(0.0f), observedMirrored(0.0f);
        for (size_t pixel = 0; pixel < primIds.size(); ++pixel) {
            if (primIds[pixel] < 0) continue;
            foundInstance0 |= instanceIds[pixel] == 0;
            foundInstance1 |= instanceIds[pixel] == 1;
            const GfVec3f normal(
                normals[pixel * 3], normals[pixel * 3 + 1],
                normals[pixel * 3 + 2]);
            const float normalLength = normal.GetLength();
            validAovs &= elementIds[pixel] >= 0 && normalLength > 0.9f &&
                std::isfinite(depths[pixel]) && depths[pixel] >= 0.0f &&
                depths[pixel] <= 1.0f;
            if (instanceIds[pixel] == 0) {
                observedRotated = normal;
                rotatedNormal |= std::abs(normal[0]) > 0.2f && normal[2] > 0.8f;
            }
            if (instanceIds[pixel] == 1) {
                observedMirrored = normal;
                mirroredNormal |= normal[2] > 0.9f;
                // Coarse face zero lies in XY; its flat normal has no Y
                // component. Shared vertices also belong to the slanted face,
                // so a generated smooth normal must interpolate that slope.
                smoothMissingNormals |= elementIds[pixel] == 0 &&
                    std::abs(normal[1]) > 0.05f && normal[2] > 0.7f;
            }
        }
        ok &= Check(foundInstance0 && foundInstance1,
                    "instance transforms or flattened instance IDs are incorrect");
        ok &= Check(validAovs,
                    "primary-hit depth, normal, or coarse-face AOV is invalid");
        ok &= Check(smoothMissingNormals,
                    "missing authored normals fell back to flat triangle shading");
        if (!rotatedNormal || !mirroredNormal)
            std::cerr << "Observed transformed normals: rotated="
                      << observedRotated << " mirrored=" << observedMirrored << '\n';
        ok &= Check(rotatedNormal && mirroredNormal,
                    "instance normal transform failed for rotated/nonuniform or mirrored placement");

        const VtDictionary beforeColorStats = bridge.GetStats();
        scene->AddPrims({{SdfPath("/Proto/Mesh"), HdPrimTypeTokens->mesh,
                          InstancedMeshData(false, GfVec3f(0.05f, 0.15f, 0.95f))}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Proto/Mesh"));
        PumpPrepare(bridge, camera, aovs, geometryTags, resolution, resolution);
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "displayColor replacement frame did not converge");
        const std::vector<float> changedColors = ReadBuffer<float>(&colorBuffer);
        const VtDictionary afterColorStats = bridge.GetStats();
        ok &= Check(changedColors != initialColors,
                    "displayColor edit did not change rendered pixels");
        ok &= Check(Stat(afterColorStats, "snapshotBuilds") ==
                        Stat(beforeColorStats, "snapshotBuilds"),
                    "displayColor edit rebuilt the full snapshot");
        ok &= Check(Stat(afterColorStats, "prototypeBuildCount") ==
                        Stat(beforeColorStats, "prototypeBuildCount"),
                    "displayColor-only edit rebuilt Embree prototype geometry");
        ok &= Check(Stat(afterColorStats, "prototypeCount") == 2,
                    "displayColor edit changed unique prototype storage");

        camera.worldToView.SetTranslate(GfVec3d(0, 0, -4));
        const int64_t previewsBeforeMove = Stat(
            bridge.GetStats(), "previewPublishes");
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                   resolution, resolution),
                    "camera change did not start a replacement frame");
        thread.StartRender();
        ok &= Check(WaitFor([&] {
            return Stat(bridge.GetStats(), "previewPublishes") >
                previewsBeforeMove;
        }), "camera change did not publish a preview");
        thread.StopRender();
        const std::vector<float> previewDepths = ReadBuffer<float>(&depthBuffer);
        ok &= Check(Stat(bridge.GetStats(), "previewPublishes") >
                        previewsBeforeMove && previewDepths != depths,
                    "camera preview did not publish current depth");
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "camera replacement frame did not converge");
        const std::vector<float> movedDepths = ReadBuffer<float>(&depthBuffer);
        const bool depthChanged = movedDepths.size() == depths.size() &&
            !std::equal(movedDepths.begin(), movedDepths.end(), depths.begin());
        ok &= Check(depthChanged,
                    "camera replacement did not publish updated depth");
        thread.StopRender();

        // A native implicit has no polygon element id. Radius edits should
        // replace only its prototype and must visibly change its silhouette.
        scene->RemovePrims({SdfPath("/Proto"), SdfPath("/Instancer"),
                            SdfPath("/StaticMesh"), SdfPath("/Dome")});
        scene->AddPrims({{SdfPath("/Sphere"), HdPrimTypeTokens->sphere,
                          FixtureSphereData(0.6)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                              SdfPath("/Sphere"));
        renderSettings.samples = 1;
        renderSettings.sphereBatching = true;
        bridge.SetSettings(renderSettings);
        camera.worldToView.SetTranslate(GfVec3d(0, 0, -4));
        PumpPrepare(bridge, camera, aovs, geometryTags, resolution, resolution);
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "analytic sphere frame did not converge");
        const std::vector<int> smallSpherePrims = ReadBuffer<int>(&primBuffer);
        const std::vector<int> smallSphereElements =
            ReadBuffer<int>(&elementBuffer);
        size_t smallSpherePixels = 0;
        bool implicitElements = true;
        bool spherePickingPath = false;
        for (size_t i = 0; i < smallSpherePrims.size(); ++i) {
            if (smallSpherePrims[i] < 0) continue;
            ++smallSpherePixels;
            implicitElements &= smallSphereElements[i] == -1;
            spherePickingPath |= bridge.GetPrimPath(smallSpherePrims[i]) ==
                SdfPath("/Sphere");
        }
        const VtDictionary beforeRadiusStats = bridge.GetStats();
        scene->AddPrims({{SdfPath("/Sphere"), HdPrimTypeTokens->sphere,
                          FixtureSphereData(1.2)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Sphere"));
        PumpPrepare(bridge, camera, aovs, geometryTags, resolution, resolution);
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "sphere radius replacement frame did not converge");
        const std::vector<int> largeSpherePrims = ReadBuffer<int>(&primBuffer);
        const size_t largeSpherePixels = static_cast<size_t>(std::count_if(
            largeSpherePrims.begin(), largeSpherePrims.end(),
            [](int value) { return value >= 0; }));
        const VtDictionary afterRadiusStats = bridge.GetStats();
        ok &= Check(smallSpherePixels > 0 && implicitElements &&
                        spherePickingPath &&
                        largeSpherePixels > smallSpherePixels,
                    "analytic sphere picking or radius silhouette is invalid");
        ok &= Check(Stat(afterRadiusStats, "snapshotBuilds") ==
                        Stat(beforeRadiusStats, "snapshotBuilds") &&
                        Stat(afterRadiusStats, "prototypeBuildCount") ==
                            Stat(beforeRadiusStats, "prototypeBuildCount") &&
                        Stat(afterRadiusStats, "sphereBatchRebuilds") ==
                            Stat(beforeRadiusStats, "sphereBatchRebuilds") + 1 &&
                        Stat(afterRadiusStats, "sphereBatchEntriesUpdated") ==
                            Stat(beforeRadiusStats, "sphereBatchEntriesUpdated") + 1,
                    "sphere radius edit did not rebuild exactly its stable batch");
        thread.StopRender();

        scene->RemovePrims({SdfPath("/Sphere")});
        scene->AddPrims({
            {SdfPath("/Mixed"), TfToken("xform"),
             FixtureXformData(GfVec3d(0.0))},
            {SdfPath("/Mixed/Sphere"), HdPrimTypeTokens->sphere,
             FixtureSphereData(0.2)},
            {SdfPath("/Mixed/Mesh"), HdPrimTypeTokens->mesh,
             FixtureMeshData(true)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                              SdfPath("/Mixed"));
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                resolution, resolution),
                    "mixed sphere/mesh parent did not prepare");
        const VtDictionary mixedBefore = bridge.GetStats();
        scene->AddPrims({{SdfPath("/Mixed"), TfToken("xform"),
                          FixtureXformData(GfVec3d(1.0, 0.0, 0.0))}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Mixed"));
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                resolution, resolution),
                    "mixed parent placement edit did not prepare");
        const VtDictionary mixedAfter = bridge.GetStats();
        ok &= Check(Stat(mixedAfter, "sphereBatchRebuilds") ==
                            Stat(mixedBefore, "sphereBatchRebuilds") + 1 &&
                        Stat(mixedAfter, "lastPlacementRecordsVisited") > 0,
                    "mixed parent edit skipped the ordinary mesh placement");

        // The fixed cap must split 1025 independent spheres into two stable
        // batches. Editing the final sphere must leave the unrelated
        // 1024-entry batch untouched.
        scene->RemovePrims({SdfPath("/Mixed")});
        HdRetainedSceneIndex::AddedPrimEntries batchSpheres;
        batchSpheres.reserve(1025);
        for (int i = 0; i < 1025; ++i) {
            const SdfPath path("/Batch/S" + std::to_string(i));
            batchSpheres.push_back({path, HdPrimTypeTokens->sphere,
                FixtureSphereData(0.05, GfVec3d(i % 41, i / 41, 0.0))});
        }
        scene->AddPrims(batchSpheres);
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                              SdfPath("/Batch"));
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                resolution, resolution),
                    "1025-sphere batch scene did not prepare");
        VtDictionary batchStats = bridge.GetStats();
        ok &= Check(Stat(batchStats, "sphereBatchCount") == 2 &&
                        Stat(batchStats, "sphereBatchEntryCount") == 1025,
                    "1025 spheres were not split at the 1024-entry cap");
        const int64_t batchRebuilds = Stat(batchStats, "sphereBatchRebuilds");
        const int64_t batchEntries = Stat(batchStats,
                                          "sphereBatchEntriesUpdated");
        scene->AddPrims({{SdfPath("/Batch/S1024"), HdPrimTypeTokens->sphere,
            FixtureSphereData(0.1, GfVec3d(0.0, 25.0, 0.0))}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Batch/S1024"));
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                resolution, resolution),
                    "single-entry sphere batch edit did not prepare");
        batchStats = bridge.GetStats();
        ok &= Check(Stat(batchStats, "sphereBatchRebuilds") ==
                            batchRebuilds + 1 &&
                        Stat(batchStats, "sphereBatchEntriesUpdated") ==
                            batchEntries + 1 &&
                        Stat(batchStats, "sphereBatchConservativeFallbacks") == 0,
                    "sphere batch edit touched an unrelated batch");
        const int64_t materialRebuilds = Stat(batchStats,
                                              "sphereBatchRebuilds");
        const int64_t materialUpdates = Stat(batchStats,
            "sphereBatchEntriesUpdated");
        scene->AddPrims({{SdfPath("/Batch/S1024"), HdPrimTypeTokens->sphere,
            FixtureSphereData(0.1, GfVec3d(0.0, 25.0, 0.0),
                              GfVec3f(0.1f, 0.8f, 0.2f))}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Material,
                              SdfPath("/Batch/S1024"));
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                resolution, resolution),
                    "sphere batch material edit did not prepare");
        batchStats = bridge.GetStats();
        ok &= Check(Stat(batchStats, "sphereBatchRebuilds") ==
                            materialRebuilds &&
                        Stat(batchStats, "sphereBatchEntriesUpdated") ==
                            materialUpdates + 1,
                    "sphere material edit rebuilt geometry or touched siblings");
        ok &= Check(Stat(batchStats, "embreeLiveBytes") > 0 &&
                        Stat(batchStats, "embreePeakBytes") >=
                            Stat(batchStats, "embreeLiveBytes"),
                    "Embree memory monitor did not account live allocations");
        activeBridge = nullptr;
    }
    {
        // Before the first snapshot nothing is displayed, so capture gets the
        // long budget; once one is published it drops back to the short one.
        constexpr int primCount = 2500;
        HdRetainedSceneIndexRefPtr largeScene = HdRetainedSceneIndex::New();
        HdRetainedSceneIndex::AddedPrimEntries spheres;
        spheres.reserve(primCount);
        for (int i = 0; i < primCount; ++i)
            spheres.push_back({SdfPath("/Large/S" + std::to_string(i)),
                HdPrimTypeTokens->sphere,
                FixtureSphereData(0.05, GfVec3d(i % 50, i / 50, 0.0))});
        largeScene->AddPrims(spheres);
        prism::hydra::SceneBridge bridge(largeScene, &thread);
        prism::hydra::CameraState camera;
        const TfTokenVector geometryTags{HdRenderTagTokens->geometry};
        prism::hydra::RenderSettings settings;
        settings.sphereBatching = true;
        bridge.SetSettings(settings);
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        ok &= Check(DoubleStat(bridge.GetStats(),
                               "captureBudgetMilliseconds") == 50.0,
                    "first-load capture did not use the long budget");
        ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags, 32, 32),
                    "large scene initial prepare did not publish");
        VtDictionary stats = bridge.GetStats();
        ok &= Check(Stat(stats, "capturedPrims") >= primCount &&
                        Stat(stats, "sphereBatchEntryCount") == primCount,
                    "large scene was not fully loaded");
        largeScene->AddPrims({{SdfPath("/Large/S0"), HdPrimTypeTokens->sphere,
            FixtureSphereData(0.1)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Prototype,
                              SdfPath("/Large/S0"));
        ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags, 32, 32),
                    "large scene edit did not prepare");
        ok &= Check(DoubleStat(bridge.GetStats(),
                               "captureBudgetMilliseconds") == 3.0,
                    "capture after first publication kept the long budget");
    }
    {
        // Snapshot builds convert meshes in parallel ahead of the serial walk.
        // Counts, storage sharing and prim ids must follow the serial rules.
        constexpr unsigned resolution = 8;
        const TfTokenVector geometryTags{HdRenderTagTokens->geometry};
        prism::hydra::CameraState camera;
        prism::hydra::RenderSettings settings;
        settings.samples = 1;
        settings.maxDepth = 1;
        settings.refinementDelayMs = 0;
        auto primBinding = [](PrismRenderBuffer *buffer) {
            HdRenderPassAovBinding result;
            result.aovName = HdAovTokens->primId;
            result.renderBuffer = buffer;
            result.clearValue = VtValue(-1);
            return HdRenderPassAovBindingVector{result};
        };
        struct Loaded {
            std::vector<std::pair<SdfPath, SdfPath>> picking;
            VtDictionary stats;
        };
        // Picking tables are published with a rendered frame.
        auto load = [&](prism::hydra::SceneBridge &bridge,
                        PrismRenderBuffer *primBuffer) {
            Loaded result;
            bridge.SetSettings(settings);
            activeBridge = &bridge;
            ok &= Check(PumpPrepare(bridge, camera, primBinding(primBuffer),
                                    geometryTags, resolution, resolution),
                        "conversion scene did not prepare");
            thread.StartRender();
            ok &= Check(WaitFor([&] {
                return bridge.IsConverged() && !bridge.GetPrimPath(0).IsEmpty();
            }), "conversion scene frame did not publish");
            thread.StopRender();
            result.stats = bridge.GetStats();
            for (int64_t id = 0; id < Stat(result.stats, "primCount"); ++id)
                result.picking.emplace_back(
                    bridge.GetPrimPath(static_cast<int>(id)),
                    bridge.GetInstancerPath(static_cast<int>(id)));
            return result;
        };
        PrismRenderBuffer firstPrims(SdfPath("/ConversionPrimsA"), &thread);
        PrismRenderBuffer secondPrims(SdfPath("/ConversionPrimsB"), &thread);
        firstPrims.Allocate(GfVec3i(resolution, resolution, 1),
                            HdFormatInt32, false);
        secondPrims.Allocate(GfVec3i(resolution, resolution, 1),
                             HdFormatInt32, false);
        HdRetainedSceneIndexRefPtr firstScene = HdRetainedSceneIndex::New();
        HdRetainedSceneIndexRefPtr secondScene = HdRetainedSceneIndex::New();
        AddConversionScene(firstScene);
        AddConversionScene(secondScene);

        prism::hydra::SceneBridge first(firstScene, &thread);
        Loaded const firstLoad = load(first, &firstPrims);
        VtDictionary const &stats = firstLoad.stats;
        // Distinct geometries: 64 grid meshes, the subset mesh, and one
        // storage shared by both twins and the instanced prototype.
        ok &= Check(Stat(stats, "primCount") == 68 &&
                        Stat(stats, "meshCount") == 66 &&
                        Stat(stats, "triangleCount") == 64 * 3 + 3 + 2 &&
                        Stat(stats, "instanceCount") == 2 &&
                        Stat(stats, "prototypeBuildCount") == 66,
                    "parallel conversion changed prim, triangle, prototype or instance counts");
        ok &= Check(Stat(stats, "prototypeCount") == 66 &&
                        Stat(stats, "prototypePathCount") == 68,
                    "identical prototype paths did not share geometry storage");
        ok &= Check(Stat(stats, "prototypeConversions") == 68 &&
                        Stat(stats, "prototypeConversionCacheHits") == 0,
                    "initial load did not convert each visible mesh path once");
        bool pickingComplete = firstLoad.picking.size() == 68;
        for (auto const &[primPath, instancerPath] : firstLoad.picking)
            pickingComplete &= !primPath.IsEmpty() &&
                (primPath == SdfPath("/Proto/Mesh")) ==
                    (instancerPath == SdfPath("/Instancer"));
        ok &= Check(pickingComplete,
                    "parallel conversion published an incomplete picking table");
        {
            prism::hydra::SceneBridge second(secondScene, &thread);
            Loaded const secondLoad = load(second, &secondPrims);
            bool sameStats = true;
            for (char const *name : {"meshCount", "triangleCount", "primCount",
                    "instanceCount", "prototypeCount", "prototypePathCount",
                    "prototypeBuildCount", "prototypeConversions",
                    "prototypeConversionCacheHits", "estimatedGeometryBytes",
                    "inactiveSnapshotGeometryBytes",
                    "pickingInstanceTopologyPrimCount",
                    "pickingInstanceTopologyFailures"})
                sameStats &= Stat(firstLoad.stats, name) ==
                    Stat(secondLoad.stats, name);
            ok &= Check(firstLoad.picking == secondLoad.picking,
                        "parallel conversion made picking prim ids nondeterministic");
            ok &= Check(sameStats,
                        "parallel conversion made geometry statistics nondeterministic");
            activeBridge = nullptr;
        }

        // Showing a mesh that had no placement records rebuilds the snapshot
        // from the incrementally captured scene. Every other prim keeps its
        // captured data source, so only the newly visible mesh converts.
        activeBridge = &first;
        firstScene->AddPrims({{SdfPath("/Hidden"), HdPrimTypeTokens->mesh,
                               VisibilityMeshData(true, 0.9f)}});
        first.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                             SdfPath("/Hidden"));
        ok &= Check(PumpPrepare(first, camera, primBinding(&firstPrims),
                                geometryTags, resolution, resolution),
                    "visibility restore did not prepare");
        VtDictionary const shown = first.GetStats();
        ok &= Check(Stat(shown, "visibilityRecoveryRebuilds") ==
                        Stat(stats, "visibilityRecoveryRebuilds") + 1 &&
                        Stat(shown, "prototypeConversionCacheHits") > 0 &&
                        Stat(shown, "prototypeConversionCacheHits") == 68 &&
                        Stat(shown, "prototypeConversions") == 1 &&
                        Stat(shown, "prototypeBuildCount") == 67,
                    "unchanged prims were converted again in an incremental snapshot build");
        ok &= Check(Stat(shown, "primCount") == 69 &&
                        Stat(shown, "prototypePathCount") == 69 &&
                        Stat(shown, "prototypeCount") == 67 &&
                        Stat(shown, "triangleCount") == 64 * 3 + 3 + 2 + 3 &&
                        Stat(shown, "instanceCount") == 2,
                    "conversion cache changed incremental snapshot geometry");
        activeBridge = nullptr;
    }
    {
        // A full rebuild publishes its snapshot first; the spare bank used by
        // later local edits is built by a follow-up job and installed without
        // touching the displayed frame.
        constexpr unsigned resolution = 16;
        const TfTokenVector geometryTags{HdRenderTagTokens->geometry};
        prism::hydra::CameraState camera;
        GfFrustum frustum;
        frustum.SetPerspective(100.0, 1.0, 0.1, 100.0);
        camera.projection = frustum.ComputeProjectionMatrix();
        camera.worldToView.SetTranslate(GfVec3d(0, 0, -3));
        camera.nearClip = 0.1f;
        camera.farClip = 100.0f;
        prism::hydra::RenderSettings settings;
        settings.samples = 1;
        settings.maxDepth = 2;
        settings.refinementDelayMs = 0;
        PrismRenderBuffer colorBuffer(SdfPath("/SpareColor"), &thread);
        PrismRenderBuffer primBuffer(SdfPath("/SparePrims"), &thread);
        colorBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                             HdFormatFloat32Vec4, false);
        primBuffer.Allocate(GfVec3i(resolution, resolution, 1),
                            HdFormatInt32, false);
        HdRenderPassAovBindingVector aovs(2);
        aovs[0].aovName = HdAovTokens->color;
        aovs[0].renderBuffer = &colorBuffer;
        aovs[0].clearValue = VtValue(GfVec4f(0.0f));
        aovs[1].aovName = HdAovTokens->primId;
        aovs[1].renderBuffer = &primBuffer;
        aovs[1].clearValue = VtValue(-1);
        // Drives Prepare by hand until the displayed generation catches up,
        // stopping at publication rather than waiting for the spare bank.
        auto prepareUntilDisplayed = [&](prism::hydra::SceneBridge &bridge,
                                         HdRenderPassAovBindingVector const &bindings) {
            bridge.Prepare(camera, bindings, geometryTags, resolution, resolution);
            const int64_t target = Stat(bridge.GetStats(), "preparationGeneration");
            auto const deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(60);
            while (Stat(bridge.GetStats(), "displayedGeneration") < target &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                bridge.Prepare(camera, bindings, geometryTags, resolution,
                               resolution);
            }
            return Stat(bridge.GetStats(), "displayedGeneration") >= target;
        };
        // Spare-only Prepare calls report no change, so PumpPrepare's return
        // value is not meaningful for them; wait for preparation to settle.
        auto settle = [&](prism::hydra::SceneBridge &bridge,
                          HdRenderPassAovBindingVector const &bindings) {
            auto const deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(60);
            while (BoolStat(bridge.GetStats(), "preparationPending") &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                bridge.Prepare(camera, bindings, geometryTags, resolution,
                               resolution);
            }
            return !BoolStat(bridge.GetStats(), "preparationPending");
        };
        auto renderFrame = [&](prism::hydra::SceneBridge &bridge) {
            activeBridge = &bridge;
            thread.StartRender();
            const bool done = WaitFor([&] {
                return bridge.IsConverged() &&
                    Stat(bridge.GetStats(), "completedSamples") > 0;
            });
            thread.StopRender();
            return done;
        };
        auto gridPixels = [&](prism::hydra::SceneBridge &bridge,
                              size_t *hitPixels) {
            size_t count = 0;
            *hitPixels = 0;
            for (int const id : ReadBuffer<int>(&primBuffer)) {
                if (id < 0) continue;
                ++*hitPixels;
                count += bridge.GetPrimPath(id).GetParentPath() ==
                    SdfPath("/Grid");
            }
            return count;
        };

        // A: the initial load leaves a displayed snapshot and an installed
        // spare, and the spare reused every conversion.
        {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            AddConversionScene(scene);
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags,
                                    resolution, resolution),
                        "spare scene initial prepare did not publish");
            VtDictionary const stats = bridge.GetStats();
            ok &= Check(Stat(stats, "snapshotBuilds") == 1 &&
                            Stat(stats, "spareSnapshotsInstalled") == 1 &&
                            Stat(stats, "spareSnapshotsDiscarded") == 0 &&
                            Stat(stats, "retainedSnapshotCount") == 2,
                        "initial load did not install exactly one spare bank");
            ok &= Check(Stat(stats, "sparePrototypeConversionCacheHits") == 68 &&
                            Stat(stats, "sparePrototypeConversions") == 0 &&
                            Stat(stats, "prototypeBuildCount") == 66,
                        "spare bank converted or rebuilt mesh prototypes");
            ok &= Check(DoubleStat(stats, "firstPublishSeconds") > 0.0 &&
                            DoubleStat(stats, "spareSnapshotSeconds") >= 0.0,
                        "first publication or spare timing was not recorded");
        }

        // B: the snapshot is displayed while the spare job is still pending,
        // because only a later Prepare call can install it.
        {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            AddConversionScene(scene);
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(prepareUntilDisplayed(bridge, {}),
                        "manual preparation did not publish");
            VtDictionary const published = bridge.GetStats();
            ok &= Check(BoolStat(published, "preparationPending") &&
                            Stat(published, "spareSnapshotsInstalled") == 0 &&
                            Stat(published, "retainedSnapshotCount") == 1 &&
                            Stat(published, "snapshotBuilds") == 1,
                        "publication waited for the spare bank");
            ok &= Check(settle(bridge, {}), "spare bank did not settle");
            VtDictionary const settled = bridge.GetStats();
            ok &= Check(Stat(settled, "spareSnapshotsInstalled") == 1 &&
                            Stat(settled, "retainedSnapshotCount") == 2 &&
                            Stat(settled, "snapshotBuilds") == 1 &&
                            Stat(settled, "discardedSnapshotBuilds") == 0 &&
                            Stat(settled, "prototypeBuildCount") == 66,
                        "installing the spare bank counted as a snapshot build");
        }

        // C: a transform edit while the spare job runs cancels it; the edit
        // is still displayed and a following edit uses the rebuilt banks.
        {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            AddConversionScene(scene);
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(prepareUntilDisplayed(bridge, aovs) &&
                            BoolStat(bridge.GetStats(), "preparationPending"),
                        "spare job was not pending after publication");
            scene->AddPrims({{SdfPath("/Grid"), TfToken("xform"),
                              FixtureXformData(GfVec3d(100.0, 0.0, 0.0))}});
            bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                                  SdfPath("/Grid"));
            ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                    resolution, resolution),
                        "transform edit during the spare job did not prepare");
            VtDictionary const moved = bridge.GetStats();
            ok &= Check(Stat(moved, "spareSnapshotsDiscarded") +
                            Stat(moved, "spareSnapshotsInstalled") == 2 &&
                            Stat(moved, "spareSnapshotsInstalled") >= 1 &&
                            Stat(moved, "primCount") == 68 &&
                            Stat(moved, "triangleCount") == 64 * 3 + 3 + 2,
                        "spare job interrupted by an edit was not accounted for");
            size_t hits = 0;
            ok &= Check(renderFrame(bridge), "moved grid frame did not converge");
            ok &= Check(gridPixels(bridge, &hits) == 0 && hits > 0,
                        "transform edit during the spare job was not displayed");
            const int64_t reusedBefore = Stat(moved, "reusedSnapshotBuilds");
            scene->AddPrims({{SdfPath("/Grid"), TfToken("xform"),
                              FixtureXformData(GfVec3d(0.0, 0.0, 0.5))}});
            bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                                  SdfPath("/Grid"));
            ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                    resolution, resolution),
                        "second transform edit did not prepare");
            VtDictionary const returned = bridge.GetStats();
            ok &= Check(Stat(returned, "reusedSnapshotBuilds") ==
                            reusedBefore + 1 &&
                            Stat(returned, "snapshotBuilds") ==
                                Stat(moved, "snapshotBuilds") &&
                            Stat(returned, "primCount") == 68,
                        "second edit did not reuse the installed spare bank");
            ok &= Check(renderFrame(bridge), "returned grid frame did not converge");
            std::vector<int> const prims = ReadBuffer<int>(&primBuffer);
            const size_t center = (resolution / 2) * resolution + resolution / 2;
            ok &= Check(prims.size() == resolution * resolution &&
                            prims[center] >= 0 &&
                            bridge.GetPrimPath(prims[center]).GetParentPath() ==
                                SdfPath("/Grid"),
                        "second transform edit was not displayed");
            activeBridge = nullptr;
        }

        // D: the spare bank binds the same materials, including geom-subset
        // materials and a displayColor fallback, as the displayed snapshot.
        {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            scene->AddPrims({
                {SdfPath("/Looks"), TfToken("xform"),
                 FixtureXformData(GfVec3d(0.0))},
                {SdfPath("/Looks/Red"), HdPrimTypeTokens->material,
                 PreviewMaterialData(SdfPath("/Looks/Red"),
                                     GfVec3f(0.8f, 0.1f, 0.1f))},
                {SdfPath("/Looks/Blue"), HdPrimTypeTokens->material,
                 PreviewMaterialData(SdfPath("/Looks/Blue"),
                                     GfVec3f(0.1f, 0.1f, 0.8f))},
                {SdfPath("/Left"), TfToken("xform"),
                 FixtureXformData(GfVec3d(-1.1, 0.0, 0.0))},
                {SdfPath("/Left/Mesh"), HdPrimTypeTokens->mesh,
                 FixtureMeshData(false)},
                {SdfPath("/Left/Mesh/Quad"), HdPrimTypeTokens->geomSubset,
                 FaceSubsetData(VtIntArray{0}, SdfPath("/Looks/Red"))},
                {SdfPath("/Left/Mesh/Apex"), HdPrimTypeTokens->geomSubset,
                 FaceSubsetData(VtIntArray{1}, SdfPath("/Looks/Blue"))},
                {SdfPath("/Right"), TfToken("xform"),
                 FixtureXformData(GfVec3d(1.1, 0.0, 0.0))},
                {SdfPath("/Right/Mesh"), HdPrimTypeTokens->mesh,
                 FixtureMeshData(false, HdRenderTagTokens->geometry,
                                 GfVec3f(0.2f, 0.9f, 0.3f), 0.1f)},
                {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, DomeData()}});
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                    resolution, resolution),
                        "material parity scene did not prepare");
            VtDictionary const before = bridge.GetStats();
            ok &= Check(Stat(before, "spareSnapshotsInstalled") == 1 &&
                            Stat(before, "materialFailures") <= 0,
                        "material parity scene did not install a spare bank");
            ok &= Check(renderFrame(bridge), "material parity frame did not converge");
            std::vector<float> const colorsBefore = ReadBuffer<float>(&colorBuffer);
            std::vector<int> const primsBefore = ReadBuffer<int>(&primBuffer);
            std::vector<SdfPath> pathsBefore;
            for (int64_t id = 0; id < Stat(before, "primCount"); ++id)
                pathsBefore.push_back(bridge.GetPrimPath(static_cast<int>(id)));
            // The Apex subset is edge-on to this camera; the red Quad subset
            // and the green displayColor mesh are both in view.
            bool red = false, green = false;
            for (size_t pixel = 0; pixel < primsBefore.size(); ++pixel) {
                if (primsBefore[pixel] < 0) continue;
                float const r = colorsBefore[pixel * 4];
                float const g = colorsBefore[pixel * 4 + 1];
                float const b = colorsBefore[pixel * 4 + 2];
                SdfPath const path = bridge.GetPrimPath(primsBefore[pixel]);
                if (path == SdfPath("/Left/Mesh")) {
                    red |= r > 2.0f * b && r > 2.0f * g;
                } else if (path == SdfPath("/Right/Mesh")) {
                    green |= g > 2.0f * r && g > 2.0f * b;
                }
            }
            ok &= Check(red && green,
                        "subset materials or displayColor were not rendered");

            // Re-authoring an unchanged transform publishes the spare bank and
            // retires the snapshot that was displayed.
            scene->AddPrims({{SdfPath("/Right"), TfToken("xform"),
                              FixtureXformData(GfVec3d(1.1, 0.0, 0.0))}});
            bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                                  SdfPath("/Right"));
            ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                    resolution, resolution),
                        "bank swap did not prepare");
            VtDictionary const after = bridge.GetStats();
            ok &= Check(Stat(after, "reusedSnapshotBuilds") ==
                            Stat(before, "reusedSnapshotBuilds") + 1 &&
                            Stat(after, "snapshotBuilds") ==
                                Stat(before, "snapshotBuilds") &&
                            Stat(after, "primCount") == Stat(before, "primCount"),
                        "unchanged transform edit did not swap to the spare bank");
            ok &= Check(renderFrame(bridge), "swapped bank frame did not converge");
            std::vector<SdfPath> pathsAfter;
            for (int64_t id = 0; id < Stat(after, "primCount"); ++id)
                pathsAfter.push_back(bridge.GetPrimPath(static_cast<int>(id)));
            ok &= Check(pathsAfter == pathsBefore &&
                            ReadBuffer<int>(&primBuffer) == primsBefore,
                        "spare bank changed picking");
            ok &= Check(ReadBuffer<float>(&colorBuffer) == colorsBefore,
                        "spare bank changed material assignments");
            activeBridge = nullptr;
        }

        // F: a spare that finished while nobody called Prepare is installed
        // by the Prepare that follows an edit, so the edit stays incremental.
        {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            AddConversionScene(scene);
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(prepareUntilDisplayed(bridge, aovs),
                        "idle-host scene did not publish");
            ok &= Check(WaitFor([&] {
                return BoolStat(bridge.GetStats(), "spareSnapshotReady");
            }), "spare job did not finish without Prepare calls");
            VtDictionary const ready = bridge.GetStats();
            ok &= Check(BoolStat(ready, "preparationPending") &&
                            Stat(ready, "spareSnapshotsInstalled") == 0,
                        "reading spareSnapshotReady consumed the spare job");
            scene->AddPrims({{SdfPath("/Grid"), TfToken("xform"),
                              FixtureXformData(GfVec3d(0.0, 0.0, 0.5))}});
            bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                                  SdfPath("/Grid"));
            ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                    resolution, resolution),
                        "edit after an unconsumed spare did not prepare");
            VtDictionary const edited = bridge.GetStats();
            ok &= Check(Stat(edited, "spareSnapshotsInstalled") ==
                            Stat(ready, "spareSnapshotsInstalled") + 1 &&
                            Stat(edited, "spareSnapshotsDiscarded") ==
                                Stat(ready, "spareSnapshotsDiscarded") &&
                            Stat(edited, "snapshotBuilds") ==
                                Stat(ready, "snapshotBuilds") &&
                            Stat(edited, "reusedSnapshotBuilds") ==
                                Stat(ready, "reusedSnapshotBuilds") + 1 &&
                            Stat(edited, "primCount") == 68,
                        "finished spare was discarded instead of taking the edit");
            ok &= Check(renderFrame(bridge), "idle-host edit frame did not converge");
            std::vector<int> const prims = ReadBuffer<int>(&primBuffer);
            const size_t center = (resolution / 2) * resolution + resolution / 2;
            ok &= Check(prims.size() == resolution * resolution &&
                            prims[center] >= 0 &&
                            bridge.GetPrimPath(prims[center]).GetParentPath() ==
                                SdfPath("/Grid"),
                        "edit applied to the installed spare was not displayed");
            activeBridge = nullptr;
        }

        // G: a structural edit after an unconsumed spare still installs it,
        // then rebuilds because structure cannot be replayed on a bank.
        {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            AddConversionScene(scene);
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(prepareUntilDisplayed(bridge, {}),
                        "structural idle-host scene did not publish");
            ok &= Check(WaitFor([&] {
                return BoolStat(bridge.GetStats(), "spareSnapshotReady");
            }), "structural idle-host spare did not finish");
            VtDictionary const ready = bridge.GetStats();
            scene->AddPrims({{SdfPath("/Extra"), HdPrimTypeTokens->mesh,
                              FixtureMeshData(false, HdRenderTagTokens->geometry,
                                              GfVec3f(0.7f), 0.95f)}});
            bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                                  SdfPath("/Extra"));
            ok &= Check(PumpPrepare(bridge, camera, {}, geometryTags,
                                    resolution, resolution),
                        "structural edit after an unconsumed spare did not prepare");
            VtDictionary const edited = bridge.GetStats();
            ok &= Check(Stat(edited, "primCount") == 69 &&
                            Stat(edited, "prototypeCount") == 67 &&
                            Stat(edited, "triangleCount") == 64 * 3 + 3 + 2 + 3 &&
                            Stat(edited, "instanceCount") == 2,
                        "structural edit after an unconsumed spare lost scene contents");
            ok &= Check(Stat(edited, "snapshotBuilds") ==
                            Stat(ready, "snapshotBuilds") + 1 &&
                            Stat(edited, "reusedSnapshotBuilds") ==
                                Stat(ready, "reusedSnapshotBuilds") &&
                            Stat(edited, "spareSnapshotsInstalled") ==
                                Stat(ready, "spareSnapshotsInstalled") + 2 &&
                            Stat(edited, "spareSnapshotsDiscarded") ==
                                Stat(ready, "spareSnapshotsDiscarded") &&
                            Stat(edited, "retainedSnapshotCount") == 2,
                        "structural edit did not rebuild with a fresh spare bank");
        }

        // H: an edit while the spare job is still running cancels it before
        // it completes; that spare is discarded and the edit is displayed.
        // Enough meshes keep the spare job running well past the edit.
        {
            constexpr int meshCount = 10000;
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            HdRetainedSceneIndex::AddedPrimEntries prims;
            prims.reserve(meshCount + 1);
            prims.push_back({SdfPath("/Grid"), TfToken("xform"),
                             FixtureXformData(GfVec3d(0.0))});
            for (int i = 0; i < meshCount; ++i)
                prims.push_back({SdfPath("/Grid/M" + std::to_string(i)),
                    HdPrimTypeTokens->mesh,
                    FixtureMeshData(false, HdRenderTagTokens->geometry,
                                    GfVec3f(0.7f),
                                    0.00005f * static_cast<float>(i + 1))});
            scene->AddPrims(prims);
            prism::hydra::SceneBridge bridge(scene, &thread);
            bridge.SetSettings(settings);
            ok &= Check(prepareUntilDisplayed(bridge, aovs),
                        "large spare scene did not publish");
            ok &= Check(!BoolStat(bridge.GetStats(), "spareSnapshotReady"),
                        "spare job finished before the edit could interrupt it");
            scene->AddPrims({{SdfPath("/Grid"), TfToken("xform"),
                              FixtureXformData(GfVec3d(100.0, 0.0, 0.0))}});
            bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                                  SdfPath("/Grid"));
            ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags,
                                    resolution, resolution),
                        "edit during a running spare job did not prepare");
            VtDictionary const edited = bridge.GetStats();
            ok &= Check(Stat(edited, "spareSnapshotsDiscarded") == 1 &&
                            Stat(edited, "spareSnapshotsInstalled") == 1 &&
                            Stat(edited, "primCount") == meshCount &&
                            Stat(edited, "retainedSnapshotCount") == 2,
                        "interrupted spare job was not discarded");
            size_t hits = 0;
            ok &= Check(renderFrame(bridge), "large moved frame did not converge");
            ok &= Check(gridPixels(bridge, &hits) == 0,
                        "edit during a running spare job was not displayed");
            activeBridge = nullptr;
        }

        // E: tearing a bridge down while its spare job is pending must cancel
        // and join it like any other build.
        for (int repetition = 0; repetition < 5; ++repetition) {
            HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
            AddConversionScene(scene);
            prism::hydra::SceneBridge bridge(scene, &thread);
            ok &= Check(prepareUntilDisplayed(bridge, {}) &&
                            BoolStat(bridge.GetStats(), "preparationPending"),
                        "spare job was not pending before teardown");
        }
    }
    {
        // Until a bound buffer shows a rendered frame it must hold its AOV
        // clear value: the host blends the delegate's colour over its own
        // background, so the zero-filled allocation would show as black.
        const TfTokenVector geometryTags{HdRenderTagTokens->geometry};
        prism::hydra::CameraState camera;
        GfFrustum frustum;
        frustum.SetPerspective(100.0, 1.0, 0.1, 100.0);
        camera.projection = frustum.ComputeProjectionMatrix();
        camera.worldToView.SetTranslate(GfVec3d(0, 0, -3));
        camera.nearClip = 0.1f;
        camera.farClip = 100.0f;
        prism::hydra::RenderSettings settings;
        settings.samples = 1;
        settings.maxDepth = 1;
        settings.refinementDelayMs = 0;
        const GfVec4f colorClear(0.25f, 0.5f, 0.75f, 1.0f);
        auto isCleared = [&](PrismRenderBuffer *color, PrismRenderBuffer *depth) {
            const std::vector<float> c = ReadBuffer<float>(color);
            const std::vector<float> d = ReadBuffer<float>(depth);
            bool same = !c.empty() && c.size() == static_cast<size_t>(
                color->GetWidth()) * color->GetHeight() * 4 &&
                d.size() == static_cast<size_t>(depth->GetWidth()) *
                    depth->GetHeight();
            for (size_t i = 0; same && i < c.size(); ++i)
                same &= c[i] == colorClear[i % 4];
            for (size_t i = 0; same && i < d.size(); ++i) same &= d[i] == 1.0f;
            return same;
        };
        auto allocate = [&](PrismRenderBuffer &color, PrismRenderBuffer &depth,
                            unsigned resolution) {
            color.Allocate(GfVec3i(resolution, resolution, 1),
                           HdFormatFloat32Vec4, false);
            depth.Allocate(GfVec3i(resolution, resolution, 1),
                           HdFormatFloat32, false);
        };
        PrismRenderBuffer color(SdfPath("/LoadColor"), &thread);
        PrismRenderBuffer depth(SdfPath("/LoadDepth"), &thread);
        allocate(color, depth, 8);
        HdRenderPassAovBindingVector aovs(2);
        aovs[0].aovName = HdAovTokens->color;
        aovs[0].renderBuffer = &color;
        aovs[0].clearValue = VtValue(colorClear);
        aovs[1].aovName = HdAovTokens->depth;
        aovs[1].renderBuffer = &depth;
        aovs[1].clearValue = VtValue(1.0f);
        HdRetainedSceneIndexRefPtr loadScene = HdRetainedSceneIndex::New();
        AddConversionScene(loadScene);
        prism::hydra::SceneBridge bridge(loadScene, &thread);
        bridge.SetSettings(settings);

        bridge.Prepare(camera, aovs, geometryTags, 8, 8);
        ok &= Check(isCleared(&color, &depth),
                    "first Prepare left a bound buffer without its clear value");
        ok &= Check(PumpPrepare(bridge, camera, aovs, geometryTags, 8, 8),
                    "cleared-buffer scene did not prepare");
        ok &= Check(isCleared(&color, &depth),
                    "buffer changed before any frame was rendered");
        activeBridge = &bridge;
        thread.StartRender();
        ok &= Check(WaitFor([&] {
            return bridge.IsConverged() &&
                Stat(bridge.GetStats(), "completedSamples") > 0;
        }), "cleared-buffer scene did not render");
        thread.StopRender();
        ok &= Check(!isCleared(&color, &depth),
                    "rendered image was not written over the clear value");
        const std::vector<float> shown = ReadBuffer<float>(&color);
        for (int i = 0; i < 3; ++i)
            bridge.Prepare(camera, aovs, geometryTags, 8, 8);
        ok &= Check(ReadBuffer<float>(&color) == shown,
                    "Prepare erased a displayed image");

        // A resize reallocates zero-filled storage; the next Prepare must
        // replace it before anything can present it.
        allocate(color, depth, 12);
        bridge.Prepare(camera, aovs, geometryTags, 12, 12);
        ok &= Check(isCleared(&color, &depth),
                    "resized buffers were not cleared before a new frame");
        activeBridge = nullptr;
    }
    thread.StopThread();
    return ok ? 0 : 1;
}
