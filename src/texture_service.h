#pragma once

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/texture.h>

#include <filesystem>
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
    ResourceVersion const &resourceVersion() const noexcept;

private:
    friend class TextureService;
    struct Impl;
    explicit TextureResource(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

class TextureService final : public std::enable_shared_from_this<TextureService> {
public:
    static std::shared_ptr<TextureService> Acquire();
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
    // The calling thread's last lookup or readPixels failure.
    static std::string const &lastError() noexcept;
    OIIO::TextureSystem *textureSystem() const noexcept;
    TextureServiceStats stats() const noexcept;
    std::string textureStatistics() const;

private:
    TextureService();
    struct Impl;
    std::shared_ptr<const TextureResource> prepareImpl(
        std::filesystem::path const &source,
        TexturePrepareOptions const &options,
        std::string const &trustedFingerprint, bool ahead = false);
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism
