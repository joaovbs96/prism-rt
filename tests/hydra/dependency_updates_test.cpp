#include "integration_fixture.h"
#include "../../src/hydra/task_delegate.h"
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <iostream>

namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;
HdContainerDataSourceHandle Triangle(float offset) {
    auto topology=HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New({3}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New({0,1,2})).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdMeshSchemaTokens->mesh,HdMeshSchema::Builder().SetTopology(topology).Build()),Plane(offset));
}
HdContainerDataSourceHandle InstancedMesh() {
    auto by=HdInstancedBySchema::Builder().SetPaths(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({SdfPath("/Instancer")})).Build();
    return HdOverlayContainerDataSource::New(Plane(),HdRetainedContainerDataSource::New(
        HdInstancedBySchemaTokens->instancedBy,by));
}
HdContainerDataSourceHandle Instances(VtIntArray indices,float first=-.5f) {
    HdDataSourceBaseHandle arrays[]={HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)};
    auto topology=HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({SdfPath("/Proto")}))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(1,arrays)).Build();
    auto translations=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New({{first,0,0},{.5f,0,0},{.8f,0,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->instance)).Build();
    return HdRetainedContainerDataSource::New(HdInstancerTopologySchemaTokens->instancerTopology,topology,
        HdPrimvarsSchemaTokens->primvars,HdRetainedContainerDataSource::New(
            HdInstancerTokens->instanceTranslations,translations));
}
HdContainerDataSourceHandle NestedInstances(VtIntArray indices,bool outer) {
    auto data=Instances(indices);
    if(!outer) {
        auto by=HdInstancedBySchema::Builder().SetPaths(
            HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({SdfPath("/Outer")})).Build();
        return HdOverlayContainerDataSource::New(data,HdRetainedContainerDataSource::New(
            HdInstancedBySchemaTokens->instancedBy,by));
    }
    HdDataSourceBaseHandle arrays[]={HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)};
    auto topology=HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({SdfPath("/Instancer")}))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(1,arrays)).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdInstancerTopologySchemaTokens->instancerTopology,topology),data);
}
}

