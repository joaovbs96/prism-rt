#include "integration_fixture.h"
#include "../../src/hydra/task_delegate.h"
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <iostream>
#include <map>

namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;
void Notice(Fixture &fixture,StructuralChange::Operation op,SdfPath path,
            TfToken previous={},TfToken current={},bool children=false) {
    fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure,{path},{},
        {{op,path,previous,current,children}}}});
}
std::map<SdfPath,int> Ids(Fixture &fixture) {
    std::map<SdfPath,int> ids;
    auto frame=fixture.prim.Capture().frame;
    for(size_t i=0;i<frame->picking->primPaths.size();++i)
        if(!frame->picking->primPaths[i].IsEmpty()) ids[frame->picking->primPaths[i]]=static_cast<int>(i);
    return ids;
}
HdContainerDataSourceHandle Hidden() {
    auto visibility=HdVisibilitySchema::Builder().SetVisibility(
        HdRetainedTypedSampledDataSource<bool>::New(false)).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdVisibilitySchemaTokens->visibility,visibility),Plane());
}
HdContainerDataSourceHandle InvalidMesh() {
    auto topology=HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New({}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New({})).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdMeshSchemaTokens->mesh,HdMeshSchema::Builder().SetTopology(topology).Build()),Plane());
}
HdContainerDataSourceHandle EmptyInstancedMesh() {
    auto by=HdInstancedBySchema::Builder().SetPaths(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({SdfPath("/Instancer")})).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdInstancedBySchemaTokens->instancedBy,by),Plane());
}
HdContainerDataSourceHandle SphereData() {
    return HdRetainedContainerDataSource::New(HdSphereSchemaTokens->sphere,
        HdSphereSchema::Builder().SetRadius(HdRetainedTypedSampledDataSource<double>::New(.4)).Build());
}
HdContainerDataSourceHandle NativeTopology(VtArray<SdfPath> roots) {
    auto topology=HdInstancerTopologySchema::Builder().SetPrototypes(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(roots)).Build();
    return HdRetainedContainerDataSource::New(HdInstancerTopologySchemaTokens->instancerTopology,topology);
}
HdContainerDataSourceHandle LargeMesh() {
    constexpr int side=400;
    VtVec3fArray points; VtIntArray counts,indices;
    points.reserve((side+1)*(side+1)); counts.reserve(side*side); indices.reserve(side*side*4);
    for(int y=0;y<=side;++y) for(int x=0;x<=side;++x)
        points.push_back(GfVec3f(-.6f+1.2f*x/side,-.6f+1.2f*y/side,0));
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        const int p=y*(side+1)+x; counts.push_back(4);
        for(int index : {p,p+1,p+side+2,p+side+1}) indices.push_back(index);
    }
    auto topology=HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(counts))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)).Build();
    auto pv=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex)).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdMeshSchemaTokens->mesh,HdMeshSchema::Builder().SetTopology(topology).Build(),
        HdPrimvarsSchemaTokens->primvars,HdRetainedContainerDataSource::New(TfToken("points"),pv)),Plane());
}
}

