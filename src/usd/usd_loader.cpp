// prism::usd::Load (include/prism/usd.h): a USD stage into a public scene.
// The gprims and materials come from the standalone USD reader
// (src/mesh/usd_mesh.h); the UsdLux lights, domes and light linking moved
// here unchanged from the CLI.

// First: usd_mesh.h includes Hydra's material schema before windows.h,
// which defines `interface`.
#include "../mesh/usd_mesh.h"

#include <prism/usd.h>

#include "../color/color_management.h"
#include "../lights/ies.h"
#include "../materialx/shared_mdl_context.h"
#include "../mdl/context.h"
#include "../renderer/renderer_context.h"
#include "../scene/scene_internal.h"
#include "gf_convert.h"

#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/collectionMembershipQuery.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/domeLight_1.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/tokens.h>

#include <filesystem>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::usd {
namespace {

// A light's texture file, resolved as the Hydra delegate resolves it (the
// asset's resolved path, else the resolver on its authored path); empty when
// it does not exist.
std::string ResolvedLightTexture(SdfAssetPath const& asset)
{
    std::string resolved = asset.GetResolvedPath();
    if (resolved.empty() && !asset.GetAssetPath().empty())
        resolved = ArGetResolver().Resolve(asset.GetAssetPath()).GetPathString();
    if (!resolved.empty() && !std::filesystem::exists(resolved)) resolved.clear();
    return resolved;
}

// UsdLuxShapingAPI inputs, as the Hydra delegate reads them
// (scene_bridge.cpp _ShapingParameters): the light is shaped when one of
// them has a value (the API applied, which gives every input its fallback,
// or an input authored); unauthored ones take the schema's fallbacks.
LightDesc::Shaping ShapingParameters(UsdPrim const& prim, UsdTimeCode time)
{
    LightDesc::Shaping s;
    const UsdLuxShapingAPI shaping(prim);
    auto read = [&](UsdAttribute const& attribute, auto* value) {
        if (attribute && attribute.HasValue() && attribute.Get(value, time)) s.active = true;
    };
    GfVec3f tint(0.0f);
    SdfAssetPath ies;
    read(shaping.GetShapingFocusAttr(), &s.focus);
    read(shaping.GetShapingFocusTintAttr(), &tint);
    read(shaping.GetShapingConeAngleAttr(), &s.coneAngle);
    read(shaping.GetShapingConeSoftnessAttr(), &s.coneSoftness);
    read(shaping.GetShapingIesFileAttr(), &ies);
    read(shaping.GetShapingIesAngleScaleAttr(), &s.iesAngleScale);
    read(shaping.GetShapingIesNormalizeAttr(), &s.iesNormalize);
    s.focusTint = Color3(tint[0], tint[1], tint[2]);
    if (s.active && !ies.GetAssetPath().empty()) {
        std::string resolved = ies.GetResolvedPath();
        if (resolved.empty()) resolved = ArGetResolver().Resolve(ies.GetAssetPath()).GetPathString();
        if (!resolved.empty() && std::filesystem::exists(resolved)) s.iesFile = resolved;
        else WarnUnresolvedIesProfile(ies.GetAssetPath());
    }
    return s;
}

// Light and shadow linking as the Hydra delegate gets it from
// HdsiLightLinkingSceneIndex: a category per distinct collection that does
// not include everything (at most 32 of each kind), named by its rules, and
// on each placement the categories of the collections that include its prim.
class LightLinks {
public:
    // The category of a light's lightLink or shadowLink collection (empty:
    // it links everything).
    std::string category(UsdCollectionAPI const& collection, bool shadow)
    {
        if (!collection) return {};
        UsdCollectionMembershipQuery query = collection.ComputeMembershipQuery();
        std::string key;
        if (query.UsesPathExpansionRuleMap()) {
            auto const& rules = query.GetAsPathExpansionRuleMap();
            if (rules.size() == 1 && rules.begin()->first == SdfPath::AbsoluteRootPath() &&
                rules.begin()->second == UsdTokens->expandPrims) return {};
            for (auto const& [path, rule] : rules) key += path.GetString() + "=" + rule.GetString() + ";";
        } else {
            const SdfPathExpression expression = collection.ResolveCompleteMembershipExpression();
            if (expression == SdfPathExpression::Everything()) return {};
            key = "expression:" + expression.GetText();
        }
        auto& kind = shadow ? m_shadow : m_light;
        for (auto const& category : kind)
            if (category.key == key) return key;
        if (kind.size() >= 32) {
            if (!m_warned) std::cerr << "PrismRT: more than 32 light or shadow link collections; further ones are ignored\n";
            m_warned = true;
            return {};
        }
        kind.push_back({key, std::move(query)});
        return key;
    }
    bool active() const { return !m_light.empty() || !m_shadow.empty(); }
    // The light and shadow categories of a prim.
    void categories(SdfPath const& path, std::vector<std::string>* light, std::vector<std::string>* shadow) const
    {
        for (auto const& category : m_light) if (category.query.IsPathIncluded(path)) light->push_back(category.key);
        for (auto const& category : m_shadow) if (category.query.IsPathIncluded(path)) shadow->push_back(category.key);
    }

private:
    struct Category {
        std::string key;
        UsdCollectionMembershipQuery query;
    };
    std::vector<Category> m_light, m_shadow;
    bool m_warned = false;
};

} // namespace