int RunDependencyUpdateTests() {
    try {
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,Plane(-.5f)},
                {SdfPath("/B"),HdPrimTypeTokens->mesh,Plane(.5f)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            fixture.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,Triangle(-.2f)}});
            fixture.bridge->MarkSceneDirtyBatch({
                {SceneDirtyKind::Prototype,{SdfPath("/A")},{}},
                {SceneDirtyKind::Placement,{SdfPath("/A")},{}}});
            fixture.Render(); const auto edited=fixture.bridge->GetStats();
            Require(Stat(edited,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                    Stat(edited,"topLevelSceneCommits")==Stat(before,"topLevelSceneCommits")+1 &&
                    Stat(edited,"lastPrototypePathsUpdated")==1 &&
                    Stat(edited,"lastPrototypePlacementsRepointed")==1 &&
                    Stat(edited,"lastPlacementTransformsUpdated")==1 && Stat(edited,"triangleCount")==3,
                    "Combined prototype/placement edit rebuilt TLAS or updated unrelated placements");
            Fixture fresh;
            fresh.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,Triangle(-.2f)},
                {SdfPath("/B"),HdPrimTypeTokens->mesh,Plane(.5f)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fresh.Render();
            Require(Read<int>(fixture.prim)==Read<int>(fresh.prim) &&
                    Read<float>(fixture.color)==Read<float>(fresh.color),
                    "Targeted prototype repoint differs from fresh scene geometry/materials");
            fixture.scene->AddPrims({{SdfPath("/B"),HdPrimTypeTokens->mesh,Plane(.3f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/B")); fixture.Render();
            const auto replayed=fixture.bridge->GetStats();
            Require(Stat(replayed,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                    Stat(replayed,"lastPrototypePathsUpdated")==1,
                    "Alternate bank lost preceding prototype edit replay");
            for(int i=0;i<2;++i) {
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/B")); fixture.Render();
            }
            const auto unchanged=fixture.bridge->GetStats();
            Require(Stat(unchanged,"lastPlacementTransformsUpdated")==0 &&
                    Stat(unchanged,"lastVisibilityPlacementsUpdated")==0 &&
                    Stat(unchanged,"lastPrototypePathsUpdated")==0 &&
                    Stat(unchanged,"lastPrototypePlacementsRepointed")==0,
                    "Unchanged edit committed transforms or exposed stale work counters");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Proto"),HdPrimTypeTokens->mesh,InstancedMesh()},
                {SdfPath("/Instancer"),HdPrimTypeTokens->instancer,Instances({0,0,1})}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            PublishedPickHit initial;
            Require(fixture.prim.Capture().frame->pickScene->PickPixel(24,16,&initial) && initial.instanceId==2,
                    "Initial duplicate-index fixture did not identify right instance ordinal");
            fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,Instances({0,1,0})}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Instancer")); fixture.Render();
            const auto reordered=fixture.bridge->GetStats(); PublishedPickHit reorderedHit;
            Require(fixture.prim.Capture().frame->pickScene->PickPixel(24,16,&reorderedHit) && reorderedHit.instanceId==1 &&
                    Stat(reordered,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                    Stat(reordered,"lastPlacementTransformsUpdated")==0 &&
                    Stat(reordered,"placementTopologyFallbacks")==Stat(before,"placementTopologyFallbacks"),
                    "Duplicate source reorder rebuilt stable placements or kept stale picking ordinal");
            fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,Instances({0,1,0},-.4f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Instancer")); fixture.Render();
            const auto moved=fixture.bridge->GetStats();
            Require(Stat(moved,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                    Stat(moved,"prototypeBuilds")==Stat(before,"prototypeBuilds") &&
                    Stat(moved,"lastPlacementTransformsUpdated")==2 &&
                    Stat(moved,"lastPlacementRecordsVisited")>=2,
                    "Instancer primvars touched BLAS or failed to update both duplicate occurrences");
            fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,Instances({0,1})}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Instancer")); fixture.Render();
            const auto count=fixture.bridge->GetStats();
            Require(Stat(count,"instanceCount")==2 &&
                    Stat(count,"placementTopologyFallbacks")>Stat(before,"placementTopologyFallbacks") &&
                    Stat(count,"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                    "Instance count fallback lost instances or rebuilt BLAS");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Proto"),HdPrimTypeTokens->mesh,InstancedMesh()},
                {SdfPath("/Instancer"),HdPrimTypeTokens->instancer,NestedInstances({0,1},false)},
                {SdfPath("/Outer"),HdPrimTypeTokens->instancer,NestedInstances({0,1},true)}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            std::vector<PublishedPickHit> oldHits(Fixture::Size*Fixture::Size);
            std::vector<bool> hadHit(oldHits.size());
            {
                auto frame=fixture.prim.Capture().frame;
                for(unsigned y=0;y<Fixture::Size;++y) for(unsigned x=0;x<Fixture::Size;++x)
                    hadHit[y*Fixture::Size+x]=frame->pickScene->PickPixel(x,y,&oldHits[y*Fixture::Size+x]);
            }
            fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,NestedInstances({1,0},false)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Instancer")); fixture.Render();
            const auto reordered=fixture.bridge->GetStats(); unsigned checked=0;
            {
                auto frame=fixture.prim.Capture().frame;
                for(unsigned y=0;y<Fixture::Size;++y) for(unsigned x=0;x<Fixture::Size;++x) {
                    const auto i=y*Fixture::Size+x; if(!hadHit[i]) continue;
                    PublishedPickHit hit;
                    Require(frame->pickScene->PickPixel(x,y,&hit) && hit.worldPoint==oldHits[i].worldPoint &&
                        hit.instanceId==(oldHits[i].instanceId^1),
                        "Nested source-chain reorder lost geometry or retained stale mixed-radix ordinal");
                    ++checked;
                }
            }
            Require(checked>0 && Stat(reordered,"instanceCount")==4 &&
                Stat(reordered,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Stat(reordered,"lastPlacementTransformsUpdated")==0,
                "Nested stable-chain reorder rebuilt TLAS or transformed unchanged placements");
            fixture.scene->AddPrims({{SdfPath("/Outer"),HdPrimTypeTokens->instancer,NestedInstances({0},true)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Outer")); fixture.Render();
            const auto count=fixture.bridge->GetStats();
            Require(Stat(count,"instanceCount")==2 &&
                Stat(count,"placementTopologyFallbacks")>Stat(before,"placementTopologyFallbacks") &&
                Stat(count,"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                "Nested count fallback lost child placements or rebuilt BLAS");
        }
        std::cout << "Dependency-local prototype and placement tests passed\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
