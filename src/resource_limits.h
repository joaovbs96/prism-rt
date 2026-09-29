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

namespace prism {

inline std::filesystem::path PersistentCacheRoot() {
    if (const char* value = std::getenv("PRISM_CACHE_DIR"); value && *value)
        return value;
    if (const char* value = std::getenv("LOCALAPPDATA"); value && *value)
        return std::filesystem::path(value) / "PrismRT" / "cache";
    return std::filesystem::temp_directory_path() / "PrismRT" / "cache";
}

inline uint64_t PersistentDiskBudgetBytes() {
    double gib = 10.0;
    if (const char* value = std::getenv("PRISM_DISK_CACHE_GB"); value && *value) {
        try {
            size_t consumed = 0;
            const double parsed = std::stod(value, &consumed);
            if (consumed == std::string(value).size() && parsed > 0 && std::isfinite(parsed))
                gib = std::min(parsed, 1024.0);
        } catch (...) { /* Invalid overrides use the documented default. */ }
    }
    return std::max<uint64_t>(1, static_cast<uint64_t>(gib * 1024.0 * 1024.0 * 1024.0));
}

// Separate recency queues share one allowance instead of independently claiming
// the entire disk budget. Live texture pins may temporarily exceed their share.
// Resolver-materialized source files are reported separately; these functions
// currently budget prepared textures and generated-source artifacts only.
inline uint64_t GeneratedArtifactBudgetBytes() {
    return std::min<uint64_t>(256ull * 1024 * 1024, PersistentDiskBudgetBytes() / 16);
}
inline uint64_t TextureArtifactBudgetBytes() {
    return PersistentDiskBudgetBytes() - GeneratedArtifactBudgetBytes();
}

inline uint64_t ResourceLimit(const char* name, uint64_t fallback,
                              uint64_t maximum = UINT64_MAX) {
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (*text == '-' || end == text || *end || value == 0 || value > maximum) {
        std::cerr << "PrismRT: ignoring invalid " << name << '\n';
        return fallback;
    }
    return value;
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
    static PreparationAdmission& Shared() { static PreparationAdmission value; return value; }
    uint64_t peakReservedBytes() const { std::lock_guard lock(mutex); return peak; }
    uint64_t admissionWaitNanoseconds() const {
        std::lock_guard lock(mutex); return waitNanoseconds;
    }
private:
    PreparationAdmission() : limit(ResourceLimit("PRISM_PREPARATION_MB", 2048, 1048576) * 1024 * 1024) {}
    mutable std::mutex mutex;
    std::condition_variable changed;
    uint64_t limit, used = 0, peak = 0, active = 0, observedEstimate = 0;
    uint64_t waitNanoseconds = 0;
};

// Measured whole-process peak (~2 GB) is far below the former 512 MB per-job estimate; these match observed use.
inline constexpr uint64_t kMaterialCompileReservationBytes = 192ull * 1024ull * 1024ull;
// A template bind performs no native JIT, so it needs far less than a full compile.
inline constexpr uint64_t kMaterialBindReservationBytes = 64ull * 1024ull * 1024ull;

inline unsigned MaterialWorkerCount() {
    const unsigned fallback = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    return static_cast<unsigned>(ResourceLimit("PRISM_MATERIAL_THREADS", fallback, 64));
}

} // namespace prism
