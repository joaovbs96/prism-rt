#include "integration_fixture.h"
#include "../../src/hydra/task_delegate.h"
#include "../../src/camera_rays.h"
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <cmath>
#include <iostream>
#include <utility>

namespace {
pxr::HdContainerDataSourceHandle PlaneWithDoubleUv()
{
    using namespace pxr;
    auto points=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            {{-.6f,-.6f,0},{.6f,-.6f,0},{.6f,.6f,0},{-.6f,.6f,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->vertex)).Build();
    auto st=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec2dArray>::New(
            {{0,0},{1,0},{1,1},{0,1}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
            HdPrimvarSchemaTokens->vertex)).Build();
    auto primvars=HdRetainedContainerDataSource::New(
        TfToken("points"),points,TfToken("st"),st);
    auto topology=HdMeshTopologySchema::Builder().SetFaceVertexCounts(
        HdRetainedTypedSampledDataSource<VtIntArray>::New({4}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(
            {0,1,2,3})).Build();
    return HdRetainedContainerDataSource::New(HdPrimvarsSchemaTokens->primvars,
        primvars,HdMeshSchemaTokens->mesh,
        HdMeshSchema::Builder().SetTopology(topology).Build());
}

void AddAovs(prism::hydra::test::Fixture &fixture,
             pxr::PrismRenderBuffer &peye, pxr::PrismRenderBuffer &st)
{
    using namespace pxr;
    peye.Allocate(GfVec3i(prism::hydra::test::Fixture::Size,
        prism::hydra::test::Fixture::Size,1),HdFormatFloat32Vec3,false);
    st.Allocate(GfVec3i(prism::hydra::test::Fixture::Size,
        prism::hydra::test::Fixture::Size,1),HdFormatFloat32Vec2,false);
    HdRenderPassAovBinding peyeBinding,stBinding;
    peyeBinding.aovName=HdAovTokens->Peye; peyeBinding.renderBuffer=&peye;
    peyeBinding.clearValue=VtValue(GfVec3f(0));
    stBinding.aovName=TfToken("primvars:st"); stBinding.renderBuffer=&st;
    stBinding.clearValue=VtValue(GfVec2f(0));
    fixture.bindings.push_back(peyeBinding); fixture.bindings.push_back(stBinding);
}

std::pair<pxr::GfVec3d,pxr::GfVec2d> ExpectedAt(
    prism::hydra::test::Fixture const &fixture, unsigned int x, unsigned int y)
{
    using namespace pxr;
    prism::CameraRayParameters params;
    params.inverseProjection=fixture.camera.projection.GetInverse();
    params.cameraToWorld=fixture.camera.worldToView.GetInverse();
    params.nearClip=fixture.camera.nearClip; params.farClip=fixture.camera.farClip;
    params.orthographic=fixture.camera.orthographic;
    unsigned int seed=0;
    const RTCRay ray=prism::GenerateCameraRays(params,x,y,
        prism::hydra::test::Fixture::Size,prism::hydra::test::Fixture::Size,seed).center;
    const double t=-static_cast<double>(ray.org_z)/ray.dir_z;
    GfVec3d const world(ray.org_x+t*ray.dir_x,ray.org_y+t*ray.dir_y,0.0);
    GfVec3d const eye=fixture.camera.worldToView.Transform(world);
    return {eye,GfVec2d((world[0]+.6)/1.2,(world[1]+.6)/1.2)};
}
}

int RunC2GeometricAovIntegrationTests()
{
    using namespace prism::hydra::test;
    using namespace pxr;
    try {
        PrismTaskDelegate delegate;
        Require(delegate.GetDefaultAovDescriptor(HdAovTokens->Peye).format==HdFormatFloat32Vec3,
                "Peye default descriptor is unavailable");
        Require(delegate.GetDefaultAovDescriptor(TfToken("primvars:st")).format==HdFormatFloat32Vec2,
                "primvars:st default descriptor is unavailable");
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,
                                      PlaneWithDoubleUv()}});
            PrismRenderBuffer peye(SdfPath("/Peye"),&fixture.thread),
                st(SdfPath("/st"),&fixture.thread);
            AddAovs(fixture,peye,st);
            fixture.Render();
            const size_t pixel=Fixture::Size/2*Fixture::Size+Fixture::Size/2;
            auto const [expectedEye,expectedSt]=ExpectedAt(
                fixture,Fixture::Size/2,Fixture::Size/2-1);
            const auto eye=Read<float>(peye); const auto uv=Read<float>(st);
            for(size_t i=0;i<3;++i)
                Require(std::abs(eye[pixel*3+i]-expectedEye[i])<1e-3,
                        "Peye differs from the transformed deterministic center hit");
            Require(std::abs(uv[pixel*2]-expectedSt[0])<1e-3 &&
                    std::abs(uv[pixel*2+1]-expectedSt[1])<1e-3,
                    "primvars:st differs from the interpolated center-hit UV");
            fixture.Preview();
            auto const [previewEye,previewSt]=ExpectedAt(fixture,17,15);
            const auto previewEyeValues=Read<float>(peye),previewStValues=Read<float>(st);
            for(size_t i=0;i<3;++i)
                Require(std::abs(previewEyeValues[pixel*3+i]-previewEye[i])<1e-3,
                        "preview Peye differs from its center-ray hit");
            Require(std::abs(previewStValues[pixel*2]-previewSt[0])<1e-3 &&
                    std::abs(previewStValues[pixel*2+1]-previewSt[1])<1e-3,
                    "preview primvars:st differs from its center-ray UV");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}});
            PrismRenderBuffer peye(SdfPath("/Peye"),&fixture.thread),
                st(SdfPath("/st"),&fixture.thread);
            AddAovs(fixture,peye,st); fixture.Render();
            const size_t pixel=Fixture::Size/2*Fixture::Size+Fixture::Size/2;
            const auto uv=Read<float>(st);
            Require(uv[pixel*2]==0.0f && uv[pixel*2+1]==0.0f,
                    "missing st primvar did not use the existing hit.u/v fallback");
        }
        std::cout<<"Geometric AOV integration tests passed\n";
        return 0;
    } catch(std::exception const &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
