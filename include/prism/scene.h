#pragma once

// The public scene (docs/sdk_design.md, sections 3.1, 3.5 and 5): meshes,
// analytic spheres, instances, materials, lights, environments and cameras
// as handles; commit() publishes them as a renderable revision.
//
// Edit model: one writer per scene (external synchronisation); edits are
// recorded and nothing changes for rendering until commit(). commit() is
// incremental: it rebuilds what its edits reach (DirtyKind) and reuses the
// rest (compiled materials, mesh conversions, the acceleration structure);
// sessions keep rendering the previous revision until the new one is
// published.

#include "material.h"
#include "types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace prism {

class Renderer;
namespace detail { struct SceneAccess; }

// Handles: a 32-bit slot and a 32-bit generation; 0 is no handle. A removed
// handle throws Error(InvalidHandle); handles of another scene are not
// detected (they may name an object of this one).
template <class Tag>
struct Handle {
    uint64_t v = 0;
    explicit operator bool() const { return v != 0; }
    bool operator==(Handle const&) const = default;
    auto operator<=>(Handle const&) const = default;
};
using MeshId = Handle<struct MeshTag>;
using SpheresId = Handle<struct SpheresTag>;
using InstanceId = Handle<struct InstanceTag>;
using MaterialId = Handle<struct MaterialTag>;
using LightId = Handle<struct LightTag>;
using EnvironmentId = Handle<struct EnvironmentTag>;
using CameraId = Handle<struct CameraTag>;

// USD's primvar interpolations.
enum class Interpolation { Constant, Uniform, Varying, Vertex, FaceVarying, Instance };

// Named data for MDL scene-data lookups (MaterialX geompropvalue), as floats,
// doubles or ints of 1-4 components (constant ones: one element).
struct Primvar {
    std::string name;
    Interpolation interpolation = Interpolation::Vertex;
    BufferView data;
};

struct SubdivisionDesc {
    enum class Scheme { None, CatmullClark, Loop, Bilinear } scheme = Scheme::None;
    // Highest level for this mesh (-1: the scene's SceneOptions); the scene's
    // face limits and refinement budget may lower it.
    int level = -1;
    BufferView creaseIndices, creaseLengths, creaseSharpness;   // Int32, Int32, Float32
    BufferView cornerIndices, cornerSharpness;                  // Int32, Float32
    BufferView holeIndices;  // Int32 faces that are holes (polygon meshes too)
    // USD's subdivision tags by token (empty: USD's fallback).
    std::string interpolateBoundary, faceVaryingLinearInterpolation, triangleSubdivisionRule;
};

// A mesh in its own (prototype) space.
//
// Two forms:
// - Triangle list (faceVertexCounts empty): faceVertexIndices are triangles;
//   normals, uvs, tangents and primvars are per vertex (or constant) and are
//   used as given. Normals default to area-weighted ones, uvs to zero.
// - Polygon mesh (faceVertexCounts given): faces are fan-triangulated as
//   USD's; normals are generated (area-weighted) where none are authored and
//   normalised; subdivision surfaces refine and displacing materials displace
//   it at commit (SceneOptions limits).
// A triangle list with face-varying or uniform attributes is converted as a
// polygon mesh of triangles.
struct MeshDesc {
    Sampled<BufferView> points;   // 3 Float32 or Float64 per point; one sample renders (section 5)
    BufferView faceVertexCounts;  // Int32 per face; empty: a triangle list
    BufferView faceVertexIndices; // Int32 or UInt32
    BufferView normals;
    Interpolation normalsInterpolation = Interpolation::Vertex;
    BufferView uvs;               // 2 Float32 or Float64
    Interpolation uvsInterpolation = Interpolation::FaceVarying;
    BufferView tangents;          // triangle lists: 4 Float32 per vertex (glTF TANGENT); empty: from the uvs
    std::vector<Primvar> primvars;
    BufferView velocities, accelerations;  // reserved (section 5): accepted, not rendered
    SubdivisionDesc subdivision;
    bool leftHanded = false;      // USD orientation leftHanded
    // Per-face material slots (Int32): an index into the instance's
    // materials. Empty: slot 0 everywhere.
    BufferView faceMaterialSlots;
    bool doubleSided = true;
    BufferView singleSidedFaces;  // UInt8 per face: 1 for single-sided faces (with doubleSided true)
    // Placed through Embree instances even when it has a single instance
    // (USD native and point instancing); otherwise a mesh with one instance
    // that does not refine is baked into world space.
    bool instanced = false;
};

// Analytic spheres in their own space.
struct SpheresDesc {
    Sampled<BufferView> centers;  // 3 Float32 per sphere; one sample renders
    BufferView radii;             // 1 Float32 per sphere
};

