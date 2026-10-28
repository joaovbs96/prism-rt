#include "mesh_conversion.h"

#include "subdivision.h"
#include "triangulation.h"
#include "../material.h"
#include "../matrix4d.h"
#include "../ray.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace prism::mesh {
namespace {

prism::subdivision::Scheme SchemeKind(std::string_view scheme)
{
    return scheme == "loop" ? prism::subdivision::Scheme::Loop
        : scheme == "bilinear" ? prism::subdivision::Scheme::Bilinear
                               : prism::subdivision::Scheme::CatmullClark;
}

// A point or normal as a double vector: floats widened, doubles rounded to
// float first (as GfVec3d(GfVec3f(...)) did in the Vt-based conversion).
Vec3d WidenedPoint(PrimvarView const &view, size_t i)
{
    if (view.type == PrimvarView::Type::Float) {
        float const *p = view.floats() + 3 * i;
        return Vec3d(double(p[0]), double(p[1]), double(p[2]));
    }
    double const *p = view.doubles() + 3 * i;
    return Vec3d(double(float(p[0])), double(float(p[1])), double(float(p[2])));
}

} // namespace

Interpolation InterpolationFromName(std::string_view name)
{
    if (name.empty()) return Interpolation::Unauthored;
    if (name == "constant") return Interpolation::Constant;
    if (name == "uniform") return Interpolation::Uniform;
    if (name == "varying") return Interpolation::Varying;
    if (name == "vertex") return Interpolation::Vertex;
    if (name == "faceVarying") return Interpolation::FaceVarying;
    if (name == "instance") return Interpolation::Instance;
    return Interpolation::Other;
}

std::vector<NamedPrimvar> NamedPrimvars(MeshSource const &source, size_t points, size_t faces, size_t corners)
{
    static const std::unordered_set<std::string> skipped = {
        "points", "normals", "widths", "velocities", "accelerations", "angularVelocities"};
    constexpr int kMaxComponents = 64;
    std::vector<NamedPrimvar> result;
    int total = 0;
    std::vector<std::pair<std::string_view, PrimvarView> const *> sorted;
    sorted.reserve(source.primvars.size());
    for (auto const &primvar : source.primvars) sorted.push_back(&primvar);
    std::sort(sorted.begin(), sorted.end(), [](auto const *a, auto const *b) { return a->first < b->first; });
    for (auto const *primvar : sorted) {
        const std::string text(primvar->first);
        if (skipped.count(text) || text.find(':') != std::string::npos) continue;
        PrimvarView const &data = primvar->second;
        // Constant display colors drive the fallback material, so their edits
        // stay material edits.
        if ((text == "displayColor" || text == "displayOpacity") &&
            data.interpolation == Interpolation::Constant) continue;
        NamedPrimvar extra;
        extra.name = text;
        extra.interpolation = data.interpolation;
        if (!PrimvarFloats(data, &extra.values, &extra.components)) continue;
        const size_t count = extra.values.size() / size_t(extra.components);
        size_t expected = 0;
        if (extra.interpolation == Interpolation::Constant) expected = 1;
        else if (extra.interpolation == Interpolation::Uniform) expected = faces;
        else if (extra.interpolation == Interpolation::FaceVarying) expected = corners;
        else expected = points; // vertex, varying
        if (extra.interpolation == Interpolation::Constant ? count < 1 : count != expected) continue;
        if (total + extra.components > kMaxComponents) break;
        total += extra.components;
        result.push_back(std::move(extra));
    }
    return result;
}

bool PrimvarFloats(PrimvarView const &value, std::vector<float> *out, int *components)
{
    // Arrays and single values alike, scalar by scalar.
    auto take = [&](auto const *data) {
        const size_t n = size_t(value.components);
        out->clear();
        out->reserve(value.size * n);
        for (size_t i = 0; i < value.size * n; ++i) out->push_back(float(data[i]));
        *components = value.components;
        return !out->empty();
    };
    if (value.components < 1) return false;
    switch (value.type) {
    case PrimvarView::Type::Float: return take(value.floats());
    case PrimvarView::Type::Double: return take(value.doubles());
    case PrimvarView::Type::Int: return take(value.ints());
    default: return false;
    }
}

