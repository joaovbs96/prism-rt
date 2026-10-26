// UsdLuxShapingAPI: spotlight cones, softness, focus and IES profiles
// (shaders/integrator/lights.h LightShaping, src/lights/ies.h).
//
// prism_light_shaping_test                 unit checks: the LM-63 parser and its
//                                          symmetries, the shaping formulas,
//                                          the light table's power estimate
// prism_light_shaping_test spotlight IMAGE  spotlight.usda rendered: every pixel
//                                          against the analytic irradiance
// prism_light_shaping_test ies IMAGE FILE   ies_light.usda rendered with FILE
//                                          (its profile): every pixel against
//                                          an independent interpolation of FILE

#include "../src/device/light_table.h"
#include "../src/lights.h"
#include "../src/lights/ies.h"

#include <OpenImageIO/imagebuf.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
constexpr double kPiD = 3.14159265358979323846;

void Check(bool ok, std::string const& what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

double Radians(double degrees) { return degrees * kPiD / 180.0; }

// --- unit checks ---------------------------------------------------------

std::string Profile(std::string const& horizontal, int horizontalCount, std::string const& values)
{
    return "IESNA:LM-63-2002\n[TEST] symmetry\nTILT=NONE\n1 -1 1 3 " + std::to_string(horizontalCount) +
           " 1 2 0 0 0\n1 1 0\n0 45 90\n" + horizontal + "\n" + values + "\n";
}

void CheckParser()
{
    // Rotational symmetry: one horizontal angle.
    {
        const prism::IesProfile p = prism::ParseIesProfile(Profile("0", 1, "10 20 30"), "rotational");
        Check(p.width == 1 && std::abs(p.peak - 30.0f) < 1e-5f, "rotational profile: one column, peak 30");
        Check(std::abs(p.sample(float(Radians(22.5)), 1.0f) * p.peak - 15.0f) < 1e-3f,
              "rotational profile: linear between vertical angles");
        Check(p.sample(float(Radians(92.0)), 0.0f) == 0.0f, "zero a grid step beyond the last vertical angle");
    }
    // Quadrant symmetry (0..90): I(phi) = I(180 - phi) = I(180 + phi) = I(360 - phi).
    {
        const prism::IesProfile p = prism::ParseIesProfile(Profile("0 90", 2, "1 1 1\n3 3 3"), "quadrant");
        auto at = [&](double phi) { return p.sample(float(Radians(45.0)), float(Radians(phi))) * p.peak; };
        Check(std::abs(at(45) - 2.0f) < 1e-3f && std::abs(at(135) - 2.0f) < 1e-3f &&
              std::abs(at(225) - 2.0f) < 1e-3f && std::abs(at(315) - 2.0f) < 1e-3f &&
              std::abs(at(90) - 3.0f) < 1e-3f && std::abs(at(270) - 3.0f) < 1e-3f &&
              std::abs(at(180) - 1.0f) < 1e-3f, "quadrant symmetry");
    }
    // Bilateral symmetry about the 0-180 plane: I(phi) = I(360 - phi).
    {
        const prism::IesProfile p = prism::ParseIesProfile(Profile("0 90 180", 3, "1 1 1\n2 2 2\n5 5 5"), "bilateral");
        auto at = [&](double phi) { return p.sample(float(Radians(45.0)), float(Radians(phi))) * p.peak; };
        Check(std::abs(at(270) - 2.0f) < 1e-3f && std::abs(at(225) - 3.5f) < 1e-3f && std::abs(at(180) - 5.0f) < 1e-3f,
              "symmetry about the 0-180 plane");
    }
    // Symmetry about the 90-270 plane: I(phi) = I(180 - phi).
    {
        const prism::IesProfile p = prism::ParseIesProfile(Profile("90 180 270", 3, "2 2 2\n4 4 4\n6 6 6"), "90-270");
        auto at = [&](double phi) { return p.sample(float(Radians(45.0)), float(Radians(phi))) * p.peak; };
        Check(std::abs(at(0) - 4.0f) < 1e-3f && std::abs(at(45) - 3.0f) < 1e-3f && std::abs(at(300) - 5.333333f) < 1e-3f,
              "symmetry about the 90-270 plane");
    }
    // Full profile with an incomplete range wraps over 360 degrees.
    {
        const prism::IesProfile p =
            prism::ParseIesProfile(Profile("0 120 240", 3, "0 0 0\n3 3 3\n6 6 6"), "wrap");
        auto at = [&](double phi) { return p.sample(float(Radians(45.0)), float(Radians(phi))) * p.peak; };
        Check(std::abs(at(300) - 3.0f) < 1e-3f && std::abs(at(60) - 1.5f) < 1e-3f, "full profile wraps at 360");
    }
    // Commas and a TILT=INCLUDE block.
    {
        const std::string text = "IESNA:LM-63-1995\nTILT=INCLUDE\n1\n2\n0 90\n1 1\n"
                                 "1,-1,2,3,1,1,2,0,0,0\n1,1,0\n0,45,90\n0\n1,2,3\n";
        const prism::IesProfile p = prism::ParseIesProfile(text, "include");
        Check(std::abs(p.peak - 6.0f) < 1e-5f, "TILT=INCLUDE and commas; the candela multiplier applies");
    }
    // Malformed files throw; missing ones load as null with a warning.
    bool threw = false;
    try {
        prism::ParseIesProfile("IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 3 1 1 2 0 0\n", "short");
    } catch (std::exception const&) {
        threw = true;
    }
    Check(threw, "a truncated profile is rejected");
    Check(prism::LoadIesProfile("does/not/exist.ies") == nullptr, "a missing profile loads as null");
}

// The profile as the schema samples it: bilinear in the file's angles.
struct ReferenceProfile {
    std::vector<double> vertical, horizontal, candela;
    double multiplier = 1.0;
    double mean = 0.0;

    bool Load(std::string const& path)
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
            if (line.rfind("TILT", 0) == 0) break;
        std::vector<double> n;
        double value;
        while (file >> value) n.push_back(value);
        if (n.size() < 13) return false;
        multiplier = n[2];
        const size_t nv = size_t(n[3]), nh = size_t(n[4]);
        size_t k = 13;
        if (n.size() < k + nv + nh + nv * nh) return false;
        vertical.assign(n.begin() + k, n.begin() + k + nv);
        k += nv;
        horizontal.assign(n.begin() + k, n.begin() + k + nh);
        k += nh;
        candela.assign(n.begin() + k, n.begin() + k + nv * nh);
        // Mean over the sphere of the interpolant (fine midpoint rule).
        double sum = 0.0;
        const int steps = 1440;
        for (int t = 0; t < steps; ++t) {
            const double theta = 180.0 * (t + 0.5) / steps;
            double ring = 0.0;
            for (int f = 0; f < 360; ++f) ring += At(theta, f + 0.5) / 360.0;
            sum += ring * std::sin(Radians(theta)) * Radians(180.0 / steps);
        }
        mean = 0.5 * sum;
        return true;
    }

    // Full 0..360 profile (as cosine_lobe.ies), degrees. Past its last
    // vertical angle it falls linearly to zero over one degree: the step of
    // the grid the renderer resamples it to (src/lights/ies.h).
    double At(double theta, double phi) const
    {
        if (theta > vertical.back() && theta < vertical.back() + 1.0)
            return At(vertical.back(), phi) * (vertical.back() + 1.0 - theta);
        if (theta < vertical.front() || theta > vertical.back()) return 0.0;
        auto locate = [](std::vector<double> const& x, double at, size_t* i, double* t) {
            size_t k = 0;
            while (k + 2 < x.size() && at > x[k + 1]) ++k;
            *i = k;
            *t = std::clamp((at - x[k]) / (x[k + 1] - x[k]), 0.0, 1.0);
        };
        size_t v, h;
        double tv, th;
        locate(vertical, theta, &v, &tv);
        locate(horizontal, std::fmod(phi + 360.0, 360.0), &h, &th);
        auto c = [&](size_t hi, size_t vi) { return candela[hi * vertical.size() + vi] * multiplier; };
        const double a = c(h, v) * (1 - tv) + c(h, v + 1) * tv;
        const double b = c(h + 1, v) * (1 - tv) + c(h + 1, v + 1) * tv;
        return a * (1 - th) + b * th;
    }
};