LoadResult Load(Scene& scene, UsdStageRefPtr const& stage, LoadOptions const& options)
{
    if (!stage) throw Error(ErrorCode::InvalidArgument, "no USD stage");
    RendererContext& renderer = detail::SceneAccess::context(scene);
    const UsdTimeCode time = options.time;
    LoadResult result;

    // ── Lights and domes (UsdLux), read first: placements need the link
    // categories.
    LightLinks links;
    std::vector<LightDesc> lights;
    std::vector<EnvironmentDesc> domes;
    {
        UsdGeomXformCache transforms(time);
        for (const auto& prim : stage->Traverse()) {
            if (!prim.HasAPI<UsdLuxLightAPI>()) continue;
            if (UsdGeomImageable(prim).ComputeVisibility(time) == UsdGeomTokens->invisible) continue;
            UsdLuxLightAPI light(prim);
            GfVec3f color(1); float intensity = 1, exposure = 0;
            light.GetColorAttr().Get(&color, time); light.GetIntensityAttr().Get(&intensity, time);
            light.GetExposureAttr().Get(&exposure, time);
            bool enableTemperature = false; float temperature = 6500.0f;
            light.GetEnableColorTemperatureAttr().Get(&enableTemperature, time);
            light.GetColorTemperatureAttr().Get(&temperature, time);
            if (enableTemperature) color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(temperature));
            const GfMatrix4d world = transforms.GetLocalToWorldTransform(prim);
            const bool isDome = prim.IsA<UsdLuxDomeLight>() || prim.IsA<UsdLuxDomeLight_1>();
            if (isDome) {
                if (!options.domes) continue;
                EnvironmentDesc dome;
                dome.kind = EnvironmentDesc::Kind::Dome;
                dome.color = Color3(color[0], color[1], color[2]);
                dome.colorSpace = color::kLightColorSpace;
                dome.intensity = intensity;
                dome.exposure = exposure;
                SdfAssetPath asset;
                const UsdAttribute file = prim.GetAttribute(UsdLuxTokens->inputsTextureFile);
                file.Get(&asset, time);
                dome.texture = ResolvedLightTexture(asset);
                // As in the Hydra delegate: a white dome. Light textures are
                // linear Rec.709, like the light's color: the delegate cannot
                // see attribute colorSpace metadata (docs/color_management.md).
                if (!asset.GetAssetPath().empty() && dome.texture.empty())
                    std::cerr << "PrismRT: could not resolve dome texture '" << asset.GetAssetPath()
                              << "'; the dome is white\n";
                dome.textureColorSpace = color::kLightColorSpace;
                dome.transform = Constant(ToMatrix4d(world));
                light.GetDiffuseAttr().Get(&dome.diffuse, time);
                light.GetSpecularAttr().Get(&dome.specular, time);
                domes.push_back(std::move(dome));
                continue;
            }
            LightDesc params;
            if (prim.IsA<UsdLuxSphereLight>()) {
                params.shape = LightDesc::Shape::Sphere;
                UsdLuxSphereLight(prim).GetRadiusAttr().Get(&params.radius, time);
                UsdLuxSphereLight(prim).GetTreatAsPointAttr().Get(&params.treatAsPoint, time);
            } else if (prim.IsA<UsdLuxRectLight>()) {
                params.shape = LightDesc::Shape::Rect;
                UsdLuxRectLight(prim).GetWidthAttr().Get(&params.width, time);
                UsdLuxRectLight(prim).GetHeightAttr().Get(&params.height, time);
            } else if (prim.IsA<UsdLuxDiskLight>()) {
                params.shape = LightDesc::Shape::Disk;
                UsdLuxDiskLight(prim).GetRadiusAttr().Get(&params.radius, time);
            } else if (prim.IsA<UsdLuxCylinderLight>()) {
                params.shape = LightDesc::Shape::Cylinder;
                UsdLuxCylinderLight(prim).GetRadiusAttr().Get(&params.radius, time);
                UsdLuxCylinderLight(prim).GetLengthAttr().Get(&params.length, time);
            } else if (prim.IsA<UsdLuxDistantLight>()) {
                params.shape = LightDesc::Shape::Distant;
                UsdLuxDistantLight(prim).GetAngleAttr().Get(&params.angle, time);
            } else {
                std::cerr << "PrismRT: light type '" << prim.GetTypeName() << "' is not supported; skipped\n";
                continue;
            }
            params.color = Color3(color[0], color[1], color[2]);
            params.intensity = intensity;
            params.exposure = exposure;
            light.GetNormalizeAttr().Get(&params.normalize, time);
            light.GetDiffuseAttr().Get(&params.diffuse, time);
            light.GetSpecularAttr().Get(&params.specular, time);
            params.shaping = ShapingParameters(prim, time);
            // Hydra's light:cameraVisibility, which UsdImaging reads from the
            // prim's `cameraVisibility` attribute.
            if (const UsdAttribute visibility = prim.GetAttribute(TfToken("cameraVisibility")))
                visibility.Get(&params.cameraVisible, time);
            params.transform = Constant(ToMatrix4d(world));
            params.lightLinkCategory = links.category(light.GetLightLinkCollectionAPI(), false);
            params.shadowLinkCategory = links.category(light.GetShadowLinkCollectionAPI(), true);
            if (params.shape == LightDesc::Shape::Rect) {
                SdfAssetPath asset;
                const UsdAttribute file = UsdLuxRectLight(prim).GetTextureFileAttr();
                file.Get(&asset, time);
                params.texture = ResolvedLightTexture(asset);
                params.textureColorSpace = color::kLightColorSpace;
            }
            lights.push_back(std::move(params));
        }
    }

    // ── Materials and gprims.
    USDMesh::LoadOptions load;
    load.bindingPurposes = options.bindingPurposes;
    load.includedPurposes = options.includedPurposes;
    load.time = time;
    load.renderer = renderer.options();
    load.materials = renderer.materialSettings();
    const std::string libraries = renderer.options().materials.materialxLibraries;
    const std::string mdlPath = renderer.options().materials.materialxMdlPath.empty()
        ? libraries + "/mdl" : renderer.options().materials.materialxMdlPath;
    mdl::Context* context = nullptr;
    if (!libraries.empty()) {
        mdl::Context& shared = materials::SharedMdlContext();
        if (!shared.is_valid()) throw Error(ErrorCode::Internal, shared.get_error());
        context = &shared;
    }
    USDMesh reader(stage, context, libraries, mdlPath, std::move(load));
    result.meshPlacements = reader.meshPlacementCount();
    result.spheres = reader.sphereCount();
    result.materials = reader.materials.size();
    reader.emit(scene, links.active() ? USDMesh::LinkCategories(
        [&](SdfPath const& path, std::vector<std::string>* light, std::vector<std::string>* shadow) {
            links.categories(path, light, shadow);
        }) : USDMesh::LinkCategories());

    for (auto const& light : lights) scene.addLight(light);
    for (auto const& dome : domes) scene.addEnvironment(dome);
    result.lights = lights.size();
    result.domes = domes.size();
    return result;
}

} // namespace prism::usd
