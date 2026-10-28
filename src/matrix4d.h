#pragma once

// Double-precision vectors and a 4x4 matrix for camera and mesh math in the
// core, without USD. They follow pxr's Gf types exactly: row vectors
// (p' = p * M), m[row][column], and the same arithmetic in the same order
// (Transform projects by 1/w, normalisation divides by max(length, 1e-10)
// through a reciprocal, the inverse is Gf's cofactor expansion), so results
// are bit-identical to GfVec3d/GfVec4d/GfMatrix4d (tests/matrix4d_test.cpp
// checks this against Gf). Conversions to and from Gf live with the USD
// code (src/usd/gf_convert.h).

#include <cfloat>
#include <cmath>
#include <cstddef>

namespace prism {

struct Vec3d {
    double v[3] = {0.0, 0.0, 0.0};

    Vec3d() = default;
    explicit Vec3d(double s) : v{s, s, s} {}
    Vec3d(double x, double y, double z) : v{x, y, z} {}

    double operator[](size_t i) const { return v[i]; }
    double& operator[](size_t i) { return v[i]; }

    Vec3d operator-() const { return Vec3d(-v[0], -v[1], -v[2]); }
    Vec3d& operator+=(Vec3d const& o) { v[0] += o[0]; v[1] += o[1]; v[2] += o[2]; return *this; }
    Vec3d& operator-=(Vec3d const& o) { v[0] -= o[0]; v[1] -= o[1]; v[2] -= o[2]; return *this; }
    Vec3d& operator*=(double s) { v[0] *= s; v[1] *= s; v[2] *= s; return *this; }
    // Like Gf: a multiplication by the reciprocal.
    Vec3d& operator/=(double s) { return *this *= (1.0 / s); }
    friend Vec3d operator+(Vec3d const& a, Vec3d const& b) { return Vec3d(a) += b; }
    friend Vec3d operator-(Vec3d const& a, Vec3d const& b) { return Vec3d(a) -= b; }
    Vec3d operator*(double s) const { return Vec3d(*this) *= s; }
    friend Vec3d operator*(double s, Vec3d const& a) { return a * s; }
    Vec3d operator/(double s) const { return *this * (1.0 / s); }

    double dot(Vec3d const& o) const { return v[0] * o[0] + v[1] * o[1] + v[2] * o[2]; }
    double lengthSq() const { return dot(*this); }
    double length() const { return std::sqrt(lengthSq()); }
    // GfVec3d::GetNormalized: divides by the length, or by eps when shorter.
    Vec3d normalized(double eps = 1e-10) const
    {
        Vec3d result(*this);
        const double len = length();
        result /= (len > eps) ? len : eps;
        return result;
    }
};

inline Vec3d cross(Vec3d const& a, Vec3d const& b)
{
    return Vec3d(a[1] * b[2] - a[2] * b[1],
                 a[2] * b[0] - a[0] * b[2],
                 a[0] * b[1] - a[1] * b[0]);
}

struct Vec4d {
    double v[4] = {0.0, 0.0, 0.0, 0.0};

    Vec4d() = default;
    Vec4d(double x, double y, double z, double w) : v{x, y, z, w} {}

    double operator[](size_t i) const { return v[i]; }
    double& operator[](size_t i) { return v[i]; }

    Vec4d& operator*=(double s) { v[0] *= s; v[1] *= s; v[2] *= s; v[3] *= s; return *this; }
    Vec4d& operator/=(double s) { return *this *= (1.0 / s); }
};

// Row-major 4x4 matrix, row-vector convention (translation in row 3).
struct Matrix4d {
    double m[4][4] = {{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0},
                      {0.0, 0.0, 1.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};

    // The identity.
    Matrix4d() = default;
    // s on the diagonal (GfMatrix4d(double)).
    explicit Matrix4d(double s) { setDiagonal(s); }
    Matrix4d(double m00, double m01, double m02, double m03,
             double m10, double m11, double m12, double m13,
             double m20, double m21, double m22, double m23,
             double m30, double m31, double m32, double m33)
        : m{{m00, m01, m02, m03}, {m10, m11, m12, m13}, {m20, m21, m22, m23}, {m30, m31, m32, m33}} {}

    double* operator[](size_t row) { return m[row]; }
    double const* operator[](size_t row) const { return m[row]; }

    bool operator==(Matrix4d const& o) const
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (m[r][c] != o.m[r][c]) return false;
        return true;
    }
    bool operator!=(Matrix4d const& o) const { return !(*this == o); }

