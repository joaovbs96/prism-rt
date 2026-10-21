#include "render_products.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/spec.h>
#include <pxr/usd/usdRender/tokens.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::output {
namespace {

void Warn(std::vector<std::string> *warnings, std::string message)
{
    if (warnings) warnings->push_back(std::move(message));
}

int VarComponents(AovKind kind, TfToken const &dataType)
{
    if (kind == AovKind::Color) {
        const std::string &type = dataType.GetString();
        return type == "color4f" || type == "color4h" || type == "float4" || type == "half4" ? 4 : 3;
    }
    return AovComponents(kind);
}

void Finish(RenderProductRequest &product)
{
    DataWindowPixels(product.dataWindowNDC, product.width, product.height,
                     &product.dataX, &product.dataY, &product.dataWidth, &product.dataHeight);
}

} // namespace

bool RenderProductRequest::SameView(RenderProductRequest const &other) const
{
    return camera == other.camera && width == other.width && height == other.height &&
        pixelAspectRatio == other.pixelAspectRatio &&
        aspectRatioConformPolicy == other.aspectRatioConformPolicy &&
        dataX == other.dataX && dataY == other.dataY &&
        dataWidth == other.dataWidth && dataHeight == other.dataHeight;
}

AovKind RenderVarAov(std::string const &sourceName, TfToken const &sourceType)
{
    const std::string &type = sourceType.GetString();
    if (type.empty() || type == "raw") {
        // Beauty under its common names.
        if (sourceName == "color" || sourceName == "Ci" || sourceName == "C") return AovKind::Color;
        return AovKindFromName(sourceName);
    }
    if (type == "lpe") {
        // Every light path from the camera: the beauty.
        std::string text = TfStringReplace(sourceName, " ", "");
        if (text == "C.*" || text == "C.*L" || text == "C.*[LO]" || text == "C.*[<L.>O]")
            return AovKind::Color;
        return AovKind::Unknown;
    }
    if (type == "primvar" && sourceName == "st") return AovKind::St;
    return AovKind::Unknown;
}

void DataWindowPixels(GfRange2f const &ndc, int width, int height,
                      int *x, int *y, int *dataWidth, int *dataHeight)
{
    // Pixel column c is in when xmin <= (c + 0.5) / width < xmax; NDC y is up,
    // rows go down.
    auto first = [](double edge) { return static_cast<int>(std::ceil(edge - 0.5)); };
    const int x0 = first(double(width) * ndc.GetMin()[0]);
    const int x1 = first(double(width) * ndc.GetMax()[0]);
    const int y0 = first(double(height) * (1.0 - ndc.GetMax()[1]));
    const int y1 = first(double(height) * (1.0 - ndc.GetMin()[1]));
    *x = x0;
    *y = y0;
    *dataWidth = std::max(0, x1 - x0);
    *dataHeight = std::max(0, y1 - y0);
}

CameraUtilConformWindowPolicy ConformPolicy(TfToken const &policy)
{
    if (policy == UsdRenderTokens->cropAperture) return CameraUtilCrop;
    if (policy == UsdRenderTokens->adjustApertureWidth) return CameraUtilMatchVertically;
    if (policy == UsdRenderTokens->adjustApertureHeight) return CameraUtilMatchHorizontally;
    if (policy == UsdRenderTokens->adjustPixelAspectRatio) return CameraUtilDontConform;
    return CameraUtilFit; // expandAperture
}

std::string SettingText(VtDictionary const &settings, std::string const &name)
{
    const auto found = settings.find(name);
    if (found == settings.end()) return {};
    const VtValue &value = found->second;
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<SdfAssetPath>()) {
        const auto &asset = value.UncheckedGet<SdfAssetPath>();
        return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }
    return {};
}

Image BuildProductImage(RenderProductRequest const &product,
                        std::vector<RenderedAov> const &aovs,
                        std::vector<std::string> *warnings)
{
    Image image;
    image.width = product.dataWidth;
    image.height = product.dataHeight;
    if (!product.FullDataWindow()) {
        image.x = product.dataX;
        image.y = product.dataY;
        image.fullWidth = product.width;
        image.fullHeight = product.height;
    }
    const size_t pixels = size_t(image.width) * image.height;
    bool beautyTaken = false;
    for (auto const &var : product.vars) {
        auto const found = std::find_if(aovs.begin(), aovs.end(),
            [&](RenderedAov const &aov) { return aov.kind == var.kind; });
        if (found == aovs.end() || found->pixels.size() != pixels * size_t(found->components)) {
            Warn(warnings, "Render var <" + var.path.GetString() + "> was not rendered; skipped");
            continue;
        }
        const bool beauty = var.kind == AovKind::Color && !beautyTaken;
        beautyTaken = beautyTaken || beauty;
        ImageLayer layer;
        layer.name = var.name;
        layer.color = var.kind == AovKind::Color;
        layer.channels = LayerChannels(var.name, var.kind, var.components, beauty);
        layer.pixels.resize(pixels * size_t(var.components));
        const int available = found->components;
        // The CLI renders RGB only; its images are opaque (as the delegate's
        // with the dome visible to the camera).
        const bool opaqueAlpha = var.kind == AovKind::Color && available == 3;
        if (var.components > available && !opaqueAlpha)
            Warn(warnings, "Render var <" + var.path.GetString() + "> asks for " +
                 std::to_string(var.components) + " channels; the render has " +
                 std::to_string(available) + " (alpha is written as 1)");
        for (size_t p = 0; p < pixels; ++p)
            for (int c = 0; c < var.components; ++c)
                layer.pixels[p * var.components + c] = c < available
                    ? found->pixels[p * available + c] : 1.0f;
        image.layers.push_back(std::move(layer));
    }
    return image;
}