void CheckCosineLobe(std::string const& file)
{
    const auto profile = prism::LoadIesProfile(file);
    Check(profile != nullptr, "cosine_lobe.ies loads");
    if (!profile) return;
    Check(profile->width == prism::IesProfile::kIesColumns, "cosine_lobe.ies varies with phi");
    Check(std::abs(profile->peak - 1.5f) < 1e-4f, "cosine_lobe.ies peak 1.5 cd after the multiplier");
    // At the file's angles the grid is exact.
    const float at = profile->sample(float(Radians(30.0)), float(Radians(60.0))) * profile->peak;
    const double expected = std::cos(Radians(30.0)) * (1.0 + 0.5 * std::cos(Radians(60.0)));
    Check(std::abs(at - expected) < 1e-4, "cosine_lobe.ies at (30, 60) degrees");
    // Its mean over the sphere: 0.1875 for the exact lobe, a little less for
    // the linear interpolation of its samples, plus the grid step past 60
    // degrees over which it falls to zero (ReferenceProfile).
    ReferenceProfile reference;
    reference.Load(file);
    std::printf("cosine_lobe.ies mean %.5f, reference %.5f, exact lobe 0.1875\n", profile->mean, reference.mean);
    Check(std::abs(profile->mean / reference.mean - 1.0) < 2e-3, "cosine_lobe.ies mean intensity");
}

