#include "../../src/hydra/scene_bridge.h"
#include "../../src/hydra/render_buffer.h"
#include "../../src/hydra/triangulation.h"
#include "../../src/scene.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
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
#include <pxr/imaging/hd/xformSchema.h>

#include <cstdint>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

HdContainerDataSourceHandle FixtureMeshData(
    bool holeSecondFace,
    TfToken const &purpose = HdRenderTagTokens->geometry,
    GfVec3f const &displayColor = GfVec3f(0.7f),
    float pointOffset = 0.0f)
{
    const VtVec3fArray points{{-1 + pointOffset, -1, 0}, {1, -1, 0}, {1, 1, 0},
                              {-1, 1, 0}, {0, 0, 1}};
    HdContainerDataSourceHandle pointsPrimvar = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points))
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

HdContainerDataSourceHandle InstancerData(double offset = 1.0)
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
        HdXformSchemaTokens->xform, xform);
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

} // namespace

int main()
{
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
        prism::hydra::SceneBridge bridge(scene, &thread);
        activeBridge = &bridge;
        prism::hydra::CameraState camera;
        const TfTokenVector geometryTags{HdRenderTagTokens->geometry};
        ok &= Check(bridge.Prepare(camera, {}, geometryTags, 32, 32),
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

        camera.projection[0][0] = 2.0;
        ok &= Check(bridge.Prepare(camera, {}, geometryTags, 32, 32),
                    "camera edit did not reset accumulation");
        ok &= Check(Stat(bridge.GetStats(), "snapshotBuilds") == 1,
                    "camera-only edit rebuilt geometry");

        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false)}});
        bridge.MarkSceneDirty();
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        ok &= Check(Stat(bridge.GetStats(), "triangleCount") == 3,
                    "topology invalidation did not rebuild triangles");

        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(false, HdRenderTagTokens->guide)}});
        bridge.MarkSceneDirty();
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        ok &= Check(Stat(bridge.GetStats(), "meshCount") == 0,
                    "purpose filtering did not exclude guide mesh");

        scene->RemovePrims({SdfPath("/Mesh")});
        bridge.MarkSceneDirty();
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
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
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "meshCount") == 1 &&
                        Stat(stats, "instanceCount") == 2,
                    "point-instancer placements did not share a prototype");
        ok &= Check(Stat(stats, "prototypeCount") == 1 &&
                        Stat(stats, "triangleCount") == 2,
                    "native instances duplicated prototype storage");
        const int64_t buildsBeforeTransform = Stat(stats, "snapshotBuilds");
        scene->AddPrims({{SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
                          InstancerData(1.25)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Placement,
                              SdfPath("/Instancer"));
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeTransform &&
                        Stat(stats, "placementInvalidations") > 0 &&
                        Stat(stats, "prototypeCount") == 1,
                    "transform edit rebuilt prototype geometry");

        scene->AddPrims({{SdfPath("/StaticMesh"), HdPrimTypeTokens->mesh,
                          FixtureMeshData(true)}});
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Structure,
                              SdfPath("/StaticMesh"));
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
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
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforePrototypeEdit &&
                        Stat(stats, "prototypeBuildCount") ==
                            prototypeBuildsBeforeEdit + 1 &&
                        Stat(stats, "prototypeCount") == 2,
                    "topology edit rebuilt unaffected prototypes");
        const int64_t buildsBeforeStateEdits = Stat(stats, "snapshotBuilds");
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Material,
                              SdfPath("/Looks/Unused"));
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        bridge.MarkSceneDirty(prism::hydra::SceneDirtyKind::Environment,
                              SdfPath("/Dome"));
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        stats = bridge.GetStats();
        ok &= Check(Stat(stats, "snapshotBuilds") == buildsBeforeStateEdits &&
                        Stat(stats, "prototypeCount") == 2 &&
                        Stat(stats, "materialInvalidations") > 0 &&
                        Stat(stats, "environmentInvalidations") > 0,
                    "material or dome edit rebuilt prototype storage");
        auto resourceSettings = bridge.GetSettings();
        ++resourceSettings.resourceRevision;
        bridge.SetSettings(resourceSettings);
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
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
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
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
        bridge.Prepare(camera, aovs, geometryTags, resolution, resolution);
        stats = bridge.GetStats();
        const std::vector<float> previewColor = ReadBuffer<float>(&colorBuffer);
        bool previewAlpha = previewColor.size() == resolution * resolution * 4;
        for (size_t pixel = 0; previewAlpha && pixel < resolution * resolution;
             ++pixel)
            previewAlpha = previewColor[pixel * 4 + 3] == 1.0f;
        ok &= Check(Stat(stats, "previewPublishes") > 0 &&
                        Stat(stats, "previewWidth") <= 16 &&
                        Stat(stats, "previewHeight") <= 16 &&
                        Stat(stats, "completedSamples") == 0,
                    "bounded camera preview was not published synchronously");
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
        bridge.Prepare(camera, aovs, geometryTags, resolution, resolution);
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
            }
        }
        ok &= Check(foundInstance0 && foundInstance1,
                    "instance transforms or flattened instance IDs are incorrect");
        ok &= Check(validAovs,
                    "primary-hit depth, normal, or coarse-face AOV is invalid");
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
        bridge.Prepare(camera, aovs, geometryTags, resolution, resolution);
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
                        Stat(beforeColorStats, "prototypeBuildCount") + 1,
                    "displayColor edit rebuilt an unexpected number of prototypes");
        ok &= Check(Stat(afterColorStats, "prototypeCount") == 2,
                    "displayColor edit changed unique prototype storage");

        camera.worldToView.SetTranslate(GfVec3d(0, 0, -4));
        const int64_t previewsBeforeMove = Stat(
            bridge.GetStats(), "previewPublishes");
        ok &= Check(bridge.Prepare(camera, aovs, geometryTags,
                                   resolution, resolution),
                    "camera change did not start a replacement frame");
        const std::vector<float> previewDepths = ReadBuffer<float>(&depthBuffer);
        ok &= Check(Stat(bridge.GetStats(), "previewPublishes") ==
                        previewsBeforeMove + 1 && previewDepths != depths,
                    "camera change did not synchronously publish current depth");
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "camera replacement frame did not converge");
        const std::vector<float> movedDepths = ReadBuffer<float>(&depthBuffer);
        const bool depthChanged = movedDepths.size() == depths.size() &&
            !std::equal(movedDepths.begin(), movedDepths.end(), depths.begin());
        ok &= Check(depthChanged,
                    "camera replacement did not publish updated depth");
        thread.StopRender();
        activeBridge = nullptr;
    }
    thread.StopThread();
    return ok ? 0 : 1;
}
