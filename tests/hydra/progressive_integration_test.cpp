#include "integration_fixture.h"
#include "../../src/hydra/refinement_tiles.h"
#include <iostream>

int RunProgressiveIntegrationTests()
{
    using namespace pxr;
    using namespace prism::hydra::test;
    try {
        // An odd viewport exercises clipped tiles. Coverage and early spatial
        // distribution are observable scheduling requirements, not timings.
        const auto schedule = prism::hydra::MakeRefinementTiles(1001, 733);
        std::vector<unsigned char> coverage(1001 * 733, 0);
        unsigned quadrants = 0;
        for (size_t i = 0; i < schedule.size(); ++i) {
            auto const &tile = schedule[i];
            if (i < 4) quadrants |= 1u << ((tile.x >= 500) + 2 * (tile.y >= 366));
            for (unsigned y = tile.y; y < tile.y + tile.height; ++y)
                for (unsigned x = tile.x; x < tile.x + tile.width; ++x)
                    ++coverage[y * 1001 + x];
        }
        Require(quadrants == 15 && std::all_of(coverage.begin(), coverage.end(),
                    [](unsigned char count) { return count == 1; }),
                "Refinement tiles failed spatial distribution or exact coverage");
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
        fixture.thread.PauseRender();
        const auto stats = fixture.bridge->GetStats();
        Require(Stat(stats,"completedSamples") == 0,
                "Intermediate presentation waited for a completed sample");
        const auto raw = fixture.color.Capture();
        const auto shown = fixture.color.CapturePresentationState();
        Require(shown.snapshot && raw.frame == shown.raw.frame &&
                    raw.pixels == shown.raw.pixels,
                "Intermediate presentation was not isolated from raw pixels");
        unsigned displayedQuadrants = 0;
        for (auto const &completed : shown.snapshot->tiles)
            if (completed)
                displayedQuadrants |= 1u << ((completed->x >= size/2) +
                                             2 * (completed->y >= size/2));
        Require(displayedQuadrants == 15,
                "Intermediate completed tiles did not cover all four screen quadrants");
        const auto tile = std::find_if(shown.snapshot->tiles.begin(),
            shown.snapshot->tiles.end(), [](auto const &value) { return bool(value); });
        Require(tile != shown.snapshot->tiles.end(), "Partial packet has no completed tiles");
        const auto held = (*tile)->rgba;
        fixture.thread.StopRender();
        fixture.thread.ResumeRender();
        Require(Stat(fixture.bridge->GetStats(),"completedSamples") == 0,
                "Canceled intermediate pass committed a partial sample");
        Require(fixture.color.Capture().pixels == raw.pixels &&
                fixture.color.CapturePresentation().pixels == raw.pixels &&
                (*tile)->rgba == held,
                "Cancellation changed raw pixels or retained presentation");
        Require(Stat(stats,"firstPassPrimaryScratchBytes") ==
                static_cast<int64_t>(size)*size*sizeof(int),
                "Prim-ID staging retained unused geometric attributes");
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