LightShaping Shaping(float coneDegrees, float softness, float focus, float tint)
{
    LightShaping s{};
    s.axis = float3(0.0f, 0.0f, -1.0f);
    s.xAxis = float3(1.0f, 0.0f, 0.0f);
    s.cutoff = coneDegrees >= 180.0f ? 4.0f : float(Radians(coneDegrees));
    s.smoothStart = s.cutoff * (1.0f - softness);
    s.focus = focus;
    s.tint = float3(tint);
    return s;
}

void CheckFormulas()
{
    // Cone and softness: 1 - smoothstep(smoothStart, cutoff, theta).
    const LightShaping soft = Shaping(40.0f, 0.5f, 0.0f, 0.0f);
    const float t = 0.5f; // theta = 30 degrees: halfway between 20 and 40
    const float3 e(std::sin(float(Radians(30.0))), 0.0f, -std::cos(float(Radians(30.0))));
    Check(std::abs(shapingConeFocus(soft, e).x - (1.0f - t * t * (3.0f - 2.0f * t))) < 1e-4f, "softness smoothstep");
    Check(shapingConeFocus(Shaping(29.0f, 0.0f, 0.0f, 0.0f), e).x == 0.0f, "zero outside the cone");
    Check(shapingConeFocus(Shaping(31.0f, 0.0f, 0.0f, 0.0f), e).x == 1.0f, "one inside a hard cone");
    // Focus: lerp(tint, 1, |cos|^focus).
    const float f = std::pow(std::cos(float(Radians(30.0))), 3.0f);
    Check(std::abs(shapingConeFocus(Shaping(180.0f, 0.0f, 3.0f, 0.25f), e).x - (0.25f + 0.75f * f)) < 1e-5f,
          "focus and focus tint");
    // Behind the light (sphere lights): |cos| makes focus symmetric.
    const float3 back(std::sin(float(Radians(30.0))), 0.0f, std::cos(float(Radians(30.0))));
    Check(std::abs(shapingConeFocus(Shaping(180.0f, 0.0f, 3.0f, 0.0f), back).x - f) < 1e-5f, "focus behind the light");
    // Angle scale (the schema's applyAngleScale).
    LightShaping s = Shaping(180.0f, 0.0f, 0.0f, 0.0f);
    s.angleScale = 0.5f;
    Check(std::abs(iesAngles(s, e).x - float(Radians(60.0))) < 1e-5f, "positive angle scale divides theta");
    s.angleScale = -0.5f;
    Check(std::abs(iesAngles(s, e).x - float(kPiD + (Radians(30.0) - kPiD) / 0.5)) < 1e-4f,
          "negative angle scale scales about theta = pi");
    // phi: from local +X toward local +Y.
    s.angleScale = 0.0f;
    const float3 towardY(0.0f, std::sin(float(Radians(30.0))), -std::cos(float(Radians(30.0))));
    Check(std::abs(iesAngles(s, towardY).y - float(kPiD / 2.0)) < 1e-5f, "phi 90 degrees along local +Y");
    // The importance bound is zero only where no point of the light reaches.
    const LightShaping hard = Shaping(30.0f, 0.0f, 0.0f, 0.0f);
    const float3 center(0.0f, 0.0f, 0.0f);
    const float3 outside(std::sin(float(Radians(40.0))), 0.0f, -std::cos(float(Radians(40.0))));
    Check(shapingImportance(hard, outside, center, 0.0f) == 0.0f, "point light: no importance outside its cone");
    Check(shapingImportance(hard, outside, center, 0.3f) > 0.0f, "area light: importance where its edge reaches");
}

