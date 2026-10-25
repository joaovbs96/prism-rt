#pragma once

// Polygon and subdivision mesh conversion shared by both USD loaders: the
// Hydra delegate (src/hydra/scene_bridge.cpp, from scene-index data sources)
// and the standalone CLI (src/mesh/usd_mesh.h, from UsdGeomMesh). Each fills
// a MeshSource; ConvertMesh triangulates it into a prototype-space Mesh,
// refining subdivision surfaces (OpenSubdiv, subdivision.h) and displacing
// it by its material at load time.

#include "../geometry.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Material;

namespace prism::mesh {

// Uniform subdivision of subdivision surfaces: the highest level, and the
// faces one mesh may refine to (meshes denser than that use lower levels, and
// their control hulls beyond it): prism:subdivisionLevel (0 renders control
// hulls) and prism:subdivisionMaxFaces, which default to
// PRISM_SUBDIVISION_LEVEL and PRISM_SUBDIVISION_MAX_FACES.
struct SubdivisionSettings {
    int level = 2;
    size_t maxFaces = size_t(1) << 16;
    // Meshes whose material displaces them (prism:displacementLevel,
    // prism:displacementMaxFaces).
    int displacementLevel = 4;
    size_t displacementMaxFaces = size_t(1) << 18;
    // Faces all refined prototypes of one scene build may add up to
    // (prism:subdivisionBudgetFaces). Beyond it the meshes that refine to
    // the most faces drop levels first. Each face costs a few hundred bytes
    // once triangulated, with normals, UVs, tangents and its Embree BVH.
    size_t budgetFaces = size_t(24) << 20;

    bool operator==(SubdivisionSettings const &) const = default;
};

// Coarse-face metadata is immutable and shared by all placements of a prototype.
class SharedFaceTable {
public:
    SharedFaceTable() = default;
    SharedFaceTable(std::vector<int> values)
        : values(std::make_shared<const std::vector<int>>(std::move(values))) {}
    size_t size() const { return values ? values->size() : 0; }
    int operator[](size_t i) const { return (*values)[i]; }
private:
    std::shared_ptr<const std::vector<int>> values;
};

// Everything mesh conversion derives from a prim except the Mesh itself. It
// is small enough to cache beside shared storage, so a reused conversion
// still replays its warnings and material-subset metadata.
struct MeshConversionInfo {
    SharedFaceTable coarseFaces;
    size_t faceCount = 0;
    size_t geometryHash = 0;
    size_t malformedFaceCount = 0;
    size_t degenerateFaceCount = 0;
    // Warnings after the motion check apply only once topology was readable.
    bool topologyRead = false;
    bool motionBlur = false;
    bool multipleUvSets = false;
    bool subdivisionControlHull = false;
    int subdivisionLevel = 0;
    bool missingPoints = false;
    bool unsupportedPoints = false;
};

// Geometry allocates its transform as a raw pointer; free it with a mesh that
// is discarded because identical storage already exists.
struct ConvertedMeshDeleter {
    void operator()(Mesh *mesh) const
    {
        if (!mesh) return;
        delete mesh->transform;
        delete mesh;
    }
};

struct MeshConversion {
    MeshConversionInfo info;
    // Null when no triangle survived conversion.
    std::unique_ptr<Mesh, ConvertedMeshDeleter> mesh;
};

// A primvar's value (indices applied) and interpolation token (constant,
// uniform, varying, vertex, faceVarying).
struct PrimvarData {
    pxr::TfToken interpolation;
    pxr::VtValue value;
};

// The mesh prim as both loaders read it. Empty tokens are unauthored.
struct MeshSource {
    pxr::VtIntArray faceVertexCounts, faceVertexIndices, holeIndices;
    PrimvarData points;   // VtVec3fArray or VtVec3dArray
    PrimvarData normals;  // primvars:normals over normals
    PrimvarData uvs;      // primvar st, else uv
    bool multipleUvSets = false;
    // The other primvars (inherited constant ones included), for the mesh's
    // named primvars (geompropvalue lookups); points, normals and
    // namespaced names are skipped.
    std::vector<std::pair<pxr::TfToken, PrimvarData>> primvars;
    pxr::TfToken subdivisionScheme;  // empty: none (a polygon mesh)
    pxr::TfToken orientation;        // empty: rightHanded
    std::optional<bool> doubleSided; // unauthored: single-sided
    // Subdivision tags.
    pxr::VtIntArray creaseIndices, creaseLengths, cornerIndices;
    pxr::VtFloatArray creaseSharpnesses, cornerSharpnesses;
    pxr::TfToken interpolateBoundary, faceVaryingLinearInterpolation, triangleSubdivisionRule;
    bool motionBlur = false;
};

// Values of a primvar as floats (scalars, vectors; arrays or single values).
bool PrimvarFloats(pxr::VtValue const &value, std::vector<float> *out, int *components);

// The index of a primvar value at one face corner for its interpolation.
size_t PrimvarIndex(pxr::TfToken const &interpolation, size_t pointIndex, size_t cornerIndex, size_t faceIndex);

// The subdivision scheme a mesh refines with: the authored one, bilinear for
// displaced polygon meshes, none for polygon meshes.
pxr::TfToken RefinementScheme(pxr::TfToken const &authored, bool displaced);

// The faces a mesh refines to at each subdivision level up to the one its
// conversion would use (index = level); a single entry, its own face count,
// for meshes that do not refine. Reads only the topology.
std::vector<size_t> RefinementFaces(std::span<const int> faceVertexCounts, pxr::TfToken const &scheme,
                                    bool displaced, SubdivisionSettings const &subdivision);

// Highest subdivision level for each mesh such that all of them together
// stay within the scene's refinement budget. Meshes are lowered one level
// at a time, always the one currently refining to the most faces, so dense
// meshes (whose control hulls are already fine) give up refinement first.
// Fixed entries (already converted prototypes) count but are not lowered.
std::vector<int> BudgetSubdivisionLevels(std::vector<std::vector<size_t>> const &faces,
                                         std::vector<bool> const &fixed, size_t budget,
                                         size_t *plannedFaces, size_t *requestedFaces);

// Moves the vertices of a converted mesh by its material's displacement.
// Vertices at one position (seams of normals or texture coordinates) move
// together, so the surface stays closed; normals and tangents are rebuilt.
void Displace(Mesh &mesh, Material const &material);

// Pure: touches no scene, Embree or material state, so loaders run it on TBB
// workers. The caller emits the warnings recorded in MeshConversionInfo.
// levelCap, when not negative, limits the subdivision level (see
// BudgetSubdivisionLevels).
MeshConversion ConvertMesh(MeshSource const &source, SubdivisionSettings const &subdivision,
                           Material const *displacer = nullptr, int levelCap = -1);

} // namespace prism::mesh
