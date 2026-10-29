#include "integration_fixture.h"
#include "../../src/progress.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

void SetEnvironment(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::string StringStat(pxr::VtDictionary const &stats, const char *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<std::string>()
        ? it->second.UncheckedGet<std::string>() : std::string("<missing>");
}

int IntStat(pxr::VtDictionary const &stats, const char *name)
{
    auto it = stats.find(name);
    return it != stats.end() && it->second.IsHolding<int>()
        ? it->second.UncheckedGet<int>() : -1;
}

} // namespace

int RunProgressStatusTests()
{
    using namespace pxr;
    using namespace prism::hydra::test;
    try {
        std::cerr << "progress status test start\n";
        {
            prism::Progress model;
            auto start = model.Read();
            Require(start.phase == "idle" && start.device.empty(), "A fresh model is not idle");
            model.Begin(prism::Phase::Loading);
            model.Begin(prism::Phase::Materials);
            model.SetTotal(prism::Phase::Materials, 3);
            model.Add(prism::Phase::Materials);
            model.Begin(prism::Phase::Textures);
            auto overlap = model.Read();
            Require(overlap.phase == "materials" && overlap.phases[size_t(prism::Phase::Materials)].done == 1 &&
                    overlap.phases[size_t(prism::Phase::Materials)].total == 3 &&
                    overlap.phases[size_t(prism::Phase::Loading)].active &&
                    overlap.phases[size_t(prism::Phase::Textures)].active,
                    "Overlapping phases were not reported with the later stage first");
            model.End(prism::Phase::Materials);
            model.End(prism::Phase::Textures);
            model.Begin(prism::Phase::Rendering);
            Require(model.Read().phase == "loading", "Loading must outrank rendering");
            model.End(prism::Phase::Loading);
            Require(model.Read().phase == "rendering", "A finished load did not leave rendering");
            model.Begin(prism::Phase::Kernel);
            model.Begin(prism::Phase::Upload);
            model.SetDetail(prism::Phase::Kernel, "12 MDL programs");
            auto nested = model.Read();
            Require(nested.phase == "kernel" && nested.detail == "12 MDL programs",
                    "A kernel compile nested in an upload must be the primary phase");
            model.SetError("boom");
            Require(model.Read().phase == "error", "An error did not take over the phase");
            // Counters stay readable after a phase ends and reset on the next Begin.
            model.End(prism::Phase::Materials);
            Require(model.Read().phases[size_t(prism::Phase::Materials)].done == 1,
                    "Counters vanished with their phase");
            model.Begin(prism::Phase::Materials);
            Require(model.Read().phases[size_t(prism::Phase::Materials)].done == 0,
                    "A new phase did not reset its counters");
            // GPU work that starts while materials compile (kernel and upload
            // are active) does not hide them.
            model.SetError("");
            Require(model.Read().phase == "materials",
                    "A kernel compile overlapping the material phase took precedence over it");
            model.End(prism::Phase::Materials);
            // Overlapping counted holds end the phase with the last one.
            model.End(prism::Phase::Kernel);
            model.Hold(prism::Phase::Kernel);
            model.Hold(prism::Phase::Kernel);
            model.Release(prism::Phase::Kernel);
            Require(model.IsActive(prism::Phase::Kernel), "A counted phase ended with its first holder");
            model.Release(prism::Phase::Kernel);
            Require(!model.IsActive(prism::Phase::Kernel), "A counted phase outlived its holders");
        }

        // Frontend: statistics progress to rendering/converged and name the device.
        // The fallback case disables Vulkan and then restores the caller's
        // setting (a CPU-only run keeps Vulkan disabled for the later checks).
        const char *disabledVulkan = std::getenv("PRISM_DISABLE_VULKAN");
        const std::string callerDisabledVulkan = disabledVulkan ? disabledVulkan : "";
        for (int fallback = 0; fallback < 2; ++fallback) {
            if (fallback) SetEnvironment("PRISM_DISABLE_VULKAN", "1");
            Fixture fixture;
            auto settings = fixture.bridge->GetSettings();
            settings.device = fallback ? prism::device::DevicePreference::Vulkan
                                       : prism::device::DevicePreference::Cpu;
            settings.samples = 8;
            fixture.bridge->SetSettings(settings);
            fixture.scene->AddPrims({{SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()},
                                     {SdfPath("/Mesh"), HdPrimTypeTokens->mesh, Plane()}});

            // A poller uses only the lock-free progress statistics, as a host's
            // UI thread does, and records every phase it sees.
            std::atomic<bool> polling{true};
            std::vector<std::string> seen;
            std::set<std::string> devices;
            std::thread poller([&] {
                while (polling.load()) {
                    const auto stats = fixture.bridge->GetProgressStats();
                    const std::string phase = StringStat(stats, "prismPhase");
                    if (seen.empty() || seen.back() != phase) seen.push_back(phase);
                    devices.insert(StringStat(stats, "prismDevice"));
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
            fixture.Render();
            polling = false;
            poller.join();
            {
                const std::string last = StringStat(fixture.bridge->GetProgressStats(), "prismPhase");
                if (seen.empty() || seen.back() != last) seen.push_back(last);
            }

            static const std::set<std::string> valid{"idle", "device", "loading", "materials",
                "textures", "kernel", "upload", "rendering"};
            for (auto const &phase : seen)
                Require(valid.count(phase), "Unexpected prismPhase value");
            Require(!seen.empty(), "The poller observed nothing");
            const auto stats = fixture.bridge->GetStats();
            Require(StringStat(stats, "prismPhase") == "rendering",
                    "The renderer did not end in the rendering phase");
            Require(std::find(seen.begin(), seen.end(), "rendering") != seen.end(),
                    "The rendering phase was never observed");
            Require(seen.back() == "rendering", "A phase was reported after rendering began");
            // Loading precedes rendering when it was observed at all.
            auto rendering = std::find(seen.begin(), seen.end(), "rendering");
            Require(std::find(rendering, seen.end(), "loading") == seen.end(),
                    "Loading was reported after rendering began");
            Require(IntStat(stats, "prismMeshesTotal") >= 1 &&
                    IntStat(stats, "prismMeshesDone") == IntStat(stats, "prismMeshesTotal"),
                    "Mesh conversion counters did not complete");
            Require(IntStat(stats, "numCompletedSamples") == 8 &&
                    stats.find("converged")->second.UncheckedGet<bool>(),
                    "The fixture did not converge");
            Require(StringStat(stats, "prismError").empty(), "Unexpected prismError");
            const std::string device = StringStat(stats, "prismDevice");
            if (fallback)
                Require(device.rfind("cpu (Vulkan fallback: ", 0) == 0,
                        "A failed Vulkan device was not reported as a fallback");
            else
                Require(device == "cpu", "The CPU device was misnamed");
            if (fallback) SetEnvironment("PRISM_DISABLE_VULKAN", callerDisabledVulkan.c_str());
        }
        std::cout << "Progress status checks passed\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "Progress status test failed: " << e.what() << "\n";
        return 1;
    }
}
