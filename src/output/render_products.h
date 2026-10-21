#pragma once

// UsdRender settings, products and vars, resolved the same way for both
// frontends (the standalone CLI and prism_hydra_render) with
// UsdRenderComputeSpec (docs/hydra.md, "Render settings and products").

#include "aov.h"
#include "image_writer.h"

#include <pxr/base/gf/range2f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <optional>
#include <string>
#include <vector>

namespace prism::output {

struct RenderVarRequest {
    pxr::SdfPath path;
    std::string name;          // the layer name: the RenderVar prim's name
    std::string sourceName;
    pxr::TfToken sourceType, dataType;
    AovKind kind = AovKind::Unknown;
    int components = 0;        // channels written (color3f: 3, color4f: 4, ...)
};

// What one image is rendered from: a RenderProduct, or the settings prim
// itself (no vars; the frontend's own output).
struct RenderProductRequest {
    pxr::SdfPath path;
    pxr::TfToken type;              // raster
    std::string output;             // productName, against the output directory
    pxr::SdfPath camera;
    int width = 0, height = 0;      // the display window
    float pixelAspectRatio = 1.0f;
    pxr::TfToken aspectRatioConformPolicy;
    pxr::GfRange2f dataWindowNDC{pxr::GfVec2f(0.0f), pxr::GfVec2f(1.0f)};
    // The pixels whose centres are inside dataWindowNDC, top row first:
    // columns [dataX, dataX + dataWidth), rows [dataY, dataY + dataHeight).
    int dataX = 0, dataY = 0, dataWidth = 0, dataHeight = 0;
    std::vector<RenderVarRequest> vars;

    bool FullDataWindow() const
    {
        return dataX == 0 && dataY == 0 && dataWidth == width && dataHeight == height;
    }
    // Renders of two products are the same image (only their vars differ).
    bool SameView(RenderProductRequest const &other) const;
};

struct RenderJob {
    pxr::SdfPath settings;
    // The settings prim's own camera, resolution and data window.
    RenderProductRequest base;
    std::vector<RenderProductRequest> products;
    pxr::VtArray<pxr::TfToken> includedPurposes;
    pxr::VtArray<pxr::TfToken> materialBindingPurposes;
    // prism:* attributes of the settings prim.
    pxr::VtDictionary namespacedSettings;
};

// The RenderSettings prim at `settingsPath`, or the stage's
// renderSettingsPrimPath when empty; nullopt when there is none. Relative
// product names resolve against outputDirectory (when not empty).
// Unsupported products and vars are left out with a message in *warnings.
// Throws std::runtime_error when `settingsPath` is not a RenderSettings prim.
std::optional<RenderJob> ResolveRenderJob(pxr::UsdStageRefPtr const &stage,
                                          std::string const &settingsPath,
                                          std::string const &outputDirectory,
                                          std::vector<std::string> *warnings);

// The AOV a RenderVar asks for (Unknown when PrismRT has none).
AovKind RenderVarAov(std::string const &sourceName, pxr::TfToken const &sourceType);

// Pixels of a dataWindowNDC (UsdRender rasterization: a pixel is in when
// its centre is), top row first.
void DataWindowPixels(pxr::GfRange2f const &ndc, int width, int height,
                      int *x, int *y, int *dataWidth, int *dataHeight);

// CameraUtil's policy for an aspectRatioConformPolicy token.
pxr::CameraUtilConformWindowPolicy ConformPolicy(pxr::TfToken const &policy);

// One rendered AOV of a product's view: dataWidth x dataHeight pixels of
// `components` floats, top row first (integer AOVs as floats).
struct RenderedAov {
    AovKind kind = AovKind::Unknown;
    int components = 0;
    std::vector<float> pixels;
};

// The product's image: a layer per var, from the rendered AOVs. Vars whose
// AOV was not rendered are left out with a message in *warnings; a colour
// var asking for alpha from an RGB render gets alpha 1.
Image BuildProductImage(RenderProductRequest const &product,
                        std::vector<RenderedAov> const &aovs,
                        std::vector<std::string> *warnings);

// The value of a prism:* setting as text (string, token or asset path), or
// empty.
std::string SettingText(pxr::VtDictionary const &settings, std::string const &name);

} // namespace prism::output
