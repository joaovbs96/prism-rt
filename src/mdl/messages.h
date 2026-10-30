#pragma once

// How Prism reports the MDL SDK's messages (src/mdl/context.cpp's receiving
// logger and MaterialCompiler::print_context_messages), kept free of SDK
// includes so tests can check the rules directly.

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mdl {

// Compiler warnings about code MaterialX's MDL generator emits but does not
// use (an unused variable, parameter or let temporary). Every generated
// material has some and none affects rendering, so they are shown only as
// "mdl" debug output (PRISM_MDL_VERBOSE).
inline bool IsUnusedCodeMessage(std::string_view text)
{
    static constexpr std::array<std::string_view, 3> kCodes = {
        "C181 unused variable", "C183 unused parameter", "C350 unused let temporary"};
    for (std::string_view code : kCodes)
        if (text.find(code) != std::string_view::npos) return true;
    return false;
}

// Texture paths of a generated module the SDK resolves against its search
// paths rather than one absolute file: MaterialX writes a file name that is
// not absolute ("<hash>.exr", published next to the document in the material
// cache) as an absolute MDL path ("/<hash>.exr").
inline bool HasSearchPathTexture(std::vector<std::string> const& paths)
{
    for (std::string const& path : paths) {
        if (path.empty()) continue;
        if (path.front() == '/' || path.front() == '\\') return true;
        if (!std::filesystem::path(path).has_root_name() && !std::filesystem::path(path).is_absolute()) return true;
    }
    return false;
}

} // namespace mdl