    Matrix4d& setDiagonal(double s)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[r][c] = r == c ? s : 0.0;
        return *this;
    }
    // GfMatrix4d::SetScale(double): s on the first three diagonal entries.
    Matrix4d& setScale(double s)
    {
        setDiagonal(s);
        m[3][3] = 1.0;
        return *this;
    }

    // The point vec (w = 1), projected back by 1/w (GfMatrix4d::Transform).
    Vec3d transform(Vec3d const& vec) const
    {
        const double x = vec[0] * m[0][0] + vec[1] * m[1][0] + vec[2] * m[2][0] + m[3][0];
        const double y = vec[0] * m[0][1] + vec[1] * m[1][1] + vec[2] * m[2][1] + m[3][1];
        const double z = vec[0] * m[0][2] + vec[1] * m[1][2] + vec[2] * m[2][2] + m[3][2];
        const double w = vec[0] * m[0][3] + vec[1] * m[1][3] + vec[2] * m[2][3] + m[3][3];
        const double inv = (w != 0.0) ? 1.0 / w : 1.0;
        return Vec3d(inv * x, inv * y, inv * z);
    }
    // The direction vec (w = 0; GfMatrix4d::TransformDir).
    Vec3d transformDir(Vec3d const& vec) const
    {
        return Vec3d(vec[0] * m[0][0] + vec[1] * m[1][0] + vec[2] * m[2][0],
                     vec[0] * m[0][1] + vec[1] * m[1][1] + vec[2] * m[2][1],
                     vec[0] * m[0][2] + vec[1] * m[1][2] + vec[2] * m[2][2]);
    }

