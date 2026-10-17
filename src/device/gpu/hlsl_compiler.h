#pragma once

// Compiles the GPU kernel with DXC: to DXIL for the Metal device (followed by
// Metal shader converter) and to SPIR-V for the Vulkan device.

#include <filesystem>
#include <string>
#include <vector>

namespace prism::device::gpu {

enum class HlslTarget { Dxil, Spirv };

// The per-device shader cache directory (PRISM_CACHE_DIR or the platform
// cache location, plus `device`).
std::filesystem::path ShaderCacheDirectory(std::string const& device);

// A stable digest of a source text, for cache file names.
std::string SourceDigest(std::string const& text);

// Compiles the compute entry point `main` of `source`. The source and the
// output pass through `workDirectory`.
bool CompileHlsl(std::string const& source, HlslTarget target, std::filesystem::path const& workDirectory,
                 std::vector<char>* binary, std::string* error);

} // namespace prism::device::gpu
