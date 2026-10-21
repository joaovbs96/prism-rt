#pragma once

// Image outputs shared by the standalone CLI and prism_hydra_render
// (docs/color_management.md, "Outputs"; docs/hydra.md, "Render products").
// - EXR: every layer as named float channels, linear in the working space,
//   tagged with oiio:ColorSpace and OpenEXR chromaticities; a cropped image
//   keeps its data window inside the display window.
// - Other formats (PNG, JPEG, ...): the colour layer only, through the
//   display and view (ApplyView) exactly once; alpha stays linear.

#include "aov.h"

#include <string>
#include <vector>

namespace prism::color { class ColorManagement; }

namespace prism::output {

struct ImageLayer {
    std::string name;                  // the RenderVar or AOV name (messages)
    std::vector<std::string> channels; // full channel names, e.g. R or normal.X
    std::vector<float> pixels;         // width*height*channels.size(), top row first
    // Radiance (RGB, optional alpha): the source of 8-bit outputs.
    bool color = false;
};

struct Image {
    int width = 0, height = 0;
    // Position of these pixels in a fullWidth x fullHeight display window
    // (a data window crop). Zero full size: the image is the whole window.
    int x = 0, y = 0, fullWidth = 0, fullHeight = 0;
    std::vector<ImageLayer> layers;
};

// The channels of an AOV layer. The beauty layer is unprefixed R, G, B (and
// A with four components); other layers are prefixed with their name:
// colours <name>.R/G/B/A, vectors <name>.X/Y/Z, texture coordinates
// <name>.U/V, depths <name>.Z and ids <name>.id.
std::vector<std::string> LayerChannels(std::string const &layer, AovKind kind,
                                       int components, bool beauty);

// True for formats written as linear floats with every layer (EXR).
bool IsMultiChannelFormat(std::string const &path);

// Writes `image` to `path`. Layers that the format cannot hold are dropped
// with a message in *warnings. Throws std::runtime_error on failure.
void WriteImage(std::string const &path, Image const &image,
                prism::color::ColorManagement const &color,
                std::vector<std::string> *warnings = nullptr);

} // namespace prism::output
