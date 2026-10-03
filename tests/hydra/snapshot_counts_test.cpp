#include "integration_fixture.h"

#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>

#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

HdContainerDataSourceHandle TrianglePlane()
{
    auto const topology = HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<
            VtIntArray>::New({3}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<
            VtIntArray>::New({0,1,2})).Build();
    auto const mesh = HdMeshSchema::Builder().SetTopology(topology).Build();
    return HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(HdMeshSchemaTokens->mesh, mesh),
        prism::hydra::test::Plane());
}

HdContainerDataSourceHandle Instanced(
    HdContainerDataSourceHandle mesh)
{
    auto const instancedBy = HdInstancedBySchema::Builder().SetPaths(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
            {SdfPath("/Instancer")})).Build();
    return HdOverlayContainerDataSource::New(mesh,
        HdRetainedContainerDataSource::New(
            HdInstancedBySchemaTokens->instancedBy, instancedBy));
}

HdContainerDataSourceHandle InstancerData(VtIntArray const &sourceIndices)
{
    HdDataSourceBaseHandle const indices[] = {
        HdRetainedTypedSampledDataSource<VtIntArray>::New(sourceIndices)};
    auto const topology = HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<
            VtArray<SdfPath>>::New({SdfPath("/Proto")}))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(1, indices))
        .Build();
    auto const translations = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<
            VtVec3fArray>::New({{-.5f,0,0},{.5f,0,0},{0,0,0},{0,0,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->instance)).Build();
    auto const primvars = HdRetainedContainerDataSource::New(
        HdInstancerTokens->instanceTranslations, translations);
    GfMatrix4d matrix(1.0);
    auto const xform = HdXformSchema::Builder()
        .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix))
        .Build();
    return HdRetainedContainerDataSource::New(
        HdInstancerTopologySchemaTokens->instancerTopology, topology,
        HdPrimvarsSchemaTokens->primvars, primvars,
        HdXformSchemaTokens->xform, xform);
}

void CheckCounts(prism::hydra::SceneBridge &bridge, int64_t meshes,
                 int64_t triangles, int64_t prims, int64_t slots,
                 int64_t instances, char const *reason)
{
    auto const stats = bridge.GetStats();
    auto const stat = [&](char const *name) {
        return prism::hydra::test::Stat(stats, name);
    };
    prism::hydra::test::Require(
        stat("meshCount") == meshes &&
        stat("triangleCount") == triangles &&
        stat("primCount") == prims &&
        stat("primPathSlotCount") == slots &&
        stat("instanceCount") == instances,
        reason);
}

} // namespace

int RunCachedCountTests()
{
    using namespace pxr;
    using namespace prism::hydra;
    using namespace prism::hydra::test;
    try {
        Fixture fixture;
        fixture.scene->AddPrims({
            {SdfPath("/Proto"), HdPrimTypeTokens->mesh, Instanced(Plane())},
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData({1,3})},
            {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()}});
        fixture.Render();
        CheckCounts(*fixture.bridge, 1, 2, 1, 1, 2,
            "Instanced geometry diagnostics multiplied unique storage counts");

        fixture.scene->AddPrims({
            {SdfPath("/Proto"), HdPrimTypeTokens->mesh,
             Instanced(TrianglePlane())}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,
                                       SdfPath("/Proto"));
        fixture.Render();
        CheckCounts(*fixture.bridge, 1, 1, 1, 1, 2,
            "Triangle prototype refresh did not update cached geometry counts");

        fixture.scene->AddPrims({
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData({3})}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,
                                       SdfPath("/Instancer"));
        fixture.Render();
        CheckCounts(*fixture.bridge, 1, 1, 1, 1, 1,
            "Changing placement count changed unique mesh or triangle counts");

        Fixture ordinary;
        ordinary.scene->AddPrims({
            {SdfPath("/Ordinary"), HdPrimTypeTokens->mesh, Plane()},
            {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()}});
        ordinary.Render();
        CheckCounts(*ordinary.bridge, 1, 2, 1, 1, 0,
            "Initial non-instanced snapshot cached incorrect diagnostics");
        ordinary.scene->RemovePrims({SdfPath("/Ordinary")});
        ordinary.bridge->MarkSceneDirty(SceneDirtyKind::Structure,
                                        SdfPath("/Ordinary"));
        ordinary.Render();
        CheckCounts(*ordinary.bridge, 0, 0, 0, 0, 0,
            "Structural removal left stale cached diagnostics");
        ordinary.scene->AddPrims({
            {SdfPath("/Ordinary"), HdPrimTypeTokens->mesh, Plane()}});
        ordinary.bridge->MarkSceneDirty(SceneDirtyKind::Structure,
                                        SdfPath("/Ordinary"));
        ordinary.Render();
        CheckCounts(*ordinary.bridge, 1, 2, 1, 1, 0,
            "Structural addition failed to refresh cached diagnostics");
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
