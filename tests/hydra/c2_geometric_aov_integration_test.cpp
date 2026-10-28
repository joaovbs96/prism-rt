#include "integration_fixture.h"
#include "../../src/hydra/task_delegate.h"
#include "../../src/camera_rays.h"
#include "../../src/usd/gf_convert.h"
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
    params.inverseProjection=prism::usd::ToMatrix4d(fixture.camera.projection.GetInverse());
    params.cameraToWorld=prism::usd::ToMatrix4d(fixture.camera.worldToView.GetInverse());
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
        // Each subset must stage exactly its requested native outputs, even
        // when bindings repeat an AOV or include an unsupported/null target.
        const std::vector<TfToken> kinds{HdAovTokens->depth, HdAovTokens->cameraDepth,
            HdAovTokens->normal, HdAovTokens->Neye, HdAovTokens->Peye,
            TfToken("primvars:st"), HdAovTokens->primId, HdAovTokens->instanceId,
            HdAovTokens->elementId};
        PrismTaskDelegate descriptors;
        for (size_t requested = 0; requested <= kinds.size(); ++requested) {
            Fixture subset;
            subset.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()},
                {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            subset.camera.fStop = 2.8f;
            subset.camera.focalLength = 50;
            subset.camera.focusDistance = 3;
            subset.bindings.resize(1);
            auto settings = subset.bridge->GetSettings(); settings.samples = 1;
            subset.bridge->SetSettings(settings);
            std::vector<std::unique_ptr<PrismRenderBuffer>> outputs;
            size_t bytesPerPixel = 0;
            for (size_t i = 0; i < requested; ++i) {
                const auto descriptor = descriptors.GetDefaultAovDescriptor(kinds[i]);
                bytesPerPixel += HdDataSizeOfFormat(descriptor.format);
                // Distinct destinations for the same kind share one plane.
                for (unsigned duplicate = 0; duplicate < 2; ++duplicate) {
                    auto buffer = std::make_unique<PrismRenderBuffer>(
                        SdfPath("/out" + std::to_string(outputs.size())), &subset.thread);
                    buffer->Allocate(GfVec3i(Fixture::Size,Fixture::Size,1),descriptor.format,false);
                    HdRenderPassAovBinding binding;
                    binding.aovName=kinds[i]; binding.renderBuffer=buffer.get();
                    binding.clearValue=descriptor.clearValue;
                    subset.bindings.push_back(binding); outputs.push_back(std::move(buffer));
                }
            }
            HdRenderPassAovBinding nullBinding;
            nullBinding.aovName=HdAovTokens->normal;
            subset.bindings.push_back(nullBinding);
            PrismRenderBuffer unsupported(SdfPath("/unsupported"), &subset.thread);
            unsupported.Allocate(GfVec3i(Fixture::Size,Fixture::Size,1),HdFormatFloat32,false);
            HdRenderPassAovBinding unsupportedBinding;
            unsupportedBinding.aovName=TfToken("unsupported:staging-test");
            unsupportedBinding.renderBuffer=&unsupported;
            unsupportedBinding.clearValue=VtValue(-1.0f);
            subset.bindings.push_back(unsupportedBinding);
            PrismRenderBuffer unsupportedInt(SdfPath("/unsupportedInt"), &subset.thread);
            unsupportedInt.Allocate(GfVec3i(Fixture::Size,Fixture::Size,1),HdFormatInt32,false);
            auto integerClear = unsupportedBinding;
            integerClear.aovName = TfToken("unsupported:integer-clear");
            integerClear.renderBuffer = &unsupportedInt;
            integerClear.clearValue = VtValue(16777217);
            subset.bindings.push_back(integerClear);
            subset.Render();
            Require(Stat(subset.bridge->GetStats(),"firstPassPrimaryScratchBytes") ==
                    static_cast<int64_t>(Fixture::Size)*Fixture::Size*bytesPerPixel,
                    "AOV staging does not match distinct requested output planes");
            for(size_t i=0;i<outputs.size();i+=2)
                Require(*outputs[i]->Capture().pixels==*outputs[i+1]->Capture().pixels,
                        "Duplicate AOV bindings disagree");

            // Rebind the same scene to deterministic outputs only. Keep an
            // old map alive across publication, and compare every native byte
            // against the equivalent mixed color/geometric render above.
            auto mixedBindings = subset.bindings;
            auto mixedColor = subset.color.Capture();
            std::vector<PrismRenderBuffer::ReadView> expected;
            for (auto const &output : outputs) expected.push_back(output->Capture());
            auto *retained = outputs.empty() ? &unsupported : outputs.front().get();
            auto retainedView = retained->Capture();
            auto *mapped = static_cast<uint8_t *>(retained->Map());
            auto before = subset.bridge->GetStats();
            subset.bindings.erase(subset.bindings.begin());
            HdRenderPassAovBinding nullColor;
            nullColor.aovName = HdAovTokens->color;
            subset.bindings.push_back(nullColor);
            subset.bindings.push_back(subset.bindings.front());
            settings.samples = 10000;
            settings.refinementDelayMs = 2000;
            subset.bridge->SetSettings(settings);
            subset.Prepare();
            subset.thread.StartRender();
            Wait([&] { return subset.bridge->IsConverged(); },
                 "Geometric-only pass did not converge independently of color budget");
            subset.thread.StopRender();
            Require(std::memcmp(mapped, retainedView.pixels->data(), retainedView.pixels->size()) == 0,
                    "Geometric publication mutated a retained old map");
            retained->Unmap();
            auto after = subset.bridge->GetStats();
            Require(Stat(after,"completedSamples") == 1 &&
                    Stat(after,"colorTracedPixels") == Stat(before,"colorTracedPixels") &&
                    Stat(after,"previewPublishes") == Stat(before,"previewPublishes") &&
                    Stat(after,"geometricOnlyPixels") - Stat(before,"geometricOnlyPixels") ==
                        (requested ? static_cast<int64_t>(Fixture::Size)*Fixture::Size : 0),
                    "Geometric-only/unsupported-only request performed unexpected ray work");
            Require(Stat(after,"colorPassScratchBytes") == 0 &&
                    Stat(after,"colorRngStorageBytes") == 0 &&
                    Stat(after,"firstPassPrimaryScratchBytes") ==
                        static_cast<int64_t>(Fixture::Size)*Fixture::Size*bytesPerPixel,
                    "Geometric-only request retained color pass or RNG storage");
            for (size_t i=0; i<outputs.size(); ++i)
                Require(*outputs[i]->Capture().pixels == *expected[i].pixels && outputs[i]->IsConverged(),
                        "Geometric-only AOV differs from its mixed-render reference");
            for (float value : Read<float>(unsupported))
                Require(value == -1.0f, "Unsupported-only output lost its clear value");
            for (int value : Read<int>(unsupportedInt))
                Require(value == 16777217, "Unsupported integer clear lost exact large IDs");
            auto completed = retained->Capture();
            settings.samples = 50;
            subset.bridge->SetSettings(settings);
            Require(subset.bridge->IsConverged(), "Color budget invalidated deterministic convergence");
            subset.Prepare();
            subset.thread.StartRender();
            Wait([&] { return subset.bridge->IsConverged(); }, "Completed geometric pass failed to resume");
            subset.thread.StopRender();
            Require(retained->Capture().pixels == completed.pixels &&
                    Stat(subset.bridge->GetStats(),"geometricOnlyPixels") == Stat(after,"geometricOnlyPixels"),
                    "Color sample-budget change repeated a completed geometric pass");
            if (requested == 0) {
                for (auto &binding : subset.bindings)
                    if (binding.renderBuffer == &unsupported) binding.clearValue = VtValue(-7.0f);
                subset.Prepare();
                subset.thread.StartRender();
                Wait([&] { return subset.bridge->IsConverged(); }, "Unsupported clear rebinding timed out");
                subset.thread.StopRender();
                for (float value : Read<float>(unsupported))
                    Require(value == -7.0f, "Unsupported-only rebinding published an old clear value");
                Require(Stat(subset.bridge->GetStats(),"colorTracedPixels") == Stat(after,"colorTracedPixels") &&
                        Stat(subset.bridge->GetStats(),"geometricOnlyPixels") == Stat(after,"geometricOnlyPixels"),
                        "Unsupported-only clear rebinding performed ray work");
            }
            settings.samples = 1;
            settings.refinementDelayMs = 0;
            subset.bridge->SetSettings(settings);
            subset.bindings = mixedBindings;
            subset.Render();
            Require(*subset.color.Capture().pixels == *mixedColor.pixels,
                    "Adding color after geometric-only rendering changed fresh accumulation");
        }
        {
            Fixture geometric;
            geometric.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}});
            geometric.bindings.erase(geometric.bindings.begin());
            auto settings = geometric.bridge->GetSettings();
            settings.samples = 900; settings.refinementDelayMs = 2000;
            geometric.bridge->SetSettings(settings);
            auto prepare = [&](unsigned width, unsigned height) {
                Wait([&] {
                    geometric.bridge->Prepare(geometric.camera, geometric.bindings,
                        {HdRenderTagTokens->geometry}, width, height);
                    auto stats = geometric.bridge->GetStats();
                    return !stats["preparationPending"].UncheckedGet<bool>() &&
                        Stat(stats,"displayedGeneration") >= Stat(stats,"preparationGeneration");
                }, "Geometric-only fixture preparation timed out");
            };
            auto render = [&](unsigned width, unsigned height) {
                prepare(width,height);
                geometric.thread.StartRender();
                Wait([&] { return geometric.bridge->IsConverged(); }, "Geometric-only render timed out");
                geometric.thread.StopRender();
            };
            constexpr unsigned width=37, height=29;
            geometric.prim.Allocate(GfVec3i(width,height,1),HdFormatInt32,false);
            render(width,height);
            auto first = geometric.prim.Capture();
            auto stats = geometric.bridge->GetStats();
            Require(Stat(stats,"completedSamples") == 1 &&
                    Stat(stats,"colorTracedPixels") == 0 && Stat(stats,"previewPublishes") == 0 &&
                    Stat(stats,"geometricOnlyPixels") == width*height,
                    "Fresh odd-sized geometric-only render traced color or multiple passes");
            geometric.camera.worldToView.SetTranslate(GfVec3d(.25,0,-3));
            render(width,height);
            Require(Stat(geometric.bridge->GetStats(),"geometricOnlyPixels") == 2*width*height &&
                    *geometric.prim.Capture().pixels != *first.pixels,
                    "Camera edit did not invalidate geometric-only outputs");
            geometric.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane(.25f)}});
            geometric.bridge->MarkSceneDirty();
            render(width,height);
            Require(Stat(geometric.bridge->GetStats(),"geometricOnlyPixels") == 3*width*height,
                    "Scene edit did not invalidate geometric-only outputs");

            // Observe actual center-ray work before stopping a large pass.
            // No intermediate full-resolution geometric buffer may escape.
            constexpr unsigned largeWidth=4097, largeHeight=2049;
            geometric.prim.Allocate(GfVec3i(largeWidth,largeHeight,1),HdFormatInt32,false);
            prepare(largeWidth,largeHeight);
            auto raw = geometric.prim.Capture();
            const auto before = Stat(geometric.bridge->GetStats(),"geometricOnlyPixels");
            geometric.thread.StartRender();
            Wait([&] { return Stat(geometric.bridge->GetStats(),"geometricOnlyPixels") > before; },
                 "Geometric-only cancellation fixture did not start ray work");
            geometric.thread.PauseRender();
            geometric.thread.StopRender();
            geometric.thread.ResumeRender();
            stats = geometric.bridge->GetStats();
            Require(!geometric.bridge->IsConverged() && !geometric.prim.IsConverged() &&
                    Stat(stats,"completedSamples") == 0 &&
                    geometric.prim.Capture().frame == raw.frame &&
                    *geometric.prim.Capture().pixels == *raw.pixels &&
                    Stat(stats,"colorTracedPixels") == 0,
                    "Canceled geometric-only pass published partial output or convergence");
            geometric.prim.Allocate(GfVec3i(width,height,1),HdFormatInt32,false);
            const auto resumed = Stat(stats,"geometricOnlyPixels");
            render(width,height);
            Require(Stat(geometric.bridge->GetStats(),"geometricOnlyPixels") == resumed+width*height &&
                    Stat(geometric.bridge->GetStats(),"completedSamples") == 1,
                    "Canceled geometric-only pass failed to restart cleanly after resize");
        }
        {
            Fixture repeated, reference;
            for (auto *fixture : {&repeated, &reference})
                fixture->scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()},
                    {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            repeated.bindings.push_back(repeated.bindings[0]);
            repeated.bindings.push_back(repeated.bindings[1]);
            repeated.Render(); reference.Render();
            Require(Read<float>(repeated.color)==Read<float>(reference.color) &&
                    Read<int>(repeated.prim)==Read<int>(reference.prim),
                    "Repeated bindings to one destination accumulated or published twice");
        }
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
