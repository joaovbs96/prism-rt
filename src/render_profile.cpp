// The render profile's process state (src/render_profile.h): one registry of
// per-thread counters in the core library.

#include "render_profile.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace prism::profiling {
namespace {

struct Registry {
    std::mutex mutex;
    // Counters live as long as the process: a thread that exits leaves its
    // sums for the report.
    std::vector<std::unique_ptr<Counters>> threads;
};

Registry& GetRegistry()
{
    static auto* registry = new Registry;  // leaked: threads may outlive statics
    return *registry;
}

thread_local Counters* t_counters = nullptr;

} // namespace

char const* StageName(Stage stage)
{
    switch (stage) {
    case Sample: return "sample (total)";
    case Intersection: return "intersection";
    case HitPreparation: return "hit preparation";
    case LightChoice: return "NEE light choice";
    case ShadowRay: return "NEE shadow ray";
    case ShadowIntersection: return "shadow intersection";
    case ShadowHitPreparation: return "shadow hit preparation";
    case Cutout: return "shadow cutout";
    case MaterialPrepare: return "MDL prepare (init)";
    case MaterialSample: return "MDL sample";
    case MaterialEvaluate: return "MDL evaluate";
    case MaterialMedium: return "MDL medium";
    case Emission: return "emitter radiance";
    case TextureLookup: return "texture lookup";
    case BsdfDataLookup: return "BSDF table lookup";
    case Environment: return "environment";
    case Film: return "film";
    case AdaptiveCheck: return "adaptive check";
    case Denoise: return "denoise";
    case StageCount: break;
    }
    return "?";
}

bool Enabled()
{
    static const bool enabled = [] {
        char const* value = std::getenv("PRISM_RENDER_PROFILE");
        return value && *value && std::string(value) != "0";
    }();
    return enabled;
}

Counters* ThreadCounters()
{
    if (t_counters) return t_counters;
    auto counters = std::make_unique<Counters>();
    t_counters = counters.get();
    Registry& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    registry.threads.push_back(std::move(counters));
    return t_counters;
}

void Reset()
{
    Registry& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    for (auto const& counters : registry.threads)
        for (int stage = 0; stage < StageCount; ++stage) {
            counters->calls[stage].store(0, std::memory_order_relaxed);
            counters->nanoseconds[stage].store(0, std::memory_order_relaxed);
        }
}

std::string Report(double wallSeconds)
{
    if (!Enabled()) return {};
    std::array<uint64_t, StageCount> calls{}, nanoseconds{};
    {
        Registry& registry = GetRegistry();
        std::lock_guard lock(registry.mutex);
        for (auto const& counters : registry.threads)
            for (int stage = 0; stage < StageCount; ++stage) {
                calls[stage] += counters->calls[stage].load(std::memory_order_relaxed);
                nanoseconds[stage] += counters->nanoseconds[stage].load(std::memory_order_relaxed);
            }
    }
    bool any = false;
    for (uint64_t count : calls) any |= count != 0;
    if (!any) return {};
    const double sampleSeconds = double(nanoseconds[Sample]) * 1e-9;
    std::ostringstream out;
    out << "Render profile (summed worker elapsed time; stages nest, shares are of the sample total";
    if (wallSeconds > 0.0) {
        char wall[64];
        std::snprintf(wall, sizeof(wall), "; render wall time %.3f s", wallSeconds);
        out << wall;
    }
    out << "):\n";
    char line[160];
    std::snprintf(line, sizeof(line), "  %-24s %14s %12s %10s %8s\n", "stage", "calls", "seconds", "ns/call", "share");
    out << line;
    for (int stage = 0; stage < StageCount; ++stage) {
        if (!calls[stage]) continue;
        const double seconds = double(nanoseconds[stage]) * 1e-9;
        const double perCall = double(nanoseconds[stage]) / double(calls[stage]);
        const double share = sampleSeconds > 0.0 ? 100.0 * seconds / sampleSeconds : 0.0;
        std::snprintf(line, sizeof(line), "  %-24s %14llu %12.3f %10.0f %7.1f%%\n", StageName(Stage(stage)),
                      static_cast<unsigned long long>(calls[stage]), seconds, perCall, share);
        out << line;
    }
    return out.str();
}

} // namespace prism::profiling
