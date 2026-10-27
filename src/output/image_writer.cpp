#include "image_writer.h"

#include "../color/color_management.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <stdexcept>

namespace prism::output {
namespace {

// EXR chromaticities of the working spaces MaterialX names (OpenEXR's
// red, green, blue and white xy).
std::optional<std::array<float, 8>> Chromaticities(prism::color::ColorManagement const &color)
{
    const std::string &space = color.materialXSpace();
    if (space == "acescg") return std::array<float, 8>{0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f, 0.32168f, 0.33767f};
    if (space == "lin_rec709") return std::array<float, 8>{0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f};
    if (space == "lin_displayp3") return std::array<float, 8>{0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f};
    if (space == "lin_adobergb") return std::array<float, 8>{0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f, 0.3127f, 0.3290f};
    return std::nullopt;
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void Check(Image const &image)
{
    if (image.width <= 0 || image.height <= 0) throw std::runtime_error("Image has no pixels");
    const size_t pixels = size_t(image.width) * image.height;
    for (auto const &layer : image.layers)
        if (layer.channels.empty() || layer.pixels.size() != pixels * layer.channels.size())
            throw std::runtime_error("Image layer '" + layer.name + "' has the wrong size");
}

void SetWindow(OIIO::ImageSpec &spec, Image const &image)
{
    if (image.fullWidth <= 0 || image.fullHeight <= 0) return;
    spec.x = image.x;
    spec.y = image.y;
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = image.fullWidth;
    spec.full_height = image.fullHeight;
}

void Write(std::string const &path, OIIO::ImageSpec const &spec, std::vector<float> const &pixels)
{
    OIIO::ImageBuf buffer(spec);
    if (!buffer.set_pixels(OIIO::ROI::All(), OIIO::TypeDesc::FLOAT, pixels.data()))
        throw std::runtime_error("Cannot store image pixels: " + buffer.geterror());
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(parent, ignored);
    }
    if (!buffer.write(path)) throw std::runtime_error("Cannot write image " + path + ": " + buffer.geterror());
}

// The EXR header of channels `names`: alpha and depth channels, the data
// window and the colour space tags.
OIIO::ImageSpec ExrSpec(Image const &image, std::vector<std::string> const &names,
                        prism::color::ColorManagement const &color)
{
    OIIO::ImageSpec spec(image.width, image.height, int(names.size()), OIIO::TypeDesc::FLOAT);
    spec.channelnames = names;
    const auto alpha = std::find(names.begin(), names.end(), "A");
    spec.alpha_channel = alpha == names.end() ? -1 : int(alpha - names.begin());
    const auto depth = std::find(names.begin(), names.end(), "Z");
    spec.z_channel = depth == names.end() ? -1 : int(depth - names.begin());
    SetWindow(spec, image);
    spec.attribute("oiio:ColorSpace", color.workingSpace());
    if (auto xy = Chromaticities(color))
        spec.attribute("chromaticities", OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8), xy->data());
    return spec;
}

// A multi-part EXR: a part per layer, named after it (made unique).
void WriteParts(std::string const &path, Image const &image, prism::color::ColorManagement const &color)
{
    std::vector<OIIO::ImageSpec> specs;
    std::vector<std::string> parts;
    for (auto const &layer : image.layers) {
        std::string name = layer.name.empty() ? std::string("layer") : layer.name;
        for (int n = 2; std::find(parts.begin(), parts.end(), name) != parts.end(); ++n)
            name = layer.name + "_" + std::to_string(n);
        parts.push_back(name);
        specs.push_back(ExrSpec(image, layer.channels, color));
        specs.back().attribute("oiio:subimagename", name); // the part name
    }
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(parent, ignored);
    }
    auto output = OIIO::ImageOutput::create(path);
    if (!output || !output->supports("multiimage"))
        throw std::runtime_error("Cannot write a multi-part image " + path);
    if (!output->open(path, int(specs.size()), specs.data()))
        throw std::runtime_error("Cannot write image " + path + ": " + output->geterror());
    for (size_t i = 0; i < specs.size(); ++i) {
        if (i > 0 && !output->open(path, specs[i], OIIO::ImageOutput::AppendSubimage))
            throw std::runtime_error("Cannot write image " + path + ": " + output->geterror());
        if (!output->write_image(OIIO::TypeDesc::FLOAT, image.layers[i].pixels.data()))
            throw std::runtime_error("Cannot write image " + path + ": " + output->geterror());
    }
    if (!output->close()) throw std::runtime_error("Cannot write image " + path + ": " + output->geterror());
}

} // namespace