void CheckPower()
{
    // A point light with a 60 degree cone emits (1 - cos 60) / 2 of its power.
    prism::LightParameters params;
    params.shape = prism::LightShape::Sphere;
    params.treatAsPoint = true;
    params.shaping.active = true;
    params.shaping.coneAngle = 60.0f;
    const std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const prism::SceneLight light = prism::MakeSceneLight(params, identity);
    const double fraction = prism::device::ShapingPowerFraction(light);
    std::printf("60 degree cone: power fraction %.5f (exact 0.25)\n", fraction);
    Check(std::abs(fraction / 0.25 - 1.0) < 0.01, "power of a 60 degree cone");
}

// --- rendered checks -----------------------------------------------------

struct Image {
    int width = 0, height = 0, channels = 0;
    std::vector<float> pixels;
    bool Load(std::string const& path)
    {
        OIIO::ImageBuf buffer(path);
        if (!buffer.read(0, 0, true, OIIO::TypeDesc::FLOAT)) return false;
        auto const& spec = buffer.spec();
        width = spec.width;
        height = spec.height;
        channels = spec.nchannels;
        pixels.resize(size_t(width) * height * channels);
        return buffer.get_pixels(OIIO::ROI(spec.x, spec.x + width, spec.y, spec.y + height, 0, 1, 0, channels),
                                 OIIO::TypeDesc::FLOAT, pixels.data());
    }
    double Gray(int x, int y) const
    {
        const float* p = &pixels[(size_t(y) * width + x) * channels];
        return (double(p[0]) + p[1] + p[2]) / 3.0;
    }
};

// Floor point of pixel (i, j) at sub-pixel (u, v): x in [-8, 8], z in [-2, 2].
void FloorPoint(Image const& image, double i, double j, double* x, double* z)
{
    *x = -8.0 + i * 16.0 / image.width;
    *z = -2.0 + j * 4.0 / image.height;
}

struct Region {
    std::string name;
    double cx;                                           // light at (cx, 1, 0)
    std::function<double(double dx, double dz)> value;   // pixel value at a floor point
    std::function<bool(double dx, double dz)> skip;      // near a discontinuity
};

// Pixels within `radius` of each light: the box average of `value` over the
// pixel (8 x 8 points) against the render, except where `skip` holds at the
// pixel's centre (within about 3 pixels of a discontinuity).
void CheckRegions(Image const& image, std::vector<Region> const& regions, double radius, double tolerance)
{
    for (Region const& r : regions) {
        double peak = 0.0, sumError = 0.0, maxError = 0.0, maxDark = 0.0;
        int count = 0, dark = 0;
        double worstX = 0, worstZ = 0, worstRender = 0, worstExpected = 0;
        for (int j = 0; j < image.height; ++j) {
            for (int i = 0; i < image.width; ++i) {
                double x, z;
                FloorPoint(image, i + 0.5, j + 0.5, &x, &z);
                if (std::hypot(x - r.cx, z) > radius) continue;
                double expected = 0.0;
                bool skip = false;
                for (int sv = 0; sv < 8; ++sv)
                    for (int su = 0; su < 8; ++su) {
                        double px, pz;
                        FloorPoint(image, i + (su + 0.5) / 8.0, j + (sv + 0.5) / 8.0, &px, &pz);
                        expected += r.value(px - r.cx, pz) / 64.0;
                    }
                skip = r.skip(x - r.cx, z);
                const double rendered = image.Gray(i, j);
                peak = std::max(peak, expected);
                if (skip) continue;
                if (expected == 0.0) {
                    ++dark;
                    maxDark = std::max(maxDark, std::abs(rendered));
                    continue;
                }
                const double error = std::abs(rendered - expected) / expected;
                if (expected < 0.02) continue; // the dim fringe: relative error is noise
                ++count;
                sumError += error;
                if (error > maxError) {
                    maxError = error;
                    worstX = x;
                    worstZ = z;
                    worstRender = rendered;
                    worstExpected = expected;
                }
            }
        }
        const double meanError = count ? sumError / count : 1.0;
        std::printf("%-11s pixels %d (dark %d): mean relative error %.5f, max %.5f at (%.3f, %.3f) "
                    "render %.5f expected %.5f; peak %.4f; dark max %.3g\n",
                    r.name.c_str(), count, dark, meanError, maxError, worstX, worstZ, worstRender, worstExpected,
                    peak, maxDark);
        Check(count > 100 && meanError < tolerance / 4.0 && maxError < tolerance, r.name + ": lit pixels");
        Check(maxDark < 1e-6, r.name + ": pixels outside the light's reach are black");
    }
}

