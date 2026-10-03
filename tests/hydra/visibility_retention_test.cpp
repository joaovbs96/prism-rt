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
        std::cout << "Retained visibility/purpose/instance tests passed\n"; return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
