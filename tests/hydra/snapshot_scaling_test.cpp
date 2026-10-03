#include "integration_fixture.h"

#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>

#include <algorithm>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

HdContainerDataSourceHandle InstancedPlane(
    HdContainerDataSourceHandle mesh = {})
{
    if (!mesh) mesh = prism::hydra::test::Plane();
    HdContainerDataSourceHandle const instancedBy =
        HdInstancedBySchema::Builder().SetPaths(
            HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                {SdfPath("/Instancer")})).Build();
    return HdOverlayContainerDataSource::New(
        mesh,
        HdRetainedContainerDataSource::New(
            HdInstancedBySchemaTokens->instancedBy, instancedBy));
}

HdContainerDataSourceHandle InstancerData(VtIntArray const &sourceIndices,
                                           float offset)
{
    HdDataSourceBaseHandle const indices[] = {
        HdRetainedTypedSampledDataSource<VtIntArray>::New(sourceIndices)};
    HdContainerDataSourceHandle const topology =
        HdInstancerTopologySchema::Builder()
            .SetPrototypes(HdRetainedTypedSampledDataSource<
                VtArray<SdfPath>>::New({SdfPath("/Proto")}))
            .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(
                1, indices))
            .Build();
    auto translations = HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<
            VtVec3fArray>::New({{-.5f,0,0},{.5f,0,0},{0,0,0},{0,0,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->instance)).Build();
    auto primvars = HdRetainedContainerDataSource::New(
        HdInstancerTokens->instanceTranslations, translations);
    GfMatrix4d matrix(1.0);
    matrix.SetTranslate(GfVec3d(offset, 0, 0));
    auto xform = HdXformSchema::Builder()
        .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix))
        .Build();
    return HdRetainedContainerDataSource::New(
        HdInstancerTopologySchemaTokens->instancerTopology, topology,
        HdPrimvarsSchemaTokens->primvars, primvars,
        HdXformSchemaTokens->xform, xform);
}

std::shared_ptr<const prism::hydra::PickingTable> PublishedPicking(
    prism::hydra::test::Fixture const &fixture)
{
    auto const frame = fixture.color.Capture().frame;
    prism::hydra::test::Require(frame && frame->picking,
        "Published render buffer has no picking table");
    return frame->picking;
}

std::vector<int> const &TopologyFor(
    prism::hydra::PickingTable const &table, SdfPath const &path)
{
    auto const found = std::find(table.primPaths.begin(), table.primPaths.end(), path);
    prism::hydra::test::Require(found != table.primPaths.end(),
        "Picking table lost an instanced prototype path");
    const size_t primId = static_cast<size_t>(found - table.primPaths.begin());
    prism::hydra::test::Require(primId < table.instanceTopologies.size() &&
        !table.instanceTopologies[primId].empty(),
        "Picking table lost instance topology");
    return table.instanceTopologies[primId].front().sourceIndicesByOrdinal;
}

size_t LogicalPlacementCountFor(
    prism::hydra::PickingTable const &table, SdfPath const &path)
{
    auto const found = std::find(table.primPaths.begin(), table.primPaths.end(), path);
    prism::hydra::test::Require(found != table.primPaths.end(),
        "Picking table has no logical placement row for the path");
    const size_t primId = static_cast<size_t>(found - table.primPaths.begin());
    prism::hydra::test::Require(primId < table.logicalPlacementCountsByPrim.size(),
        "Picking table placement-count metadata is incomplete");
    return table.logicalPlacementCountsByPrim[primId];
}

} // namespace

