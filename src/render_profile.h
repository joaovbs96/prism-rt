#pragma once

#include "internal_export.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// The render profile (PRISM_RENDER_PROFILE=1, docs/performance.md): per-stage
// call counts and summed worker elapsed time of the CPU render loops. Off,
// every scope is one thread-local null check and the images are unchanged.
// Sums are worker elapsed time, not CPU or wall time; stages nest (a texture
// lookup inside a material prepare counts in both), and the clocks add their
// own cost to the stages they time.
namespace prism::profiling {

enum Stage : int {
    Sample,               // one path sample of a pixel (everything below)
    Intersection,         // closest-hit Embree query of a path segment
    HitPreparation,       // RayHit from the Embree hit (prepareHit)
    LightChoice,          // NEE: the light pick and the light sample
    ShadowRay,            // NEE: the whole shadow-ray test (visible)
    ShadowIntersection,   // Embree queries of shadow rays
    ShadowHitPreparation, // prepareHit of shadow-ray crossings
    Cutout,               // cutout opacity of a shadow-ray crossing
    MaterialPrepare,      // MDL shading state and init (texture results)
    MaterialSample,       // MDL BSDF sample (with cutout, thin-walled, emission)
    MaterialEvaluate,     // MDL BSDF evaluate
    MaterialMedium,       // MDL volume coefficients
    Emission,             // emitter radiance at a light sample (MDL init + EDF)
    TextureLookup,        // MDL 2D texture lookups (OIIO)
    BsdfDataLookup,       // MDL BSDF data-table lookups
    Environment,          // dome radiance, sampling and lobes
    Film,                 // accumulation into the image after the samples
    AdaptiveCheck,        // adaptive sampling convergence checks
    Denoise,              // denoising
    StageCount
};

PRISM_INTERNAL_API char const* StageName(Stage stage);

struct Counters {
    std::array<std::atomic<uint64_t>, StageCount> calls{};
    std::array<std::atomic<uint64_t>, StageCount> nanoseconds{};
    // Written by the owning thread only; read by Report.
    void Add(Stage stage, uint64_t ns)
    {
        nanoseconds[stage].store(nanoseconds[stage].load(std::memory_order_relaxed) + ns,
                                 std::memory_order_relaxed);
        calls[stage].store(calls[stage].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};

// The counters this thread records into, or null (profiling off, or a thread
// outside the render loops). Each module has its own copy; only the core's
// render loops set it (Attach), so only core code records.
inline thread_local Counters* active = nullptr;
// Inside a shadow-ray test (ShadowScope): intersections count as shadow ones.
inline thread_local bool shadow = false;

// Whether PRISM_RENDER_PROFILE is set (read once per process).
PRISM_INTERNAL_API bool Enabled();
// This thread's counters, created and registered on first use.
PRISM_INTERNAL_API Counters* ThreadCounters();
// Clears every thread's counters.
PRISM_INTERNAL_API void Reset();
// The table of every stage with calls since the last Reset; empty when
// profiling is off or nothing was recorded. `wallSeconds` (optional) adds
// the render's wall time for reference.
PRISM_INTERNAL_API std::string Report(double wallSeconds = 0.0);

// Records on this thread for its lifetime when profiling is on: a render
// loop's task body or a worker job.
class Attach {
public:
    Attach() : previous(active)
    {
        if (!previous && Enabled()) active = ThreadCounters();
    }
    ~Attach() { active = previous; }
    Attach(Attach const&) = delete;
    Attach& operator=(Attach const&) = delete;
private:
    Counters* previous;
};

class Scope {
public:
    explicit Scope(Stage stage) : counters(active), stage(stage)
    {
        if (counters) start = Clock::now();
    }
    // `inShadow` while a ShadowScope is open.
    Scope(Stage stage, Stage inShadow) : counters(active), stage(stage)
    {
        if (!counters) return;
        if (shadow) this->stage = inShadow;
        start = Clock::now();
    }
    ~Scope() { stop(); }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
    void stop()
    {
        if (!counters) return;
        counters->Add(stage, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
        counters = nullptr;
    }
private:
    using Clock = std::chrono::steady_clock;
    Counters* counters;
    Stage stage;
    Clock::time_point start;
};

class ShadowScope {
public:
    ShadowScope() : recording(active != nullptr), previous(false)
    {
        if (!recording) return;
        previous = shadow;
        shadow = true;
    }
    ~ShadowScope()
    {
        if (recording) shadow = previous;
    }
    ShadowScope(ShadowScope const&) = delete;
    ShadowScope& operator=(ShadowScope const&) = delete;
private:
    bool recording;
    bool previous;
};

} // namespace prism::profiling
