// GPU kernels compile for a scene with an MDL material: the GLSL kernel to
// SPIR-V (Vulkan device; runs everywhere the device builds, including macOS
// where no driver can execute it), and the HLSL double-precision rewrite.
//   prism_gpu_shader_compile_test <scene.usda>
#include "../src/device/gpu/gpu_scene.h"
#include "../src/device/gpu/hlsl_fp32_rewrite.h"
#include "../src/mesh/usd_mesh.h"
#include "../src/scene.h"
#ifdef PRISM_HAS_VULKAN
#include "../src/device/vulkan/vulkan_device.h"
#endif

#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main(int argc, char** argv)
{
    using namespace prism::device::gpu;
    // The fp32 rewrite: types and literal suffixes change, identifiers do not.
    std::string rewritten, error;
    check(RewriteHlslToFp32("double2 a = double2(1.0l, 2.5L); double3x3 m; "
                            "float b = mdl_read_argblock_as_double(4); // double comment\n",
                            &rewritten, &error) &&
          rewritten == "float2 a = float2(1.0, 2.5); float3x3 m; "
                       "float b = mdl_read_argblock_as_double(4); // double comment\n",
          "HLSL double rewrite: " + rewritten);
    check(RewriteHlslToFp32("int x = 10L; uint y = 0x1fL;", &rewritten, &error) &&
          rewritten == "int x = 10L; uint y = 0x1fL;", "integer literal suffixes are kept: " + rewritten);

    if (argc < 2) {
        std::cerr << "usage: prism_gpu_shader_compile_test <scene.usda>\n";
        return 2;
    }
    mdl::Context context;
    check(context.is_valid(), "MDL context");
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
        check(!data.mdlMaterials.empty(), "the scene has MDL materials");
        MdlShaderCode code = GenerateMdlShaderCode(data.mdlMaterials, ShadingLanguage::Glsl);
        check(code.success, "GLSL MDL code: " + code.error);
        for (auto const& m : code.materials) check(m.valid, "GLSL material: " + m.error);
#ifdef PRISM_HAS_VULKAN
        std::vector<uint32_t> spirv;
        check(prism::device::CompileVulkanKernel(code.code, &spirv, &error) && spirv.size() > 1000 &&
                  spirv[0] == 0x07230203u,
              "Vulkan kernel compiles to SPIR-V: " + error);
#endif
        MdlShaderCode hlsl = GenerateMdlShaderCode(data.mdlMaterials, ShadingLanguage::Hlsl);
        check(hlsl.success && RewriteHlslToFp32(hlsl.code, &rewritten, &error),
              "HLSL MDL code rewrites to single precision: " + error);
    }
    rtcReleaseDevice(device);
    if (failures) return 1;
    std::cout << "GPU shader compile: all checks passed\n";
    return 0;
}
