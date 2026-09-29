#include "../src/texture_service.h"

#include <OpenImageIO/imagebuf.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, char const *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<char> bytes(std::filesystem::path const &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void setEnv(char const *name, std::string const &value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
}

int main()
{
    try {
        const auto root = std::filesystem::temp_directory_path() / "prism-texture-service-test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
#ifdef _WIN32
        _putenv_s("PRISM_CACHE_DIR", root.string().c_str());
#else
        setenv("PRISM_CACHE_DIR", root.string().c_str(), 1);
#endif
        const auto source = root / "source.exr";
        OIIO::ImageSpec spec(2, 2, 3, OIIO::TypeDesc::FLOAT);
        OIIO::ImageBuf image(spec);
        std::array<float, 12> pixels{
            0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f,
            0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f};
        require(image.set_pixels(OIIO::ROI(0, 2, 0, 2, 0, 1, 0, 3),
                                 OIIO::TypeDesc::FLOAT, pixels.data()),
                "cannot create texture fixture");
        require(image.write(source.string()), "cannot write texture fixture");
        const auto original = bytes(source);

        auto service = prism::TextureService::Acquire();
        prism::TexturePrepareOptions prep;
        prep.exposure = 1.0f;
        const auto sourceStampBefore = service->sourceIdentity(source, prep);
        const auto hashStatsBefore = service->stats().sourceBytesHashed;
        auto resource = service->prepare(source, prep);
        const auto hashStatsAfterFirst = service->stats().sourceBytesHashed;
        auto gammaVariantOptions = prep;
        gammaVariantOptions.gamma = 2.0f;
        auto gammaVariant = service->prepare(source, gammaVariantOptions);
        auto same = service->prepare(source, prep);
        require(resource == same, "same prepared input did not reuse resource identity");
        require(hashStatsAfterFirst - hashStatsBefore == original.size() &&
                service->stats().sourceBytesHashed == hashStatsAfterFirst,
                "retained source fingerprint was recomputed on a cache hit");
        require(resource->resourceVersion().fingerprint.rfind("sha1:v1:", 0) == 0 &&
                resource->resourceVersion().fingerprint.size() == 48 &&
                resource->resourceVersion().generation == 0,
                "prepared resource omitted its immutable version identity");
        require(gammaVariant->resourceVersion().fingerprint ==
                    resource->resourceVersion().fingerprint &&
                gammaVariant->resourceVersion().interpretationIdentity !=
                    resource->resourceVersion().interpretationIdentity,
                "content fingerprint was coupled to texture interpretation");
        require(resource->width() == 2 && resource->height() == 2 && resource->channels() == 3,
                "prepared texture dimensions or channel count changed");
        require(bytes(source) == original, "preparation modified the source file");

        std::array<std::shared_ptr<const prism::TextureResource>, 6> concurrent;
        std::array<std::thread, 6> workers;
        for (size_t i = 0; i < workers.size(); ++i)
            workers[i] = std::thread([&, i] { concurrent[i] = service->prepare(source, prep); });
        for (auto &worker : workers) worker.join();
        for (auto const &item : concurrent)
            require(item == resource, "concurrent prepare did not deduplicate resource handles");
        require(service->stats().sourceBytesHashed == hashStatsAfterFirst &&
                gammaVariant->preparedPath() != resource->preparedPath(),
                "interpretation variant rehashed source bytes or reused prepared interpretation");

        const auto raceSource = root / "concurrent-first-use.exr";
        require(std::filesystem::copy_file(source, raceSource),
                "cannot create concurrent fingerprint fixture");
        const auto independentSource = root / "independent-first-use.exr";
        require(std::filesystem::copy_file(source, independentSource),
                "cannot create independent fingerprint fixture");
        const uint64_t raceBytes = std::filesystem::file_size(raceSource);
        const uint64_t independentBytes = std::filesystem::file_size(independentSource);
        const auto beforeRaceHashes = service->stats().sourceBytesHashed;
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::array<std::thread, 6> firstUseWorkers;
        std::array<std::shared_ptr<const prism::TextureResource>, 6> firstUseResources;
        for (size_t i = 0; i < firstUseWorkers.size(); ++i) {
            firstUseWorkers[i] = std::thread([&, i] {
                ++ready;
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                firstUseResources[i] = service->prepare(i < 3 ? raceSource : independentSource, prep);
            });
        }
        while (ready.load(std::memory_order_acquire) != static_cast<int>(firstUseWorkers.size()))
            std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto &worker : firstUseWorkers) worker.join();
        for (size_t i = 1; i < 3; ++i)
            require(firstUseResources[i] == firstUseResources.front(),
                    "first-use concurrent prep did not deduplicate");
        for (size_t i = 4; i < 6; ++i)
            require(firstUseResources[i] == firstUseResources[3],
                    "independent concurrent prep did not deduplicate within its source");
        require(firstUseResources.front() != firstUseResources[3] &&
                firstUseResources.front()->preparedPath() == firstUseResources[3]->preparedPath() &&
                firstUseResources.front()->sourcePath() == std::filesystem::weakly_canonical(raceSource) &&
                firstUseResources[3]->sourcePath() == std::filesystem::weakly_canonical(independentSource) &&
                firstUseResources[3]->resourceVersion().resolvedIdentifier ==
                    std::filesystem::weakly_canonical(independentSource).generic_string() &&
                service->stats().sourceBytesHashed - beforeRaceHashes == raceBytes + independentBytes,
                "content aliases lost per-source identity or independent prep rehashed bytes");

        const auto trustedSource = root / "renderer-owned-copy.exr";
        require(std::filesystem::copy_file(source, trustedSource),
                "cannot create trusted-source fixture");
        const auto rawFingerprint = resource->resourceVersion().fingerprint;
        require(rawFingerprint.rfind("sha1:v1:", 0) == 0,
                "unexpected raw fingerprint encoding for trusted alias fixture");
        service->registerTrustedSource(trustedSource,
            {"sha1", "1", rawFingerprint.substr(std::string("sha1:v1:").size())});
        const auto beforeTrustedPrepare = service->stats().sourceBytesHashed;
        auto trustedPrepared = service->prepare(trustedSource, prep);
        auto trustedInterpretation = prep;
        trustedInterpretation.gamma = 2.0f;
        auto trustedPreparedVariant = service->prepare(trustedSource, trustedInterpretation);
        require(service->stats().sourceBytesHashed == beforeTrustedPrepare &&
                trustedPrepared->preparedPath() == resource->preparedPath() &&
                trustedPrepared->preparedPath() != trustedPreparedVariant->preparedPath(),
                "registered trusted source missed content aliasing, rehashed, or lost interpretation partitioning");

        auto versionedOptions = prep;
        versionedOptions.sourceVersion = "asset-revision-1";
        require(service->sourceIdentity(source, versionedOptions) != sourceStampBefore,
                "explicit source version did not partition the dependency identity");
        auto versionedNormal = service->prepare(source, versionedOptions);
        require(versionedNormal->resourceVersion().fingerprint ==
                    resource->resourceVersion().fingerprint &&
                versionedNormal->resourceVersion().interpretationIdentity !=
                    resource->resourceVersion().interpretationIdentity &&
                versionedNormal->preparedPath() != resource->preparedPath(),
                "source revision changed the content fingerprint or reused the old recipe");
        versionedNormal.reset();
        auto trusted = service->prepareTrusted(source,
            {"SHA256", "test-v1", "known-content-token"}, versionedOptions);
        require(trusted->resourceVersion().fingerprint != resource->resourceVersion().fingerprint,
                "trusted content fingerprint did not partition prepared identity");
        trusted.reset();

        std::array<float, 3> first{};
        require(service->readPixels(*resource, 0, 0, 1, 1, 0, 3, first.data()),
                "cannot read prepared texture pixels");
        require(std::abs(first[0] - 0.2f) < 1e-5f &&
                std::abs(first[1] - 0.4f) < 1e-5f &&
                std::abs(first[2] - 0.6f) < 1e-5f,
                "exposure was not applied before sampling");

        prism::TextureLookup query;
        query.s = 0.25f;
        query.t = 0.25f;
        query.wrapS = prism::TextureWrap::Periodic;
        query.wrapT = prism::TextureWrap::Clamp;
        query.filtering = prism::TextureFiltering::Bilinear;
        std::array<float, 3> sampled{};
        require(service->lookup(*resource, query, 3, sampled.data()),
                "TextureSystem lookup failed");
        for (int c = 0; c < 3; ++c)
            require(std::abs(sampled[c] - first[c]) < 1e-4f,
                    "bilinear texel-center sample differs from prepared pixels");
        query.s = 1.25f;
        std::array<float, 3> wrapped{};
        require(service->lookup(*resource, query, 3, wrapped.data()),
                "periodic TextureSystem lookup failed");
        for (int c = 0; c < 3; ++c)
            require(std::abs(wrapped[c] - sampled[c]) < 1e-4f,
                    "periodic S wrapping did not repeat the texture");

        OIIO::ImageSpec alphaSpec(1, 1, 4, OIIO::TypeDesc::FLOAT);
        alphaSpec.alpha_channel = 3;
        OIIO::ImageBuf alphaImage(alphaSpec);
        std::array<float, 4> alphaValues{0.5f, 0.25f, 0.75f, 0.8f};
        require(alphaImage.set_pixels(OIIO::ROI(0, 1, 0, 1, 0, 1, 0, 4),
                                      OIIO::TypeDesc::FLOAT, alphaValues.data()),
                "cannot create alpha texture fixture");
        const auto alphaPath = root / "alpha.exr";
        require(alphaImage.write(alphaPath.string()), "cannot write alpha texture fixture");
        prism::TexturePrepareOptions colorOptions;
        colorOptions.gamma = 2.0f;
        colorOptions.exposure = 1.0f;
        auto alphaResource = service->prepare(alphaPath, colorOptions);
        std::array<float, 4> colorResult{};
        require(service->readPixels(*alphaResource, 0, 0, 1, 1, 0, 4, colorResult.data()),
                "cannot read gamma texture fixture");
        require(std::abs(colorResult[0] - 0.5f) < 1e-5f &&
                std::abs(colorResult[1] - 0.125f) < 1e-5f &&
                std::abs(colorResult[2] - 1.125f) < 1e-5f &&
                std::abs(colorResult[3] - 0.8f) < 1e-5f,
                "gamma/exposure did not preserve alpha or transform RGB correctly");
        OIIO::ImageBuf alphaSource(alphaPath.string());
        require(alphaSource.read(0, 0, true, OIIO::TypeDesc::FLOAT),
                "cannot reread alpha source for associated-alpha fixture");
        require(alphaSource.spec().get_int_attribute("oiio:UnassociatedAlpha", 0) == 0,
                "EXR fixture unexpectedly marks its alpha as unassociated");
        auto unassociateOptions = colorOptions;
        unassociateOptions.unassociateAlpha = true;
        auto unassociated = service->prepare(alphaPath, unassociateOptions);
        require(unassociated->preparedPath() != alphaResource->preparedPath(),
                "unassociated-alpha interpretation did not partition cache identity");
        std::array<float, 4> unassociatedValues{};
        require(service->readPixels(*unassociated, 0, 0, 1, 1, 0, 4,
                                    unassociatedValues.data()),
                "cannot read unassociated-alpha output");
        require(std::abs(unassociatedValues[0] - 0.78125f) < 1e-4f &&
                std::abs(unassociatedValues[1] - 0.1953125f) < 1e-4f &&
                std::abs(unassociatedValues[2] - 1.7578125f) < 1e-4f &&
                std::abs(unassociatedValues[3] - 0.8f) < 1e-5f,
                "associated alpha was not removed before gamma/exposure while preserving alpha");

        OIIO::ImageSpec pngSpec(1, 1, 4, OIIO::TypeDesc::UINT8);
        pngSpec.channelnames = {"R", "G", "B", "A"};
        pngSpec.alpha_channel = 3;
        pngSpec.attribute("oiio:UnassociatedAlpha", 1);
        OIIO::ImageBuf pngSource(pngSpec);
        const std::array<unsigned char, 4> pngValues{128, 64, 32, 128};
        require(pngSource.set_pixels(OIIO::ROI(0, 1, 0, 1, 0, 1, 0, 4),
                                     OIIO::TypeDesc::UINT8, pngValues.data()),
                "cannot create unassociated PNG fixture");
        const auto pngPath = root / "unassociated.png";
        require(pngSource.write(pngPath.string()), "cannot write unassociated PNG fixture");
        prism::TexturePrepareOptions pngUnassociateOptions;
        pngUnassociateOptions.unassociateAlpha = true;
        auto pngUnassociated = service->prepare(pngPath, pngUnassociateOptions);
        std::array<float, 4> pngActual{};
        const std::array<float, 4> pngExpected{
            128.0f / 255.0f, 64.0f / 255.0f, 32.0f / 255.0f, 128.0f / 255.0f};
        require(service->readPixels(*pngUnassociated, 0, 0, 1, 1, 0, 4,
                                    pngActual.data()),
                "cannot read prepared straight-alpha PNG values");
        for (int c = 0; c < 4; ++c) {
            if (std::abs(pngActual[c] - pngExpected[c]) >= 1e-5f)
                std::cerr << "PNG straight-alpha mismatch channel " << c << " actual="
                          << pngActual[c] << " expected=" << pngExpected[c] << '\n';
            require(std::abs(pngActual[c] - pngExpected[c]) < 1e-5f,
                    "PNG reader did not preserve straight-alpha source values");
        }
        auto associatedEmbedded = service->prepare(alphaImage, colorOptions);
        auto unassociatedEmbedded = service->prepare(alphaImage, unassociateOptions);
        require(associatedEmbedded->preparedPath() != unassociatedEmbedded->preparedPath(),
                "in-memory unassociated-alpha option did not partition cache identity");
        auto alphaOverrideOptions = colorOptions;
        alphaOverrideOptions.alphaChannel = 0;
        alphaOverrideOptions.resolverIdentity = "resolver-A";
        alphaOverrideOptions.colorSpace = "linear";
        auto alphaOverride = service->prepare(alphaPath, alphaOverrideOptions);
        std::array<float, 4> overriddenValues{};
        require(service->readPixels(*alphaOverride, 0, 0, 1, 1, 0, 4, overriddenValues.data()),
                "cannot read explicit-alpha texture");
        require(std::abs(overriddenValues[0] - alphaValues[0]) < 1e-5f &&
                std::abs(overriddenValues[3] - 1.28f) < 1e-4f,
                "gamma/exposure did not skip the explicitly selected alpha channel");
        auto defaultAlphaVariant = service->prepare(alphaPath, colorOptions);
        require(defaultAlphaVariant->preparedPath() != alphaOverride->preparedPath(),
                "explicit alpha interpretation did not partition cache identity");
        auto resolverOptions = alphaOverrideOptions;
        resolverOptions.resolverIdentity = "resolver-B";
        auto resolverVariant = service->prepare(alphaPath, resolverOptions);
        require(resolverVariant->preparedPath() != alphaOverride->preparedPath(),
                "resolver identity did not partition prepared cache identity");
        auto colorSpaceOptions = alphaOverrideOptions;
        colorSpaceOptions.colorSpace = "sRGB";
        auto colorSpaceVariant = service->prepare(alphaPath, colorSpaceOptions);
        require(colorSpaceVariant->preparedPath() != alphaOverride->preparedPath(),
                "color-space interpretation did not partition prepared cache identity");

        OIIO::ImageSpec windowSpec(2, 2, 3, OIIO::TypeDesc::FLOAT);
        windowSpec.x = 2;
        windowSpec.y = 1;
        windowSpec.full_x = 0;
        windowSpec.full_y = 0;
        windowSpec.full_width = 4;
        windowSpec.full_height = 4;
        OIIO::ImageBuf windowImage(windowSpec);
        std::array<float, 12> windowPixels{-1,2,3, 4,5,6, 7,8,9, 10,11,12};
        require(windowImage.set_pixels(OIIO::ROI(2, 4, 1, 3, 0, 1, 0, 3),
                                       OIIO::TypeDesc::FLOAT, windowPixels.data()),
                "cannot create data-window fixture");
        const auto windowPath = root / "window.exr";
        require(windowImage.write(windowPath.string()), "cannot write data-window fixture");
        auto windowResource = service->prepare(windowPath);
        require(windowResource->width() == 4 && windowResource->height() == 4,
                "source display window dimensions were not preserved");
        std::array<float, 3> outside{}, inside{};
        require(service->readPixels(*windowResource, 0, 0, 1, 1, 0, 3, outside.data()) &&
                service->readPixels(*windowResource, 2, 1, 3, 2, 0, 3, inside.data()),
                "cannot read normalized data-window texture");
        require(outside[0] == 0.0f && outside[1] == 0.0f && outside[2] == 0.0f &&
                std::abs(inside[0] + 1.0f) < 1e-5f &&
                std::abs(inside[1] - 2.0f) < 1e-5f &&
                std::abs(inside[2] - 3.0f) < 1e-5f,
                "data-window pixels were cropped or offset incorrectly");

        prism::TexturePrepareOptions expandedOptions;
        expandedOptions.firstChannel = 1;
        expandedOptions.channelCount = 4;
        auto expanded = service->prepare(source, expandedOptions);
        require(expanded->channels() == 4, "requested channel expansion was not prepared");
        std::array<float, 4> expandedPixel{};
        require(service->readPixels(*expanded, 0, 0, 1, 1, 0, 4, expandedPixel.data()),
                "cannot read expanded texture channels");
        require(std::abs(expandedPixel[0] - 0.2f) < 1e-5f &&
                std::abs(expandedPixel[1] - 0.3f) < 1e-5f &&
                expandedPixel[2] == 0.0f && expandedPixel[3] == 0.0f,
                "channel crop and zero expansion are incorrect");

        auto embedded = service->prepare(image);
        auto embeddedAgain = service->prepare(image);
        require(embedded->preparedPath() == embeddedAgain->preparedPath(),
                "same in-memory pixel content did not produce a stable cache identity");
        require(embedded->sourcePath().empty(), "in-memory resource reports a removed source path");
        prism::TexturePrepareOptions embeddedGammaOptions;
        embeddedGammaOptions.gamma = 2.0f;
        auto embeddedGamma = service->prepare(image, embeddedGammaOptions);
        require(embeddedGamma->resourceVersion().fingerprint ==
                    embedded->resourceVersion().fingerprint &&
                embeddedGamma->preparedPath() != embedded->preparedPath(),
                "in-memory content fingerprint changed with interpretation");

        pixels[0] = 0.8f;
        OIIO::ImageBuf changed(spec);
        require(changed.set_pixels(OIIO::ROI(0, 2, 0, 2, 0, 1, 0, 3),
                                   OIIO::TypeDesc::FLOAT, pixels.data()),
                "cannot create changed texture fixture");
        const auto changedPath = root / "changed.exr";
        require(changed.write(changedPath.string()), "cannot write changed texture fixture");
        auto changedResource = service->prepare(changedPath, prep);
        require(changedResource->preparedPath() != resource->preparedPath(),
                "changed source content reused the old cache entry");

        bool missingFailed = false;
        try { (void)service->prepare(root / "missing-source.exr"); }
        catch (std::exception const &) { missingFailed = true; }
        require(missingFailed, "missing texture source was accepted");

        prism::TexturePrepareOptions invalidationOptions;
        auto invalidationResource = service->prepare(source, invalidationOptions);
        const auto invalidationPath = invalidationResource->preparedPath();
        const auto oldResourceVersion = invalidationResource->resourceVersion();
        const auto oldIdentity = service->sourceIdentity(source, invalidationOptions);
        require(!service->invalidate(source, invalidationOptions),
                "invalidation removed a currently referenced prepared resource");
        require(service->sourceIdentity(source, invalidationOptions) != oldIdentity,
                "invalidation did not advance the dependency generation while pinned");
        require(invalidationResource->resourceVersion().generation == oldResourceVersion.generation &&
                std::filesystem::exists(invalidationPath),
                "invalidation mutated an already-published resource snapshot");
        invalidationResource.reset();
        require(service->invalidate(source, invalidationOptions) &&
                !std::filesystem::exists(invalidationPath),
                "explicit invalidation did not remove an unreferenced cached resource");
        auto invalidationReload = service->prepare(source, invalidationOptions);
        require(invalidationReload->resourceVersion().generation == oldResourceVersion.generation + 2 &&
                invalidationReload->preparedPath() != invalidationPath &&
                !std::filesystem::exists(invalidationPath),
                "explicit invalidation did not remove the old file and publish a new generation");

        const auto reloadSource = root / "same-metadata.ppm";
        auto writePpm = [&](unsigned char r, unsigned char g, unsigned char b) {
            std::ofstream out(reloadSource, std::ios::binary | std::ios::trunc);
            out << "P6\n1 1\n255\n";
            const std::array<unsigned char, 3> value{r, g, b};
            out.write(reinterpret_cast<char const *>(value.data()), value.size());
        };
        writePpm(10, 20, 30);
        auto reloadFirst = service->prepare(reloadSource);
        const auto originalStamp = std::filesystem::last_write_time(reloadSource);
        const auto originalSize = std::filesystem::file_size(reloadSource);
        const auto cheapIdentityBeforeMutation = service->sourceIdentity(reloadSource);
        writePpm(40, 50, 60);
        std::filesystem::last_write_time(reloadSource, originalStamp);
        require(std::filesystem::file_size(reloadSource) == originalSize &&
                service->sourceIdentity(reloadSource) == cheapIdentityBeforeMutation,
                "same-metadata reload fixture did not preserve its cheap dependency stamp");
        const auto oldReloadVersion = reloadFirst->resourceVersion();
        reloadFirst.reset();
        require(service->invalidate(reloadSource), "same-metadata source invalidation failed");
        auto reloadSecond = service->prepare(reloadSource);
        require(reloadSecond->resourceVersion().generation == oldReloadVersion.generation + 1 &&
                reloadSecond->resourceVersion().fingerprint != oldReloadVersion.fingerprint,
                "explicit reload failed to detect changed bytes with unchanged metadata");

        const auto retainedFingerprint=reloadSecond->resourceVersion().fingerprint;
        service->registerTrustedSource(reloadSource,
            {"sha1","1",retainedFingerprint.substr(std::string("sha1:v1:").size())});
        require(std::filesystem::remove(reloadSource),"cannot remove reload fixture");
        service->invalidate(reloadSource);
        writePpm(70,80,90);
        std::filesystem::last_write_time(reloadSource,originalStamp);
        auto recreated=service->prepare(reloadSource);
        require(recreated->resourceVersion().generation==
                    reloadSecond->resourceVersion().generation+1 &&
                recreated->resourceVersion().fingerprint!=retainedFingerprint &&
                reloadSecond->resourceVersion().fingerprint==retainedFingerprint,
                "missing-source reload reused a stale trusted digest or mutated a retained version");
        recreated.reset();

        changedResource.reset();
        alphaResource.reset();
        unassociated.reset();
        pngUnassociated.reset();
        associatedEmbedded.reset();
        unassociatedEmbedded.reset();
        defaultAlphaVariant.reset();
        alphaOverride.reset();
        resolverVariant.reset();
        colorSpaceVariant.reset();
        windowResource.reset();
        for (auto &item : concurrent) item.reset();
        for (auto &item : firstUseResources) item.reset();
        trustedPrepared.reset();
        trustedPreparedVariant.reset();
        expanded.reset();
        invalidationReload.reset();
        reloadSecond.reset();
        trusted.reset();
        embedded.reset();
        embeddedAgain.reset();
        embeddedGamma.reset();
        resource.reset();
        same.reset();
        gammaVariant.reset();

        const auto cacheRootA = root / "root-switch-a";
        const auto cacheRootB = root / "root-switch-b";
        setEnv("PRISM_CACHE_DIR", cacheRootA.string());
        auto rootPinned = service->prepare(source);
        setEnv("PRISM_CACHE_DIR", cacheRootB.string());
        auto rootSwitched = service->prepare(source);
        require(rootPinned->preparedPath().parent_path() == cacheRootA &&
                rootSwitched->preparedPath().parent_path() == cacheRootB &&
                rootPinned->preparedPath() != rootSwitched->preparedPath(),
                "live memory-cached texture crossed cache roots or pinned the wrong root");
        rootPinned.reset();
        rootSwitched.reset();

        const auto evictionCache = root / "eviction-cache";
        setEnv("PRISM_CACHE_DIR", evictionCache.string());
        setEnv("PRISM_DISK_CACHE_GB", "0.00000001");
        auto pinnedA = service->prepare(source);
        auto pinnedB = service->prepare(changedPath);
        const auto pinnedAPath = pinnedA->preparedPath();
        const auto pinnedBPath = pinnedB->preparedPath();
        require(std::filesystem::exists(pinnedAPath) && std::filesystem::exists(pinnedBPath),
                "prepared files disappeared while their resources were pinned");
        auto pinnedC = service->prepare(alphaPath);
        require(std::filesystem::exists(pinnedAPath) && std::filesystem::exists(pinnedBPath),
                "disk eviction removed a resource still referenced in this process");
        pinnedA.reset();
        pinnedB.reset();
        auto trigger = service->prepare(windowPath);
        require(!std::filesystem::exists(pinnedAPath) && !std::filesystem::exists(pinnedBPath),
                "unreferenced old cache files were not evicted under a constrained budget");
        require(std::filesystem::exists(pinnedC->preparedPath()) &&
                std::filesystem::exists(trigger->preparedPath()),
                "eviction removed one of the currently pinned resources");
        const auto reloadedPath = pinnedAPath;
        pinnedC.reset();
        trigger.reset();
        auto reloaded = service->prepare(source);
        require(reloaded->preparedPath() == reloadedPath && std::filesystem::exists(reloadedPath),
                "evicted texture was not prepared again on reload");
        reloaded.reset();
        service.reset();
        setEnv("PRISM_DISK_CACHE_GB", "10");
        std::filesystem::remove_all(root);
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "texture_service: " << error.what() << '\n';
        return 1;
    }
}
