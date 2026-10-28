#pragma once

// Material resources (texture files) published into the material cache:
// content-addressed copies the texture service trusts. The resolver a
// resource is read through is abstract here (MaterialResourceReader): plain
// files for the glTF and OBJ loaders, USD's Ar for the USD loaders
// (resolved_resource.h).

#include <cctype>

#include "../texture_service.h"
#include "../fingerprint_records.h"
#include <OpenImageIO/hash.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
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

// A resolver context, type-erased: requests for one resource share their
// work only within equal contexts (operator== of the wrapped value). The
// default context is that of plain files.
class MaterialResourceContext {
public:
    MaterialResourceContext() = default;
    template <class T>
    explicit MaterialResourceContext(T value) : holder(std::make_shared<Holder<T>>(std::move(value))) {}
    bool operator==(MaterialResourceContext const& other) const
    {
        if (!holder || !other.holder) return !holder && !other.holder;
        return holder->equals(*other.holder);
    }
private:
    struct Base {
        virtual ~Base() = default;
        virtual bool equals(Base const& other) const = 0;
    };
    template <class T>
    struct Holder : Base {
        explicit Holder(T v) : value(std::move(v)) {}
        bool equals(Base const& other) const override
        {
            auto const* same = dynamic_cast<Holder const*>(&other);
            return same && same->value == value;
        }
        T value;
    };
    std::shared_ptr<const Base> holder;
};

// How a resource is read: its resolved path, the resolver's identity (part
// of the texture service's source identity), its context, and how to open
// it (nullopt: it cannot be opened; null bytes: it cannot be read).
struct MaterialResourceReader {
    using Bytes = std::pair<std::shared_ptr<const char>, size_t>;
    std::string path;
    std::string resolverIdentity;
    MaterialResourceContext context;
    std::function<std::optional<Bytes>()> open;
};

// The resolver identity of plain files: that of USD's default resolver
// context, so the glTF/OBJ loaders and the USD loaders key a file alike
// (prism_material_resource_test checks it against
// ArResolverContext().GetDebugString()).
inline constexpr char const* kFileResolverIdentity = "";

// A reader of a local file, as its whole contents.
inline MaterialResourceReader FileResourceReader(std::filesystem::path const& file)
{
    MaterialResourceReader reader;
    reader.path = file.string();
    reader.resolverIdentity = kFileResolverIdentity;
    reader.open = [file]() -> std::optional<MaterialResourceReader::Bytes> {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) return std::nullopt;
        const std::streamoff size = in.tellg();
        if (size < 0) return MaterialResourceReader::Bytes{nullptr, 0};
        std::shared_ptr<char> bytes(new char[size_t(size) ? size_t(size) : 1], std::default_delete<char[]>());
        in.seekg(0);
        if (size && !in.read(bytes.get(), size)) return MaterialResourceReader::Bytes{nullptr, 0};
        return MaterialResourceReader::Bytes{std::move(bytes), size_t(size)};
    };
    return reader;
}

struct ResolvedMaterialResource;
struct ResolvedMaterialResourceStats {
    uint64_t sourceReadCalls = 0;
    uint64_t sourceHashCalls = 0;
    uint64_t sourceBytesRead = 0;
    uint64_t waitNanoseconds = 0;
};
namespace resource_detail {
struct Entry {
    MaterialResourceContext context;
    std::promise<std::shared_ptr<const ResolvedMaterialResource>> promise;
    std::shared_future<std::shared_ptr<const ResolvedMaterialResource>> future =
        promise.get_future().share();
};
struct Registry {
    std::mutex mutex;
    std::map<std::string, std::vector<std::shared_ptr<Entry>>> records;
    std::map<std::string, std::string> verifiedPublications;
    std::atomic<uint64_t> sourceReadCalls{0}, sourceHashCalls{0}, sourceBytesRead{0}, waitNanoseconds{0};
    // Requests answered by a persistent fingerprint record.
    std::atomic<uint64_t> recordHits{0};
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

// In-memory resource bytes (a glTF image embedded in a buffer or a data URI)
// published like PrepareMaterialResource's artifacts: content-addressed
// (<sha1><extension>) in cacheDir, written once through a temporary file and
// a rename, and registered with the texture service as a trusted source.
inline std::shared_ptr<const ResolvedMaterialResource> PublishMaterialResourceBytes(
    const char* bytes, size_t size, const std::string& extension, const std::filesystem::path& cacheDir)
{
    if (!bytes || !size) throw std::runtime_error("empty material resource");
    const auto textures = prism::TextureService::Acquire();
    auto resource = std::make_shared<ResolvedMaterialResource>();
    resource->fingerprint = OIIO::SHA1::digest(bytes, size);
    resource->textureService = textures;
    resource->sourceBytes = size;
    std::filesystem::create_directories(cacheDir);
    resource->path = cacheDir / (resource->fingerprint + ImageExtensionFor(extension, bytes, size));
    resource_detail::PublicationLock publication(resource->path);
    std::error_code error;
    if (std::filesystem::exists(resource->path)) {
        if (!resource_detail::Matches(resource->path, bytes, size))
            throw std::runtime_error("corrupt material resource cache artifact: " + resource->path.string());
    } else {
        static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
        const auto process = _getpid();
#else
        const auto process = getpid();
#endif
        const std::filesystem::path temporary = resource->path.string() + "." + std::to_string(process) + "." +
            std::to_string(++serial) + ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out || !out.write(bytes, static_cast<std::streamsize>(size)))
                throw std::runtime_error("cannot materialize resource: " + temporary.string());
            out.close();
            if (!out) throw std::runtime_error("cannot close material resource: " + temporary.string());
        }
        std::filesystem::rename(temporary, resource->path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            if (!std::filesystem::is_regular_file(resource->path))
                throw std::runtime_error("cannot publish material resource: " + resource->path.string());
        }
    }
    textures->registerTrustedSource(resource->path, {"sha1", "1", resource->fingerprint});
    return resource;
}

