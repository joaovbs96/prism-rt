#pragma once

// Default render settings shared by the frontends: the Hydra delegate's
// prism:* render settings (usdview), prism_hydra_render and the standalone
// CLI. See docs/hydra.md, "Render settings".

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string_view>

namespace prism::defaults {

// Sampling. With adaptive sampling the samples per pixel are the per-pixel
// maximum; a pixel stops once its error estimate is below the threshold
// (src/device/adaptive_sampling.h). The threshold was chosen from the study
// in docs/hydra.md ("Adaptive sampling").
inline constexpr int kSamplesPerPixel = 2048;
inline constexpr float kAdaptiveThreshold = 0.01f;
inline constexpr int kAdaptiveMinSamples = 64;
// Render time limit in seconds; 0 renders until the samples or the
// threshold stop it.
inline constexpr double kTimeLimitSeconds = 0.0;

// Settings that also have an environment variable. Precedence: command-line
// flag > RenderSettings prism:* > environment variable > built-in default.
struct EnvironmentSettings {
    int subdivisionLevel = 2;                // PRISM_SUBDIVISION_LEVEL, 0-6
    int subdivisionMaxFaces = 1 << 16;       // PRISM_SUBDIVISION_MAX_FACES
    int displacementLevel = 4;               // PRISM_DISPLACEMENT_LEVEL, 0-6
    int displacementMaxFaces = 1 << 18;      // PRISM_DISPLACEMENT_MAX_FACES
    int subdivisionBudgetFaces = 24 << 20;   // PRISM_SUBDIVISION_BUDGET_FACES
    int gpuTextureBudgetMiB = 0;             // PRISM_GPU_TEXTURE_BUDGET_MIB; 0: automatic
    bool sssFallback = false;                // PRISM_SSS_FALLBACK
};

inline constexpr int kMaxSubdivisionLevel = 6;
// The smallest explicit GPU texture budget.
inline constexpr int kMinGpuTextureBudgetMiB = 64;

inline int ClampSubdivisionLevel(int level) { return std::clamp(level, 0, kMaxSubdivisionLevel); }
// Face limits are at least one face.
inline int ClampFaces(long long faces) { return int(std::clamp(faces, 1ll, (long long)INT_MAX)); }
// 0 is automatic (45% of device memory); explicit budgets are at least 64 MiB.
inline int ClampGpuTextureBudgetMiB(long long mib)
{
    return mib <= 0 ? 0 : int(std::clamp(mib, (long long)kMinGpuTextureBudgetMiB, (long long)INT_MAX));
}

// The built-in defaults with the environment variables applied, read once.
inline EnvironmentSettings const& FromEnvironment()
{
    static const EnvironmentSettings settings = [] {
        EnvironmentSettings result;
        auto text = [](const char* name) -> const char* {
            const char* value = std::getenv(name);
            return value && *value ? value : nullptr;
        };
        if (const char* v = text("PRISM_SUBDIVISION_LEVEL")) result.subdivisionLevel = ClampSubdivisionLevel(std::atoi(v));
        if (const char* v = text("PRISM_SUBDIVISION_MAX_FACES")) result.subdivisionMaxFaces = ClampFaces(std::atoll(v));
        if (const char* v = text("PRISM_DISPLACEMENT_LEVEL")) result.displacementLevel = ClampSubdivisionLevel(std::atoi(v));
        if (const char* v = text("PRISM_DISPLACEMENT_MAX_FACES")) result.displacementMaxFaces = ClampFaces(std::atoll(v));
        if (const char* v = text("PRISM_SUBDIVISION_BUDGET_FACES")) result.subdivisionBudgetFaces = ClampFaces(std::atoll(v));
        if (const char* v = text("PRISM_GPU_TEXTURE_BUDGET_MIB")) result.gpuTextureBudgetMiB = ClampGpuTextureBudgetMiB(std::atoll(v));
        if (const char* v = text("PRISM_SSS_FALLBACK")) result.sssFallback = std::string_view(v) != "0";
        return result;
    }();
    return settings;
}

} // namespace prism::defaults
