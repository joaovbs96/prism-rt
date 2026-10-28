#pragma once

// Wavefront OBJ scenes into a public prism::Scene (module prism_obj; docs/obj.md).

#include "scene.h"

#include <filesystem>

namespace prism::obj {

struct LoadResult {
    MeshId mesh;          // the file's triangles (one vertex per face corner)
    InstanceId instance;  // its placement (identity)
    size_t triangles = 0;
    size_t materials = 0;       // MTL materials converted to MaterialX
    size_t failedMaterials = 0; // rendered with the error material
};

// Adds an OBJ file: one mesh of its triangles, an instance of it, and its
// MTL materials (MaterialX open_pbr_surface documents, compiled at commit;
// faces of materials with MTL disp are displaced at commit with the scene's
// displacement settings). Throws prism::Error (NotFound, or InvalidArgument
// for a file that does not load or has no triangles).
LoadResult Load(Scene& scene, std::filesystem::path const& path);

} // namespace prism::obj
