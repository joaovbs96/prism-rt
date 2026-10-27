// Directional albedo (reflection + transmission) of smooth, non-absorbing
// dielectrics through the full MaterialX -> MDL -> native pipeline, sampled at
// a flat surface from outside (front face) and inside (back face). A smooth
// dielectric must return all the energy at every angle: R + T = 1, with
// R = Fresnel(theta) (1 under total internal reflection); tinted ones
// R = s Fresnel and T = t (1 - Fresnel). Covers DocumentLoader's dielectric
// repairs (docs/materialx_compatibility.md).
#include "../src/mdl/context.h"
#include "../src/mdl_material.h"
#include "../src/renderer/renderer_options.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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

// The fraction of light that leaves a closed sphere entered at `degrees`: a
// chord keeps the incidence angle, so paths alternate between the plane's
// front face and back faces at the refracted angle. Refraction and total
// internal reflection must agree on one IOR, or paths are trapped inside.
double SphereEscape(MDLMaterial& material, double degrees, int paths)
{
    std::mt19937 generator(5);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    double escaped = 0.0;
    for (int p = 0; p < paths; ++p) {
        double weight = 1.0, cosine = std::cos(degrees * 3.14159265358979 / 180.0);
        bool front = true;
        for (int bounce = 0; bounce < 64 && weight > 0.0; ++bounce) {
            const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
            const RayHit hit = Hit(Float3(float(sine), 0.0f, float(front ? -cosine : cosine)).normalize());
            Material::PreparedShadingContext context;
            require(material.prepare(hit, context), "prepare failed");
            context.hasXi = true;
            for (float& x : context.xi) x = uniform(generator);
            unsigned seed = 3;
            const BRDFSample b = material.samplePrepared(hit, seed, context);
            if (!b.scattered) break;
            weight *= (double(b.color.x()) + b.color.y() + b.color.z()) / 3.0;
            // Up (+z) leaves: reflected from outside, transmitted from inside.
            if (b.wi.z() > 0.0f) {
                escaped += weight;
                break;
            }
            cosine = std::abs(double(b.wi.normalize().z()));
            front = false;
        }
    }
    return escaped / paths;
}
} // namespace

