#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/utils.h>
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
HdContainerDataSourceHandle EmptyInstancedMesh(SdfPath instancer=SdfPath("/Instancer")) {
    auto by=HdInstancedBySchema::Builder().SetPaths(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({instancer})).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdInstancedBySchemaTokens->instancedBy,by),Plane());
}
HdContainerDataSourceHandle StructuralInstances(int count, SdfPath prototype=SdfPath("/Proto"), bool duplicates=false) {
    VtIntArray indices; VtVec3fArray positions;
    for (int i=0; i<count; ++i) {
        indices.push_back(duplicates ? i/2 : i); positions.push_back(GfVec3f(i ? 10.f+i : 0.f,0,0));
    }
    HdDataSourceBaseHandle arrays[]={HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)};
    auto topology=HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({prototype}))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(1,arrays)).Build();
    auto translations=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(positions))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->instance)).Build();
    return HdRetainedContainerDataSource::New(HdInstancerTopologySchemaTokens->instancerTopology,topology,
        HdPrimvarsSchemaTokens->primvars,HdRetainedContainerDataSource::New(
            HdInstancerTokens->instanceTranslations,translations));
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
HdContainerDataSourceHandle RedMaterial() {
    HdMaterialNode preview;
    preview.path=SdfPath("/Red/Preview"); preview.identifier=TfToken("UsdPreviewSurface");
    preview.parameters[TfToken("diffuseColor")]=VtValue(GfVec3f(.8f,.05f,.05f));
    HdMaterialNetworkMap networkMap; networkMap.map[TfToken("surface")].nodes={preview};
    HdDataSourceBaseHandle network=HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(networkMap);
    TfToken context=HdMaterialSchemaTokens->universalRenderContext;
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,
        HdMaterialSchema::BuildRetained(1,&context,&network));
}
HdContainerDataSourceHandle RedSubset() {
    HdDataSourceBaseHandle binding=HdMaterialBindingSchema::Builder()
        .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(SdfPath("/Red"))).Build();
    TfToken purpose=HdMaterialBindingsSchemaTokens->allPurpose;
    return HdRetainedContainerDataSource::New(HdGeomSubsetSchemaTokens->geomSubset,
        HdGeomSubsetSchema::Builder().SetType(HdGeomSubsetSchema::BuildTypeDataSource(
            HdGeomSubsetSchemaTokens->typeFaceSet))
            .SetIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New({0})).Build(),
        HdMaterialBindingsSchemaTokens->materialBindings,
        HdMaterialBindingsSchema::BuildRetained(1,&purpose,&binding));
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
        for (int count : {16,128,1024,16384}) {
            Fixture fixture;
            auto addGroup=[&](Fixture &target,SdfPath const &root) {
                const auto instancer=root.AppendChild(TfToken("Instancer"));
                const auto prototype=root.AppendChild(TfToken("Proto"));
                const auto outer=root.AppendChild(TfToken("Outer"));
                auto by=HdInstancedBySchema::Builder().SetPaths(
                    HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({outer})).Build();
                auto inner=HdOverlayContainerDataSource::New(
                    HdRetainedContainerDataSource::New(HdInstancedBySchemaTokens->instancedBy,by),
                    StructuralInstances(count/2,prototype,true));
                target.scene->AddPrims({{root,TfToken("xform"),Plane(.1f)},
                    {outer,HdPrimTypeTokens->instancer,StructuralInstances(2,instancer)},
                    {instancer,HdPrimTypeTokens->instancer,inner},
                    {prototype,HdPrimTypeTokens->mesh,EmptyInstancedMesh(instancer)}});
            };
            fixture.scene->AddPrims({{SdfPath("/Stable"),HdPrimTypeTokens->mesh,Plane(20)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            addGroup(fixture,SdfPath("/Before")); fixture.Render();
            const auto before=fixture.bridge->GetStats(); const auto ids=Ids(fixture);
            const auto image=Read<float>(fixture.color);
            auto held=fixture.prim.Capture().frame;
            fixture.scene->RemovePrims({SdfPath("/Before")}); addGroup(fixture,SdfPath("/After"));
            fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure,
                {SdfPath("/Before"),SdfPath("/After")},{},
                {{StructuralChange::Operation::Rename,SdfPath("/Before"),TfToken("xform"),
                    TfToken("xform"),true,SdfPath("/After")}}}});
            fixture.Render(); const auto renamed=fixture.bridge->GetStats(); const auto newIds=Ids(fixture);
            Require(Stat(renamed,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Stat(renamed,"prototypeBuilds")==Stat(before,"prototypeBuilds") &&
                Stat(renamed,"lastPlacementRecordsVisited")==count &&
                Stat(renamed,"lastPlacementTopologyAttached")==0 &&
                Stat(renamed,"lastPlacementTopologyDetached")==0 &&
                Stat(renamed,"lastStructuralPlacementsAttached")==0 &&
                Stat(renamed,"lastStructuralPlacementsDetached")==0 &&
                Read<float>(fixture.color)==image,
                "Subtree rename replaced compatible geometry/placements or changed pixels");
            Require(newIds.at(SdfPath("/Stable"))==ids.at(SdfPath("/Stable")) &&
                newIds.at(SdfPath("/After/Proto"))!=ids.at(SdfPath("/Before/Proto")) &&
                !newIds.contains(SdfPath("/Before/Proto")),
                "Rename failed destination-path ID or tombstone semantics");
            PublishedPickHit oldHit,newHit;
            Require(held->pickScene->PickPixel(16,16,&oldHit) &&
                held->picking->primPaths[oldHit.primId]==SdfPath("/Before/Proto") &&
                fixture.prim.Capture().frame->pickScene->PickPixel(16,16,&newHit) &&
                newHit.primId==newIds.at(SdfPath("/After/Proto")),
                "Rename changed frame-owned paths or lost destination picking");
            Fixture fresh;
            fresh.scene->AddPrims({{SdfPath("/Stable"),HdPrimTypeTokens->mesh,Plane(20)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            addGroup(fresh,SdfPath("/After")); fresh.Render();
            Require(Read<float>(fixture.color)==Read<float>(fresh.color),
                "Rename differs from fresh destination scene");
            held.reset();
            fixture.scene->RemovePrims({SdfPath("/After")}); addGroup(fixture,SdfPath("/Before"));
            fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure,
                {SdfPath("/After"),SdfPath("/Before")},{},
                {{StructuralChange::Operation::Rename,SdfPath("/After"),TfToken("xform"),
                    TfToken("xform"),true,SdfPath("/Before")}}}});
            fixture.Render();
            Require(Ids(fixture)==ids && Read<float>(fixture.color)==image,
                "Rename bank replay did not restore destination-path identities");
        }

        for (int count : {16,128,1024,16384}) {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Stable"),HdPrimTypeTokens->mesh,Plane(20)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            const int stable=Ids(fixture).at(SdfPath("/Stable"));
            auto addGroup=[&](Fixture &target) {
                target.scene->AddPrims({{SdfPath("/Group"),TfToken("xform"),Plane(.1f)},
                    {SdfPath("/Group/Instancer"),HdPrimTypeTokens->instancer,
                        StructuralInstances(count,SdfPath("/Group/Proto"))},
                    {SdfPath("/Group/Proto"),HdPrimTypeTokens->mesh,
                        EmptyInstancedMesh(SdfPath("/Group/Instancer"))}});
            };
            addGroup(fixture);
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Group"),{},TfToken("xform"));
            fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"lastStructuralPlacementsAttached")==count &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Ids(fixture).at(SdfPath("/Stable"))==stable,
                "Subtree addition missed descendants or rebuilt unrelated geometry");
            Fixture fresh;
            fresh.scene->AddPrims({{SdfPath("/Stable"),HdPrimTypeTokens->mesh,Plane(20)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            addGroup(fresh); fresh.Render();
            Require(Read<float>(fixture.color)==Read<float>(fresh.color),
                "Subtree inherited transforms differ from fresh rebuild");
            // Reconcile the other bank before asserting physical detach work.
            // An add immediately followed by removal can correctly be a no-op
            // on the older bank, which never contained the added subtree.
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Stable"));
            fixture.Render();
            auto held=fixture.prim.Capture().frame; PublishedPickHit oldHit;
            Require(held->pickScene->PickPixel(16,16,&oldHit),"Subtree instance not pickable");
            fixture.scene->RemovePrims({SdfPath("/Group")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Group"),TfToken("xform"),{},true);
            fixture.Render(); PublishedPickHit retained;
            Require(Stat(fixture.bridge->GetStats(),"lastStructuralPlacementsDetached")==count &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Ids(fixture).size()==1 && Ids(fixture).at(SdfPath("/Stable"))==stable &&
                held->pickScene->PickPixel(16,16,&retained) && retained.primId==oldHit.primId,
                "Subtree removal missed retained descendants or corrupted old picking");
            held.reset();
            addGroup(fixture);
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Group"),{},TfToken("xform"));
            fixture.Render();
            Require(Read<float>(fixture.color)==Read<float>(fresh.color),
                "Subtree bank replay retained removed data or changed re-added geometry");
        }

        for (int count : {16,128,1024,16384}) {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Stable"),HdPrimTypeTokens->mesh,Plane(20)},
                {SdfPath("/Instancer"),HdPrimTypeTokens->instancer,StructuralInstances(count)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()},
                {SdfPath("/Red"),HdPrimTypeTokens->material,RedMaterial()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            const int stable=Ids(fixture).at(SdfPath("/Stable"));
            fixture.scene->AddPrims({{SdfPath("/Proto"),HdPrimTypeTokens->mesh,EmptyInstancedMesh()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Proto"),{},HdPrimTypeTokens->mesh);
            fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"lastStructuralPlacementsAttached")==count &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Ids(fixture).at(SdfPath("/Stable"))==stable,
                "Instanced prototype addition rebuilt TLAS or missed dependent placements");
            const auto original=Read<float>(fixture.color);
            Fixture fresh;
            fresh.scene->AddPrims({{SdfPath("/Stable"),HdPrimTypeTokens->mesh,Plane(20)},
                {SdfPath("/Instancer"),HdPrimTypeTokens->instancer,StructuralInstances(count)},
                {SdfPath("/Proto"),HdPrimTypeTokens->mesh,EmptyInstancedMesh()},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fresh.Render();
            Require(Read<float>(fresh.color)==original,"Instanced prototype addition differs from fresh build");
            fixture.scene->AddPrims({{SdfPath("/Proto/Subset"),HdPrimTypeTokens->geomSubset,RedSubset()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Proto/Subset"),{},HdPrimTypeTokens->geomSubset);
            fixture.Render();
            Require(Read<float>(fixture.color)!=original &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                "Instanced subset addition failed to refresh dependent bindings");
            fixture.scene->RemovePrims({SdfPath("/Proto/Subset")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Proto/Subset"),HdPrimTypeTokens->geomSubset);
            fixture.Render();
            Require(Read<float>(fixture.color)==original,"Instanced subset removal left stale bindings");
            auto held=fixture.prim.Capture().frame; PublishedPickHit oldHit;
            Require(held->pickScene->PickPixel(16,16,&oldHit),"Instanced prototype was not pickable");
            fixture.scene->RemovePrims({SdfPath("/Proto")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Proto"),HdPrimTypeTokens->mesh);
            fixture.Render(); PublishedPickHit retained;
            Require(Stat(fixture.bridge->GetStats(),"lastStructuralPlacementsDetached")==count &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                held->pickScene->PickPixel(16,16,&retained) && retained.primId==oldHit.primId &&
                !Ids(fixture).contains(SdfPath("/Proto")),
                "Instanced prototype removal lost old-frame picking or left placements");
        }

        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Red"),HdPrimTypeTokens->material,RedMaterial()},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            auto const original=Read<float>(fixture.color); auto const ids=Read<int>(fixture.prim);
            fixture.scene->AddPrims({{SdfPath("/Leaf/Subset"),HdPrimTypeTokens->geomSubset,RedSubset()}});
            Notice(fixture,StructuralChange::Operation::Add,SdfPath("/Leaf/Subset"),{},HdPrimTypeTokens->geomSubset);
            fixture.Render(); auto const added=fixture.bridge->GetStats();
            Require(Stat(added,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                    Stat(added,"prototypeBuilds")==Stat(before,"prototypeBuilds") &&
                    Stat(added,"structuralFallbacks")==Stat(before,"structuralFallbacks") &&
                    Stat(added,"lastStructuralPathsUpdated")==1 && Read<int>(fixture.prim)==ids &&
                    Read<float>(fixture.color)!=original,
                    "Subset addition rebuilt geometry or did not update its parent binding");
            Fixture fresh;
            fresh.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Leaf/Subset"),HdPrimTypeTokens->geomSubset,RedSubset()},
                {SdfPath("/Red"),HdPrimTypeTokens->material,RedMaterial()},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fresh.Render();
            Require(Read<float>(fixture.color)==Read<float>(fresh.color),
                    "Subset addition differs from a fresh parent material binding");
            fixture.scene->RemovePrims({SdfPath("/Leaf/Subset")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Leaf/Subset"),HdPrimTypeTokens->geomSubset);
            fixture.Render();
            Require(Read<float>(fixture.color)==original && Read<int>(fixture.prim)==ids &&
                    Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                    "Subset removal left its old material or rebuilt the top-level scene");
        }
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
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")==Stat(before,"structuralFallbacks") &&
                Stat(fixture.bridge->GetStats(),"meshCount")==0 &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                "Removed zero-placement instanced prototype rebuilt the scene or retained geometry");
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
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")==Stat(before,"structuralFallbacks") &&
                Stat(fixture.bridge->GetStats(),"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds"),
                "Native-prototype leaf removal rebuilt unrelated geometry");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Leaf"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Leaf/Subset"),HdPrimTypeTokens->geomSubset,HdRetainedContainerDataSource::New()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            fixture.scene->RemovePrims({SdfPath("/Leaf")});
            Notice(fixture,StructuralChange::Operation::Remove,SdfPath("/Leaf"),HdPrimTypeTokens->mesh); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")==Stat(before,"structuralFallbacks") &&
                Stat(fixture.bridge->GetStats(),"primCount")==0,
                "Mesh removal with subset children rebuilt or left geometry");
            fixture.scene->AddPrims({{SdfPath("/Renamed"),HdPrimTypeTokens->mesh,Plane()}});
            Notice(fixture,StructuralChange::Operation::Rename,SdfPath("/Renamed"),{},HdPrimTypeTokens->mesh); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"structuralFallbacks")>Stat(before,"structuralFallbacks"),
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
