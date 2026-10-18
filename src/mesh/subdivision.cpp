#include "subdivision.h"

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace prism::subdivision {

namespace {

using namespace OpenSubdiv;

template <int N>
struct Value {
    float v[N];
    void Clear(void* = nullptr) { std::fill(v, v + N, 0.0f); }
    void AddWithWeight(Value const& source, float weight)
    {
        for (int i = 0; i < N; ++i) v[i] += weight * source.v[i];
    }
};

// A run of `stride` floats per element in one buffer, for OpenSubdiv's
// primvar refinement of data whose size is known only at run time.
struct StridedBuffer {
    float* data = nullptr;
    int stride = 0;
    struct Element {
        float* v;
        int n;
        void Clear(void* = nullptr) { std::fill(v, v + n, 0.0f); }
        void AddWithWeight(Element const& source, float weight)
        {
            for (int i = 0; i < n; ++i) v[i] += weight * source.v[i];
        }
    };
    Element operator[](int i) const { return {data + size_t(i) * stride, stride}; }
};

Sdc::SchemeType SchemeType(Scheme scheme)
{
    switch (scheme) {
    case Scheme::Loop: return Sdc::SCHEME_LOOP;
    case Scheme::Bilinear: return Sdc::SCHEME_BILINEAR;
    default: return Sdc::SCHEME_CATMARK;
    }
}

Sdc::Options SchemeOptions(Input const& input)
{
    Sdc::Options options;
    switch (input.boundary) {
    case BoundaryInterpolation::None: options.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_NONE); break;
    case BoundaryInterpolation::EdgeOnly:
        options.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
        break;
    default: options.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER); break;
    }
    switch (input.faceVarying) {
    case FaceVaryingInterpolation::None: options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_NONE); break;
    case FaceVaryingInterpolation::CornersOnly:
        options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_CORNERS_ONLY);
        break;
    case FaceVaryingInterpolation::CornersPlus2:
        options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_CORNERS_PLUS2);
        break;
    case FaceVaryingInterpolation::Boundaries:
        options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_BOUNDARIES);
        break;
    case FaceVaryingInterpolation::All: options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_ALL); break;
    default: options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_CORNERS_PLUS1); break;
    }
    options.SetTriangleSubdivision(input.smoothTriangles ? Sdc::Options::TRI_SUB_SMOOTH
                                                         : Sdc::Options::TRI_SUB_CATMARK);
    return options;
}

} // namespace

size_t RefinedFaceCount(std::span<const int> faceVertexCounts, Scheme scheme, int level)
{
    if (level <= 0) return faceVertexCounts.size();
    const size_t perLevel = size_t(1) << (2 * (level - 1)); // 4^(level-1)
    size_t total = 0;
    for (int count : faceVertexCounts) {
        if (count < 3) continue;
        // The first level splits an n-gon into n quads (Catmull-Clark,
        // bilinear) or a triangle into 4 (Loop); later levels quadruple.
        const size_t first = scheme == Scheme::Loop ? 4 : (count == 4 ? 4 : size_t(count));
        total += first * perLevel;
    }
    return total;
}

