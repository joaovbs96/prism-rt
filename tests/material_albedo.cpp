// Directional albedo per channel of opaque surfaces through the full
// MaterialX -> MDL -> native pipeline, independent of the MaterialX version
// linked: the Cornell box's walls (open_pbr_surface with specular_weight 0,
// written as hdMtlx writes them, and as the repo's documents do) reflect
// exactly their base colour at every angle (a smooth diffuse base, sampled by
// its cosine, weighs base_color per sample). With the specular lobe on, and
// for standard_surface and gltf_pbr, the base colour must still show (the
// red channel well above the others, also at grazing angles). MaterialX 1.39.3 once lost the diffuse
// lobe here (docs/materialx_compatibility.md).
//
// -f file.mtlx: print the albedo per channel of that document's first material.
// -g file.mtlx: print the MDL generated for it (the generation cache off).
// -v: print every albedo.
#include "../src/materialx/loader.h"
#include "../src/materialx/mdl_generator.h"
#include "../src/mdl/context.h"
#include "../src/mdl_material.h"
#include "../src/renderer/renderer_options.h"

#include <MaterialXCore/Util.h>

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
#include <vector>

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

std::string Input(std::string const& name, std::string const& type, std::string const& value)
{
    return "    <input name=\"" + name + "\" type=\"" + type + "\" value=\"" + value + "\" />\n";
}

// The Cornell box's wall (assets/scenes/CornellBox.usda); `nodedef` as hdMtlx
// writes it (the shader's info:id).
std::string CornellWall(std::string const& color, std::string const& specular, bool nodedef)
{
    return std::string("  <open_pbr_surface name=\"Surface\" type=\"surfaceshader\"") +
           (nodedef ? " nodedef=\"ND_open_pbr_surface_surfaceshader\"" : "") + ">\n" +
           Input("base_color", "color3", color) + Input("base_diffuse_roughness", "float", "0") +
           Input("base_metalness", "float", "0") + Input("base_weight", "float", "1") +
           Input("coat_weight", "float", "0") + Input("emission_luminance", "float", "0") +
           Input("specular_weight", "float", specular) + Input("transmission_weight", "float", "0") +
           "  </open_pbr_surface>\n";
}

std::string Shader(std::string const& node, std::string const& inputs)
{
    return "  <" + node + " name=\"Surface\" type=\"surfaceshader\">\n" + inputs + "  </" + node + ">\n";
}

prism::MaterialSettings Settings()
{
    return prism::MaterialSettings{prism::color::ColorManagement::Create({}),
                                   prism::RendererOptions::FromEnvironment().CacheRoot()};
}

