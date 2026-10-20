// OpenPBR transmission through the full MaterialX -> MDL -> native pipeline,
// sampled at a flat surface from both sides:
// - closed glass refracts with specular_ior (not coat_ior) and carries its
//   transmission_depth medium;
// - thin-walled glass transmits undeflected, tinted by transmission_color,
//   with no medium, from the front and the back.
#include "../src/mdl/context.h"
#include "../src/mdl_material.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, std::string const& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Document(std::string const& inputs)
{
    return "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n"
           "  <open_pbr_surface name=\"Surface\" type=\"surfaceshader\">\n" + inputs +
           "  </open_pbr_surface>\n"
           "  <surfacematerial name=\"PrismMaterial\" type=\"material\">\n"
           "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Surface\" />\n"
           "  </surfacematerial>\n</materialx>\n";
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

struct Stats {
    int transmitted = 0, undeflected = 0;
    Color3 weight = Color3(0.0f); // mean transmitted weight
    double eta = 0.0;             // sin(incident) / sin(transmitted), last refraction
};

Stats Sample(MDLMaterial& material, RayHit const& hit, int count)
{
    Stats s;
    unsigned seed = 7;
    const Float3 d = hit.d.normalize();
    for (int i = 0; i < count; ++i) {
        const BRDFSample b = material.sample(hit, seed);
        if (!b.scattered || b.null_event || dot(b.wi, hit.n_g) >= 0.0f) continue;
        ++s.transmitted;
        s.weight = s.weight + b.color;
        const Float3 w = b.wi.normalize();
        if (dot(w, d) > 0.9999f) ++s.undeflected;
        s.eta = std::hypot(d.x(), d.y()) / std::hypot(w.x(), w.y());
    }
    if (s.transmitted) s.weight = s.weight / float(s.transmitted);
    return s;
}

bool Near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
} // namespace

int main()
{
    const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    const char* mdlPath = std::getenv("PRISM_MATERIALX_MDL");
    if (!libraries || !*libraries || !mdlPath || !*mdlPath) return 77;
    try {
        mdl::Context context;
        require(context.is_valid(), "MDL context initialization failed");
        const auto dir = std::filesystem::temp_directory_path() / "prism-openpbr-transmission";
        std::filesystem::create_directories(dir);
        const std::string glass = Input("transmission_weight", "float", "1") +
                                  Input("specular_roughness", "float", "0");
        const std::string thin = Input("geometry_thin_walled", "boolean", "true");
        const std::string green = Input("transmission_color", "color3", "0.5, 1, 0.05");
        struct Case {
            const char* name;
            std::string inputs;
            double ior;   // 0: thin-walled
            bool medium;
        };
        const Case cases[] = {
            {"closed_1.5", glass + Input("specular_ior", "float", "1.5"), 1.5, false},
            {"closed_1.2", glass + Input("specular_ior", "float", "1.2"), 1.2, false},
            {"closed_green", glass + green + Input("transmission_depth", "float", "2"), 1.5, true},
            {"thin", glass + thin, 0.0, false},
            {"thin_green", glass + thin + green + Input("transmission_depth", "float", "0.001"), 0.0, false},
        };
        const int count = 2000;
        for (auto const& test : cases) {
            const auto path = dir / (std::string(test.name) + ".mtlx");
            { std::ofstream out(path); out << Document(test.inputs); }
            MDLMaterial material(path.string(), context, libraries, mdlPath);
            require(material.is_valid(), std::string("compile failed: ") + test.name);
            for (bool front : {true, false}) {
                const std::string where = std::string(test.name) + (front ? " front" : " back");
                const RayHit hit = Hit(Float3(0.3f, 0.0f, front ? -1.0f : 1.0f).normalize());
                require(hit.front_face == front, where + ": wrong side");
                const Stats s = Sample(material, hit, count);
                std::cout << where << ": transmitted " << s.transmitted << " undeflected " << s.undeflected
                          << " eta " << s.eta << " weight " << s.weight.x() << "," << s.weight.y() << ","
                          << s.weight.z() << "\n";
                require(s.transmitted > count / 2, where + ": too little transmission");
                if (test.ior == 0.0) {
                    require(s.undeflected == s.transmitted, where + ": thin wall deflected the transmission");
                } else {
                    require(s.undeflected == 0, where + ": closed glass did not refract");
                    const double expected = front ? test.ior : 1.0 / test.ior;
                    require(Near(s.eta, expected, 1e-3), where + ": refracted with the wrong IOR (" +
                                                             std::to_string(s.eta) + ")");
                }
                if (std::string(test.name) == "thin_green") {
                    require(Near(s.weight.x() / s.weight.y(), 0.5, 0.01) &&
                                Near(s.weight.z() / s.weight.y(), 0.05, 0.005),
                            where + ": thin wall not tinted by transmission_color");
                } else {
                    require(Near(s.weight.x(), s.weight.y(), 1e-4) && Near(s.weight.z(), s.weight.y(), 1e-4),
                            where + ": untinted glass tinted the transmission");
                }
                Material::PreparedShadingContext prepared;
                material.prepare(hit, prepared);
                const auto medium = material.medium(hit, prepared);
                require(bool(medium) == test.medium, where + ": wrong medium");
                if (medium)
                    require(Near(medium->absorption.x(), -std::log(0.5) / 2.0, 1e-3) &&
                                Near(medium->absorption.y(), 0.0, 1e-4) &&
                                Near(medium->absorption.z(), -std::log(0.05) / 2.0, 1e-3),
                            where + ": wrong absorption");
            }
        }
        std::filesystem::remove_all(dir);
    } catch (std::exception const& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "openpbr transmission ok\n";
    return 0;
}
