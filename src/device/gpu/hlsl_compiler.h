#pragma once

// Compiles the GPU kernel with DXC: to DXIL for the Metal device (followed by
// Metal shader converter) and to SPIR-V for the Vulkan device.

#include "../../internal_export.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace prism::device::gpu {

enum class HlslTarget { Dxil, Spirv };

// The per-device shader cache directory (PRISM_CACHE_DIR or the platform
// cache location, plus `device`).
PRISM_INTERNAL_API std::filesystem::path ShaderCacheDirectory(std::string const& device);

// A stable digest of a source text, for cache file names.
std::string SourceDigest(std::string const& text);

// What to compile: by default the compute entry point `main`. Library
// profiles (lib_6_x: ray tracing shaders) take no entry point.
struct HlslStage {
    std::string profile = "cs_6_6";
    std::string entry = "main";
};

// Cost of one DXC run (Windows: peak memory of the process).
struct HlslCompileStats {
    double seconds = 0.0;
    uint64_t peakBytes = 0;
};

// Compiles `stage` of `source`. The source and the output pass through
// `workDirectory`. When `cancel` turns true, DXC is stopped (Windows) and
// the compile fails with an error HlslCancelled recognises; nothing is kept.
PRISM_INTERNAL_API bool CompileHlsl(std::string const& source, HlslTarget target, std::filesystem::path const& workDirectory,
                 std::vector<char>* binary, std::string* error, HlslStage const& stage = {},
                 HlslCompileStats* stats = nullptr, std::atomic<bool> const* cancel = nullptr);

// Whether a CompileHlsl error means it was cancelled.
PRISM_INTERNAL_API bool HlslCancelled(std::string const& error);

// The DXC executable, options and memory limit CompileHlsl uses for `target`
// and `stage`, for keys of results cached by callers.
std::string HlslCompilerSignature(HlslTarget target, HlslStage const& stage = {});

// Whether a CompileHlsl error means DXC exceeded its memory limit (Windows).
bool HlslTooLarge(std::string const& error);

} // namespace prism::device::gpu
