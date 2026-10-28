#pragma once

// glTF 2.0 scenes into a public prism::Scene (module prism_gltf; docs/gltf.md).

#include "scene.h"

#include <filesystem>

namespace prism::gltf {

struct LoadResult {
    MeshId mesh;          // the scene's triangles, merged in world space
    InstanceId instance;  // its placement (identity)
    size_t triangles = 0;
    size_t materials = 0;       // converted to MaterialX
    size_t failedMaterials = 0; // rendered with the error material
};

// Adds the default scene of a .gltf or .glb file: one mesh of all its
// triangles, an instance of it, and its materials (MaterialX gltf_pbr
// documents, compiled at commit). Throws prism::Error (NotFound, or
// InvalidArgument for a file that does not parse or has no triangles).
LoadResult Load(Scene& scene, std::filesystem::path const& path);

} // namespace prism::gltf
