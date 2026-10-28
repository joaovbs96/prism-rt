#pragma once

// Tests that need a converted USD scene build it through the public scene
// API, as the CLI does: a renderer, prism::usd::Load, commit, and the
// committed SceneManager.

#include <prism/renderer.h>
#include <prism/usd.h>

#include "../src/renderer/renderer_context.h"
#include "../src/scene.h"
#include "../src/scene/scene_internal.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace prism::test {

// The environment's renderer options with the built-in colour config, and
// the MaterialX libraries' mdl directory (what these tests compiled with).
inline RendererOptions TestRendererOptions()
{
    RendererOptions options = RendererOptions::FromEnvironment();
    options.color = {};
    options.materials.materialxMdlPath = options.materials.materialxLibraries + "/mdl";
    return options;
}

// Loads and commits a USD file into `scene`; returns its SceneManager.
inline SceneManager& LoadUsd(Scene& scene, std::string const& path, usd::LoadResult* loaded = nullptr)
{
    auto stage = pxr::UsdStage::Open(path);
    if (!stage) throw std::runtime_error("cannot open " + path);
    const usd::LoadResult result = usd::Load(scene, stage);
    if (loaded) *loaded = result;
    scene.commit();
    return detail::SceneAccess::committed(scene);
}

} // namespace prism::test
