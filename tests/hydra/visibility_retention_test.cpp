#include "integration_fixture.h"
#include "../../src/hydra/task_delegate.h"
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/purposeSchema.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <iostream>

namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;
HdContainerDataSourceHandle Visible(bool visible) {
    return HdRetainedContainerDataSource::New(HdVisibilitySchemaTokens->visibility,
        HdVisibilitySchema::Builder().SetVisibility(
            HdRetainedTypedSampledDataSource<bool>::New(visible)).Build());
}
HdContainerDataSourceHandle Purpose(TfToken purpose) {
    return HdRetainedContainerDataSource::New(HdPurposeSchemaTokens->purpose,
        HdPurposeSchema::Builder().SetPurpose(
            HdRetainedTypedSampledDataSource<TfToken>::New(purpose)).Build());
}
HdContainerDataSourceHandle RetainedSphere(bool visible) {
    return HdOverlayContainerDataSource::New(Visible(visible),
        HdRetainedContainerDataSource::New(HdSphereSchemaTokens->sphere,
            HdSphereSchema::Builder().SetRadius(
                HdRetainedTypedSampledDataSource<double>::New(.4)).Build()));
}
HdContainerDataSourceHandle SmallRedMesh(float redValue=.7f) {
    auto points=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            VtVec3fArray{{-.2f,-.2f,0},{.2f,-.2f,0},{.2f,.2f,0},{-.2f,.2f,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex)).Build();
    auto red=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(VtVec3fArray{GfVec3f(redValue,.1f,.1f)}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant)).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->primvars,HdRetainedContainerDataSource::New(
            TfToken("points"),points,TfToken("displayColor"),red)),Plane());
}
// A deterministic grid makes retention pressure real while keeping rendered
// validation small. No synthetic memory estimate or allocator hook is needed.
HdContainerDataSourceHandle GridMesh(int cells=200, float offset=0) {
    VtVec3fArray vertices;
    VtIntArray counts, indices;
    vertices.reserve((cells+1)*(cells+1));
    counts.reserve(cells*cells); indices.reserve(cells*cells*4);
    for(int y=0;y<=cells;++y) for(int x=0;x<=cells;++x)
        vertices.emplace_back(-.6f+1.2f*x/cells+offset,-.6f+1.2f*y/cells,0);
    for(int y=0;y<cells;++y) for(int x=0;x<cells;++x) {
        int a=y*(cells+1)+x;
        counts.push_back(4);
        for(int id:{a,a+1,a+cells+2,a+cells+1}) indices.push_back(id);
    }
    auto points=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(vertices))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex)).Build();
    auto topology=HdMeshTopologySchema::Builder().SetFaceVertexCounts(
        HdRetainedTypedSampledDataSource<VtIntArray>::New(counts))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)).Build();
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->primvars,HdRetainedContainerDataSource::New(TfToken("points"),points),
        HdMeshSchemaTokens->mesh,HdMeshSchema::Builder().SetTopology(topology).Build()),Plane());
}
HdContainerDataSourceHandle InstancedBy(SdfPath instancer,SdfPath root) {
    return HdRetainedContainerDataSource::New(HdInstancedBySchemaTokens->instancedBy,
        HdInstancedBySchema::Builder().SetPaths(
            HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(VtArray<SdfPath>{instancer}))
            .SetPrototypeRoots(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                VtArray<SdfPath>{root})).Build());
}
HdContainerDataSourceHandle Instancer(SdfPath prototype,bool visible,bool empty=false) {
    HdDataSourceBaseHandle indices[]={HdRetainedTypedSampledDataSource<VtIntArray>::New(
        empty?VtIntArray{}:VtIntArray{0})};
    return HdOverlayContainerDataSource::New(Visible(visible),
        HdRetainedContainerDataSource::New(HdInstancerTopologySchemaTokens->instancerTopology,
            HdInstancerTopologySchema::Builder().SetPrototypes(
                HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(VtArray<SdfPath>{prototype}))
                .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(1,indices)).Build()));
}
size_t Hits(Fixture const& fixture) {
    const auto ids=Read<int>(fixture.prim);
    return std::count_if(ids.begin(),ids.end(),[](int id){return id>=0;});
}
void RenderTags(Fixture& fixture,TfTokenVector const& tags) {
    Wait([&]{fixture.bridge->Prepare(fixture.camera,fixture.bindings,tags,Fixture::Size,Fixture::Size);
        const auto stats=fixture.bridge->GetStats(); auto pending=stats.find("preparationPending");
        return pending!=stats.end() && pending->second.IsHolding<bool>() &&
            !pending->second.UncheckedGet<bool>() &&
            Stat(stats,"displayedGeneration")>=Stat(stats,"preparationGeneration");},"Render-tag prepare timeout");
    fixture.thread.StartRender();
    Wait([&]{return fixture.color.IsConverged() && fixture.bridge->IsConverged();},"Render-tag frame timeout");
    fixture.thread.StopRender();
}
}
int RunVisibilityRetentionTests() {
    try {
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            fixture.bridge->SetSettings(settings);
            auto mesh=GridMesh();
            fixture.scene->AddPrims({{SdfPath("/Hidden"),TfToken("xform"),Visible(false)},
                {SdfPath("/Hidden/Mesh"),HdPrimTypeTokens->mesh,mesh},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            Require(Stat(before,"evictedPrototypeCount")==1,"Rename fixture did not evict hidden geometry");
            fixture.scene->RemovePrims({SdfPath("/Hidden")});
            fixture.scene->AddPrims({{SdfPath("/Cold"),TfToken("xform"),Visible(false)},
                {SdfPath("/Cold/Mesh"),HdPrimTypeTokens->mesh,mesh}});
            fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure,
                {SdfPath("/Hidden"),SdfPath("/Cold")},{},
                {{StructuralChange::Operation::Rename,SdfPath("/Hidden"),TfToken("xform"),
                    TfToken("xform"),true,SdfPath("/Cold")}}}});
            fixture.Render(); const auto renamed=fixture.bridge->GetStats();
            Require(Stat(renamed,"evictedPrototypeCount")==1 && Stat(renamed,"meshCount")==0 &&
                Stat(renamed,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                Stat(renamed,"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                "Renaming an evicted subtree eagerly restored geometry or rebuilt the scene");
            fixture.scene->AddPrims({{SdfPath("/Cold"),TfToken("xform"),Visible(true)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Cold")); fixture.Render();
            Require(Hits(fixture)>0 && Stat(fixture.bridge->GetStats(),"evictedPrototypeCount")==0,
                "Renamed cold prototype failed to restore or retained a ghost old path");
            fixture.scene->RemovePrims({SdfPath("/Cold")});
            fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure,{SdfPath("/Cold")},{},
                {{StructuralChange::Operation::Remove,SdfPath("/Cold"),TfToken("xform"),{},true}}}});
            fixture.Render();
            Require(Hits(fixture)==0 && Stat(fixture.bridge->GetStats(),"primCount")==0,
                "Removing a restored renamed subtree left selectable geometry");
        }

        {
            auto scene=HdRetainedSceneIndex::New();
            scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}});
            PrismTaskDelegate delegate; delegate.SetTerminalSceneIndex(scene);
            HdSceneIndexObserver::DirtiedPrimEntries entries;
            entries.emplace_back(SdfPath("/Mesh"),HdDataSourceLocatorSet(
                HdPurposeSchema::GetDefaultLocator()));
            delegate.PrimsDirtied(*scene,entries);
            Require(Stat(delegate.GetRenderStats(),"placementInvalidations")==1,
                    "Purpose scene-index notice did not route to placement eligibility");
        }
        for(bool initiallyVisible:{false,true}) {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.sphereBatching=true;
            fixture.bridge->SetSettings(settings);
            fixture.scene->AddPrims({{SdfPath("/Sphere"),HdPrimTypeTokens->sphere,
                                     RetainedSphere(initiallyVisible)}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            Require((Hits(fixture)>0)==initiallyVisible,"Initial sphere eligibility incorrect");
            Require(Stat(before,"sphereBatchEntryCount")==static_cast<int64_t>(initiallyVisible),
                    "Initially hidden sphere entered a visible batch");
            bool namedFallback=false;
            for(bool visible:{!initiallyVisible,initiallyVisible}) {
                fixture.scene->AddPrims({{SdfPath("/Sphere"),HdPrimTypeTokens->sphere,RetainedSphere(visible)}});
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Sphere")); fixture.Render();
                Require((Hits(fixture)>0)==visible,"Sphere eligibility toggle rendered incorrectly");
                if(initiallyVisible && !visible) {
                    const auto stats=fixture.bridge->GetStats();
                    auto reason=stats.find("lastRebuildReason");
                    namedFallback=Stat(stats,"sphereBatchConservativeFallbacks")>Stat(before,"sphereBatchConservativeFallbacks") &&
                        reason!=stats.end() && reason->second.IsHolding<std::string>() &&
                        reason->second.UncheckedGet<std::string>()=="sphere batch eligibility or topology changed";

                }
            }
            const auto after=fixture.bridge->GetStats();
            if(!initiallyVisible)
                Require(Stat(after,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                        Stat(after,"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                        "Initially hidden ordinary sphere rebuilt on visibility toggles");
            else {
                Require(namedFallback,
                        "Batched visibility change did not report named membership fallback");
            }
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({
                {SdfPath("/A"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/B"),HdPrimTypeTokens->mesh,Plane(1)}});
            fixture.Render();
            fixture.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,SmallRedMesh()}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/A")); fixture.Render();
            const auto ids=Read<int>(fixture.prim);
            Require(std::find(ids.begin(),ids.end(),0)!=ids.end() &&
                    std::find(ids.begin(),ids.end(),1)!=ids.end(),
                    "Refreshing one prototype removed an unchanged placement from the TLAS");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Hidden"),TfToken("xform"),Visible(false)},
                {SdfPath("/Hidden/Mesh"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render();
            Require(Hits(fixture)==0 && Stat(fixture.bridge->GetStats(),"meshCount")==1,
                    "Initially hidden geometry storage was dropped");
            fixture.scene->AddPrims({{SdfPath("/Hidden/Mesh"),HdPrimTypeTokens->mesh,SmallRedMesh()}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Hidden/Mesh")); fixture.Render();
            Require(Hits(fixture)==0 && Stat(fixture.bridge->GetStats(),"meshCount")==1,
                    "Hidden geometry edit lost retained prototype or became visible");
            fixture.scene->AddPrims({{SdfPath("/Hidden/Mesh"),HdPrimTypeTokens->mesh,SmallRedMesh(.9f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Material,SdfPath("/Hidden/Mesh")); fixture.Render();
            Require(Hits(fixture)==0,"Hidden material edit enabled a disabled placement");
            const auto before=fixture.bridge->GetStats();
            for(bool visible:{true,false,true}) {
                fixture.scene->AddPrims({{SdfPath("/Hidden"),TfToken("xform"),Visible(visible)}});
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Hidden")); fixture.Render();
                const auto now=fixture.bridge->GetStats();
                Require(Stat(now,"snapshotBuilds")==Stat(before,"snapshotBuilds") &&
                        Stat(now,"prototypeBuilds")==Stat(before,"prototypeBuilds") &&
                        Stat(now,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                        Stat(now,"visibilityRecoveryRebuilds")==Stat(before,"visibilityRecoveryRebuilds"),
                        "Visibility-only edit rebuilt retained geometry");
                Require((Hits(fixture)>0)==visible,"Ancestor visibility did not toggle disabled placement");
            }
            Require(Hits(fixture)<100,"Showing edited hidden mesh resurrected old larger geometry");
            const auto color=Read<float>(fixture.color);
            const size_t center=(Fixture::Size/2*Fixture::Size+Fixture::Size/2)*4;
            Require(color[center]>2*color[center+1],"Hidden display-material edit was discarded");
            Fixture reference;
            reference.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,SmallRedMesh(.9f)},
                                      {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            reference.Render();
            Require(color==Read<float>(reference.color),
                    "Geometry/material edited while hidden differs from a fresh visible scene");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                HdOverlayContainerDataSource::New(Purpose(HdRenderTagTokens->guide),Plane())}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            Require(Hits(fixture)==0 && Stat(before,"meshCount")==1,"Purpose-filtered prototype was dropped");
            RenderTags(fixture,{HdRenderTagTokens->guide}); Require(Hits(fixture)>0,"Guide tag did not enable placement");
            RenderTags(fixture,{}); Require(Hits(fixture)==0,"Empty tags unexpectedly include guide purpose");
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                HdOverlayContainerDataSource::New(Purpose(HdRenderTagTokens->render),Plane())}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Mesh")); RenderTags(fixture,{});
            Require(Hits(fixture)>0,"Empty tags no longer include render purpose");
            const auto after=fixture.bridge->GetStats();
            Require(Stat(after,"snapshotBuilds")==Stat(before,"snapshotBuilds") &&
                    Stat(after,"topLevelSceneBuilds")==Stat(before,"topLevelSceneBuilds") &&
                    Stat(after,"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                    "Purpose/tag-only edits rebuilt geometry");
        }
        for(bool empty:{false,true}) {
            Fixture fixture;
            auto inner=HdOverlayContainerDataSource::New(InstancedBy(SdfPath("/Outer"),SdfPath("/Inner")),
                Instancer(SdfPath("/Proto"),true,empty));
            fixture.scene->AddPrims({{SdfPath("/Outer"),HdPrimTypeTokens->instancer,Instancer(SdfPath("/Inner"),false)},
                {SdfPath("/Inner"),HdPrimTypeTokens->instancer,inner},
                {SdfPath("/Proto"),TfToken("scope"),HdRetainedContainerDataSource::New()},
                {SdfPath("/Proto/Mesh"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(InstancedBy(SdfPath("/Inner"),SdfPath("/Proto")),Plane())}});
            fixture.Render(); const auto before=fixture.bridge->GetStats();
            Require(Hits(fixture)==0 && Stat(before,"meshCount")==1,
                    "Hidden nested/zero-instance prototype storage was dropped");
            if(!empty) {
                fixture.scene->AddPrims({{SdfPath("/Outer"),HdPrimTypeTokens->instancer,Instancer(SdfPath("/Inner"),true)}});
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Outer")); fixture.Render();
                Require(Hits(fixture)>0 && Stat(fixture.bridge->GetStats(),"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                        "Outer instancer visibility did not enable retained nested placement");
            } else {
                fixture.scene->AddPrims({
                    {SdfPath("/Outer"),HdPrimTypeTokens->instancer,Instancer(SdfPath("/Inner"),true)},
                    {SdfPath("/Inner"),HdPrimTypeTokens->instancer,
                     HdOverlayContainerDataSource::New(InstancedBy(SdfPath("/Outer"),SdfPath("/Inner")),
                         Instancer(SdfPath("/Proto"),true,false))}});
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Outer"));
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Inner")); fixture.Render();
                Require(Hits(fixture)>0 && Stat(fixture.bridge->GetStats(),"primCount")==1 &&
                        Stat(fixture.bridge->GetStats(),"prototypeBuilds")==Stat(before,"prototypeBuilds"),
                        "Zero-instance prototype activation lost its geometry or picking ID");
            }
        }
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            fixture.bridge->SetSettings(settings);
            fixture.scene->AddPrims({{SdfPath("/Hidden"),TfToken("xform"),Visible(false)},
                {SdfPath("/Hidden/Mesh"),HdPrimTypeTokens->mesh,GridMesh()},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render();
            const auto cold=fixture.bridge->GetStats();
            Require(Stat(cold,"evictedPrototypeCount")==1 && Stat(cold,"meshCount")==0 &&
                    Stat(cold,"hiddenGeometryResidentBytes")==0 && Stat(cold,"primCount")==1,
                    "Budget eviction did not release hidden geometry and preserve its identity");
            Require(Stat(cold,"sparePrototypeConversions")==0,
                    "Spare bank reconverted a cold evicted prototype");
            fixture.scene->AddPrims({{SdfPath("/Hidden/Mesh"),HdPrimTypeTokens->mesh,SmallRedMesh(.9f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Hidden/Mesh")); fixture.Render();
            const auto edited=fixture.bridge->GetStats();
            Require(Stat(edited,"evictedPrototypeCount")==1 && Stat(edited,"meshCount")==0 &&
                    Stat(edited,"prototypeBuilds")==Stat(cold,"prototypeBuilds"),
                    "Editing an evicted prototype eagerly rebuilt its geometry");
            settings.hiddenGeometryBudgetMiB=0; fixture.bridge->SetSettings(settings);
            fixture.Prepare();
            Require(Stat(fixture.bridge->GetStats(),"completedSamples")==Stat(edited,"completedSamples") &&
                    Stat(fixture.bridge->GetStats(),"evictedPrototypeCount")==1,
                    "Increasing retention budget reset accumulation or restored cold geometry");
            fixture.scene->AddPrims({{SdfPath("/Hidden"),TfToken("xform"),Visible(true)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Hidden")); fixture.Render();
            Require(Hits(fixture)>0 && Hits(fixture)<100 &&
                    Stat(fixture.bridge->GetStats(),"evictedPrototypeCount")==0 &&
                    Stat(fixture.bridge->GetStats(),"hiddenGeometryRestorations")>0,
                    "Evicted geometry did not restore its latest captured source");
            Fixture reference;
            reference.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,SmallRedMesh(.9f)},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}}); reference.Render();
            Require(Read<float>(fixture.color)==Read<float>(reference.color),
                    "Restored evicted geometry differs from a fresh scene");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Hidden"),HdPrimTypeTokens->mesh,
                HdOverlayContainerDataSource::New(Visible(false),GridMesh())}});
            fixture.Render();
            const auto before=fixture.bridge->GetStats();
            Require(Stat(before,"hiddenGeometryResidentBytes")>1024*1024,
                    "Unlimited retention did not account for actual hidden storage");
            auto pinned=fixture.prim.Capture();
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            fixture.bridge->SetSettings(settings); fixture.Prepare();
            const auto budgeted=fixture.bridge->GetStats();
            Require(Stat(budgeted,"completedSamples")==Stat(before,"completedSamples") &&
                    fixture.prim.Capture().pixels==pinned.pixels &&
                    Stat(budgeted,"hiddenGeometryPinnedBytes")>0 &&
                    Stat(budgeted,"hiddenGeometryBudgetOverageBytes")>0,
                    "Budget change modified a published frame or hid pinned overage");
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Environment); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"hiddenGeometryPinnedBytes")>0,
                    "An externally retained pick frame lost its prototype storage");
            pinned={}; fixture.Prepare();
            Require(Stat(fixture.bridge->GetStats(),"hiddenGeometryResidentBytes")==0,
                    "Released frames or inactive-bank aliases kept evicted storage alive");
        }
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            settings.samples=1; fixture.bridge->SetSettings(settings);
            auto grid=GridMesh();
            fixture.scene->AddPrims({{SdfPath("/Hidden"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(Visible(false),grid)},
                {SdfPath("/Visible"),HdPrimTypeTokens->mesh,grid}});
            fixture.Render(); auto stats=fixture.bridge->GetStats();
            Require(Hits(fixture)>0 && Stat(stats,"meshCount")==1 &&
                    Stat(stats,"hiddenGeometryResidentBytes")==0 &&
                    Stat(stats,"hiddenGeometryEvictions")==0,
                    "Budget evicted or double-counted storage shared with visible geometry");
        }
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            settings.samples=1; fixture.bridge->SetSettings(settings);
            auto grid=GridMesh();
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,grid}});
            fixture.Render();
            auto pinned=fixture.prim.Capture();
            Require(Hits(fixture)>0,"Visible frame was empty before pinning");
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                HdOverlayContainerDataSource::New(Visible(false),grid)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Mesh")); fixture.Render();
            const auto hidden=fixture.bridge->GetStats();
            Require(Hits(fixture)==0 && Stat(hidden,"evictedPrototypeCount")==1 &&
                    Stat(hidden,"hiddenGeometryPinnedBytes")>1024*1024 &&
                    Stat(hidden,"hiddenGeometryBudgetOverageBytes")>0,
                    "Historical visible frame exempted currently hidden storage from the budget");
            pinned={}; fixture.Prepare();
            Require(Stat(fixture.bridge->GetStats(),"hiddenGeometryResidentBytes")==0,
                    "Formerly visible inactive-bank storage remained resident after unpinning");
        }
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            settings.samples=1; fixture.bridge->SetSettings(settings);
            fixture.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(Visible(false),GridMesh(96))},
                {SdfPath("/B"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(Visible(false),GridMesh(96,.01f))}});
            fixture.Render(); auto stats=fixture.bridge->GetStats();
            Require(Stat(stats,"evictedPrototypeCount")==1 && Stat(stats,"meshCount")==1 &&
                    Stat(stats,"hiddenGeometryResidentBytes")<=1024*1024,
                    "Budget did not retain exactly the newest fitting unique storage");
            fixture.scene->AddPrims({{SdfPath("/B"),HdPrimTypeTokens->mesh,GridMesh(96,.01f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/B")); fixture.Render();
            Require(Hits(fixture)>0 && Stat(fixture.bridge->GetStats(),"hiddenGeometryRestorations")==0,
                    "Equal-age storage eviction did not use canonical-path ordering");
        }
        for(bool purpose:{false,true}) {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            settings.samples=1; fixture.bridge->SetSettings(settings);
            if(purpose) {
                fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(Purpose(HdRenderTagTokens->guide),GridMesh())}});
            } else {
                fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,
                        Instancer(SdfPath("/Mesh"),true,true)},
                    {SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                        HdOverlayContainerDataSource::New(InstancedBy(SdfPath("/Instancer"),SdfPath("/Mesh")),GridMesh())}});
            }
            fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"evictedPrototypeCount")==1,
                    "Filtered or zero-instance geometry was not budgeted");
            if(purpose) RenderTags(fixture,{HdRenderTagTokens->guide});
            else {
                fixture.scene->AddPrims({{SdfPath("/Instancer"),HdPrimTypeTokens->instancer,
                    Instancer(SdfPath("/Mesh"),true,false)}});
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Instancer")); fixture.Render();
            }
            Require(Hits(fixture)>0 && Stat(fixture.bridge->GetStats(),"evictedPrototypeCount")==0,
                    "Filtered or zero-instance evicted prototype did not restore");
        }
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.hiddenGeometryBudgetMiB=1;
            settings.samples=1; fixture.bridge->SetSettings(settings);
            fixture.scene->AddPrims({{SdfPath("/Parent"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(Visible(false),GridMesh())},
                {SdfPath("/Parent/Child"),HdPrimTypeTokens->mesh,SmallRedMesh()}});
            fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"evictedPrototypeCount")==1,
                    "Parent mesh did not create retention pressure");
            fixture.scene->AddPrims({{SdfPath("/Parent"),HdPrimTypeTokens->mesh,GridMesh()}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Parent")); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"enabledPlacementCount")==2,
                    "Restoring a parent prototype duplicated its resident child placement");
        }
        {
            Fixture fixture;
            auto settings=fixture.bridge->GetSettings(); settings.samples=1;
            fixture.bridge->SetSettings(settings);
            const auto a=GridMesh(96), b=GridMesh(96,.01f);
            fixture.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,a},
                {SdfPath("/B"),HdPrimTypeTokens->mesh,HdOverlayContainerDataSource::New(Visible(false),b)}});
            fixture.Render();
            fixture.scene->AddPrims({{SdfPath("/A"),HdPrimTypeTokens->mesh,
                    HdOverlayContainerDataSource::New(Visible(false),a)},
                {SdfPath("/B"),HdPrimTypeTokens->mesh,b}});
            fixture.bridge->MarkScenePathsDirty(SceneDirtyKind::Placement,{SdfPath("/A"),SdfPath("/B")});
            fixture.Render();
            settings.hiddenGeometryBudgetMiB=1; fixture.bridge->SetSettings(settings);
            fixture.scene->AddPrims({{SdfPath("/B"),HdPrimTypeTokens->mesh,
                HdOverlayContainerDataSource::New(Visible(false),b)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/B")); fixture.Render();
            const auto hidden=fixture.bridge->GetStats();
            Require(Stat(hidden,"evictedPrototypeCount")==1 && Stat(hidden,"meshCount")==1,
                    "LRU budget did not retain one recently enabled prototype");
            fixture.scene->AddPrims({{SdfPath("/B"),HdPrimTypeTokens->mesh,b}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/B")); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"hiddenGeometryRestorations")==
                    Stat(hidden,"hiddenGeometryRestorations"),
                    "LRU policy evicted the most recently enabled storage");
        }
        std::cout << "Retained visibility/purpose/instance tests passed\n"; return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
