// A scene of textured (varying) emitters raises the varying-emitter note while
// its materials load, so the Vulkan device can compile the raygen shader
// variant with PRISM_VARYING_EMITTERS meanwhile; a scene without them does
// not (docs/gpu_devices.md, "Raygen prewarm"). CPU only, no device.
//   prism_raygen_prewarm_test <textured_emitter.usda> <scene without varying emitters.usda>
#include "../src/device/gpu/gpu_scene.h"
#include "../src/mdl_material.h"
#include "../src/varying_emitter_signal.h"
#include "scene_api_test_util.h"

#include <cstdlib>
#include <format>
#include <iostream>

namespace {
int failures = 0;
// The renderer, whose note of varying emission (RendererContext::varyingEmitters)
// its materials raise.
std::unique_ptr<prism::Renderer> gRenderer;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// Loads `path`; returns the notes it raised and the materials of varying emission it holds.
void Load(const char* path, uint64_t* notes, size_t* varying, size_t* materials)
{
    using namespace prism;
    using namespace prism::device;
    auto const& signal = gRenderer->context()->varyingEmitters();
    const uint64_t before = signal->Notes();
    prism::Scene api = gRenderer->createScene();
    SceneManager& scene = prism::test::LoadUsd(api, path);
    gpu::GpuSceneData data = gpu::BuildGpuScene(scene);
    *materials = data.mdlMaterials.size();
    *varying = 0;
    for (auto const* material : data.mdlMaterials) *varying += material->variesEmission() ? 1 : 0;
    *notes = signal->Notes() - before;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: prism_raygen_prewarm_test <textured emitter scene> <scene without them>\n";
        return 2;
    }
    gRenderer = std::make_unique<prism::Renderer>(prism::test::TestRendererOptions());
    auto const& gSignal = gRenderer->context()->varyingEmitters();

    // A subscriber hears each note.
    int heard = 0;
    const int token = gSignal->Subscribe([&] { ++heard; });

    uint64_t notes = 0;
    size_t varying = 0, materials = 0;
    Load(argv[1], &notes, &varying, &materials);
    std::cout << std::format("{}: {} materials, {} varying, {} notes\n", argv[1], materials, varying, notes);
    check(varying > 0, "the textured emitter scene has materials of varying emission");
    check(notes >= varying, "each material of varying emission is noted (the raygen variant is prewarmed)");
    check(heard > 0 && uint64_t(heard) == notes, "subscribers hear the notes");

    heard = 0;
    Load(argv[2], &notes, &varying, &materials);
    std::cout << std::format("{}: {} materials, {} varying, {} notes\n", argv[2], materials, varying, notes);
    check(materials > 0, "the control scene has MDL materials");
    check(varying == 0, "the control scene has no material of varying emission");
    check(notes == 0 && heard == 0, "no note without varying emitters (only the plain raygen is prewarmed)");

    gSignal->Unsubscribe(token);
    gRenderer.reset();
    if (failures) return 1;
    std::cout << "raygen prewarm: all checks passed\n";
    return 0;
}
