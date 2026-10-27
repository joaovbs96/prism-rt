// Directional albedo of energy-compensated Oren-Nayar diffuse (EON:
// oren_nayar_diffuse_bsdf with energy_compensation, open_pbr_surface's base)
// through the full MaterialX -> MDL -> native pipeline, per channel. EON's
// albedo is rho E(mu) + rho_ms (1 - E(mu)), with E the uncompensated lobe's
// albedo for a white tint and rho_ms = rho^2 E_avg / (1 - rho (1 - E_avg))
// the multiple-scattering tint (OpenPBR; MaterialX's GLSL
// mx_oren_nayar_compensated_diffuse_dir_albedo). A white tint is a white
// furnace: albedo 1 at every angle. MaterialX's MDL library left the
// compensation lobe white (rho E + (1 - E)), which DocumentLoader repairs
// (docs/materialx_compatibility.md). -v prints every albedo.
#include "../src/mdl/context.h"
#include "../src/mdl_material.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include "../src/renderer/renderer_options.h"

namespace {
void require(bool condition, std::string const& message)
{
    if (!condition) throw std::runtime_error(message);
}

// A hit on the plane z = 0 (normal +z) from above, at `degrees` from the normal.
RayHit Hit(double degrees)
{
    const double theta = degrees * 3.14159265358979 / 180.0;
    const Float3 d = Float3(float(std::sin(theta)), 0.0f, float(-std::cos(theta))).normalize();
    RTCRayHit raw{};
    raw.ray.org_x = -d.x(); raw.ray.org_y = -d.y(); raw.ray.org_z = -d.z();
    raw.ray.dir_x = d.x(); raw.ray.dir_y = d.y(); raw.ray.dir_z = d.z();
    raw.ray.tfar = 1.0f;
    raw.hit.Ng_x = 0.0f; raw.hit.Ng_y = 0.0f; raw.hit.Ng_z = 1.0f;
    raw.hit.geomID = 0; raw.hit.primID = 0; raw.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    RayHit hit(raw, nullptr, RTC_GEOMETRY_TYPE_USER);
    hit.n_s = hit.n;
    hit.tangent = Float3(1.0f, 0.0f, 0.0f);
    hit.u = hit.v = 0.5f;
    return hit;
}

// The mean sampled weight per channel (reflection only).
std::array<double, 3> Albedo(MDLMaterial& material, double degrees, int count)
{
    const RayHit hit = Hit(degrees);
    std::mt19937 generator(7);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    Material::PreparedShadingContext context;
    require(material.prepare(hit, context), "prepare failed");
    context.hasXi = true;
    unsigned seed = 7;
    std::array<double, 3> sum{};
    for (int i = 0; i < count; ++i) {
        for (float& x : context.xi) x = uniform(generator);
        const BRDFSample b = material.samplePrepared(hit, seed, context);
        if (!b.scattered || b.null_event || dot(b.wi, hit.n_g) <= 0.0f) continue;
        for (int c = 0; c < 3; ++c) sum[c] += b.color[c];
    }
    for (double& s : sum) s /= count;
    return sum;
}

std::string Document(std::string const& surface)
{
    return "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n" + surface +
           "  <surfacematerial name=\"PrismMaterial\" type=\"material\">\n"
           "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Surface\" />\n"
           "  </surfacematerial>\n</materialx>\n";
}

std::string OrenNayar(std::string const& color, double roughness, bool compensated)
{
    return "  <oren_nayar_diffuse_bsdf name=\"Diffuse\" type=\"BSDF\">\n"
           "    <input name=\"color\" type=\"color3\" value=\"" + color + "\" />\n"
           "    <input name=\"roughness\" type=\"float\" value=\"" + std::to_string(roughness) + "\" />\n"
           "    <input name=\"energy_compensation\" type=\"boolean\" value=\"" + (compensated ? "true" : "false") +
           "\" />\n  </oren_nayar_diffuse_bsdf>\n"
           "  <surface name=\"Surface\" type=\"surfaceshader\">\n"
           "    <input name=\"bsdf\" type=\"BSDF\" nodename=\"Diffuse\" />\n  </surface>\n";
}

std::string OpenPbr(std::string const& color, double roughness)
{
    return "  <open_pbr_surface name=\"Surface\" type=\"surfaceshader\">\n"
           "    <input name=\"base_color\" type=\"color3\" value=\"" + color + "\" />\n"
           "    <input name=\"base_diffuse_roughness\" type=\"float\" value=\"" + std::to_string(roughness) + "\" />\n"
           "    <input name=\"specular_weight\" type=\"float\" value=\"0\" />\n  </open_pbr_surface>\n";
}
} // namespace

