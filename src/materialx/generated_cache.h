#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prism::materialx {

struct GeneratedTemplateArtifact {
    struct Uniform { std::string name, path, type; };
    std::string moduleName;
    std::string source;
    std::vector<std::string> texturePaths;
    std::vector<std::pair<std::string,std::pair<size_t,size_t>>> spans;
    std::vector<Uniform> uniforms;
};

// Hashes length-delimited fields with OIIO's stable SHA-1 implementation.
std::string GeneratedTemplateKey(std::vector<std::string> const& fields);

// Hashes the complete, ordered contents below a MaterialX library root. The
// root's absolute name is deliberately excluded so relocated identical
// installations share artifacts.
std::string MaterialXLibraryDigest(std::filesystem::path const& root);

class GeneratedTemplateDiskCache {
public:
    GeneratedTemplateDiskCache(std::filesystem::path root, uint64_t budgetBytes);
    std::optional<GeneratedTemplateArtifact> load(std::string const& key) const;
    bool store(std::string const& key, GeneratedTemplateArtifact const& value) const;
    std::filesystem::path pathForTesting(std::string const& key) const;
private:
    void prune() const;
    void pruneUnlocked() const;
    std::filesystem::path m_root;
    uint64_t m_budgetBytes;
};

} // namespace prism::materialx
