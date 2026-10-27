#include "integration_fixture.h"
#include "../../src/device/device.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Adaptive sampling stops tracing converged pixels, but every published
// buffer must still hold their accumulated values. The render buffers publish
// by swapping storages; a host that maps a buffer while it is published
// (usdview's presentation, every frame) keeps the storage it maps, and the
// next pass then starts from fresh, zero-filled storage. Converged pixels
// left unwritten there showed as alpha 0: usdview's clear colour in place of
// the dome once most pixels had converged. A host thread maps the buffers
// throughout the render, as usdview does, and checks every image it sees.
namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;

bool Available(prism::device::DeviceKind kind)
{
    for (auto const &info : prism::device::AvailableDevices(prism::RendererOptions::FromEnvironment().devices))
        if (info.kind == kind) return true;
    return false;
}

std::string StringStat(VtDictionary const &stats, const char *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<std::string>()
        ? it->second.UncheckedGet<std::string>() : std::string("<missing>");
}

// A cold GPU kernel compile can take far longer than the fixture's default wait.
template <class Predicate>
void WaitLong(Predicate ready, const char *reason)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct Outputs {
    std::vector<float> color, albedo, emission, depth, pworld;
    std::vector<int> prim;
    int64_t samples = 0;
};

struct Run {
    Fixture fixture;
    PrismRenderBuffer albedo{SdfPath("/albedo"), &fixture.thread};
    PrismRenderBuffer emission{SdfPath("/emission"), &fixture.thread};
    PrismRenderBuffer depth{SdfPath("/depth"), &fixture.thread};
    PrismRenderBuffer pworld{SdfPath("/pworld"), &fixture.thread};

    Run(prism::device::DevicePreference device, int samples, float threshold)
    {
        auto bind = [&](PrismRenderBuffer &buffer, TfToken const &name, HdFormat format,
                        VtValue clear) {
            buffer.Allocate(GfVec3i(Fixture::Size, Fixture::Size, 1), format, false);
            HdRenderPassAovBinding binding;
            binding.aovName = name;
            binding.renderBuffer = &buffer;
            binding.clearValue = clear;
            fixture.bindings.push_back(binding);
        };
        bind(albedo, TfToken("albedo"), HdFormatFloat32Vec3, VtValue(GfVec3f(0)));
        bind(emission, TfToken("emission"), HdFormatFloat32Vec3, VtValue(GfVec3f(0)));
        bind(depth, HdAovTokens->depth, HdFormatFloat32, VtValue(1.0f));
        bind(pworld, TfToken("Pworld"), HdFormatFloat32Vec3, VtValue(GfVec3f(0)));
        auto settings = fixture.bridge->GetSettings();
        settings.device = device;
        settings.samples = samples;
        settings.adaptiveThreshold = threshold;
        settings.adaptiveMinSamples = 4;
        fixture.bridge->SetSettings(settings);
        fixture.scene->AddPrims({{SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()},
                                 {SdfPath("/Mesh"), HdPrimTypeTokens->mesh, Plane()}});
    }

    // Renders to convergence; `during` runs while the render thread works.
    template <class During>
    Outputs Render(During &&during)
    {
        const TfTokenVector tags{HdRenderTagTokens->geometry};
        WaitLong([&] {
            fixture.bridge->Prepare(fixture.camera, fixture.bindings, tags,
                                    Fixture::Size, Fixture::Size);
            auto stats = fixture.bridge->GetStats();
            auto pending = stats.find("preparationPending");
            return pending != stats.end() && pending->second.IsHolding<bool>() &&
                !pending->second.UncheckedGet<bool>() &&
                Stat(stats, "displayedGeneration") >= Stat(stats, "preparationGeneration");
        }, "Adaptive publication fixture preparation timed out");
        fixture.thread.StartRender();
        during([&] { return fixture.color.IsConverged() && fixture.bridge->IsConverged(); });
        WaitLong([&] { return fixture.color.IsConverged() && fixture.bridge->IsConverged(); },
                 "Adaptive publication fixture render timed out");
        fixture.thread.StopRender();
        Outputs out;
        out.color = Read<float>(fixture.color);
        out.albedo = Read<float>(albedo);
        out.emission = Read<float>(emission);
        out.depth = Read<float>(depth);
        out.pworld = Read<float>(pworld);
        out.prim = Read<int>(fixture.prim);
        out.samples = Stat(fixture.bridge->GetStats(), "completedSamples");
        return out;
    }
};

// What a host sees: alpha 0 beside covered pixels (the dome covers every
// pixel), or a zero emission AOV where the camera sees the dome.
struct HostChecks {
    std::atomic<int> images{0}, badColor{0}, badEmission{0};
    std::atomic<int> worstUncovered{0};
    std::atomic<bool> emissionPublished{false};
};

