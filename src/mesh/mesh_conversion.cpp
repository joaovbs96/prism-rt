#include "mesh_conversion.h"

#include "subdivision.h"
#include "../hydra/triangulation.h"
#include "../material.h"
#include "../ray.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/primvarSchema.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace prism::mesh {
namespace {



prism::subdivision::Scheme SchemeKind(TfToken const &scheme)
{
    return scheme == TfToken("loop") ? prism::subdivision::Scheme::Loop
        : scheme == TfToken("bilinear") ? prism::subdivision::Scheme::Bilinear
                                        : prism::subdivision::Scheme::CatmullClark;
}

} // namespace

std::vector<NamedPrimvar> NamedPrimvars(MeshSource const &source, size_t points, size_t faces, size_t corners)
{
    static const std::unordered_set<std::string> skipped = {
        "points", "normals", "widths", "velocities", "accelerations", "angularVelocities"};
    constexpr int kMaxComponents = 64;
    std::vector<NamedPrimvar> result;
    int total = 0;
    std::vector<std::pair<TfToken, PrimvarData> const *> sorted;
    sorted.reserve(source.primvars.size());
    for (auto const &primvar : source.primvars) sorted.push_back(&primvar);
    std::sort(sorted.begin(), sorted.end(), [](auto const *a, auto const *b) { return a->first < b->first; });
    for (auto const *primvar : sorted) {
        const std::string text = primvar->first.GetString();
        if (skipped.count(text) || text.find(':') != std::string::npos) continue;
        PrimvarData const &data = primvar->second;
        // Constant display colors drive the fallback material, so their edits
        // stay material edits.
        if ((text == "displayColor" || text == "displayOpacity") &&
            data.interpolation == HdPrimvarSchemaTokens->constant) continue;
        NamedPrimvar extra;
        extra.name = text;
        extra.interpolation = data.interpolation;
        if (!PrimvarFloats(data.value, &extra.values, &extra.components)) continue;
        const size_t count = extra.values.size() / size_t(extra.components);
        size_t expected = 0;
        if (extra.interpolation == HdPrimvarSchemaTokens->constant) expected = 1;
        else if (extra.interpolation == HdPrimvarSchemaTokens->uniform) expected = faces;
        else if (extra.interpolation == HdPrimvarSchemaTokens->faceVarying) expected = corners;
        else expected = points; // vertex, varying
        if (extra.interpolation == HdPrimvarSchemaTokens->constant ? count < 1 : count != expected) continue;
        if (total + extra.components > kMaxComponents) break;
        total += extra.components;
        result.push_back(std::move(extra));
    }
    return result;
}

bool PrimvarFloats(VtValue const &value, std::vector<float> *out, int *components)
{
    auto take = [&](auto const &array, int n) {
        out->clear();
        out->reserve(array.size() * size_t(n));
        for (auto const &element : array) {
            if constexpr (std::is_arithmetic_v<std::decay_t<decltype(element)>>) {
                out->push_back(float(element));
            } else {
                for (int c = 0; c < n; ++c) out->push_back(float(element[c]));
            }
        }
        *components = n;
        return !out->empty();
    };
    if (value.IsHolding<VtFloatArray>()) return take(value.UncheckedGet<VtFloatArray>(), 1);
    if (value.IsHolding<VtDoubleArray>()) return take(value.UncheckedGet<VtDoubleArray>(), 1);
    if (value.IsHolding<VtIntArray>()) return take(value.UncheckedGet<VtIntArray>(), 1);
    if (value.IsHolding<VtVec2fArray>()) return take(value.UncheckedGet<VtVec2fArray>(), 2);
    if (value.IsHolding<VtVec2dArray>()) return take(value.UncheckedGet<VtVec2dArray>(), 2);
    if (value.IsHolding<VtVec3fArray>()) return take(value.UncheckedGet<VtVec3fArray>(), 3);
    if (value.IsHolding<VtVec3dArray>()) return take(value.UncheckedGet<VtVec3dArray>(), 3);
    if (value.IsHolding<VtVec4fArray>()) return take(value.UncheckedGet<VtVec4fArray>(), 4);
    if (value.IsHolding<VtVec4dArray>()) return take(value.UncheckedGet<VtVec4dArray>(), 4);
    if (value.IsHolding<float>()) return take(std::array<float, 1>{value.UncheckedGet<float>()}, 1);
    if (value.IsHolding<double>()) return take(std::array<double, 1>{value.UncheckedGet<double>()}, 1);
    if (value.IsHolding<int>()) return take(std::array<int, 1>{value.UncheckedGet<int>()}, 1);
    if (value.IsHolding<GfVec2f>()) return take(std::array<GfVec2f, 1>{value.UncheckedGet<GfVec2f>()}, 2);
    if (value.IsHolding<GfVec3f>()) return take(std::array<GfVec3f, 1>{value.UncheckedGet<GfVec3f>()}, 3);
    if (value.IsHolding<GfVec4f>()) return take(std::array<GfVec4f, 1>{value.UncheckedGet<GfVec4f>()}, 4);
    return false;
}

