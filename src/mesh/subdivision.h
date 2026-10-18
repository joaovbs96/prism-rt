#pragma once

// Uniform subdivision-surface refinement (OpenSubdiv Far) of a polygon mesh,
// with USD's subdivision tags. The result is a polygon mesh of the limit
// surface: limit positions and normals per vertex, face-varying texture
// coordinates per corner, and the coarse face of every refined face.

#include <cstddef>
#include <span>
#include <vector>

namespace prism::subdivision {

enum class Scheme { CatmullClark, Loop, Bilinear };
enum class BoundaryInterpolation { None, EdgeOnly, EdgeAndCorner };
enum class FaceVaryingInterpolation { None, CornersOnly, CornersPlus1, CornersPlus2, Boundaries, All };

struct Input {
    Scheme scheme = Scheme::CatmullClark;
    std::span<const int> faceVertexCounts;
    std::span<const int> faceVertexIndices;
    std::span<const float> points;          // xyz per point
    std::span<const int> holes;             // coarse face indices
    std::span<const int> creaseIndices;
    std::span<const int> creaseLengths;
    std::span<const float> creaseSharpnesses; // per crease, or per crease edge
    std::span<const int> cornerIndices;
    std::span<const float> cornerSharpnesses;
    BoundaryInterpolation boundary = BoundaryInterpolation::EdgeAndCorner;
    FaceVaryingInterpolation faceVarying = FaceVaryingInterpolation::CornersPlus1;
    bool smoothTriangles = false;           // triangleSubdivisionRule "smooth"
    // Texture coordinates (xy) per face-vertex corner, or per point when
    // uvPerPoint; empty for none. Equal values of corners sharing a point are
    // shared in the refinement.
    std::span<const float> uvs;
    bool uvPerPoint = false;
    int level = 2;
};

struct Output {
    std::vector<int> faceVertexCounts;
    std::vector<int> faceVertexIndices;
    std::vector<float> points;   // xyz, limit positions
    std::vector<float> normals;  // xyz per point, limit normals (right-handed winding)
    std::vector<float> uvs;      // xy per corner of faceVertexIndices
    std::vector<int> coarseFaces; // per refined face
    bool valid = false;
};

Output Refine(Input const& input);

// Faces after `level` uniform refinements of a mesh with these face sizes
// (quads: 4^level per quad, n-gons split into n quads first).
size_t RefinedFaceCount(std::span<const int> faceVertexCounts, Scheme scheme, int level);

} // namespace prism::subdivision
