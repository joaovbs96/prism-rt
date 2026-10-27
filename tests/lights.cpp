// Light sampling of the shared integrator (shaders/integrator/lights.h):
// for every analytic shape, the radiance integral over the sphere seen from
// a point must agree between light sampling and uniform direction sampling
// through intersectLight, and the density of a sampled direction must match
// the density intersectLight reports for it. Also checks the light table's
// alias distribution (also over a million entries) and the composite
// environment's density.

#include "../src/device/light_table.h"
#include "../shaders/integrator/path_tracer.h"
#include "../src/environment.h"
#include "../src/lights.h"

#include <cmath>
#include <cstdio>
#include <random>
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

LightRecord Record(prism::SceneLight const& light)
{
    LightRecord r{};
    r.info.z = kLightTextureNone;
    r.radiance = float4(light.radiance.x(), light.radiance.y(), light.radiance.z(), 0.0f);
    auto f4 = [](Float3 v, float w) { return float4(v.x(), v.y(), v.z(), w); };
    switch (light.shape) {
    case prism::LightShape::Sphere:
        r.info.x = kLightSphere;
        r.p0 = f4(light.position, light.radius);
        break;
    case prism::LightShape::Rect:
    case prism::LightShape::Disk:
        r.info.x = light.shape == prism::LightShape::Rect ? kLightRect : kLightDisk;
        r.p0 = f4(light.position, 0.0f);
        r.u = f4(light.axisU, 0.0f);
        r.v = f4(light.axisV, light.area);
        r.n = f4(light.normal, 0.0f);
        break;
    case prism::LightShape::Cylinder:
        r.info.x = kLightCylinder;
        r.p0 = f4(light.position, 0.0f);
        r.u = f4(light.axisU, 0.0f);
        r.v = f4(light.axisV, light.area);
        break;
    default:
        break;
    }
    return r;
}

std::array<double, 16> Transform(float tx, float ty, float tz, float scale = 1.0f, bool rotate = false)
{
    // Row-vector convention; optional rotation of 90 degrees about Y.
    std::array<double, 16> m{scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, tx, ty, tz, 1};
    if (rotate) {
        m[0] = 0; m[2] = -scale;
        m[8] = scale; m[10] = 0;
    }
    return m;
}

void CheckShape(char const* name, LightRecord const& light, float3 p)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    constexpr int samples = 4000000;
    double lightEstimate = 0.0, directionEstimate = 0.0;
    int pdfMismatches = 0, checked = 0;
    for (int i = 0; i < samples; ++i) {
        const LightSampleTextured s = sampleLight(light, p, float2(uniform(rng), uniform(rng)));
        if (s.sample.valid) {
            lightEstimate += s.sample.radiance.x / s.sample.pdf;
            Ray ray = makeRay(p, s.sample.wi);
            ray.tnear = 0.0f;
            const LightHitTextured h = intersectLight(light, ray, infinity(), p);
            ++checked;
            if (!h.hit.hit || std::abs(h.hit.pdf - s.sample.pdf) > 2e-2f * s.sample.pdf ||
                std::abs(h.hit.t - s.sample.distance) > 1e-3f * s.sample.distance)
                ++pdfMismatches;
        }
        const float y = 1.0f - 2.0f * uniform(rng);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = 2.0f * kPi * uniform(rng);
        Ray ray = makeRay(p, float3(r * std::cos(phi), y, r * std::sin(phi)));
        ray.tnear = 0.0f;
        const LightHitTextured h = intersectLight(light, ray, infinity(), p);
        if (h.hit.hit) directionEstimate += h.hit.radiance.x * 4.0 * kPi;
    }
    lightEstimate /= samples;
    directionEstimate /= samples;
    const double ratio = lightEstimate / directionEstimate;
    std::printf("%-9s light %.5f direction %.5f ratio %.4f pdf mismatches %d/%d\n", name, lightEstimate,
                directionEstimate, ratio, pdfMismatches, checked);
    Check(std::abs(ratio - 1.0) < 0.02, name);
    Check(pdfMismatches <= checked / 100, "sampled density equals the hit density");
}

} // namespace

// PathTracer::pickLight over a large table (a million emissive triangles and
// a dome, as in the OpenPBR playground): the realized selection frequencies
// must be the table's probabilities, which MIS assumes. Selection numbers
// lie on the sampler's 24-bit grid.
struct PickBackend {
    std::vector<float4> const* alias = nullptr;
    Rng rng = {12345u};
    uint lightCount() { return uint(alias->size()) - 1u; }
    float4 lightAlias(uint i) { return (*alias)[i]; }
};
struct PickHit {
    SurfaceHit surface;
};
struct PickShading {};