size_t PrimvarIndex(Interpolation interpolation, size_t pointIndex, size_t cornerIndex, size_t faceIndex)
{
    if (interpolation == Interpolation::FaceVarying) return cornerIndex;
    if (interpolation == Interpolation::Uniform) return faceIndex;
    if (interpolation == Interpolation::Constant) return 0;
    return pointIndex;
}

std::string_view RefinementScheme(std::string_view authored, bool displaced)
{
    std::string_view scheme = authored.empty() ? std::string_view("none") : authored;
    if (displaced && scheme == "none") scheme = "bilinear";
    return scheme;
}

std::vector<size_t> RefinementFaces(std::span<const int> counts, std::string_view authored,
                                    bool displaced, SubdivisionSettings const &subdivision)
{
    std::vector<size_t> faces{counts.size()};
    const std::string_view scheme = RefinementScheme(authored, displaced);
    if (scheme == "none") return faces;
    const auto kind = SchemeKind(scheme);
    const int top = displaced ? subdivision.displacementLevel : subdivision.level;
    const size_t maxFaces = displaced ? subdivision.displacementMaxFaces : subdivision.maxFaces;
    for (int level = 1; level <= top; ++level) {
        const size_t refined = prism::subdivision::RefinedFaceCount(counts, kind, level);
        if (refined > maxFaces) break;
        faces.push_back(refined);
    }
    return faces;
}

std::vector<int> BudgetSubdivisionLevels(std::vector<std::vector<size_t>> const &faces,
                                         std::vector<bool> const &fixed, size_t budget,
                                         size_t *plannedFaces, size_t *requestedFaces)
{
    std::vector<int> levels(faces.size(), 0);
    size_t total = 0;
    std::priority_queue<std::pair<size_t, size_t>> largest;
    for (size_t i = 0; i < faces.size(); ++i) {
        if (faces[i].empty()) continue;
        levels[i] = int(faces[i].size()) - 1;
        total += faces[i].back();
        if (!fixed[i] && levels[i] > 0) largest.emplace(faces[i].back(), i);
    }
    if (requestedFaces) *requestedFaces = total;
    while (total > budget && !largest.empty()) {
        const size_t i = largest.top().second;
        largest.pop();
        total -= faces[i][levels[i]];
        --levels[i];
        total += faces[i][levels[i]];
        if (levels[i] > 0) largest.emplace(faces[i][levels[i]], i);
    }
    if (plannedFaces) *plannedFaces = total;
    return levels;
}