int main(int argc, char** argv)
{
    const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    const char* mdlPath = std::getenv("PRISM_MATERIALX_MDL");
    if (!libraries || !*libraries || !mdlPath || !*mdlPath) return 77;
    const bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    try {
        mdl::Context context;
        require(context.is_valid(), "MDL context initialization failed");
        const auto dir = std::filesystem::temp_directory_path() / "prism-eon-albedo";
        std::filesystem::create_directories(dir);
        int serial = 0;
        auto compile = [&](std::string const& surface) {
            const auto path = dir / ("eon_" + std::to_string(serial++) + ".mtlx");
            { std::ofstream out(path); out << Document(surface); }
            auto material = std::make_unique<MDLMaterial>(path.string(), context, libraries, mdlPath,
                prism::MaterialSettings{prism::color::ColorManagement::Create({}), prism::RendererOptions::FromEnvironment().CacheRoot()});
            require(material->is_valid(), "compile failed: " + path.string());
            return material;
        };
        const int count = 40000;
        const std::array<double, 3> rho{0.95, 0.5, 0.2};
        const std::string tint = "0.95, 0.5, 0.2";
        int failures = 0;
        for (double roughness : {0.5, 1.0}) {
            // E(mu): the uncompensated lobe's albedo for a white tint.
            auto single = compile(OrenNayar("1, 1, 1", roughness, false));
            auto white = compile(OrenNayar("1, 1, 1", roughness, true));
            auto tinted = compile(OrenNayar(tint, roughness, true));
            auto openPbr = compile(OpenPbr(tint, roughness));
            const double r = std::min(1.0, roughness);
            const double average = (1.0 + (2.0 / 3.0 - 28.0 / (15.0 * 3.14159265358979)) * r) /
                                   (1.0 + (0.5 - 2.0 / (3.0 * 3.14159265358979)) * r);
            for (double degrees : {0.0, 30.0, 60.0, 80.0}) {
                const double e = Albedo(*single, degrees, count)[0];
                const auto w = Albedo(*white, degrees, count);
                const bool furnace = std::abs(w[0] - 1.0) < 0.01;
                if (verbose || !furnace)
                    std::printf("roughness %.1f %2.0f deg: E %.4f, white compensated %.4f (furnace 1)%s\n", roughness,
                                degrees, e, w[0], furnace ? "" : "  FAIL");
                failures += !furnace;
                for (auto const& [name, material] : {std::pair{"oren_nayar", tinted.get()},
                                                     std::pair{"open_pbr", openPbr.get()}}) {
                    const auto a = Albedo(*material, degrees, count);
                    for (int c = 0; c < 3; ++c) {
                        const double ms = rho[c] * rho[c] * average / (1.0 - rho[c] * (1.0 - average));
                        const double expected = rho[c] * e + ms * (1.0 - e);
                        const bool ok = std::abs(a[c] - expected) <= 0.01 + 0.01 * expected;
                        if (verbose || !ok)
                            std::printf("%-10s roughness %.1f %2.0f deg channel %d (rho %.2f): albedo %.4f, EON %.4f "
                                        "(untinted compensation %.4f)%s\n",
                                        name, roughness, degrees, c, rho[c], a[c], expected, rho[c] * e + (1.0 - e),
                                        ok ? "" : "  FAIL");
                        failures += !ok;
                    }
                }
            }
        }
        std::filesystem::remove_all(dir);
        require(failures == 0, std::to_string(failures) + " albedos off");
    } catch (std::exception const& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("EON albedo ok\n");
    return 0;
}
