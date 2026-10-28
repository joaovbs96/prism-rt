// Developer tool: flattens a USD scene for the GPU devices and writes the
// generated MDL HLSL code next to a summary.
//   prism_gpu_codegen_dump <scene.usd> <output-directory>
#include "../src/device/gpu/gpu_scene.h"
#include "scene_api_test_util.h"

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
    {
        const prism::Renderer renderer(prism::test::TestRendererOptions());
        prism::Scene api = renderer.createScene();
        SceneManager& scene = prism::test::LoadUsd(api, argv[1]);

        GpuSceneData data = BuildGpuScene(scene);
        std::cout << "prototypes " << data.prototypes.size() << ", instances " << data.instances.size()
                  << ", triangles " << data.triangleCount() << ", spheres " << data.spheres.size()
                  << ", materials " << data.materials.size() << " (MDL " << data.mdlMaterials.size()
                  << "), images " << data.images.size() << '\n';
        for (auto const& warning : data.warnings) std::cout << "warning: " << warning << '\n';
        if (argc == 4 && data.mdlMaterials.size() > size_t(std::atoi(argv[3])))
            data.mdlMaterials.resize(size_t(std::atoi(argv[3])));
        MdlShaderCode code = GenerateMdlShaderCode(data.mdlMaterials);
        if (!code.success) { std::cerr << code.error << '\n'; return 1; }
        std::ofstream(output / "materials.hlsl") << code.code;
        size_t invalid = 0;
        for (auto const& m : code.materials) invalid += m.valid ? 0 : 1;
        std::cout << "programs " << code.programCount << ", textures " << code.textures.size() - 1
                  << ", argument bytes " << code.argumentBlocks.size() << ", ro data " << code.roData.size()
                  << ", invalid materials " << invalid << ", translate " << code.translateSeconds
                  << " s, code " << code.code.size() / 1024 << " KiB\n";
        for (auto const& m : code.materials)
            if (!m.valid) std::cout << "  invalid: " << m.error << '\n';
    }
    return 0;
}