std::optional<RenderJob> ResolveRenderJob(UsdStageRefPtr const &stage,
                                          std::string const &settingsPath,
                                          std::string const &outputDirectory,
                                          std::vector<std::string> *warnings)
{
    std::string path = settingsPath;
    if (path.empty()) {
        stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &path);
        if (path.empty()) return std::nullopt;
    }
    if (!SdfPath::IsValidPathString(path))
        throw std::runtime_error("Invalid render settings path: " + path);
    UsdRenderSettings settings(stage->GetPrimAtPath(SdfPath(path)));
    if (!settings) {
        if (!settingsPath.empty())
            throw std::runtime_error("Not a RenderSettings prim: " + path);
        Warn(warnings, "renderSettingsPrimPath <" + path + "> is not a RenderSettings prim; ignored");
        return std::nullopt;
    }
    const UsdRenderSpec spec = UsdRenderComputeSpec(settings, {TfToken("prism")});
    RenderJob job;
    job.settings = settings.GetPath();
    job.includedPurposes = spec.includedPurposes;
    job.materialBindingPurposes = spec.materialBindingPurposes;
    job.namespacedSettings = spec.namespacedSettings;

    // The settings prim's own values (UsdRenderComputeSpec only resolves
    // them into products).
    RenderProductRequest &base = job.base;
    base.path = job.settings;
    base.type = UsdRenderTokens->raster;
    SdfPathVector cameras;
    settings.GetCameraRel().GetForwardedTargets(&cameras);
    if (!cameras.empty()) base.camera = cameras.front();
    GfVec2i resolution(0);
    settings.GetResolutionAttr().Get(&resolution);
    base.width = resolution[0];
    base.height = resolution[1];
    settings.GetPixelAspectRatioAttr().Get(&base.pixelAspectRatio);
    settings.GetAspectRatioConformPolicyAttr().Get(&base.aspectRatioConformPolicy);
    GfVec4f window(0, 0, 1, 1);
    settings.GetDataWindowNDCAttr().Get(&window);
    base.dataWindowNDC = GfRange2f(GfVec2f(window[0], window[1]), GfVec2f(window[2], window[3]));
    if (base.aspectRatioConformPolicy == UsdRenderTokens->adjustPixelAspectRatio && base.height > 0) {
        // As UsdRenderComputeSpec does for products.
        if (UsdGeomCamera camera{stage->GetPrimAtPath(base.camera)}) {
            float apertureWidth = 0, apertureHeight = 0;
            camera.GetHorizontalApertureAttr().Get(&apertureWidth);
            camera.GetVerticalApertureAttr().Get(&apertureHeight);
            if (apertureWidth > 0 && apertureHeight > 0 && base.width > 0)
                base.pixelAspectRatio = (apertureWidth / apertureHeight) /
                    (float(base.width) / float(base.height));
        }
    }
    Finish(base);

    for (auto const &source : spec.products) {
        RenderProductRequest product;
        product.path = source.renderProductPath;
        product.type = source.type;
        if (!source.type.IsEmpty() && source.type != UsdRenderTokens->raster) {
            Warn(warnings, "Render product <" + product.path.GetString() + "> of type '" +
                 source.type.GetString() + "' is not supported (only raster); skipped");
            continue;
        }
        std::string name = source.name.GetString();
        if (name.empty()) name = product.path.GetName() + ".exr";
        std::filesystem::path output(name);
        if (output.is_relative() && !outputDirectory.empty())
            output = std::filesystem::path(outputDirectory) / output;
        product.output = output.string();
        product.camera = source.cameraPath;
        product.width = source.resolution[0];
        product.height = source.resolution[1];
        product.pixelAspectRatio = source.pixelAspectRatio;
        product.aspectRatioConformPolicy = source.aspectRatioConformPolicy;
        product.dataWindowNDC = source.dataWindowNDC;
        Finish(product);
        if (product.width <= 0 || product.height <= 0 || product.dataWidth <= 0 || product.dataHeight <= 0) {
            Warn(warnings, "Render product <" + product.path.GetString() +
                 "> has an empty resolution or data window; skipped");
            continue;
        }
        // Motion blur is not rendered, so disableMotionBlur has nothing to do.
        if (source.disableDepthOfField)
            Warn(warnings, "Render product <" + product.path.GetString() +
                 ">: disableDepthOfField is not supported; ignored");
        for (size_t index : source.renderVarIndices) {
            auto const &var = spec.renderVars[index];
            RenderVarRequest request;
            request.path = var.renderVarPath;
            request.name = var.renderVarPath.GetName();
            request.sourceName = var.sourceName;
            request.sourceType = var.sourceType;
            request.dataType = var.dataType;
            request.kind = RenderVarAov(var.sourceName, var.sourceType);
            if (request.kind == AovKind::Unknown) {
                Warn(warnings, "Render var <" + var.renderVarPath.GetString() + "> (" +
                     (var.sourceType.IsEmpty() ? std::string("raw") : var.sourceType.GetString()) +
                     " '" + var.sourceName + "') is not supported; skipped");
                continue;
            }
            request.components = VarComponents(request.kind, var.dataType);
            product.vars.push_back(std::move(request));
        }
        if (product.vars.empty()) {
            Warn(warnings, "Render product <" + product.path.GetString() +
                 "> has no supported render vars; skipped");
            continue;
        }
        job.products.push_back(std::move(product));
    }
    return job;
}

} // namespace prism::output
