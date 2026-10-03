#include "integration_fixture.h"
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/utils.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <array>
#include <cmath>
#include <iostream>

namespace {
pxr::HdContainerDataSourceHandle Mesh(float opacity, bool color=true)
{
    using namespace pxr;
    auto points=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            {{-.6f,-.6f,0},{.6f,-.6f,0},{.6f,.6f,0},{-.6f,.6f,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->vertex)).Build();
    auto alpha=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtFloatArray>::New({opacity}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->constant)).Build();
    HdContainerDataSourceHandle primvars=HdRetainedContainerDataSource::New(
        TfToken("points"),points,TfToken("displayOpacity"),alpha);
    if(color) {
        auto displayColor=HdPrimvarSchema::Builder().SetPrimvarValue(
            HdRetainedTypedSampledDataSource<VtVec3fArray>::New({GfVec3f(1,0,0)}))
            .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                HdPrimvarSchemaTokens->constant)).Build();
        primvars=HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(TfToken("displayColor"),displayColor),
            primvars);
    }
    auto topology=HdMeshTopologySchema::Builder().SetFaceVertexCounts(
        HdRetainedTypedSampledDataSource<VtIntArray>::New({4}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(
            {0,1,2,3})).Build();
    return HdRetainedContainerDataSource::New(HdPrimvarsSchemaTokens->primvars,
        primvars,HdMeshSchemaTokens->mesh,
        HdMeshSchema::Builder().SetTopology(topology).Build());
}

pxr::HdContainerDataSourceHandle Sphere(float opacity)
{
    using namespace pxr;
    auto alpha=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtFloatArray>::New({opacity}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->constant)).Build();
    auto primvars=HdRetainedContainerDataSource::New(
        TfToken("displayOpacity"),alpha);
    return HdRetainedContainerDataSource::New(
        HdSphereSchemaTokens->sphere,HdSphereSchema::Builder()
            .SetRadius(HdRetainedTypedSampledDataSource<double>::New(.7)).Build(),
        HdPrimvarsSchemaTokens->primvars,primvars);
}

pxr::HdContainerDataSourceHandle PreviewMaterial(pxr::SdfPath const &path)
{
    using namespace pxr;
    HdMaterialNode preview;
    preview.path=path.AppendChild(TfToken("Preview"));
    preview.identifier=TfToken("UsdPreviewSurface");
    preview.parameters[TfToken("diffuseColor")]=VtValue(GfVec3f(.1f,.8f,.2f));
    HdMaterialNetworkMap networkMap;
    networkMap.map[TfToken("surface")].nodes={preview};
    HdDataSourceBaseHandle const network=
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(networkMap);
    TfToken context=HdMaterialSchemaTokens->universalRenderContext;
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,
        HdMaterialSchema::BuildRetained(1,&context,&network));
}

pxr::HdContainerDataSourceHandle Bind(pxr::SdfPath const &path,
                                     pxr::HdContainerDataSourceHandle const &mesh)
{
    using namespace pxr;
    HdDataSourceBaseHandle const binding=HdMaterialBindingSchema::Builder()
        .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(path)).Build();
    TfToken purpose=HdMaterialBindingsSchemaTokens->allPurpose;
    HdDataSourceBaseHandle const bindings=
        HdMaterialBindingsSchema::BuildRetained(1,&purpose,&binding);
    return HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(HdMaterialBindingsSchema::GetSchemaToken(),bindings),
        mesh);
}

std::array<float,3> CenterRgb(pxr::PrismRenderBuffer const &buffer)
{
    auto rgba=prism::hydra::test::Read<float>(buffer);
    const size_t p=(prism::hydra::test::Fixture::Size/2*
        prism::hydra::test::Fixture::Size+prism::hydra::test::Fixture::Size/2)*4;
    return {rgba[p],rgba[p+1],rgba[p+2]};
}
}

int RunB3FallbackOpacityIntegrationTests()
{
    using namespace prism::hydra;
    using namespace prism::hydra::test;
    using namespace pxr;
    try {
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()},
                {SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Mesh(0.0f)}});
            fixture.Render(); const auto transparent=CenterRgb(fixture.color);
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Mesh(1.0f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Mesh"));
            fixture.Render(); const auto opaque=CenterRgb(fixture.color);
            Require(transparent[1]>.8f && opaque[0]>.8f && opaque[1]<.2f,
                    "mesh displayOpacity did not change the fallback surface response");
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Mesh(.5f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Mesh"));
            fixture.Render(); const auto fractional=CenterRgb(fixture.color);
            Require(fractional[1]>.2f && fractional[1]<.8f,
                    "fractional displayOpacity did not produce a stochastic fallback blend");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()},
                {SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Mesh(0.0f,false)}});
            fixture.Render(); const auto transparent=CenterRgb(fixture.color);
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Mesh(1.0f,false)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Mesh"));
            fixture.Render(); const auto opaque=CenterRgb(fixture.color);
            Require(transparent[0]>.8f && opaque[0]>.2f && opaque[0]<.8f,
                    "opacity fallback without displayColor was not applied");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()},
                {SdfPath("/Sphere"),HdPrimTypeTokens->sphere,Sphere(0.0f)}});
            fixture.Render(); const auto transparent=CenterRgb(fixture.color);
            fixture.scene->AddPrims({{SdfPath("/Sphere"),HdPrimTypeTokens->sphere,Sphere(1.0f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Sphere"));
            fixture.Render(); const auto opaque=CenterRgb(fixture.color);
            Require(transparent[0]>.8f && opaque[0]>.2f && opaque[0]<.8f,
                    "sphere displayOpacity fallback was not applied");
        }
        {
            Fixture fixture;
            const SdfPath materialPath("/Looks/Material");
            fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()},
                {materialPath,HdPrimTypeTokens->material,PreviewMaterial(materialPath)},
                {SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                    Bind(materialPath,Mesh(0.0f))}});
            fixture.Render(); const auto transparentAuthored=CenterRgb(fixture.color);
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                Bind(materialPath,Mesh(1.0f))}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Mesh"));
            fixture.Render(); const auto opaqueAuthored=CenterRgb(fixture.color);
            for(size_t channel=0;channel<3;++channel)
                Require(std::abs(transparentAuthored[channel]-opaqueAuthored[channel])<1e-5f,
                        "displayOpacity overrode an authored material");
        }
        std::cout<<"Display opacity fallback integration tests passed\n";
        return 0;
    } catch(std::exception const &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
