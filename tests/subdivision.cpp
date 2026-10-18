// Uniform subdivision (src/mesh/subdivision.h): refined topology, limit
// normals, creases and face-varying texture coordinates of a cube.

#include "../src/mesh/subdivision.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, char const* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

const std::vector<float> kPoints = {-0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
                                    -0.5f, 0.5f, -0.5f,  0.5f, 0.5f, -0.5f,  -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f};
const std::vector<int> kCounts = {4, 4, 4, 4, 4, 4};
const std::vector<int> kIndices = {0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4};

} // namespace

int main()
{
    using namespace prism::subdivision;
    Input input;
    input.points = kPoints;
    input.faceVertexCounts = kCounts;
    input.faceVertexIndices = kIndices;
    input.level = 2;

    // Smooth cube: 96 quads approaching a sphere, normals pointing outward.
    Output smooth = Refine(input);
    Check(smooth.valid, "refines");
    Check(smooth.faceVertexCounts.size() == 96, "6 * 4^2 faces");
    Check(RefinedFaceCount(kCounts, Scheme::CatmullClark, 2) == 96, "face count estimate");
    Check(smooth.coarseFaces.size() == 96 && smooth.coarseFaces.front() == 0 && smooth.coarseFaces.back() == 5,
          "coarse faces");
    bool outward = true, inside = true;
    for (size_t v = 0; v * 3 < smooth.points.size(); ++v) {
        const float* p = &smooth.points[3 * v];
        const float* n = &smooth.normals[3 * v];
        outward &= p[0] * n[0] + p[1] * n[1] + p[2] * n[2] > 0.0f;
        inside &= std::abs(p[0]) < 0.5f && std::abs(p[1]) < 0.5f && std::abs(p[2]) < 0.5f;
    }
    Check(outward, "limit normals point outward");
    Check(inside, "smooth limit lies inside the cage");

    // Infinitely sharp creases on every edge keep the cube.
    std::vector<int> creaseIndices;
    std::vector<int> creaseLengths;
    for (size_t f = 0; f < kCounts.size(); ++f) {
        for (int k = 0; k < 4; ++k) {
            creaseIndices.push_back(kIndices[4 * f + k]);
            creaseIndices.push_back(kIndices[4 * f + (k + 1) % 4]);
            creaseLengths.push_back(2);
        }
    }
    std::vector<float> creaseSharpnesses(creaseLengths.size(), 10.0f);
    input.creaseIndices = creaseIndices;
    input.creaseLengths = creaseLengths;
    input.creaseSharpnesses = creaseSharpnesses;
    Output sharp = Refine(input);
    float extent = 0.0f;
    for (float c : sharp.points) extent = std::max(extent, std::abs(c));
    Check(sharp.valid && std::abs(extent - 0.5f) < 1e-4f, "sharp creases keep the corners");

    // Face-varying coordinates: one quad per face with its own (0..1) square.
    std::vector<float> uvs;
    for (size_t f = 0; f < kCounts.size(); ++f)
        uvs.insert(uvs.end(), {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f});
    input.creaseIndices = {};
    input.creaseLengths = {};
    input.creaseSharpnesses = {};
    input.uvs = uvs;
    input.faceVarying = FaceVaryingInterpolation::All;
    Output textured = Refine(input);
    bool inUnitSquare = textured.uvs.size() == textured.faceVertexIndices.size() * 2;
    for (float c : textured.uvs) inUnitSquare &= c >= -1e-5f && c <= 1.0f + 1e-5f;
    Check(inUnitSquare, "linear face-varying coordinates stay in each face's square");

    // Loop needs triangles: quads fall back to Catmull-Clark.
    input.uvs = {};
    input.scheme = Scheme::Loop;
    Check(Refine(input).faceVertexCounts.size() == 96, "loop on quads falls back");

    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("subdivision ok\n");
    return 0;
}