int PrintGenerated(const char* libraries, const char* mdlPath, const char* file)
{
    auto color = prism::color::ColorManagement::Create({});
    MDLGenerator generator(libraries, color.get());
    DocumentLoader loader(generator.get_stdlib(), true, false);
    loader.setDistributeLibraryLayers(MDLGenerator::dropsLayerBaseUnderMix(libraries));
    loader.setZeroUnitIorReflections(MDLGenerator::reflectsAtUnitIor(mdlPath));
    auto doc = loader.load(file);
    require(bool(doc), std::string("cannot load ") + file);
    auto materials = doc->getMaterialNodes();
    require(!materials.empty(), "no material");
    const GeneratedMDL generated = generator.generate(doc, materials[0]->getName(), *color, false);
    require(generated.success, "generation failed: " + generated.error_message);
    std::printf("%s\n", generated.mdl_source.c_str());
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    const char* mdlPath = std::getenv("PRISM_MATERIALX_MDL");
    if (!libraries || !*libraries || !mdlPath || !*mdlPath) return 77;
    const char* file = argc > 2 && (std::strcmp(argv[1], "-f") == 0 || std::strcmp(argv[1], "-g") == 0) ? argv[2]
                                                                                                         : nullptr;
    const bool verbose = file || (argc > 1 && std::strcmp(argv[1], "-v") == 0);
    try {
        if (file && std::strcmp(argv[1], "-g") == 0) return PrintGenerated(libraries, mdlPath, file);
        const auto [major, minor, patch] = MaterialX::getVersionIntegers();
        std::printf("MaterialX %d.%d.%d\n", major, minor, patch);
        mdl::Context context;
        require(context.is_valid(), "MDL context initialization failed");
        const auto dir = std::filesystem::temp_directory_path() / "prism-material-albedo";
        std::filesystem::create_directories(dir);
        const int count = 20000;
        const std::vector<double> angles{0.0, 30.0, 60.0, 80.0};
        if (file) {
            MDLMaterial material(file, context, libraries, mdlPath, Settings());
            require(material.is_valid(), std::string("compile failed: ") + file);
            for (double degrees : angles) {
                const auto a = Albedo(material, degrees, count);
                std::printf("%2.0f deg: albedo %.4f %.4f %.4f\n", degrees, a[0], a[1], a[2]);
            }
            return 0;
        }
        // exact: albedo == base colour per channel; otherwise the base colour
        // must show: red above 0.3 and 0.25 above green and blue.
        struct Case {
            const char* name;
            std::string surface;
            std::array<double, 3> base;
            bool exact;
        };
        const std::vector<Case> cases{
            {"cornell_red_hdmtlx", CornellWall("0.65, 0.05, 0.05", "0", true), {0.65, 0.05, 0.05}, true},
            {"cornell_green_hdmtlx", CornellWall("0.12, 0.45, 0.15", "0", true), {0.12, 0.45, 0.15}, true},
            {"cornell_white_hdmtlx", CornellWall("0.73, 0.73, 0.73", "0", true), {0.73, 0.73, 0.73}, true},
            {"cornell_red", CornellWall("0.65, 0.05, 0.05", "0", false), {0.65, 0.05, 0.05}, true},
            {"cornell_red_specular", CornellWall("0.65, 0.05, 0.05", "1", true), {0.65, 0.05, 0.05}, false},
            {"open_pbr_default_red", Shader("open_pbr_surface", Input("base_color", "color3", "0.65, 0.05, 0.05")),
             {0.65, 0.05, 0.05}, false},
            {"standard_surface_red",
             Shader("standard_surface", Input("base_color", "color3", "0.65, 0.05, 0.05") +
                                            Input("specular", "float", "0")),
             {0.65, 0.05, 0.05}, false},
            {"gltf_pbr_red",
             Shader("gltf_pbr", Input("base_color", "color3", "0.65, 0.05, 0.05") + Input("metallic", "float", "0") +
                                    Input("roughness", "float", "0.5")),
             {0.65, 0.05, 0.05}, false},
        };
        int failures = 0;
        for (auto const& test : cases) {
            const auto path = dir / (std::string(test.name) + ".mtlx");
            { std::ofstream out(path); out << Document(test.surface); }
            MDLMaterial material(path.string(), context, libraries, mdlPath, Settings());
            require(material.is_valid(), std::string("compile failed: ") + test.name);
            for (double degrees : angles) {
                const auto a = Albedo(material, degrees, count);
                bool ok = true;
                if (test.exact) {
                    for (int c = 0; c < 3; ++c) ok = ok && std::abs(a[c] - test.base[c]) <= 2e-3 + 0.01 * test.base[c];
                } else {
                    ok = a[0] > 0.3 && a[0] - a[1] > 0.25 && a[0] - a[2] > 0.25;
                }
                if (verbose || !ok)
                    std::printf("%-22s %2.0f deg: albedo %.4f %.4f %.4f (base %.2f %.2f %.2f%s)%s\n", test.name,
                                degrees, a[0], a[1], a[2], test.base[0], test.base[1], test.base[2],
                                test.exact ? ", exact" : "", ok ? "" : "  FAIL");
                failures += !ok;
            }
        }
        std::filesystem::remove_all(dir);
        require(failures == 0, std::to_string(failures) + " albedos off");
    } catch (std::exception const& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("material albedo ok\n");
    return 0;
}
