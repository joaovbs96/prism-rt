#include "../../src/hydra/scene_bridge.h"
#include "../../src/hydra/render_buffer.h"
#include "../../src/hydra/triangulation.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
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

HdContainerDataSourceHandle MeshData(bool holeSecondFace,
                                     TfToken const &purpose = HdRenderTagTokens->geometry)
{
    const VtVec3fArray points{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0},
                              {-1, 1, 0}, {0, 0, 1}};
    HdContainerDataSourceHandle pointsPrimvar = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->vertex))
        .Build();
    HdContainerDataSourceHandle primvars = HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->points, pointsPrimvar);
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

HdContainerDataSourceHandle InstancedMeshData()
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
        MeshData(true));
}

HdContainerDataSourceHandle InstancerData()
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
    HdContainerDataSourceHandle primvars = HdRetainedContainerDataSource::New(
        HdInstancerTokens->instanceTranslations, translations);
    GfMatrix4d instancerMatrix(1.0);
    instancerMatrix.SetTranslate(GfVec3d(1, 0, 0));
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

} // namespace

int main()
{
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
                      MeshData(true)}});
    HdRenderThread thread;
    prism::hydra::SceneBridge *activeBridge = nullptr;
    thread.SetRenderCallback([&] {
        if (activeBridge) activeBridge->Render();
    });
    thread.StartThread();
    bool ok = true;
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
                          MeshData(false)}});
        bridge.MarkSceneDirty();
        bridge.Prepare(camera, {}, geometryTags, 32, 32);
        ok &= Check(Stat(bridge.GetStats(), "triangleCount") == 3,
                    "topology invalidation did not rebuild triangles");

        scene->AddPrims({{SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                          MeshData(false, HdRenderTagTokens->guide)}});
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
        ok &= Check(Stat(stats, "meshCount") == 2 &&
                        Stat(stats, "instanceCount") == 2,
                    "point-instancer placements were not flattened");
        ok &= Check(Stat(stats, "triangleCount") == 4,
                    "flattened instances lost prototype triangles");

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
        bridge.SetSettings(renderSettings);
        bridge.Prepare(camera, aovs, geometryTags, resolution, resolution);
        stats = bridge.GetStats();
        const std::vector<float> previewColor = ReadBuffer<float>(&colorBuffer);
        bool previewAlpha = previewColor.size() == resolution * resolution * 4;
        for (size_t pixel = 0; previewAlpha && pixel < resolution * resolution;
             ++pixel)
            previewAlpha = previewColor[pixel * 4 + 3] == 1.0f;
        ok &= Check(Stat(stats, "previewPublishes") > 0 &&
                        Stat(stats, "previewWidth") == resolution &&
                        Stat(stats, "previewHeight") == resolution &&
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
        bridge.SetSettings(renderSettings);
        bridge.Prepare(camera, aovs, geometryTags, resolution, resolution);
        thread.StartRender();
        ok &= Check(WaitFor([&] { return bridge.IsConverged(); }),
                    "one-sample restart did not converge");
        std::vector<int> const primIds = ReadBuffer<int>(&primBuffer);
        std::vector<int> const instanceIds = ReadBuffer<int>(&instanceBuffer);
        std::vector<int> const elementIds = ReadBuffer<int>(&elementBuffer);
        std::vector<float> const normals = ReadBuffer<float>(&normalBuffer);
        std::vector<float> const depths = ReadBuffer<float>(&depthBuffer);
        bool foundInstance0 = false, foundInstance1 = false;
        bool validAovs = false;
        for (size_t pixel = 0; pixel < primIds.size(); ++pixel) {
            if (primIds[pixel] < 0) continue;
            foundInstance0 |= instanceIds[pixel] == 0;
            foundInstance1 |= instanceIds[pixel] == 1;
            const float normalLength = GfVec3f(
                normals[pixel * 3], normals[pixel * 3 + 1],
                normals[pixel * 3 + 2]).GetLength();
            validAovs |= elementIds[pixel] >= 0 && normalLength > 0.9f &&
                std::isfinite(depths[pixel]) && depths[pixel] >= 0.0f &&
                depths[pixel] <= 1.0f;
        }
        ok &= Check(foundInstance0 && foundInstance1,
                    "instance transforms or flattened instance IDs are incorrect");
        ok &= Check(validAovs,
                    "primary-hit depth, normal, or coarse-face AOV is invalid");

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