// Owned resolver inputs only. The registry never calls Hydra or traverses a
// stage. Waiters release the map mutex before waiting or opening an asset.
inline MaterialResourceResult PrepareMaterialResource(
    const MaterialResourceReader& reader,
    const std::string& sourceVersion,
    const std::filesystem::path& cacheDir,
    const std::string& extension)
{
    auto* registry = &resource_detail::GetRegistry();
    const auto textures = prism::TextureService::Acquire();
    prism::TexturePrepareOptions options;
    options.resolverIdentity = reader.resolverIdentity;
    options.sourceVersion = sourceVersion;
    std::error_code error;
    const std::filesystem::path file(reader.path);
    const bool fileBacked = std::filesystem::is_regular_file(file, error);
    const std::string version = fileBacked
        ? textures->sourceIdentity(file, options)
        : reader.path + "\nresolver-version:" + sourceVersion;
    const std::string key = std::filesystem::absolute(cacheDir).generic_string() +
        "\n" + extension + "\n" + version;
    std::shared_ptr<resource_detail::Entry> entry;
    bool owner = false;
    {
        std::lock_guard lock(registry->mutex);
        auto& entries = registry->records[key];
        for (auto const& candidate : entries)
            if (candidate->context == reader.context) { entry = candidate; break; }
        if (!entry) {
            entry = std::make_shared<resource_detail::Entry>();
            entry->context = reader.context;
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
        return PrepareMaterialResource(reader, sourceVersion, cacheDir, extension);
    }
    std::filesystem::path temporary;
    try {
        // A file this cache already holds, unchanged since (see
        // fingerprint_records.h): its digest and artifact come from the
        // record instead of reading and hashing it again.
        const std::string recordIdentity = fileBacked
            ? "material-resource\n" + version + "\n" + fingerprint_records::FileChangeStamp(file) + "\n" + extension
            : std::string();
        fingerprint_records::Record record;
        if (fileBacked && fingerprint_records::Read(cacheDir, recordIdentity, &record) &&
            record.artifact.find_first_of("/\\") == std::string::npos) {
            const auto artifact = cacheDir / record.artifact;
            std::error_code statError;
            const auto sourceBytes = std::filesystem::file_size(file, statError);
            if (!statError && fingerprint_records::FileStamp(artifact) == record.artifactStamp &&
                textures->sourceIdentity(file, options) == version) {
                auto resource = std::make_shared<ResolvedMaterialResource>();
                resource->path = artifact;
                resource->fingerprint = record.fingerprint;
                resource->sourceBytes = sourceBytes;
                resource->textureService = textures;
                {
                    const auto stamp = textures->sourceIdentity(resource->path);
                    std::lock_guard lock(registry->mutex);
                    registry->verifiedPublications[stamp] = resource->fingerprint;
                }
                textures->registerTrustedSource(resource->path, {"sha1", "1", resource->fingerprint});
                registry->recordHits.fetch_add(1, std::memory_order_relaxed);
                entry->promise.set_value(resource);
                return {resource, 0};
            }
        }
        registry->sourceReadCalls.fetch_add(1, std::memory_order_relaxed);
        auto opened = reader.open ? reader.open() : std::nullopt;
        if (!opened) throw std::runtime_error("cannot open material resource: " + reader.path);
        const auto bytes = std::move(opened->first);
        const size_t size = opened->second;
        if (!bytes) throw std::runtime_error("cannot read material resource: " + reader.path);
        registry->sourceBytesRead.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
        auto resource = std::make_shared<ResolvedMaterialResource>();
        registry->sourceHashCalls.fetch_add(1, std::memory_order_relaxed);
        resource->fingerprint = OIIO::SHA1::digest(bytes.get(), size);
        resource->textureService = textures;
        resource->sourceBytes = size;
        if (fileBacked && textures->sourceIdentity(file, options) != version)
            throw std::runtime_error("material resource changed while reading: " + reader.path);
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
        if (fileBacked)
            fingerprint_records::Write(cacheDir, recordIdentity, {resource->fingerprint,
                resource->path.filename().string(), fingerprint_records::FileStamp(resource->path)});
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
