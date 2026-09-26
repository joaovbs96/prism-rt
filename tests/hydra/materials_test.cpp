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

    HdMaterialRelationship texToNormal = texToDiffuse;
    texToNormal.outputName = TfToken("normal");
    network.relationships.push_back(texToNormal);

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

    const std::filesystem::path texture = std::filesystem::temp_directory_path() /
        "prism_hydra_material_test.ppm";
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
    const auto changedTexture = service.Resolve(
        MakeMaterial(false, relativeTexture, texture.string(), 0.1f), materialPath);
    if (!changedTexture || changedTexture == first) {
        std::cerr << "Texture content change did not invalidate the material cache\n";
        return 11;
    }

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
    const auto afterReplacement = replacement.Resolve(universal, materialPath);
    const auto survivorMdl = std::dynamic_pointer_cast<MDLMaterial>(survivor);
    if (afterReplacement != changedTexture || !survivorMdl || !survivorMdl->is_valid()) {
        std::cerr << "Shared MDL cache/context did not survive service replacement\n";
        return 12;
    }
    std::error_code ec;
    std::filesystem::remove(texture, ec);
    std::cout << "Hydra material conversion and cache tests passed\n";
    return 0;
}
