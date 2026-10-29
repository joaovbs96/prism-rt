#pragma once

// USD stages into a public prism::Scene (module prism_usd). The only public
// header with pxr types (docs/sdk_design.md, section 4.1).

#include "scene.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

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

} // namespace prism::usd
