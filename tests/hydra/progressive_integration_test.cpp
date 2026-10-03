#include "integration_fixture.h"
#include <iostream>

int RunProgressiveIntegrationTests()
{
    using namespace pxr;
    using namespace prism::hydra::test;
    try {
        Fixture fixture;
        fixture.scene->AddPrims({{SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()},
                                 {SdfPath("/Mesh"), HdPrimTypeTokens->mesh, Plane()}});
        constexpr unsigned size = 2048;
        fixture.color.Allocate(GfVec3i(size,size,1), HdFormatFloat32Vec4,false);
        fixture.prim.Allocate(GfVec3i(size,size,1), HdFormatInt32,false);
        auto settings = fixture.bridge->GetSettings(); settings.samples = 2;
        fixture.bridge->SetSettings(settings);
        TfTokenVector tags{HdRenderTagTokens->geometry};
        Wait([&] {
            fixture.bridge->Prepare(fixture.camera,fixture.bindings,tags,size,size);
            const auto stats = fixture.bridge->GetStats();
            const auto pending = stats.find("preparationPending");
            return pending != stats.end() && pending->second.IsHolding<bool>() &&
                   !pending->second.UncheckedGet<bool>() &&
                   Stat(stats,"displayedGeneration") >= Stat(stats,"preparationGeneration");
        }, "Progressive fixture preparation timed out");
        fixture.thread.StartRender();
        Wait([&] { return Stat(fixture.bridge->GetStats(),"refinementPublishes") > 0; },
             "No intermediate display was published");
        const auto stats = fixture.bridge->GetStats();
        Require(Stat(stats,"completedSamples") == 0,
                "Intermediate presentation waited for a completed sample");
        const auto raw = fixture.color.Capture();
        const auto shown = fixture.color.CapturePresentation();
        Require(raw.frame == shown.frame && raw.pixels != shown.pixels,
                "Intermediate presentation was not isolated from raw pixels");
        const auto held = *shown.pixels;
        fixture.thread.StopRender();
        Require(Stat(fixture.bridge->GetStats(),"completedSamples") == 0,
                "Canceled intermediate pass committed a partial sample");
        Require(fixture.color.Capture().pixels == raw.pixels &&
                fixture.color.CapturePresentation().pixels == raw.pixels &&
                *shown.pixels == held,
                "Cancellation changed raw pixels or retained presentation");
        Require(Stat(stats,"firstPassPrimaryScratchBytes") <
                static_cast<int64_t>(size)*size*64,
                "First pass retained full differential ray hits");
        // Changing only the color background must preserve accumulation.
        Fixture small; small.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
        auto smallSettings=small.bridge->GetSettings(); smallSettings.samples=2;
        small.bridge->SetSettings(smallSettings); small.Render();
        auto before=small.color.Capture();
        small.bindings[0].clearValue=VtValue(GfVec4f(.2f,.3f,.4f,1));
        small.Prepare();
        Require(Stat(small.bridge->GetStats(),"completedSamples")==2 &&
                small.color.Capture().pixels==before.pixels,
                "Presentation background reset traced accumulation");
        std::cout << "Progressive display isolation and cancellation tests passed\n";
        return 0;
    } catch(std::exception const &e) { std::cerr << e.what() << '\n'; return 1; }
}