void CheckMapped(PrismRenderBuffer &color, PrismRenderBuffer &emission,
                 std::vector<bool> const &dome, HostChecks &checks)
{
    const size_t pixels = size_t(Fixture::Size) * Fixture::Size;
    {
        auto const *rgba = static_cast<float const *>(color.Map());
        if (rgba) {
            int covered = 0, uncovered = 0;
            for (size_t p = 0; p < pixels; ++p)
                (rgba[4 * p + 3] == 0.0f ? uncovered : covered) += 1;
            // All alpha 0 is the clear value before the first publication.
            if (covered && uncovered) {
                checks.badColor.fetch_add(1);
                int worst = checks.worstUncovered.load();
                while (uncovered > worst && !checks.worstUncovered.compare_exchange_weak(worst, uncovered)) {}
            }
            checks.images.fetch_add(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        color.Unmap();
    }
    auto const *rgb = static_cast<float const *>(emission.Map());
    if (rgb) {
        int zero = 0, lit = 0;
        for (size_t p = 0; p < pixels; ++p) {
            if (!dome[p]) continue;
            (rgb[3 * p] == 0.0f && rgb[3 * p + 1] == 0.0f && rgb[3 * p + 2] == 0.0f ? zero : lit) += 1;
        }
        // The dome's pixels converge together: once a pass has published
        // them, no later image may show them zero (the clear value).
        if (zero && (lit || checks.emissionPublished.load())) checks.badEmission.fetch_add(1);
        if (lit && !zero) checks.emissionPublished.store(true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    emission.Unmap();
}

void RunDevice(prism::device::DevicePreference device, const char *name)
{
    // Reference: a fixed sample count without adaptive sampling.
    Outputs reference;
    {
        Run run(device, 16, 0.0f);
        reference = run.Render([](auto const &) {});
        Require(reference.samples == 16, "The reference render stopped at the wrong sample count");
        const std::string used = StringStat(run.fixture.bridge->GetStats(), "prismDevice");
        Require(used.rfind(name, 0) == 0, "The render did not run on the requested device");
    }
    const size_t pixels = size_t(Fixture::Size) * Fixture::Size;
    // Interior pixels: their 3x3 window shows one prim, so no sample of
    // theirs crosses the plane's edge (their values do not depend on the
    // sample count).
    const int size = static_cast<int>(Fixture::Size);
    auto interior = [&](int x, int y) {
        for (int ny = std::max(0, y - 1); ny <= std::min(size - 1, y + 1); ++ny)
            for (int nx = std::max(0, x - 1); nx <= std::min(size - 1, x + 1); ++nx)
                if (reference.prim[size_t(ny) * size + nx] != reference.prim[size_t(y) * size + x])
                    return false;
        return true;
    };
    std::vector<bool> background(pixels), inside(pixels);
    size_t planePixels = 0, backgroundPixels = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const size_t p = size_t(y) * size + x;
            inside[p] = interior(x, y);
            // The dome seen directly: the emission AOV is not zero there.
            background[p] = reference.prim[p] < 0 && inside[p] && reference.emission[3 * p] > 0.0f;
            planePixels += reference.prim[p] >= 0;
            backgroundPixels += background[p];
        }
    Require(planePixels > 0 && backgroundPixels > 0,
            "The fixture does not show both the plane and the dome");

    // Adaptive: the dome's pixels converge at the first check, the plane's
    // stay active for many passes; a host maps the outputs all along.
    Run run(device, 256, 0.01f);
    HostChecks checks;
    const Outputs adaptive = run.Render([&](auto const &converged) {
        while (!converged())
            CheckMapped(run.fixture.color, run.emission, background, checks);
    });
    std::cout << name << ": " << adaptive.samples << " samples, " << checks.images.load()
              << " mapped images, " << checks.badColor.load() << " with uncovered pixels (worst "
              << checks.worstUncovered.load() << "), " << checks.badEmission.load()
              << " with lost emission\n";
    Require(adaptive.samples > 4 + 8,
            "Adaptive sampling did not continue past the first convergence check");
    Require(checks.images.load() > 0, "The host saw no published image");
    Require(checks.badColor.load() == 0,
            "A published colour image lost the alpha of converged pixels");
    Require(checks.badEmission.load() == 0,
            "A published emission image lost the values of converged pixels");

    // The final images: converged pixels keep their colour, alpha and AOVs.
    for (size_t p = 0; p < pixels; ++p) {
        Require(adaptive.color[4 * p + 3] == reference.color[4 * p + 3],
                "A converged pixel's alpha differs from the reference");
        auto close = [](float value, float expected) {
            return std::abs(value - expected) <= 1e-4f * std::max(1.0f, std::abs(expected));
        };
        for (int c = 0; background[p] && c < 3; ++c) {
            Require(close(adaptive.color[4 * p + c], reference.color[4 * p + c]),
                    "A converged dome pixel's colour differs from the reference");
            Require(close(adaptive.emission[3 * p + c], reference.emission[3 * p + c]),
                    "A converged dome pixel's emission AOV differs from the reference");
        }
        for (int c = 0; inside[p] && c < 3; ++c)
            Require(std::abs(adaptive.albedo[3 * p + c] - reference.albedo[3 * p + c]) <= 1e-3f,
                    "A converged pixel's albedo differs from the reference");
    }
    Require(adaptive.depth == reference.depth && adaptive.pworld == reference.pworld &&
                adaptive.prim == reference.prim,
            "Adaptive sampling changed the depth, Pworld or primId outputs");
}

} // namespace

int RunAdaptivePublicationTests(std::string const &device)
{
    try {
        if (device == "vulkan" || device == "metal") {
            const bool vulkan = device == "vulkan";
            if (!Available(vulkan ? prism::device::DeviceKind::Vulkan : prism::device::DeviceKind::Metal)) {
                std::cout << "Adaptive publication checks skipped: no " << device << " device\n";
                return 0;
            }
            RunDevice(vulkan ? prism::device::DevicePreference::Vulkan
                             : prism::device::DevicePreference::Metal, device.c_str());
        } else {
            RunDevice(prism::device::DevicePreference::Cpu, "cpu");
        }
        std::cout << "Adaptive publication tests passed (" << device << ")\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "Adaptive publication tests failed (" << device << "): "
                  << error.what() << '\n';
        return 1;
    }
}
