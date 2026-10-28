#include "../src/materialx/resolved_resource.h"

#include <chrono>
#include <atomic>
#include <exception>
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

std::string readBytes(std::filesystem::path const &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeBytes(std::filesystem::path const &path, std::string const &value)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(value.data(), static_cast<std::streamsize>(value.size())))
        throw std::runtime_error("cannot write resource fixture: " + path.string());
}

void setCacheEnvironment(std::filesystem::path const &path)
{
#ifdef _WIN32
    _putenv_s("PRISM_CACHE_DIR", path.string().c_str());
#else
    setenv("PRISM_CACHE_DIR", path.string().c_str(), 1);
#endif
}

PXR_NS::ArResolvedPath resolve(std::filesystem::path const &path)
{
    const auto result = PXR_NS::ArGetResolver().Resolve(path.string());
    if (result.empty()) throw std::runtime_error("filesystem resolver did not resolve fixture: " + path.string());
    return result;
}
}

int main()
{
    try {
        const auto root = std::filesystem::temp_directory_path() /
            ("prism-resolved-resource-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto cache = root / "cache";
        std::filesystem::create_directories(root);
        setCacheEnvironment(root / "texture-cache");
        const PXR_NS::ArResolverContext context;
        const std::string version = "asset-revision-1";

        const auto retainedSource = root / "retained.mtlx";
        writeBytes(retainedSource, "<materialx version=\"1.38\"/>\n");
        const auto retainedPath = resolve(retainedSource);
        const auto retainedStatsBefore = prism::materialx::GetResolvedMaterialResourceStats();
        auto first = prism::materialx::PrepareMaterialResource(
            retainedPath, context, version, cache, ".mtlx");
        const auto retainedSize = std::filesystem::file_size(retainedSource);
        require(first.resource && first.bytesRead == retainedSize,
                "first request did not report its source bytes");
        const auto retainedStatsAfterFirst = prism::materialx::GetResolvedMaterialResourceStats();
        require(retainedStatsAfterFirst.sourceReadCalls - retainedStatsBefore.sourceReadCalls == 1 &&
                retainedStatsAfterFirst.sourceHashCalls - retainedStatsBefore.sourceHashCalls == 1 &&
                retainedStatsAfterFirst.sourceBytesRead - retainedStatsBefore.sourceBytesRead == retainedSize,
                "first retained-resource request counters did not record one read/hash and its bytes");
        auto retained = prism::materialx::PrepareMaterialResource(
            retainedPath, context, version, cache, ".mtlx");
        require(retained.resource == first.resource && retained.bytesRead == 0,
                "retained same-version request reread the asset");
        const auto retainedStatsAfterHit = prism::materialx::GetResolvedMaterialResourceStats();
        require(retainedStatsAfterHit.sourceReadCalls == retainedStatsAfterFirst.sourceReadCalls &&
                retainedStatsAfterHit.sourceHashCalls == retainedStatsAfterFirst.sourceHashCalls &&
                retainedStatsAfterHit.sourceBytesRead == retainedStatsAfterFirst.sourceBytesRead,
                "retained same-version request incremented source read/hash counters");

        // Plain-file readers (glTF, OBJ) key and publish a file as USD's
        // default resolver does.
        require(PXR_NS::ArResolverContext().GetDebugString() == prism::materialx::kFileResolverIdentity,
                "the plain-file resolver identity differs from the default Ar context's");
        const auto plainSource = root / "plain.mtlx";
        writeBytes(plainSource, "<materialx name=\"plain\"/>\n");
        const auto viaFile = prism::materialx::PrepareMaterialResource(
            prism::materialx::FileResourceReader(std::filesystem::absolute(plainSource)), "", cache, ".mtlx");
        const auto viaAr = prism::materialx::PrepareMaterialResource(
            PXR_NS::ArResolvedPath(std::filesystem::absolute(plainSource).string()), PXR_NS::ArResolverContext(), "",
            cache, ".mtlx");
        require(viaFile.resource && viaAr.resource && viaFile.bytesRead == std::filesystem::file_size(plainSource) &&
                    viaFile.resource->path == viaAr.resource->path &&
                    viaFile.resource->fingerprint == viaAr.resource->fingerprint,
                "a plain-file reader published a file differently from the Ar reader");
        bool missingThrew = false;
        try {
            (void)prism::materialx::PrepareMaterialResource(
                prism::materialx::FileResourceReader(root / "missing.png"), "", cache, ".png");
        } catch (std::exception const &) { missingThrew = true; }
        require(missingThrew, "a missing plain file did not fail");

        const auto concurrentSource = root / "concurrent.mtlx";
        writeBytes(concurrentSource, "<materialx name=\"shared\">concurrent</materialx>");
        const auto concurrentPath = resolve(concurrentSource);
        const auto concurrentStatsBefore = prism::materialx::GetResolvedMaterialResourceStats();
        constexpr size_t requestCount = 8;
        std::vector<prism::materialx::MaterialResourceResult> results(requestCount);
        std::vector<std::exception_ptr> errors(requestCount);
        std::vector<std::thread> workers;
        std::atomic<size_t> ready{0};
        std::atomic<bool> start{false};
        for (size_t i = 0; i < requestCount; ++i) {
            workers.emplace_back([&, i] {
                ++ready;
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                try {
                    results[i] = prism::materialx::PrepareMaterialResource(
                        concurrentPath, context, version, cache, ".mtlx");
                } catch (...) { errors[i] = std::current_exception(); }
            });
        }
        while (ready.load(std::memory_order_acquire) != requestCount) std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto &worker : workers) worker.join();
        uint64_t concurrentBytesRead = 0;
        for (size_t i = 0; i < requestCount; ++i) {
            if (errors[i]) std::rethrow_exception(errors[i]);
            require(results[i].resource == results.front().resource,
                    "concurrent same-version requests returned different resources");
            concurrentBytesRead += results[i].bytesRead;
        }
        require(concurrentBytesRead == std::filesystem::file_size(concurrentSource),
                "concurrent waiters duplicated asset reads");
        const auto concurrentStatsAfter = prism::materialx::GetResolvedMaterialResourceStats();
        require(concurrentStatsAfter.sourceReadCalls - concurrentStatsBefore.sourceReadCalls == 1 &&
                concurrentStatsAfter.sourceHashCalls - concurrentStatsBefore.sourceHashCalls == 1 &&
                concurrentStatsAfter.sourceBytesRead - concurrentStatsBefore.sourceBytesRead == concurrentBytesRead &&
                concurrentStatsAfter.waitNanoseconds >= concurrentStatsBefore.waitNanoseconds,
                "concurrent waiter stats did not attribute one owner read/hash and cumulative waiting");

        const auto identicalA = root / "identical-a.mtlx";
        const auto identicalB = root / "identical-b.mtlx";
        const std::string identicalContents = "<materialx>identical-content</materialx>";
        writeBytes(identicalA, identicalContents);
        writeBytes(identicalB, identicalContents);
        const auto aliasesStatsBefore = prism::materialx::GetResolvedMaterialResourceStats();
        const auto aliasA = prism::materialx::PrepareMaterialResource(
            resolve(identicalA), context, version, cache, ".mtlx");
        const auto aliasB = prism::materialx::PrepareMaterialResource(
            resolve(identicalB), context, version, cache, ".mtlx");
        require(aliasA.bytesRead == identicalContents.size() &&
                aliasB.bytesRead == identicalContents.size(),
                "distinct source files did not each report their initial read");
        require(aliasA.resource->fingerprint == aliasB.resource->fingerprint &&
                aliasA.resource->path == aliasB.resource->path,
                "identical source content did not share its materialized artifact");
        const auto aliasesStatsAfter = prism::materialx::GetResolvedMaterialResourceStats();
        require(aliasesStatsAfter.sourceReadCalls - aliasesStatsBefore.sourceReadCalls == 2 &&
                aliasesStatsAfter.sourceHashCalls - aliasesStatsBefore.sourceHashCalls == 2 &&
                aliasesStatsAfter.sourceBytesRead - aliasesStatsBefore.sourceBytesRead ==
                    2 * identicalContents.size(),
                "distinct identical-content sources were not counted as two source reads and hashes");

        const auto reloadSource = root / "reload-same-metadata.mtlx";
        writeBytes(reloadSource, "<materialx>first-bytes</materialx>");
        const auto reloadResolved = resolve(reloadSource);
        const auto reloadOptionsIdentity = [&] {
            prism::TexturePrepareOptions options;
            options.resolverIdentity = context.GetDebugString();
            options.sourceVersion = version;
            return options;
        }();
        auto oldVersion = prism::materialx::PrepareMaterialResource(
            reloadResolved, context, version, cache, ".mtlx");
        const auto oldMaterializedBytes = readBytes(oldVersion.resource->path);
        const auto oldTime = std::filesystem::last_write_time(reloadSource);
        const auto oldSize = std::filesystem::file_size(reloadSource);
        writeBytes(reloadSource, "<materialx>other-bytes</materialx>");
        std::filesystem::last_write_time(reloadSource, oldTime);
        require(std::filesystem::file_size(reloadSource) == oldSize,
                "reload fixture changed size unexpectedly");
        auto textureService = prism::TextureService::Acquire();
        (void)textureService->invalidate(reloadSource, reloadOptionsIdentity);
        auto newVersion = prism::materialx::PrepareMaterialResource(
            reloadResolved, context, version, cache, ".mtlx");
        require(newVersion.bytesRead == oldSize &&
                newVersion.resource->fingerprint != oldVersion.resource->fingerprint &&
                newVersion.resource->path != oldVersion.resource->path,
                "explicit invalidation did not refresh same-metadata source content");
        require(readBytes(oldVersion.resource->path) == oldMaterializedBytes,
                "reload modified the retained prior materialized artifact");

        const auto corruptSource = root / "corrupt-artifact-source.mtlx";
        const std::string expectedCorruptContents = "<materialx>cache-check</materialx>";
        writeBytes(corruptSource, expectedCorruptContents);
        const auto corruptDigest = OIIO::SHA1::digest(
            expectedCorruptContents.data(), expectedCorruptContents.size());
        const auto corruptArtifact = cache / (corruptDigest + ".mtlx");
        writeBytes(corruptArtifact, "not-the-source-bytes");
        bool corruptRejected = false;
        bool clearCorruptionError = false;
        try {
            (void)prism::materialx::PrepareMaterialResource(
                resolve(corruptSource), context, "corruption-check-v1", cache, ".mtlx");
        } catch (std::exception const &error) {
            corruptRejected = true;
            clearCorruptionError = std::string(error.what()).find(
                "corrupt material resource cache artifact") != std::string::npos;
        }
        require(corruptRejected && clearCorruptionError &&
                readBytes(corruptArtifact) == "not-the-source-bytes",
                "corrupt cache artifact was trusted or overwritten in place");
        std::filesystem::remove(corruptArtifact);
        auto corruptRecovery = prism::materialx::PrepareMaterialResource(
            resolve(corruptSource), context, "corruption-check-v1", cache, ".mtlx");
        require(corruptRecovery.bytesRead == expectedCorruptContents.size() &&
                readBytes(corruptRecovery.resource->path) == expectedCorruptContents,
                "retry after removing corrupt cache artifact did not recover");

        const auto missingSource = root / "created-after-failure.mtlx";
        const PXR_NS::ArResolvedPath missingPath(missingSource.string());
        constexpr size_t waiterCount = 6;
        std::vector<std::exception_ptr> missingErrors(waiterCount);
        workers.clear();
        ready = 0;
        start = false;
        for (size_t i = 0; i < waiterCount; ++i) {
            workers.emplace_back([&, i] {
                ++ready;
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                try {
                    (void)prism::materialx::PrepareMaterialResource(
                        missingPath, context, version, cache, ".mtlx");
                } catch (...) { missingErrors[i] = std::current_exception(); }
            });
        }
        while (ready.load(std::memory_order_acquire) != waiterCount) std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto &worker : workers) worker.join();
        for (auto const &error : missingErrors)
            require(static_cast<bool>(error), "missing-resource waiter did not receive failure");
        writeBytes(missingSource, "created-later");
        auto recovered = prism::materialx::PrepareMaterialResource(
            resolve(missingSource), context, version, cache, ".mtlx");
        require(recovered.resource && recovered.bytesRead == std::string("created-later").size(),
                "resource registry stayed poisoned after a failed first read");

        std::cout << "resolved material resource registry checks passed\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "resolved material resource test: " << e.what() << '\n';
        return 1;
    }
}
