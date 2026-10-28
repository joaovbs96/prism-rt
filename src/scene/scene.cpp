// The public scene (include/prism/scene.h) and its commit: a full rebuild of
// the internal SceneManager from the scene's descriptions. The conversions
// are the loaders' earlier ones, moved here unchanged where they render
// (docs/sdk_design.md, section 7, step 3): USD's per-prim placements and
// shape conversion (subdivision budget, displacement, polygon meshes baked
// into world space or placed as Embree instances), the glTF/OBJ merged
// triangle meshes and the OBJ displacement of faces, UsdLux lights, domes
// and the CLI's environments.

#include <prism/renderer.h>
#include <prism/scene.h>

#include "scene_internal.h"

#include "../batched_spheres.h"
#include "../device/light_table.h"
#include "../environment.h"
#include "../geometry.h"
#include "../hydra/fallback_material.h"
#include "../lights.h"
#include "../lights/ies.h"
#include "../material.h"
#include "../materialx/cached_document.h"
#include "../materialx/shared_mdl_context.h"
#include "../mdl/context.h"
#include "../mdl_material.h"
#include "../mesh/mesh_conversion.h"
#include "../mesh/polygon_mesh.h"
#include "../renderer/renderer_context.h"
#include "../scene.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

namespace prism {
namespace {

[[noreturn]] void Throw(ErrorCode code, std::string const& message) { throw Error(code, message); }

// Descriptions by handle: a slot index and its generation (from 1), so a
// removed handle is never valid again.
template <class T>
class Slots {
public:
    uint64_t add(T value)
    {
        uint32_t slot;
        if (!m_free.empty()) {
            slot = m_free.back();
            m_free.pop_back();
        } else {
            slot = uint32_t(m_entries.size());
            m_entries.emplace_back();
        }
        Entry& entry = m_entries[slot];
        ++entry.generation;
        entry.value = std::move(value);
        return (uint64_t(entry.generation) << 32) | slot;
    }
    T& get(uint64_t handle, const char* kind)
    {
        Entry* entry = find(handle);
        if (!entry) Throw(ErrorCode::InvalidHandle, std::string("invalid ") + kind + " handle");
        return *entry->value;
    }
    T const& get(uint64_t handle, const char* kind) const { return const_cast<Slots*>(this)->get(handle, kind); }
    bool contains(uint64_t handle) const { return const_cast<Slots*>(this)->find(handle) != nullptr; }
    void remove(uint64_t handle, const char* kind)
    {
        Entry* entry = find(handle);
        if (!entry) Throw(ErrorCode::InvalidHandle, std::string("invalid ") + kind + " handle");
        entry->value.reset();
        m_free.push_back(uint32_t(handle & 0xffffffffu));
    }
    // In slot order (the order of addition while nothing was removed).
    template <class F>
    void forEach(F const& f)
    {
        for (uint32_t slot = 0; slot < m_entries.size(); ++slot)
            if (m_entries[slot].value) f((uint64_t(m_entries[slot].generation) << 32) | slot, *m_entries[slot].value);
    }
    size_t size() const { return m_entries.size() - m_free.size(); }

private:
    struct Entry {
        uint32_t generation = 0;
        std::optional<T> value;
    };
    Entry* find(uint64_t handle)
    {
        const uint32_t slot = uint32_t(handle & 0xffffffffu), generation = uint32_t(handle >> 32);
        if (!handle || slot >= m_entries.size()) return nullptr;
        Entry& entry = m_entries[slot];
        return entry.generation == generation && entry.value ? &entry : nullptr;
    }
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_free;
};

// A view the scene may keep: the caller's memory when an owner keeps it
// alive and it is packed, otherwise a packed copy.
BufferView Retain(BufferView view, const char* what)
{
    if (view.count == 0) return BufferView{};
    if (!view.data) Throw(ErrorCode::InvalidArgument, std::string(what) + ": no data for " +
                                                          std::to_string(view.count) + " elements");
    if (view.components == 0) Throw(ErrorCode::InvalidArgument, std::string(what) + ": zero components");
    const size_t packed = view.elementBytes();
    const size_t stride = view.strideBytes ? view.strideBytes : packed;
    if (view.owner && stride == packed) {
        view.strideBytes = 0;
        return view;
    }
    auto copy = std::make_shared<std::vector<unsigned char>>(view.count * packed);
    auto const* source = static_cast<unsigned char const*>(view.data);
    for (size_t i = 0; i < view.count; ++i) std::memcpy(copy->data() + i * packed, source + i * stride, packed);
    view.data = copy->data();
    view.owner = std::shared_ptr<const void>(copy, copy->data());
    view.strideBytes = 0;
    return view;
}

void Check(bool ok, std::string const& message)
{
    if (!ok) Throw(ErrorCode::InvalidArgument, message);
}

bool IsFloat(BufferView const& v) { return v.type == DataType::Float32 || v.type == DataType::Float64; }
bool IsInt(BufferView const& v) { return v.type == DataType::Int32 || v.type == DataType::UInt32; }

void CheckArray(BufferView const& v, const char* what, uint32_t components, bool floats, bool ints)
{
    if (v.empty()) return;
    Check(!components || v.components == components,
          std::string(what) + " needs " + std::to_string(components) + " components per element");
    Check((floats && IsFloat(v)) || (ints && IsInt(v)), std::string(what) + " has an unsupported data type");
}

template <class T>
T const& Nearest(Sampled<T> const& samples)
{
    size_t best = 0;
    for (size_t i = 1; i < samples.size(); ++i)
        if (std::abs(samples[i].time) < std::abs(samples[best].time)) best = i;
    return samples[best].value;
}

MeshDesc RetainMesh(MeshDesc desc)
{
    Check(!desc.points.empty(), "a mesh needs points");
    for (auto& sample : desc.points) {
        sample.value = Retain(std::move(sample.value), "mesh points");
        CheckArray(sample.value, "mesh points", 3, true, false);
    }
    desc.faceVertexCounts = Retain(std::move(desc.faceVertexCounts), "faceVertexCounts");
    desc.faceVertexIndices = Retain(std::move(desc.faceVertexIndices), "faceVertexIndices");
    CheckArray(desc.faceVertexCounts, "faceVertexCounts", 1, false, true);
    CheckArray(desc.faceVertexIndices, "faceVertexIndices", 1, false, true);
    Check(!desc.faceVertexCounts.empty() || desc.faceVertexIndices.count % 3 == 0,
          "a triangle list needs three indices per triangle");
    desc.normals = Retain(std::move(desc.normals), "normals");
    CheckArray(desc.normals, "normals", 3, true, false);
    desc.uvs = Retain(std::move(desc.uvs), "uvs");
    CheckArray(desc.uvs, "uvs", 2, true, false);
    desc.tangents = Retain(std::move(desc.tangents), "tangents");
    Check(desc.tangents.empty() || (desc.tangents.components == 4 && desc.tangents.type == DataType::Float32),
          "tangents need 4 Float32 components");
    for (auto& primvar : desc.primvars) {
        primvar.data = Retain(std::move(primvar.data), "primvar");
        Check(primvar.data.components >= 1 && primvar.data.components <= 4, "primvar '" + primvar.name +
              "' needs 1-4 components");
        Check(primvar.data.type != DataType::UInt8, "primvar '" + primvar.name + "' has an unsupported data type");
    }
    desc.velocities = Retain(std::move(desc.velocities), "velocities");
    desc.accelerations = Retain(std::move(desc.accelerations), "accelerations");
    auto& s = desc.subdivision;
    s.creaseIndices = Retain(std::move(s.creaseIndices), "creaseIndices");
    s.creaseLengths = Retain(std::move(s.creaseLengths), "creaseLengths");
    s.creaseSharpness = Retain(std::move(s.creaseSharpness), "creaseSharpness");
    s.cornerIndices = Retain(std::move(s.cornerIndices), "cornerIndices");
    s.cornerSharpness = Retain(std::move(s.cornerSharpness), "cornerSharpness");
    s.holeIndices = Retain(std::move(s.holeIndices), "holeIndices");
    for (auto const* ints : {&s.creaseIndices, &s.creaseLengths, &s.cornerIndices, &s.holeIndices})
        Check(ints->empty() || (ints->type == DataType::Int32 && ints->components == 1),
              "subdivision tags need Int32 indices");
    for (auto const* floats : {&s.creaseSharpness, &s.cornerSharpness})
        Check(floats->empty() || (floats->type == DataType::Float32 && floats->components == 1),
              "subdivision sharpness needs Float32 values");
    desc.faceMaterialSlots = Retain(std::move(desc.faceMaterialSlots), "faceMaterialSlots");
    CheckArray(desc.faceMaterialSlots, "faceMaterialSlots", 1, false, true);
    desc.singleSidedFaces = Retain(std::move(desc.singleSidedFaces), "singleSidedFaces");
    Check(desc.singleSidedFaces.empty() ||
              (desc.singleSidedFaces.type == DataType::UInt8 && desc.singleSidedFaces.components == 1),
          "singleSidedFaces needs one UInt8 per face");
    return desc;
}

SpheresDesc RetainSpheres(SpheresDesc desc)
{
    Check(!desc.centers.empty(), "spheres need centers");
    for (auto& sample : desc.centers) {
        sample.value = Retain(std::move(sample.value), "sphere centers");
        Check(sample.value.components == 3 && sample.value.type == DataType::Float32,
              "sphere centers need 3 Float32 components");
    }
    desc.radii = Retain(std::move(desc.radii), "sphere radii");
    Check(desc.radii.components == 1 && desc.radii.type == DataType::Float32, "sphere radii need Float32 values");
    Check(!desc.radii.empty() && desc.radii.count == Nearest(desc.centers).count,
          "spheres need one radius per center");
    return desc;
}

InstanceDesc RetainInstance(InstanceDesc desc)
{
    Check(!desc.transform.empty(), "an instance needs a transform");
    for (auto& primvar : desc.primvars) {
        primvar.data = Retain(std::move(primvar.data), "instance primvar");
        Check(primvar.data.count >= 1 && primvar.data.components >= 1 && primvar.data.components <= 4 &&
                  primvar.data.type != DataType::UInt8,
              "instance primvar '" + primvar.name + "' needs 1-4 numeric components");
    }
    return desc;
}

// Typed reads of retained views.
float FloatAt(BufferView const& v, size_t i)
{
    return v.type == DataType::Float64 ? float(static_cast<double const*>(v.data)[i])
                                       : static_cast<float const*>(v.data)[i];
}
int IntAt(BufferView const& v, size_t i)
{
    return v.type == DataType::UInt32 ? int(static_cast<uint32_t const*>(v.data)[i])
                                      : static_cast<int32_t const*>(v.data)[i];
}

mesh::Interpolation ToInternal(Interpolation i)
{
    switch (i) {
    case Interpolation::Constant: return mesh::Interpolation::Constant;
    case Interpolation::Uniform: return mesh::Interpolation::Uniform;
    case Interpolation::Varying: return mesh::Interpolation::Varying;
    case Interpolation::Vertex: return mesh::Interpolation::Vertex;
    case Interpolation::FaceVarying: return mesh::Interpolation::FaceVarying;
    case Interpolation::Instance: return mesh::Interpolation::Instance;
    }
    return mesh::Interpolation::Vertex;
}

mesh::PrimvarView View(BufferView const& v, Interpolation interpolation)
{
    mesh::PrimvarView view;
    view.interpolation = ToInternal(interpolation);
    if (v.empty()) return view;
    using Type = mesh::PrimvarView::Type;
    view.type = v.type == DataType::Float32 ? Type::Float
        : v.type == DataType::Float64 ? Type::Double
        : (v.type == DataType::Int32 || v.type == DataType::UInt32) ? Type::Int : Type::Unsupported;
    view.components = int(v.components);
    view.array = true;
    view.data = v.data;
    view.size = v.count;
    return view;
}

std::span<const int> Ints(BufferView const& v)
{
    return v.empty() ? std::span<const int>() : std::span<const int>(static_cast<int const*>(v.data), v.count);
}
std::span<const float> Floats(BufferView const& v)
{
    return v.empty() ? std::span<const float>() : std::span<const float>(static_cast<float const*>(v.data), v.count);
}

const char* SchemeName(SubdivisionDesc::Scheme scheme)
{
    switch (scheme) {
    case SubdivisionDesc::Scheme::CatmullClark: return "catmullClark";
    case SubdivisionDesc::Scheme::Loop: return "loop";
    case SubdivisionDesc::Scheme::Bilinear: return "bilinear";
    default: return "none";
    }
}

// The mesh conversion's view of a mesh description. `counts` holds the face
// sizes of a triangle list converted as a polygon mesh.
mesh::MeshSource SourceOf(MeshDesc const& desc, std::vector<int> const& counts)
{
    mesh::MeshSource source;
    source.faceVertexCounts = desc.faceVertexCounts.empty() ? std::span<const int>(counts) : Ints(desc.faceVertexCounts);
    source.faceVertexIndices = Ints(desc.faceVertexIndices);
    source.holeIndices = Ints(desc.subdivision.holeIndices);
    source.points = View(Nearest(desc.points), Interpolation::Vertex);
    source.normals = View(desc.normals, desc.normalsInterpolation);
    source.uvs = View(desc.uvs, desc.uvsInterpolation);
    source.primvars.reserve(desc.primvars.size());
    for (auto const& primvar : desc.primvars)
        source.primvars.emplace_back(primvar.name, View(primvar.data, primvar.interpolation));
    source.subdivisionScheme = SchemeName(desc.subdivision.scheme);
    source.orientation = desc.leftHanded ? "leftHanded" : "";
    source.doubleSided = desc.doubleSided;
    source.creaseIndices = Ints(desc.subdivision.creaseIndices);
    source.creaseLengths = Ints(desc.subdivision.creaseLengths);
    source.cornerIndices = Ints(desc.subdivision.cornerIndices);
    source.creaseSharpnesses = Floats(desc.subdivision.creaseSharpness);
    source.cornerSharpnesses = Floats(desc.subdivision.cornerSharpness);
    source.interpolateBoundary = desc.subdivision.interpolateBoundary;
    source.faceVaryingLinearInterpolation = desc.subdivision.faceVaryingLinearInterpolation;
    source.triangleSubdivisionRule = desc.subdivision.triangleSubdivisionRule;
    return source;
}

bool PerVertex(BufferView const& v, Interpolation i)
{
    return v.empty() || i == Interpolation::Vertex || i == Interpolation::Varying || i == Interpolation::Constant;
}

// A triangle list whose attributes are per vertex: its arrays are the mesh.
bool VerbatimTriangles(MeshDesc const& desc)
{
    if (!desc.faceVertexCounts.empty()) return false;
    if (!PerVertex(desc.normals, desc.normalsInterpolation) || !PerVertex(desc.uvs, desc.uvsInterpolation)) return false;
    for (auto const& primvar : desc.primvars)
        if (!PerVertex(primvar.data, primvar.interpolation)) return false;
    return desc.subdivision.scheme == SubdivisionDesc::Scheme::None;
}

// The element of a per-vertex (or constant) attribute at a vertex.
size_t ElementAt(BufferView const& v, Interpolation i, size_t vertex)
{
    return i == Interpolation::Constant ? 0 : vertex;
}

// A triangle list as given: positions, normals, uvs, tangents and primvars
// per vertex (glTF and OBJ meshes). Missing normals are area weighted, uvs
// zero; tangents left empty come from the uvs at setupEmbree.
std::unique_ptr<Mesh, mesh::ConvertedMeshDeleter> TriangleMesh(MeshDesc const& desc)
{
    std::unique_ptr<Mesh, mesh::ConvertedMeshDeleter> result(new Mesh());
    Mesh& m = *result;
    BufferView const& points = Nearest(desc.points);
    const size_t n = points.count;
    m.vertices.resize(n);
    for (size_t i = 0; i < n; ++i)
        m.vertices[i] = Float3(FloatAt(points, 3 * i), FloatAt(points, 3 * i + 1), FloatAt(points, 3 * i + 2));
    const size_t triangles = desc.faceVertexIndices.count / 3;
    m.indices.reserve(triangles * 3);
    for (size_t t = 0; t < triangles; ++t) {
        const int a = IntAt(desc.faceVertexIndices, 3 * t), b = IntAt(desc.faceVertexIndices, 3 * t + 1),
                  c = IntAt(desc.faceVertexIndices, 3 * t + 2);
        Check(a >= 0 && b >= 0 && c >= 0 && size_t(a) < n && size_t(b) < n && size_t(c) < n,
              "triangle " + std::to_string(t) + " has a vertex index out of range");
        m.indices.insert(m.indices.end(), {unsigned(a), unsigned(b), unsigned(c)});
    }
    if (!desc.normals.empty()) {
        Check(desc.normalsInterpolation == Interpolation::Constant || desc.normals.count >= n,
              "a triangle list needs a normal per vertex");
        m.normals.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const size_t e = ElementAt(desc.normals, desc.normalsInterpolation, i);
            m.normals[i] = Float3(FloatAt(desc.normals, 3 * e), FloatAt(desc.normals, 3 * e + 1),
                                  FloatAt(desc.normals, 3 * e + 2));
        }
    } else {
        std::vector<Float3> sum(n, Float3(0.0f));
        for (size_t t = 0; t < triangles; ++t) {
            unsigned const* tri = &m.indices[3 * t];
            const Float3 normal = cross(m.vertices[tri[1]] - m.vertices[tri[0]], m.vertices[tri[2]] - m.vertices[tri[0]]);
            for (int k = 0; k < 3; ++k) sum[tri[k]] = sum[tri[k]] + normal;
        }
        m.normals.resize(n);
        for (size_t i = 0; i < n; ++i)
            m.normals[i] = dot(sum[i], sum[i]) > 0.0f ? sum[i].normalize() : Float3(0.0f, 1.0f, 0.0f);
    }
    m.texcoords.resize(n, Float2(0.0f, 0.0f));
    if (!desc.uvs.empty()) {
        Check(desc.uvsInterpolation == Interpolation::Constant || desc.uvs.count >= n,
              "a triangle list needs a uv per vertex");
        for (size_t i = 0; i < n; ++i) {
            const size_t e = ElementAt(desc.uvs, desc.uvsInterpolation, i);
            m.texcoords[i] = Float2(FloatAt(desc.uvs, 2 * e), FloatAt(desc.uvs, 2 * e + 1));
        }
    }
    if (!desc.tangents.empty()) {
        Check(desc.tangents.count >= n, "a triangle list needs a tangent per vertex");
        auto const* t = static_cast<float const*>(desc.tangents.data);
        m.tangents.assign(t, t + 4 * n);
    }
    int stride = 0;
    for (auto const& primvar : desc.primvars) {
        Check(primvar.interpolation == Interpolation::Constant || primvar.data.count >= n,
              "primvar '" + primvar.name + "' needs a value per vertex");
        m.primvars.push_back({primvar.name, int(primvar.data.components), stride});
        stride += int(primvar.data.components);
    }
    m.primvarStride = stride;
    if (stride) {
        m.primvarValues.resize(n * size_t(stride));
        for (size_t p = 0; p < desc.primvars.size(); ++p) {
            auto const& primvar = desc.primvars[p];
            const int components = int(primvar.data.components);
            for (size_t i = 0; i < n; ++i) {
                const size_t e = ElementAt(primvar.data, primvar.interpolation, i);
                for (int c = 0; c < components; ++c) {
                    const size_t k = e * size_t(components) + size_t(c);
                    const float value = primvar.data.type == DataType::Int32 ? float(static_cast<int32_t const*>(primvar.data.data)[k])
                        : primvar.data.type == DataType::UInt32 ? float(static_cast<uint32_t const*>(primvar.data.data)[k])
                        : FloatAt(primvar.data, k);
                    m.primvarValues[i * size_t(stride) + size_t(m.primvars[p].offset) + size_t(c)] = value;
                }
            }
        }
    }
    // Single-sided triangles (glTF materials that are not doubleSided).
    if (!desc.doubleSided) {
        m.single_sided_all = true;
    } else if (!desc.singleSidedFaces.empty()) {
        auto const* flags = static_cast<uint8_t const*>(desc.singleSidedFaces.data);
        const size_t count = std::min(desc.singleSidedFaces.count, triangles);
        const bool all = count == triangles && std::all_of(flags, flags + count, [](uint8_t f) { return f != 0; });
        const bool none = std::none_of(flags, flags + count, [](uint8_t f) { return f != 0; });
        if (all && count) m.single_sided_all = true;
        else if (!none) {
            m.single_sided.assign(flags, flags + count);
            m.single_sided.resize(triangles, 0);
        }
    }
    return result;
}

// The OBJ loader's displacement, for a triangle list whose slots displace:
// each displacing slot's faces become a mesh (corners at one position
// welded, so the surface stays closed), refined bilinearly to the
// displacement level within its face limit and displaced, appended after the
// other faces. `slots` (per triangle) is updated to the new triangles.
size_t DisplaceTriangleSlots(Mesh& mesh, std::vector<int>& slots, std::vector<Material*> const& materials,
                             mesh::SubdivisionSettings const& subdivision, size_t* displacedSlots)
{
    std::map<int, std::vector<size_t>> displaced; // slot -> triangles
    const size_t triangles = mesh.indices.size() / 3;
    for (size_t t = 0; t < triangles; ++t) {
        const int slot = slots[t];
        Material* material = size_t(slot) < materials.size() ? materials[size_t(slot)] : nullptr;
        if (material && material->hasDisplacement()) displaced[slot].push_back(t);
    }
    *displacedSlots = displaced.size();
    if (displaced.empty()) return 0;
    std::vector<Float3> keptVertices, keptNormals;
    std::vector<Float2> keptTexcoords;
    std::vector<unsigned> keptIndices;
    std::vector<int> keptSlots;
    for (size_t t = 0; t < triangles; ++t) {
        const int slot = slots[t];
        if (displaced.contains(slot)) continue;
        for (int k = 0; k < 3; ++k) {
            const unsigned v = mesh.indices[3 * t + k];
            keptIndices.push_back(unsigned(keptVertices.size()));
            keptVertices.push_back(mesh.vertices[v]);
            keptNormals.push_back(mesh.normals[v]);
            keptTexcoords.push_back(mesh.texcoords[v]);
        }
        keptSlots.push_back(slot);
    }
    size_t refinedTriangles = 0;
    for (auto const& [slot, faces] : displaced) {
        std::vector<int> counts, corners;
        std::vector<float> points, uvs; // xyz per point, uv per corner
        std::map<std::array<float, 3>, int> welded;
        for (size_t t : faces) {
            counts.push_back(3);
            for (int k = 0; k < 3; ++k) {
                const unsigned v = mesh.indices[3 * t + k];
                const std::array<float, 3> key{mesh.vertices[v].x(), mesh.vertices[v].y(), mesh.vertices[v].z()};
                auto [found, added] = welded.emplace(key, int(points.size() / 3));
                if (added) points.insert(points.end(), key.begin(), key.end());
                corners.push_back(found->second);
                uvs.push_back(mesh.texcoords[v].u());
                uvs.push_back(mesh.texcoords[v].v());
            }
        }
        using PrimvarView = mesh::PrimvarView;
        mesh::MeshSource source;
        source.faceVertexCounts = counts;
        source.faceVertexIndices = corners;
        source.points = {mesh::Interpolation::Vertex, PrimvarView::Type::Float, 3, true, points.data(), points.size() / 3};
        source.uvs = {mesh::Interpolation::FaceVarying, PrimvarView::Type::Float, 2, true, uvs.data(), uvs.size() / 2};
        source.doubleSided = true;
        mesh::MeshConversion conversion = mesh::ConvertMesh(source, subdivision, materials[size_t(slot)]);
        if (!conversion.mesh) continue;
        Mesh const& refined = *conversion.mesh;
        const unsigned base = unsigned(keptVertices.size());
        keptVertices.insert(keptVertices.end(), refined.vertices.begin(), refined.vertices.end());
        keptNormals.insert(keptNormals.end(), refined.normals.begin(), refined.normals.end());
        keptTexcoords.insert(keptTexcoords.end(), refined.texcoords.begin(), refined.texcoords.end());
        for (unsigned index : refined.indices) keptIndices.push_back(base + index);
        keptSlots.insert(keptSlots.end(), refined.indices.size() / 3, slot);
        refinedTriangles += refined.indices.size() / 3;
    }
    mesh.vertices = std::move(keptVertices);
    mesh.normals = std::move(keptNormals);
    mesh.texcoords = std::move(keptTexcoords);
    mesh.indices = std::move(keptIndices);
    mesh.tangents.clear();
    // Primvars and single-sided flags do not survive (as in the OBJ loader,
    // whose meshes had neither).
    mesh.primvars.clear();
    mesh.primvarStride = 0;
    mesh.primvarValues.clear();
    mesh.single_sided.clear();
    slots = std::move(keptSlots);
    return refinedTriangles;
}

struct GeometryDeleter {
    void operator()(Geometry* geometry) const
    {
        if (!geometry) return;
        if (geometry->geom) rtcReleaseGeometry(geometry->geom);
        delete geometry->transform;
        delete geometry;
    }
};

std::array<double, 16> Flat(Matrix4d const& m)
{
    std::array<double, 16> result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) result[size_t(row * 4 + column)] = m[size_t(row)][column];
    return result;
}

