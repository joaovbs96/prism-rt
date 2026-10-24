#include "integration_fixture.h"

#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;

constexpr unsigned kInitialSize = 32;
constexpr unsigned kResizedSize = 16;
constexpr int kContinuedSamples = 128;

void AddScene(Fixture &fixture)
{
    fixture.scene->AddPrims({
        {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()},
        {SdfPath("/Mesh"), HdPrimTypeTokens->mesh, Plane()},
    });
}

void AddColorBinding(Fixture &fixture, PrismRenderBuffer &buffer)
{
    buffer.Allocate(GfVec3i(Fixture::Size, Fixture::Size, 1),
                    HdFormatFloat32Vec4, false);
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = &buffer;
    binding.clearValue = VtValue(GfVec4f(0));
    fixture.bindings.push_back(binding);
}

void SetSampleBudget(Fixture &fixture, int samples)
{
    RenderSettings settings = fixture.bridge->GetSettings();
    settings.samples = samples;
    fixture.bridge->SetSettings(settings);
}

void Prepare(Fixture &fixture, unsigned width, unsigned height)
{
    TfTokenVector const tags{HdRenderTagTokens->geometry};
    Wait([&] {
        fixture.bridge->Prepare(fixture.camera, fixture.bindings, tags,
                                width, height);
        VtDictionary const stats = fixture.bridge->GetStats();
        auto const pending = stats.find("preparationPending");
        return pending != stats.end() && pending->second.IsHolding<bool>() &&
            !pending->second.UncheckedGet<bool>() &&
            Stat(stats, "displayedGeneration") >=
                Stat(stats, "preparationGeneration");
    }, "Accumulation integration fixture preparation timed out");
}

void Render(Fixture &fixture, unsigned width, unsigned height, int expectedSamples)
{
    Prepare(fixture, width, height);
    fixture.thread.StartRender();
    Wait([&] { return fixture.bridge->IsConverged() && fixture.color.IsConverged(); },
         "Accumulation integration fixture render timed out");
    fixture.thread.StopRender();
    Require(Stat(fixture.bridge->GetStats(), "completedSamples") == expectedSamples,
            "Render stopped at the wrong sample count");
}

void ResizeBuffers(Fixture &fixture, PrismRenderBuffer &secondColor,
                   unsigned size)
{
    fixture.color.Allocate(GfVec3i(size, size, 1), HdFormatFloat32Vec4, false);
    secondColor.Allocate(GfVec3i(size, size, 1), HdFormatFloat32Vec4, false);
    fixture.prim.Allocate(GfVec3i(size, size, 1), HdFormatInt32, false);
}

void RequireCoherentColorFrames(Fixture &fixture, PrismRenderBuffer &secondColor)
{
    auto const colorFrame = fixture.color.Capture().frame;
    Require(colorFrame && colorFrame == secondColor.Capture().frame &&
                colorFrame == fixture.prim.Capture().frame,
            "Color bindings and IDs do not share one published frame");
    Require(Read<float>(fixture.color) == Read<float>(secondColor),
            "Multiple color bindings produced different pixels");
}

void RequireUsefulColor(std::vector<float> const &pixels)
{
    bool hasRadiance = false;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4)
        hasRadiance |= std::isfinite(pixels[i]) && std::isfinite(pixels[i + 1]) &&
            std::isfinite(pixels[i + 2]) &&
            pixels[i] + pixels[i + 1] + pixels[i + 2] > 0.003f;
    Require(hasRadiance, "Accumulation fixture produced no nonzero color samples");
}

