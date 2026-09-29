#include "../src/texture_service.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/oiioversion.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int kTextureCount = 20;
constexpr int kTextureSize = 512;

struct Args {
    std::filesystem::path fixtures;
    std::filesystem::path cache;
    uint64_t samples = 30000;
    int cacheMB = 32;
};

Args parse(int argc, char **argv)
{
    Args result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fixtures" && i + 1 < argc) result.fixtures = argv[++i];
        else if (arg == "--cache" && i + 1 < argc) result.cache = argv[++i];
        else if (arg == "--samples" && i + 1 < argc) result.samples = std::stoull(argv[++i]);
        else if (arg == "--cache-mb" && i + 1 < argc) result.cacheMB = std::stoi(argv[++i]);
        else throw std::runtime_error("usage: texture_cache_benchmark --fixtures DIR --cache DIR [--samples N] [--cache-mb N]");
    }
    if (result.fixtures.empty() || result.cache.empty() || !result.samples || result.cacheMB <= 0)
        throw std::runtime_error("invalid benchmark arguments");
    return result;
}

void setEnv(char const *name, std::string const &value)
{
#ifdef _WIN32
    if (_putenv_s(name, value.c_str()) != 0) throw std::runtime_error("cannot set environment variable");
#else
    if (setenv(name, value.c_str(), 1) != 0) throw std::runtime_error("cannot set environment variable");
#endif
}

uint32_t next(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float random01(uint32_t &state)
{
    return float(next(state) >> 8) * (1.0f / 16777216.0f);
}

void makeFixtures(std::filesystem::path const &directory)
{
    std::filesystem::create_directories(directory);
    for (int asset = 0; asset < kTextureCount; ++asset) {
        const auto path = directory / ("checker-" + std::to_string(asset) + ".exr");
        if (std::filesystem::exists(path)) continue;
        OIIO::ImageSpec spec(kTextureSize, kTextureSize, 4, OIIO::TypeDesc::FLOAT);
        spec.alpha_channel = 3;
        OIIO::ImageBuf image(spec);
        std::vector<float> pixels(size_t(kTextureSize) * kTextureSize * 4);
        for (int y = 0; y < kTextureSize; ++y) {
            for (int x = 0; x < kTextureSize; ++x) {
                const size_t offset = (size_t(y) * kTextureSize + x) * 4;
                const uint32_t pattern = uint32_t(x * 73856093u) ^
                    uint32_t(y * 19349663u) ^ uint32_t(asset * 83492791u);
                pixels[offset] = float(pattern & 255u) / 255.0f;
                pixels[offset + 1] = float((pattern >> 8) & 255u) / 255.0f;
                pixels[offset + 2] = float((pattern >> 16) & 255u) / 255.0f;
                pixels[offset + 3] = 1.0f;
            }
        }
        if (!image.set_pixels(OIIO::ROI(0, kTextureSize, 0, kTextureSize,
                                         0, 1, 0, 4),
                              OIIO::TypeDesc::FLOAT, pixels.data()) ||
            !image.write(path.string()))
            throw std::runtime_error("cannot create benchmark source " + path.string());
    }
}

std::string jsonEscape(std::string const &value)
{
    std::string result;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(char(c)); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c >= 0x20) result.push_back(char(c));
    }
    return result;
}

uint64_t hashFloat(uint64_t hash, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    hash ^= bits;
    return hash * 1099511628211ull;
}
}

