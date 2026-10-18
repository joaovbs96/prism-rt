#include "../../src/hydra/materials.h"
#include "../../src/mdl/context.h"
#include "../../src/mdl_material.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/utils.h>
#include <pxr/usd/sdf/assetPath.h>

#include <filesystem>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>
#include <fstream>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace prism::hydra;

namespace {

HdContainerDataSourceHandle MakeMaterial(bool materialXContext,
                                         const std::string& texturePath,
                                         const std::string& resolvedTexturePath,
                                         float metallic)
{
    HdMaterialNode preview;
    preview.path = SdfPath("/Looks/Test/Preview");
    preview.identifier = materialXContext
        ? TfToken("ND_UsdPreviewSurface_surfaceshader")
        : TfToken("UsdPreviewSurface");
    preview.parameters[TfToken("diffuseColor")] = VtValue(GfVec3f(0.6f, 0.3f, 0.1f));
    preview.parameters[TfToken("roughness")] = VtValue(0.42f);
    preview.parameters[TfToken("metallic")] = VtValue(metallic);
    preview.parameters[TfToken("normal")] = VtValue(GfVec3f(0.0f, 0.0f, 1.0f));

    HdMaterialNode texture;
    texture.path = SdfPath("/Looks/Test/Albedo");
    texture.identifier = materialXContext
        ? TfToken("ND_UsdUVTexture_23") : TfToken("UsdUVTexture");
    texture.parameters[TfToken("file")] =
        VtValue(SdfAssetPath(texturePath, resolvedTexturePath));
    texture.parameters[TfToken("wrapS")] = VtValue(TfToken("repeat"));
    texture.parameters[TfToken("wrapT")] = VtValue(TfToken("repeat"));
    texture.parameters[TfToken("scale")] = VtValue(GfVec4f(1.0f));
    texture.parameters[TfToken("bias")] = VtValue(GfVec4f(0.0f));

    HdMaterialNode transform;
    transform.path = SdfPath("/Looks/Test/UVTransform");
    transform.identifier = materialXContext
        ? TfToken("ND_UsdTransform2d") : TfToken("UsdTransform2d");
    transform.parameters[TfToken("translation")] = VtValue(GfVec2f(0.1f, 0.2f));
    transform.parameters[TfToken("rotation")] = VtValue(5.0f);
    transform.parameters[TfToken("scale")] = VtValue(GfVec2f(0.9f, 1.1f));

    HdMaterialNode uv;
    uv.path = SdfPath("/Looks/Test/UV");
    uv.identifier = materialXContext
        ? TfToken("ND_UsdPrimvarReader_vector2") : TfToken("UsdPrimvarReader_float2");
    uv.parameters[TfToken("varname")] = VtValue(TfToken("st"));

    HdMaterialNetwork network;
    network.nodes = {uv, transform, texture, preview};
    HdMaterialRelationship uvToTransform;
    uvToTransform.inputId = uv.path;
    uvToTransform.inputName = TfToken("result");
    uvToTransform.outputId = transform.path;
    uvToTransform.outputName = TfToken("in");
    network.relationships.push_back(uvToTransform);

    HdMaterialRelationship transformToTexture;
    transformToTexture.inputId = transform.path;
    transformToTexture.inputName = TfToken("result");
    transformToTexture.outputId = texture.path;
    transformToTexture.outputName = TfToken("st");
    network.relationships.push_back(transformToTexture);

    HdMaterialRelationship texToDiffuse;
    texToDiffuse.inputId = texture.path;
    texToDiffuse.inputName = TfToken("rgb");
    texToDiffuse.outputId = preview.path;
    texToDiffuse.outputName = TfToken("diffuseColor");
    network.relationships.push_back(texToDiffuse);

    HdMaterialRelationship texToRoughness = texToDiffuse;
    texToRoughness.inputName = TfToken("r");
    texToRoughness.outputName = TfToken("roughness");
    network.relationships.push_back(texToRoughness);

    HdMaterialRelationship texToOpacity = texToDiffuse;
    texToOpacity.inputName = TfToken("a");
    texToOpacity.outputName = TfToken("opacity");
    network.relationships.push_back(texToOpacity);

    HdMaterialNetworkMap networkMap;
    networkMap.map[TfToken("surface")] = network;
    HdContainerDataSourceHandle networkDs =
        pxr::HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(networkMap);
    const TfToken context = materialXContext ? TfToken("mtlx")
        : HdMaterialSchemaTokens->universalRenderContext;
    HdDataSourceBaseHandle networkBase = networkDs;
    HdContainerDataSourceHandle materialDs = HdMaterialSchema::BuildRetained(
        1, &context, &networkBase);
    return HdRetainedContainerDataSource::New(
        HdMaterialSchemaTokens->material, materialDs);
}

} // namespace