size_t PrimvarIndex(TfToken const &interpolation, size_t pointIndex, size_t cornerIndex, size_t faceIndex)
{
    if (interpolation == HdPrimvarSchemaTokens->faceVarying) return cornerIndex;
    if (interpolation == HdPrimvarSchemaTokens->uniform) return faceIndex;
    if (interpolation == HdPrimvarSchemaTokens->constant) return 0;
    return pointIndex;
}

TfToken RefinementScheme(TfToken const &authored, bool displaced)
{
    TfToken scheme = authored.IsEmpty() ? TfToken("none") : authored;
    if (displaced && scheme == TfToken("none")) scheme = TfToken("bilinear");
    return scheme;
}

std::vector<size_t> RefinementFaces(std::span<const int> counts, TfToken const &authored,
                                    bool displaced, SubdivisionSettings const &subdivision)
{
    std::vector<size_t> faces{counts.size()};
    const TfToken scheme = RefinementScheme(authored, displaced);
    if (scheme == TfToken("none")) return faces;
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
    using prism::hydra::TriangulatePolygon;
    MeshConversion result;
    MeshConversionInfo &info = result.info;
    info.motionBlur = source.motionBlur;

    PrimvarData const &pointsPv = source.points;
    VtVec3fArray const *pointsFloat = nullptr;
    VtVec3dArray const *pointsDouble = nullptr;
    if (pointsPv.value.IsHolding<VtVec3fArray>()) {
        pointsFloat = &pointsPv.value.UncheckedGet<VtVec3fArray>();
    } else if (pointsPv.value.IsHolding<VtVec3dArray>()) {
        pointsDouble = &pointsPv.value.UncheckedGet<VtVec3dArray>();
    } else {
        info.missingPoints = pointsPv.value.IsEmpty();
        info.unsupportedPoints = !pointsPv.value.IsEmpty();
        return result;
    }
    size_t pointCount = pointsFloat ? pointsFloat->size() : pointsDouble->size();
    auto pointAt = [&](size_t i) -> GfVec3d {
        return pointsFloat ? GfVec3d((*pointsFloat)[i])
                           : GfVec3d(GfVec3f((*pointsDouble)[i]));
    };
    // Const views: indexing a non-const VtArray detaches (copies) the buffer
    // it still shares with the captured data source.
    VtIntArray const &coarseCounts = source.faceVertexCounts;
    VtIntArray const &coarseIndices = source.faceVertexIndices;
    VtIntArray const &coarseHoles = source.holeIndices;
    if (pointCount == 0) {
        info.missingPoints = true;
        return result;
    }
    info.topologyRead = true;
    info.faceCount = coarseCounts.size();

    PrimvarData normalPv = source.normals;
    PrimvarData uvPv = source.uvs;
    info.multipleUvSets = source.multipleUvSets;
    std::vector<NamedPrimvar> extras = NamedPrimvars(source, pointCount, coarseCounts.size(),
                                                     coarseIndices.size());
    int extraStride = 0;
    int pointDataStride = 0;
    std::vector<int> extraOffsets, extraPointOffsets;
    for (auto const &extra : extras) {
        extraOffsets.push_back(extraStride);
        extraStride += extra.components;
        const bool perPoint = extra.interpolation == HdPrimvarSchemaTokens->vertex ||
            extra.interpolation == HdPrimvarSchemaTokens->varying;
        extraPointOffsets.push_back(perPoint ? pointDataStride : -1);
        if (perPoint) pointDataStride += extra.components;
    }

    // Subdivision surfaces become their limit surface at a uniform level;
    // their normals come from the limit (authored ones do not apply).
    // UsdImaging always provides the scheme (USD's fallback is catmullClark);
    // scene indices that omit it describe polygon meshes.
    prism::subdivision::Output refined;
    VtIntArray refinedCounts, refinedIndices;
    VtVec3fArray refinedPoints;
    // Displaced surfaces are diced finer; polygon meshes then refine bilinearly.
    const bool displaced = displacer && displacer->hasDisplacement();
    const TfToken scheme = RefinementScheme(source.subdivisionScheme, displaced);
    if (scheme != TfToken("none")) {
        prism::subdivision::Input input;
        input.scheme = SchemeKind(scheme);
        input.level = displaced ? subdivision.displacementLevel : subdivision.level;
        const size_t maxFaces = displaced ? subdivision.displacementMaxFaces : subdivision.maxFaces;
        std::span<const int> countSpan(coarseCounts.cdata(), coarseCounts.size());
        while (input.level > 0 &&
               prism::subdivision::RefinedFaceCount(countSpan, input.scheme, input.level) > maxFaces)
            --input.level;
        if (levelCap >= 0) input.level = std::min(input.level, levelCap);
        info.subdivisionLevel = input.level;
        if (input.level > 0) {
            std::vector<float> points(pointCount * 3);
            for (size_t i = 0; i < pointCount; ++i) {
                const GfVec3d p = pointAt(i);
                points[3 * i] = float(p[0]); points[3 * i + 1] = float(p[1]); points[3 * i + 2] = float(p[2]);
            }
            input.points = points;
            input.faceVertexCounts = countSpan;
            input.faceVertexIndices = std::span<const int>(coarseIndices.cdata(), coarseIndices.size());
            input.holes = std::span<const int>(coarseHoles.cdata(), coarseHoles.size());
            {
                TfToken const &value = source.interpolateBoundary;
                if (value == TfToken("none")) input.boundary = prism::subdivision::BoundaryInterpolation::None;
                else if (value == TfToken("edgeOnly"))
                    input.boundary = prism::subdivision::BoundaryInterpolation::EdgeOnly;
            }
            {
                using F = prism::subdivision::FaceVaryingInterpolation;
                TfToken const &value = source.faceVaryingLinearInterpolation;
                if (value == TfToken("none")) input.faceVarying = F::None;
                else if (value == TfToken("cornersOnly")) input.faceVarying = F::CornersOnly;
                else if (value == TfToken("cornersPlus2")) input.faceVarying = F::CornersPlus2;
                else if (value == TfToken("boundaries")) input.faceVarying = F::Boundaries;
                else if (value == TfToken("all")) input.faceVarying = F::All;
            }
            if (!source.triangleSubdivisionRule.IsEmpty())
                input.smoothTriangles = source.triangleSubdivisionRule == TfToken("smooth");
            input.creaseIndices = std::span<const int>(source.creaseIndices.cdata(), source.creaseIndices.size());
            input.creaseLengths = std::span<const int>(source.creaseLengths.cdata(), source.creaseLengths.size());
            input.creaseSharpnesses = std::span<const float>(source.creaseSharpnesses.cdata(),
                                                             source.creaseSharpnesses.size());
            input.cornerIndices = std::span<const int>(source.cornerIndices.cdata(), source.cornerIndices.size());
            input.cornerSharpnesses = std::span<const float>(source.cornerSharpnesses.cdata(),
                                                             source.cornerSharpnesses.size());
            std::vector<float> uvs;
            const bool faceVaryingUvs = uvPv.interpolation == HdPrimvarSchemaTokens->faceVarying;
            const bool vertexUvs = uvPv.interpolation == HdPrimvarSchemaTokens->vertex ||
                uvPv.interpolation == HdPrimvarSchemaTokens->varying;
            auto appendUvs = [&](auto const &values, size_t expected) {
                if (values.size() != expected) return;
                uvs.reserve(expected * 2);
                for (auto const &uv : values) {
                    uvs.push_back(float(uv[0]));
                    uvs.push_back(float(uv[1]));
                }
            };
            if (faceVaryingUvs || vertexUvs) {
                const size_t expected = faceVaryingUvs ? coarseIndices.size() : pointCount;
                if (uvPv.value.IsHolding<VtVec2fArray>()) appendUvs(uvPv.value.UncheckedGet<VtVec2fArray>(), expected);
                else if (uvPv.value.IsHolding<VtVec2dArray>())
                    appendUvs(uvPv.value.UncheckedGet<VtVec2dArray>(), expected);
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
            refinedCounts.assign(refined.faceVertexCounts.begin(), refined.faceVertexCounts.end());
            refinedIndices.assign(refined.faceVertexIndices.begin(), refined.faceVertexIndices.end());
            refinedPoints.resize(refined.points.size() / 3);
            for (size_t i = 0; i < refinedPoints.size(); ++i)
                refinedPoints[i] = GfVec3f(refined.points[3 * i], refined.points[3 * i + 1], refined.points[3 * i + 2]);
            pointsFloat = &refinedPoints;
            pointsDouble = nullptr;
            pointCount = refinedPoints.size();
            normalPv = PrimvarData();
            if (!refined.uvs.empty()) {
                VtVec2fArray refinedUvs(refined.uvs.size() / 2);
                for (size_t i = 0; i < refinedUvs.size(); ++i)
                    refinedUvs[i] = GfVec2f(refined.uvs[2 * i], refined.uvs[2 * i + 1]);
                uvPv.value = VtValue(std::move(refinedUvs));
                uvPv.interpolation = HdPrimvarSchemaTokens->faceVarying;
            } else if (uvPv.interpolation != HdPrimvarSchemaTokens->constant) {
                uvPv = PrimvarData();
            }
        } else {
            info.subdivisionControlHull = true;
        }
    }
    VtIntArray const &counts = refined.valid ? refinedCounts : coarseCounts;
    VtIntArray const &indices = refined.valid ? refinedIndices : coarseIndices;
    static const VtIntArray noHoles;
    VtIntArray const &holes = refined.valid ? noHoles : coarseHoles;
    // The authored face of a (refined) face.
    auto coarseFace = [&](size_t face) {
        return refined.valid ? size_t(refined.coarseFaces[face]) : face;
    };
    // Face-varying primvars of refined meshes: the mean of each authored face.
    std::vector<std::vector<float>> faceMeans(extras.size());
    if (refined.valid) {
        for (size_t e = 0; e < extras.size(); ++e) {
            if (extras[e].interpolation != HdPrimvarSchemaTokens->faceVarying) continue;
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
            } else if (extra.interpolation == HdPrimvarSchemaTokens->faceVarying) {
                values = refined.valid ? faceMeans[e].data() + coarseFace(face) * n
                                       : extra.values.data() + cornerIndex * n;
            } else if (extra.interpolation == HdPrimvarSchemaTokens->uniform) {
                values = extra.values.data() + coarseFace(face) * n;
            } else {
                values = extra.values.data();
            }
            out.insert(out.end(), values, values + n);
        }
    };
    VtVec3fArray const *authoredNormalsFloat = nullptr;
    VtVec3dArray const *authoredNormalsDouble = nullptr;
    if (normalPv.value.IsHolding<VtVec3fArray>()) {
        authoredNormalsFloat = &normalPv.value.UncheckedGet<VtVec3fArray>();
    } else if (normalPv.value.IsHolding<VtVec3dArray>()) {
        authoredNormalsDouble = &normalPv.value.UncheckedGet<VtVec3dArray>();
    }
    const size_t authoredNormalCount = authoredNormalsFloat
        ? authoredNormalsFloat->size() : (authoredNormalsDouble ? authoredNormalsDouble->size() : 0);
    auto authoredNormalAt = [&](size_t i) -> GfVec3d {
        return authoredNormalsFloat ? GfVec3d((*authoredNormalsFloat)[i])
            : GfVec3d(GfVec3f((*authoredNormalsDouble)[i]));
    };
    VtVec2fArray const *authoredUvsFloat = nullptr;
    VtVec2dArray const *authoredUvsDouble = nullptr;
    if (uvPv.value.IsHolding<VtVec2fArray>()) {
        authoredUvsFloat = &uvPv.value.UncheckedGet<VtVec2fArray>();
    } else if (uvPv.value.IsHolding<VtVec2dArray>()) {
        authoredUvsDouble = &uvPv.value.UncheckedGet<VtVec2dArray>();
    }
    const size_t authoredUvCount = authoredUvsFloat
        ? authoredUvsFloat->size() : (authoredUvsDouble ? authoredUvsDouble->size() : 0);

    const bool flip = source.orientation == TfToken("leftHanded");
    const GfMatrix4d geometryTransform(1.0);
    const bool mirrored = geometryTransform.GetDeterminant() < 0.0;
    const GfMatrix4d normalMatrix = geometryTransform.GetInverse().GetTranspose();
    std::unordered_set<int> holeSet(holes.begin(), holes.end());

    // Match the standalone USD path when normals are missing, zero, or only
    // partially authored: accumulate one area-weighted coarse-face normal at
    // each referenced point. Keep this in prototype space; placement applies
    // the inverse transpose later.
    std::vector<GfVec3d> generatedNormals(pointCount, GfVec3d(0.0));
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
            GfVec3d contribution = GfCross(
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
            GfVec3d n(refined.normals[3 * i], refined.normals[3 * i + 1], refined.normals[3 * i + 2]);
            if (n.GetLengthSq() > 1e-20) generatedNormals[i] = flip ? -n : n;
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
    std::vector<GfVec3d> facePoints;
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
            GfVec3d wp[3];
            size_t first = mesh->vertices.size();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                wp[v] = geometryTransform.Transform(pointAt(pi));
                mesh->vertices.emplace_back(static_cast<float>(wp[v][0]),
                                            static_cast<float>(wp[v][1]),
                                            static_cast<float>(wp[v][2]));
                const size_t ui = PrimvarIndex(uvPv.interpolation, pi, ci, coarseFace(face));
                if (ui < authoredUvCount) {
                    if (authoredUvsFloat) {
                        mesh->texcoords.emplace_back((*authoredUvsFloat)[ui][0],
                                                     (*authoredUvsFloat)[ui][1]);
                    } else {
                        mesh->texcoords.emplace_back(
                            static_cast<float>((*authoredUvsDouble)[ui][0]),
                            static_cast<float>((*authoredUvsDouble)[ui][1]));
                    }
                }
                else mesh->texcoords.emplace_back(0.0f, 0.0f);
                if (extraStride) appendExtras(mesh->primvarValues, size_t(pi), ci, face);
            }
            const GfVec3d geometric = GfCross(wp[1] - wp[0], wp[2] - wp[0]).GetNormalized();
            for (int v = 0; v < 3; ++v) {
                const size_t ci = corner + local[v];
                const int pi = indices[ci];
                const size_t ni = PrimvarIndex(normalPv.interpolation, pi, ci, coarseFace(face));
                GfVec3d n = geometric;
                if (ni < authoredNormalCount) {
                    GfVec3d authored = authoredNormalAt(ni);
                    if (authored.GetLengthSq() > 1e-20 &&
                        std::isfinite(authored.GetLengthSq())) {
                        n = normalMatrix.TransformDir(authored).GetNormalized();
                        if (flip != mirrored) n = -n;
                    } else if (generatedNormals[pi].GetLengthSq() > 1e-20 &&
                               std::isfinite(generatedNormals[pi].GetLengthSq())) {
                        n = generatedNormals[pi].GetNormalized();
                    }
                } else if (generatedNormals[pi].GetLengthSq() > 1e-20 &&
                           std::isfinite(generatedNormals[pi].GetLengthSq())) {
                    n = generatedNormals[pi].GetNormalized();
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