void Displace(Mesh &mesh, Material const &material)
{
    const size_t count = mesh.vertices.size();
    if (!count) return;
    // A triangle and corner per vertex, for primvar lookups at the vertex.
    std::vector<std::pair<unsigned, int>> corner(count, {0u, -1});
    for (size_t t = 0; t * 3 + 2 < mesh.indices.size(); ++t)
        for (int k = 0; k < 3; ++k)
            if (corner[mesh.indices[3 * t + k]].second < 0) corner[mesh.indices[3 * t + k]] = {unsigned(t), k};
    std::vector<Float3> offsets(count, Float3(0.0f));
    tbb::parallel_for(size_t(0), count, [&](size_t i) {
        RTCRayHit miss{};
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit hit(miss);
        hit.p = mesh.vertices[i];
        hit.n = hit.n_g = mesh.normals[i].is_zero() ? Float3(0.0f, 0.0f, 1.0f) : mesh.normals[i].normalize();
        hit.o = hit.p + hit.n;
        hit.d = -hit.n;
        hit.front_face = true;
        hit.u = mesh.texcoords.size() == count ? mesh.texcoords[i].u() : 0.0f;
        hit.v = mesh.texcoords.size() == count ? mesh.texcoords[i].v() : 0.0f;
        if (mesh.tangents.size() == 4 * count) {
            hit.tangent = Float3(mesh.tangents[4 * i], mesh.tangents[4 * i + 1], mesh.tangents[4 * i + 2]);
            hit.bitangent_sign = mesh.tangents[4 * i + 3] < 0.0f ? -1.0f : 1.0f;
        }
        if (corner[i].second >= 0) {
            hit.primvarMesh = &mesh;
            hit.primid = int(corner[i].first);
            hit.bary_u = corner[i].second == 1 ? 1.0f : 0.0f;
            hit.bary_v = corner[i].second == 2 ? 1.0f : 0.0f;
        }
        offsets[i] = material.displacement(hit);
    });
    struct PositionHash {
        size_t operator()(std::array<uint32_t, 3> const &p) const {
            return (size_t(p[0]) * 73856093u) ^ (size_t(p[1]) * 19349663u) ^ (size_t(p[2]) * 83492791u);
        }
    };
    std::unordered_map<std::array<uint32_t, 3>, unsigned, PositionHash> groupOf;
    std::vector<unsigned> group(count);
    std::vector<Float3> sum;
    std::vector<int> members;
    for (size_t i = 0; i < count; ++i) {
        const Float3 &p = mesh.vertices[i];
        const std::array<uint32_t, 3> key{std::bit_cast<uint32_t>(p.x()), std::bit_cast<uint32_t>(p.y()),
                                         std::bit_cast<uint32_t>(p.z())};
        auto [found, inserted] = groupOf.emplace(key, unsigned(sum.size()));
        if (inserted) { sum.push_back(Float3(0.0f)); members.push_back(0); }
        group[i] = found->second;
        sum[found->second] += offsets[i];
        ++members[found->second];
    }
    for (size_t i = 0; i < count; ++i) mesh.vertices[i] += sum[group[i]] / float(members[group[i]]);
    // Smooth area-weighted normals per position (the winding faces outward
    // after the conversion's orientation handling).
    std::vector<Float3> normals(sum.size(), Float3(0.0f));
    for (size_t t = 0; t * 3 + 2 < mesh.indices.size(); ++t) {
        const unsigned *tri = &mesh.indices[3 * t];
        const Float3 n = cross(mesh.vertices[tri[1]] - mesh.vertices[tri[0]],
                               mesh.vertices[tri[2]] - mesh.vertices[tri[0]]);
        for (int k = 0; k < 3; ++k) normals[group[tri[k]]] += n;
    }
    for (size_t i = 0; i < count; ++i) {
        const Float3 n = normals[group[i]];
        if (n.length() > 0.0f) {
            Float3 unit = n.normalize();
            // Keep the side of the original normal.
            if (dot(unit, mesh.normals[i]) < 0.0f) unit = -unit;
            mesh.normals[i] = unit;
        }
    }
    mesh.tangents.clear();
    mesh.ensureTangents();
}