int main()
{
    const std::filesystem::path libraries =
        std::getenv("PRISM_MATERIALX_LIBRARY_PATH")
            ? std::getenv("PRISM_MATERIALX_LIBRARY_PATH") : std::string();
    if (libraries.empty()) {
        std::cerr << "PRISM_MATERIALX_LIBRARY_PATH must point to MaterialX libraries\n";
        return 2;
    }

    // Concurrent runs (parallel ctest, other checkouts) must not share the
    // fixed temp texture or the persistent cache they mutate and invalidate.
    const std::string runTag = std::to_string(static_cast<unsigned long>(
#ifdef _WIN32
        _getpid()
#else
        getpid()
#endif
        ));
    const std::string testCache = (std::filesystem::temp_directory_path() /
        ("prism_hydra_materials_test_cache_" + runTag)).string();
#ifdef _WIN32
    _putenv_s("PRISM_CACHE_DIR", testCache.c_str());
#else
    setenv("PRISM_CACHE_DIR", testCache.c_str(), 1);
#endif
    const std::filesystem::path texture = std::filesystem::temp_directory_path() /
        ("prism_hydra_material_test_" + runTag + ".ppm");
    {
        std::ofstream image(texture, std::ios::binary | std::ios::trunc);
        image << "P3\n1 1\n255\n160 90 40\n";
    }
    mdl::Context& context = SharedMdlContext();
    if (!context.is_valid()) {
        std::cerr << "MDL SDK unavailable: " << context.get_error() << "\n";
        return 3;
    }

    // A failed MDL load must remain a safe, visible error material rather
    // than exposing partially initialized backend state.
    MDLMaterial missingDocument(
        (std::filesystem::temp_directory_path() / "prism_missing_hydra_material.mtlx").string(),
        context, libraries.string(), (libraries / "mdl").string());
    if (missingDocument.is_valid()) {
        std::cerr << "Missing MaterialX document unexpectedly compiled\n";
        return 13;
    }
    unsigned int fallbackSeed = 0;
    RTCRayHit missRay{};
    missRay.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    const BRDFSample fallback = missingDocument.sample(RayHit(missRay), fallbackSeed);
    if (fallback.emission.r() != 1.0f || fallback.emission.g() != 0.0f ||
        fallback.emission.b() != 1.0f || fallback.scattered) {
        std::cerr << "Failed MDL material did not produce the magenta fallback\n";
        return 14;
    }

    MaterialService service(&context, libraries.string(),
        (libraries / "mdl").string());

    const SdfPath materialPath("/Looks/Test");
    const std::string relativeTexture = texture.filename().string();
    const auto universal = MakeMaterial(false, relativeTexture, texture.string(), 0.1f);
    const auto first = service.Resolve(universal, materialPath);
    if (!first) {
        std::cerr << "UsdPreviewSurface graph did not produce a material\n";
        return 4;
    }
    const auto firstMdl = std::dynamic_pointer_cast<MDLMaterial>(first);
    if (!firstMdl || !firstMdl->is_valid()) {
        std::cerr << "UsdPreviewSurface graph returned an invalid MDL material\n";
        return 8;
    }
    if (service.Resolve(universal, materialPath) != first) {
        std::cerr << "Identical material graph did not reuse cached MDL material\n";
        return 5;
    }
    const auto cachedStats = service.GetStats();
    if (service.Resolve(MakeMaterial(false, relativeTexture, texture.string(), 0.1f),
                        materialPath) != first ||
        service.GetStats().conversionSeconds != cachedStats.conversionSeconds ||
        service.GetStats().resourceReadSeconds != cachedStats.resourceReadSeconds ||
        service.GetStats().pathCacheHits != cachedStats.pathCacheHits + 1) {
        std::cerr << "Fresh data-source wrapper repeated material conversion/resource reads\n";
        return 15;
    }
    service.Invalidate(materialPath);
    const auto changed = service.Resolve(
        MakeMaterial(false, relativeTexture, texture.string(), 0.47f), materialPath);
    if (!changed || changed == first) {
        std::cerr << "Edited material graph did not invalidate the cache\n";
        return 6;
    }
    const auto changedMdl = std::dynamic_pointer_cast<MDLMaterial>(changed);
    if (!changedMdl || !changedMdl->is_valid()) {
        std::cerr << "Edited PreviewSurface graph failed MDL compilation\n";
        return 9;
    }

    {
        std::ofstream image(texture, std::ios::binary | std::ios::trunc);
        image << "P3\n1 1\n255\n190 70 25\n";
    }
    service.Invalidate(materialPath);
    const auto changedTexture = service.Resolve(
        MakeMaterial(false, relativeTexture, texture.string(), 0.1f), materialPath);
    if (!changedTexture || changedTexture == first) {
        std::cerr << "Texture content change did not invalidate the material cache\n";
        return 11;
    }

    service.Invalidate(materialPath);
    const auto mtlx = service.Resolve(
        MakeMaterial(true, relativeTexture, texture.string(), 0.1f), materialPath);
    if (!mtlx) {
        std::cerr << "MaterialX render-context graph did not produce a material\n";
        return 7;
    }
    const auto mtlxMdl = std::dynamic_pointer_cast<MDLMaterial>(mtlx);
    if (!mtlxMdl || !mtlxMdl->is_valid()) {
        std::cerr << "MaterialX-context graph failed MDL compilation\n";
        return 10;
    }

    std::shared_ptr<Material> survivor;
    {
        MaterialService transient(&context, libraries.string(),
            (libraries / "mdl").string());
        survivor = transient.Resolve(universal, materialPath);
    }
    MaterialService replacement(&context, libraries.string(),
        (libraries / "mdl").string());
    const auto replacementBefore = replacement.GetStats();
    const auto afterReplacement = replacement.Resolve(universal, materialPath);
    const auto replacementAfter = replacement.GetStats();
    const auto survivorMdl = std::dynamic_pointer_cast<MDLMaterial>(survivor);
    if (afterReplacement != changedTexture || !survivorMdl || !survivorMdl->is_valid() ||
        replacementAfter.conversionSeconds != replacementBefore.conversionSeconds ||
        replacementAfter.resourceReadSeconds != replacementBefore.resourceReadSeconds ||
        replacementAfter.earlyCacheHits != replacementBefore.earlyCacheHits + 1) {
        std::cerr << "Shared MDL cache/context did not survive service replacement\n";
        return 12;
    }
    // Invalidation drops another service's association in the same scene scope;
    // unchanged recaptured content must still be reused.
    service.Invalidate(materialPath);
    const auto crossInvalidationBefore = replacement.GetStats();
    if (replacement.Resolve(universal, materialPath) != changedTexture) {
        std::cerr << "Cross-service invalidation changed the resolved material\n";
        return 16;
    }
    const auto crossInvalidationAfter = replacement.GetStats();
    if (crossInvalidationAfter.pathCacheHits != crossInvalidationBefore.pathCacheHits ||
        crossInvalidationAfter.conversionSeconds != crossInvalidationBefore.conversionSeconds ||
        crossInvalidationAfter.earlyCacheHits !=
            crossInvalidationBefore.earlyCacheHits + 1) {
        std::cerr << "Cross-service invalidation did not recapture and validate exact content\n";
        return 17;
    }
    const auto otherTexture = texture.parent_path() / ("prism_hydra_unrelated_texture_" + runTag + ".ppm");
    std::filesystem::copy_file(texture, otherTexture,
        std::filesystem::copy_options::overwrite_existing);
    const SdfPath otherPath("/Looks/Unrelated");
    const auto otherSource = MakeMaterial(true, otherTexture.string(), otherTexture.string(), 0.1f);
    const auto unrelated = service.Resolve(otherSource, otherPath);
    const auto reloadSource = MakeMaterial(true, relativeTexture, texture.string(), 0.1f);
    const auto retained = std::dynamic_pointer_cast<MDLMaterial>(service.Resolve(reloadSource, materialPath));
    if (!retained || !retained->is_valid()) return 18;
    RTCRayHit embreeHit{};
    embreeHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    RayHit hit(embreeHit);
    hit.p = Float3(0.0f); hit.d = Float3(0.0f, 0.0f, -1.0f);
    hit.n = hit.n_g = hit.n_s = Float3(0.0f, 0.0f, 1.0f);
    hit.u = hit.v = 0.5f; hit.front_face = true;
    const auto oldEvaluation = retained->evaluate(hit, Float3(0.0f, 0.0f, 1.0f));
    Material::PreparedShadingContext prepared;
    if (!retained->prepare(hit, prepared)) {
        std::cerr << "Textured opacity material did not prepare its shading state\n";
        return 19;
    }
    const auto preparedEvaluation = retained->evaluatePrepared(
        hit, Float3(0.0f, 0.0f, 1.0f), prepared);
    if (!preparedEvaluation.valid) {
        std::cerr << "Prepared textured opacity evaluation failed\n";
        return 19;
    }
    const auto stamp = std::filesystem::last_write_time(texture);
    const auto oldSize = std::filesystem::file_size(texture);
    {
        std::ofstream image(texture, std::ios::binary | std::ios::trunc);
        image << "P3\n1 1\n255\n25 70 190\n";
    }
    std::filesystem::last_write_time(texture, stamp);
    if (std::filesystem::file_size(texture) != oldSize ||
        service.ReloadResource(texture.string()) != std::vector<SdfPath>{materialPath}) {
        std::cerr << "Resource reload did not isolate its dependent material\n";
        return 19;
    }
    const auto reloadBefore = service.GetStats();
    if (service.Resolve(otherSource, otherPath) != unrelated ||
        service.GetStats().pathCacheHits != reloadBefore.pathCacheHits + 1) {
        std::cerr << "Resource reload invalidated an unrelated source\n";
        return 20;
    }
    const auto reloaded = std::dynamic_pointer_cast<MDLMaterial>(service.Resolve(reloadSource, materialPath));
    if (!reloaded || !reloaded->is_valid() || reloaded == retained ||
        reloaded->get_target_code().program != retained->get_target_code().program ||
        service.GetStats().sdkTranslationCalls != reloadBefore.sdkTranslationCalls) {
        std::cerr << "Resource reload did not retain the compatible program\n";
        return 21;
    }
    const auto retainedEvaluation = retained->evaluate(hit, Float3(0.0f, 0.0f, 1.0f));
    const auto newEvaluation = reloaded->evaluate(hit, Float3(0.0f, 0.0f, 1.0f));
    float difference = 0;
    for (int i = 0; i < 3; ++i) {
        if (oldEvaluation.value[i] != retainedEvaluation.value[i]) {
            std::cerr << "Reload mutated a retained binding\n";
            return 22;
        }
        difference += std::abs(oldEvaluation.value[i] - newEvaluation.value[i]);
    }
    if (!oldEvaluation.valid || !newEvaluation.valid || difference < 1.0e-5f) {
        std::cerr << "Reload did not update shaded texture values\n";
        return 23;
    }
    std::error_code ec;
    std::filesystem::remove(otherTexture, ec);
    std::filesystem::remove(texture, ec);
    std::filesystem::remove_all(testCache, ec);
    std::cout << "Hydra material conversion and cache tests passed\n";
    return 0;
}