// Point light 1 unit above the floor, intensity 100, radius 0.1 (treatAsPoint:
// I = 100 pi 0.01 = pi), floor albedo 0.5: pixel = 0.5 / pi * pi * S * cos / d^2.
double PointIrradiance(double dx, double dz, double shaping)
{
    const double d2 = 1.0 + dx * dx + dz * dz;
    const double cosTheta = 1.0 / std::sqrt(d2);
    return 0.5 * shaping * cosTheta / d2;
}

double ThetaDegrees(double dx, double dz) { return std::atan(std::hypot(dx, dz)) * 180.0 / kPiD; }

void CheckSpotlight(Image const& image)
{
    auto smooth = [](double start, double cutoff, double theta) {
        if (theta > cutoff) return 0.0;
        if (theta <= start) return 1.0;
        const double t = (theta - start) / (cutoff - start);
        return 1.0 - t * t * (3.0 - 2.0 * t);
    };
    std::vector<Region> regions = {
        {"hard", -5.0,
         [](double dx, double dz) { return ThetaDegrees(dx, dz) <= 30.0 ? PointIrradiance(dx, dz, 1.0) : 0.0; },
         [](double dx, double dz) { return std::abs(std::hypot(dx, dz) - std::tan(Radians(30.0))) < 0.1; }},
        {"soft", 0.0,
         [&](double dx, double dz) { return PointIrradiance(dx, dz, smooth(20.0, 40.0, ThetaDegrees(dx, dz))); },
         [](double, double) { return false; }},
        {"focus", 5.0,
         [](double dx, double dz) {
             const double theta = ThetaDegrees(dx, dz);
             if (theta > 50.0) return 0.0;
             const double f = std::pow(std::cos(Radians(theta)), 3.0);
             return PointIrradiance(dx, dz, 0.25 + 0.75 * f);
         },
         [](double dx, double dz) { return std::abs(std::hypot(dx, dz) - std::tan(Radians(50.0))) < 0.1; }},
    };
    CheckRegions(image, regions, 1.9, 0.02);
}

void CheckIes(Image const& image, std::string const& file)
{
    ReferenceProfile profile;
    if (!profile.Load(file)) {
        Check(false, "reading " + file);
        return;
    }
    std::printf("reference mean %.5f\n", profile.mean);
    const double lastAngle = profile.vertical.back();
    // Raw at x = -4: local +X = world +X, local +Y = world -Z.
    // Normalized at x = 4: local +X = world -Z, local +Y = world -X; angle scale 0.7.
    std::vector<Region> regions = {
        {"raw", -4.0,
         [&](double dx, double dz) {
             const double phi = std::atan2(-dz, dx) * 180.0 / kPiD;
             return PointIrradiance(dx, dz, profile.At(ThetaDegrees(dx, dz), phi));
         },
         [&](double dx, double dz) { return std::abs(ThetaDegrees(dx, dz) - lastAngle) < 2.0; }},
        {"normalized", 4.0,
         [&](double dx, double dz) {
             const double phi = std::atan2(-dx, -dz) * 180.0 / kPiD;
             return PointIrradiance(dx, dz, profile.At(ThetaDegrees(dx, dz) / 0.7, phi) / profile.mean);
         },
         [&](double dx, double dz) { return std::abs(ThetaDegrees(dx, dz) / 0.7 - lastAngle) < 2.5; }},
    };
    CheckRegions(image, regions, 2.5, 0.02);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "spotlight" && argc == 3) {
        Image image;
        if (!image.Load(argv[2])) {
            std::printf("FAIL: cannot read %s\n", argv[2]);
            return 1;
        }
        CheckSpotlight(image);
    } else if (mode == "ies" && argc == 4) {
        Image image;
        if (!image.Load(argv[2])) {
            std::printf("FAIL: cannot read %s\n", argv[2]);
            return 1;
        }
        CheckIes(image, argv[3]);
    } else if (mode == "unit" && argc == 3) {
        CheckParser();
        CheckCosineLobe(argv[2]);
        CheckFormulas();
        CheckPower();
    } else {
        std::printf("usage: prism_light_shaping_test unit COSINE_LOBE.ies | spotlight IMAGE | ies IMAGE FILE\n");
        return 2;
    }
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("light shaping ok\n");
    return 0;
}
