// RendererOptions::FromEnvironment reads the API-level environment variables
// (docs/sdk_design.md, 2.2) and nothing below the front ends reads them: a
// default-constructed RendererOptions and the colour management ignore them.
#include "../src/renderer/renderer_options.h"
#include "../src/device/device.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
} // namespace

int main()
{
    setEnv("OCIO", "C:/no/such/config.ocio");
    setEnv("PRISM_MATERIALX_LIBRARY_PATH", "/libraries");
    setEnv("PRISM_MATERIALX_MDL", "/libraries/mdl");
    setEnv("PRISM_MDL_COMPILE_MODE", "class");
    setEnv("PRISM_MATERIAL_PREWARM", "0");
    setEnv("PRISM_TEXTURE_PREPARE", "serial");
    setEnv("PRISM_MATERIAL_THREADS", "3");
    setEnv("PRISM_TEXTURE_THREADS", "-2");
    setEnv("PRISM_DISABLE_VULKAN", "1");
    setEnv("PRISM_DISABLE_METAL", "1");
    setEnv("PRISM_CACHE_DIR", "/cache-root");

    const auto options = prism::RendererOptions::FromEnvironment();
    check(options.color.config == "C:/no/such/config.ocio", "OCIO is the colour config");
    check(options.materials.materialxLibraries == "/libraries" &&
          options.materials.materialxMdlPath == "/libraries/mdl", "MaterialX paths");
    check(options.materials.compileMode == prism::MaterialOptions::CompileMode::Class, "compile mode");
    check(!options.materials.prewarm, "PRISM_MATERIAL_PREWARM=0 disables the pre-warm");
    check(!options.materials.parallelTexturePreparation, "PRISM_TEXTURE_PREPARE=serial");
    check(options.limits.materialThreads == 3 && options.limits.MaterialWorkers() == 3, "material threads");
    check(options.limits.textureThreads == 0 && options.limits.TexturePreparationThreads() >= 1,
          "an invalid texture thread count takes the default");
    check(options.devices.disableVulkan && options.devices.disableMetal, "disabled devices");
    check(options.CacheRoot() == std::filesystem::path("/cache-root"), "PRISM_CACHE_DIR is the cache root");

    // Without FromEnvironment nothing reads the environment.
    const prism::RendererOptions defaults;
    check(defaults.CacheRoot() == prism::DefaultCacheDirectory(), "default cache root");
    check(defaults.materials.materialxLibraries.empty() && defaults.materials.prewarm &&
          defaults.limits.materialThreads == 0 && !defaults.devices.disableVulkan,
          "default options are the built-in defaults");
    try {
        // $OCIO names a missing config: only an explicit config may load it.
        const auto color = prism::color::ColorManagement::Create({});
        check(color->config() == prism::color::kDefaultConfig, "an empty config is the built-in one");
    } catch (std::exception const& error) {
        check(false, std::string("the colour management read $OCIO: ") + error.what());
    }

    // Disabled devices are not opened: the CPU device with a note.
    using namespace prism::device;
    std::string note;
    auto device = CreateDevice(DevicePreference::Vulkan, options.devices, &note);
    check(device && device->Info().kind == DeviceKind::Cpu &&
          note.find("disabled by PRISM_DISABLE_VULKAN") != std::string::npos,
          "a disabled Vulkan device falls back to the CPU: " + note);
    const auto available = AvailableDevices(options.devices);
    check(available.size() == 1 && available.front().kind == DeviceKind::Cpu, "only the CPU is available");

    if (failures) return 1;
    std::cout << "renderer options: all checks passed\n";
    return 0;
}
