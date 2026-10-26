// The Vulkan device's material shaders stay within what the driver compiles,
// checked on the CPU (DXC only, no device): several textured emitters and a
// rough tinted glass, whose program is the largest (docs/gpu_devices.md,
// "Driver compile budget").
//   prism_gpu_program_budget_test <textured_emitters_glass.usda>
#include "../src/device/gpu/gpu_scene.h"
#include "../src/device/gpu/gpu_materials.h"
#include "../src/device/gpu/hlsl_compiler.h"
#include "../src/device/vulkan/vulkan_material_calls.h"
#include "../src/mdl_material.h"
#include "../src/mesh/usd_mesh.h"
#include "../src/scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <thread>
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

void SetVariable(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

size_t Count(std::string const& text, std::string const& pattern)
{
    size_t count = 0;
    for (size_t at = text.find(pattern); at != std::string::npos; at = text.find(pattern, at + 1)) ++count;
    return count;
}

// The most driver compiles of `spirvBytes` holding a CompileMemorySlot at once, of `threads` started together.
int MostAtOnce(size_t spirvBytes, int threads)
{
    std::atomic<int> running{0}, most{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i)
        workers.emplace_back([&] {
            prism::device::vulkan::CompileMemorySlot slot(prism::device::vulkan::DriverCompileBytes(spirvBytes));
            const int now = ++running;
            int seen = most.load();
            while (now > seen && !most.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            --running;
        });
    for (auto& worker : workers) worker.join();
    return most.load();
}
} // namespace

int main(int argc, char** argv)
{
    using namespace prism::device;
    namespace vk = prism::device::vulkan;
    constexpr size_t MiB = size_t(1) << 20;

    // The driver's compile memory grows with the square of the SPIR-V size
    // (measured: 15 MB took 5-6 GB) and the limit refuses what cannot fit.
    check(vk::DriverCompileBytes(15 * MiB) > (uint64_t(4) << 30) &&
              vk::DriverCompileBytes(15 * MiB) < (uint64_t(7) << 30),
          "driver compile estimate of 15 MiB of SPIR-V");
    check(vk::DriverCompileBytes(2 * MiB) < (uint64_t(512) << 20), "driver compile estimate of 2 MiB");
    // DXC's peak for the largest OpenPBR callable source (2671 KB) was 1740 MiB.
    check(vk::DxcCompileBytes(2671 * 1024) >= (uint64_t(1740) << 20), "DXC estimate covers the measured peak");
    check(vk::DriverSpirvLimitError(size_t(vk::kDefaultMaxDriverSpirvMib) * MiB, "callable").empty() ||
              vk::DriverSpirvLimitError(size_t(vk::kDefaultMaxDriverSpirvMib) * MiB, "callable")
                  .find("needs about") != std::string::npos,
          "the default limit itself passes the size check");
    check(vk::DriverSpirvLimitError((vk::kDefaultMaxDriverSpirvMib + 1) * MiB, "callable")
              .find("too large for the driver") != std::string::npos,
          "SPIR-V above the limit is refused");

    // Concurrent driver compiles share the budget: two that do not fit
    // together run one after the other; one larger than the budget runs alone;
    // small ones run together.
    SetVariable("PRISM_VULKAN_DRIVER_MEMORY_MIB", "1000");
    check(vk::DriverMemoryBudget() == 1000 * MiB, "PRISM_VULKAN_DRIVER_MEMORY_MIB sets the budget");
    check(MostAtOnce(4 * MiB, 2) == 1, "two 640 MiB compiles in a 1000 MiB budget run one at a time");
    check(MostAtOnce(8 * MiB, 1) == 1, "a compile larger than the budget still runs");
    check(MostAtOnce(MiB / 4, 3) == 3, "three small compiles run together");
    SetVariable("PRISM_VULKAN_DRIVER_MEMORY_MIB", nullptr);

    if (argc < 2) {
        std::cerr << "usage: prism_gpu_program_budget_test <textured_emitters_glass.usda>\n";
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
        gpu::GpuSceneData data = gpu::BuildGpuScene(scene);
        check(data.mdlMaterials.size() == 5, std::format("5 MDL materials, not {}", data.mdlMaterials.size()));
        size_t varying = 0;
        for (auto const* material : data.mdlMaterials) varying += material->variesEmission() ? 1 : 0;
        check(varying == 3, std::format("3 materials of varying emission, not {}", varying));

        // Per-program callables (the layout of large material sets).
        gpu::MdlShaderCode code = gpu::GenerateProgramShaderCode(data.mdlMaterials);
        check(code.success && code.perProgram && code.varyingEmitters, "per-program code: " + code.error);
        std::vector<std::string> sources;
        std::string error;
        check(vk::MaterialCallSources(code, false, &sources, &error) && sources.size() == code.programs.size() + 1,
              "callable sources: " + error);
        check(sources.size() > 0 && sources[0].find("#define PRISM_VARYING_EMITTERS 1") != std::string::npos,
              "the raygen shader evaluates varying emitters");
        size_t largest = 0, smallest = SIZE_MAX;
        for (size_t i = 1; i < sources.size(); ++i) {
            // One BSDF evaluation call site: DXC inlines each call of the
            // program's code (a second one tripled the glass's SPIR-V).
            check(Count(sources[i], "evaluate(shading, ") == 1,
                  std::format("callable {}: one call site of the BSDF evaluation", i - 1));
            std::vector<char> spirv;
            const bool ok = gpu::CompileHlsl(sources[i], gpu::HlslTarget::Spirv, gpu::ShaderCacheDirectory("test"),
                                             &spirv, &error, gpu::HlslStage{vk::kMaterialCallProfile, ""});
            check(ok && spirv.size() > 1000, std::format("callable {} compiles: {}", i - 1, error));
            check(vk::DriverSpirvLimitError(spirv.size(), "callable").empty(),
                  std::format("callable {} ({} KB) fits the driver", i - 1, spirv.size() >> 10));
            largest = std::max(largest, spirv.size());
            smallest = std::min(smallest, spirv.size());
            std::cout << std::format("callable {}: {} KB HLSL, {} KB SPIR-V, driver estimate {} MiB\n", i - 1,
                                     sources[i].size() >> 10, spirv.size() >> 10,
                                     vk::DriverCompileBytes(spirv.size()) >> 20);
        }
        // The glass is the largest program: 8.7 MB of SPIR-V, 12.1 MB with
        // two evaluation call sites.
        check(largest < 10 * MiB, std::format("largest callable {} KB below 10 MiB", largest >> 10));
        check(largest > 3 * smallest, "the glass callable is the largest by far");
        // A lower limit refuses it before any pipeline is created.
        SetVariable("PRISM_VULKAN_MAX_SPIRV_MIB", "4");
        check(vk::DriverSpirvLimitError(largest, "material callable").find("too large for the driver") !=
                  std::string::npos,
              "PRISM_VULKAN_MAX_SPIRV_MIB=4 refuses the glass callable");
        SetVariable("PRISM_VULKAN_MAX_SPIRV_MIB", nullptr);
    }
    rtcReleaseDevice(device);
    if (failures) return 1;
    std::cout << "GPU program budget: all checks passed\n";
    return 0;
}
