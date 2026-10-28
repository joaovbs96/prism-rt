#include "polygon_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace prism::mesh {
namespace {

// GfVec3f's arithmetic: float components; division by a double reciprocal.
struct Vec3f {
    float v[3] = {0.f, 0.f, 0.f};
    Vec3f() = default;
    Vec3f(float x, float y, float z) : v{x, y, z} {}
    float operator[](size_t i) const { return v[i]; }
    Vec3f operator-(Vec3f const& o) const { return Vec3f(v[0] - o[0], v[1] - o[1], v[2] - o[2]); }
    Vec3f operator-() const { return Vec3f(-v[0], -v[1], -v[2]); }
    Vec3f& operator+=(Vec3f const& o)
    {
        v[0] += o[0]; v[1] += o[1]; v[2] += o[2];
        return *this;
    }
    float lengthSq() const { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }
    float length() const { return std::sqrt(lengthSq()); }
    // GfVec3f::operator/(double): *this * (1.0 / s), each component through
    // float *= double.
    Vec3f operator/(double s) const
    {
        const double r = 1.0 / s;
        Vec3f result(*this);
        result.v[0] *= r; result.v[1] *= r; result.v[2] *= r;
        return result;
    }
};

// GfCross.
Vec3f Cross(Vec3f const& a, Vec3f const& b)
{
    return Vec3f(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
}

// A float or double array of `n` components as floats (GfVecNf(GfVecNd));
// empty for anything else.
std::vector<float> FloatArray(PrimvarView const& view, int n)
{
    std::vector<float> result;
    if (view.floatArray(n)) {
        result.assign(view.floats(), view.floats() + view.size * size_t(n));
    } else if (view.doubleArray(n)) {
        result.resize(view.size * size_t(n));
        for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<float>(view.doubles()[i]);
    }
    return result;
}

// Transform and normalize a normal (in double, as GfMatrix4d::TransformDir).
Float3 TransformNormal(Vec3f const& n, Matrix4d const& normalMatrix, bool flip)
{
    Vec3d tn = normalMatrix.transformDir(Vec3d(n[0], n[1], n[2]));
    if (flip) tn = -tn;
    const double len = tn.length();
    if (len > 0.0) tn /= len;
    return Float3(static_cast<float>(tn[0]), static_cast<float>(tn[1]), static_cast<float>(tn[2]));
}

// Fan triangulation of a face given its corner vertex indices; leftHanded
// faces swap the winding.
void Triangulate(std::vector<unsigned int>& out, std::vector<unsigned int> const& corners, bool leftHanded)
{
    const int count = static_cast<int>(corners.size());
    for (int i = 1; i + 1 < count; ++i) {
        out.push_back(corners[0]);
        out.push_back(corners[leftHanded ? i + 1 : i]);
        out.push_back(corners[leftHanded ? i : i + 1]);
    }
}

} // namespace

bool BuildPolygonMesh(MeshSource const& source, Matrix4d const& world, Mesh& out, std::vector<int>& coarseFaces)
{
    const std::vector<float> pointData = FloatArray(source.points, 3);
    const size_t pointCount = pointData.size() / 3;
    auto pointAt = [&](size_t i) { return Vec3f(pointData[3 * i], pointData[3 * i + 1], pointData[3 * i + 2]); };
    std::span<const int> counts = source.faceVertexCounts;
    std::span<const int> indices = source.faceVertexIndices;
    if (pointCount == 0 || counts.empty()) return false;
    const bool leftHanded = source.orientation == "leftHanded";
    const Matrix4d normalMatrix = world.inverse().transpose();
    const bool mirrored = world.determinant() < 0.0;
    std::unordered_set<int> holes(source.holeIndices.begin(), source.holeIndices.end());
    // Faces with a valid winding: three corners or more, inside the arrays.
    std::vector<uint8_t> valid(counts.size(), 0);
    {
        size_t corner = 0;
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            bool ok = count >= 3 && corner + size_t(count) <= indices.size() && !holes.count(int(face));
            for (int k = 0; ok && k < count; ++k)
                ok = indices[corner + k] >= 0 && size_t(indices[corner + k]) < pointCount;
            valid[face] = ok;
            corner += size_t(std::max(0, count));
        }
    }
    const std::vector<NamedPrimvar> extras = NamedPrimvars(source, pointCount, counts.size(), indices.size());
    const std::vector<float> uvs = FloatArray(source.uvs, 2);
    const std::vector<float> normals = FloatArray(source.normals, 3);
    const size_t uvCount = uvs.size() / 2, normalCount = normals.size() / 3;
    const Interpolation uvInterpolation = source.uvs.interpolation;
    const Interpolation normalInterpolation = source.normals.interpolation;
    auto perCorner = [](Interpolation interpolation) {
        return interpolation == Interpolation::FaceVarying || interpolation == Interpolation::Uniform;
    };
    bool expand = (uvCount && perCorner(uvInterpolation)) || (normalCount && perCorner(normalInterpolation));
    for (auto const& extra : extras)
        expand |= extra.interpolation == Interpolation::FaceVarying || extra.interpolation == Interpolation::Uniform;