SceneManager::PlacementTransform PlacementOf(Matrix4d const& world, bool flipWithMirror)
{
    SceneManager::PlacementTransform placement;
    const Matrix4d normal = world.inverse().transpose();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            placement.normal[size_t(r * 3 + c)] = float(normal[size_t(r)][c]);
            placement.tangent[size_t(r * 3 + c)] = float(world[size_t(r)][c]);
        }
    placement.mirrored = world.determinant() < 0.0;
    placement.flipOrientation = flipWithMirror && placement.mirrored;
    return placement;
}

// An Embree instance of `prototype` at `world` (row vectors; Embree's
// column-major affine convention).
unsigned AttachInstance(RTCDevice device, RTCScene target, RTCScene prototype, Matrix4d const& world)
{
    RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(instance, prototype);
    float matrix[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) matrix[c * 4 + r] = float(world[size_t(c)][r]);
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matrix);
    rtcCommitGeometry(instance);
    const unsigned id = rtcAttachGeometry(target, instance);
    rtcReleaseGeometry(instance);
    return id;
}

} // namespace

// One committed revision: the SceneManager devices render and everything it
// points to.
struct Revision {
    std::unique_ptr<SceneManager> manager;
    std::vector<std::unique_ptr<Mesh, mesh::ConvertedMeshDeleter>> meshes;
    std::vector<std::unique_ptr<Geometry, GeometryDeleter>> geometries;  // spheres
    std::vector<RTCScene> prototypes;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::unique_ptr<Texture<Color3>>> textures;
    std::vector<std::unique_ptr<Environment>> environments;
    std::vector<std::shared_ptr<const InstancePrimvars>> primvars;
    std::unordered_map<unsigned, detail::PlacementInfo> placements;

