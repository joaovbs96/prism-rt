#pragma once

// Internal access to a public prism::Scene for the code in this repository
// (the loader modules and the CLI), not part of the public API: materials the
// modules compile themselves (USD's material service, docs/sdk_design.md
// 3.6), the committed SceneManager the CLI renders until the session API
// exists (migration step 4), and the render settings that still live on
// SceneManager (docs/DECISIONS.md, 2026-10-28: deferred from step 2).

#include <prism/scene.h>

#include "../mesh/mesh_conversion.h"
#include "../texture.h"

#include <memory>
#include <string>

class Material;
class SceneManager;

namespace prism {
class RendererContext;
}

namespace prism::detail {

// What the id AOVs report for an Embree placement of the committed scene.
struct PlacementInfo {
    InstanceId instance;
    int primId = -1;
    int instanceId = -1;
    uint64_t userData = 0;
    // The mesh's coarse face per triangle (elementId); empty: the triangle.
    prism::mesh::SharedFaceTable coarseFaces;
};

// SceneManager's render settings (section 2.7 of the design), applied to
// each committed SceneManager until sessions take them.
struct SceneRenderSettings {
    int maxDepth = 32;
    int rouletteDepth = 4;
    bool environmentSampling = true;
    prism::TextureFiltering textureFiltering = prism::TextureFiltering::Filtered;
    bool honorDoubleSided = false;
    bool textureStreaming = true;
    int gpuTextureBudgetMiB = 0;
};

struct SceneAccess {
    static RendererContext& context(Scene const& scene);
    // A material the caller compiled (or a built-in one): the scene holds
    // it while any revision uses it. A null material is rejected.
    static MaterialId addCompiledMaterial(Scene& scene, std::shared_ptr<Material> material, std::string name);
    static void setRenderSettings(Scene& scene, SceneRenderSettings const& settings);
    // The last committed revision's scene; throws when nothing is committed.
    static SceneManager& committed(Scene& scene);
    // The placement of an Embree geometry id of the committed scene, or null.
    static PlacementInfo const* placement(Scene const& scene, unsigned geometryId);
};

} // namespace prism::detail
