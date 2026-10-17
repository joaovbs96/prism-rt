#include "shader_sources.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace prism::device::gpu {

bool LoadShaderSource(std::string_view path, std::string* source)
{
    if (const char* dir = std::getenv("PRISM_SHADER_DIR"); dir && *dir) {
        std::ifstream file(std::filesystem::path(dir) / std::string(path), std::ios::binary);
        if (file) {
            std::ostringstream text;
            text << file.rdbuf();
            *source = text.str();
            return true;
        }
    }
    for (EmbeddedShader const& shader : EmbeddedShaders()) {
        if (path != shader.path) continue;
        source->clear();
        for (size_t i = 0; i < shader.chunkCount; ++i) source->append(shader.chunks[i]);
        return true;
    }
    return false;
}

namespace {

bool ExpandText(std::string const& text, std::string_view path, std::string const& mdlCode,
                std::set<std::string>& seen, std::string& out, std::string* error);

bool Expand(std::string_view path, std::string const& mdlCode, std::set<std::string>& seen,
            std::string& out, std::string* error)
{
    if (!seen.insert(std::string(path)).second) return true;
    std::string text;
    if (!LoadShaderSource(path, &text)) {
        if (error) *error = "missing shader source: " + std::string(path);
        return false;
    }
    return ExpandText(text, path, mdlCode, seen, out, error);
}

bool ExpandText(std::string const& text, std::string_view path, std::string const& mdlCode,
                std::set<std::string>& seen, std::string& out, std::string* error)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t first = line.find_first_not_of(" \t");
        std::string_view trimmed = first == std::string::npos ? std::string_view{}
                                                              : std::string_view(line).substr(first);
        if (trimmed.starts_with("#include \"")) {
            const size_t end = trimmed.find('"', 10);
            if (end == std::string_view::npos) {
                if (error) *error = "malformed include in " + std::string(path);
                return false;
            }
            if (!Expand(trimmed.substr(10, end - 10), mdlCode, seen, out, error)) return false;
            continue;
        }
        if (trimmed.starts_with("//@PRISM_MDL_CODE@")) {
            // Generated code may include Prism's API type headers.
            std::string_view code = mdlCode;
            if (code.starts_with("// GLSL version")) code = code.substr(code.find('\n') + 1);
            if (code.starts_with("#version")) code = code.substr(code.find('\n') + 1);
            if (!ExpandText(std::string(code), "generated MDL code", {}, seen, out, error)) return false;
            continue;
        }
        out.append(line);
        out.push_back('\n');
    }
    return true;
}

} // namespace

bool AssembleShader(std::string_view top, std::string const& mdlCode, std::string* source,
                    std::string* error)
{
    std::set<std::string> seen;
    source->clear();
    return Expand(top, mdlCode, seen, *source, error);
}

} // namespace prism::device::gpu
