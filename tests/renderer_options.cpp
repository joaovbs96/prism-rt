// RendererOptions::FromEnvironment reads the API-level environment variables
// (docs/sdk_design.md, 2.2) and nothing below the front ends reads them: a
// default-constructed RendererOptions and the colour management ignore them.
#include "../src/renderer/renderer_context.h"
#include "../src/device/device.h"
#include "../src/resource_limits.h"
#include "../src/texture_service.h"

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
    setEnv("PRISM_DISABLE_GENERATION_CACHE", "1");
    setEnv("PRISM_MDL_RESOURCE_MODE", "builtin");
    setEnv("PRISM_MATERIAL_CACHE_MB", "100");
    setEnv("PRISM_PREPARATION_MB", "300");
    setEnv("PRISM_TEXTURE_CACHE_MB", "1024");
    setEnv("PRISM_DISK_CACHE_GB", "2.5");
    setEnv("PRISM_TEXTURE_COMPRESSION", "piz");
    setEnv("PRISM_TEXTURE_BACKGROUND_WRITES", "0");

    const auto options = prism::RendererOptions::FromEnvironment();
    check(!options.materials.generationCache && options.materials.mdlResourceMode == "builtin",
          "generation cache and resource mode");
    check(options.limits.materialCacheMiB == 100 && options.limits.preparationMiB == 300 &&
          options.limits.textureCacheMiB == 1024 && options.limits.diskCacheGiB == 2.5, "budgets");
    check(options.textures.compression == "piz" && !options.textures.backgroundWrites, "texture options");
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

    // The runtime (shared caches): the first renderer's cache root, the
    // largest admission and material cache budgets.
    prism::RendererOptions first, second;
    first.cacheDirectory = "/first-cache";
    first.limits.materialCacheMiB = 100;
    first.limits.preparationMiB = 300;
    second.cacheDirectory = "/second-cache";
    second.limits.materialCacheMiB = 700;
    second.limits.preparationMiB = 200;
    prism::RendererContext firstRenderer(first);
    prism::RendererContext secondRenderer(second);
    const auto runtime = prism::Runtime::Instance().settings();
    check(runtime.CacheRoot() == std::filesystem::path("/first-cache") &&
          prism::PersistentCacheRoot() == std::filesystem::path("/first-cache"),
          "the first renderer's cache root");
    check(runtime.materialCacheMiB == 700 && runtime.preparationMiB == 300, "the largest budgets");
    check(prism::TextureServiceOptions::For(second).cacheDirectory == std::filesystem::path("/first-cache"),
          "every renderer's textures under the runtime's root");

    // Diagnostics per renderer: the sink gets each warn-once message once per
    // logger; the environment's verbose switches are debug categories.
    setEnv("PRISM_MDL_VERBOSE", "1");
    const auto verbose = prism::RendererOptions::FromEnvironment();
    check(verbose.debugCategories == std::vector<std::string>{"mdl"}, "PRISM_MDL_VERBOSE is the mdl category");
    std::vector<std::string> messages;
    prism::RendererOptions logged;
    logged.log = [&](prism::LogLevel, std::string_view, std::string_view message) {
        messages.emplace_back(message);
    };
    prism::RendererContext loggedA(logged), loggedB(logged);
    for (int i = 0; i < 2; ++i) {
        loggedA.logger().warnOnce("test", "key", "once per renderer");
        loggedB.logger().warnOnce("test", "key", "once per renderer");
    }
    check(messages.size() == 2, "a warn-once message reaches each renderer's sink once");
    check(!loggedA.logger().enabled(prism::LogLevel::Debug, "mdl") &&
          prism::Logger({}, prism::LogLevel::Warning, {"mdl"}).enabled(prism::LogLevel::Debug, "mdl"),
          "debug output per category");
    {
        prism::logging::Bind bind(&loggedA.logger());
        check(&prism::logging::Current() == &loggedA.logger(), "the bound logger is the thread's");
    }
    check(&prism::logging::Current() == &prism::logging::Default(), "unbound threads use the default logger");

    // Progress per renderer: the free functions report to the model bound to
    // the calling thread only.
    {
        prism::progress::Bind bind(&loggedA.progress());
        prism::progress::AddTotal(prism::Phase::Materials, 3);
    }
    prism::progress::AddTotal(prism::Phase::Materials, 5);
    check(loggedA.progress().Read().phases[size_t(prism::Phase::Materials)].total == 3 &&
          loggedB.progress().Read().phases[size_t(prism::Phase::Materials)].total == 0,
          "progress reaches the bound renderer only");

    if (failures) return 1;
    std::cout << "renderer options: all checks passed\n";
    return 0;
}
