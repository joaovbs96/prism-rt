#pragma once

// Shader sources of the GPU devices (the shaders/ directory, embedded at build
// time) and assembly of a kernel from them.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

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

// Expands `#include "..."` (each file once) starting at `top` and replaces the
// line `//@PRISM_MDL_CODE@` by mdlCode. A leading #version line of mdlCode
// is dropped; the top file supplies its own.
bool AssembleShader(std::string_view top, std::string const& mdlCode, std::string* source,
                    std::string* error);

} // namespace prism::device::gpu
