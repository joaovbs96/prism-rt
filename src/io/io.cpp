// prism_io (include/prism/io.h): render results as image files, through the
// image writer the CLI and prism_hydra_render share (output/image_writer.h).

#include <prism/io.h>

#include "../output/aov.h"
#include "../output/image_writer.h"
#include "../renderer/renderer_context.h"

#include <stdexcept>

namespace prism::io {

bool IsMultiChannelFormat(std::string const& path) { return output::IsMultiChannelFormat(path); }

void Write(std::string const& path, RenderResult const& result, Renderer const& renderer, WriteOptions const& options,
           std::vector<std::string>* warnings)
{
    std::vector<Layer> layers = options.layers;
    if (layers.empty())
        for (auto const& [aov, image] : result.aovs) layers.push_back({aov, std::string(), 0});
    const Window& window = result.dataWindow;
    output::Image file;
    file.width = int(window.width);
    file.height = int(window.height);
    file.multiPart = options.multiPart;
    if (!(window.x == 0 && window.y == 0 && window.width == result.width && window.height == result.height)) {
        file.x = int(window.x);
        file.y = int(window.y);
        file.fullWidth = int(result.width);
        file.fullHeight = int(result.height);
    }
    const size_t pixels = size_t(window.width) * window.height;
    bool beautyTaken = false;
    for (Layer const& layer : layers) {
        const std::string name = layer.name.empty() ? std::string(AovName(layer.aov)) : layer.name;
        const auto kind = output::AovKindFromName(AovName(layer.aov));
        auto found = result.aovs.find(layer.aov);
        if (found == result.aovs.end() || found->second.pixelCount() != pixels) {
            if (warnings) warnings->push_back("Render var <" + name + "> was not rendered; skipped");
            continue;
        }
        Image const& image = found->second;
        const int available = int(image.channels());
        const int components = layer.components ? int(layer.components) : available;
        const bool beauty = layer.aov == Aov::Color && !beautyTaken;
        beautyTaken = beautyTaken || beauty;
        output::ImageLayer out;
        out.name = name;
        out.color = output::ColorAov(kind);
        out.channels = output::LayerChannels(name, kind, components, beauty);
        out.pixels.resize(pixels * size_t(components));
        // RGB colour renders are opaque: a requested alpha is 1.
        const bool opaqueAlpha = output::ColorAov(kind) && available == 3;
        if (components > available && !opaqueAlpha && warnings)
            warnings->push_back("Render var <" + name + "> asks for " + std::to_string(components) +
                                " channels; the render has " + std::to_string(available) + " (alpha is written as 1)");
        if (image.type() == DataType::Int32) {
            int32_t const* values = image.ints();
            for (size_t p = 0; p < pixels; ++p)
                for (int c = 0; c < components; ++c)
                    out.pixels[p * components + c] = c < available ? float(values[p * available + c]) : 1.0f;
        } else {
            float const* values = image.floats();
            for (size_t p = 0; p < pixels; ++p)
                for (int c = 0; c < components; ++c)
                    out.pixels[p * components + c] = c < available ? values[p * available + c] : 1.0f;
        }
        file.layers.push_back(std::move(out));
    }
    if (file.layers.empty()) throw Error(ErrorCode::InvalidArgument, "nothing to write to " + path);
    try {
        output::WriteImage(path, file, *renderer.context()->colorManagement(), warnings);
    } catch (Error const&) {
        throw;
    } catch (std::exception const& error) {
        throw Error(ErrorCode::Internal, error.what());
    }
}

} // namespace prism::io
