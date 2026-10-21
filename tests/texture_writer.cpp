// Prepared-texture writer: its mip chain and file must match OIIO's
// make_texture, and a failed write must fail instead of hanging (see
// src/texture_writer.h).

#include "../src/texture_writer.h"
#include "../src/texture_gpu_file.h"
#include "../src/fingerprint_records.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/texture.h>
#include <tbb/parallel_for.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string const &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void setEnv(char const *name, char const *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

std::vector<float> randomImage(int width, int height, int channels, unsigned seed, bool quantized)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<float> pixels(size_t(width) * height * channels);
    for (float &v : pixels) {
        v = unit(rng);
        if (quantized) v = float(int(v * 255.0f)) / 255.0f;
        else v *= 4.0f;
    }
    return pixels;
}

OIIO::ImageSpec textureSpec(int width, int height, int channels)
{
    OIIO::ImageSpec spec(width, height, channels, OIIO::TypeDesc::FLOAT);
    if (channels == 2 || channels == 4) spec.alpha_channel = channels - 1;
    spec.tile_width = spec.tile_height = 64;
    spec.attribute("compression", prism::texture_writer::PreparedCompression());
    spec.attribute("planarconfig", "contig");
    spec.attribute("wrapmodes", "black,black");
    spec.attribute("textureformat", "Plain Texture");
    spec.attribute("fovcot", float(width) / float(height));
    spec.attribute("openexr:roundingmode", 0);
    return spec;
}

// make_texture as TextureService called it before texture_writer.
bool makeTexture(std::vector<float> &pixels, int width, int height, int channels,
                 std::filesystem::path const &path)
{
    OIIO::ImageSpec spec(width, height, channels, OIIO::TypeDesc::FLOAT);
    if (channels == 2 || channels == 4) spec.alpha_channel = channels - 1;
    OIIO::ImageBuf image(spec, OIIO::span<float>(pixels.data(), pixels.size()));
    OIIO::ImageSpec config;
    config.format = OIIO::TypeDesc::FLOAT;
    config.tile_width = config.tile_height = 64;
    config.attribute("maketx:resize", 0);
    config.attribute("maketx:forcefloat", 1);
    config.attribute("maketx:fileformatname", "openexr");
    config.attribute("maketx:nomipmap", 0);
    return OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture, image, path.string(), config);
}

std::vector<std::vector<float>> readLevels(std::filesystem::path const &path, OIIO::ImageSpec *top)
{
    std::vector<std::vector<float>> levels;
    auto input = OIIO::ImageInput::open(path.string());
    if (!input) return levels;
    if (top) *top = input->spec();
    for (int level = 0; input->seek_subimage(0, level); ++level) {
        OIIO::ImageSpec const &spec = input->spec();
        std::vector<float> pixels(size_t(spec.width) * spec.height * spec.nchannels);
        if (!input->read_image(0, level, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data())) break;
        levels.push_back(std::move(pixels));
    }
    return levels;
}