MeshConversion ConvertMesh(MeshSource const &source, SubdivisionSettings const &subdivision,
                           Material const *displacer, int levelCap)
{
    MeshConversion result;
    MeshConversionInfo &info = result.info;
    info.motionBlur = source.motionBlur;

    PrimvarView points = source.points;
    if (!points.floatArray(3) && !points.doubleArray(3)) {
        info.missingPoints = points.empty();
        info.unsupportedPoints = !points.empty();
        return result;
    }
    size_t pointCount = points.size;
    auto pointAt = [&](size_t i) -> Vec3d { return WidenedPoint(points, i); };
    std::span<const int> const coarseCounts = source.faceVertexCounts;
    std::span<const int> const coarseIndices = source.faceVertexIndices;
    std::span<const int> const coarseHoles = source.holeIndices;
    if (pointCount == 0) {
        info.missingPoints = true;
        return result;
    }
    info.topologyRead = true;
    info.faceCount = coarseCounts.size();

    PrimvarView normalPv = source.normals;
    PrimvarView uvPv = source.uvs;
    info.multipleUvSets = source.multipleUvSets;
    std::vector<NamedPrimvar> extras = NamedPrimvars(source, pointCount, coarseCounts.size(),
                                                     coarseIndices.size());
    int extraStride = 0;
    int pointDataStride = 0;
    std::vector<int> extraOffsets, extraPointOffsets;
    for (auto const &extra : extras) {
        extraOffsets.push_back(extraStride);
        extraStride += extra.components;
        const bool perPoint = extra.interpolation == Interpolation::Vertex ||
            extra.interpolation == Interpolation::Varying;
        extraPointOffsets.push_back(perPoint ? pointDataStride : -1);
        if (perPoint) pointDataStride += extra.components;
    }

    // Subdivision surfaces become their limit surface at a uniform level;
    // their normals come from the limit (authored ones do not apply).
    // UsdImaging always provides the scheme (USD's fallback is catmullClark);
    // scene indices that omit it describe polygon meshes.
    prism::subdivision::Output refined;
    // Displaced surfaces are diced finer; polygon meshes then refine bilinearly.
    const bool displaced = displacer && displacer->hasDisplacement();
    const std::string_view scheme = RefinementScheme(source.subdivisionScheme, displaced);
    if (scheme != "none") {
        prism::subdivision::Input input;
        input.scheme = SchemeKind(scheme);
        input.level = displaced ? subdivision.displacementLevel : subdivision.level;
        const size_t maxFaces = displaced ? subdivision.displacementMaxFaces : subdivision.maxFaces;
        std::span<const int> countSpan = coarseCounts;
        while (input.level > 0 &&
               prism::subdivision::RefinedFaceCount(countSpan, input.scheme, input.level) > maxFaces)
            --input.level;
        if (levelCap >= 0) input.level = std::min(input.level, levelCap);
        info.subdivisionLevel = input.level;
        if (input.level > 0) {
            std::vector<float> points(pointCount * 3);
            for (size_t i = 0; i < pointCount; ++i) {
                const Vec3d p = pointAt(i);
                points[3 * i] = float(p[0]); points[3 * i + 1] = float(p[1]); points[3 * i + 2] = float(p[2]);
            }
            input.points = points;
            input.faceVertexCounts = countSpan;
            input.faceVertexIndices = coarseIndices;
            input.holes = coarseHoles;
            {
                std::string_view const value = source.interpolateBoundary;
                if (value == "none") input.boundary = prism::subdivision::BoundaryInterpolation::None;
                else if (value == "edgeOnly")
                    input.boundary = prism::subdivision::BoundaryInterpolation::EdgeOnly;
            }
            {
                using F = prism::subdivision::FaceVaryingInterpolation;
                std::string_view const value = source.faceVaryingLinearInterpolation;
                if (value == "none") input.faceVarying = F::None;
                else if (value == "cornersOnly") input.faceVarying = F::CornersOnly;
                else if (value == "cornersPlus2") input.faceVarying = F::CornersPlus2;
                else if (value == "boundaries") input.faceVarying = F::Boundaries;
                else if (value == "all") input.faceVarying = F::All;
            }
            if (!source.triangleSubdivisionRule.empty())
                input.smoothTriangles = source.triangleSubdivisionRule == "smooth";
            input.creaseIndices = source.creaseIndices;
            input.creaseLengths = source.creaseLengths;
            input.creaseSharpnesses = source.creaseSharpnesses;
            input.cornerIndices = source.cornerIndices;
            input.cornerSharpnesses = source.cornerSharpnesses;
            std::vector<float> uvs;
            const bool faceVaryingUvs = uvPv.interpolation == Interpolation::FaceVarying;
            const bool vertexUvs = uvPv.interpolation == Interpolation::Vertex ||
                uvPv.interpolation == Interpolation::Varying;
            auto appendUvs = [&](auto const *values, size_t expected) {
                if (uvPv.size != expected) return;
                uvs.reserve(expected * 2);
                for (size_t i = 0; i < expected; ++i) {
                    uvs.push_back(float(values[2 * i]));
                    uvs.push_back(float(values[2 * i + 1]));
                }
            };
            if (faceVaryingUvs || vertexUvs) {
                const size_t expected = faceVaryingUvs ? coarseIndices.size() : pointCount;
                if (uvPv.floatArray(2)) appendUvs(uvPv.floats(), expected);
                else if (uvPv.doubleArray(2)) appendUvs(uvPv.doubles(), expected);
            }
            input.uvs = uvs;
            input.uvPerPoint = vertexUvs;
            std::vector<float> pointData;
            if (pointDataStride) {
                pointData.resize(pointCount * size_t(pointDataStride));
                for (size_t e = 0; e < extras.size(); ++e) {
                    if (extraPointOffsets[e] < 0) continue;
                    const int n = extras[e].components;
                    for (size_t p = 0; p < pointCount; ++p)
                        for (int c = 0; c < n; ++c)
                            pointData[p * pointDataStride + extraPointOffsets[e] + c] = extras[e].values[p * n + c];
                }
                input.pointData = pointData;
                input.pointDataStride = pointDataStride;
            }
            refined = prism::subdivision::Refine(input);
        }
        if (refined.valid) {
            points.type = PrimvarView::Type::Float;
            points.data = refined.points.data();
            points.size = refined.points.size() / 3;
            pointCount = points.size;
            normalPv = PrimvarView();
            if (!refined.uvs.empty()) {
                uvPv.type = PrimvarView::Type::Float;
                uvPv.components = 2;
                uvPv.array = true;
                uvPv.data = refined.uvs.data();
                uvPv.size = refined.uvs.size() / 2;
                uvPv.interpolation = Interpolation::FaceVarying;
            } else if (uvPv.interpolation != Interpolation::Constant) {
                uvPv = PrimvarView();
            }
        } else {
            info.subdivisionControlHull = true;
        }
    }
    std::span<const int> const counts =
        refined.valid ? std::span<const int>(refined.faceVertexCounts) : coarseCounts;
    std::span<const int> const indices =
        refined.valid ? std::span<const int>(refined.faceVertexIndices) : coarseIndices;
    std::span<const int> const holes = refined.valid ? std::span<const int>() : coarseHoles;
    // The authored face of a (refined) face.
    auto coarseFace = [&](size_t face) {
        return refined.valid ? size_t(refined.coarseFaces[face]) : face;
    };
    // Face-varying primvars of refined meshes: the mean of each authored face.
    std::vector<std::vector<float>> faceMeans(extras.size());
    if (refined.valid) {
        for (size_t e = 0; e < extras.size(); ++e) {
            if (extras[e].interpolation != Interpolation::FaceVarying) continue;
            const int n = extras[e].components;
            faceMeans[e].assign(coarseCounts.size() * size_t(n), 0.0f);
            size_t corner = 0;
            for (size_t f = 0; f < coarseCounts.size(); ++f) {
                const int count = std::max(0, coarseCounts[f]);
                for (int k = 0; k < count && corner + k < coarseIndices.size(); ++k)
                    for (int c = 0; c < n; ++c)
                        faceMeans[e][f * n + c] += extras[e].values[(corner + k) * n + c] / float(std::max(1, count));
                corner += size_t(count);
            }
        }
    }
    // Values of every extra primvar at one corner of a (refined) face.
    auto appendExtras = [&](std::vector<float> &out, size_t point, size_t cornerIndex, size_t face) {
        for (size_t e = 0; e < extras.size(); ++e) {
            auto const &extra = extras[e];
            const int n = extra.components;
            const float *values = nullptr;
            if (extraPointOffsets[e] >= 0) {
                values = refined.valid
                    ? refined.pointData.data() + point * pointDataStride + extraPointOffsets[e]
                    : extra.values.data() + point * n;
            } else if (extra.interpolation == Interpolation::FaceVarying) {
                values = refined.valid ? faceMeans[e].data() + coarseFace(face) * n
                                       : extra.values.data() + cornerIndex * n;
            } else if (extra.interpolation == Interpolation::Uniform) {
                values = extra.values.data() + coarseFace(face) * n;
            } else {
                values = extra.values.data();
            }
            out.insert(out.end(), values, values + n);
        }
    };
    const bool normalsAuthored = normalPv.floatArray(3) || normalPv.doubleArray(3);
    const size_t authoredNormalCount = normalsAuthored ? normalPv.size : 0;
    auto authoredNormalAt = [&](size_t i) -> Vec3d { return WidenedPoint(normalPv, i); };
    float const *authoredUvsFloat = uvPv.floatArray(2) ? uvPv.floats() : nullptr;
    double const *authoredUvsDouble = uvPv.doubleArray(2) ? uvPv.doubles() : nullptr;
    const size_t authoredUvCount = (authoredUvsFloat || authoredUvsDouble) ? uvPv.size : 0;

    const bool flip = source.orientation == "leftHanded";
    const Matrix4d geometryTransform(1.0);
    const bool mirrored = geometryTransform.determinant() < 0.0;
    const Matrix4d normalMatrix = geometryTransform.inverse().transpose();
    std::unordered_set<int> holeSet(holes.begin(), holes.end());

    // Match the standalone USD path when normals are missing, zero, or only
    // partially authored: accumulate one area-weighted coarse-face normal at
    // each referenced point. Keep this in prototype space; placement applies
    // the inverse transpose later.
    std::vector<Vec3d> generatedNormals(pointCount, Vec3d(0.0));
    size_t generatedCorner = 0;
    size_t triangleEstimate = 0;
    for (size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count >= 3) triangleEstimate += count - 2;
        if (count < 3 || generatedCorner + count > indices.size()) {
            generatedCorner += std::max(0, count);
            continue;
        }
        if (holeSet.count(static_cast<int>(face))) {
            generatedCorner += count;
            continue;
        }
        bool valid = true;
        for (int k = 0; k < count; ++k) {
            const int pi = indices[generatedCorner + k];
            valid &= pi >= 0 && static_cast<size_t>(pi) < pointCount;
        }
        if (valid) {
            const int p0 = indices[generatedCorner];
            const int p1 = indices[generatedCorner + 1];
            const int p2 = indices[generatedCorner + 2];
            Vec3d contribution = cross(
                pointAt(p1) - pointAt(p0), pointAt(p2) - pointAt(p0));
            if (flip) contribution = -contribution;
            for (int k = 0; k < count; ++k)
                generatedNormals[indices[generatedCorner + k]] += contribution;
        }
        generatedCorner += count;
    }
    if (refined.valid) {
        // Limit normals follow the winding of the authored faces.
        for (size_t i = 0; i < pointCount && 3 * i + 2 < refined.normals.size(); ++i) {
            Vec3d n(refined.normals[3 * i], refined.normals[3 * i + 1], refined.normals[3 * i + 2]);
            if (n.lengthSq() > 1e-20) generatedNormals[i] = flip ? -n : n;
        }
    }

    std::unique_ptr<Mesh, ConvertedMeshDeleter> mesh(new Mesh());
    mesh->primvarStride = extraStride;
    // USD doubleSided (default false), for prism:honorDoubleSided.
    mesh->single_sided_all = source.doubleSided ? !*source.doubleSided : true;
    for (size_t e = 0; e < extras.size(); ++e)
        mesh->primvars.push_back({extras[e].name, extras[e].components, extraOffsets[e]});
    mesh->vertices.reserve(3 * triangleEstimate);
    mesh->normals.reserve(3 * triangleEstimate);
    mesh->texcoords.reserve(3 * triangleEstimate);
    mesh->indices.reserve(3 * triangleEstimate);
    std::vector<int> coarseFaces;
    coarseFaces.reserve(triangleEstimate);
    // Reused across faces so ordinary faces allocate nothing.
    std::vector<Vec3d> facePoints;
    std::vector<std::array<int, 3>> triangles;
    size_t corner = 0;
    for (size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count < 3 || corner + count > indices.size()) {
            ++info.malformedFaceCount;
            corner += std::max(0, count);
            continue;
        }
        if (holeSet.count(static_cast<int>(face))) { corner += count; continue; }
        facePoints.clear();
        bool validFace = true;
        for (int k = 0; k < count; ++k) {
            int const pi = indices[corner + k];
            if (pi < 0 || static_cast<size_t>(pi) >= pointCount) {
                validFace = false;
                break;
            }
            facePoints.emplace_back(pointAt(pi));
        }
        if (!validFace) {
            ++info.malformedFaceCount;
            corner += count;
            continue;
        }
        triangles.resize(count - 2);
        size_t const triangleCount = TriangulatePolygon(
            facePoints.data(), facePoints.size(), triangles.data());
        if (triangleCount == 0) {
            ++info.degenerateFaceCount;
            corner += count;
            continue;
        }
        for (size_t t = 0; t < triangleCount; ++t) {
            std::array<int, 3> const &triangle = triangles[t];
            int local[3] = {triangle[0], triangle[1], triangle[2]};
            if (flip) std::swap(local[1], local[2]);
            Vec3d wp[3];
            size_t first = mesh->vertices.size();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                wp[v] = geometryTransform.transform(pointAt(pi));
                mesh->vertices.emplace_back(static_cast<float>(wp[v][0]),
                                            static_cast<float>(wp[v][1]),
                                            static_cast<float>(wp[v][2]));
                const size_t ui = PrimvarIndex(uvPv.interpolation, pi, ci, coarseFace(face));
                if (ui < authoredUvCount) {
                    if (authoredUvsFloat) {
                        mesh->texcoords.emplace_back(authoredUvsFloat[2 * ui],
                                                     authoredUvsFloat[2 * ui + 1]);
                    } else {
                        mesh->texcoords.emplace_back(
                            static_cast<float>(authoredUvsDouble[2 * ui]),
                            static_cast<float>(authoredUvsDouble[2 * ui + 1]));
                    }
                }
                else mesh->texcoords.emplace_back(0.0f, 0.0f);
                if (extraStride) appendExtras(mesh->primvarValues, size_t(pi), ci, face);
            }
            const Vec3d geometric = cross(wp[1] - wp[0], wp[2] - wp[0]).normalized();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                const size_t ni = PrimvarIndex(normalPv.interpolation, pi, ci, coarseFace(face));
                Vec3d n = geometric;
                if (ni < authoredNormalCount) {
                    Vec3d authored = authoredNormalAt(ni);
                    if (authored.lengthSq() > 1e-20 &&
                        std::isfinite(authored.lengthSq())) {
                        n = normalMatrix.transformDir(authored).normalized();
                        if (flip != mirrored) n = -n;
                    } else if (generatedNormals[pi].lengthSq() > 1e-20 &&
                               std::isfinite(generatedNormals[pi].lengthSq())) {
                        n = generatedNormals[pi].normalized();
                    }
                } else if (generatedNormals[pi].lengthSq() > 1e-20 &&
                           std::isfinite(generatedNormals[pi].lengthSq())) {
                    n = generatedNormals[pi].normalized();
                }
                mesh->normals.emplace_back(static_cast<float>(n[0]),
                                           static_cast<float>(n[1]),
                                           static_cast<float>(n[2]));
            }
            mesh->indices.push_back(static_cast<unsigned int>(first));
            mesh->indices.push_back(static_cast<unsigned int>(first + 1));
            mesh->indices.push_back(static_cast<unsigned int>(first + 2));
            coarseFaces.push_back(static_cast<int>(coarseFace(face)));
        }
        corner += count;
    }
    if (mesh->indices.empty()) return result;

    mesh->ensureTangents();
    // Merge only identical complete attribute tuples after tangent generation.
    // Keeping tangent sign in the key preserves mirrored UV seams and avoids
    // changing interpolated frames when positions/normals happen to match.
    struct VertexHash {
        size_t operator()(std::array<uint32_t,14> const& value) const {
            size_t h = 0;
            for (auto bits : value) h ^= std::hash<uint32_t>{}(bits) + size_t{0x9e3779b9} + (h<<6) + (h>>2);
            return h;
        }
    };
    std::unordered_map<std::array<uint32_t,14>, unsigned, VertexHash> uniqueVertices;
    // Named primvars take part in the merge through a 64-bit digest, checked
    // against the values on a match.
    const size_t extraCount = size_t(mesh->primvarStride);
    std::vector<float> compactExtras;
    compactExtras.reserve(extraCount * std::min(mesh->vertices.size(), pointCount));
    uniqueVertices.reserve(mesh->vertices.size());
    // Shared corners usually make the compact arrays far smaller than the
    // expanded ones; the authored point count is the closest cheap guess.
    const size_t compactEstimate = std::min(mesh->vertices.size(), pointCount);
    std::vector<Float3> compactPositions, compactNormals;
    std::vector<Float2> compactUvs;
    std::vector<float> compactTangents;
    compactPositions.reserve(compactEstimate);
    compactNormals.reserve(compactEstimate);
    compactUvs.reserve(compactEstimate);
    compactTangents.reserve(4 * compactEstimate);
    std::vector<unsigned> remap(mesh->vertices.size());
    for (size_t i=0; i<mesh->vertices.size(); ++i) {
        const auto& p=mesh->vertices[i]; const auto& n=mesh->normals[i];
        const auto& uv=mesh->texcoords[i];
        const float values[12]={p.x(),p.y(),p.z(),n.x(),n.y(),n.z(),uv.u(),uv.v(),
            mesh->tangents[4*i],mesh->tangents[4*i+1],mesh->tangents[4*i+2],mesh->tangents[4*i+3]};
        std::array<uint32_t,14> key{};
        for (size_t c=0;c<12;++c) key[c]=std::bit_cast<uint32_t>(values[c]);
        float const* extra = extraCount ? mesh->primvarValues.data() + i * extraCount : nullptr;
        if (extra) {
            uint64_t digest = 1469598103934665603ull;
            for (size_t c=0;c<extraCount;++c) digest=(digest^std::bit_cast<uint32_t>(extra[c]))*1099511628211ull;
            key[12]=uint32_t(digest); key[13]=uint32_t(digest>>32);
        }
        auto [found,inserted]=uniqueVertices.emplace(key,static_cast<unsigned>(compactPositions.size()));
        bool fresh=inserted;
        if (!inserted && extra &&
            !std::equal(extra, extra + extraCount, compactExtras.data() + size_t(found->second) * extraCount))
            fresh=true; // digest collision: keep the vertex apart
        remap[i]=fresh ? static_cast<unsigned>(compactPositions.size()) : found->second;
        if (fresh) {
            compactPositions.push_back(p); compactNormals.push_back(n); compactUvs.push_back(uv);
            compactTangents.insert(compactTangents.end(),values+8,values+12);
            if (extra) compactExtras.insert(compactExtras.end(), extra, extra + extraCount);
        }
    }
    for (auto& index : mesh->indices) index=remap[index];
    mesh->vertices=std::move(compactPositions); mesh->normals=std::move(compactNormals);
    mesh->texcoords=std::move(compactUvs); mesh->tangents=std::move(compactTangents);
    mesh->primvarValues=std::move(compactExtras);
    if (displaced) Displace(*mesh, *displacer);
    auto hashCombine = [](size_t &seed, auto const &value) {
        const size_t valueHash = std::hash<std::decay_t<decltype(value)>>{}(value);
        seed ^= valueHash + size_t{0x9e3779b9} + (seed << 6) + (seed >> 2);
    };
    size_t geometryHash = 0;
    hashCombine(geometryHash, mesh->vertices.size());
    hashCombine(geometryHash, mesh->normals.size());
    hashCombine(geometryHash, mesh->texcoords.size());
    hashCombine(geometryHash, mesh->tangents.size());
    hashCombine(geometryHash, mesh->indices.size());
    for (Float3 const &v : mesh->vertices) {
        hashCombine(geometryHash, v.x()); hashCombine(geometryHash, v.y());
        hashCombine(geometryHash, v.z());
    }
    for (Float3 const &n : mesh->normals) {
        hashCombine(geometryHash, n.x()); hashCombine(geometryHash, n.y());
        hashCombine(geometryHash, n.z());
    }
    for (Float2 const &uv : mesh->texcoords) {
        hashCombine(geometryHash, uv.u()); hashCombine(geometryHash, uv.v());
    }
    for (float const value : mesh->tangents) hashCombine(geometryHash, value);
    for (auto const &primvar : mesh->primvars) hashCombine(geometryHash, primvar.name);
    hashCombine(geometryHash, mesh->single_sided_all);
    for (float const value : mesh->primvarValues) hashCombine(geometryHash, value);
    for (unsigned int const index : mesh->indices) hashCombine(geometryHash, index);
    info.geometryHash = geometryHash;
    info.coarseFaces = SharedFaceTable(std::move(coarseFaces));
    result.mesh = std::move(mesh);
    return result;
}

} // namespace prism::mesh
