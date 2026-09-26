#pragma once
#include <pxr/base/gf/vec3d.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace prism::hydra {

// Return original face-corner indices, preserving winding and face-varying data.
// Project onto the dominant Newell-normal plane and clip ears so concave faces
// do not produce triangles outside their boundary. Convex faces keep fan order.
inline std::vector<std::array<int,3>> TriangulatePolygon(
    const std::vector<pxr::GfVec3d>& points)
{
    std::vector<std::array<int,3>> result;
    if (points.size() < 3) return result;
    pxr::GfVec3d normal(0);
    for (size_t i=0;i<points.size();++i) {
        const auto& a=points[i]; const auto& b=points[(i+1)%points.size()];
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
    for (const auto& p:points) scale=std::max(scale,(p-points.front()).GetLengthSq());
    const double epsilon=std::max(scale*1e-12,1e-30);
    double area=0;
    for (size_t i=1;i+1<points.size();++i) area+=cross(0,int(i),int(i+1));
    if (std::abs(area)<=epsilon) return result;
    const double winding=area>0?1.0:-1.0;
    std::vector<int> remaining(points.size());
    std::iota(remaining.begin(),remaining.end(),0);
    while (remaining.size()>3) {
        bool removed=false;
        for (size_t step=0;step<remaining.size();++step) {
            const size_t i=(step+1)%remaining.size();
            const int a=remaining[(i+remaining.size()-1)%remaining.size()];
            const int b=remaining[i], c=remaining[(i+1)%remaining.size()];
            const double turn=winding*cross(a,b,c);
            if (std::abs(turn)<=epsilon) {
                remaining.erase(remaining.begin()+i); removed=true; break;
            }
            if (turn<0) continue;
            bool occupied=false;
            for (int p:remaining) {
                if (p==a || p==b || p==c) continue;
                if (winding*cross(a,b,p)>=-epsilon &&
                    winding*cross(b,c,p)>=-epsilon &&
                    winding*cross(c,a,p)>=-epsilon) { occupied=true; break; }
            }
            if (occupied) continue;
            result.push_back({a,b,c});
            remaining.erase(remaining.begin()+i); removed=true; break;
        }
        if (!removed) return {}; // Degenerate or self-intersecting polygon.
    }
    if (remaining.size()==3 && std::abs(cross(remaining[0],remaining[1],remaining[2]))>epsilon)
        result.push_back({remaining[0],remaining[1],remaining[2]});
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
} // namespace prism::hydra
