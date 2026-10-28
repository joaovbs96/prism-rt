#pragma once

// Image files of render results (module prism_io; docs/sdk_design.md 3.7, 4.1;
// docs/color_management.md, "Outputs"):
// - EXR: every layer as named float channels, linear in the renderer's
//   working space, tagged with its colour space and chromaticities; a crop
//   keeps its data window inside the display window. The first Color layer
//   is the beauty (R, G, B); other layers are prefixed with their names
//   (normal.X, albedo.R, primId.id, ...). One part holds every layer, or a
//   part per layer (WriteOptions::multiPart).
// - Other formats (PNG, JPEG, ...): the first colour layer (Color or
//   Denoised) only, display-encoded through the renderer's display and view
//   exactly once; other layers are dropped with a warning.

#include "renderer.h"
#include "session.h"

#include <string>
#include <vector>

namespace prism::io {

struct Layer {
    Aov aov = Aov::Color;
    std::string name;         // empty: the AOV's name (AovName)
    // Channels written; 0: the image's. A colour layer of 4 from an RGB
    // image gets alpha 1 (the CLI's images are opaque).
    unsigned components = 0;
};

struct WriteOptions {
    // Empty: every AOV of the result, in the order of the Aov enum (Color
    // first).
    std::vector<Layer> layers;
    bool multiPart = false;   // EXR: a part per layer, named after it
};

// True for formats written as linear floats with every layer (EXR).
bool IsMultiChannelFormat(std::string const& path);

// Writes `result` to `path` through `renderer`'s colour management. Layers
// whose AOV was not rendered, or that the format cannot hold, are left out
// with a message in *warnings. Throws Error(InvalidArgument) for a result
// with nothing to write and Error(Internal) when the file cannot be written.
void Write(std::string const& path, RenderResult const& result, Renderer const& renderer,
           WriteOptions const& options = {}, std::vector<std::string>* warnings = nullptr);

} // namespace prism::io