    ~Revision()
    {
        if (manager && manager->scene) rtcReleaseScene(manager->scene);
        for (RTCScene scene : prototypes) rtcReleaseScene(scene);
        for (auto& mesh : meshes)
            if (mesh && mesh->geom) {
                rtcReleaseGeometry(mesh->geom);
                mesh->geom = nullptr;
            }
    }
};

struct Scene::Impl {
    std::shared_ptr<RendererContext> context;
    SceneOptions options;
    Slots<MeshDesc> meshes;
    Slots<SpheresDesc> spheres;
    Slots<InstanceDesc> instances;
    struct MaterialEntry {
        MaterialDesc desc;
        std::shared_ptr<Material> compiled;  // null: not compiled yet
        bool prebuilt = false;
    };
    Slots<MaterialEntry> materials;
    Slots<LightDesc> lights;
    Slots<EnvironmentDesc> environments;
    Slots<CameraDesc> cameras;
    detail::SceneRenderSettings renderSettings;
    uint64_t renderSettingsGeneration = 0;
    std::mutex renderMutex;  // the scene's sessions render one at a time
    std::atomic<int> renderWaiters{0};  // RenderLock: renders and commits waiting for it
    // Replaced under the render lock; a progressive session keeps the
    // revision it renders alive (SceneAccess::committedRevision).
    std::shared_ptr<Revision> committed;
    std::atomic<uint64_t> revision{0};
    std::vector<Diagnostic> diagnostics;