struct InstanceDesc {
    std::variant<MeshId, SpheresId> prototype;
    Sampled<Matrix4d> transform = Constant(Matrix4d());  // local to world
    // Material slots; slot 0 is the default (and the fallback of empty slots).
    // Spheres use slot 0. No material: a grey Diffuse.
    std::vector<MaterialId> materials;
    std::vector<Primvar> primvars;  // instance-rate (constant interpolation)
    bool visible = true;
    // Light and shadow linking: the categories (LightDesc::lightLinkCategory
    // and shadowLinkCategory) this instance belongs to.
    std::vector<std::string> lightLinkCategories, shadowLinkCategories;
    // The front end's numbering, reported by the id AOVs (Hydra prim ids).
    int32_t primId = -1, instanceId = -1;
    uint64_t userData = 0;
};

// UsdLux's analytic lights. `color` is linear Rec.709 (as UsdLux authors it,
// colour temperature already applied).
struct LightDesc {
    enum class Shape { Sphere, Rect, Disk, Cylinder, Distant, Point };
    struct Shaping {  // UsdLuxShapingAPI
        bool active = false;
        float focus = 0.0f;
        Color3 focusTint{0.0f};   // linear Rec.709
        float coneAngle = 90.0f, coneSoftness = 0.0f;
        std::string iesFile;      // empty: none
        float iesAngleScale = 0.0f;
        bool iesNormalize = false;
    };
    Shape shape = Shape::Sphere;
    Sampled<Matrix4d> transform = Constant(Matrix4d());
    Color3 color{1.0f};
    float intensity = 1, exposure = 0, diffuse = 1, specular = 1;
    bool normalize = false, cameraVisible = false;
    float radius = 0.5f, width = 1, height = 1, length = 1, angle = 0.53f;
    bool treatAsPoint = false;
    Shaping shaping;
    std::string texture, textureColorSpace = "lin_rec709";  // rect lights
    std::string lightLinkCategory, shadowLinkCategory;      // empty: every surface
};

struct EnvironmentDesc {
    enum class Kind {
        Dome,      // UsdLuxDomeLight: lat-long `texture` (white when empty) placed by `transform`
        LatLong,   // the lat-long `texture` in its own axes (no transform)
        Constant,  // a constant radiance of the colour
    };
    Kind kind = Kind::Dome;
    std::string texture, textureColorSpace = "lin_rec709";
    // The colour multiplier, in colorSpace: "lin_rec709", or empty for the
    // scene's working space. Radiance = texel * color * intensity * 2^exposure.
    Color3 color{1.0f};
    std::string colorSpace = "lin_rec709";
    float intensity = 1, exposure = 0;
    float diffuse = 1, specular = 1;  // Dome and LatLong: as diffuse and glossy lobes see it
    Sampled<Matrix4d> transform = Constant(Matrix4d());  // Dome
};

struct CameraDesc {
    Sampled<Matrix4d> cameraToWorld = Constant(Matrix4d());
    enum class Projection { Perspective, Orthographic } projection = Projection::Perspective;
    // USD camera units: tenths of the scene unit for focal length and apertures.
    float focalLength = 50, horizontalAperture = 20.955f, verticalAperture = 15.2908f,
          horizontalApertureOffset = 0, verticalApertureOffset = 0;
    float nearClip = 0.001f, farClip = 1e30f;
    float fStop = 0, focusDistance = 1;
    float exposureScale = 1;
};

struct SceneOptions {
    // Subdivision surfaces (prism:subdivisionLevel etc.).
    int subdivisionLevel = 2;
    uint64_t subdivisionMaxFaces = uint64_t(1) << 16;
    uint64_t subdivisionBudgetFaces = uint64_t(24) << 20;
    // Meshes their material displaces (prism:displacementLevel etc.).
    int displacementLevel = 4;
    uint64_t displacementMaxFaces = uint64_t(1) << 18;
    // With defaultDome, a scene with no lights, emissive geometry or
    // environment is lit by defaultDomeTexture as an untransformed dome
    // (usdview's default dome); without the texture it stays black.
    bool defaultDome = false;
    std::string defaultDomeTexture;
    double timeCodesPerSecond = 24;  // reserved (section 5)
};

enum class DiagnosticLevel { Info, Warning, Error };
struct Diagnostic {
    DiagnosticLevel level = DiagnosticLevel::Warning;
    std::string category;  // "material", "geometry", "environment", "light"
    std::string message;
};

