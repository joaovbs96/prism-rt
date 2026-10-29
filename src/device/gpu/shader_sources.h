#pragma once

// Shader sources of the GPU devices (the shaders/ directory, embedded at build
// time) and assembly of a kernel from them.

#include "../../internal_export.h"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::device::gpu {

struct EmbeddedShader {
    const char* path;          // relative to shaders/
    const char* const* chunks;
    size_t chunkCount;
};

std::span<const EmbeddedShader> EmbeddedShaders();

// A shader file by its path relative to shaders/. PRISM_SHADER_DIR, if set,
// takes precedence over the embedded copy (shader development).
bool LoadShaderSource(std::string_view path, std::string* source);

// Expands `#include "..."` starting at `top` (each file once) and inserts
// mdlCode at the line `//@PRISM_MDL_CODE@`. Includes resolve next to the
// including file, then in searchDirectories (e.g. "gpu/metal").
PRISM_INTERNAL_API bool AssembleShader(std::string_view top, std::vector<std::string> const& searchDirectories,
                    std::string const& mdlCode, std::string* source, std::string* error);

} // namespace prism::device::gpu
