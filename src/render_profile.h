#pragma once

#include <array>
#include <chrono>
#include <cstdint>

// Opt-in task-local instrumentation. Ordinary rendering leaves active null;
// only benchmark passes read the clock. Sums are worker elapsed time, not CPU
// time or wall time; scheduling and clock overhead affect these measurements.
namespace prism::profiling {
enum Stage {
    Intersection, HitPreparation, MaterialSample, MaterialEvaluate,
    ShadowIntersection, ShadowHitPreparation, Cutout, Environment
};
struct Counters {
    std::array<uint64_t, 8> calls{};
    std::array<uint64_t, 8> nanoseconds{};
};
inline thread_local Counters* active = nullptr;
inline thread_local bool shadow = false;

class Scope {
public:
    explicit Scope(Stage stage) : counters(active), stage(stage) {
        if (counters) start = Clock::now();
    }
    ~Scope() { stop(); }
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;
    void stop() {
        if (!counters) return;
        counters->nanoseconds[stage] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start).count());
        ++counters->calls[stage];
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
    ShadowScope() : previous(shadow) { shadow = true; }
    ~ShadowScope() { shadow = previous; }
private:
    bool previous;
};
} // namespace prism::profiling