Output Refine(Input const& input)
{
    Output output;
    const int pointCount = int(input.points.size() / 3);
    if (input.level <= 0 || pointCount == 0 || input.faceVertexCounts.empty()) return output;

    Scheme scheme = input.scheme;
    if (scheme == Scheme::Loop &&
        std::any_of(input.faceVertexCounts.begin(), input.faceVertexCounts.end(), [](int c) { return c != 3; }))
        scheme = Scheme::CatmullClark; // Loop needs triangles

    Far::TopologyDescriptor descriptor;
    descriptor.numVertices = pointCount;
    descriptor.numFaces = int(input.faceVertexCounts.size());
    descriptor.numVertsPerFace = input.faceVertexCounts.data();
    descriptor.vertIndicesPerFace = input.faceVertexIndices.data();

    // USD creases are chains of vertices with one sharpness per chain or one
    // per edge; OpenSubdiv takes edges.
    std::vector<int> creasePairs;
    std::vector<float> creaseWeights;
    {
        size_t offset = 0;
        size_t edgeCount = 0;
        for (int length : input.creaseLengths) edgeCount += size_t(std::max(0, length - 1));
        const bool perEdge = input.creaseSharpnesses.size() == edgeCount &&
            input.creaseSharpnesses.size() != input.creaseLengths.size();
        size_t edge = 0;
        for (size_t c = 0; c < input.creaseLengths.size(); ++c) {
            const int length = input.creaseLengths[c];
            for (int k = 0; k + 1 < length; ++k, ++edge) {
                if (offset + k + 1 >= input.creaseIndices.size()) break;
                creasePairs.push_back(input.creaseIndices[offset + k]);
                creasePairs.push_back(input.creaseIndices[offset + k + 1]);
                const size_t s = perEdge ? edge : c;
                creaseWeights.push_back(s < input.creaseSharpnesses.size() ? input.creaseSharpnesses[s] : 10.0f);
            }
            offset += size_t(std::max(0, length));
        }
    }
    descriptor.numCreases = int(creaseWeights.size());
    descriptor.creaseVertexIndexPairs = creasePairs.data();
    descriptor.creaseWeights = creaseWeights.data();
    const size_t corners = std::min(input.cornerIndices.size(), input.cornerSharpnesses.size());
    descriptor.numCorners = int(corners);
    descriptor.cornerVertexIndices = input.cornerIndices.data();
    descriptor.cornerWeights = input.cornerSharpnesses.data();
    descriptor.numHoles = int(input.holes.size());
    descriptor.holeIndices = input.holes.data();

    // Face-varying texture coordinates: corners of one point with equal
    // values share a value.
    std::vector<int> uvIndices;
    std::vector<Value<2>> uvValues;
    Far::TopologyDescriptor::FVarChannel channel;
    const bool hasUvs = input.uvPerPoint ? input.uvs.size() == size_t(pointCount) * 2
                                         : input.uvs.size() == input.faceVertexIndices.size() * 2;
    if (hasUvs) {
        if (input.uvPerPoint) {
            uvIndices.assign(input.faceVertexIndices.begin(), input.faceVertexIndices.end());
            for (int p = 0; p < pointCount; ++p) uvValues.push_back({{input.uvs[2 * p], input.uvs[2 * p + 1]}});
        } else {
            struct KeyHash {
                size_t operator()(std::pair<int, uint64_t> const& k) const
                {
                    return std::hash<uint64_t>{}(k.second * 0x9e3779b97f4a7c15ull ^ uint64_t(k.first));
                }
            };
            std::unordered_map<std::pair<int, uint64_t>, int, KeyHash> shared;
            uvIndices.resize(input.faceVertexIndices.size());
            for (size_t c = 0; c < input.faceVertexIndices.size(); ++c) {
                const float u = input.uvs[2 * c], v = input.uvs[2 * c + 1];
                const uint64_t bits = (uint64_t(std::bit_cast<uint32_t>(u)) << 32) | std::bit_cast<uint32_t>(v);
                auto [found, inserted] =
                    shared.emplace(std::make_pair(input.faceVertexIndices[c], bits), int(uvValues.size()));
                if (inserted) uvValues.push_back({{u, v}});
                uvIndices[c] = found->second;
            }
        }
        channel.numValues = int(uvValues.size());
        channel.valueIndices = uvIndices.data();
        descriptor.numFVarChannels = 1;
        descriptor.fvarChannels = &channel;
    }

    using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
    std::unique_ptr<Far::TopologyRefiner> refiner(
        Factory::Create(descriptor, Factory::Options(SchemeType(scheme), SchemeOptions(input))));
    if (!refiner) return output;
    const int level = std::min(input.level, 8);
    Far::TopologyRefiner::UniformOptions uniform(level);
    uniform.fullTopologyInLastLevel = true; // needed by the limit evaluation
    refiner->RefineUniform(uniform);

    std::vector<Value<3>> vertices(size_t(refiner->GetNumVerticesTotal()));
    for (int p = 0; p < pointCount; ++p)
        vertices[p] = {{input.points[3 * p], input.points[3 * p + 1], input.points[3 * p + 2]}};
    std::vector<Value<2>> fvar;
    if (hasUvs) {
        fvar.resize(size_t(refiner->GetNumFVarValuesTotal(0)));
        std::copy(uvValues.begin(), uvValues.end(), fvar.begin());
    }
    Far::PrimvarRefiner primvars(*refiner);
    Value<3>* source = vertices.data();
    Value<2>* fvarSource = fvar.data();
    for (int l = 1; l <= level; ++l) {
        Value<3>* destination = source + refiner->GetLevel(l - 1).GetNumVertices();
        primvars.Interpolate(l, source, destination);
        source = destination;
        if (hasUvs) {
            Value<2>* fvarDestination = fvarSource + refiner->GetLevel(l - 1).GetNumFVarValues(0);
            primvars.InterpolateFaceVarying(l, fvarSource, fvarDestination, 0);
            fvarSource = fvarDestination;
        }
    }

    // Named per-point data follows the vertex refinement.
    std::vector<float> pointData;
    const int stride = input.pointDataStride;
    const bool hasPointData = stride > 0 && input.pointData.size() == size_t(pointCount) * size_t(stride);
    if (hasPointData) {
        pointData.assign(size_t(refiner->GetNumVerticesTotal()) * stride, 0.0f);
        std::copy(input.pointData.begin(), input.pointData.end(), pointData.begin());
        size_t offset = 0;
        for (int l = 1; l <= level; ++l) {
            const size_t next = offset + size_t(refiner->GetLevel(l - 1).GetNumVertices());
            StridedBuffer source{pointData.data() + offset * stride, stride};
            StridedBuffer destination{pointData.data() + next * stride, stride};
            primvars.Interpolate(l, source, destination);
            offset = next;
        }
        std::vector<float> limitData(size_t(refiner->GetLevel(level).GetNumVertices()) * stride, 0.0f);
        StridedBuffer source{pointData.data() + offset * stride, stride};
        StridedBuffer destination{limitData.data(), stride};
        primvars.Limit(source, destination);
        output.pointData = std::move(limitData);
    }

    Far::TopologyLevel const& last = refiner->GetLevel(level);
    const int vertexCount = last.GetNumVertices();
    const size_t limitCount = size_t(vertexCount);
    std::vector<Value<3>> limit(limitCount), du(limitCount), dv(limitCount);
    primvars.Limit(source, limit, du, dv);
    std::vector<Value<2>> limitUvs;
    if (hasUvs) {
        limitUvs.resize(size_t(last.GetNumFVarValues(0)));
        primvars.LimitFaceVarying(fvarSource, limitUvs, 0);
    }

    output.points.resize(size_t(vertexCount) * 3);
    output.normals.resize(size_t(vertexCount) * 3);
    for (int v = 0; v < vertexCount; ++v) {
        for (int k = 0; k < 3; ++k) output.points[3 * v + k] = limit[v].v[k];
        const float* a = du[v].v;
        const float* b = dv[v].v;
        float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; ++k) output.normals[3 * v + k] = length > 0.0f && std::isfinite(length) ? n[k] / length : 0.0f;
    }
    const int faceCount = last.GetNumFaces();
    output.faceVertexCounts.reserve(size_t(faceCount));
    output.faceVertexIndices.reserve(size_t(faceCount) * 4);
    output.coarseFaces.reserve(size_t(faceCount));
    if (hasUvs) output.uvs.reserve(size_t(faceCount) * 8);
    for (int f = 0; f < faceCount; ++f) {
        if (last.IsFaceHole(f)) continue;
        Far::ConstIndexArray face = last.GetFaceVertices(f);
        output.faceVertexCounts.push_back(face.size());
        for (int k = 0; k < face.size(); ++k) output.faceVertexIndices.push_back(face[k]);
        if (hasUvs) {
            Far::ConstIndexArray values = last.GetFaceFVarValues(f, 0);
            for (int k = 0; k < values.size(); ++k) {
                output.uvs.push_back(limitUvs[values[k]].v[0]);
                output.uvs.push_back(limitUvs[values[k]].v[1]);
            }
        }
        int coarse = f;
        for (int l = level; l > 0; --l) coarse = refiner->GetLevel(l).GetFaceParentFace(coarse);
        output.coarseFaces.push_back(coarse);
    }
    output.valid = !output.faceVertexCounts.empty();
    return output;
}

} // namespace prism::subdivision