std::vector<std::string> LayerChannels(std::string const &layer, AovKind kind,
                                       int components, bool beauty)
{
    static const char *const color[] = {"R", "G", "B", "A"};
    static const char *const vector[] = {"X", "Y", "Z", "W"};
    static const char *const coordinates[] = {"U", "V"};
    std::vector<std::string> names;
    const std::string prefix = beauty ? std::string() : layer + ".";
    for (int c = 0; c < components; ++c) {
        const char *suffix = nullptr;
        if (ColorAov(kind) || LightAov(kind)) suffix = color[std::min(c, 3)];
        else if (kind == AovKind::St && components == 2) suffix = coordinates[c];
        else if (components == 1)
            suffix = kind == AovKind::Depth || kind == AovKind::CameraDepth ? "Z" : "id";
        else suffix = vector[std::min(c, 3)];
        names.push_back(prefix + suffix);
    }
    return names;
}

bool IsMultiChannelFormat(std::string const &path)
{
    return Lower(std::filesystem::path(path).extension().string()) == ".exr";
}

void WriteImage(std::string const &path, Image const &image,
                prism::color::ColorManagement const &color,
                std::vector<std::string> *warnings)
{
    Check(image);
    const size_t pixels = size_t(image.width) * image.height;
    if (IsMultiChannelFormat(path)) {
        // Linear working space, every layer, tagged.
        std::vector<std::string> names;
        for (auto const &layer : image.layers)
            names.insert(names.end(), layer.channels.begin(), layer.channels.end());
        if (names.empty()) throw std::runtime_error("Image has no channels: " + path);
        for (size_t i = 0; i < names.size(); ++i)
            if (std::count(names.begin(), names.end(), names[i]) != 1)
                throw std::runtime_error("Duplicate image channel '" + names[i] + "' in " + path);
        if (image.multiPart) {
            WriteParts(path, image, color);
            return;
        }
        const OIIO::ImageSpec spec = ExrSpec(image, names, color);
        std::vector<float> interleaved(pixels * names.size());
        size_t offset = 0;
        for (auto const &layer : image.layers) {
            const size_t count = layer.channels.size();
            for (size_t p = 0; p < pixels; ++p)
                std::copy_n(layer.pixels.data() + p * count, count,
                            interleaved.data() + p * names.size() + offset);
            offset += count;
        }
        Write(path, spec, interleaved);
        return;
    }
    // Display-encoded through the view, exactly once: the colour layer only.
    const auto source = std::find_if(image.layers.begin(), image.layers.end(),
                                     [](ImageLayer const &layer) { return layer.color; });
    if (source == image.layers.end())
        throw std::runtime_error("No colour output for " + path + " (only EXR holds other AOVs)");
    const int channels = int(source->channels.size());
    if (channels < 3) throw std::runtime_error("Colour output needs three channels: " + path);
    if (warnings)
        for (auto const &layer : image.layers)
            if (&layer != &*source)
                warnings->push_back("'" + layer.name + "' is not written to " + path +
                                    " (only EXR holds other AOVs)");
    std::vector<float> encoded = source->pixels;
    // The renderer's colour is premultiplied by alpha (coverage): the view
    // applies to the unpremultiplied colour, and the file keeps it so.
    if (channels == 4)
        for (size_t p = 0; p < pixels; ++p) {
            const float alpha = encoded[p * 4 + 3];
            if (alpha > 0.0f && alpha != 1.0f)
                for (int c = 0; c < 3; ++c) encoded[p * 4 + c] /= alpha;
        }
    color.ApplyView(encoded.data(), pixels, channels);
    // 8-bit formats hold no data window: a crop is written as its pixels.
    OIIO::ImageSpec spec(image.width, image.height, channels, OIIO::TypeDesc::FLOAT);
    if (channels == 4) {
        spec.alpha_channel = 3;
        spec.attribute("oiio:UnassociatedAlpha", 1);
    }
    Write(path, spec, encoded);
}

} // namespace prism::output
