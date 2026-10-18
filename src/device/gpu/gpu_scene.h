#pragma once

// API-neutral flattening of a SceneManager into the buffers shared by the GPU
// devices. Every SceneManager placement becomes one top-level instance of a
// prototype (one bottom-level structure per distinct Geometry). Materials,
// textures and the environment become records that the kernels evaluate with
// the same mathematics as the CPU classes.

#include "../hlsl_math.h"
#include "../../../shaders/shared/prism_gpu_types.h"
#include "../../../shaders/integrator/lights.h"
#include "gpu_materials.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

class SceneManager;
class MDLMaterial;

namespace prism::device::gpu {


struct GpuSceneData {
    // Geometry. Positions are packed float3 (bottom-level vertex input).
    std::vector<float> positions, normals, texcoords, tangents;
    std::vector<uint32_t> indices; // per prototype, relative to its first vertex
    std::vector<GpuPrototype> prototypes;
    std::vector<float4> spheres;   // center, radius
    std::vector<GpuInstance> instances;
    // Row-major 3x4 object-to-world matrix per instance.
    std::vector<std::array<float, 12>> instanceTransforms;
    std::vector<uint32_t> primitiveMaterials;

    // Shading.
    std::vector<GpuMaterial> materials;
    std::vector<GpuLegacyTexture> legacyTextures;
    std::vector<std::shared_ptr<const prism::TextureResource>> images;
    // MDL materials referenced by materials of kind MDL (info.y indexes this
    // list until ApplyMdlShaderCode replaces it by the dispatch index).
    std::vector<MDLMaterial const*> mdlMaterials;
    std::vector<GpuMdlTexture> mdlTextures; // index 0 invalid
    std::vector<uint32_t> mdlTiles;
    std::vector<float> bsdfData;

    // Environment: one record per dome (at least one).
    std::vector<GpuEnvironment> environments;
    // Importance tables of the lat-long environments (GpuEnvironment.info3.x
    // offsets); those of a single map are shared between scene builds of
    // the same environment (devices upload them once).
    struct EnvironmentTables {
        std::vector<float> texelPdf, aliasProbability;
        std::vector<uint32_t> alias;
    };
    std::shared_ptr<const EnvironmentTables> environmentTables;

    // Lights (device/light_table.h): analytic lights first, then emissive
    // triangles; the alias table has one more entry for the domes.
    std::vector<LightRecord> lights;
    std::vector<float4> lightAlias;
    uint32_t analyticLights = 0;

    // Integrator settings of the SceneManager.
    // Some instance has a material that may transmit shadow rays.
    bool hasCutouts = true;
    int maxDepth = 32;
    int rouletteDepth = 4;
    bool environmentMis = true;
    bool filteredTextures = true;

    std::vector<std::string> warnings;
    size_t triangleCount() const { return indices.size() / 3; }
};

GpuSceneData BuildGpuScene(SceneManager& scene);

// Whether two scenes have the same prototype geometry (vertex data, indices,
// prototypes, spheres), so their per-prototype acceleration structures and
// geometry buffers can be kept. Placements and materials may differ.
bool SameGeometry(GpuSceneData const& a, GpuSceneData const& b);

// Records the MDL link unit in the scene data: dispatch indices and argument
// block offsets of MDL materials (invalid ones become magenta), and the MDL
// texture records (appending their images).
void ApplyMdlShaderCode(GpuSceneData& data, MdlShaderCode const& code);

} // namespace prism::device::gpu