    void diagnose(DiagnosticLevel level, std::string category, std::string message, bool print = true)
    {
        if (print) std::cerr << "PrismRT: " << message << '\n';
        diagnostics.push_back({level, std::move(category), std::move(message)});
    }

    std::shared_ptr<Material> compile(MaterialEntry& entry);
    CommitResult commit(ProgressCallback const& progress);
};

std::shared_ptr<Material> Scene::Impl::compile(MaterialEntry& entry)
{
    MaterialDesc const& desc = entry.desc;
    // A material with its texture (Diffuse and Emissive keep a pointer).
    struct Owned {
        std::unique_ptr<Texture<Color3>> texture;
        std::unique_ptr<Material> material;
    };
    auto own = [](std::unique_ptr<Texture<Color3>> texture, auto make) {
        auto owned = std::make_shared<Owned>();
        owned->texture = std::move(texture);
        owned->material = make(owned->texture.get());
        return std::shared_ptr<Material>(owned, owned->material.get());
    };
    const std::string label = desc.name.empty() ? std::string("(unnamed)") : desc.name;
    switch (desc.kind) {
    case MaterialDesc::Kind::Diffuse:
        return own(std::make_unique<ColorTexture>(desc.color), [&](Texture<Color3>* texture) -> std::unique_ptr<Material> {
            if (desc.displayColorPrimvar || desc.displayOpacityPrimvar ||
                prism::hydra::NormalizeFallbackOpacity(desc.opacity) < 1.0f)
                return std::make_unique<prism::hydra::FallbackMaterial>(texture, desc.opacity, desc.displayColorPrimvar,
                                                                        desc.displayOpacityPrimvar);
            return std::make_unique<Diffuse>(texture);
        });
    case MaterialDesc::Kind::Emissive:
        return own(std::make_unique<ColorTexture>(desc.color), [&](Texture<Color3>* texture) {
            return std::unique_ptr<Material>(std::make_unique<Emissive>(texture, desc.intensity));
        });
    case MaterialDesc::Kind::MaterialXFile:
    case MaterialDesc::Kind::MaterialXString:
        break;
    }
    std::string failure;
    try {
        RendererOptions const& options = context->options();
        const std::string libraries = options.materials.materialxLibraries;
        const std::string mdlPath = options.materials.materialxMdlPath.empty() ? libraries + "/mdl"
                                                                               : options.materials.materialxMdlPath;
        if (libraries.empty()) {
            failure = "no MaterialX libraries (PRISM_MATERIALX_LIBRARY_PATH)";
        } else {
            mdl::Context& mdlContext = materials::SharedMdlContext();
            if (!mdlContext.is_valid()) throw std::runtime_error(mdlContext.get_error());
            const MaterialSettings settings = context->materialSettings();
            std::string path = desc.path;
            if (desc.kind == MaterialDesc::Kind::MaterialXString)
                path = materialx::PublishMaterialXDocument(desc.document, settings.cacheDirectory / "materials", "api_")
                           .string();
            else if (!std::filesystem::exists(path))
                Throw(ErrorCode::NotFound, "MaterialX document '" + path + "' does not exist");
            if (!desc.searchPath.empty()) mdlContext.add_resource_path(desc.searchPath);
            auto material = std::make_shared<MDLMaterial>(path, mdlContext, libraries, mdlPath, settings,
                                                          desc.materialName);
            if (material->is_valid()) return material;
            failure = "MDL compilation failed (" + path + ")";
        }
    } catch (Error const& error) {
        if (error.code() == ErrorCode::NotFound) throw;
        failure = error.what();
    } catch (std::exception const& error) {
        failure = error.what();
    }
    diagnose(DiagnosticLevel::Error, "material", "material " + label + " failed to load (" +
             (failure.empty() ? std::string("unknown error") : failure) +
             "); rendering it with the magenta error material");
    return std::shared_ptr<Material>(std::shared_ptr<Material>(), ErrorMaterial::Shared());
}

