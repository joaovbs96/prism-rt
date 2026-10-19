#pragma once

// Progress and status model for work that happens before (and between)
// rendered frames: scene conversion, material compilation, texture
// preparation, GPU kernel compilation and upload. Hosts poll it from their UI
// thread about ten times a second, so reads and updates never touch the
// renderer's own locks: phase state is atomic, and the few strings (detail
// text, device name, error) sit behind this object's private mutex, which is
// only held to copy a short string.
//
// Several phases can be active at once (textures prepare while materials
// compile; a kernel compile nests inside the GPU upload). Read() reports them
// all and names the primary one, the latest stage of the pipeline that is
// still running. Updates are atomic increments and cost nothing when no
// Progress is installed: instrumented code uses the free functions in
// prism::progress, which ignore the call without a current model.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace prism {

enum class Phase : uint8_t {
    Device,     // creating the render device
    Loading,    // capturing and converting the scene (meshes)
    Materials,  // converting and compiling materials (MaterialX -> MDL -> native)
    Textures,   // preparing textures
    Kernel,     // compiling the GPU kernel
    Upload,     // uploading buffers, images and acceleration structures
    Rendering,  // the device is ready and traces samples
    Count
};

inline const char* PhaseName(Phase phase)
{
    switch (phase) {
    case Phase::Device: return "device";
    case Phase::Loading: return "loading";
    case Phase::Materials: return "materials";
    case Phase::Textures: return "textures";
    case Phase::Kernel: return "kernel";
    case Phase::Upload: return "upload";
    case Phase::Rendering: return "rendering";
    case Phase::Count: break;
    }
    return "idle";
}

class Progress {
public:
    static constexpr size_t kPhases = size_t(Phase::Count);

    struct PhaseState {
        bool active = false;
        uint64_t done = 0;
        uint64_t total = 0;
        double seconds = 0.0;   // since the phase began (while active)
        std::string detail;
    };
    struct Snapshot {
        // The reported phase: "idle", a PhaseName(), or "error".
        std::string phase = "idle";
        double phaseSeconds = 0.0;
        std::string detail;      // detail of the primary phase
        std::string device;
        std::string error;
        std::array<PhaseState, kPhases> phases;
    };

    Progress() { Install(); }
    ~Progress()
    {
        Progress* self = this;
        s_current.compare_exchange_strong(self, nullptr);
    }
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    // The model that instrumented code reports to: the most recently created
    // one that is still alive (a process normally has one renderer).
    static Progress* Current() { return s_current.load(std::memory_order_acquire); }
    void Install() { s_current.store(this, std::memory_order_release); }

    // Starts a phase with fresh counters. Beginning an active phase keeps it.
    void Begin(Phase phase)
    {
        Slot& slot = m_slots[size_t(phase)];
        if (slot.active.load(std::memory_order_acquire)) return;
        slot.done.store(0, std::memory_order_relaxed);
        slot.total.store(0, std::memory_order_relaxed);
        slot.startNs.store(NowNs(), std::memory_order_relaxed);
        { std::lock_guard lock(m_mutex); slot.detail.clear(); }
        slot.active.store(true, std::memory_order_release);
    }
    // Ends a phase; its counters stay readable.
    void End(Phase phase) { m_slots[size_t(phase)].active.store(false, std::memory_order_release); }
    void SetActive(Phase phase, bool active) { active ? Begin(phase) : End(phase); }
    bool IsActive(Phase phase) const { return m_slots[size_t(phase)].active.load(std::memory_order_acquire); }

    // Sets the total and restarts the count of the phase's current stage.
    void SetTotal(Phase phase, uint64_t total)
    {
        Slot& slot = m_slots[size_t(phase)];
        slot.done.store(0, std::memory_order_relaxed);
        slot.total.store(total, std::memory_order_relaxed);
    }
    void AddTotal(Phase phase, uint64_t n) { m_slots[size_t(phase)].total.fetch_add(n, std::memory_order_relaxed); }
    void Add(Phase phase, uint64_t n = 1) { m_slots[size_t(phase)].done.fetch_add(n, std::memory_order_relaxed); }

