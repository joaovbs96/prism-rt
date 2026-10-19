#include "integration_fixture.h"
#include "../../src/device/device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// A GPU device that is still preparing (kernel compile, upload) is covered by
// a CPU preview that hands over to the GPU at a pass boundary. The test
// stretches the preparation with PRISM_TEST_GPU_PREPARATION_DELAY_MS and checks
// that samples are published before the GPU is ready, that the handover
// happens exactly once without restarting accumulation, and that the final
// image matches a GPU-only render within noise.
namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;

void SetEnvironment(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool VulkanAvailable()
{
    for (auto const &info : prism::device::AvailableDevices())
        if (info.kind == prism::device::DeviceKind::Vulkan) return true;
    return false;
}

std::string StringStat(VtDictionary const &stats, const char *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<std::string>()
        ? it->second.UncheckedGet<std::string>() : std::string("<missing>");
}

bool BoolStat(VtDictionary const &stats, const char *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<bool>() && it->second.UncheckedGet<bool>();
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

void AddScene(Fixture &fixture, int samples, bool preview)
{
    auto settings = fixture.bridge->GetSettings();
    settings.device = prism::device::DevicePreference::Vulkan;
    settings.samples = samples;
    settings.gpuPreparationPreview = preview;
    fixture.bridge->SetSettings(settings);
    fixture.scene->AddPrims({{SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()},
                             {SdfPath("/Mesh"), HdPrimTypeTokens->mesh, Plane()}});
}

std::vector<float> Colors(Fixture &fixture)
{
    return Read<float>(fixture.color);
}

} // namespace

int RunGpuPreparationPreviewTests()
{
    try {
        std::cerr << "gpu preparation preview test start\n";
        if (!VulkanAvailable()) {
            std::cout << "GPU preparation preview checks skipped: no Vulkan device\n";
            return 0;
        }
        SetEnvironment("PRISM_GPU_PREPARATION_PREVIEW_DELAY_MS", "100");

        // Reference: the GPU alone (no preview, no delay).
        SetEnvironment("PRISM_TEST_GPU_PREPARATION_DELAY_MS", "");
        constexpr int kSamples = 3000;
        std::vector<float> reference;
        {
            Fixture fixture;
            AddScene(fixture, kSamples, false);
            fixture.Prepare();
            fixture.thread.StartRender();
            WaitLong([&] { return fixture.color.IsConverged() && fixture.bridge->IsConverged(); },
                     "GPU-only reference render timed out");
            fixture.thread.StopRender();
            const auto stats = fixture.bridge->GetStats();
            Require(StringStat(stats, "prismDevice").rfind("vulkan", 0) == 0,
                    "The reference render did not run on the Vulkan device");
            Require(Stat(stats, "gpuPreviewHandovers") == 0 && !BoolStat(stats, "prismGpuPreview"),
                    "A GPU-only render reported a CPU preview");
            reference = Colors(fixture);
        }

        // With the preview disabled nothing is published until the GPU is ready.
        SetEnvironment("PRISM_TEST_GPU_PREPARATION_DELAY_MS", "600");
        {
            Fixture fixture;
            AddScene(fixture, 16, false);
            fixture.Prepare();
            fixture.thread.StartRender();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            auto stats = fixture.bridge->GetStats();
            Require(Stat(stats, "completedSamples") == 0 && !BoolStat(stats, "prismGpuPreview"),
                    "Samples were published before the GPU was ready with the preview disabled");
            WaitLong([&] { return fixture.color.IsConverged() && fixture.bridge->IsConverged(); },
                     "Render without the preview timed out");
            fixture.thread.StopRender();
            Require(Stat(fixture.bridge->GetStats(), "gpuPreviewHandovers") == 0,
                    "A disabled preview handed over");
        }

        // With the preview, the CPU renders while the GPU preparation is held back.
        SetEnvironment("PRISM_TEST_GPU_PREPARATION_DELAY_MS", "2500");
        std::vector<float> handover;
        {
            Fixture fixture;
            AddScene(fixture, kSamples, true);
            fixture.Prepare();
            fixture.thread.StartRender();
            WaitLong([&] {
                auto stats = fixture.bridge->GetStats();
                return BoolStat(stats, "prismGpuPreview") && Stat(stats, "completedSamples") > 0;
            }, "The CPU preview never published samples");
            auto stats = fixture.bridge->GetStats();
            Require(Stat(stats, "devicePreparations") == 0,
                    "Samples were counted before the GPU preparation started");
            Require(StringStat(stats, "prismDevice") == "cpu (preview while the GPU prepares)",
                    "The preview did not name itself in the device statistic");
            const auto previewSamples = Stat(stats, "completedSamples");
            const auto previewPixels = Colors(fixture);
            Require(std::any_of(previewPixels.begin(), previewPixels.end(),
                                [](float v) { return v > 0.0f; }),
                    "The CPU preview left the buffer empty");

            WaitLong([&] { return Stat(fixture.bridge->GetStats(), "gpuPreviewHandovers") >= 1; },
                     "The render never handed over to the GPU");
            stats = fixture.bridge->GetStats();
            const auto handoverSample = Stat(stats, "gpuPreviewHandoverSample");
            Require(handoverSample >= previewSamples && handoverSample > 0 && handoverSample < kSamples,
                    "The handover did not continue from the CPU's sample index");
            Require(Stat(stats, "gpuPreviewHandovers") == 1, "More than one handover");
            Require(!BoolStat(stats, "prismGpuPreview") &&
                    StringStat(stats, "prismDevice").rfind("vulkan", 0) == 0,
                    "The device statistic did not switch to Vulkan");
            // Accumulation continues: the count never drops back to zero.
            Require(Stat(stats, "completedSamples") >= handoverSample,
                    "The sample count went backwards at the handover");
            WaitLong([&] { return fixture.color.IsConverged() && fixture.bridge->IsConverged(); },
                     "The handed-over render timed out");
            fixture.thread.StopRender();
            stats = fixture.bridge->GetStats();
            Require(Stat(stats, "completedSamples") == kSamples, "The handed-over render lost samples");
            Require(Stat(stats, "devicePreparations") == 1, "The GPU was prepared more than once");
            handover = Colors(fixture);
        }

        // Same picture as the GPU alone, up to noise.
        Require(handover.size() == reference.size() && !handover.empty(), "Image sizes differ");
        double sumSquares = 0.0, sumReference = 0.0, sumHandover = 0.0;
        for (size_t i = 0; i < handover.size(); i += 4) {
            for (size_t c = 0; c < 3; ++c) {
                const double a = handover[i + c], b = reference[i + c];
                sumSquares += (a - b) * (a - b);
                sumHandover += a;
                sumReference += b;
            }
        }
        const double pixels = double(handover.size() / 4) * 3.0;
        const double rmse = std::sqrt(sumSquares / pixels);
        const double mean = std::max(sumReference / pixels, 1.0e-6);
        const double meanError = std::abs(sumHandover - sumReference) / pixels / mean;
        std::cerr << "handover vs GPU-only: rmse " << rmse / mean << " (relative), mean error "
                  << meanError << "\n";
        Require(rmse / mean < 0.05, "The handed-over image differs from the GPU render beyond noise");
        Require(meanError < 0.01, "The handed-over image has a different mean than the GPU render");

        SetEnvironment("PRISM_TEST_GPU_PREPARATION_DELAY_MS", "");
        SetEnvironment("PRISM_GPU_PREPARATION_PREVIEW_DELAY_MS", "");
        std::cout << "GPU preparation preview checks passed\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "GPU preparation preview test failed: " << e.what() << "\n";
        return 1;
    }
}