void CheckLargeTablePicks()
{
    const size_t n = 1000001;
    const double dome = 0.99;
    std::vector<double> pmf(n, (1.0 - dome) / double(n - 1));
    pmf[n - 1] = dome;
    const std::vector<float4> alias = prism::device::BuildAliasTable(pmf);
    PathTracer<PickBackend, PickHit, PickShading> tracer;
    tracer.backend.alias = &alias;
    std::mt19937 generator(7);
    const size_t draws = 4000000;
    size_t domePicks = 0, lowHalf = 0;
    for (size_t i = 0; i < draws; ++i) {
        const float u = (float(generator() >> 8) + 0.5f) / 16777216.0f;
        const LightPick pick = tracer.pickLight(u);
        if (pick.index == n - 1) ++domePicks;
        else if (pick.index < (n - 1) / 2) ++lowHalf;
    }
    const double domeFrequency = double(domePicks) / double(draws);
    const double lowFrequency = double(lowHalf) / double(draws);
    std::printf("large table: dome %.5f (table %.5f), first half of the triangles %.5f (table %.5f)\n",
                domeFrequency, dome, lowFrequency, 0.5 * (1.0 - dome));
    Check(std::abs(domeFrequency / dome - 1.0) < 1e-3, "large table: the dome is picked by its probability");
    Check(std::abs(lowFrequency / (0.5 * (1.0 - dome)) - 1.0) < 0.03,
          "large table: triangles are picked by their probability");
}

int main()
{
    CheckLargeTablePicks();
    const auto color = prism::color::ColorManagement::Create({});
    prism::LightParameters params;
    params.intensity = 2.0f;

    params.shape = prism::LightShape::Sphere;
    params.radius = 0.3f;
    CheckShape("sphere", Record(prism::MakeSceneLight(params, Transform(0, 1, 0), *color)), float3(0.2f, 0, 0.1f));

    params.shape = prism::LightShape::Rect;
    params.width = 1.5f;
    params.height = 0.5f;
    CheckShape("rect", Record(prism::MakeSceneLight(params, Transform(0, 0, 1.0f), *color)), float3(0.3f, 0.1f, 0));
    // Close enough to be sampled by solid angle.
    CheckShape("rect near", Record(prism::MakeSceneLight(params, Transform(0, 0, 0.2f), *color)), float3(0.3f, 0.1f, 0));

    params.shape = prism::LightShape::Disk;
    params.radius = 0.4f;
    CheckShape("disk", Record(prism::MakeSceneLight(params, Transform(0, 0, 1.0f, 1.5f), *color)),
               float3(0.1f, 0.2f, 0));

    params.shape = prism::LightShape::Cylinder;
    params.radius = 0.1f;
    params.length = 2.0f;
    CheckShape("cylinder", Record(prism::MakeSceneLight(params, Transform(0, 1.0f, 0), *color)), float3(0.3f, 0, 0.2f));
    CheckShape("cylinder", Record(prism::MakeSceneLight(params, Transform(0, 1.0f, 0, 1.0f, true), *color)),
               float3(0.3f, 0, 0.2f));
    CheckShape("cyl near", Record(prism::MakeSceneLight(params, Transform(0, 1.0f, 0), *color)), float3(0.7f, 0.8f, 0.05f));

    // Normalized lights keep their power when their size changes.
    params.shape = prism::LightShape::Rect;
    params.normalize = true;
    const auto narrowLight = prism::MakeSceneLight(params, Transform(0, 0, 0), *color);
    const auto wideLight = prism::MakeSceneLight(params, Transform(0, 0, 0, 3.0f), *color);
    Check(std::abs(prism::LightPower(narrowLight, 1.0f, *color) / prism::LightPower(wideLight, 1.0f, *color) - 1.0f) < 1e-4f,
          "normalize keeps the power");

    // Two constant domes: radiance sums, the mixture density integrates to one.
    ColorTexture a(Color3(1.0f)), b(Color3(3.0f));
    ConstantColorBackground domeA(&a), domeB(&b);
    CompositeEnvironment composite({&domeA, &domeB}, *color);
    Check(std::abs(composite.weights[0] - 0.25f) < 1e-3f, "domes are selected by power");
    Check(std::abs(composite.radiance(Float3(0, 1, 0)).x() - 4.0f) < 1e-4f, "dome radiance sums");
    Check(std::abs(composite.directionPdf(Float3(0, 1, 0)) * 4.0f * kPi - 1.0f) < 1e-4f,
          "mixture density");

    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("lights ok\n");
    return 0;
}
