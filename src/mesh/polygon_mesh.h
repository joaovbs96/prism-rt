#pragma once

// The polygon conversion of meshes that neither refine nor displace, for the
// public scene's commit (src/scene/scene.cpp): fan triangulation, a vertex
// per point unless a primvar varies per face or face corner (then one per
// corner), positions and normals transformed by `world` (identity:
// prototype space). Normals follow Hydra's conversion: generated ones face
// along the authored winding (leftHanded flips them, authored normals too),
// and a mirroring transform turns them with the triangles (SceneManager flips
// placed prototypes the same way).
//
// It was the standalone USD loader's conversion (usd_mesh.h, with Gf/Vt
// types); this std-only version repeats Gf's arithmetic in the same order
// (GfVec3f's division through a double reciprocal, prism::Matrix4d for
// GfMatrix4d), so its meshes are bit-identical to the earlier ones.

#include "mesh_conversion.h"
#include "../matrix4d.h"

#include <vector>

namespace prism::mesh {

// Fills `out`; `coarseFaces` receives the mesh's coarse face per triangle.
// False when no triangle is left.
bool BuildPolygonMesh(MeshSource const& source, Matrix4d const& world, Mesh& out, std::vector<int>& coarseFaces);

} // namespace prism::mesh
