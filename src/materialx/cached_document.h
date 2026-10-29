#pragma once

// An in-memory MaterialX document as an immutable .mtlx file in the material
// cache, for MDLMaterial (which compiles from a path and resolves textures
// beside it). Content-named: material edits and concurrent processes must not
// overwrite a document an MDL transaction still references.

#include <OpenImageIO/hash.h>

#include "../module_state.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace prism::materialx {

// cacheDir / "<prefix><sha1 of xml>.mtlx", written once (temporary file and
// rename). Throws when it cannot be written.
inline std::filesystem::path PublishMaterialXDocument(const std::string& xml, const std::filesystem::path& cacheDir,
                                                      const std::string& prefix = "standalone_")
{
    const std::filesystem::path path = cacheDir / (prefix + OIIO::SHA1::digest(xml.data(), xml.size()) + ".mtlx");
    std::filesystem::create_directories(cacheDir);
    if (std::filesystem::exists(path)) return path;
#ifdef _WIN32
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    const auto staging = std::filesystem::path(path.string() + ".tmp-" + std::to_string(process) + "-" +
                                               std::to_string(prism::NextTemporaryFileSerial()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{staging};
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        output << xml;
        output.close();
        if (!output) throw std::runtime_error("Cannot write recovered MaterialX document");
    }
    std::error_code error;
    std::filesystem::rename(staging, path, error);
    // A concurrent process may have published identical content first.
    if (error && !std::filesystem::exists(path))
        throw std::filesystem::filesystem_error("Publish recovered MaterialX", staging, path, error);
    return path;
}

} // namespace prism::materialx