CommitResult Scene::Impl::commit(ProgressCallback const& progress)
{
    const auto start = std::chrono::steady_clock::now();
    // Progress per phase (and material); a false return stops the commit.
    auto report = [&](const char* phase, size_t done, size_t total) {
        if (!progress) return;
        ProgressInfo info;
        info.phase = phase;
        info.done = done;
        info.total = total;
        info.fraction = total ? double(done) / double(total) : 0.0;
        info.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!progress(info)) Throw(ErrorCode::Cancelled, "the commit was cancelled");
    };
    diagnostics.clear();
    auto revision = std::make_unique<Revision>();
    auto colorManagement = context->colorManagement();
    RTCDevice& device = context->embreeDevice();
    revision->manager = std::make_unique<SceneManager>(device, colorManagement);
    SceneManager& manager = *revision->manager;
    CommitResult result;

    // ── Materials: compiled once per description (load time only).
    size_t materialCount = 0, materialsDone = 0;
    materials.forEach([&](uint64_t, MaterialEntry&) { ++materialCount; });
    report("materials", 0, materialCount);
    materials.forEach([&](uint64_t, MaterialEntry& entry) {
        if (!entry.compiled) entry.compiled = compile(entry);
        revision->materials.push_back(entry.compiled);
        report("materials", ++materialsDone, materialCount);
    });
    result.materials = revision->materials.size();
    // Geometry without a material: the delegate's fallback grey.
    std::shared_ptr<Material> grey;
    auto defaultMaterial = [&]() -> Material* {
        if (!grey) {
            MaterialEntry fallback;
            fallback.desc.kind = MaterialDesc::Kind::Diffuse;
            fallback.desc.color = Color3(0.5f);
            grey = compile(fallback);
            revision->materials.push_back(grey);
        }
        return grey.get();
    };
    auto resolve = [&](std::vector<MaterialId> const& ids) {
        std::vector<Material*> resolved;
        for (MaterialId id : ids) resolved.push_back(id ? materials.get(id.v, "material").compiled.get() : nullptr);
        if (resolved.empty()) resolved.push_back(nullptr);
        if (!resolved[0]) resolved[0] = defaultMaterial();
        return resolved;
    };

    mesh::SubdivisionSettings subdivision;
    subdivision.level = options.subdivisionLevel;
    subdivision.maxFaces = size_t(options.subdivisionMaxFaces);
    subdivision.budgetFaces = size_t(options.subdivisionBudgetFaces);
    subdivision.displacementLevel = options.displacementLevel;
    subdivision.displacementMaxFaces = size_t(options.displacementMaxFaces);

    // ── Mesh placements and the conversions they need ("shapes"): one per
    // mesh and displacing material(s), in the order of their first instance.
    struct Shape {
        uint64_t mesh = 0;
        MeshDesc const* desc = nullptr;
        bool verbatim = false;           // a triangle list taken as given
        Material* displacer = nullptr;   // polygon meshes: slot 0's displacement
        std::vector<Material*> slotMaterials;  // triangle lists with displacing slots
        bool flatten = false;            // baked into world space (one placement)
        Matrix4d world;
        std::vector<int> counts;         // a triangle list's faces (polygon conversion)
        std::vector<int> triangleSlots;  // per triangle (empty: from the faces)
        int level = 0;
        // Results.
        std::unique_ptr<Mesh, mesh::ConvertedMeshDeleter> converted;
        mesh::SharedFaceTable coarseFaces;
        bool instanced = false;
        RTCScene scene = nullptr;
    };
    struct Placement {
        uint64_t instance = 0;
        InstanceDesc const* desc = nullptr;
        size_t shape = 0;
        std::vector<Material*> materials;
        Matrix4d world;
    };
    std::vector<std::unique_ptr<Shape>> shapes;
    std::vector<Placement> placements;
    std::map<uint64_t, size_t> instanceCounts;
    instances.forEach([&](uint64_t, InstanceDesc& instance) {
        if (!instance.visible) return;
        if (auto const* mesh = std::get_if<MeshId>(&instance.prototype)) ++instanceCounts[mesh->v];
    });
    std::map<std::tuple<uint64_t, Material*, std::vector<Material*>>, size_t> shapeIndex;
    instances.forEach([&](uint64_t handle, InstanceDesc& instance) {
        if (!instance.visible) return;
        auto const* meshId = std::get_if<MeshId>(&instance.prototype);
        if (!meshId) return;
        MeshDesc const& desc = meshes.get(meshId->v, "mesh (of an instance)");
        Placement placement;
        placement.instance = handle;
        placement.desc = &instance;
        placement.materials = resolve(instance.materials);
        placement.world = Nearest(instance.transform);
        const bool verbatim = VerbatimTriangles(desc);
        Material* displacer = nullptr;
        std::vector<Material*> slotMaterials;
        if (verbatim) {
            bool displaces = false;
            for (Material* material : placement.materials) displaces |= material && material->hasDisplacement();
            if (displaces) slotMaterials = placement.materials;
        } else if (placement.materials[0]->hasDisplacement()) {
            displacer = placement.materials[0];
        }
        const bool single = instanceCounts[meshId->v] == 1 && !desc.instanced;
        auto key = std::make_tuple(meshId->v, displacer, slotMaterials);
        auto found = shapeIndex.find(key);
        if (found == shapeIndex.end() || single) {
            auto shape = std::make_unique<Shape>();
            shape->mesh = meshId->v;
            shape->desc = &desc;
            shape->verbatim = verbatim;
            shape->displacer = displacer;
            shape->slotMaterials = slotMaterials;
            shape->flatten = single;
            shape->world = placement.world;
            if (!verbatim && desc.faceVertexCounts.empty())
                shape->counts.assign(desc.faceVertexIndices.count / 3, 3);
            found = shapeIndex.insert_or_assign(key, shapes.size()).first;
            shapes.push_back(std::move(shape));
        }
        placement.shape = found->second;
        placements.push_back(std::move(placement));
    });

    report("geometry", 0, shapes.size());
    // Refinement levels within the scene's budget, then the conversions:
    // meshes that refine or displace by the shared conversion (as
    // prototypes), the others by the polygon conversion.
    {
        std::vector<std::vector<size_t>> faces(shapes.size());
        std::vector<bool> fixed(shapes.size(), false);
        for (size_t i = 0; i < shapes.size(); ++i) {
            Shape const& shape = *shapes[i];
            if (shape.verbatim) continue;
            const mesh::MeshSource source = SourceOf(*shape.desc, shape.counts);
            faces[i] = mesh::RefinementFaces(source.faceVertexCounts, source.subdivisionScheme,
                                             shape.displacer != nullptr, subdivision);
        }
        const std::vector<int> levels =
            mesh::BudgetSubdivisionLevels(faces, fixed, subdivision.budgetFaces, nullptr, nullptr);
        std::atomic<bool> controlHulls{false};
        std::atomic<size_t> displacedSlots{0}, displacedTriangles{0};
        std::vector<std::string> errors(shapes.size());
        tbb::parallel_for(size_t(0), shapes.size(), [&](size_t i) {
            Shape& shape = *shapes[i];
            try {
                if (shape.verbatim) {
                    shape.converted = TriangleMesh(*shape.desc);
                    shape.instanced = !shape.flatten || !(shape.world == Matrix4d());
                    if (!shape.slotMaterials.empty()) {
                        const size_t triangles = shape.converted->indices.size() / 3;
                        shape.triangleSlots.assign(triangles, 0);
                        BufferView const& slots = shape.desc->faceMaterialSlots;
                        for (size_t t = 0; t < triangles && t < slots.count; ++t) shape.triangleSlots[t] = IntAt(slots, t);
                        size_t slotsDisplaced = 0;
                        displacedTriangles += DisplaceTriangleSlots(*shape.converted, shape.triangleSlots,
                                                                    shape.slotMaterials, subdivision, &slotsDisplaced);
                        displacedSlots += slotsDisplaced;
                    }
                    if (shape.converted->indices.empty()) shape.converted.reset();
                    return;
                }
                const mesh::MeshSource source = SourceOf(*shape.desc, shape.counts);
                shape.level = levels[i];
                if (shape.desc->subdivision.level >= 0) shape.level = std::min(shape.level, shape.desc->subdivision.level);
                const std::string_view scheme = mesh::RefinementScheme(source.subdivisionScheme, shape.displacer != nullptr);
                if (shape.level > 0 || shape.displacer) {
                    mesh::MeshConversion conversion = mesh::ConvertMesh(source, subdivision, shape.displacer, shape.level);
                    shape.coarseFaces = conversion.info.coarseFaces;
                    shape.converted = std::move(conversion.mesh);
                    shape.instanced = true;
                    if (conversion.info.subdivisionControlHull) controlHulls = true;
                } else {
                    if (scheme != "none") controlHulls = true;
                    std::unique_ptr<Mesh, mesh::ConvertedMeshDeleter> converted(new Mesh());
                    std::vector<int> coarse;
                    if (mesh::BuildPolygonMesh(source, shape.flatten ? shape.world : Matrix4d(), *converted, coarse)) {
                        shape.coarseFaces = mesh::SharedFaceTable(std::move(coarse));
                        shape.converted = std::move(converted);
                    }
                    shape.instanced = !shape.flatten;
                }
            } catch (std::exception const& error) {
                errors[i] = error.what();
            }
        });
        for (auto const& error : errors)
            if (!error.empty()) Throw(ErrorCode::InvalidArgument, error);
        if (controlHulls)
            diagnose(DiagnosticLevel::Warning, "geometry", "some subdivision surfaces are rendered as control hulls "
                     "(subdivision level 0, over the maximum faces, or the refinement budget)");
        if (displacedSlots) {
            diagnose(DiagnosticLevel::Info, "geometry", "displaced the faces of " + std::to_string(displacedSlots.load()) +
                     " material(s) into " + std::to_string(displacedTriangles.load()) + " triangles", false);
            std::cout << "PrismRT: " << diagnostics.back().message << '\n';
        }
    }

    // ── Attach: mesh placements in instance order, then spheres.
    auto instancePrimvars = [&](InstanceDesc const& instance) -> std::shared_ptr<const InstancePrimvars> {
        if (instance.primvars.empty()) return nullptr;
        auto values = std::make_shared<InstancePrimvars>();
        for (auto const& primvar : instance.primvars) {
            InstancePrimvars::Value value;
            value.name = primvar.name;
            value.components = int(primvar.data.components);
            for (int c = 0; c < value.components; ++c)
                value.value[size_t(c)] = primvar.data.type == DataType::Int32
                    ? float(static_cast<int32_t const*>(primvar.data.data)[c])
                    : primvar.data.type == DataType::UInt32 ? float(static_cast<uint32_t const*>(primvar.data.data)[c])
                    : FloatAt(primvar.data, size_t(c));
            values->values.push_back(std::move(value));
        }
        return values;
    };
    std::vector<std::pair<unsigned, InstanceDesc const*>> attached;
    for (Placement const& placement : placements) {
        Shape& shape = *shapes[placement.shape];
        if (!shape.converted) continue;
        Mesh* mesh = shape.converted.get();
        unsigned id;
        if (!shape.instanced) {
            id = mesh->setupEmbree(device, manager.scene);
        } else {
            if (!shape.scene) {
                shape.scene = rtcNewScene(device);
                mesh->setupEmbree(device, shape.scene);
                rtcCommitScene(shape.scene);
                revision->prototypes.push_back(shape.scene);
            }
            id = AttachInstance(device, manager.scene, shape.scene, placement.world);
            manager.intersection_geometry.emplace(int(id), mesh->geom);
            manager.placement_transforms.emplace(int(id), PlacementOf(placement.world, true));
        }
        manager.geometry.emplace(int(id), mesh);
        auto& binding = manager.placement_materials[int(id)];
        binding.fallback = placement.materials[0];
        // Per-triangle materials of the slots other than 0.
        BufferView const& faceSlots = shape.desc->faceMaterialSlots;
        if (!faceSlots.empty() || !shape.triangleSlots.empty()) {
            const size_t triangles = mesh->indices.size() / 3;
            for (size_t t = 0; t < triangles; ++t) {
                int slot = 0;
                if (!shape.triangleSlots.empty()) {
                    slot = t < shape.triangleSlots.size() ? shape.triangleSlots[t] : 0;
                } else {
                    const size_t face = shape.coarseFaces.size() ? size_t(shape.coarseFaces[t]) : t;
                    slot = face < faceSlots.count ? IntAt(faceSlots, face) : 0;
                }
                if (slot <= 0) continue;
                Material* material = size_t(slot) < placement.materials.size() ? placement.materials[size_t(slot)] : nullptr;
                binding.triangles[int(t)] = material ? material : placement.materials[0];
            }
        }
        if (auto primvars = instancePrimvars(*placement.desc)) {
            manager.placement_primvars[int(id)] = primvars;
            revision->primvars.push_back(std::move(primvars));
        }
        detail::PlacementInfo info;
        info.instance = InstanceId{placement.instance};
        info.primId = placement.desc->primId;
        info.instanceId = placement.desc->instanceId;
        info.userData = placement.desc->userData;
        info.coarseFaces = shape.coarseFaces;
        info.triangleList = shape.verbatim;
        revision->placements[id] = std::move(info);
        attached.emplace_back(id, placement.desc);
        result.triangles += mesh->indices.size() / 3;
    }
    for (auto& shape : shapes)
        if (shape->converted) revision->meshes.push_back(std::move(shape->converted));
    result.meshes = revision->meshes.size();

    // Spheres: a prototype per spheres description, placed by Embree
    // instances (affine instances keep nonuniformly scaled spheres analytic).
    std::map<uint64_t, std::pair<Geometry*, RTCScene>> spherePrototypes;
    instances.forEach([&](uint64_t handle, InstanceDesc& instance) {
        if (!instance.visible) return;
        auto const* spheresId = std::get_if<SpheresId>(&instance.prototype);
        if (!spheresId) return;
        SpheresDesc const& desc = spheres.get(spheresId->v, "spheres (of an instance)");
        auto& prototype = spherePrototypes[spheresId->v];
        if (!prototype.first) {
            BufferView const& centers = Nearest(desc.centers);
            auto const* c = static_cast<float const*>(centers.data);
            auto const* r = static_cast<float const*>(desc.radii.data);
            std::unique_ptr<Geometry, GeometryDeleter> geometry;
            if (centers.count == 1) {
                geometry.reset(new Sphere(Float3(c[0], c[1], c[2]), r[0], nullptr));
            } else {
                auto* batch = new BatchedSpheres();
                for (size_t i = 0; i < centers.count; ++i)
                    batch->entries.push_back({Float3(c[3 * i], c[3 * i + 1], c[3 * i + 2]), r[i], nullptr});
                geometry.reset(batch);
            }
            RTCScene scene = rtcNewScene(device);
            geometry->setupEmbree(device, scene);
            rtcCommitScene(scene);
            revision->prototypes.push_back(scene);
            prototype = {geometry.get(), scene};
            revision->geometries.push_back(std::move(geometry));
        }
        const Matrix4d world = Nearest(instance.transform);
        const unsigned id = AttachInstance(device, manager.scene, prototype.second, world);
        manager.geometry.emplace(int(id), prototype.first);
        manager.intersection_geometry.emplace(int(id), prototype.first->geom);
        // Reflection changes the UV frame's handedness, not sphere winding.
        manager.placement_transforms.emplace(int(id), PlacementOf(world, false));
        manager.placement_materials[int(id)].fallback = resolve(instance.materials)[0];
        if (auto primvars = instancePrimvars(instance)) {
            manager.placement_primvars[int(id)] = primvars;
            revision->primvars.push_back(std::move(primvars));
        }
        detail::PlacementInfo info;
        info.instance = InstanceId{handle};
        info.primId = instance.primId;
        info.instanceId = instance.instanceId;
        info.userData = instance.userData;
        revision->placements[id] = std::move(info);
        attached.emplace_back(id, &instance);
    });
    result.instances = attached.size();
    manager.max_depth = renderSettings.maxDepth;
    manager.rr_min_depth = renderSettings.rouletteDepth;
    manager.environmentSampling = renderSettings.environmentSampling;
    manager.textureFiltering = renderSettings.textureFiltering;
    manager.honorDoubleSided = renderSettings.honorDoubleSided;
    manager.textureStreaming = renderSettings.textureStreaming;
    manager.gpuTextureBudgetMiB = renderSettings.gpuTextureBudgetMiB;

    report("lights", 0, 0);
    // ── Environments, in order; several add up.
    auto toWorking = [&](Color3 const& color, std::string const& space) {
        if (space.empty()) return color;
        if (space == color::kLightColorSpace) return colorManagement->FromRec709(color);
        float pixel[3] = {color.x(), color.y(), color.z()};
        colorManagement->ToWorkingSpace(space, pixel, 1, 3);
        return Color3(pixel[0], pixel[1], pixel[2]);
    };
    auto black = std::make_unique<ColorTexture>(Color3(0));
    auto background = std::make_unique<ConstantColorBackground>(black.get());
    manager.environment = background.get();
    Environment* blackBackground = background.get();
    revision->textures.push_back(std::move(black));
    revision->environments.push_back(std::move(background));
    std::vector<Environment const*> domes;
    environments.forEach([&](uint64_t, EnvironmentDesc& desc) {
        std::unique_ptr<Environment> environment;
        if (desc.kind == EnvironmentDesc::Kind::Constant) {
            auto texture = std::make_unique<ColorTexture>(toWorking(desc.color, desc.colorSpace) *
                                                          (desc.intensity * std::exp2(desc.exposure)));
            environment = std::make_unique<ConstantColorBackground>(texture.get());
            revision->textures.push_back(std::move(texture));
        } else if (desc.kind == EnvironmentDesc::Kind::LatLong) {
            if (!std::filesystem::exists(desc.texture))
                Throw(ErrorCode::NotFound, "Environment does not exist: " + desc.texture);
            auto texture = std::make_unique<ImageTexture<Color3>>(desc.texture, desc.textureColorSpace, colorManagement);
            texture->adjustExposure(desc.exposure);
            environment = std::make_unique<EnvironmentMap>(texture.get(), toWorking(desc.color, desc.colorSpace) *
                                                           desc.intensity, *colorManagement);
            revision->textures.push_back(std::move(texture));
        } else {
            std::unique_ptr<Texture<Color3>> texture;
            if (!desc.texture.empty())
                texture = std::make_unique<ImageTexture<Color3>>(desc.texture, desc.textureColorSpace, colorManagement);
            else
                texture = std::make_unique<ColorTexture>(Color3(1));
            const Color3 multiplier = toWorking(desc.color, desc.colorSpace) * (desc.intensity * std::exp2(desc.exposure));
            const Matrix4d inverse = Nearest(desc.transform).inverse();
            std::array<float, 9> linear;
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col) linear[size_t(row * 3 + col)] = float(inverse[size_t(col)][row]);
            environment = std::make_unique<TransformedEnvironmentMap>(texture.get(), multiplier, linear, *colorManagement);
            revision->textures.push_back(std::move(texture));
        }
        environment->diffuseFactor = desc.diffuse;
        environment->specularFactor = desc.specular;
        domes.push_back(environment.get());
        revision->environments.push_back(std::move(environment));
    });
    if (domes.size() == 1) {
        manager.environment = const_cast<Environment*>(domes.front());
    } else if (domes.size() > 1) {
        auto composite = std::make_unique<CompositeEnvironment>(domes, *colorManagement);
        manager.environment = composite.get();
        revision->environments.push_back(std::move(composite));
    }

    // ── Lights, and light and shadow linking: a category bit per distinct
    // category of the lights (at most 32 of each kind), in light order.
    std::vector<std::string> lightCategories, shadowCategories;
    bool warnedCategories = false;
    auto bit = [&](std::vector<std::string>& categories, std::string const& name) -> uint32_t {
        if (name.empty()) return 0u;
        for (size_t i = 0; i < categories.size(); ++i)
            if (categories[i] == name) return 1u << i;
        if (categories.size() >= 32) {
            if (!warnedCategories)
                diagnose(DiagnosticLevel::Warning, "light", "more than 32 light or shadow link collections; further ones are ignored");
            warnedCategories = true;
            return 0u;
        }
        categories.push_back(name);
        return 1u << (categories.size() - 1);
    };
    lights.forEach([&](uint64_t, LightDesc& desc) {
        LightParameters params;
        params.shape = LightShape(int(desc.shape));
        params.color = desc.color;
        params.intensity = desc.intensity;
        params.exposure = desc.exposure;
        params.normalize = desc.normalize;
        params.radius = desc.radius;
        params.width = desc.width;
        params.height = desc.height;
        params.length = desc.length;
        params.angle = desc.angle;
        params.treatAsPoint = desc.treatAsPoint;
        params.cameraVisible = desc.cameraVisible;
        params.diffuse = desc.diffuse;
        params.specular = desc.specular;
        params.shaping.active = desc.shaping.active;
        params.shaping.focus = desc.shaping.focus;
        params.shaping.focusTint = desc.shaping.focusTint;
        params.shaping.coneAngle = desc.shaping.coneAngle;
        params.shaping.coneSoftness = desc.shaping.coneSoftness;
        params.shaping.iesAngleScale = desc.shaping.iesAngleScale;
        params.shaping.iesNormalize = desc.shaping.iesNormalize;
        if (desc.shaping.active && !desc.shaping.iesFile.empty()) {
            if (std::filesystem::exists(desc.shaping.iesFile)) params.shaping.ies = LoadIesProfile(desc.shaping.iesFile);
            else WarnUnresolvedIesProfile(desc.shaping.iesFile);
        }
        SceneLight light = MakeSceneLight(params, Flat(Nearest(desc.transform)), *colorManagement);
        light.lightLinkBit = bit(lightCategories, desc.lightLinkCategory);
        light.shadowLinkBit = bit(shadowCategories, desc.shadowLinkCategory);
        if (desc.shape == LightDesc::Shape::Rect && !desc.texture.empty())
            light.texture = std::make_shared<ImageTexture<Color3>>(desc.texture, desc.textureColorSpace, colorManagement);
        manager.lights.push_back(std::move(light));
    });
    result.lights = manager.lights.size();
    manager.lightLinking = !lightCategories.empty() || !shadowCategories.empty();
    if (manager.lightLinking) {
        auto mask = [](std::vector<std::string> const& categories, std::vector<std::string> const& names) {
            uint32_t bits = 0;
            for (size_t i = 0; i < categories.size(); ++i)
                if (std::find(names.begin(), names.end(), categories[i]) != names.end()) bits |= 1u << i;
            return bits;
        };
        for (auto const& [id, instance] : attached)
            manager.placement_link_masks[int(id)] = {mask(lightCategories, instance->lightLinkCategories),
                                                     mask(shadowCategories, instance->shadowLinkCategories)};
    }

    report("acceleration", 0, 0);
    manager.commitScene();

    // A scene with no lights, emissive geometry or environment would render
    // black: usdview's default dome, by the Hydra delegate's rule
    // (scene_bridge.cpp _ApplyDefaultDome).
    if (options.defaultDome && manager.environment == blackBackground && manager.lights.empty() &&
        !device::SceneHasEmitters(manager)) {
        if (options.defaultDomeTexture.empty()) {
            diagnose(DiagnosticLevel::Warning, "environment", "the scene has no lights, emissive geometry or "
                     "environment, and USD's default dome texture was not found; the image will be black (pass --env "
                     "or --env-color)");
        } else {
            // As a USD dome (intensity 1, exposure 0, untransformed), like usdview's.
            auto texture = std::make_unique<ImageTexture<Color3>>(options.defaultDomeTexture, color::kLightColorSpace,
                                                                  colorManagement);
            auto dome = std::make_unique<TransformedEnvironmentMap>(texture.get(), Color3(1.0f),
                                                                    std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1},
                                                                    *colorManagement);
            manager.environment = dome.get();
            revision->textures.push_back(std::move(texture));
            revision->environments.push_back(std::move(dome));
            diagnose(DiagnosticLevel::Info, "environment", "the scene has no lights, emissive geometry or environment; "
                     "lighting it with USD's default dome (" + options.defaultDomeTexture + "). --env, --env-color or "
                     "--default-dome false override this");
        }
    }

    {
        // Sessions render the committed revision (and set their render
        // settings on it) under the render lock: a progressive session
        // yields at its next pass boundary and takes this revision.
        detail::RenderLock lock(renderMutex, renderWaiters);
        manager.max_depth = renderSettings.maxDepth;
        manager.rr_min_depth = renderSettings.rouletteDepth;
        manager.environmentSampling = renderSettings.environmentSampling;
        manager.textureFiltering = renderSettings.textureFiltering;
        manager.honorDoubleSided = renderSettings.honorDoubleSided;
        manager.textureStreaming = renderSettings.textureStreaming;
        manager.gpuTextureBudgetMiB = renderSettings.gpuTextureBudgetMiB;
        committed = std::move(revision);
        result.revision = ++this->revision;
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

// ── Scene

Scene::Scene(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Scene::~Scene() = default;

MeshId Scene::addMesh(MeshDesc const& desc) { return MeshId{m_impl->meshes.add(RetainMesh(desc))}; }
void Scene::updateMesh(MeshId id, MeshDesc const& desc)
{
    MeshDesc& mesh = m_impl->meshes.get(id.v, "mesh");
    mesh = RetainMesh(desc);
}
void Scene::updateMeshPoints(MeshId id, Sampled<BufferView> const& points)
{
    MeshDesc desc = m_impl->meshes.get(id.v, "mesh");
    desc.points = points;
    m_impl->meshes.get(id.v, "mesh") = RetainMesh(std::move(desc));
}
SpheresId Scene::addSpheres(SpheresDesc const& desc) { return SpheresId{m_impl->spheres.add(RetainSpheres(desc))}; }
void Scene::updateSpheres(SpheresId id, SpheresDesc const& desc)
{
    SpheresDesc& spheres = m_impl->spheres.get(id.v, "spheres");
    spheres = RetainSpheres(desc);
}

InstanceId Scene::addInstance(InstanceDesc const& desc)
{
    InstanceDesc retained = RetainInstance(desc);
    if (auto const* mesh = std::get_if<MeshId>(&retained.prototype)) m_impl->meshes.get(mesh->v, "mesh");
    else m_impl->spheres.get(std::get<SpheresId>(retained.prototype).v, "spheres");
    for (MaterialId material : retained.materials)
        if (material) m_impl->materials.get(material.v, "material");
    return InstanceId{m_impl->instances.add(std::move(retained))};
}
void Scene::updateInstance(InstanceId id, InstanceDesc const& desc)
{
    m_impl->instances.get(id.v, "instance");
    InstanceDesc retained = RetainInstance(desc);
    if (auto const* mesh = std::get_if<MeshId>(&retained.prototype)) m_impl->meshes.get(mesh->v, "mesh");
    else m_impl->spheres.get(std::get<SpheresId>(retained.prototype).v, "spheres");
    for (MaterialId material : retained.materials)
        if (material) m_impl->materials.get(material.v, "material");
    m_impl->instances.get(id.v, "instance") = std::move(retained);
}
void Scene::setTransform(InstanceId id, Sampled<Matrix4d> const& transform)
{
    Check(!transform.empty(), "an instance needs a transform");
    m_impl->instances.get(id.v, "instance").transform = transform;
}
void Scene::setVisible(InstanceId id, bool visible) { m_impl->instances.get(id.v, "instance").visible = visible; }
void Scene::setMaterials(InstanceId id, std::vector<MaterialId> const& materials)
{
    for (MaterialId material : materials)
        if (material) m_impl->materials.get(material.v, "material");
    m_impl->instances.get(id.v, "instance").materials = materials;
}

MaterialId Scene::addMaterial(MaterialDesc const& desc)
{
    Impl::MaterialEntry entry;
    entry.desc = desc;
    return MaterialId{m_impl->materials.add(std::move(entry))};
}
void Scene::updateMaterial(MaterialId id, MaterialDesc const& desc)
{
    auto& entry = m_impl->materials.get(id.v, "material");
    entry.desc = desc;
    entry.compiled.reset();
    entry.prebuilt = false;
}
LightId Scene::addLight(LightDesc const& desc)
{
    Check(!desc.transform.empty(), "a light needs a transform");
    return LightId{m_impl->lights.add(desc)};
}
void Scene::updateLight(LightId id, LightDesc const& desc)
{
    Check(!desc.transform.empty(), "a light needs a transform");
    m_impl->lights.get(id.v, "light") = desc;
}
EnvironmentId Scene::addEnvironment(EnvironmentDesc const& desc)
{
    Check(!desc.transform.empty(), "an environment needs a transform");
    Check(desc.kind != EnvironmentDesc::Kind::LatLong || !desc.texture.empty(), "a lat-long environment needs a texture");
    return EnvironmentId{m_impl->environments.add(desc)};
}
void Scene::updateEnvironment(EnvironmentId id, EnvironmentDesc const& desc)
{
    Check(!desc.transform.empty(), "an environment needs a transform");
    m_impl->environments.get(id.v, "environment") = desc;
}
CameraId Scene::addCamera(CameraDesc const& desc)
{
    Check(!desc.cameraToWorld.empty(), "a camera needs a transform");
    return CameraId{m_impl->cameras.add(desc)};
}
void Scene::updateCamera(CameraId id, CameraDesc const& desc)
{
    Check(!desc.cameraToWorld.empty(), "a camera needs a transform");
    m_impl->cameras.get(id.v, "camera") = desc;
}
CameraDesc Scene::camera(CameraId id) const { return m_impl->cameras.get(id.v, "camera"); }

void Scene::remove(MeshId id) { m_impl->meshes.remove(id.v, "mesh"); }
void Scene::remove(SpheresId id) { m_impl->spheres.remove(id.v, "spheres"); }
void Scene::remove(InstanceId id) { m_impl->instances.remove(id.v, "instance"); }
void Scene::remove(MaterialId id) { m_impl->materials.remove(id.v, "material"); }
void Scene::remove(LightId id) { m_impl->lights.remove(id.v, "light"); }
void Scene::remove(EnvironmentId id) { m_impl->environments.remove(id.v, "environment"); }
void Scene::remove(CameraId id) { m_impl->cameras.remove(id.v, "camera"); }

CommitResult Scene::commit(ProgressCallback const& progress) { return m_impl->commit(progress); }
uint64_t Scene::revision() const { return m_impl->revision; }
std::vector<Diagnostic> Scene::diagnostics() const { return m_impl->diagnostics; }
SceneOptions const& Scene::options() const { return m_impl->options; }

// ── Renderer

Renderer::Renderer() : Renderer(RendererOptions::FromEnvironment()) {}
Renderer::Renderer(RendererOptions const& options) : m_context(std::make_shared<RendererContext>(options)) {}
Renderer::Renderer(std::shared_ptr<RendererContext> context) : m_context(std::move(context))
{
    if (!m_context) Throw(ErrorCode::InvalidArgument, "a renderer needs a context");
}

Scene Renderer::createScene(SceneOptions const& options) const
{
    auto impl = std::make_shared<Scene::Impl>();
    impl->context = m_context;
    impl->options = options;
    return Scene(std::move(impl));
}

// ── Internal access

namespace detail {

RendererContext& SceneAccess::context(Scene const& scene) { return *scene.m_impl->context; }

MaterialId SceneAccess::addCompiledMaterial(Scene& scene, std::shared_ptr<Material> material, std::string name)
{
    if (!material) Throw(ErrorCode::InvalidArgument, "a compiled material is null");
    Scene::Impl::MaterialEntry entry;
    entry.desc.name = std::move(name);
    entry.compiled = std::move(material);
    entry.prebuilt = true;
    return MaterialId{scene.m_impl->materials.add(std::move(entry))};
}

void SceneAccess::setRenderSettings(Scene& scene, SceneRenderSettings const& settings)
{
    scene.m_impl->renderSettings = settings;
    ++scene.m_impl->renderSettingsGeneration;
    if (auto& revision = scene.m_impl->committed) {
        SceneManager& manager = *revision->manager;
        manager.max_depth = settings.maxDepth;
        manager.rr_min_depth = settings.rouletteDepth;
        manager.environmentSampling = settings.environmentSampling;
        manager.textureFiltering = settings.textureFiltering;
        manager.honorDoubleSided = settings.honorDoubleSided;
        manager.textureStreaming = settings.textureStreaming;
        manager.gpuTextureBudgetMiB = settings.gpuTextureBudgetMiB;
    }
}

SceneRenderSettings const& SceneAccess::renderSettings(Scene const& scene) { return scene.m_impl->renderSettings; }
uint64_t SceneAccess::renderSettingsGeneration(Scene const& scene) { return scene.m_impl->renderSettingsGeneration; }
std::mutex& SceneAccess::renderMutex(Scene const& scene) { return scene.m_impl->renderMutex; }

SceneManager& SceneAccess::committed(Scene& scene)
{
    if (!scene.m_impl->committed) Throw(ErrorCode::InvalidArgument, "the scene has not been committed");
    return *scene.m_impl->committed->manager;
}

std::atomic<int>& SceneAccess::renderWaiters(Scene const& scene) { return scene.m_impl->renderWaiters; }

CommittedRevision SceneAccess::committedRevision(Scene const& scene)
{
    auto const& revision = scene.m_impl->committed;
    if (!revision) Throw(ErrorCode::InvalidArgument, "the scene has not been committed");
    CommittedRevision result;
    result.manager = std::shared_ptr<SceneManager>(revision, revision->manager.get());
    result.placements = std::shared_ptr<const std::unordered_map<unsigned, PlacementInfo>>(revision, &revision->placements);
    result.revision = scene.m_impl->revision.load();
    return result;
}

PlacementInfo const* CommittedRevision::placement(unsigned geometryId) const
{
    if (!placements) return nullptr;
    auto found = placements->find(geometryId);
    return found == placements->end() ? nullptr : &found->second;
}

RenderLock::RenderLock(Scene const& scene)
    : RenderLock(SceneAccess::renderMutex(scene), SceneAccess::renderWaiters(scene))
{
}

RenderLock::RenderLock(std::mutex& mutex, std::atomic<int>& waiters) : m_mutex(mutex), m_waiters(waiters) { lock(); }

RenderLock::~RenderLock()
{
    if (m_locked) m_mutex.unlock();
}

void RenderLock::lock()
{
    m_waiters.fetch_add(1);
    m_mutex.lock();
    m_waiters.fetch_sub(1);
    m_locked = true;
}

void RenderLock::unlock()
{
    m_locked = false;
    m_mutex.unlock();
}

bool RenderLock::yield()
{
    const int waiting = m_waiters.load();
    if (waiting == 0) return false;
    unlock();
    // Until one of them has the lock (or a second passed: one may have
    // given up waiting), then queue again.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (m_waiters.load() >= waiting && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lock();
    return true;
}

PlacementInfo const* SceneAccess::placement(Scene const& scene, unsigned geometryId)
{
    auto const& revision = scene.m_impl->committed;
    if (!revision) return nullptr;
    auto found = revision->placements.find(geometryId);
    return found == revision->placements.end() ? nullptr : &found->second;
}

} // namespace detail
} // namespace prism
