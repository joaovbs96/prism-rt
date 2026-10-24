// Directional albedo (reflection + transmission) of smooth, non-absorbing
// dielectrics through the full MaterialX -> MDL -> native pipeline, sampled at
// a flat surface from outside (front face) and inside (back face). A smooth
// dielectric must return all the energy at every angle: R + T = 1, with
// R = Fresnel(theta) (1 under total internal reflection).
#include "../src/mdl/context.h"
#include "../src/mdl_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, std::string const& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Document(std::string const& body)
{
    return "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n" + body +
           "  <surfacematerial name=\"PrismMaterial\" type=\"material\">\n"
           "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Surface\" />\n"
           "  </surfacematerial>\n</materialx>\n";
}

std::string Shader(std::string const& node, std::string const& inputs)
{
    return "  <" + node + " name=\"Surface\" type=\"surfaceshader\">\n" + inputs + "  </" + node + ">\n";
}

std::string Input(std::string const& name, std::string const& type, std::string const& value)
{
    return "    <input name=\"" + name + "\" type=\"" + type + "\" value=\"" + value + "\" />\n";
}

// A hit on the plane z = 0 (normal +z) along d; d.z > 0 hits the back face.
RayHit Hit(Float3 d)
{
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

// Unpolarized Fresnel reflectance; cosI from the incident side, eta = n_t / n_i.
double Fresnel(double cosI, double eta)
{
    const double sin2T = (1.0 - cosI * cosI) / (eta * eta);
    if (sin2T >= 1.0) return 1.0;
    const double cosT = std::sqrt(1.0 - sin2T);
    const double rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    const double rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

struct Albedo {
    double r = 0.0, t = 0.0;
    int nr = 0, nt = 0, delta = 0;
    double spread = 0.0; // largest angle (degrees) of a reflection from the mirror direction
};

Albedo Measure(MDLMaterial& material, RayHit const& hit, int count)
{
    Albedo a;
    // Stratified-free but well distributed sample points (the renderer's LCG
    // correlates consecutive dimensions).
    std::mt19937 generator(11);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    Material::PreparedShadingContext context;
    require(material.prepare(hit, context), "prepare failed");
    context.hasXi = true;
    unsigned seed = 11;
    for (int i = 0; i < count; ++i) {
        for (float& x : context.xi) x = uniform(generator);
        const BRDFSample b = material.samplePrepared(hit, seed, context);
        if (!b.scattered || b.null_event) continue;
        const double w = (double(b.color.x()) + b.color.y() + b.color.z()) / 3.0;
        const bool reflected = dot(b.wi, hit.n_g) >= 0.0f;
        (reflected ? a.r : a.t) += w;
        ++(reflected ? a.nr : a.nt);
        a.delta += b.delta;
        if (reflected) {
            const Float3 d = hit.d.normalize(), m = d - hit.n_g * (2.0f * dot(d, hit.n_g));
            a.spread = std::max(a.spread, std::acos(std::min(1.0, double(dot(m, b.wi.normalize())))) * 57.2958);
        }
    }
    a.r /= count;
    a.t /= count;
    return a;
}
} // namespace

int main(int argc, char** argv)
{
    const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    const char* mdlPath = std::getenv("PRISM_MATERIALX_MDL");
    if (!libraries || !*libraries || !mdlPath || !*mdlPath) return 77;
    // -v [case]: print every albedo (of one case).
    const bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    const char* only = verbose && argc > 2 ? argv[2] : nullptr;
    try {
        mdl::Context context;
        require(context.is_valid(), "MDL context initialization failed");
        const auto dir = std::filesystem::temp_directory_path() / "prism-dielectric-albedo";
        std::filesystem::create_directories(dir);
        const std::string glass = Input("base_weight", "float", "0") +
                                  Input("specular_roughness", "float", "0") +
                                  Input("specular_ior", "float", "1.5") +
                                  Input("transmission_weight", "float", "1");
        // check: R + T must be 1 (energy) and R must be the exact Fresnel
        // reflectance (within sampling noise); otherwise the case is only
        // reported with -v.
        struct Case {
            const char* name;
            std::string body;
            bool energy, fresnel;
        };
        const std::string dielectric =
            "  <dielectric_bsdf name=\"Bsdf\" type=\"BSDF\">\n" + Input("ior", "float", "1.5") +
            Input("roughness", "vector2", "0, 0") + Input("scatter_mode", "string", "RT") +
            "  </dielectric_bsdf>\n  <surface name=\"Surface\" type=\"surfaceshader\">\n"
            "    <input name=\"bsdf\" type=\"BSDF\" nodename=\"Bsdf\" />\n  </surface>\n";
        const std::string rough = Input("base_weight", "float", "0") + Input("specular_roughness", "float", "0.3") +
                                  Input("specular_ior", "float", "1.5") + Input("transmission_weight", "float", "1");
        const std::string coat = Input("coat_weight", "float", "1") + Input("coat_roughness", "float", "0");
        const Case cases[] = {
            {"dielectric_rt", dielectric, true, true},
            {"open_pbr", Shader("open_pbr_surface", glass), true, true},
            {"open_pbr_depth",
             Shader("open_pbr_surface", glass + Input("transmission_color", "color3", "0.5, 0.8, 0.9") +
                                            Input("transmission_depth", "float", "1")),
             true, true},
            {"open_pbr_rough", Shader("open_pbr_surface", rough), false, false},
            {"standard_surface",
             Shader("standard_surface", Input("base", "float", "0") + Input("specular_roughness", "float", "0") +
                                            Input("specular_IOR", "float", "1.5") + Input("transmission", "float", "1")),
             true, true},
            {"gltf_pbr",
             Shader("gltf_pbr", Input("metallic", "float", "0") + Input("roughness", "float", "0") +
                                    Input("ior", "float", "1.5") + Input("transmission", "float", "1")),
             true, true},
            // Not repaired (docs/materialx_compatibility.md): coat darkening
            // and the coat's own Schlick layer over the refracting base.
            {"open_pbr_coat", Shader("open_pbr_surface", glass + coat), false, false},
            {"open_pbr_coat_no_darkening",
             Shader("open_pbr_surface", glass + coat + Input("coat_darkening", "float", "0")), false, false},
        };
        const int count = 20000;
        const double angles[] = {0.0, 20.0, 40.0, 60.0, 75.0, 85.0};
        int failures = 0;
        for (auto const& test : cases) {
            if (only && std::strcmp(only, test.name) != 0) continue;
            const auto path = dir / (std::string(test.name) + ".mtlx");
            { std::ofstream out(path); out << Document(test.body); }
            MDLMaterial material(path.string(), context, libraries, mdlPath);
            require(material.is_valid(), std::string("compile failed: ") + test.name);
            for (bool front : {true, false}) {
                for (double degrees : angles) {
                    const double theta = degrees * 3.14159265358979 / 180.0;
                    const Float3 d(float(std::sin(theta)), 0.0f, float(front ? -std::cos(theta) : std::cos(theta)));
                    const RayHit hit = Hit(d.normalize());
                    require(hit.front_face == front, std::string(test.name) + ": wrong side");
                    const Albedo a = Measure(material, hit, count);
                    const double fresnel = Fresnel(std::cos(theta), front ? 1.5 : 1.0 / 1.5);
                    // R is a binomial fraction (each sample weighs 1): 4 sigma.
                    const double noise = 4.0 * std::sqrt(fresnel * (1.0 - fresnel) / count) + 1e-3;
                    const bool energyOk = std::abs(a.r + a.t - 1.0) <= 2e-3;
                    const bool fresnelOk = std::abs(a.r - fresnel) <= noise;
                    const bool ok = (!test.energy || energyOk) && (!test.fresnel || fresnelOk);
                    if (verbose || !ok)
                        std::printf("%-26s %s %2.0f deg: R %.4f T %.4f R+T %.4f (Fresnel R %.4f)%s\n", test.name,
                                    front ? "outside" : "inside ", degrees, a.r, a.t, a.r + a.t, fresnel,
                                    ok ? "" : "  FAIL");
                    if (!ok) ++failures;
                }
            }
        }
        std::filesystem::remove_all(dir);
        require(failures == 0, std::to_string(failures) + " directional albedos off");
    } catch (std::exception const& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("dielectric albedo ok\n");
    return 0;
}
