#include "integration_fixture.h"
#include <iostream>

namespace {
pxr::HdContainerDataSourceHandle Plane(bool doubles)
{
    using namespace pxr;
    HdSampledDataSourceHandle pointsValue, normalsValue, uvValue;
    if (doubles) {
        pointsValue=HdRetainedTypedSampledDataSource<VtVec3dArray>::New(
            {{-.6,-.6,0},{.6,-.6,0},{.6,.6,0},{-.6,.6,0}});
        normalsValue=HdRetainedTypedSampledDataSource<VtVec3dArray>::New(
            {GfVec3d(.6,0,.8),GfVec3d(.6,0,.8),GfVec3d(.6,0,.8),GfVec3d(.6,0,.8)});
        uvValue=HdRetainedTypedSampledDataSource<VtVec2dArray>::New(
            {{0,0},{1,0},{1,1},{0,1}});
    } else {
        pointsValue=HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            {{-.6f,-.6f,0},{.6f,-.6f,0},{.6f,.6f,0},{-.6f,.6f,0}});
        normalsValue=HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            {GfVec3f(.6f,0,.8f),GfVec3f(.6f,0,.8f),GfVec3f(.6f,0,.8f),GfVec3f(.6f,0,.8f)});
        uvValue=HdRetainedTypedSampledDataSource<VtVec2fArray>::New(
            {{0,0},{1,0},{1,1},{0,1}});
    }
    auto vertex=[](HdSampledDataSourceHandle value) {
        return HdPrimvarSchema::Builder().SetPrimvarValue(value)
            .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                HdPrimvarSchemaTokens->vertex)).Build();
    };
    auto points=vertex(pointsValue), normals=vertex(normalsValue), uv=vertex(uvValue);
    auto color=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New({GfVec3f(.7f)}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->constant)).Build();
    auto primvars=HdRetainedContainerDataSource::New(
        TfToken("points"),points,TfToken("normals"),normals,
        TfToken("st"),uv,TfToken("displayColor"),color);
    auto topology=HdMeshTopologySchema::Builder().SetFaceVertexCounts(
        HdRetainedTypedSampledDataSource<VtIntArray>::New({4}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(
            {0,1,2,3})).Build();
    return HdRetainedContainerDataSource::New(HdPrimvarsSchemaTokens->primvars,
        primvars,HdMeshSchemaTokens->mesh,
        HdMeshSchema::Builder().SetTopology(topology).Build());
}
}

int RunB2DoublePrimvarIntegrationTests()
{
    using namespace prism::hydra::test;
    using namespace pxr;
    try {
        std::vector<float> referenceColor, referenceNormal;
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane(false)}});
            PrismRenderBuffer normal(SdfPath("/normal"),&fixture.thread);
            normal.Allocate(GfVec3i(Fixture::Size,Fixture::Size,1),HdFormatFloat32Vec3,false);
            HdRenderPassAovBinding binding; binding.aovName=HdAovTokens->normal;
            binding.renderBuffer=&normal; binding.clearValue=VtValue(GfVec3f(0));
            fixture.bindings.push_back(binding);
            fixture.Render(); referenceColor=Read<float>(fixture.color);
            referenceNormal=Read<float>(normal);
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane(true)}});
            PrismRenderBuffer normal(SdfPath("/normal"),&fixture.thread);
            normal.Allocate(GfVec3i(Fixture::Size,Fixture::Size,1),HdFormatFloat32Vec3,false);
            HdRenderPassAovBinding binding; binding.aovName=HdAovTokens->normal;
            binding.renderBuffer=&normal; binding.clearValue=VtValue(GfVec3f(0));
            fixture.bindings.push_back(binding);
            fixture.Render();
            Require(Read<float>(fixture.color)==referenceColor,
                    "point3d geometry does not match point3f reference pixels");
            Require(Read<float>(normal)==referenceNormal,
                    "double normals do not match the float reference AOV");
            const size_t center=(Fixture::Size/2*Fixture::Size+Fixture::Size/2)*3;
            const auto values=Read<float>(normal);
            Require(values[center]>.5f && values[center+2]>.7f,
                    "authored non-axis normal was not preserved");
        }
        std::cout << "Double-precision Hydra primvar integration tests passed\n";
        return 0;
    } catch(std::exception const &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
