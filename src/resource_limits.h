#pragma once

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "renderer/runtime.h"

namespace prism {

// The root of the persistent caches: the runtime's (the first renderer's
// RendererOptions::cacheDirectory, else the platform's cache directory).
inline std::filesystem::path PersistentCacheRoot() {
    return Runtime::Instance().settings().CacheRoot();
}

// 20 GiB: a prepared texture now has a GPU-ready copy beside its float
// .tx.exr (about half as large again), and the OpenPBR playground alone
// needs 13 GiB with both; a budget below a scene's working set evicts, at
// the start of every warm load, files that load then prepares again.
// The runtime's disk budget (Limits::diskCacheGiB).
inline uint64_t PersistentDiskBudgetBytes(double gib) {
    return std::max<uint64_t>(1, static_cast<uint64_t>(gib * 1024.0 * 1024.0 * 1024.0));
}
inline uint64_t PersistentDiskBudgetBytes() {
    return PersistentDiskBudgetBytes(Runtime::Instance().settings().diskCacheGiB);
}

// Separate recency queues share one allowance instead of independently claiming
// the entire disk budget. Live texture pins may temporarily exceed their share.
// Resolver-materialized source files are reported separately; these functions
// currently budget prepared textures and generated-source artifacts only.
inline uint64_t GeneratedArtifactBudgetBytes(uint64_t diskBudget) {
    return std::min<uint64_t>(256ull * 1024 * 1024, diskBudget / 16);
}
inline uint64_t TextureArtifactBudgetBytes(uint64_t diskBudget) {
    return diskBudget - GeneratedArtifactBudgetBytes(diskBudget);
}
inline uint64_t GeneratedArtifactBudgetBytes() {
    return GeneratedArtifactBudgetBytes(PersistentDiskBudgetBytes());
}

// Admission accounting is deliberately separate from actual resident memory.
// SDK allocations are opaque; these reservations limit simultaneous expensive
// work, not the geometry and resources already pinned by a published scene.
class PreparationAdmission {
public:
    class Reservation {
    public:
        Reservation(PreparationAdmission& owner, uint64_t estimate) : owner(owner) {
            const auto started = std::chrono::steady_clock::now();
            std::unique_lock lock(owner.mutex);
            bytes = std::max(estimate, owner.observedEstimate);
            owner.changed.wait(lock, [&] {
                return owner.active == 0 ||
                    (bytes <= owner.limit && owner.used <= owner.limit - bytes);
            });
            owner.waitNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
            if (bytes > owner.limit)
                std::cerr << "PrismRT: preparation job exceeds admission budget; running alone\n";
            ++owner.active;
            owner.used += bytes;
            owner.peak = std::max(owner.peak, owner.used);
        }
        ~Reservation() {
            { std::lock_guard lock(owner.mutex); owner.used -= bytes; --owner.active; }
            owner.changed.notify_all();
        }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        void observe(uint64_t measuredBytes) {
            std::lock_guard lock(owner.mutex);
            owner.observedEstimate = std::max(owner.observedEstimate, measuredBytes);
        }
    private:
        PreparationAdmission& owner;
        uint64_t bytes = 0;
    };
    // Process-wide (the runtime's): its budget is the largest any renderer
    // asked for (Limits::preparationMiB).
    static PreparationAdmission& Shared() { static PreparationAdmission value; return value; }
    uint64_t peakReservedBytes() const { std::lock_guard lock(mutex); return peak; }
    uint64_t admissionWaitNanoseconds() const {
        std::lock_guard lock(mutex); return waitNanoseconds;
    }
    // Raises the budget to `bytes` if it is lower.
    void RaiseLimit(uint64_t bytes) {
        { std::lock_guard lock(mutex); limit = std::max(limit, bytes); }
        changed.notify_all();
    }
private:
    PreparationAdmission() : limit(Runtime::Instance().settings().preparationMiB * 1024 * 1024) {}
    mutable std::mutex mutex;
    std::condition_variable changed;
    uint64_t limit, used = 0, peak = 0, active = 0, observedEstimate = 0;
    uint64_t waitNanoseconds = 0;
};

// Measured whole-process peak (~2 GB) is far below the former 512 MB per-job estimate; these match observed use.
inline constexpr uint64_t kMaterialCompileReservationBytes = 192ull * 1024ull * 1024ull;
// A template bind performs no native JIT, so it needs far less than a full compile.
inline constexpr uint64_t kMaterialBindReservationBytes = 64ull * 1024ull * 1024ull;

// Material compile workers and texture preparation threads per batch:
// Limits::MaterialWorkers and Limits::TexturePreparationThreads
// (renderer/renderer_options.h). One preparation of a W x H image with C
// channels holds the decoded pixels as floats (4WHC bytes), a second such
// buffer while a per-pixel conversion runs, and make_texture's mip chain (a
// third more) plus tile buffers: at most about 2.5 x 4WHC. That is ~120 MiB
// for a 2048x2048 RGB texture (~1 GiB for eight) and ~640 MiB for a
// 4096x4096 RGBA one. The source file copy made while materializing it is
// small by comparison.

} // namespace prism