int main(int argc, char **argv)
{
    try {
        const Args args = parse(argc, argv);
        std::filesystem::create_directories(args.cache);
        makeFixtures(args.fixtures);
        setEnv("PRISM_CACHE_DIR", args.cache.string());
        setEnv("PRISM_TEXTURE_CACHE_MB", std::to_string(args.cacheMB));

        auto service = prism::TextureService::Acquire();
        std::vector<std::shared_ptr<const prism::TextureResource>> textures;
        textures.reserve(kTextureCount);
        const auto prepareStart = std::chrono::steady_clock::now();
        for (int i = 0; i < kTextureCount; ++i) {
            auto texture = service->prepare(args.fixtures /
                ("checker-" + std::to_string(i) + ".exr"));
            if (texture->width() != kTextureSize || texture->height() != kTextureSize ||
                texture->channels() != 4)
                throw std::runtime_error("fixture dimensions or channels changed during preparation");
            textures.push_back(std::move(texture));
        }
        const double preparationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - prepareStart).count();

        bool dedupeIdentity = true;
        for (int i = 0; i < kTextureCount; ++i) {
            auto repeated = service->prepare(args.fixtures /
                ("checker-" + std::to_string(i) + ".exr"));
            dedupeIdentity = dedupeIdentity && repeated == textures[size_t(i)];
        }

        constexpr int channels = 3;
        uint64_t filteredHash = 1469598103934665603ull;
        uint64_t bilinearHash = 1469598103934665603ull;
        double errorSquared = 0.0;
        double maxError = 0.0;
        uint64_t values = 0;
        bool finite = true;
        uint32_t random = 0x741c29a5u;
        const auto lookupStart = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < args.samples; ++i) {
            const size_t textureIndex = std::min<size_t>(
                size_t(random01(random) * kTextureCount), kTextureCount - 1);
            prism::TextureLookup query;
            query.s = random01(random);
            query.t = random01(random);
            query.dsdx = 8.0f / kTextureSize;
            query.dtdx = 0.0f;
            query.dsdy = 0.0f;
            query.dtdy = 4.0f / kTextureSize;
            query.wrapS = prism::TextureWrap::Periodic;
            query.wrapT = prism::TextureWrap::Clamp;
            query.filtering = prism::TextureFiltering::Filtered;
            std::array<float, channels> filtered{};
            if (!service->lookup(*textures[textureIndex], query, channels, filtered.data()))
                throw std::runtime_error("filtered texture lookup failed");
            query.filtering = prism::TextureFiltering::Bilinear;
            query.dsdx = query.dtdx = query.dsdy = query.dtdy = 0.0f;
            std::array<float, channels> reference{};
            if (!service->lookup(*textures[textureIndex], query, channels, reference.data()))
                throw std::runtime_error("bilinear reference lookup failed");
            for (int c = 0; c < channels; ++c) {
                finite = finite && std::isfinite(filtered[c]) && std::isfinite(reference[c]);
                filteredHash = hashFloat(filteredHash, filtered[c]);
                bilinearHash = hashFloat(bilinearHash, reference[c]);
                const double delta = double(filtered[c]) - reference[c];
                errorSquared += delta * delta;
                maxError = std::max(maxError, std::abs(delta));
                ++values;
            }
        }
        const double lookupSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lookupStart).count();
        const auto stats = service->stats();
        const std::string oiioStats = service->textureStatistics();
        const std::string expectedVersion = std::string("ver ") + OIIO_VERSION_STRING;
        if (oiioStats.find(expectedVersion) == std::string::npos)
            throw std::runtime_error("runtime OpenImageIO version does not match compiled headers: expected " + expectedVersion);
        std::cout << std::setprecision(9)
            << "RESULT_JSON: {\"textureCount\":" << kTextureCount
            << ",\"textureSize\":" << kTextureSize
            << ",\"sourceFloatMiB\":" << (double(kTextureCount) * kTextureSize * kTextureSize * 4 * sizeof(float) / (1024.0 * 1024.0))
            << ",\"cacheBudgetMiB\":" << args.cacheMB
            << ",\"samples\":" << args.samples
            << ",\"preparationSeconds\":" << preparationSeconds
            << ",\"lookupSeconds\":" << lookupSeconds
            << ",\"filteredChecksum\":\"0x" << std::hex << filteredHash
            << "\",\"bilinearChecksum\":\"0x" << bilinearHash << std::dec
            << "\",\"finite\":" << (finite ? "true" : "false")
            << ",\"dedupeIdentity\":" << (dedupeIdentity ? "true" : "false")
            << ",\"filteredVsBilinearRms\":" << std::sqrt(errorSquared / std::max<uint64_t>(1, values))
            << ",\"filteredVsBilinearMax\":" << maxError
            << ",\"filteredVsBilinearMeaning\":\"difference between anisotropic mip-filtered and bilinear highest-resolution lookup modes; not error against ground truth\""
            << ",\"serviceStats\":{\"prepareCalls\":" << stats.prepareCalls
            << ",\"memoryCacheHits\":" << stats.memoryCacheHits
            << ",\"diskCacheHits\":" << stats.diskCacheHits
            << ",\"sourceBytesHashed\":" << stats.sourceBytesHashed
            << ",\"preparationNanoseconds\":" << stats.preparationNanoseconds << "}"
            << ",\"oiioStats\":\"" << jsonEscape(oiioStats) << "\"}\n";
        return finite && dedupeIdentity ? 0 : 2;
    } catch (std::exception const &error) {
        std::cerr << "texture_cache_benchmark: " << error.what() << '\n';
        return 1;
    }
}