void compareWithMakeTexture(std::filesystem::path const &dir, int width, int height, int channels, bool quantized)
{
    const std::string name = std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(channels);
    std::vector<float> pixels = randomImage(width, height, channels, unsigned(width * 31 + height * 7 + channels), quantized);
    std::vector<float> copy = pixels;
    const auto reference = dir / ("make_" + name + ".exr");
    const auto written = dir / ("ours_" + name + ".exr");
    check(makeTexture(copy, width, height, channels, reference), name + ": make_texture failed: " + OIIO::geterror());
    std::vector<prism::texture_writer::Level> levels(1);
    levels[0] = {width, height, pixels};
    prism::texture_writer::BuildMipChain(levels, channels);
    std::string error;
    check(prism::texture_writer::WriteMipmappedExr(written, textureSpec(width, height, channels), levels, &error),
          name + ": write failed: " + error);
    OIIO::ImageSpec a, b;
    const auto expected = readLevels(reference, &a);
    const auto actual = readLevels(written, &b);
    check(!expected.empty() && expected.size() == actual.size(),
          name + ": level count " + std::to_string(expected.size()) + " vs " + std::to_string(actual.size()));
    check(levels.size() == expected.size(), name + ": built level count");
    for (size_t l = 0; l < std::min(expected.size(), actual.size()); ++l) {
        check(expected[l].size() == actual[l].size(), name + ": level size " + std::to_string(l));
        if (expected[l].size() != actual[l].size()) continue;
        size_t differ = 0;
        for (size_t i = 0; i < expected[l].size(); ++i)
            if (std::memcmp(&expected[l][i], &actual[l][i], sizeof(float)) != 0) ++differ;
        check(differ == 0, name + ": level " + std::to_string(l) + " differs in " + std::to_string(differ) + " values");
    }
    check(a.tile_width == b.tile_width && a.tile_height == b.tile_height, name + ": tile size");
    check(a.alpha_channel == b.alpha_channel, name + ": alpha channel");
    check(a.channelnames == b.channelnames, name + ": channel names");
    for (char const *attribute : {"wrapmodes", "textureformat", "openexr:roundingmode"})
        check(a.get_string_attribute(attribute) == b.get_string_attribute(attribute),
              name + ": attribute " + attribute + " '" + std::string(a.get_string_attribute(attribute)) +
              "' vs '" + std::string(b.get_string_attribute(attribute)) + "'");
    check(a.get_float_attribute("fovcot") == b.get_float_attribute("fovcot"), name + ": fovcot");

    // The texture system's filtered lookups agree exactly.
    auto system = OIIO::TextureSystem::create(false);
    OIIO::TextureOpt options;
    options.mipmode = OIIO::TextureOpt::MipModeAniso;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> unit(-0.25f, 1.25f), footprint(0.0f, 0.05f);
    size_t lookupsDiffer = 0;
    for (int i = 0; i < 2000; ++i) {
        const float s = unit(rng), t = unit(rng);
        const float dsdx = footprint(rng), dtdx = footprint(rng) * 0.3f, dsdy = footprint(rng) * 0.2f,
                    dtdy = footprint(rng);
        float ra[4]{}, rb[4]{};
        system->texture(OIIO::ustring(reference.string()), options, s, t, dsdx, dtdx, dsdy, dtdy, channels, ra);
        system->texture(OIIO::ustring(written.string()), options, s, t, dsdx, dtdx, dsdy, dtdy, channels, rb);
        if (std::memcmp(ra, rb, sizeof(float) * channels) != 0) ++lookupsDiffer;
    }
    check(lookupsDiffer == 0, name + ": " + std::to_string(lookupsDiffer) + " of 2000 lookups differ");
    OIIO::TextureSystem::destroy(system);
}

} // namespace

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / "prism_texture_writer_test";
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    setEnv("PRISM_TEST_TEXTURE_WRITE_FAIL_AFTER", nullptr);

    // Even sizes (the 2x2 average), odd and one-texel-wide levels (the
    // bilinear path), channel counts 1-4, quantized and float data.
    compareWithMakeTexture(dir, 256, 256, 3, true);
    compareWithMakeTexture(dir, 512, 128, 4, false);
    compareWithMakeTexture(dir, 300, 200, 1, true);
    compareWithMakeTexture(dir, 257, 129, 2, false);
    compareWithMakeTexture(dir, 1, 77, 3, false);
    compareWithMakeTexture(dir, 130, 1, 4, true);
    compareWithMakeTexture(dir, 1000, 750, 3, false);

    // A write that fails part way returns an error, quickly, instead of
    // hanging in OpenEXR's tile writer; many at once, as UDIM families do.
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 1200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!done) {
            std::cerr << "FAIL: failing texture writes did not return within 60 s\n";
            std::fflush(stderr);
            std::_Exit(2);
        }
    });
    std::vector<prism::texture_writer::Level> levels(1);
    levels[0] = {1024, 1024, randomImage(1024, 1024, 4, 3, false)};
    prism::texture_writer::BuildMipChain(levels, 4);
    const OIIO::ImageSpec spec = textureSpec(1024, 1024, 4);
    for (char const *limit : {"0", "65536", "1048576", "4194304"}) {
        setEnv("PRISM_TEST_TEXTURE_WRITE_FAIL_AFTER", limit);
        std::atomic<int> failed{0}, succeeded{0};
        tbb::parallel_for(0, 32, [&](int i) {
            std::string error;
            const auto path = dir / ("fail_" + std::string(limit) + "_" + std::to_string(i) + ".exr");
            if (prism::texture_writer::WriteMipmappedExr(path, spec, levels, &error)) ++succeeded;
            else ++failed;
        });
        check(failed == 32 && succeeded == 0,
              std::string("injected failure after ") + limit + " bytes: " + std::to_string(failed) + " failed, " +
              std::to_string(succeeded) + " succeeded");
    }
    setEnv("PRISM_TEST_TEXTURE_WRITE_FAIL_AFTER", nullptr);
    // And concurrent successful writes, through the same OpenEXR pool.
    std::atomic<int> written{0};
    tbb::parallel_for(0, 32, [&](int i) {
        std::string error;
        if (prism::texture_writer::WriteMipmappedExr(dir / ("ok_" + std::to_string(i) + ".exr"), spec, levels, &error))
            ++written;
    });
    check(written == 32, "concurrent writes: " + std::to_string(written) + " of 32 succeeded");
    done = true;
    watchdog.join();

    // GPU-ready copies: a round trip, then a damaged payload and a cut file
    // are rejected.
    {
        std::vector<uint8_t> texels(97 * 33 * 3);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = uint8_t(i * 7);
        const auto path = dir / "copy.gpu";
        std::string error;
        const auto header = prism::gpu_texture::MakeHeader(97, 33, 3, prism::gpu_texture::Component::Unorm8, 0, 0.0f);
        check(prism::gpu_texture::Write(path, header, texels.data(), &error), "GPU copy write: " + error);
        prism::gpu_texture::Header read{};
        std::vector<uint8_t> payload;
        check(prism::gpu_texture::Read(path, &read, &payload, &error) && payload == texels &&
                  read.width == 97 && read.height == 33 && read.channels == 3,
              "GPU copy round trip: " + error);
        {
            std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(sizeof(prism::gpu_texture::Header) + 100);
            file.put(char(texels[100] ^ 0x5a));
        }
        check(!prism::gpu_texture::Read(path, &read, &payload, &error), "damaged GPU copy was accepted");
        std::filesystem::resize_file(path, sizeof(prism::gpu_texture::Header) + 50);
        check(!prism::gpu_texture::Read(path, &read, nullptr, &error), "truncated GPU copy was accepted");
    }

    // GPU-ready copies with their mip chain: levels follow vkCmdBlitImage's
    // linear filter (a 2 x 2 box for even sizes, the center bilinear tap for
    // odd ones), and a range of levels reads without the others.
    {
        const uint32_t w = 6, h = 3;
        std::vector<uint8_t> texels(w * h);
        for (size_t i = 0; i < texels.size(); ++i) texels[i] = uint8_t(i * 13);
        const auto payload = prism::gpu_texture::BuildMipChain(texels.data(), w, h, 1,
                                                               prism::gpu_texture::Component::Unorm8);
        const auto header = prism::gpu_texture::MakeHeader(w, h, 1, prism::gpu_texture::Component::Unorm8, 0, 0.0f, 0);
        check(header.levelCount == 3 && header.levels[1].width == 3 && header.levels[1].height == 1 &&
                  header.levels[2].width == 1 && payload.size() == header.payloadBytes,
              "mip chain level table");
        // Level 1 texel 0: rows 0..2 into 1 row (center tap: row 1), columns 0 and 1 averaged.
        check(payload[header.levels[1].offset] == uint8_t(std::lround((texels[6] + texels[7]) / 2.0)),
              "mip chain filter: " + std::to_string(payload[header.levels[1].offset]));
        const auto path = dir / "mips.gpu";
        std::string error;
        check(prism::gpu_texture::Write(path, header, payload.data(), &error), "mip chain write: " + error);
        prism::gpu_texture::Header read{};
        check(prism::gpu_texture::Read(path, &read, nullptr, &error), "mip chain header: " + error);
        std::vector<uint8_t> tail(size_t(prism::gpu_texture::LevelRangeBytes(read, 1, 2)));
        check(prism::gpu_texture::ReadLevels(path, read, 1, 2, tail.data(), &error) &&
                  std::equal(tail.begin(), tail.end(), payload.begin() + ptrdiff_t(read.levels[1].offset)),
              "mip level range read: " + error);
        {
            std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(sizeof(prism::gpu_texture::Header) + read.levels[2].offset);
            file.put(char(payload[read.levels[2].offset] ^ 0x5a));
        }
        std::vector<uint8_t> level0(size_t(read.levels[0].bytes));
        check(prism::gpu_texture::ReadLevels(path, read, 0, 1, level0.data(), &error),
              "an intact level of a damaged copy was rejected: " + error);
        check(!prism::gpu_texture::ReadLevels(path, read, 1, 2, tail.data(), &error),
              "a damaged level was accepted");
        // Half texels round to nearest even both ways.
        for (float v : {0.0f, 1.0f, -2.5f, 65504.0f, 6.1e-5f, 3.0e-7f, 0.333333f})
            check(prism::gpu_texture::HalfToFloat(prism::gpu_texture::FloatToHalf(v)) ==
                      prism::gpu_texture::HalfToFloat(prism::gpu_texture::FloatToHalf(
                          prism::gpu_texture::HalfToFloat(prism::gpu_texture::FloatToHalf(v)))),
                  "half round trip");
    }

    // Fingerprint records: a rewrite that restores the size and modification
    // time still changes the file's stamp, so its record no longer applies.
    {
        const auto source = dir / "source.bin";
        { std::ofstream(source, std::ios::binary) << "first contents"; }
        const auto stamp = prism::fingerprint_records::FileStamp(source);
        prism::fingerprint_records::Write(dir, "identity\nsecond line", {"digest-1", "source.bin", stamp});
        prism::fingerprint_records::Record record;
        check(prism::fingerprint_records::Read(dir, "identity\nsecond line", &record) &&
                  record.fingerprint == "digest-1" && record.artifactStamp == stamp,
              "fingerprint record round trip");
        check(!prism::fingerprint_records::Read(dir, "other identity", &record), "record found for another identity");
        const auto time = std::filesystem::last_write_time(source);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        { std::ofstream(source, std::ios::binary) << "other contents"; }
        std::filesystem::last_write_time(source, time);
        check(prism::fingerprint_records::FileStamp(source) != stamp,
              "same-size, same-time rewrite kept the file's stamp");
        {
            std::ofstream(prism::fingerprint_records::RecordPath(dir, "identity\nsecond line"),
                          std::ios::binary | std::ios::app) << "x";
        }
        check(!prism::fingerprint_records::Read(dir, "identity\nsecond line", &record), "damaged record was accepted");
    }

    std::filesystem::remove_all(dir, ignored);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "texture writer: ok\n";
    return 0;
}
