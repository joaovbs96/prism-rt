#pragma once

#include <cctype>

#include "../texture_service.h"
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <OpenImageIO/hash.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#endif

namespace prism::materialx {

struct ResolvedMaterialResource;
struct ResolvedMaterialResourceStats {
    uint64_t sourceReadCalls = 0;
    uint64_t sourceHashCalls = 0;
    uint64_t sourceBytesRead = 0;
    uint64_t waitNanoseconds = 0;
};
namespace resource_detail {
struct Entry {
    PXR_NS::ArResolverContext context;
    std::promise<std::shared_ptr<const ResolvedMaterialResource>> promise;
    std::shared_future<std::shared_ptr<const ResolvedMaterialResource>> future =
        promise.get_future().share();
};
struct Registry {
    std::mutex mutex;
    std::map<std::string, std::vector<std::shared_ptr<Entry>>> records;
    std::map<std::string, std::string> verifiedPublications;
    std::atomic<uint64_t> sourceReadCalls{0}, sourceHashCalls{0}, sourceBytesRead{0}, waitNanoseconds{0};
};
inline Registry& GetRegistry() {
    static auto* registry = new Registry;
    return *registry;
}
inline prism::materialx::ResolvedMaterialResourceStats GetStats() {
    auto& registry = GetRegistry();
    return {registry.sourceReadCalls.load(std::memory_order_relaxed),
            registry.sourceHashCalls.load(std::memory_order_relaxed),
            registry.sourceBytesRead.load(std::memory_order_relaxed),
            registry.waitNanoseconds.load(std::memory_order_relaxed)};
}
class PublicationLock {
public:
    explicit PublicationLock(std::filesystem::path const& path) {
#ifdef _WIN32
        const auto identity = std::filesystem::absolute(path).generic_string();
        const auto name = "Local\\PrismMaterialResource-" + OIIO::SHA1::digest(identity.data(), identity.size());
        handle = CreateMutexA(nullptr, FALSE, name.c_str());
        if (!handle) throw std::runtime_error("cannot create material publication mutex");
        const DWORD result = WaitForSingleObject(handle, INFINITE);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            CloseHandle(handle); handle = nullptr;
            throw std::runtime_error("cannot lock material publication");
        }
#else
        handle = open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
        if (handle < 0 || flock(handle, LOCK_EX) != 0) {
            if (handle >= 0) close(handle);
            throw std::runtime_error("cannot lock material publication");
        }
#endif
    }
    ~PublicationLock() {
#ifdef _WIN32
        ReleaseMutex(handle); CloseHandle(handle);
#else
        flock(handle, LOCK_UN); close(handle);
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    int handle = -1;
#endif
};
inline bool Matches(std::filesystem::path const& path, const char* bytes, size_t size) {
    if (std::filesystem::file_size(path) != size) return false;
    std::ifstream input(path, std::ios::binary);
    std::array<char, 64 * 1024> buffer;
    size_t offset = 0;
    while (offset < size) {
        const size_t count = std::min(buffer.size(), size - offset);
        if (!input.read(buffer.data(), static_cast<std::streamsize>(count)) ||
            std::memcmp(buffer.data(), bytes + offset, count) != 0) return false;
        offset += count;
    }
    return true;
}
} // namespace resource_detail

inline ResolvedMaterialResourceStats GetResolvedMaterialResourceStats() {
    return resource_detail::GetStats();
}

struct ResolvedMaterialResource {
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t sourceBytes = 0;
    // Keep trusted-source generations alive from early preparation until a
    // native texture runtime acquires the same shared service.
    std::shared_ptr<prism::TextureService> textureService;
};
struct MaterialResourceResult {
    std::shared_ptr<const ResolvedMaterialResource> resource;
    uint64_t bytesRead = 0; // This request's work, zero on a retained-version hit.
};

// The extension image readers know a file by: the authored one, unless the
// file is a common image format behind another name (RenderMan .tex files
// are OpenEXR or TIFF images).
inline std::string ImageExtensionFor(const std::string& authored, const char* bytes, size_t size)
{
    std::string lower;
    for (char c : authored) lower += char(std::tolower(static_cast<unsigned char>(c)));
    static const char* const known[] = {".exr", ".tif", ".tiff", ".tx", ".png", ".jpg", ".jpeg",
                                        ".hdr", ".tga", ".bmp", ".psd", ".dds", ".webp", ".ppm"};
    for (const char* extension : known)
        if (lower == extension) return authored;
    auto starts = [&](std::initializer_list<unsigned char> magic) {
        if (size < magic.size()) return false;
        size_t i = 0;
        for (unsigned char c : magic)
            if (static_cast<unsigned char>(bytes[i++]) != c) return false;
        return true;
    };
    if (starts({0x76, 0x2f, 0x31, 0x01})) return ".exr";
    if (starts({'I', 'I', 0x2a, 0x00}) || starts({'M', 'M', 0x00, 0x2a}) ||
        starts({'I', 'I', 0x2b, 0x00}) || starts({'M', 'M', 0x00, 0x2b})) return ".tif";
    if (starts({0x89, 'P', 'N', 'G'})) return ".png";
    if (starts({0xff, 0xd8, 0xff})) return ".jpg";
    return authored;
}

// Owned resolver inputs only. The registry never calls Hydra or traverses a
// stage. Waiters release the map mutex before waiting or opening an asset.
inline MaterialResourceResult PrepareMaterialResource(
    const PXR_NS::ArResolvedPath& resolved,
    const PXR_NS::ArResolverContext& resolverContext,
    const std::string& sourceVersion,
    const std::filesystem::path& cacheDir,
    const std::string& extension)
{
    auto* registry = &resource_detail::GetRegistry();
    const auto textures = prism::TextureService::Acquire();
    prism::TexturePrepareOptions options;
    options.resolverIdentity = resolverContext.GetDebugString();
    options.sourceVersion = sourceVersion;
    std::error_code error;
    const std::filesystem::path file(resolved.GetPathString());
    const bool fileBacked = std::filesystem::is_regular_file(file, error);
    const std::string version = fileBacked
        ? textures->sourceIdentity(file, options)
        : resolved.GetPathString() + "\nresolver-version:" + sourceVersion;
    const std::string key = std::filesystem::absolute(cacheDir).generic_string() +
        "\n" + extension + "\n" + version;
    std::shared_ptr<resource_detail::Entry> entry;
    bool owner = false;
    {
        std::lock_guard lock(registry->mutex);
        auto& entries = registry->records[key];
        for (auto const& candidate : entries)
            if (candidate->context == resolverContext) { entry = candidate; break; }
        if (!entry) {
            entry = std::make_shared<resource_detail::Entry>();
            entry->context = resolverContext;
            entries.push_back(entry);
            owner = true;
        }
    }
    if (!owner) {
        const auto waitStart = std::chrono::steady_clock::now();
        std::shared_ptr<const ResolvedMaterialResource> resource;
        try {
            resource = entry->future.get();
        } catch (...) {
            registry->waitNanoseconds.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
            throw;
        }
        registry->waitNanoseconds.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
        if (std::filesystem::is_regular_file(resource->path)) return {resource, 0};
        // A manually removed artifact needs materialization again. Only erase
        // the exact entry, so a concurrent replacement cannot be discarded.
        {
            std::lock_guard lock(registry->mutex);
            auto found = registry->records.find(key);
            if (found != registry->records.end()) {
                std::erase(found->second, entry);
                if (found->second.empty()) registry->records.erase(found);
            }
        }
        return PrepareMaterialResource(resolved, resolverContext, sourceVersion, cacheDir, extension);
    }
    std::filesystem::path temporary;
    try {
        PXR_NS::ArResolverContextBinder binder(resolverContext);
        registry->sourceReadCalls.fetch_add(1, std::memory_order_relaxed);
        auto asset = PXR_NS::ArGetResolver().OpenAsset(resolved);
        if (!asset) throw std::runtime_error("cannot open material resource: " + resolved.GetPathString());
        const auto bytes = asset->GetBuffer();
        const size_t size = asset->GetSize();
        if (!bytes) throw std::runtime_error("cannot read material resource: " + resolved.GetPathString());
        registry->sourceBytesRead.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
        auto resource = std::make_shared<ResolvedMaterialResource>();
        registry->sourceHashCalls.fetch_add(1, std::memory_order_relaxed);
        resource->fingerprint = OIIO::SHA1::digest(bytes.get(), size);
        resource->textureService = textures;
        resource->sourceBytes = size;
        if (fileBacked && textures->sourceIdentity(file, options) != version)
            throw std::runtime_error("material resource changed while reading: " + resolved.GetPathString());
        std::filesystem::create_directories(cacheDir);
        resource->path = cacheDir / (resource->fingerprint +
            ImageExtensionFor(extension, reinterpret_cast<const char*>(bytes.get()), size));
        resource_detail::PublicationLock publication(resource->path);
        if (std::filesystem::exists(resource->path)) {
            const auto stamp = textures->sourceIdentity(resource->path);
            bool verified = false;
            {
                std::lock_guard lock(registry->mutex);
                auto found = registry->verifiedPublications.find(stamp);
                verified = found != registry->verifiedPublications.end() &&
                    found->second == resource->fingerprint;
            }
            if (!verified && !resource_detail::Matches(resource->path, bytes.get(), size))
                throw std::runtime_error("corrupt material resource cache artifact: " + resource->path.string());
        }

        if (!std::filesystem::exists(resource->path)) {
            static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
            const auto process = _getpid();
#else
            const auto process = getpid();
#endif
            temporary = resource->path.string() + "." + std::to_string(process) + "." +
                std::to_string(++serial) + ".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(bytes.get(), static_cast<std::streamsize>(size)))
                    throw std::runtime_error("cannot materialize resource: " + temporary.string());
                out.close();
                if (!out) throw std::runtime_error("cannot close material resource: " + temporary.string());
            }
            std::filesystem::rename(temporary, resource->path, error);
            if (error) {
                if (!std::filesystem::is_regular_file(resource->path))
                    throw std::runtime_error("cannot publish material resource: " + error.message());
                std::filesystem::remove(temporary, error);
            }
        }
        {
            const auto stamp = textures->sourceIdentity(resource->path);
            std::lock_guard lock(registry->mutex);
            registry->verifiedPublications[stamp] = resource->fingerprint;
            while (registry->verifiedPublications.size() > 4096)
                registry->verifiedPublications.erase(registry->verifiedPublications.begin());
        }
        textures->registerTrustedSource(resource->path, {"sha1", "1", resource->fingerprint});
        entry->promise.set_value(resource);
        // Bound only unreferenced completed bookkeeping; prepared file policy
        // remains with the existing material cache and TextureService.
        {
            std::lock_guard lock(registry->mutex);
            if (registry->records.size() > 4096) {
                for (auto it = registry->records.begin(); it != registry->records.end();) {
                    if (it->first != key) {
                        std::erase_if(it->second, [](auto const& record) {
                            return record.use_count() == 1 && record->future.wait_for(
                                std::chrono::seconds(0)) == std::future_status::ready;
                        });
                    }
                    if (it->second.empty()) it = registry->records.erase(it); else ++it;
                    if (registry->records.size() <= 4096) break;
                }
            }
        }
        return {resource, size};
    } catch (...) {
        if (!temporary.empty()) std::filesystem::remove(temporary, error);
        entry->promise.set_exception(std::current_exception());
        std::lock_guard lock(registry->mutex);
        auto found = registry->records.find(key);
        if (found != registry->records.end()) {
            std::erase(found->second, entry);
            if (found->second.empty()) registry->records.erase(found);
        }
        throw;
    }
}
} // namespace prism::materialx
