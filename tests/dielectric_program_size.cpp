// DocumentLoader's dielectric repairs keep the programs of repaired surfaces
// as small as the library's (docs/materialx_compatibility.md, "Private
// graphs"): the repair rewrites a private copy of the surface's graph, whose
// constants stay literals of an MDL function, so a class compilation keeps the
// surface's inputs as its parameters. Flattened into the material instead,
// every internal constant became a class parameter (scatter modes among them,
// which kept all three lobe variants of every dielectric_bsdf): 2.5x the
// parameters and 3.5x the GPU code. A thin wall with one shader on both sides
// compiles its scattering once, on the CPU and the GPU.
#include "../src/device/gpu/gpu_materials.h"
#include "../src/mdl/context.h"
#include "../src/mdl_material.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string Input(std::string const& name, std::string const& type, std::string const& value)
{
    return "    <input name=\"" + name + "\" type=\"" + type + "\" value=\"" + value + "\" />\n";
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
} // namespace

int main()
{
    const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    const char* mdlPath = std::getenv("PRISM_MATERIALX_MDL");
    if (!libraries || !*libraries || !mdlPath || !*mdlPath) return 77;
    mdl::Context context;
    check(context.is_valid(), "MDL context");
    const auto dir = std::filesystem::temp_directory_path() / "prism-dielectric-program-size";
    std::filesystem::create_directories(dir);

    const std::string glass = Input("specular_roughness", "float", "0.2") + Input("transmission_weight", "float", "1");
    struct Case { const char* name; std::string inputs; };
    const Case cases[] = {
        {"glass", glass},
        // Same repair, other values: the same class program.
        {"water", glass + Input("specular_ior", "float", "1.33") + Input("base_color", "color3", "0.2, 0.4, 0.6")},
        // A thin film over tinted glass is left unrepaired (the library's graph).
        {"unrepaired", glass + Input("thin_film_weight", "float", "1") + Input("thin_film_thickness", "float", "0.5") +
                           Input("transmission_color", "color3", "0.5, 0.8, 0.9")},
        {"thin", glass + Input("geometry_thin_walled", "boolean", "true")},
    };
    std::vector<std::unique_ptr<MDLMaterial>> materials;
    for (auto const& c : cases) {
        const auto path = dir / (std::string(c.name) + ".mtlx");
        { std::ofstream(path) << Document(c.inputs); }
        materials.push_back(std::make_unique<MDLMaterial>(path.string(), context, libraries, mdlPath));
        check(materials.back()->is_valid(), std::string(c.name) + " compiles");
    }
    if (failures) return 1;
    auto& glassMaterial = *materials[0];
    auto& unrepaired = *materials[2];
    auto& thin = *materials[3];

    // Class parameters: the surface's inputs, as for the library's graph
    // (flattened: 150 against 65).
    const size_t repairedUniforms = glassMaterial.get_generated_uniforms().size();
    const size_t libraryUniforms = unrepaired.get_generated_uniforms().size();
    std::cout << std::format("class parameters: repaired {}, library {}, thin-walled {}\n", repairedUniforms,
                             libraryUniforms, thin.get_generated_uniforms().size());
    check(repairedUniforms <= libraryUniforms + 4, "the repaired glass has the library's parameters");
    check(thin.get_generated_uniforms().size() <= libraryUniforms + 4, "the thin wall has the library's parameters");

    // The thin wall's backface scatters with the surface's function.
    auto const& functions = thin.get_target_code().program->functions;
    check(functions.backface_bsdf_function_index == functions.surface_bsdf_function_index,
          "the thin wall's backface has its own copy of the surface's scattering");
    auto const& glassFunctions = glassMaterial.get_target_code().program->functions;
    check(glassFunctions.backface_bsdf_function_index != glassFunctions.surface_bsdf_function_index,
          "a closed surface's (empty) backface is not the surface");

    std::vector<MDLMaterial const*> list;
    for (auto const& m : materials) list.push_back(m.get());
    const auto programs = prism::device::gpu::ProgramCodes(list);
    check(programs.size() == list.size(), "one program per material");
    for (size_t i = 0; i < programs.size(); ++i)
        check(programs[i] && programs[i]->success, std::string(cases[i].name) + ": GPU program");
    if (failures) return 1;
    for (size_t i = 0; i < programs.size(); ++i)
        std::cout << std::format("{}: {} KB of generated HLSL\n", cases[i].name, programs[i]->targetCodeBytes >> 10);
    check(programs[0]->digest == programs[1]->digest, "glasses with the same repair share one program");
    // The coupled lobes cost little over the library's (flattened: 2.9x).
    check(programs[0]->targetCodeBytes * 10 < programs[2]->targetCodeBytes * 13,
          "the repaired glass's code is within 1.3x of the library's");
    // One copy of the scattering code (two: 1.9x the SPIR-V).
    check(programs[3]->code.find("prism_p_backface_bsdf") == std::string::npos,
          "the thin wall has a backface scattering function");
    check(programs[3]->code.find("useSurface") != std::string::npos,
          "the thin wall's backface does not dispatch to the surface's function");
    check(programs[3]->targetCodeBytes * 10 < programs[2]->targetCodeBytes * 13,
          "the thin wall's code is within 1.3x of the library glass's");
    check(programs[0]->code.find("useSurface") == std::string::npos,
          "a closed surface's dispatch changed");

    std::filesystem::remove_all(dir);
    if (failures) return 1;
    std::cout << "dielectric program size: all checks passed\n";
    return 0;
}
