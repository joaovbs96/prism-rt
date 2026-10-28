#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace prism::mesh {

// Return original face-corner indices, preserving winding and face-varying data.
// Project onto the dominant Newell-normal plane and clip ears so concave faces
// do not produce triangles outside their boundary. Convex faces keep fan order.
// This output-parameter form serves per-face loops: `triangles` needs room for
// count-2 entries, and the number written is returned (zero when the face is
// degenerate). Faces of up to eight corners use no heap memory.
// Point: three doubles by operator[] (prism::Vec3d, pxr::GfVec3d).
template <class Point>
inline size_t TriangulatePolygon(const Point* points, size_t count,
                                 std::array<int,3>* triangles)
{
    if (count < 3) return 0;
    double normal[3] = {0.0, 0.0, 0.0};
    for (size_t i=0;i<count;++i) {
        const auto& a=points[i]; const auto& b=points[(i+1)%count];
        normal[0]+=(a[1]-b[1])*(a[2]+b[2]);
        normal[1]+=(a[2]-b[2])*(a[0]+b[0]);
        normal[2]+=(a[0]-b[0])*(a[1]+b[1]);
    }
    int drop=0;
    for (int i=1;i<3;++i) if (std::abs(normal[i])>std::abs(normal[drop])) drop=i;
    const int u=(drop+1)%3, v=(drop+2)%3;
    const auto cross=[&](int a,int b,int c) {
        return (points[b][u]-points[a][u])*(points[c][v]-points[a][v])-
               (points[b][v]-points[a][v])*(points[c][u]-points[a][u]);
    };
    double scale=0;
    // Like GfVec3d::GetLengthSq of the difference.
    const auto lengthSq=[&](size_t i) {
        const double d0=points[i][0]-points[0][0], d1=points[i][1]-points[0][1], d2=points[i][2]-points[0][2];
        return d0*d0+d1*d1+d2*d2;
    };
    for (size_t i=0;i<count;++i) scale=std::max(scale,lengthSq(i));
    const double epsilon=std::max(scale*1e-12,1e-30);
    double area=0;
    for (size_t i=1;i+1<count;++i) area+=cross(0,int(i),int(i+1));
    if (std::abs(area)<=epsilon) return 0;
    const double winding=area>0?1.0:-1.0;
    int small[8];
    std::vector<int> large;
    int* remaining=small;
    if (count>8) { large.resize(count); remaining=large.data(); }
    std::iota(remaining,remaining+count,0);
    size_t size=count, written=0;
    const auto erase=[&](size_t i) {
        std::copy(remaining+i+1,remaining+size,remaining+i); --size;
    };
    while (size>3) {
        bool removed=false;
        for (size_t step=0;step<size;++step) {
            const size_t i=(step+1)%size;
            const int a=remaining[(i+size-1)%size];
            const int b=remaining[i], c=remaining[(i+1)%size];
            const double turn=winding*cross(a,b,c);
            if (std::abs(turn)<=epsilon) {
                erase(i); removed=true; break;
            }
            if (turn<0) continue;
            bool occupied=false;
            for (size_t k=0;k<size;++k) {
                const int p=remaining[k];
                if (p==a || p==b || p==c) continue;
                if (winding*cross(a,b,p)>=-epsilon &&
                    winding*cross(b,c,p)>=-epsilon &&
                    winding*cross(c,a,p)>=-epsilon) { occupied=true; break; }
            }
            if (occupied) continue;
            triangles[written++]={a,b,c};
            erase(i); removed=true; break;
        }
        if (!removed) return 0; // Degenerate or self-intersecting polygon.
    }
    if (size==3 && std::abs(cross(remaining[0],remaining[1],remaining[2]))>epsilon)
        triangles[written++]={remaining[0],remaining[1],remaining[2]};
    return written;
}

template <class Point>
inline std::vector<std::array<int,3>> TriangulatePolygon(
    const std::vector<Point>& points)
{
    std::vector<std::array<int,3>> result(points.size()<3 ? 0 : points.size()-2);
    result.resize(TriangulatePolygon(points.data(),points.size(),result.data()));
    return result;
}

// Expand coarse-face material assignments to the triangles emitted by
// TriangulatePolygon. This also keeps material-subset logic independent of
// Embree primitive ordering and easy to validate without a shader runtime.
inline std::vector<int> ExpandFaceAssignments(
    std::vector<int> const &triangleCoarseFaces,
    std::unordered_map<int, int> const &faceAssignments,
    int baseAssignment)
{
    std::vector<int> result;
    result.reserve(triangleCoarseFaces.size());
    for (int const face : triangleCoarseFaces) {
        auto const found = faceAssignments.find(face);
        result.push_back(found == faceAssignments.end()
            ? baseAssignment : found->second);
    }
    return result;
}
} // namespace prism::mesh
