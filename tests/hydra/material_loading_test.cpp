#include "../../src/hydra/materials.h"
#include "../../src/mdl/context.h"
#include "../../src/mdl_material.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/utils.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace prism::hydra;

namespace {
HdContainerDataSourceHandle MakeColorMaterial(const SdfPath& path,
                                               const GfVec3f& color)
{
    HdMaterialNode surface;
    surface.path = path.AppendChild(TfToken("Surface"));
    surface.identifier = TfToken("ND_open_pbr_surface_surfaceshader");
    surface.parameters[TfToken("base_color")] = VtValue(color);
    surface.parameters[TfToken("specular_roughness")] = VtValue(0.45f);
    HdMaterialNetwork network;
    network.nodes.push_back(surface);
    HdMaterialNetworkMap map;
    map.map[TfToken("surface")] = network;
    HdContainerDataSourceHandle networkData =
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(map);
    HdDataSourceBaseHandle networkBase = networkData;
    const TfToken context("mtlx");
    HdContainerDataSourceHandle material = HdMaterialSchema::BuildRetained(
        1, &context, &networkBase);
    return HdRetainedContainerDataSource::New(
        HdMaterialSchemaTokens->material, material);
}
}

int main()
{
    const char* root = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    if (!root) return 2;
    mdl::Context& context = SharedMdlContext();
    if (!context.is_valid()) return 3;
    const std::filesystem::path libraries(root);
    MaterialService service(&context, libraries.string(),
                            (libraries / "mdl").string());
    const SdfPath redPath("/Looks/M0"), bluePath("/Looks/M99");
    std::vector<HdContainerDataSourceHandle> sources;
    std::vector<MaterialService::Request> requests;
    for (int i = 0; i < 100; ++i) {
        const SdfPath path("/Looks/M" + std::to_string(i));
        sources.push_back(MakeColorMaterial(path, GfVec3f(
            (i % 10) / 10.0f, ((i / 10) % 10) / 10.0f, 0.35f)));
        requests.push_back({sources.back(), path});
    }
    auto red = sources[0], blue = sources[99];
    // Fail quickly if two differently colored instances do not share class
    // code before paying the cost of the full 100-material stress batch.
    service.Prefetch({requests[0], requests[1]});
    if (service.GetStats().nativeJits != 1) {
        std::cerr << "two-material class reuse failed\n";
        return 4;
    }
    service.Prefetch(std::vector<MaterialService::Request>(
        requests.begin() + 2, requests.end()));
    auto redMaterial = service.Resolve(red, redPath);
    auto blueMaterial = service.Resolve(blue, bluePath);
    const auto stats = service.GetStats();
    if (!redMaterial || !blueMaterial || stats.jobs != 100 ||
        stats.pathCacheHits < 2 || stats.compilations != 100 ||
        stats.nativeJits != 1) {
        std::cerr << "parallel class-material reuse failed: jobs=" << stats.jobs
                  << " compiles=" << stats.compilations
                  << " jits=" << stats.nativeJits << "\n";
        return 5;
    }
    auto redMdl = std::dynamic_pointer_cast<MDLMaterial>(redMaterial);
    auto blueMdl = std::dynamic_pointer_cast<MDLMaterial>(blueMaterial);
    if (!redMdl || !blueMdl ||
        redMdl->get_target_code().target_code.get() !=
            blueMdl->get_target_code().target_code.get() ||
        redMdl->get_target_code().argument_data() ==
            blueMdl->get_target_code().argument_data()) {
        std::cerr << "class code or per-material argument block was not reused correctly\n";
        return 6;
    }
    RTCRayHit embreeHit{};
    embreeHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    RayHit hit(embreeHit);
    hit.p = Float3(0.0f); hit.d = Float3(0.0f, -1.0f, 0.0f);
    hit.n = hit.n_g = Float3(0.0f, 1.0f, 0.0f);
    hit.u = hit.v = 0.0f; hit.front_face = true;
    const BRDFEvaluation redEval = redMdl->evaluate(hit, Float3(0.0f, 1.0f, 0.0f));
    const BRDFEvaluation blueEval = blueMdl->evaluate(hit, Float3(0.0f, 1.0f, 0.0f));
    const float argumentDifference =
        std::abs(redEval.value.x() - blueEval.value.x()) +
        std::abs(redEval.value.y() - blueEval.value.y()) +
        std::abs(redEval.value.z() - blueEval.value.z());
    if (!redEval.valid || !blueEval.valid || argumentDifference < 1.0e-4f) {
        std::cerr << "class argument blocks did not preserve distinct base colors\n";
        return 7;
    }
    std::cout << "material stats: wall=" << stats.batchWallSeconds
              << " worker_cpu=" << stats.workerCpuSeconds
              << " conversion=" << stats.conversionSeconds
              << " generation=" << stats.generationSeconds
              << " class_compile=" << stats.classCompilationSeconds
              << " native_jit=" << stats.nativeJitSeconds << "\n";
    service.Invalidate(redPath);
    if (service.Resolve(red, redPath) != redMaterial) {
        std::cerr << "content cache was not reused after path invalidation\n";
        return 8;
    }
    service.InvalidateAll();
    std::cout << "Parallel material loading and class-code reuse passed\n";
    return 0;
}