// The kinds of edits (CommitResult::dirty), as the Hydra delegate classifies
// its scene's changes. A commit rebuilds only what its edits reach.
enum class DirtyKind : uint32_t {
    Placement = 1u << 0,    // instance transforms and visibility
    Prototype = 1u << 1,    // mesh and spheres data
    Material = 1u << 2,     // materials, and the materials of instances
    Light = 1u << 3,        // lights
    Environment = 1u << 4,  // environments
    Structure = 1u << 5,    // added or removed meshes, spheres and instances; instance updates
};
constexpr uint32_t operator|(DirtyKind a, DirtyKind b) { return uint32_t(a) | uint32_t(b); }
constexpr uint32_t operator|(uint32_t a, DirtyKind b) { return a | uint32_t(b); }
constexpr bool Has(uint32_t bits, DirtyKind kind) { return (bits & uint32_t(kind)) != 0; }

struct CommitResult {
    uint64_t revision = 0;  // the revision now committed (unchanged when there was nothing to commit)
    size_t meshes = 0, instances = 0, triangles = 0, materials = 0, lights = 0;
    double seconds = 0;
    // What the commit did. `dirty`: the DirtyKind bits of the edits it
    // published (every kind for the first commit; 0: nothing to commit, no
    // new revision).
    uint32_t dirty = 0;
    size_t materialsCompiled = 0;
    // Mesh conversions done, and the ones taken over from earlier revisions.
    size_t meshesConverted = 0, meshesReused = 0;
    // Placements attached to the acceleration structure, and placements of a
    // reused revision updated in place (transform, visibility, materials,
    // primvars).
    size_t placementsAttached = 0, placementsUpdated = 0;
    // Lighting edits alone: the revision shares the previous one's
    // acceleration structure.
    bool accelerationShared = false;
    // The revision before the previous one (no longer rendered) was updated
    // in place instead of building a new one (double buffering).
    bool revisionReused = false;
    bool lightingRebuilt = false;
};

class PRISM_API Scene {
public:
    // Scenes come from Renderer::createScene.
    Scene(Scene const&) = default;
    Scene& operator=(Scene const&) = default;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(Scene&&) noexcept = default;
    ~Scene();

    MeshId addMesh(MeshDesc const&);
    void updateMesh(MeshId, MeshDesc const&);
    void updateMeshPoints(MeshId, Sampled<BufferView> const&);
    SpheresId addSpheres(SpheresDesc const&);
    void updateSpheres(SpheresId, SpheresDesc const&);
    InstanceId addInstance(InstanceDesc const&);
    void updateInstance(InstanceId, InstanceDesc const&);
    void setTransform(InstanceId, Sampled<Matrix4d> const&);
    void setVisible(InstanceId, bool);
    void setMaterials(InstanceId, std::vector<MaterialId> const&);
    MaterialId addMaterial(MaterialDesc const&);
    void updateMaterial(MaterialId, MaterialDesc const&);
    LightId addLight(LightDesc const&);
    void updateLight(LightId, LightDesc const&);
    // Several environments add up (several domes).
    EnvironmentId addEnvironment(EnvironmentDesc const&);
    void updateEnvironment(EnvironmentId, EnvironmentDesc const&);
    CameraId addCamera(CameraDesc const&);
    void updateCamera(CameraId, CameraDesc const&);
    CameraDesc camera(CameraId) const;

    void remove(MeshId);
    void remove(SpheresId);
    void remove(InstanceId);
    void remove(MaterialId);
    void remove(LightId);
    void remove(EnvironmentId);
    void remove(CameraId);

    // Publishes the edits as a new revision: compiles new materials, converts
    // the meshes whose conversion changed (subdivision, displacement), loads
    // light and environment textures when they changed and updates or
    // builds the acceleration structure. The new revision is built beside
    // the published one (or the one before it, when nothing renders that
    // one any more, is updated in place); sessions take it at their next
    // pass. Without edits since the last commit it publishes nothing.
    // Blocking. Material failures are diagnostics (the material renders
    // magenta); invalid references throw. The callback hears each phase and
    // material on this thread; returning false stops the commit
    // (Error(Cancelled); the last committed revision stays, and the edits
    // stay pending).
    //
    // Hidden instances (setVisible(false)) keep their mesh conversions, so
    // showing them again only places them.
    CommitResult commit(ProgressCallback const& progress = {});
    // Revisions committed so far (0: none).
    uint64_t revision() const;
    // Diagnostics of the last commit (of the work it did).
    std::vector<Diagnostic> diagnostics() const;

    SceneOptions const& options() const;

    struct Impl;

private:
    friend class Renderer;
    friend struct detail::SceneAccess;
    explicit Scene(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

} // namespace prism
