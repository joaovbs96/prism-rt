#pragma once

#include "resolved_resource.h"
#include <pxr/usd/usdShade/udimUtils.h>
#include <charconv>

namespace prism::materialx {

struct UdimSourceTile {
    int tile = 0;
    PXR_NS::ArResolvedPath resolved;
};

inline std::vector<UdimSourceTile> ResolveUdimSourceTiles(
    const std::string& authored, const std::string& resolvedPattern,
    const PXR_NS::ArResolverContext& resolverContext)
{
    std::vector<UdimSourceTile> result;
    auto append = [&](int tile, const std::string& path) {
        if (tile < 1001 || path.empty()) return;
        result.push_back({tile, PXR_NS::ArResolvedPath(path)});
    };
    const std::string pattern = !resolvedPattern.empty() ? resolvedPattern : authored;
    const auto token = pattern.find("<UDIM>");
    if (token != std::string::npos) {
        const std::filesystem::path filesystemPattern(pattern);
        std::error_code error;
        const auto directory = filesystemPattern.parent_path();
        if (std::filesystem::is_directory(directory, error)) {
            const std::string name = filesystemPattern.filename().string();
            const auto nameToken = name.find("<UDIM>");
            if (nameToken == std::string::npos) {
                error = std::make_error_code(std::errc::invalid_argument);
            } else {
            const std::string prefix = name.substr(0, nameToken);
            const std::string suffix = name.substr(nameToken + 6);
            for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                if (error || !entry.is_regular_file()) continue;
                const std::string candidate = entry.path().filename().string();
                if (!candidate.starts_with(prefix) || !candidate.ends_with(suffix) ||
                    candidate.size() <= prefix.size() + suffix.size()) continue;
                const auto digits = std::string_view(candidate).substr(prefix.size(),
                    candidate.size() - prefix.size() - suffix.size());
                int tile = 0;
                const auto parsed = std::from_chars(digits.data(),
                    digits.data() + digits.size(), tile);
                if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size())
                    append(tile, entry.path().string());
            }
            }
        }
    }
    if (result.empty()) {
        PXR_NS::ArResolverContextBinder binder(resolverContext);
        for (const auto& [path, tileText] :
             PXR_NS::UsdShadeUdimUtils::ResolveUdimTilePaths(authored, {})) {
            int tile = 0;
            const auto parsed = std::from_chars(tileText.data(),
                tileText.data() + tileText.size(), tile);
            if (parsed.ec == std::errc{}) append(tile, path);
        }
        if (!result.empty()) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                std::cerr << "[MaterialX] UDIM custom-resolver enumeration is "
                    "limited by stock USD to tiles 1001-1100; keeping this "
                    "resource out of compatible-template reuse.\n";
            // A partial family would silently discard higher tiles supported
            // by the MDL SDK. Preserve the authored pattern for stock fallback.
            return {};
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.tile < b.tile;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.tile == b.tile;
    }), result.end());
    return result;
}

struct MaterialUdimResult {
    std::string mdlPath;
    std::vector<std::shared_ptr<const ResolvedMaterialResource>> tiles;
    uint64_t bytesRead = 0;
};

inline bool SameMaterialResourceFile(const std::filesystem::path& a,
                                     const std::filesystem::path& b)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(a, error);
    if (error || std::filesystem::file_size(b, error) != size || error) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    std::array<char, 64 * 1024> l{}, r{};
    while (left && right) {
        left.read(l.data(), l.size()); right.read(r.data(), r.size());
        if (left.gcount() != right.gcount() ||
            std::memcmp(l.data(), r.data(), static_cast<size_t>(left.gcount())) != 0)
            return false;
    }
    return left.eof() && right.eof();
}

inline MaterialUdimResult PrepareMaterialUdimResource(
    const std::string& authored, const std::string& resolvedPattern,
    const PXR_NS::ArResolverContext& resolverContext,
    const std::string& sourceVersion, const std::filesystem::path& cacheDir,
    std::string extension)
{
    MaterialUdimResult result;
    const auto sources = ResolveUdimSourceTiles(authored, resolvedPattern, resolverContext);
    if (sources.empty()) return result;
    if (extension.empty()) extension = ".bin";
    std::string family = "PrismMaterialUdim-v1\n";
    std::vector<std::pair<int, std::shared_ptr<const ResolvedMaterialResource>>> prepared;
    for (const auto& source : sources) {
        auto tile = PrepareMaterialResource(source.resolved, resolverContext,
            sourceVersion, cacheDir, extension);
        if (!tile.resource) return {};
        result.bytesRead += tile.bytesRead;
        family += std::to_string(source.tile) + ":" + tile.resource->fingerprint + "\n";
        prepared.emplace_back(source.tile, std::move(tile.resource));
    }
    const std::string digest = OIIO::SHA1::digest(family.data(), family.size());
    const auto textures = prism::TextureService::Acquire();
    for (const auto& [tile, resource] : prepared) {
        const auto destination = cacheDir /
            (digest + "." + std::to_string(tile) + extension);
        resource_detail::PublicationLock lock(destination);
        std::error_code error;
        bool verified = false;
        if (std::filesystem::exists(destination, error) && !error) {
            const auto stamp = textures->sourceIdentity(destination);
            auto& registry = resource_detail::GetRegistry();
            std::lock_guard registryLock(registry.mutex);
            const auto found = registry.verifiedPublications.find(stamp);
            verified = found != registry.verifiedPublications.end() &&
                found->second == resource->fingerprint;
        }
        if (verified) {
            // Same process/version already verified this immutable family tile.
        } else if (std::filesystem::exists(destination, error)) {
            if (error || !SameMaterialResourceFile(resource->path, destination)) return {};
        } else {
            // The source is already an immutable, verified publication. Copy
            // through a sibling temporary and validate by size/content.
            const auto temporary = std::filesystem::path(destination.string() + ".tmp");
            std::filesystem::copy_file(resource->path, temporary,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error) return {};
            std::filesystem::rename(temporary, destination, error);
            if (error && !std::filesystem::exists(destination)) return {};
            std::filesystem::remove(temporary, error);
        }
        textures->registerTrustedSource(destination,
            {"sha1", "1", resource->fingerprint});
        {
            const auto stamp = textures->sourceIdentity(destination);
            auto& registry = resource_detail::GetRegistry();
            std::lock_guard registryLock(registry.mutex);
            registry.verifiedPublications[stamp] = resource->fingerprint;
            while (registry.verifiedPublications.size() > 4096)
                registry.verifiedPublications.erase(
                    registry.verifiedPublications.begin());
        }
        result.tiles.push_back(resource);
    }
    result.mdlPath = "/" + digest + ".<UDIM>" + extension;
    return result;
}

} // namespace prism::materialx
