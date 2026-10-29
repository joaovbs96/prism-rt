#pragma once

// USD stages into a public prism::Scene (module prism_usd). The only public
// header with pxr types (docs/sdk_design.md, section 4.1).

#include "scene.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <string>
#include <utility>
#include <vector>

namespace prism::usd {

struct LoadOptions {
    // Material bindings resolve for the first of these purposes that has one
    // (UsdRenderSettings materialBindingPurposes; empty: full, then
    // allPurpose).
    std::vector<pxr::TfToken> bindingPurposes;
    // Purposes rendered besides default (UsdRenderSettings includedPurposes);
    // empty: render.
    std::vector<pxr::TfToken> includedPurposes;
    // The time code attributes, transforms and skinning are read at.
    pxr::UsdTimeCode time = pxr::UsdTimeCode::Default();
    // Dome lights become environments (off when the front end lights the
    // scene itself, as the CLI's --env).
    bool domes = true;
    // Materials of the scene that replace the bound material of the gprims
    // at or under a prim path (all of their face subsets too; the longest
    // matching path wins). The material preview binds its material this way.
    std::vector<std::pair<pxr::SdfPath, MaterialId>> materialOverrides;
};

struct LoadResult {
    size_t meshPlacements = 0, spheres = 0;  // placements that render
    size_t lights = 0, domes = 0;
    size_t materials = 0;                    // compiled MaterialX materials
    size_t renderablePlacements() const { return meshPlacements + spheres; }
};

// Adds the stage's gprims (meshes, implicit surfaces, analytic spheres; with
// native and point instancing, UsdSkel skinning, GeomSubset materials,
// primvars), its UsdShade materials (compiled here, through the material
// service shared with the Hydra delegate), its UsdLux lights with light and
// shadow linking, and its domes. Meshes convert at the scene's commit
// (subdivision and displacement by the scene's options).
PRISM_USD_API LoadResult Load(Scene& scene, pxr::UsdStageRefPtr const& stage, LoadOptions const& options = {});

// A GfCamera as a CameraDesc (the same float lens values; sessions frame it
// as CameraUtil does). exposureScale stays 1.
PRISM_USD_API CameraDesc CameraFromGf(pxr::GfCamera const& camera);
// A USD camera at `time`, with its exposure (UsdGeomCamera::
// ComputeLinearExposureScale; 1 when not finite). Throws Error(NotFound)
// when `path` is not a camera.
PRISM_USD_API CameraDesc LoadCamera(pxr::UsdStageRefPtr const& stage, pxr::SdfPath const& path,
                                    pxr::UsdTimeCode time = pxr::UsdTimeCode::Default());

// The built-in material preview (docs/material_preview.md): the USD-WG
// StandardShaderBall (its subdivision-surface variant) in the asset's own
// lit box, with one material bound to the ball's material surface. The stand,
// the box and the lights keep the asset's materials and lights.
struct MaterialPreviewOptions {
    // The preview stage; empty: MaterialPreviewScene().
    std::string scene;
    pxr::UsdTimeCode time = pxr::UsdTimeCode::Default();
};

struct MaterialPreview {
    LoadResult load;              // the stage's placements, lights and materials
    MaterialId material;          // the previewed material (added to the scene)
    // The document's material that is previewed (MaterialX documents: the
    // named one, else the first), and all of the document's materials.
    std::string materialName;
    std::vector<std::string> documentMaterials;
    CameraDesc camera;            // the asset's camera (not added to the scene)
};

// The shipped preview stage: $PRISM_MATERIAL_PREVIEW_SCENE when set, else
// resources/material_preview.usda (the shaderball asset with its
// standard_surface variant selected) beside the prism_usd library (or one
// directory up). Throws Error(NotFound) when there is none.
PRISM_USD_API std::string MaterialPreviewScene();

// Adds the preview stage to `scene` with `material` bound to the shaderball's
// material surface; the caller commits and renders from `camera`. A MaterialX
// document is checked first: a missing file throws Error(NotFound), an
// unreadable document or one without materials Error(InvalidArgument), and a
// materialName the document does not have Error(NotFound) (the message lists
// its materials). Without a materialName the document's first material is
// previewed (MaterialDesc's rule), also when it has several.
PRISM_USD_API MaterialPreview LoadMaterialPreview(Scene& scene, MaterialDesc material,
                                                  MaterialPreviewOptions const& options = {});

} // namespace prism::usd