int RunSnapshotScalingTests()
{
    using namespace prism::hydra;
    using namespace prism::hydra::test;
    try {
        Fixture fixture;
        fixture.scene->AddPrims({
            {SdfPath("/Proto"), HdPrimTypeTokens->mesh, InstancedPlane()},
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData({1,3}, 0.0f)},
            {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()}});
        fixture.Render();
        auto picking = PublishedPicking(fixture);
        auto const sourceIndicesBefore = TopologyFor(*picking, SdfPath("/Proto"));
        Require(picking->logicalPlacementCountsByPrim.size() ==
                    picking->primPaths.size() &&
                LogicalPlacementCountFor(*picking, SdfPath("/Proto")) == 2,
                "Picking table did not record both logical instances");

        auto assertSameTable = [&](char const *reason) {
            fixture.Render();
            Require(PublishedPicking(fixture) == picking, reason);
        };
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Material,
                                       SdfPath("/Proto"));
        assertSameTable("Material-only generation rebuilt the picking table");
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Environment,
                                       SdfPath("/Dome"));
        assertSameTable("Environment-only generation rebuilt the picking table");

        fixture.scene->AddPrims({
            {SdfPath("/Proto"), HdPrimTypeTokens->mesh, InstancedPlane()}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,
                                       SdfPath("/Proto"));
        assertSameTable("Geometry-only prototype refresh rebuilt the picking table");
        fixture.scene->AddPrims({
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData({1,3}, 0.25f)}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,
                                       SdfPath("/Instancer"));
        assertSameTable("Transform-only placement rebuilt the picking table");

        fixture.scene->AddPrims({
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData({3,1}, 0.0f)}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,
                                       SdfPath("/Instancer"));
        fixture.Render();
        auto changedPicking = PublishedPicking(fixture);
        Require(changedPicking != picking,
                "Changed same-count instance mapping reused stale picking table");
        Require(TopologyFor(*changedPicking, SdfPath("/Proto")) !=
                    sourceIndicesBefore,
                "Changed instance source-index ordering was not republished");
        fixture.scene->AddPrims({
            {SdfPath("/Instancer"), HdPrimTypeTokens->instancer,
             InstancerData({3}, 0.0f)}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,
                                       SdfPath("/Instancer"));
        fixture.Render();
        Fixture ordinary;
        ordinary.scene->AddPrims({
            {SdfPath("/Ordinary"), HdPrimTypeTokens->mesh,
             Plane()},
            {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()}});
        ordinary.Render();
        auto const ordinaryBefore = PublishedPicking(ordinary);
        auto ordinaryPath = std::find(ordinaryBefore->primPaths.begin(),
            ordinaryBefore->primPaths.end(), SdfPath("/Ordinary"));
        Require(ordinaryPath != ordinaryBefore->primPaths.end(),
                "Ordinary prim was missing from the picking table");
        const size_t ordinaryId = static_cast<size_t>(
            ordinaryPath - ordinaryBefore->primPaths.begin());
        Require(ordinaryBefore->logicalPlacementCountsByPrim[ordinaryId] == 1 &&
                ordinaryBefore->instanceTopologies[ordinaryId].empty(),
                "Ordinary prim did not have one empty-topology placement row");

        ordinary.scene->RemovePrims({SdfPath("/Ordinary")});
        ordinary.bridge->MarkSceneDirty(SceneDirtyKind::Structure,
                                        SdfPath("/Ordinary"));
        ordinary.Render();
        auto const ordinaryRemoved = PublishedPicking(ordinary);
        Require(std::find(ordinaryRemoved->primPaths.begin(),
                          ordinaryRemoved->primPaths.end(),
                          SdfPath("/Ordinary")) == ordinaryRemoved->primPaths.end() &&
                ordinaryRemoved->logicalPlacementCountsByPrim.empty(),
                "Removing an ordinary prim left a stale logical placement row");

        ordinary.scene->AddPrims({
            {SdfPath("/Ordinary"), HdPrimTypeTokens->mesh, Plane()}});
        ordinary.bridge->MarkSceneDirty(SceneDirtyKind::Structure,
                                        SdfPath("/Ordinary"));
        ordinary.Render();
        auto const ordinaryAdded = PublishedPicking(ordinary);
        ordinaryPath = std::find(ordinaryAdded->primPaths.begin(),
            ordinaryAdded->primPaths.end(), SdfPath("/Ordinary"));
        Require(ordinaryPath != ordinaryAdded->primPaths.end(),
                "Re-added ordinary prim was missing from picking table");
        const size_t readdedId = static_cast<size_t>(
            ordinaryPath - ordinaryAdded->primPaths.begin());
        Require(ordinaryAdded->logicalPlacementCountsByPrim[readdedId] == 1 &&
                ordinaryAdded->instanceTopologies[readdedId].empty(),
                "Re-added ordinary prim has stale instance topology or placement count");
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
