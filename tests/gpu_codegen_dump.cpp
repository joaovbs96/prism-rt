// Developer tool: flattens a USD scene for the GPU devices and writes the
// generated MDL shading-language code (HLSL and GLSL) next to a summary.
//   prism_gpu_codegen_dump <scene.usd> <output-directory>
#include "../src/device/gpu/gpu_scene.h"
#include "../src/mesh/usd_mesh.h"
#include "../src/scene.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: prism_gpu_codegen_dump <scene.usd> <output-directory> [max MDL materials]\n";
        return 2;
    }
    using namespace prism::device::gpu;
    const std::filesystem::path output(argv[2]);
    std::filesystem::create_directories(output);
    mdl::Context context;
    if (!context.is_valid()) { std::cerr << context.get_error() << '\n'; return 1; }
    RTCDevice device = rtcNewDevice(nullptr);
    {
        SceneManager scene(device);
        const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        const std::string libraryPath = libraries ? libraries : "";
        USDMesh mesh(argv[1], &context, libraryPath, libraryPath + "/mdl");
        scene.attachImportedMesh(&mesh);
        ColorTexture black(Color3(0.0f));
        ConstantColorBackground background(&black);
        scene.environment = &background;
        scene.commitScene();

        GpuSceneData data = BuildGpuScene(scene);
        std::cout << "prototypes " << data.prototypes.size() << ", instances " << data.instances.size()
                  << ", triangles " << data.triangleCount() << ", spheres " << data.spheres.size()
                  << ", materials " << data.materials.size() << " (MDL " << data.mdlMaterials.size()
                  << "), images " << data.images.size() << '\n';
        for (auto const& warning : data.warnings) std::cout << "warning: " << warning << '\n';
        if (argc == 4 && data.mdlMaterials.size() > size_t(std::atoi(argv[3])))
            data.mdlMaterials.resize(size_t(std::atoi(argv[3])));
        for (auto language : {ShadingLanguage::Hlsl, ShadingLanguage::Glsl}) {
            MdlShaderCode code = GenerateMdlShaderCode(data.mdlMaterials, language);
            const char* name = language == ShadingLanguage::Hlsl ? "hlsl" : "glsl";
            if (!code.success) { std::cerr << name << ": " << code.error << '\n'; return 1; }
            std::ofstream(output / (std::string("materials.") + name)) << code.code;
            size_t invalid = 0;
            for (auto const& m : code.materials) invalid += m.valid ? 0 : 1;
            std::cout << name << ": programs " << code.programCount << ", textures "
                      << code.textures.size() - 1 << ", argument bytes " << code.argumentBlocks.size()
                      << ", ro data " << code.roData.size() << ", invalid materials " << invalid
                      << ", translate " << code.translateSeconds << " s, code "
                      << code.code.size() / 1024 << " KiB\n";
            for (auto const& m : code.materials)
                if (!m.valid) std::cout << "  invalid: " << m.error << '\n';
        }
    }
    rtcReleaseDevice(device);
    return 0;
}
