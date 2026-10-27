#pragma once

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/texture.h>

#include "color/color_management.h"
#include "renderer/renderer_options.h"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>

namespace prism {

enum class TextureFiltering { Filtered, Bilinear };
enum class TextureWrap { Default, Black, Clamp, Periodic, Mirror, PeriodicPow2, Shadow };

struct TextureLookup {
    float s = 0.0f, t = 0.0f;
    float dsdx = 0.0f, dtdx = 0.0f, dsdy = 0.0f, dtdy = 0.0f;
    int firstChannel = 0;
    TextureWrap wrapS = TextureWrap::Default;
    TextureWrap wrapT = TextureWrap::Default;
    TextureFiltering filtering = TextureFiltering::Filtered;
    bool nearest = false;
};

struct TexturePrepareOptions {
    // Decode exponent applied while preparing pixels; 1 leaves values unchanged.
    float gamma = 1.0f;
    float exposure = 0.0f;
    int firstChannel = 0;
    int channelCount = 0;
    TextureWrap wrapS = TextureWrap::Default;
    TextureWrap wrapT = TextureWrap::Default;
    // Resolver provenance partitions cache identity without changing path lookup.
    std::string resolverIdentity;
    // Caller supplied asset revision. Increment when a resolver knows the same
    // path/metadata now denotes different bytes.
    std::string sourceVersion;
    // Empty preserves source color-space metadata; nonempty tags the prepared image.
    std::string colorSpace;
    // Nonempty: convert the color channels (not alpha) from colorSpace into
    // colorManagement's working space, after gamma and exposure. It names
    // that working space (colorManagement->identity(), part of the cache
    // key); the prepared image is tagged with the working space. Empty (and
    // MDL textures, which MaterialX's generated code decodes) leaves texels
    // unconverted.
    std::string workingSpace;
    // The renderer's colour management, required with workingSpace.
    std::shared_ptr<const prism::color::ColorManagement> colorManagement;
    // -1 uses source metadata (with gray-alpha inference), -2 disables alpha,
    // otherwise identifies an explicit source alpha channel.
    int alphaChannel = -1;
    // Convert associated source color values to unassociated values before
    // gamma/exposure and mip generation. Inputs marked oiio:UnassociatedAlpha
    // are left unchanged. Defaults off for existing non-MDL callers.
    bool unassociateAlpha = false;
};

struct ResourceVersion {
    std::string resolvedIdentifier;
    std::string interpretationIdentity;
    std::string fingerprint;
    uint64_t generation = 0;
};

struct TrustedTextureFingerprint {
    std::string algorithm;
    std::string version;
    std::string digest;
};

struct TextureServiceStats {
    uint64_t prepareCalls = 0;
    uint64_t memoryCacheHits = 0;
    uint64_t diskCacheHits = 0;
    uint64_t resourceHashCalls = 0;
    uint64_t sourceBytesHashed = 0;
    uint64_t sourceWaitNanoseconds = 0;
    uint64_t contentWaitNanoseconds = 0;
    uint64_t preparationNanoseconds = 0;
    // Stage times summed over all preparations. Lock waits are counted only
    // in lockWaitNanoseconds, never in the stage that acquired the lock.
    uint64_t coldPreparations = 0;
    uint64_t inspectNanoseconds = 0;
    uint64_t decodeNanoseconds = 0;
    uint64_t pixelNanoseconds = 0;
    uint64_t mipNanoseconds = 0;
    uint64_t publishNanoseconds = 0;
    uint64_t trimNanoseconds = 0;
    uint64_t lockWaitNanoseconds = 0;
    // Writing GPU-ready copies (texture_gpu_file.h), during cold
    // preparations and for prepared textures that lacked one.
    uint64_t gpuWriteNanoseconds = 0;
    uint64_t gpuFilesWritten = 0;
    // .tx.exr files written in the background, and the time lookups and
    // readers waited for them.
    uint64_t backgroundWrites = 0;
    uint64_t backgroundWaitNanoseconds = 0;
    // prepareAhead calls, those that produced the resource (cold or from
    // disk), and later ordinary calls served from memory by such a resource.
    uint64_t aheadRequests = 0;
    uint64_t aheadPreparations = 0;
    uint64_t aheadMemoryHits = 0;
};

class TextureResource final {
public:
    int width() const noexcept;
    int height() const noexcept;
    int channels() const noexcept;
    std::filesystem::path const &sourcePath() const noexcept;
    std::filesystem::path const &preparedPath() const noexcept;
    // Where its GPU-ready copy is (or will be) written; empty without one.
    std::filesystem::path const &gpuCopyPath() const noexcept;
    ResourceVersion const &resourceVersion() const noexcept;

private:
    friend class TextureService;
    struct Impl;
    explicit TextureResource(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

// What a texture service is made with: a renderer's options, the cache root
// and disk budget the runtime's (renderer/runtime.h).
struct TextureServiceOptions {
    std::filesystem::path cacheDirectory; // prepared textures and fingerprints
    uint64_t diskBudgetBytes = 0;         // the prepared textures' share
    uint64_t memoryMiB = 0;               // OIIO's tile cache; 0: default
    unsigned openFiles = 0;               // OIIO's open files; 0: default
    unsigned writerThreads = 0;           // background writers; 0: hardware threads
    bool backgroundWrites = true;
    std::string compression;              // prepared files' compression; empty: zip:1
    bool operator==(TextureServiceOptions const &) const = default;

    // A renderer's: its limits and texture options with the runtime's cache
    // root and disk budget.
    static TextureServiceOptions For(RendererOptions const &options);
    // The runtime's (the first renderer's, else the environment's).
    static TextureServiceOptions FromRuntime();
};

class TextureService final : public std::enable_shared_from_this<TextureService> {
public:
    // The service for `options`: one instance per distinct options, shared
    // while someone holds it (renderers with the same options share one).
    static std::shared_ptr<TextureService> Acquire(TextureServiceOptions const &options);
    // The service of the runtime's options, for code without a renderer at
    // hand (MDL texture callbacks, material resources). A renderer whose
    // options match the runtime's holds the same instance.
    static std::shared_ptr<TextureService> Acquire();
    // An unshared service (tests).
    static std::shared_ptr<TextureService> Create(TextureServiceOptions const &options);
    TextureServiceOptions const &options() const noexcept;
    ~TextureService();
    TextureService(TextureService const &) = delete;
    TextureService &operator=(TextureService const &) = delete;

    std::shared_ptr<const TextureResource> prepare(
        std::filesystem::path const &source,
        TexturePrepareOptions const &options = {});
    std::shared_ptr<const TextureResource> prepareTrusted(
        std::filesystem::path const &source,
        TrustedTextureFingerprint const &fingerprint,
        TexturePrepareOptions const &options = {});
    // prepare() on behalf of a caller that expects the same request shortly,
    // such as material compilation. Only the statistics differ.
    std::shared_ptr<const TextureResource> prepareAhead(
        std::filesystem::path const &source,
        TexturePrepareOptions const &options = {});
    // Register a digest for renderer-owned immutable materialized files so
    // later preparation can skip rereading their bytes for each interpretation.
    void registerTrustedSource(std::filesystem::path const &source,
                               TrustedTextureFingerprint const &fingerprint);
    // A cheap process-local dependency stamp: metadata plus explicit resolver/
    // source version, interpretation, and invalidation generation. It is not a
    // content digest; callers must bump sourceVersion or invalidate after
    // same-size/same-mtime edits.
    std::string sourceIdentity(std::filesystem::path const &source,
                               TexturePrepareOptions const &options = {}) const;
    std::string sourceIdentityTrusted(
        std::filesystem::path const &source,
        TrustedTextureFingerprint const &fingerprint,
        TexturePrepareOptions const &options = {}) const;
    std::shared_ptr<const TextureResource> prepare(
        OIIO::ImageBuf const &source,
        TexturePrepareOptions const &options = {});
    // Removes an unreferenced prepared entry for the current source fingerprint.
    // Returns false while this or another process still holds the resource.
    bool invalidate(std::filesystem::path const &source,
                    TexturePrepareOptions const &options = {});

    // On failure these take OIIO's error message, which would otherwise
    // accumulate per thread without bound, and keep it for lastError().
    bool lookup(TextureResource const &resource, TextureLookup const &query,
                int channelCount, float *result) const;
    bool readPixels(TextureResource const &resource, int xbegin, int ybegin,
                    int xend, int yend, int firstChannel, int channelCount,
                    float *result) const;
    // The GPU-ready copy of `resource` (texture_gpu_file.h): its top level
    // at the source's precision, for GPU devices to upload without decoding
    // the float .tx.exr. A cache prepared before these copies existed gets
    // one from the source on first request. Empty when there is none (an
    // in-memory source, or a failed decode); `error` then says why.
    std::filesystem::path gpuImage(TextureResource const &resource, std::string *error = nullptr) const;
    // Observers of this service's prepared file textures that have a
    // GPU-ready copy, called on the preparing thread each time prepare(),
    // prepareTrusted() or prepareAhead() returns one (again for repeated
    // requests), so a GPU device can upload it while the scene is still
    // loading. Thread safe.
    using PreparedObserver = std::function<void(std::shared_ptr<const TextureResource> const &)>;
    int addPreparedObserver(PreparedObserver observer);
    void removePreparedObserver(int token);
    // The calling thread's last lookup or readPixels failure.
    static std::string const &lastError() noexcept;
    OIIO::TextureSystem *textureSystem() const noexcept;
    TextureServiceStats stats() const noexcept;
    std::string textureStatistics() const;

private:
    explicit TextureService(TextureServiceOptions options);
    // Tells the observers about a prepared resource with a GPU copy.
    std::shared_ptr<const TextureResource> published(std::shared_ptr<const TextureResource> resource);
    struct Impl;
    std::shared_ptr<const TextureResource> prepareImpl(
        std::filesystem::path const &source,
        TexturePrepareOptions const &options,
        std::string const &trustedFingerprint, bool ahead = false);
    // The resource's OIIO handle, opened on first use once its .tx.exr is
    // written (it may still be written in the background); null on failure.
    OIIO::TextureSystem::TextureHandle *readyHandle(TextureResource const &resource) const;
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism
