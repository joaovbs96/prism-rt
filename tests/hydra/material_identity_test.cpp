// Several renderers in one process share the compiled-material caches
// (docs/sdk_design.md, step 1d): the same material compiled for renderers
// that differ in working space or SSS fallback must give distinct, correct
// materials, in either order and through every path a material service
// takes (Resolve, and Prefetch with its compatible templates). A cache whose
// key lacked MaterialSettings::Identity would hand the second renderer the
// first one's material.
#include "../../src/materialx/material_service.h"
#include "../../src/mdl/context.h"
#include "../../src/mdl_material.h"
#include "../../src/renderer/renderer_options.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using prism::materials::MaterialService;

namespace {

void require(bool condition, std::string const& message)
{
    if (!condition) throw std::runtime_error(message);
}

// An open_pbr_surface material. base_color is tagged lin_rec709, so a
// renderer working in ACEScg converts it and one in linear Rec.709 does not.
HdContainerDataSourceHandle MakeMaterial(SdfPath const& path, GfVec3f color, float subsurface)
{
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    auto add = [&](char const* name, HdDataSourceBaseHandle value, char const* colorSpace = nullptr) {
        auto builder = HdMaterialNodeParameterSchema::Builder();
        builder.SetValue(HdSampledDataSource::Cast(value));
        if (colorSpace) builder.SetColorSpace(HdRetainedTypedSampledDataSource<TfToken>::New(TfToken(colorSpace)));
        names.emplace_back(name);
        values.push_back(builder.Build());
    };
    add("base_color", HdRetainedTypedSampledDataSource<GfVec3f>::New(color), "lin_rec709");
    add("base_weight", HdRetainedTypedSampledDataSource<float>::New(1.0f));
    add("specular_weight", HdRetainedTypedSampledDataSource<float>::New(0.0f));
    add("subsurface_weight", HdRetainedTypedSampledDataSource<float>::New(subsurface));
    const SdfPath surface = path.AppendChild(TfToken("Surface"));
    HdDataSourceBaseHandle node = HdMaterialNodeSchema::Builder()
        .SetNodeIdentifier(HdRetainedTypedSampledDataSource<TfToken>::New(
            TfToken("ND_open_pbr_surface_surfaceshader")))
        .SetParameters(HdRetainedContainerDataSource::New(names.size(), names.data(), values.data()))
        .Build();
    const TfToken nodeName = surface.GetAsToken();
    const TfToken terminalName("surface");
    HdDataSourceBaseHandle terminal = HdMaterialConnectionSchema::Builder()
        .SetUpstreamNodePath(HdRetainedTypedSampledDataSource<TfToken>::New(nodeName))
        .SetUpstreamNodeOutputName(HdRetainedTypedSampledDataSource<TfToken>::New(TfToken("out")))
        .Build();
    HdDataSourceBaseHandle network = HdMaterialNetworkSchema::Builder()
        .SetNodes(HdRetainedContainerDataSource::New(1, &nodeName, &node))
        .SetTerminals(HdRetainedContainerDataSource::New(1, &terminalName, &terminal))
        .Build();
    const TfToken context("mtlx");
    return HdRetainedContainerDataSource::New(
        HdMaterialSchemaTokens->material, HdMaterialSchema::BuildRetained(1, &context, &network));
}

// The BSDF (times the cosine) straight back up, lit from straight above:
// the diffuse albedo over pi for these materials, linear in base_color.
Color3 Reflection(std::shared_ptr<Material> const& material)
{
    auto* mdl = dynamic_cast<MDLMaterial*>(material.get());
    require(mdl && mdl->is_valid(), "the material did not compile");
    RTCRayHit embreeHit{};
    embreeHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    RayHit hit(embreeHit);
    hit.p = Float3(0.0f);
    hit.n = hit.n_g = Float3(0.0f, 1.0f, 0.0f);
    hit.u = 0.3f; hit.v = 0.7f;
    hit.front_face = true;
    hit.d = Float3(0.0f, -1.0f, 0.0f);
    const auto value = mdl->evaluate(hit, Float3(0.0f, 1.0f, 0.0f));
    require(value.valid, "the material did not evaluate");
    return value.value;
}

bool Near(Color3 a, Color3 b, float tolerance)
{
    for (int c = 0; c < 3; ++c)
        if (!(std::abs(a[c] - b[c]) <= tolerance * std::max(1e-3f, std::abs(b[c])))) return false;
    return true;
}

std::string Text(Color3 c)
{
    return "(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) + ")";
}

struct Renderer {
    std::shared_ptr<const prism::color::ColorManagement> color;
    bool sssFallback = false;
    prism::MaterialSettings settings() const
    {
        prism::MaterialSettings result{color, prism::PersistentCacheRoot()};
        result.sssFallback = sssFallback;
        return result;
    }
};

} // namespace

int main()
{
    try {
        const char* root = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        if (!root || !*root) return 77;
        // A cold cache of its own: nothing compiled by another test answers.
        const auto cache = std::filesystem::temp_directory_path() /
            ("prism-material-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(cache);
#ifdef _WIN32
        _putenv_s("PRISM_CACHE_DIR", cache.string().c_str());
#else
        setenv("PRISM_CACHE_DIR", cache.string().c_str(), 1);
#endif
        mdl::Context& context = prism::materials::SharedMdlContext();
        if (!context.is_valid()) throw std::runtime_error(context.get_error());
        auto options = prism::RendererOptions::FromEnvironment();
        options.materials.materialxLibraries = root;
        options.materials.materialxMdlPath = (std::filesystem::path(root) / "mdl").string();
        // The material service always folds; its templates share programs.
        options.materials.compileMode = prism::MaterialOptions::CompileMode::Auto;

        prism::color::Options rec709Options;
        rec709Options.workingSpace = "lin_rec709";
        const Renderer aces{prism::color::ColorManagement::Create({})};
        const Renderer rec709{prism::color::ColorManagement::Create(rec709Options)};
        require(!aces.color->rec709() && rec709.color->rec709(), "the working spaces are ACEScg and linear Rec.709");
        const auto& toAces = aces.color->fromRec709();
        auto service = [&](Renderer const& renderer) {
            return std::make_unique<MaterialService>(&context, options, renderer.settings());
        };

        // Working spaces, Resolve: ACEScg first, then Rec.709 (and back).
        const SdfPath warm("/Looks/Warm");
        const auto warmSource = MakeMaterial(warm, GfVec3f(0.8f, 0.3f, 0.1f), 0.0f);
        auto acesService = service(aces);
        const auto inAces = acesService->Resolve(warmSource, warm);
        auto rec709Service = service(rec709);
        const auto inRec709 = rec709Service->Resolve(warmSource, warm);
        require(inAces && inRec709 && inAces != inRec709,
                "a renderer in another working space got the first renderer's material");
        const Color3 a = Reflection(inAces), r = Reflection(inRec709);
        require(Near(a, prism::color::Apply(toAces, r), 2e-3f),
                "the ACEScg material is not the Rec.709 one converted: " + Text(a) + " vs " +
                Text(prism::color::Apply(toAces, r)));
        require(!Near(a, r, 2e-2f), "the working space did not change the material: " + Text(a));
        auto acesAgain = service(aces);
        require(Near(Reflection(acesAgain->Resolve(warmSource, warm)), a, 1e-5f),
                "the ACEScg material changed after a Rec.709 one was compiled");
        std::cout << "working spaces: ACEScg " << Text(a) << ", Rec.709 " << Text(r) << '\n';

        // Working spaces, Prefetch: two materials of one structure (a
        // compatible template) in Rec.709 first, then in ACEScg.
        const SdfPath first("/Looks/First"), second("/Looks/Second");
        const auto firstSource = MakeMaterial(first, GfVec3f(0.2f, 0.6f, 0.3f), 0.0f);
        const auto secondSource = MakeMaterial(second, GfVec3f(0.7f, 0.5f, 0.9f), 0.0f);
        const std::vector<MaterialService::Request> batch{{firstSource, first}, {secondSource, second}};
        auto rec709Batch = service(rec709);
        rec709Batch->Prefetch(batch);
        auto acesBatch = service(aces);
        acesBatch->Prefetch(batch);
        for (auto const& [source, path] : {std::pair{firstSource, first}, std::pair{secondSource, second}}) {
            const auto inR = rec709Batch->Resolve(source, path), inA = acesBatch->Resolve(source, path);
            require(inR && inA && inR != inA, "a prefetched material was shared across working spaces: " +
                    path.GetString());
            require(Near(Reflection(inA), prism::color::Apply(toAces, Reflection(inR)), 2e-3f),
                    "a prefetched ACEScg material is not the Rec.709 one converted: " + path.GetString());
        }
        const auto stats = acesBatch->GetStats();
        std::cout << "prefetch: ACEScg compilations " << stats.compilations << ", template hits "
                  << stats.templateCacheHits << '\n';

        // SSS fallback: off first, then on. The fallback renders the material
        // as with no subsurface weight.
        const SdfPath skin("/Looks/Skin"), plain("/Looks/Plain");
        const auto skinSource = MakeMaterial(skin, GfVec3f(0.9f, 0.6f, 0.5f), 1.0f);
        const auto plainSource = MakeMaterial(plain, GfVec3f(0.9f, 0.6f, 0.5f), 0.0f);
        Renderer fallback = aces;
        fallback.sssFallback = true;
        auto sssService = service(aces);
        const auto withSss = sssService->Resolve(skinSource, skin);
        auto fallbackService = service(fallback);
        const auto withFallback = fallbackService->Resolve(skinSource, skin);
        require(withSss && withFallback && withSss != withFallback,
                "a renderer with the SSS fallback got the first renderer's material");
        const Color3 reference = Reflection(sssService->Resolve(plainSource, plain));
        const Color3 sss = Reflection(withSss), diffuse = Reflection(withFallback);
        require(Near(diffuse, reference, 2e-3f), "the SSS fallback is not the diffuse material: " + Text(diffuse) +
                " vs " + Text(reference));
        require(!Near(sss, diffuse, 2e-2f), "the SSS fallback did not change the material: " + Text(sss));
        std::cout << "SSS fallback: off " << Text(sss) << ", on " << Text(diffuse) << '\n';

        acesService.reset(); rec709Service.reset(); acesAgain.reset();
        rec709Batch.reset(); acesBatch.reset(); sssService.reset(); fallbackService.reset();
        std::error_code ignored;
        std::filesystem::remove_all(cache, ignored);
        std::cout << "material identity: distinct, correct materials per working space and SSS fallback\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "material identity: " << error.what() << '\n';
        return 1;
    }
}