int main(int argc, char** argv)
{
    const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    const char* mdlPath = std::getenv("PRISM_MATERIALX_MDL");
    if (!libraries || !*libraries || !mdlPath || !*mdlPath) return 77;
    // -v [case]: print every albedo (of one case).
    // -s [case]: survey other layered models (never fails; prints each angle
    // and the worst R + T per side).
    // -f file.mtlx: survey that document's first material.
    const char* file = argc > 2 && std::strcmp(argv[1], "-f") == 0 ? argv[2] : nullptr;
    const bool survey = file || (argc > 1 && std::strcmp(argv[1], "-s") == 0);
    const bool verbose = survey || (argc > 1 && std::strcmp(argv[1], "-v") == 0);
    const char* only = verbose && !file && argc > 2 ? argv[2] : nullptr;
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
        // Tinted cases expect R = rtint * Fresnel and T = tint * (1 - Fresnel)
        // (mean tints).
        struct Case {
            const char* name;
            std::string body;
            bool energy, fresnel;
            double tint = 1.0, rtint = 1.0;
        };
        const std::string dielectric =
            "  <dielectric_bsdf name=\"Bsdf\" type=\"BSDF\">\n" + Input("ior", "float", "1.5") +
            Input("roughness", "vector2", "0, 0") + Input("scatter_mode", "string", "RT") +
            "  </dielectric_bsdf>\n  <surface name=\"Surface\" type=\"surfaceshader\">\n"
            "    <input name=\"bsdf\" type=\"BSDF\" nodename=\"Bsdf\" />\n  </surface>\n";
        const std::string rough = Input("base_weight", "float", "0") + Input("specular_roughness", "float", "0.3") +
                                  Input("specular_ior", "float", "1.5") + Input("transmission_weight", "float", "1");
        const std::string coat = Input("coat_weight", "float", "1") + Input("coat_roughness", "float", "0");
        const std::string tint = "0.5, 0.8, 0.9";
        const double tintMean = (0.5 + 0.8 + 0.9) / 3.0;
        const std::string ssGlass = Input("base", "float", "0") + Input("specular_roughness", "float", "0") +
                                    Input("specular_IOR", "float", "1.5") + Input("transmission", "float", "1");
        const std::string gltfGlass = Input("metallic", "float", "0") + Input("roughness", "float", "0") +
                                      Input("ior", "float", "1.5") + Input("transmission", "float", "1");
        auto bsdf = [](std::string const& nodes) {
            return nodes + "  <surface name=\"Surface\" type=\"surfaceshader\">\n"
                           "    <input name=\"bsdf\" type=\"BSDF\" nodename=\"Top\" />\n  </surface>\n";
        };
        auto node = [](std::string const& category, std::string const& name, std::string const& inputs) {
            return "  <" + category + " name=\"" + name + "\" type=\"BSDF\">\n" + inputs + "  </" + category + ">\n";
        };
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
            // Smooth: tinted (prism_dielectric_split), thin films, smooth
            // coats (prism_fresnel_layer), thin walls, UsdPreviewSurface and
            // LamaDielectric. Thin walls and coats reflect more or other than
            // Fresnel's R.
            {"open_pbr_tint_depth0",
             Shader("open_pbr_surface", glass + Input("transmission_color", "color3", tint)), true, true, tintMean},
            {"open_pbr_specular_tint",
             Shader("open_pbr_surface", glass + Input("specular_color", "color3", "1, 0.5, 0.5")), true, true, 1.0,
             2.0 / 3.0},
            {"standard_surface_tint",
             Shader("standard_surface", ssGlass + Input("transmission_color", "color3", tint)), true, true, tintMean},
            {"gltf_pbr_tint", Shader("gltf_pbr", gltfGlass + Input("base_color", "color3", tint)), true, true, tintMean},
            {"open_pbr_thin_film",
             Shader("open_pbr_surface", glass + Input("thin_film_weight", "float", "1") +
                                            Input("thin_film_thickness", "float", "0.5")),
             true, false},
            {"standard_surface_thin_film",
             Shader("standard_surface", ssGlass + Input("thin_film_thickness", "float", "500") +
                                            Input("thin_film_IOR", "float", "1.4")),
             true, false},
            {"gltf_pbr_iridescence",
             Shader("gltf_pbr", gltfGlass + Input("iridescence", "float", "1") +
                                    Input("iridescence_thickness", "float", "400")),
             true, false},
            {"open_pbr_coat_no_darkening",
             Shader("open_pbr_surface", glass + coat + Input("coat_darkening", "float", "0") +
                                            Input("coat_ior", "float", "1.5")),
             true, false},
            {"standard_surface_coat",
             Shader("standard_surface", ssGlass + Input("coat", "float", "1") + Input("coat_roughness", "float", "0")),
             true, false},
            {"gltf_pbr_clearcoat", Shader("gltf_pbr", gltfGlass + Input("clearcoat", "float", "1")), true, false},
            {"standard_surface_thin_walled",
             Shader("standard_surface", ssGlass + Input("thin_walled", "boolean", "true")), true, false},
            {"usd_preview_glass",
             Shader("UsdPreviewSurface", Input("opacity", "float", "0") + Input("roughness", "float", "0") +
                                             Input("ior", "float", "1.5")),
             true, true},
            {"lama_dielectric",
             bsdf(node("LamaDielectric", "Top", Input("fresnelMode", "integer", "1") + Input("IOR", "float", "1.5") +
                                                    Input("roughness", "float", "0"))),
             true, true},
            {"lama_generalized_schlick",
             bsdf(node("LamaGeneralizedSchlick", "Top", Input("roughness", "float", "0"))), true, true},
            // Not repaired (docs/materialx_compatibility.md): coat darkening.
            {"open_pbr_coat", Shader("open_pbr_surface", glass + coat), false, false},
        };
        // Survey (-s): configurations outside the repair or only partly
        // repaired, rough ones, and opaque surfaces.
        auto link = [](std::string const& name, std::string const& node) {
            return "    <input name=\"" + name + "\" type=\"BSDF\" nodename=\"" + node + "\" />\n";
        };
        // A specular_color of 0.9999 takes the split's (t - min) term.
        const std::string dodge = Input("specular_color", "color3", "0.9999, 0.9999, 0.9999");
        const std::string transmitT = Input("ior", "float", "1.5") + Input("roughness", "vector2", "0, 0") +
                                      Input("scatter_mode", "string", "T");
        const Case surveyCases[] = {
            {"open_pbr_unrepaired", Shader("open_pbr_surface", glass + dodge), false, false},
            {"open_pbr_rough_unrepaired", Shader("open_pbr_surface", rough + dodge), false, false},
            {"open_pbr_tint_rough", Shader("open_pbr_surface", rough + Input("transmission_color", "color3", tint)),
             false, false, tintMean},
            {"open_pbr_specular_tint_rough",
             Shader("open_pbr_surface", rough + Input("specular_color", "color3", "1, 0.5, 0.5")), false, false, 1.0,
             2.0 / 3.0},
            {"open_pbr_thin_film_rough",
             Shader("open_pbr_surface", rough + Input("thin_film_weight", "float", "1") +
                                            Input("thin_film_thickness", "float", "0.5")),
             false, false},
            {"open_pbr_thin_film_tint",
             Shader("open_pbr_surface", glass + Input("thin_film_weight", "float", "1") +
                                            Input("thin_film_thickness", "float", "0.5") +
                                            Input("transmission_color", "color3", tint)),
             false, false, tintMean},
            // Coat IOR 1.6 (the default) over glass of 1.5: not repaired.
            {"open_pbr_coat_ior",
             Shader("open_pbr_surface", glass + coat + Input("coat_darkening", "float", "0")), false, false},
            {"open_pbr_coat_rough",
             Shader("open_pbr_surface", glass + Input("coat_weight", "float", "1") +
                                            Input("coat_roughness", "float", "0.3") +
                                            Input("coat_darkening", "float", "0")),
             false, false},
            {"open_pbr_thin_walled",
             Shader("open_pbr_surface", glass + Input("geometry_thin_walled", "boolean", "true")), false, false},
            {"open_pbr_fuzz", Shader("open_pbr_surface", glass + Input("fuzz_weight", "float", "1")), false, false},
            {"open_pbr_opaque_white",
             Shader("open_pbr_surface",
                    Input("base_color", "color3", "1, 1, 1") + Input("specular_roughness", "float", "0")),
             false, false},
            {"open_pbr_opaque_white_rough",
             Shader("open_pbr_surface",
                    Input("base_color", "color3", "1, 1, 1") + Input("specular_roughness", "float", "0.3")),
             false, false},
            {"standard_surface_unrepaired", Shader("standard_surface", ssGlass + dodge), false, false},
            {"standard_surface_tint_rough",
             Shader("standard_surface", Input("base", "float", "0") + Input("specular_roughness", "float", "0.3") +
                                            Input("specular_IOR", "float", "1.5") + Input("transmission", "float", "1") +
                                            Input("transmission_color", "color3", tint)),
             false, false, tintMean},
            {"standard_surface_sheen", Shader("standard_surface", ssGlass + Input("sheen", "float", "1")), false, false},
            {"standard_surface_opaque_white",
             Shader("standard_surface", Input("base", "float", "1") + Input("base_color", "color3", "1, 1, 1") +
                                            Input("specular_roughness", "float", "0")),
             false, false},
            {"standard_surface_opaque_white_rough",
             Shader("standard_surface", Input("base", "float", "1") + Input("base_color", "color3", "1, 1, 1") +
                                            Input("specular_roughness", "float", "0.3")),
             false, false},
            {"gltf_pbr_sheen",
             Shader("gltf_pbr", gltfGlass + Input("sheen_color", "color3", "1, 1, 1") +
                                    Input("sheen_roughness", "float", "0.3")),
             false, false},
            {"usd_preview_glass_rough",
             Shader("UsdPreviewSurface", Input("opacity", "float", "0") + Input("roughness", "float", "0.3") +
                                             Input("ior", "float", "1.5")),
             false, false},
            {"usd_preview_half_opacity",
             Shader("UsdPreviewSurface", Input("opacity", "float", "0.5") + Input("roughness", "float", "0") +
                                             Input("diffuseColor", "color3", "1, 1, 1") + Input("ior", "float", "1.5")),
             false, false},
            {"disney_glass",
             Shader("disney_principled", Input("baseColor", "color3", "1, 1, 1") + Input("roughness", "float", "0") +
                                             Input("specTrans", "float", "1") + Input("ior", "float", "1.5")),
             false, false},
            {"lama_dielectric_rough",
             bsdf(node("LamaDielectric", "Top", Input("fresnelMode", "integer", "1") + Input("IOR", "float", "1.5") +
                                                    Input("roughness", "float", "0.3"))),
             false, false},
            {"lama_generalized_schlick_tinted",
             bsdf(node("LamaGeneralizedSchlick", "Top", Input("roughness", "float", "0") +
                                                            Input("reflectionTint", "color3", "1, 0.5, 0.5"))),
             false, false},
            {"schlick_r_over_dielectric_t",
             bsdf(node("dielectric_bsdf", "Trans", transmitT) +
                  node("generalized_schlick_bsdf", "Refl",
                       Input("color0", "color3", "0.04, 0.04, 0.04") + Input("roughness", "vector2", "0, 0")) +
                  node("layer", "Top", link("top", "Refl") + link("base", "Trans"))),
             false, false},
            {"sheen_over_dielectric_rt",
             bsdf(node("dielectric_bsdf", "Glass",
                       Input("ior", "float", "1.5") + Input("roughness", "vector2", "0, 0") +
                           Input("scatter_mode", "string", "RT")) +
                  node("sheen_bsdf", "Sheen", Input("roughness", "float", "0.3")) +
                  node("layer", "Top", link("top", "Sheen") + link("base", "Glass"))),
             false, false},
        };
        const int count = 20000;
        const std::vector<double> angles =
            survey ? std::vector<double>{0.0, 20.0, 30.0, 40.0, 41.0, 50.0, 60.0, 75.0, 85.0}
                   : std::vector<double>{0.0, 20.0, 40.0, 60.0, 75.0, 85.0};
        std::vector<Case const*> selected;
        std::string fileBody;
        Case fileCase{"file", "", false, false};
        if (file) {
            std::ifstream in(file);
            fileBody.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            require(!fileBody.empty(), std::string("cannot read ") + file);
            selected.push_back(&fileCase);
        } else if (survey)
            for (auto const& test : surveyCases) selected.push_back(&test);
        if (!file)
            for (auto const& test : cases) selected.push_back(&test);
        int failures = 0;
        for (Case const* entry : selected) {
            auto const& test = *entry;
            if (only && std::strcmp(only, test.name) != 0) continue;
            const auto path = dir / (std::string(test.name) + ".mtlx");
            { std::ofstream out(path); out << (entry == &fileCase ? fileBody : Document(test.body)); }
            MDLMaterial material(path.string(), context, libraries, mdlPath, prism::MaterialSettings{prism::color::ColorManagement::Create({}), prism::RendererOptions::FromEnvironment().CacheRoot()});
            require(material.is_valid(), std::string("compile failed: ") + test.name);
            if (file) {
                Material::PreparedShadingContext context;
                const RayHit hit = Hit(Float3(0.0f, 0.0f, -1.0f));
                require(material.prepare(hit, context), "prepare failed");
                const auto medium = material.medium(hit, context);
                std::printf("medium: %s", medium ? "" : "none\n");
                if (medium)
                    std::printf("absorption %.4f %.4f %.4f scattering %.4f %.4f %.4f\n", medium->absorption.x(),
                                medium->absorption.y(), medium->absorption.z(), medium->scattering.x(),
                                medium->scattering.y(), medium->scattering.z());
            }
            for (bool front : {true, false}) {
                double worst = 2.0, worstAngle = 0.0;
                for (double degrees : angles) {
                    const double theta = degrees * 3.14159265358979 / 180.0;
                    const Float3 d(float(std::sin(theta)), 0.0f, float(front ? -std::cos(theta) : std::cos(theta)));
                    const RayHit hit = Hit(d.normalize());
                    require(hit.front_face == front, std::string(test.name) + ": wrong side");
                    const Albedo a = Measure(material, hit, count);
                    const double fresnel = Fresnel(std::cos(theta), front ? 1.5 : 1.0 / 1.5);
                    // R is a binomial fraction (each sample weighs 1): 4 sigma.
                    // Tinted cases pick lobes by their tints, a binomial
                    // fraction too (at most 4 sigma = 0.014).
                    const bool tinted = test.tint != 1.0 || test.rtint != 1.0;
                    const double noise = 4.0 * std::sqrt(fresnel * (1.0 - fresnel) / count) + (tinted ? 4.0 * std::sqrt(0.25 / count) : 1e-3);
                    const double expected = test.rtint * fresnel + test.tint * (1.0 - fresnel);
                    const bool energyOk =
                        std::abs(a.r + a.t - (tinted ? expected : 1.0)) <= (tinted ? 4.0 * std::sqrt(0.25 / count) + 1e-3 : 2e-3);
                    const bool fresnelOk = std::abs(a.r - test.rtint * fresnel) <= noise;
                    const bool ok = (!test.energy || energyOk) && (!test.fresnel || fresnelOk);
                    if (verbose || !ok)
                        std::printf("%-26s %s %2.0f deg: R %.4f T %.4f R+T %.4f (Fresnel R %.4f%s)%s\n", test.name,
                                    front ? "outside" : "inside ", degrees, a.r, a.t, a.r + a.t, fresnel,
                                    tinted ? (" expected R+T " + std::to_string(expected).substr(0, 6)).c_str() : "",
                                    ok ? "" : "  FAIL");
                    if (!ok && !survey) ++failures;
                    // Tinted: R + T relative to the expected R + T.
                    const double kept = (a.r + a.t) / (tinted ? expected : 1.0);
                    if (kept < worst) worst = kept, worstAngle = degrees;
                }
                if (survey)
                    std::printf("SUMMARY %-28s %s worst R+T %.4f at %2.0f deg%s\n", test.name,
                                front ? "outside" : "inside ", worst, worstAngle,
                                test.tint != 1.0 || test.rtint != 1.0 ? " (of the expected)" : "");
            }
            // Light entering a closed sphere must leave it (untinted cases).
            if (test.tint == 1.0 && test.rtint == 1.0) {
                double worst = 2.0, worstAngle = 0.0;
                for (double degrees : {0.0, 40.0, 60.0, 75.0, 85.0}) {
                    const double escaped = SphereEscape(material, degrees, 4000);
                    const bool ok = !test.energy || escaped >= 1.0 - 2e-3;
                    if (verbose || !ok)
                        std::printf("%-26s sphere  %2.0f deg: escaped %.4f%s\n", test.name, degrees, escaped,
                                    ok ? "" : "  FAIL");
                    if (!ok && !survey) ++failures;
                    if (escaped < worst) worst = escaped, worstAngle = degrees;
                }
                if (survey)
                    std::printf("SUMMARY %-28s sphere  escaped %.4f at %2.0f deg\n", test.name, worst, worstAngle);
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