    void SetDetail(Phase phase, std::string_view text)
    {
        std::lock_guard lock(m_mutex);
        m_slots[size_t(phase)].detail.assign(text);
    }
    // The active render device, for example "vulkan: NVIDIA GeForce RTX 4070 Ti",
    // "cpu" or "cpu (Vulkan fallback: kernel too large)".
    void SetDevice(std::string_view text)
    {
        std::lock_guard lock(m_mutex);
        if (m_device != text) m_device.assign(text);
    }
    // A failure the host should show instead of a phase; empty clears it.
    void SetError(std::string_view text)
    {
        std::lock_guard lock(m_mutex);
        m_error.assign(text);
    }

    Snapshot Read() const
    {
        Snapshot out;
        const int64_t now = NowNs();
        {
            std::lock_guard lock(m_mutex);
            out.device = m_device;
            out.error = m_error;
            for (size_t i = 0; i < kPhases; ++i) out.phases[i].detail = m_slots[i].detail;
        }
        // Later stages of the pipeline take precedence (the kernel compile
        // nests inside the upload); materials come before textures because
        // compilation is what the scene waits on.
        constexpr Phase order[] = {Phase::Kernel, Phase::Upload, Phase::Materials, Phase::Textures,
                                   Phase::Loading, Phase::Device, Phase::Rendering};
        for (size_t i = 0; i < kPhases; ++i) {
            Slot const& slot = m_slots[i];
            PhaseState& state = out.phases[i];
            state.active = slot.active.load(std::memory_order_acquire);
            state.done = slot.done.load(std::memory_order_relaxed);
            state.total = slot.total.load(std::memory_order_relaxed);
            if (state.active)
                state.seconds = double(now - slot.startNs.load(std::memory_order_relaxed)) * 1e-9;
        }
        for (Phase phase : order) {
            PhaseState const& state = out.phases[size_t(phase)];
            if (!state.active) continue;
            out.phase = PhaseName(phase);
            out.phaseSeconds = state.seconds;
            out.detail = state.detail;
            break;
        }
        if (!out.error.empty()) out.phase = "error";
        return out;
    }

private:
    struct Slot {
        std::atomic<bool> active{false};
        std::atomic<uint64_t> done{0};
        std::atomic<uint64_t> total{0};
        std::atomic<int64_t> startNs{0};
        std::string detail; // guarded by m_mutex
    };
    static int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::array<Slot, kPhases> m_slots;
    mutable std::mutex m_mutex;
    std::string m_device;
    std::string m_error;
    inline static std::atomic<Progress*> s_current{nullptr};
};

// One-line hooks for instrumented code. They do nothing without a current
// model (the standalone renderer) and never allocate, except for detail text.
namespace progress {

inline void Begin(Phase p) { if (auto* m = Progress::Current()) m->Begin(p); }
inline void End(Phase p) { if (auto* m = Progress::Current()) m->End(p); }
inline void SetTotal(Phase p, uint64_t n) { if (auto* m = Progress::Current()) m->SetTotal(p, n); }
inline void AddTotal(Phase p, uint64_t n) { if (auto* m = Progress::Current()) m->AddTotal(p, n); }
inline void Add(Phase p, uint64_t n = 1) { if (auto* m = Progress::Current()) m->Add(p, n); }
inline void Detail(Phase p, std::string_view text) { if (auto* m = Progress::Current()) m->SetDetail(p, text); }
inline void Device(std::string_view text) { if (auto* m = Progress::Current()) m->SetDevice(text); }

// Keeps a phase active for a scope.
class Scope {
public:
    explicit Scope(Phase p) : m_phase(p) { Begin(p); }
    ~Scope() { End(m_phase); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Phase m_phase;
};

} // namespace progress
} // namespace prism