int RunStructuralUpdateTests() {
    try {
        for(int size : {16,128,1024}) {
            Fixture fixture;
            HdRetainedSceneIndex::AddedPrimEntries entries;
            for(int i=0;i<size;++i) entries.push_back({SdfPath("/P"+std::to_string(i)),
                HdPrimTypeTokens->mesh,Plane(i ? 100.f+i : -.5f)});
            entries.push_back({SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()});
            fixture.scene->AddPrims(entries); fixture.Render();
            const auto before=fixture.bridge->GetStats(); const auto original=Ids(fixture);
            fixture.scene->AddPrims({{SdfPath("/X"),HdPrimTypeTokens->mesh,Plane(.5f)}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/X"),{},HdPrimTypeTokens->mesh);
            fixture.Render(); const int x=Ids(fixture).at(SdfPath("/X"));
            const auto added=fixture.bridge->GetStats();
            Require(Stat(added,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Stat(added,"lastStructuralPathsUpdated")==1 && Stat(added,"lastStructuralPlacementsAttached")==1 &&
                Stat(added,"lastStructuralPlacementsDetached")==0 && Stat(added,"capturedPrims")<=1 &&
                Stat(added,"prototypeBuilds")-Stat(before,"prototypeBuilds")<=1 &&
                Stat(added,"materialConversionCalls")==Stat(before,"materialConversionCalls"),
                "Eligible leaf add did unrelated capture, geometry work or TLAS allocation");
            fixture.scene->RemovePrims({SdfPath("/X")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/X"),HdPrimTypeTokens->mesh);
            fixture.Render();
            fixture.scene->AddPrims({{SdfPath("/Y"),HdPrimTypeTokens->mesh,Plane(.5f)}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Y"),{},HdPrimTypeTokens->mesh);
            fixture.Render(); const int y=Ids(fixture).at(SdfPath("/Y"));
            Require(y!=x && Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                "Lagging bank recycled a removed canonical slot or allocated TLAS");
            for(int i=0;i<3;++i) {
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/P0")); fixture.Render();
                auto ids=Ids(fixture); Require(ids.at(SdfPath("/Y"))==y,"Canonical new prim ID differs across banks");
                for(auto const &[path,id] : original) Require(ids.at(path)==id,"Unaffected prim ID changed after leaf replay");
            }
            fixture.scene->AddPrims({{SdfPath("/X"),HdPrimTypeTokens->mesh,Hidden()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/X"),{},HdPrimTypeTokens->mesh); fixture.Render();
            Require(Ids(fixture).at(SdfPath("/X"))==x,"Re-added path did not revive its canonical slot");
            Fixture fresh;
            entries.push_back({SdfPath("/Y"),HdPrimTypeTokens->mesh,Plane(.5f)});
            entries.push_back({SdfPath("/X"),HdPrimTypeTokens->mesh,Hidden()});
            fresh.scene->AddPrims(entries); fresh.Render();
            Require(Read<float>(fixture.color)==Read<float>(fresh.color),"Incremental structural scene differs from fresh radiance");
            const auto final=fixture.bridge->GetStats();
            std::cout << "Structural scale "<<size<<": captured="<<Stat(added,"capturedPrims")
                <<" affected="<<Stat(added,"lastStructuralPathsUpdated")
                <<" attached="<<Stat(added,"lastStructuralPlacementsAttached")
                <<" TLAS allocations="<<Stat(final,"topLevelSceneBuilds")-Stat(before,"topLevelSceneBuilds")<<'\n';
        }
        {
            Fixture fixture; fixture.Render(); const auto before=fixture.bridge->GetStats();
            fixture.scene->AddPrims({{SdfPath("/Transient"),HdPrimTypeTokens->mesh,Plane()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Transient"),{},HdPrimTypeTokens->mesh);
            fixture.scene->RemovePrims({SdfPath("/Transient")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Transient"),HdPrimTypeTokens->mesh);
            fixture.Render();
            PublishedPickHit absent;
            Require(Stat(fixture.bridge->GetStats(),"primCount")==0 &&
                Stat(fixture.bridge->GetStats(),"primPathSlotCount")==1 &&
                !fixture.prim.Capture().frame->pickScene->PickPixel(16,16,&absent),
                "Net-absent add/remove lost tombstone reservation or left selectable geometry");
            fixture.scene->AddPrims({{SdfPath("/Y"),HdPrimTypeTokens->mesh,Plane()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Y"),{},HdPrimTypeTokens->mesh); fixture.Render();
            Require(Ids(fixture).at(SdfPath("/Y"))==1 &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                "Empty-scene bank replay reused reserved tombstone or rebuilt TLAS");
            fixture.scene->AddPrims({{SdfPath("/Y"),HdPrimTypeTokens->mesh,InvalidMesh()}});
            Notice(fixture,StructuralChange::Operation::Replace,SdfPath("/Y"),HdPrimTypeTokens->mesh,HdPrimTypeTokens->mesh);
            fixture.Render(); Require(Stat(fixture.bridge->GetStats(),"primCount")==0,"Invalid replacement retained old geometry");
            fixture.scene->AddPrims({{SdfPath("/Y"),HdPrimTypeTokens->mesh,Plane()}});
            Notice(fixture,StructuralChange::Operation::Replace,SdfPath("/Y"),HdPrimTypeTokens->mesh,HdPrimTypeTokens->mesh);
            fixture.Render(); Require(Ids(fixture).at(SdfPath("/Y"))==1,"Valid geometry recovery lost canonical ID");
            const auto fallbackBefore=fixture.bridge->GetStats();
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Unknown")); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(fallbackBefore,"structuralFallbacks") &&
                Ids(fixture).at(SdfPath("/Y"))==1,"Unknown-prior removal bypassed fallback or full build changed canonical ID");
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Y")); fixture.Render();
            Require(Ids(fixture).at(SdfPath("/Y"))==1,"Spare full build changed canonical ID");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Proto"),HdPrimTypeTokens->mesh,EmptyInstancedMesh()},
                {SdfPath("/Instancer"),HdPrimTypeTokens->instancer,HdRetainedContainerDataSource::New()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            fixture.scene->RemovePrims({SdfPath("/Proto")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Proto"),HdPrimTypeTokens->mesh); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(before,"structuralFallbacks") &&
                Stat(fixture.bridge->GetStats(),"meshCount")==0,
                "Removed zero-placement instanced prototype bypassed conservative fallback");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Root"),TfToken("scope"),HdRetainedContainerDataSource::New()},
                {SdfPath("/Root/Leaf"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Instancer"),HdPrimTypeTokens->instancer,NativeTopology({})}});
            fixture.Render();
            fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,NativeTopology({SdfPath("/Root")})}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Instancer")); fixture.Render();
            const auto before=fixture.bridge->GetStats();
            fixture.scene->RemovePrims({SdfPath("/Root/Leaf")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Root/Leaf"),HdPrimTypeTokens->mesh); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(before,"structuralFallbacks"),
                "Native-root membership introduced by targeted instancer topology was not retained for structural eligibility");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Leaf/Subset"),HdPrimTypeTokens->geomSubset,HdRetainedContainerDataSource::New()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            fixture.scene->RemovePrims({SdfPath("/Leaf")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Leaf"),HdPrimTypeTokens->mesh); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(before,"structuralFallbacks"),
                "Mesh with subset children bypassed ancestor/subset fallback");
            fixture.scene->AddPrims({{SdfPath("/Renamed"),HdPrimTypeTokens->mesh,Plane()}});
            Notice(fixture,StructuralChange::Operation::Rename,SdfPath("/Renamed"),{},HdPrimTypeTokens->mesh); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(before,"structuralFallbacks")+1,
                "Typed rename bypassed conservative fallback");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()}});
            fixture.Render(); const int id=Ids(fixture).at(SdfPath("/Leaf"));
            const auto before=fixture.bridge->GetStats();
            fixture.scene->RemovePrims({SdfPath("/Leaf")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Leaf"),HdPrimTypeTokens->mesh);
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->sphere,SphereData()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Leaf"),{},HdPrimTypeTokens->sphere);
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Leaf")); fixture.Render();
            Require(Ids(fixture).at(SdfPath("/Leaf"))==id &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneCommits")==Stat(before,"topLevelSceneCommits")+1 &&
                Stat(fixture.bridge->GetStats(),"meshCount")==0,
                "Remove/add type change or redundant placement rebuilt TLAS, duplicated commit or changed ID");
            auto held=fixture.prim.Capture().frame; PublishedPickHit oldHit;
            Require(held->pickScene->PickPixel(16,16,&oldHit),"Held structural sphere was not hittable");
            fixture.scene->RemovePrims({SdfPath("/Leaf")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Leaf"),HdPrimTypeTokens->sphere); fixture.Render();
            fixture.scene->AddPrims({{SdfPath("/New"),HdPrimTypeTokens->mesh,Plane()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/New"),{},HdPrimTypeTokens->mesh); fixture.Render();
            PublishedPickHit hit;
            Require(held->pickScene->PickPixel(16,16,&hit) && hit.worldPoint==oldHit.worldPoint && hit.primId==id,
                "Held frame changed across structural deletion and pinned-bank fallback");
            held.reset();
        }
        {
            Fixture fixture; auto settings=fixture.bridge->GetSettings(); settings.sphereBatching=true;
            fixture.bridge->SetSettings(settings); fixture.Render(); const auto before=fixture.bridge->GetStats();
            fixture.scene->AddPrims({{SdfPath("/HiddenSphere"),HdPrimTypeTokens->sphere,
                HdOverlayContainerDataSource::New(Hidden(),SphereData())}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/HiddenSphere"),{},HdPrimTypeTokens->sphere); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralLeafUpdates")>Stat(before,"structuralLeafUpdates") &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                "Provably unbatched hidden sphere unnecessarily rebuilt TLAS");
            fixture.scene->RemovePrims({SdfPath("/HiddenSphere")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/HiddenSphere"),HdPrimTypeTokens->sphere); fixture.Render();
            fixture.scene->AddPrims({{SdfPath("/Sphere"),HdPrimTypeTokens->sphere,SphereData()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Sphere"),{},HdPrimTypeTokens->sphere); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(before,"structuralFallbacks"),
                "Batched sphere structural edit bypassed named fallback");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()}}); fixture.Render();
            const auto before=fixture.bridge->GetStats(); const int id=Ids(fixture).at(SdfPath("/Leaf"));
            auto large=LargeMesh();
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,large}});
            Notice(fixture,StructuralChange::Operation::Replace,SdfPath("/Leaf"),HdPrimTypeTokens->mesh,HdPrimTypeTokens->mesh);
            fixture.bridge->Prepare(fixture.camera,fixture.bindings,{HdRenderTagTokens->geometry},Fixture::Size,Fixture::Size);
            Wait([&] { return Stat(fixture.bridge->GetStats(),"structuralPlacementDetachCalls")>
                Stat(before,"structuralPlacementDetachCalls"); },"Cancellation fixture did not reach structural detach");
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Leaf")); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"discardedSnapshotBuilds")>Stat(before,"discardedSnapshotBuilds") &&
                Stat(fixture.bridge->GetStats(),"triangleCount")==320000 && Ids(fixture).at(SdfPath("/Leaf"))==id,
                "Canceled mutated bank was republished or lost final geometry/canonical ID");
            Fixture fresh; fresh.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,large}}); fresh.Render();
            Require(Read<float>(fixture.color)==Read<float>(fresh.color),"Canceled structural replay differs from fresh final scene");
        }
        {
            auto scene=HdRetainedSceneIndex::New();
            scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()}});
            PrismTaskDelegate delegate; delegate.SetTerminalSceneIndex(scene);
            const auto before=delegate.GetRenderStats();
            scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->camera,HdRetainedContainerDataSource::New()}});
            Require(Stat(delegate.GetRenderStats(),"preparationGeneration")>Stat(before,"preparationGeneration"),
                "Observer ignored old geometry replaced by unsupported new type");
        }
        std::cout << "Targeted structural leaf tests passed\n"; return 0;
    } catch(std::exception const &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