    // Area-weighted per-point normals of the authored winding.
    std::vector<Vec3f> generated(pointCount, Vec3f(0.f, 0.f, 0.f));
    {
        size_t corner = 0;
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            if (valid[face]) {
                const int i0 = indices[corner], i1 = indices[corner + 1], i2 = indices[corner + 2];
                Vec3f fn = Cross(pointAt(size_t(i1)) - pointAt(size_t(i0)), pointAt(size_t(i2)) - pointAt(size_t(i0)));
                if (leftHanded) fn = -fn;
                for (int k = 0; k < count; ++k) generated[size_t(indices[corner + k])] += fn;
            }
            corner += size_t(std::max(0, count));
        }
        for (auto& n : generated) {
            const float length = n.length();
            n = length > 0.f ? n / length : Vec3f(0.f, 1.f, 0.f);
        }
    }
    // The shading normal at a corner: authored where it is usable (negated
    // for leftHanded meshes), otherwise generated.
    auto normalAt = [&](size_t point, size_t corner, size_t face) {
        const size_t index = PrimvarIndex(normalInterpolation, point, corner, face);
        if (index < normalCount) {
            const Vec3f n(normals[3 * index], normals[3 * index + 1], normals[3 * index + 2]);
            const float lengthSq = n.lengthSq();
            if (lengthSq > 1e-20f && std::isfinite(lengthSq))
                return TransformNormal(leftHanded ? -n : n, normalMatrix, mirrored);
        }
        return TransformNormal(generated[point], normalMatrix, mirrored);
    };
    auto uvAt = [&](size_t point, size_t corner, size_t face) {
        const size_t index = PrimvarIndex(uvInterpolation, point, corner, face);
        return index < uvCount ? Float2(uvs[2 * index], uvs[2 * index + 1]) : Float2(0.f, 0.f);
    };
    int stride = 0;
    for (auto const& extra : extras) {
        out.primvars.push_back({extra.name, extra.components, stride});
        stride += extra.components;
    }
    out.primvarStride = stride;
    auto appendExtras = [&](size_t point, size_t corner, size_t face) {
        for (auto const& extra : extras) {
            const size_t n = size_t(extra.components);
            const size_t element = extra.interpolation == Interpolation::Constant ? 0
                : PrimvarIndex(extra.interpolation, point, corner, face);
            out.primvarValues.insert(out.primvarValues.end(), extra.values.begin() + element * n,
                                     extra.values.begin() + (element + 1) * n);
        }
    };
    auto position = [&](size_t point) {
        const Vec3d p = world.transform(Vec3d(pointData[3 * point], pointData[3 * point + 1], pointData[3 * point + 2]));
        return Float3(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
    };

    std::vector<unsigned int> corners;
    size_t corner = 0;
    if (expand) {
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            if (valid[face]) {
                corners.clear();
                for (int k = 0; k < count; ++k) {
                    const size_t point = size_t(indices[corner + k]);
                    corners.push_back(static_cast<unsigned int>(out.vertices.size()));
                    out.vertices.push_back(position(point));
                    out.texcoords.push_back(uvAt(point, corner + k, face));
                    out.normals.push_back(normalAt(point, corner + k, face));
                    if (stride) appendExtras(point, corner + k, face);
                }
                const size_t before = out.indices.size();
                Triangulate(out.indices, corners, leftHanded);
                coarseFaces.insert(coarseFaces.end(), (out.indices.size() - before) / 3, int(face));
            }
            corner += size_t(std::max(0, count));
        }
    } else {
        out.vertices.reserve(pointCount);
        for (size_t point = 0; point < pointCount; ++point) {
            out.vertices.push_back(position(point));
            out.normals.push_back(normalAt(point, 0, 0));
            out.texcoords.push_back(uvAt(point, 0, 0));
            if (stride) appendExtras(point, 0, 0);
        }
        for (size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            if (valid[face]) {
                corners.assign(indices.begin() + corner, indices.begin() + corner + count);
                const size_t before = out.indices.size();
                Triangulate(out.indices, corners, leftHanded);
                coarseFaces.insert(coarseFaces.end(), (out.indices.size() - before) / 3, int(face));
            }
            corner += size_t(std::max(0, count));
        }
    }
    out.single_sided_all = source.doubleSided ? !*source.doubleSided : true;
    return !out.indices.empty();
}

} // namespace prism::mesh
