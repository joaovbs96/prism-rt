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

bool Exists(std::string const& path)
{
    std::string ignored;
    return LoadShaderSource(path, &ignored);
}

struct Assembler {
    std::vector<std::string> searchDirectories;
    std::string const* mdlCode = nullptr;
    std::set<std::string> included; // #pragma once semantics for every file
    std::string out;
    std::string error;

    // An include resolves next to the including file, then in the search
    // directories, as with a compiler's include path.
    std::string resolve(std::string const& includer, std::string const& name) const
    {
        const auto local = (std::filesystem::path(includer).parent_path() / name).lexically_normal().generic_string();
        if (Exists(local)) return local;
        for (auto const& directory : searchDirectories) {
            const auto candidate = (std::filesystem::path(directory) / name).lexically_normal().generic_string();
            if (Exists(candidate)) return candidate;
        }
        return {};
    }

    bool file(std::string const& path)
    {
        if (!included.insert(path).second) return true;
        std::string text;
        if (!LoadShaderSource(path, &text)) {
            error = "missing shader source: " + path;
            return false;
        }
        return lines(text, path);
    }

    bool lines(std::string const& text, std::string const& path)
    {
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            const size_t first = line.find_first_not_of(" \t");
            const std::string_view trimmed =
                first == std::string::npos ? std::string_view{} : std::string_view(line).substr(first);
            if (trimmed.starts_with("#pragma once")) continue;
            if (trimmed.starts_with("#include \"")) {
                const size_t end = trimmed.find('"', 10);
                const std::string name(trimmed.substr(10, end == std::string_view::npos ? 0 : end - 10));
                const std::string resolved = resolve(path, name);
                if (resolved.empty()) {
                    error = "cannot resolve include \"" + name + "\" in " + path;
                    return false;
                }
                if (!file(resolved)) return false;
                continue;
            }
            if (trimmed.starts_with("//@PRISM_MDL_CODE@")) {
                if (mdlCode && !lines(*mdlCode, path)) return false;
                continue;
            }
            out.append(line);
            out.push_back('\n');
        }
        return true;
    }
};

} // namespace

bool AssembleShader(std::string_view top, std::vector<std::string> const& searchDirectories,
                    std::string const& mdlCode, std::string* source, std::string* error)
{
    Assembler assembler;
    assembler.searchDirectories = searchDirectories;
    assembler.mdlCode = &mdlCode;
    if (!assembler.file(std::string(top))) {
        if (error) *error = assembler.error;
        return false;
    }
    *source = std::move(assembler.out);
    return true;
}

} // namespace prism::device::gpu