// Standard Hydra progress keys read by hosts such as usdview.
void RequireProgressStats(VtDictionary const &stats, int completed, int target,
                          bool converged)
{
    auto const num = stats.find("numCompletedSamples");
    auto const tgt = stats.find("targetSamples");
    auto const pct = stats.find("percentDone");
    Require(num != stats.end() && num->second.IsHolding<int>() &&
                tgt != stats.end() && tgt->second.IsHolding<int>() &&
                pct != stats.end() && pct->second.IsHolding<double>(),
            "Render stats lack int numCompletedSamples/targetSamples or double percentDone");
    Require(num->second.UncheckedGet<int>() == completed &&
                tgt->second.UncheckedGet<int>() == target,
            "numCompletedSamples/targetSamples do not match the render");
    const double percent = pct->second.UncheckedGet<double>();
    if (converged)
        Require(percent == 100.0, "percentDone is not 100 on convergence");
    else
        Require(percent >= 0.0 && percent < 100.0 &&
                    std::abs(percent - 100.0 * completed / target) < 1e-9,
                "percentDone does not track completed/target samples");
}

} // namespace

int RunAccumulationIntegrationTests()
{
    try {
        // Raising the budget must continue the existing stream. A distinct
        // bridge renders the 128-sample reference from its own initial state.
        Fixture continued;
        PrismRenderBuffer continuedSecond(SdfPath("/continued_second"),
                                          &continued.thread);
        AddScene(continued);
        AddColorBinding(continued, continuedSecond);
        Prepare(continued, kInitialSize, kInitialSize);
        RequireProgressStats(continued.bridge->GetStats(), 0, 64, false);
        Render(continued, kInitialSize, kInitialSize, 64);
        auto const first64 = Read<float>(continued.color);
        RequireUsefulColor(first64);
        RequireProgressStats(continued.bridge->GetStats(), 64, 64, true);
        SetSampleBudget(continued, kContinuedSamples);
        Render(continued, kInitialSize, kInitialSize, kContinuedSamples);
        RequireProgressStats(continued.bridge->GetStats(), kContinuedSamples,
                             kContinuedSamples, true);
        auto const continued128 = Read<float>(continued.color);
        RequireCoherentColorFrames(continued, continuedSecond);
        Require(continued128 != first64,
                "Increasing the budget did not add visible samples");
        auto const continuationStats = continued.bridge->GetStats();
        Require(Stat(continuationStats, "completedSamples") == kContinuedSamples,
                "Continued render did not reach 128 samples");

        Fixture fresh128;
        PrismRenderBuffer fresh128Second(SdfPath("/fresh128_second"),
                                         &fresh128.thread);
        AddScene(fresh128);
        AddColorBinding(fresh128, fresh128Second);
        SetSampleBudget(fresh128, kContinuedSamples);
        Render(fresh128, kInitialSize, kInitialSize, kContinuedSamples);
        Require(continued128 == Read<float>(fresh128.color),
                "64-to-128 continuation differs from an independent fresh 128-sample bridge");
        RequireCoherentColorFrames(fresh128, fresh128Second);

        // Stop after committed work, hold the budget at that count, then resume
        // to a target that an independent bridge renders from sample zero.
        RenderSettings interruptedSettings = continued.bridge->GetSettings();
        interruptedSettings.samples = 1000000;
        continued.bridge->SetSettings(interruptedSettings);
        continued.bridge->RestartAccumulation();
        Prepare(continued, kInitialSize, kInitialSize);
        continued.thread.StartRender();
        Wait([&] {
            return Stat(continued.bridge->GetStats(), "completedSamples") >= 8;
        }, "Interrupted accumulation did not commit its initial sample batch");
        continued.thread.StopRender();
        const int committedSamples = static_cast<int>(
            Stat(continued.bridge->GetStats(), "completedSamples"));
        Require(committedSamples >= 8 && committedSamples < 1000000,
                "Interrupted accumulation has an invalid committed sample count");
        auto const interruptedPixels = Read<float>(continued.color);
        SetSampleBudget(continued, committedSamples);
        Render(continued, kInitialSize, kInitialSize, committedSamples);
        Require(Read<float>(continued.color) == interruptedPixels,
                "Stopping and holding the committed target changed accumulated pixels");

        const int resumedTarget = committedSamples + 4;
        SetSampleBudget(continued, resumedTarget);
        Render(continued, kInitialSize, kInitialSize, resumedTarget);
        auto const resumedPixels = Read<float>(continued.color);
        Fixture freshResumed;
        PrismRenderBuffer freshResumedSecond(SdfPath("/fresh_resumed_second"),
                                             &freshResumed.thread);
        AddScene(freshResumed);
        AddColorBinding(freshResumed, freshResumedSecond);
        SetSampleBudget(freshResumed, resumedTarget);
        Render(freshResumed, kInitialSize, kInitialSize, resumedTarget);
        Require(resumedPixels == Read<float>(freshResumed.color),
                "Interrupted accumulation resume differs from a fresh bridge at the committed target");
        RequireCoherentColorFrames(continued, continuedSecond);

        // Reallocation at a new size and replacement of a bound color buffer
        // both reset accumulation identity while preserving coherent AOVs.
        Fixture freshResized;
        PrismRenderBuffer freshResizedSecond(SdfPath("/fresh_resized_second"),
                                             &freshResized.thread);
        AddScene(freshResized);
        AddColorBinding(freshResized, freshResizedSecond);
        ResizeBuffers(freshResized, freshResizedSecond, kResizedSize);
        SetSampleBudget(freshResized, kContinuedSamples);
        Render(freshResized, kResizedSize, kResizedSize, kContinuedSamples);
        auto const resizedReference = Read<float>(freshResized.color);

        const uint64_t oldAllocation = continued.color.GetAllocationRevision();
        ResizeBuffers(continued, continuedSecond, kResizedSize);
        Require(continued.color.GetAllocationRevision() > oldAllocation,
                "Resizing did not replace render-buffer allocation identity");
        Prepare(continued, kResizedSize, kResizedSize);
        Require(Stat(continued.bridge->GetStats(), "completedSamples") == 0,
                "Resized AOV buffers retained the previous accumulation");
        SetSampleBudget(continued, kContinuedSamples);
        Render(continued, kResizedSize, kResizedSize, kContinuedSamples);
        Require(Read<float>(continued.color) == resizedReference,
                "Resized buffers differ from an independent 128-sample render");
        RequireCoherentColorFrames(continued, continuedSecond);

        PrismRenderBuffer replacement(SdfPath("/replacement_color"),
                                      &continued.thread);
        replacement.Allocate(GfVec3i(kResizedSize, kResizedSize, 1),
                             HdFormatFloat32Vec4, false);
        auto const oldBinding = continued.bindings.back();
        continued.bindings.back().renderBuffer = &replacement;
        Prepare(continued, kResizedSize, kResizedSize);
        Require(Stat(continued.bridge->GetStats(), "completedSamples") == 0,
                "Replacing a color buffer did not restart accumulation");
        Render(continued, kResizedSize, kResizedSize, kContinuedSamples);
        Require(Read<float>(replacement) == resizedReference,
                "Replacement color buffer did not begin with the fresh sample stream");
        Require(replacement.Capture().frame &&
                    replacement.Capture().frame == continued.color.Capture().frame &&
                    replacement.Capture().frame == continued.prim.Capture().frame,
                "Replacement buffer was not published with its peer AOVs");
        continued.bindings.back() = oldBinding;

        auto flag = [](VtDictionary const &stats, const char *name) {
            auto const found = stats.find(name);
            return found != stats.end() && found->second.IsHolding<bool>() &&
                found->second.UncheckedGet<bool>();
        };
        auto percentDone = [](VtDictionary const &stats) {
            auto const found = stats.find("percentDone");
            return found != stats.end() && found->second.IsHolding<double>()
                ? found->second.UncheckedGet<double>() : -1.0;
        };
        auto renderUntilConverged = [](Fixture &fixture) {
            Prepare(fixture, kInitialSize, kInitialSize);
            fixture.thread.StartRender();
            Wait([&] { return fixture.bridge->IsConverged() && fixture.color.IsConverged(); },
                 "Render did not converge");
            fixture.thread.StopRender();
        };

        // prism:timeLimitSeconds: a sample budget no render reaches stops at
        // the limit and reports convergence; raising the limit resumes the
        // same accumulation for the time added.
        Fixture timed;
        AddScene(timed);
        RenderSettings timedSettings = timed.bridge->GetSettings();
        timedSettings.samples = 1000000;
        timedSettings.timeLimitSeconds = 0.5;
        timed.bridge->SetSettings(timedSettings);
        renderUntilConverged(timed);
        VtDictionary const timedStats = timed.bridge->GetStats();
        const int64_t timedSamples = Stat(timedStats, "completedSamples");
        Require(flag(timedStats, "timeLimitReached") && timedSamples >= 1 && timedSamples < 1000000 &&
                    percentDone(timedStats) == 100.0,
                "The time limit did not stop the render as converged");
        RequireUsefulColor(Read<float>(timed.color));
        timedSettings.timeLimitSeconds = 1.0;
        timed.bridge->SetSettings(timedSettings);
        Require(!timed.bridge->IsConverged(), "Raising the time limit did not resume the render");
        renderUntilConverged(timed);
        Require(Stat(timed.bridge->GetStats(), "completedSamples") > timedSamples &&
                    flag(timed.bridge->GetStats(), "timeLimitReached"),
                "Raising the time limit did not add samples to the accumulation");

        // Adaptive sampling stops every pixel well before the maximum on this
        // plain scene and reports done; without a time limit the limit flag
        // stays off.
        Fixture adaptive;
        AddScene(adaptive);
        RenderSettings adaptiveSettings = adaptive.bridge->GetSettings();
        adaptiveSettings.samples = 100000;
        adaptiveSettings.adaptiveThreshold = 0.05f;
        adaptiveSettings.adaptiveMinSamples = 16;
        adaptive.bridge->SetSettings(adaptiveSettings);
        renderUntilConverged(adaptive);
        VtDictionary const adaptiveStats = adaptive.bridge->GetStats();
        Require(Stat(adaptiveStats, "completedSamples") >= 16 &&
                    Stat(adaptiveStats, "completedSamples") < 100000 &&
                    !flag(adaptiveStats, "timeLimitReached") && percentDone(adaptiveStats) == 100.0,
                "Adaptive sampling did not converge before the sample maximum");
        auto const activeFraction = adaptiveStats.find("activePixelFraction");
        Require(activeFraction != adaptiveStats.end() && activeFraction->second.IsHolding<double>() &&
                    activeFraction->second.UncheckedGet<double>() == 0.0,
                "Adaptive convergence left pixels active");

        // prism:subdivisionLevel in a running delegate: a Catmull-Clark quad
        // converts again at the new level (4^level quads).
        Fixture subdivided;
        AddScene(subdivided);
        subdivided.scene->AddPrims({{SdfPath("/Smooth"), HdPrimTypeTokens->mesh,
            HdOverlayContainerDataSource::New(
                HdRetainedContainerDataSource::New(HdMeshSchemaTokens->mesh,
                    HdMeshSchema::Builder().SetSubdivisionScheme(
                        HdRetainedTypedSampledDataSource<TfToken>::New(TfToken("catmullClark"))).Build()),
                Plane(0.3f))}});
        RenderSettings subdivisionSettings = subdivided.bridge->GetSettings();
        subdivisionSettings.subdivisionLevel = 0;
        subdivided.bridge->SetSettings(subdivisionSettings);
        renderUntilConverged(subdivided);
        const int64_t hullTriangles = Stat(subdivided.bridge->GetStats(), "triangleCount");
        subdivisionSettings.subdivisionLevel = 2;
        subdivided.bridge->SetSettings(subdivisionSettings);
        renderUntilConverged(subdivided);
        const int64_t refinedTriangles = Stat(subdivided.bridge->GetStats(), "triangleCount");
        // The hull is the same quad as /Mesh and shares its geometry (2
        // triangles in all); refined, /Smooth adds its 16 quads.
        if (!(hullTriangles == 2 && refinedTriangles == 2 + 32))
            throw std::runtime_error("Changing prism:subdivisionLevel did not refine the mesh again (triangles " +
                std::to_string(hullTriangles) + " -> " + std::to_string(refinedTriangles) + ")");

        std::cout << "Independent accumulation integration tests passed\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "Independent accumulation integration tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
