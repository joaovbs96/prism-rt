// prism::Matrix4d/Vec3d/Vec4d (src/matrix4d.h) against pxr's Gf types: the
// core's camera and mesh math replaced Gf and must give bit-identical results.

#include "../src/matrix4d.h"
#include "../src/usd/gf_convert.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>

#include <bit>
#include <cstdint>
#include <iostream>
#include <random>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int failures = 0;

bool Same(double a, double b) { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }

void Expect(bool ok, char const* what, int iteration)
{
    if (ok) return;
    if (++failures <= 20) std::cerr << "FAILED: " << what << " (case " << iteration << ")\n";
}

bool SameMatrix(prism::Matrix4d const& a, GfMatrix4d const& b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!Same(a.m[r][c], b[r][c])) return false;
    return true;
}

bool SameVec(prism::Vec3d const& a, GfVec3d const& b)
{
    return Same(a[0], b[0]) && Same(a[1], b[1]) && Same(a[2], b[2]);
}

GfMatrix4d RandomMatrix(std::mt19937_64& rng, int kind)
{
    std::uniform_real_distribution<double> value(-10.0, 10.0);
    std::uniform_real_distribution<double> angle(-180.0, 180.0);
    GfMatrix4d m(1.0);
    switch (kind % 5) {
    case 0: // general
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[r][c] = value(rng);
        break;
    case 1: { // rigid + scale, as transforms are
        GfMatrix4d rotation(1.0), scale(1.0), translation(1.0);
        rotation.SetRotate(GfRotation(GfVec3d(value(rng), value(rng), value(rng)), angle(rng)));
        scale.SetScale(GfVec3d(value(rng), value(rng), value(rng)));
        translation.SetTranslate(GfVec3d(value(rng), value(rng), value(rng)));
        m = scale * rotation * translation;
        break;
    }
    case 2: // perspective-like
        m.SetZero();
        m[0][0] = value(rng); m[1][1] = value(rng); m[2][2] = value(rng);
        m[2][3] = -1.0; m[3][2] = value(rng);
        break;
    case 3: // singular
        for (int c = 0; c < 4; ++c) { m[0][c] = value(rng); m[1][c] = 2.0 * m[0][c]; }
        break;
    default: // identity
        break;
    }
    return m;
}

} // namespace

int main()
{
    std::mt19937_64 rng(20261028);
    std::uniform_real_distribution<double> value(-100.0, 100.0);
    for (int i = 0; i < 20000; ++i) {
        const GfMatrix4d gf = RandomMatrix(rng, i);
        const GfMatrix4d gf2 = RandomMatrix(rng, i + 1);
        const prism::Matrix4d m = prism::usd::ToMatrix4d(gf);
        const prism::Matrix4d m2 = prism::usd::ToMatrix4d(gf2);
        Expect(SameMatrix(m, gf), "conversion", i);
        Expect(prism::usd::ToGf(m) == gf, "conversion back", i);

        double gfDet = 0.0, det = 0.0;
        Expect(SameMatrix(m.inverse(&det), gf.GetInverse(&gfDet)), "inverse", i);
        Expect(Same(det, gfDet), "inverse determinant", i);
        Expect(SameMatrix(m.inverse(nullptr, 1e-9), gf.GetInverse(nullptr, 1e-9)), "inverse eps", i);
        Expect(Same(m.determinant(), gf.GetDeterminant()), "determinant", i);
        Expect(SameMatrix(m.transpose(), gf.GetTranspose()), "transpose", i);
        Expect(SameMatrix(m * m2, gf * gf2), "product", i);
        Expect(SameMatrix(m.inverse().transpose(), gf.GetInverse().GetTranspose()), "normal matrix", i);

        const GfVec3d p(value(rng), value(rng), (i % 7 == 0) ? -0.0 : value(rng));
        const prism::Vec3d q = prism::usd::ToVec3d(p);
        Expect(SameVec(m.transform(q), gf.Transform(p)), "transform", i);
        Expect(SameVec(m.transformDir(q), gf.TransformDir(p)), "transformDir", i);
        Expect(SameVec(q.normalized(), p.GetNormalized()), "normalized", i);
        const GfVec3d tiny = p * 1e-12;
        Expect(SameVec(prism::usd::ToVec3d(tiny).normalized(), tiny.GetNormalized()), "normalized tiny", i);
        const GfVec3d p2(value(rng), value(rng), value(rng));
        const prism::Vec3d q2 = prism::usd::ToVec3d(p2);
        Expect(SameVec(prism::cross(q, q2), GfCross(p, p2)), "cross", i);
        Expect(SameVec(q - q2, p - p2), "difference", i);
        Expect(SameVec(q + q2, p + p2), "sum", i);
        Expect(SameVec(q * 0.37, p * 0.37), "scale", i);
        Expect(SameVec(-q, -p), "negation", i);
        Expect(Same(q.lengthSq(), p.GetLengthSq()), "lengthSq", i);
        Expect(Same(q.length(), p.GetLength()), "length", i);
        // Float round trip of points (mesh conversion's GfVec3d(GfVec3f(d))).
        const GfVec3d rounded{GfVec3f(p)};
        Expect(SameVec(prism::Vec3d(float(q[0]), float(q[1]), float(q[2])), rounded), "float round trip", i);

        GfVec4d h(value(rng), value(rng), -1.0, 1.0);
        prism::Vec4d k(h[0], h[1], h[2], h[3]);
        const GfVec4d gh = h * gf;
        prism::Vec4d kh = k * m;
        Expect(Same(kh[0], gh[0]) && Same(kh[1], gh[1]) && Same(kh[2], gh[2]) && Same(kh[3], gh[3]),
               "row vector product", i);
        GfVec4d ghd = gh;
        if (gh[3] != 0.0) { ghd /= gh[3]; kh /= kh[3]; }
        Expect(Same(kh[0], ghd[0]) && Same(kh[1], ghd[1]) && Same(kh[2], ghd[2]) && Same(kh[3], ghd[3]),
               "homogeneous division", i);
    }
    // Identity, as mesh conversion uses it.
    const GfMatrix4d identity(1.0);
    Expect(SameMatrix(prism::Matrix4d(1.0), identity), "identity", -1);
    Expect(SameMatrix(prism::Matrix4d(), identity), "default identity", -1);
    Expect(SameMatrix(prism::Matrix4d(1.0).inverse().transpose(), identity.GetInverse().GetTranspose()),
           "identity normal matrix", -1);
    const GfVec3d negativeZero(-0.0, 2.0, -0.0);
    Expect(SameVec(prism::Matrix4d(1.0).transform(prism::usd::ToVec3d(negativeZero)), identity.Transform(negativeZero)),
           "identity transform of -0", -1);
    Expect(SameVec(prism::Matrix4d(1.0).inverse().transpose().transformDir(prism::usd::ToVec3d(negativeZero)),
                   identity.GetInverse().GetTranspose().TransformDir(negativeZero)),
           "identity normal transform of -0", -1);
    if (failures) {
        std::cerr << failures << " mismatches\n";
        return 1;
    }
    std::cout << "matrix4d matches Gf\n";
    return 0;
}
