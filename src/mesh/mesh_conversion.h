#pragma once

// Polygon and subdivision mesh conversion shared by the loaders: the Hydra
// delegate (src/hydra/scene_bridge.cpp, from scene-index data sources), the
// standalone USD loader (src/mesh/usd_mesh.h, from UsdGeomMesh) and the OBJ
// loader's displaced faces (src/mesh/obj_mesh.h). Each describes its mesh as
// a MeshSource: std-only views (spans) of arrays the loader keeps alive (the
// USD loaders' Vt arrays through src/usd/usd_mesh_source.h). ConvertMesh
// triangulates it into a prototype-space Mesh, refining subdivision surfaces
// (OpenSubdiv, subdivision.h) and displacing it by its material at load time.

#include "../internal_export.h"
#include "../geometry.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// A primvar's interpolation (USD's interpolation tokens; Unauthored: none
// authored, Other: a token this conversion does not know).
enum class Interpolation { Unauthored, Constant, Uniform, Varying, Vertex, FaceVarying, Instance, Other };

// The interpolation a USD interpolation token names ("constant", "uniform",
// "varying", "vertex", "faceVarying", "instance"; empty: Unauthored).
PRISM_INTERNAL_API Interpolation InterpolationFromName(std::string_view name);

// A primvar's value (indices applied): `size` elements of `components`
// scalars of `type`, contiguous at `data`, which its loader keeps alive.
// `array`: an array value (one element per point, face, ...), not a single
// value. Empty: no value; Unsupported: a value of another type.
struct PrimvarView {
    enum class Type { Empty, Unsupported, Float, Double, Int };
    Interpolation interpolation = Interpolation::Unauthored;
    Type type = Type::Empty;
    int components = 0;
    bool array = false;
    void const *data = nullptr;
    size_t size = 0;

    bool empty() const { return type == Type::Empty; }
    // An array of `n`-component floats or doubles (points, normals, UVs).
    bool floatArray(int n) const { return array && components == n && type == Type::Float; }
    bool doubleArray(int n) const { return array && components == n && type == Type::Double; }
    float const *floats() const { return static_cast<float const *>(data); }
    double const *doubles() const { return static_cast<double const *>(data); }
    int const *ints() const { return static_cast<int const *>(data); }
};

// The mesh prim as the loaders read it. Empty strings are unauthored.
struct MeshSource {
    std::span<const int> faceVertexCounts, faceVertexIndices, holeIndices;
    PrimvarView points;   // 3 floats or doubles per point
    PrimvarView normals;  // primvars:normals over normals
    PrimvarView uvs;      // primvar st, else uv
    bool multipleUvSets = false;
    // The other primvars (inherited constant ones included), for the mesh's
    // named primvars (geompropvalue lookups); points, normals and
    // namespaced names are skipped.
    std::vector<std::pair<std::string_view, PrimvarView>> primvars;
    std::string_view subdivisionScheme;  // empty: none (a polygon mesh)
    std::string_view orientation;        // empty: rightHanded
    std::optional<bool> doubleSided;     // unauthored: single-sided
    // Subdivision tags.
    std::span<const int> creaseIndices, creaseLengths, cornerIndices;
    std::span<const float> creaseSharpnesses, cornerSharpnesses;
    std::string_view interpolateBoundary, faceVaryingLinearInterpolation, triangleSubdivisionRule;
    bool motionBlur = false;
};

// A named primvar of a mesh for materials (geompropvalue), as floats.
struct NamedPrimvar {
    std::string name;
    int components = 0;
    Interpolation interpolation = Interpolation::Unauthored;
    std::vector<float> values;
};

// The primvars of a mesh other than its geometry (points, normals; texture
// coordinates are included), sorted by name, whose size fits their
// interpolation: the mesh's named primvars. Constant displayColor and
// displayOpacity are left to the fallback material.
std::vector<NamedPrimvar> NamedPrimvars(MeshSource const &source, size_t points, size_t faces, size_t corners);

// Values of a primvar as floats (scalars, vectors; arrays or single values).
PRISM_INTERNAL_API bool PrimvarFloats(PrimvarView const &value, std::vector<float> *out, int *components);

// The index of a primvar value at one face corner for its interpolation.
size_t PrimvarIndex(Interpolation interpolation, size_t pointIndex, size_t cornerIndex, size_t faceIndex);

// The subdivision scheme a mesh refines with: the authored one, bilinear for
// displaced polygon meshes, "none" for polygon meshes.
std::string_view RefinementScheme(std::string_view authored, bool displaced);

// The faces a mesh refines to at each subdivision level up to the one its
// conversion would use (index = level); a single entry, its own face count,
// for meshes that do not refine. Reads only the topology.
PRISM_INTERNAL_API std::vector<size_t> RefinementFaces(std::span<const int> faceVertexCounts, std::string_view scheme,
                                    bool displaced, SubdivisionSettings const &subdivision);

// Highest subdivision level for each mesh such that all of them together
// stay within the scene's refinement budget. Meshes are lowered one level
// at a time, always the one currently refining to the most faces, so dense
// meshes (whose control hulls are already fine) give up refinement first.
// Fixed entries (already converted prototypes) count but are not lowered.
PRISM_INTERNAL_API std::vector<int> BudgetSubdivisionLevels(std::vector<std::vector<size_t>> const &faces,
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
PRISM_INTERNAL_API MeshConversion ConvertMesh(MeshSource const &source, SubdivisionSettings const &subdivision,
                           Material const *displacer = nullptr, int levelCap = -1);

} // namespace prism::mesh
