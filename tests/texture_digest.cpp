// Prints a digest of every mip level of prepared textures, so a change to the
// preparation pipeline can be checked for byte-identical output. Usage:
//   prism_texture_digest [--preset NAME]... [--timings] image...
// Presets: unassociate (MDL runtime options), gamma22, channel0, plain.
// Without --preset every preset is run. Prepared files go to PRISM_CACHE_DIR.
#include "../src/texture_service.h"

#include <OpenImageIO/hash.h>
#include <OpenImageIO/imageio.h>

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
std::vector<std::pair<std::string, prism::TexturePrepareOptions>> Presets()
{
    std::vector<std::pair<std::string, prism::TexturePrepareOptions>> result;
    prism::TexturePrepareOptions unassociate;
    unassociate.unassociateAlpha = true;
    result.emplace_back("unassociate", unassociate);
    prism::TexturePrepareOptions gamma = unassociate;
    gamma.gamma = 2.2f;
    result.emplace_back("gamma22", gamma);
    prism::TexturePrepareOptions channel;
    channel.channelCount = 1;
    result.emplace_back("channel0", channel);
    result.emplace_back("plain", prism::TexturePrepareOptions{});
    return result;
}

void PrintAttribute(OIIO::ImageSpec const &spec, char const *name)
{
    if (auto const *attribute = spec.find_attribute(name))
        std::cout << "  " << name << "=" << attribute->get_string() << '\n';
}

bool PrintDigest(std::filesystem::path const &prepared)
{
    auto input = OIIO::ImageInput::open(prepared.string());
    if (!input) {
        std::cout << "  cannot open prepared file: " << OIIO::geterror() << '\n';
        return false;
    }
    const OIIO::ImageSpec top = input->spec();
    PrintAttribute(top, "oiio:SHA-1");
    PrintAttribute(top, "oiio:AverageColor");
    PrintAttribute(top, "compression");
    PrintAttribute(top, "textureformat");
    PrintAttribute(top, "wrapmodes");
    std::cout << "  tile=" << top.tile_width << "x" << top.tile_height << '\n';
    for (int level = 0; input->seek_subimage(0, level); ++level) {
        const OIIO::ImageSpec spec = input->spec();
        std::vector<float> pixels(static_cast<size_t>(spec.width) * spec.height * spec.nchannels);
        if (!input->read_image(0, level, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data())) {
            std::cout << "  level " << level << ": read failed: " << input->geterror() << '\n';
            return false;
        }
        std::cout << "  level " << level << ": " << spec.width << "x" << spec.height
                  << " channels=" << spec.nchannels << " type=" << spec.format.c_str()
                  << " sha1=" << OIIO::SHA1::digest(pixels.data(), pixels.size() * sizeof(float))
                  << '\n';
    }
    return true;
}

void PrintTimings(prism::TextureServiceStats const &before, prism::TextureServiceStats const &after)
{
    auto seconds = [](uint64_t a, uint64_t b) { return (b - a) / 1.0e9; };
    std::cout << std::fixed << std::setprecision(4)
              << "  timings: cold=" << after.coldPreparations - before.coldPreparations
              << " disk=" << after.diskCacheHits - before.diskCacheHits
              << " memory=" << after.memoryCacheHits - before.memoryCacheHits
              << " inspect=" << seconds(before.inspectNanoseconds, after.inspectNanoseconds)
              << " decode=" << seconds(before.decodeNanoseconds, after.decodeNanoseconds)
              << " pixel=" << seconds(before.pixelNanoseconds, after.pixelNanoseconds)
              << " mip=" << seconds(before.mipNanoseconds, after.mipNanoseconds)
              << " publish=" << seconds(before.publishNanoseconds, after.publishNanoseconds)
              << " trim=" << seconds(before.trimNanoseconds, after.trimNanoseconds)
              << " lockwait=" << seconds(before.lockWaitNanoseconds, after.lockWaitNanoseconds)
              << " total=" << seconds(before.preparationNanoseconds, after.preparationNanoseconds)
              << '\n' << std::defaultfloat;
}
}

int main(int argc, char **argv)
{
    if (!std::getenv("PRISM_CACHE_DIR")) {
        std::cerr << "prism_texture_digest: set PRISM_CACHE_DIR to a scratch cache\n";
        return 2;
    }
    const auto all = Presets();
    std::vector<std::pair<std::string, prism::TexturePrepareOptions>> selected;
    std::vector<std::filesystem::path> images;
    bool timings = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--timings") { timings = true; continue; }
        if (argument == "--preset" && i + 1 < argc) {
            const std::string name = argv[++i];
            bool found = false;
            for (auto const &preset : all)
                if (preset.first == name) { selected.push_back(preset); found = true; }
            if (!found) { std::cerr << "unknown preset " << name << '\n'; return 2; }
            continue;
        }
        images.emplace_back(argument);
    }
    if (selected.empty()) selected = all;
    auto service = prism::TextureService::Acquire();
    int failures = 0;
    for (auto const &image : images) {
        for (auto const &[name, options] : selected) {
            std::cout << "source=" << image.filename().string() << " preset=" << name << '\n';
            const auto before = service->stats();
            std::shared_ptr<const prism::TextureResource> resource;
            try {
                resource = service->prepare(image, options);
            } catch (std::exception const &error) {
                std::cout << "  prepare failed: " << error.what() << '\n';
                ++failures;
                continue;
            }
            const auto after = service->stats();
            std::cout << "  prepared=" << resource->preparedPath().filename().string()
                      << " size=" << resource->width() << "x" << resource->height()
                      << " channels=" << resource->channels() << '\n';
            if (!PrintDigest(resource->preparedPath())) ++failures;
            if (timings) PrintTimings(before, after);
        }
    }
    return failures ? 1 : 0;
}