    Matrix4d transpose() const
    {
        Matrix4d t;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) t.m[c][r] = m[r][c];
        return t;
    }

    // GfMatrix4d::GetDeterminant: along the last column.
    double determinant() const
    {
        return (-m[0][3] * determinant3(1, 2, 3, 0, 1, 2)
                + m[1][3] * determinant3(0, 2, 3, 0, 1, 2)
                - m[2][3] * determinant3(0, 1, 3, 0, 1, 2)
                + m[3][3] * determinant3(0, 1, 2, 0, 1, 2));
    }

    // GfMatrix4d::GetInverse: cofactors of 2x2 minors; a singular matrix
    // (|det| <= eps) gives setScale(FLT_MAX). *det receives the determinant.
    Matrix4d inverse(double* det = nullptr, double eps = 0.0) const
    {
        double x00, x01, x02, x03;
        double x10, x11, x12, x13;
        double x20, x21, x22, x23;
        double x30, x31, x32, x33;
        double y01, y02, y03, y12, y13, y23;
        double z00, z10, z20, z30;
        double z01, z11, z21, z31;
        double z02, z03, z12, z13, z22, z23, z32, z33;

        x00 = m[0][0]; x01 = m[0][1];
        x10 = m[1][0]; x11 = m[1][1];
        x20 = m[2][0]; x21 = m[2][1];
        x30 = m[3][0]; x31 = m[3][1];

        y01 = x00*x11 - x10*x01;
        y02 = x00*x21 - x20*x01;
        y03 = x00*x31 - x30*x01;
        y12 = x10*x21 - x20*x11;
        y13 = x10*x31 - x30*x11;
        y23 = x20*x31 - x30*x21;

        x02 = m[0][2]; x03 = m[0][3];
        x12 = m[1][2]; x13 = m[1][3];
        x22 = m[2][2]; x23 = m[2][3];
        x32 = m[3][2]; x33 = m[3][3];

        z33 = x02*y12 - x12*y02 + x22*y01;
        z23 = x12*y03 - x32*y01 - x02*y13;
        z13 = x02*y23 - x22*y03 + x32*y02;
        z03 = x22*y13 - x32*y12 - x12*y23;
        z32 = x13*y02 - x23*y01 - x03*y12;
        z22 = x03*y13 - x13*y03 + x33*y01;
        z12 = x23*y03 - x33*y02 - x03*y23;
        z02 = x13*y23 - x23*y13 + x33*y12;

        y01 = x02*x13 - x12*x03;
        y02 = x02*x23 - x22*x03;
        y03 = x02*x33 - x32*x03;
        y12 = x12*x23 - x22*x13;
        y13 = x12*x33 - x32*x13;
        y23 = x22*x33 - x32*x23;

        z30 = x11*y02 - x21*y01 - x01*y12;
        z20 = x01*y13 - x11*y03 + x31*y01;
        z10 = x21*y03 - x31*y02 - x01*y23;
        z00 = x11*y23 - x21*y13 + x31*y12;
        z31 = x00*y12 - x10*y02 + x20*y01;
        z21 = x10*y03 - x30*y01 - x00*y13;
        z11 = x00*y23 - x20*y03 + x30*y02;
        z01 = x20*y13 - x30*y12 - x10*y23;

        const double d = x30*z30 + x20*z20 + x10*z10 + x00*z00;
        if (det) *det = d;

        Matrix4d result;
        if (std::abs(d) > eps) {
            const double rcp = 1.0 / d;
            result.m[0][0] = z00*rcp;
            result.m[0][1] = z10*rcp;
            result.m[1][0] = z01*rcp;
            result.m[0][2] = z20*rcp;
            result.m[2][0] = z02*rcp;
            result.m[0][3] = z30*rcp;
            result.m[3][0] = z03*rcp;
            result.m[1][1] = z11*rcp;
            result.m[1][2] = z21*rcp;
            result.m[2][1] = z12*rcp;
            result.m[1][3] = z31*rcp;
            result.m[3][1] = z13*rcp;
            result.m[2][2] = z22*rcp;
            result.m[2][3] = z32*rcp;
            result.m[3][2] = z23*rcp;
            result.m[3][3] = z33*rcp;
        } else {
            result.setScale(FLT_MAX);
        }
        return result;
    }

private:
    double determinant3(size_t r1, size_t r2, size_t r3, size_t c1, size_t c2, size_t c3) const
    {
        return (m[r1][c1] * m[r2][c2] * m[r3][c3] +
                m[r1][c2] * m[r2][c3] * m[r3][c1] +
                m[r1][c3] * m[r2][c1] * m[r3][c2] -
                m[r1][c1] * m[r2][c3] * m[r3][c2] -
                m[r1][c2] * m[r2][c1] * m[r3][c3] -
                m[r1][c3] * m[r2][c2] * m[r3][c1]);
    }
};

// Row vector times matrix (GfVec4d * GfMatrix4d).
inline Vec4d operator*(Vec4d const& vec, Matrix4d const& m)
{
    return Vec4d(vec[0] * m.m[0][0] + vec[1] * m.m[1][0] + vec[2] * m.m[2][0] + vec[3] * m.m[3][0],
                 vec[0] * m.m[0][1] + vec[1] * m.m[1][1] + vec[2] * m.m[2][1] + vec[3] * m.m[3][1],
                 vec[0] * m.m[0][2] + vec[1] * m.m[1][2] + vec[2] * m.m[2][2] + vec[3] * m.m[3][2],
                 vec[0] * m.m[0][3] + vec[1] * m.m[1][3] + vec[2] * m.m[2][3] + vec[3] * m.m[3][3]);
}

// Matrix product (GfMatrix4d::operator*=): a then b for row vectors.
inline Matrix4d operator*(Matrix4d const& a, Matrix4d const& b)
{
    Matrix4d r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

} // namespace prism
