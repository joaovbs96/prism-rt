#include "../../src/hydra/materials.h"
#include "../../src/mdl/context.h"
#include "../../src/mdl_material.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/utils.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace prism::hydra;

namespace {
struct IsolatedCache {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("prism-hydra-material-loading-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    IsolatedCache() {
        std::filesystem::create_directories(path);
#ifdef _WIN32
        _putenv_s("PRISM_CACHE_DIR",path.string().c_str());
#else
        setenv("PRISM_CACHE_DIR",path.string().c_str(),1);
#endif
    }
    ~IsolatedCache() {
        std::error_code error;
        std::filesystem::remove_all(path,error);
    }
};

HdContainerDataSourceHandle MakeColorMaterial(const SdfPath& path,
                                               const GfVec3f& color,
                                               float coat = -1.0f)
{
    HdMaterialNode surface;
    surface.path = path.AppendChild(TfToken("Surface"));
    surface.identifier = TfToken("ND_open_pbr_surface_surfaceshader");
    surface.parameters[TfToken("base_color")] = VtValue(color);
    surface.parameters[TfToken("specular_roughness")] = VtValue(0.45f);
    if (coat >= 0.0f)
        surface.parameters[TfToken("coat_weight")] = VtValue(coat);
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

HdContainerDataSourceHandle MakeUdimMaterial(const SdfPath& path,
                                              const std::string& pattern)
{
    HdMaterialNode image;
    image.path=path.AppendChild(TfToken("Image"));
    image.identifier=TfToken("ND_image_color3");
    image.parameters[TfToken("file")]=VtValue(SdfAssetPath(pattern,pattern));
    HdMaterialNode surface;
    surface.path=path.AppendChild(TfToken("Surface"));
    surface.identifier=TfToken("ND_standard_surface_surfaceshader");
    surface.parameters[TfToken("specular_roughness")]=VtValue(0.45f);
    HdMaterialRelationship connection;
    connection.inputId=image.path; connection.inputName=TfToken("out");
    connection.outputId=surface.path; connection.outputName=TfToken("base_color");
    HdMaterialNetwork network;
    network.nodes={image,surface}; network.relationships={connection};
    HdMaterialNetworkMap map; map.map[TfToken("surface")]=network;
    HdContainerDataSourceHandle networkData=
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(map);
    HdDataSourceBaseHandle networkBase=networkData;
    const TfToken context("mtlx");
    auto material=HdMaterialSchema::BuildRetained(1,&context,&networkBase);
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,material);
}

HdContainerDataSourceHandle MakeTwoImageMaterial(
    const SdfPath& path, const TfToken& colorNodeName,
    const TfToken& roughnessNodeName, const std::string& colorFile,
    const std::string& roughnessFile, bool authoredDefaultThinWalled = false)
{
    HdMaterialNode color;
    color.path=path.AppendChild(colorNodeName);
    color.identifier=TfToken("ND_image_color3");
    color.parameters[TfToken("file")]=VtValue(SdfAssetPath(colorFile,colorFile));
    HdMaterialNode roughness;
    roughness.path=path.AppendChild(roughnessNodeName);
    roughness.identifier=TfToken("ND_image_float");
    roughness.parameters[TfToken("file")]=VtValue(SdfAssetPath(roughnessFile,roughnessFile));
    HdMaterialNode surface;
    surface.path=path.AppendChild(TfToken("Surface"));
    surface.identifier=TfToken("ND_standard_surface_surfaceshader");
    if (authoredDefaultThinWalled)
        surface.parameters[TfToken("thin_walled")]=VtValue(false);
    HdMaterialRelationship base;
    base.inputId=color.path; base.inputName=TfToken("out");
    base.outputId=surface.path; base.outputName=TfToken("base_color");
    HdMaterialRelationship specular;
    specular.inputId=roughness.path; specular.inputName=TfToken("out");
    specular.outputId=surface.path; specular.outputName=TfToken("specular_roughness");
    HdMaterialNetwork network;
    network.nodes={color,roughness,surface};
    network.relationships={base,specular};
    HdMaterialNetworkMap map; map.map[TfToken("surface")]=network;
    HdContainerDataSourceHandle networkData=
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(map);
    HdDataSourceBaseHandle networkBase=networkData;
    const TfToken context("mtlx");
    auto material=HdMaterialSchema::BuildRetained(1,&context,&networkBase);
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,material);
}

// standard_surface with texture-free template values. A non-negative sheen
// gives it a distinct structure. reference authors the default thin_walled,
// which changes only the captured structure, so the same shader converts and
// compiles independently of every non-reference material.
HdContainerDataSourceHandle MakeFoldingMaterial(const SdfPath& path,
    const GfVec3f& color, float roughness, float metalness,
    float sheen = -1.0f, bool reference = false)
{
    HdMaterialNode surface;
    surface.path=path.AppendChild(TfToken("Surface"));
    surface.identifier=TfToken("ND_standard_surface_surfaceshader");
    surface.parameters[TfToken("base_color")]=VtValue(color);
    surface.parameters[TfToken("specular_roughness")]=VtValue(roughness);
    surface.parameters[TfToken("metalness")]=VtValue(metalness);
    surface.parameters[TfToken("coat")]=VtValue(0.25f);
    if (sheen >= 0.0f) surface.parameters[TfToken("sheen")]=VtValue(sheen);
    if (reference) surface.parameters[TfToken("thin_walled")]=VtValue(false);
    HdMaterialNetwork network;
    network.nodes={surface};
    HdMaterialNetworkMap map; map.map[TfToken("surface")]=network;
    HdContainerDataSourceHandle networkData=
        HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(map);
    HdDataSourceBaseHandle networkBase=networkData;
    const TfToken context("mtlx");
    auto material=HdMaterialSchema::BuildRetained(1,&context,&networkBase);
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,material);
}

void SetCompileMode(const char* mode)
{
#ifdef _WIN32
    _putenv_s("PRISM_MDL_COMPILE_MODE",mode ? mode : "");
#else
    if (mode) setenv("PRISM_MDL_COMPILE_MODE",mode,1);
    else unsetenv("PRISM_MDL_COMPILE_MODE");
#endif
}

// Evaluation, sampling and cutout of two materials at a few shading states.
// Folded constants may reassociate floating-point work, so compare with a
// relative tolerance rather than bitwise.
bool SameShading(MDLMaterial& a, MDLMaterial& b, std::string* detail)
{
    auto close=[](float x,float y) {
        return std::abs(x-y)<5e-4f*std::max({1.0f,std::abs(x),std::abs(y)});
    };
    RTCRayHit embreeHit{};
    embreeHit.hit.geomID=RTC_INVALID_GEOMETRY_ID;
    RayHit hit(embreeHit);
    hit.p=Float3(0.0f); hit.n=hit.n_g=Float3(0.0f,1.0f,0.0f);
    hit.u=0.3f; hit.v=0.7f; hit.front_face=true;
    for (const Float3 d : {Float3(0.0f,-1.0f,0.0f), Float3(0.6f,-0.8f,0.0f)}) {
        hit.d=d;
        for (const Float3 wi : {Float3(0.0f,1.0f,0.0f), Float3(-0.28f,0.96f,0.0f),
                                Float3(0.0f,0.6f,0.8f)}) {
            const auto x=a.evaluate(hit,wi), y=b.evaluate(hit,wi);
            for (int i=0;i<3;++i) if (!close(x.value[i],y.value[i])) {
                *detail="evaluate "+std::to_string(x.value[i])+" vs "+std::to_string(y.value[i]);
                return false;
            }
            if (x.valid!=y.valid || !close(x.pdf,y.pdf)) { *detail="evaluate pdf"; return false; }
        }
        for (unsigned int seed : {1u,17u,4099u}) {
            unsigned int seedA=seed, seedB=seed;
            const auto x=a.sample(hit,seedA), y=b.sample(hit,seedB);
            if (x.scattered!=y.scattered || !close(x.pdf,y.pdf)) { *detail="sample pdf"; return false; }
            for (int i=0;i<3;++i)
                if (!close(x.color[i],y.color[i]) || !close(x.wi[i],y.wi[i])) {
                    *detail="sample "+std::to_string(x.color[i])+" vs "+std::to_string(y.color[i]);
                    return false;
                }
        }
        if (!close(a.cutoutOpacity(hit),b.cutoutOpacity(hit))) { *detail="cutout"; return false; }
    }
    return true;
}
}

int main()
{
    const char* root = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
    if (!root) return 2;
    // This test asserts physical generation work. Isolate it from persistent
    // artifacts produced by earlier CTest entries while keeping the cache
    // alive until MaterialService and all bound resources have been released.
    IsolatedCache isolatedCache;
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
        stats.pathCacheHits < 2 || stats.compilations != 1 ||
        stats.conversionCalls != 1 || stats.generationCalls != 1 ||
        stats.sdkTranslationCalls != 1 || stats.nativeJits != 1) {
        std::cerr << "parallel class-material reuse failed: jobs=" << stats.jobs
                  << " compiles=" << stats.compilations
                  << " conversions=" << stats.conversionCalls
                  << " generations=" << stats.generationCalls
                  << " translations=" << stats.sdkTranslationCalls
                  << " jits=" << stats.nativeJits << "\n";
        return 5;
    }
    auto redMdl = std::dynamic_pointer_cast<MDLMaterial>(redMaterial);
    auto blueMdl = std::dynamic_pointer_cast<MDLMaterial>(blueMaterial);
    if (!redMdl || !blueMdl ||
        redMdl->get_target_code().program->target_code.get() !=
            blueMdl->get_target_code().program->target_code.get() ||
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
    // Exact graph capture is shared before conversion. A batch of distinct USD
    // paths with the same owned graph must invoke conversion and compilation
    // once; worker singleflight handles the latter after serial preparation.
    const auto exactBefore = service.GetStats();
    std::vector<HdContainerDataSourceHandle> exactSources;
    std::vector<MaterialService::Request> exactRequests;
    for (int i = 0; i < 65; ++i) {
        const SdfPath path("/Looks/Exact" + std::to_string(i));
        exactSources.push_back(MakeColorMaterial(path,
            GfVec3f(0.123f, 0.456f, 0.789f), 0.137f));
        exactRequests.push_back({exactSources.back(), path});
    }
    service.Prefetch(exactRequests);
    const auto exactAfter = service.GetStats();
    if (exactAfter.conversionCalls != exactBefore.conversionCalls + 1 ||
        exactAfter.compilations != exactBefore.compilations + 1) {
        std::cerr << "same-batch exact graph reuse failed: conversions="
                  << exactAfter.conversionCalls - exactBefore.conversionCalls
                  << " compilations="
                  << exactAfter.compilations - exactBefore.compilations << "\n";
        return 9;
    }
    const auto multiRoot=std::filesystem::temp_directory_path()/
        "prism_hydra_multi_image_material";
    std::filesystem::create_directories(multiRoot);
    auto writeImage=[&](const char* name,const char* rgb) {
        const auto path=multiRoot/name;
        std::ofstream out(path,std::ios::binary|std::ios::trunc);
        out << "P3\n1 1\n255\n" << rgb << '\n';
        return path.string();
    };
    const auto colorA=writeImage("color_a.ppm","128 80 32");
    const auto roughA=writeImage("rough_a.ppm","96 96 96");
    const auto colorB=writeImage("color_b.ppm","40 144 208");
    const auto roughB=writeImage("rough_b.ppm","176 176 176");
    const auto multiA=MakeTwoImageMaterial(SdfPath("/Looks/MultiA"),
        TfToken("BaseImage"),TfToken("RoughImage"),colorA,roughA);
    // Renamed source nodes deliberately reverse lexical ordering. Canonical
    // topology, rather than USD node names, must preserve both file bindings.
    const auto multiB=MakeTwoImageMaterial(SdfPath("/Looks/MultiB"),
        TfToken("ZColor"),TfToken("A_Roughness"),colorB,roughB);
    const auto multiBefore=service.GetStats();
    service.Prefetch({{multiA,SdfPath("/Looks/MultiA")},
                      {multiB,SdfPath("/Looks/MultiB")}});
    const auto multiAfter=service.GetStats();
    if (multiAfter.conversionCalls!=multiBefore.conversionCalls+1 ||
        multiAfter.generationCalls!=multiBefore.generationCalls+1 ||
        multiAfter.sdkTranslationCalls!=multiBefore.sdkTranslationCalls+1 ||
        multiAfter.templateCacheHits<multiBefore.templateCacheHits+1) {
        std::cerr << "two-image compatible template did not reuse conversion/program: "
                  << " conversions=" << multiAfter.conversionCalls-multiBefore.conversionCalls
                  << " generations=" << multiAfter.generationCalls-multiBefore.generationCalls
                  << " translations=" << multiAfter.sdkTranslationCalls-multiBefore.sdkTranslationCalls
                  << " template_hits=" << multiAfter.templateCacheHits-multiBefore.templateCacheHits
                  << "\n";
        return 15;
    }
    auto multiAMdl=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(multiA,SdfPath("/Looks/MultiA")));
    auto multiBMdl=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(multiB,SdfPath("/Looks/MultiB")));
    if (!multiAMdl||!multiBMdl||
        multiAMdl->get_target_code().program!=multiBMdl->get_target_code().program) {
        std::cerr << "two-image renamed variants did not share their program\n";
        return 16;
    }
    RTCRayHit multiEmbreeHit{};
    multiEmbreeHit.hit.geomID=RTC_INVALID_GEOMETRY_ID;
    RayHit multiHit(multiEmbreeHit);
    multiHit.p=Float3(0.0f); multiHit.d=Float3(0.0f,-1.0f,0.0f);
    multiHit.n=multiHit.n_g=Float3(0.0f,1.0f,0.0f);
    multiHit.front_face=true; multiHit.u=multiHit.v=0.5f;
    const auto multiAEval=multiAMdl->evaluate(multiHit,Float3(0.0f,1.0f,0.0f));
    const auto multiBEval=multiBMdl->evaluate(multiHit,Float3(0.0f,1.0f,0.0f));
    const auto evalDistance=[](const auto& x,const auto& y) {
        return std::abs(x.value.x()-y.value.x())+
               std::abs(x.value.y()-y.value.y())+
               std::abs(x.value.z()-y.value.z());
    };
    if (!multiAEval.valid||!multiBEval.valid||evalDistance(multiAEval,multiBEval)<1e-4f) {
        std::cerr << "two-image template bindings did not preserve distinct resources\n";
        return 17;
    }
    // A structural false-default makes this a fresh full conversion while
    // preserving the same shader meaning. Compare the template-bound variant
    // against that independent result after the work-count gate above.
    const auto multiReference=MakeTwoImageMaterial(SdfPath("/Looks/MultiReference"),
        TfToken("ReferenceColor"),TfToken("ReferenceRoughness"),colorB,roughB,true);
    service.Prefetch({{multiReference,SdfPath("/Looks/MultiReference")}});
    auto multiReferenceMdl=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(multiReference,SdfPath("/Looks/MultiReference")));
    const auto multiReferenceEval=multiReferenceMdl
        ? multiReferenceMdl->evaluate(multiHit,Float3(0.0f,1.0f,0.0f))
        : BRDFEvaluation{};
    if (!multiReferenceEval.valid||evalDistance(multiBEval,multiReferenceEval)>1e-5f) {
        std::cerr << "two-image template result differs from independent generation\n";
        return 18;
    }
    service.Invalidate(SdfPath("/Looks/MultiA"));
    service.Invalidate(SdfPath("/Looks/MultiB"));
    service.Invalidate(SdfPath("/Looks/MultiReference"));
    const auto udimRoot=std::filesystem::temp_directory_path()/
        "prism_hydra_udim_material";
    std::filesystem::create_directories(udimRoot);
    auto writeTile=[&](const std::string& family,int tile,const char* rgb) {
        std::ofstream out(udimRoot/(family+"."+std::to_string(tile)+".ppm"),
                          std::ios::binary|std::ios::trunc);
        out << "P3\n1 1\n255\n" << rgb << '\n';
    };
    writeTile("a",1001,"255 0 0"); writeTile("a",1002,"0 255 0");
    writeTile("b",1001,"0 0 255"); writeTile("b",1002,"255 255 0");
    const auto a=MakeUdimMaterial(SdfPath("/Looks/UdimA"),
        (udimRoot/"a.<UDIM>.ppm").string());
    const auto b=MakeUdimMaterial(SdfPath("/Looks/UdimB"),
        (udimRoot/"b.<UDIM>.ppm").string());
    service.Prefetch({{a,SdfPath("/Looks/UdimA")},{b,SdfPath("/Looks/UdimB")}});
    auto aMdl=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(a,SdfPath("/Looks/UdimA")));
    auto bMdl=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(b,SdfPath("/Looks/UdimB")));
    if (!aMdl||!bMdl||!aMdl->is_valid()||!bMdl->is_valid()||
        aMdl->get_target_code().program!=bMdl->get_target_code().program) {
        std::cerr << "UDIM variants did not reuse a compatible program\n"; return 10;
    }
    RayHit udimHit(embreeHit);
    udimHit.p=Float3(0.0f); udimHit.d=Float3(0.0f,-1.0f,0.0f);
    udimHit.n=udimHit.n_g=Float3(0.0f,1.0f,0.0f); udimHit.front_face=true;
    udimHit.u=0.5f; udimHit.v=0.5f;
    const auto tile1001=aMdl->evaluate(udimHit,Float3(0.0f,1.0f,0.0f));
    udimHit.u=1.5f;
    const auto tile1002=aMdl->evaluate(udimHit,Float3(0.0f,1.0f,0.0f));
    udimHit.u=0.5f;
    const auto variant=bMdl->evaluate(udimHit,Float3(0.0f,1.0f,0.0f));
    const auto distance=[](const auto& x,const auto& y) {
        return std::abs(x.value.x()-y.value.x())+
               std::abs(x.value.y()-y.value.y())+
               std::abs(x.value.z()-y.value.z());
    };
    if (!tile1001.valid||!tile1002.valid||!variant.valid||
        distance(tile1001,tile1002)<1e-4f||distance(tile1001,variant)<1e-4f) {
        std::cerr << "UDIM tile or per-material binding selection failed\n"; return 11;
    }
    const auto editedTile=udimRoot/"a.1001.ppm";
    const auto editedStamp=std::filesystem::last_write_time(editedTile);
    { std::ofstream out(editedTile,std::ios::binary|std::ios::trunc);
      out << "P3\n1 1\n255\n0 0 255\n"; }
    std::filesystem::last_write_time(editedTile,editedStamp);
    if (service.ReloadResource(editedTile.string()) !=
            std::vector<SdfPath>{SdfPath("/Looks/UdimA")}) {
        std::cerr << "UDIM tile reload did not isolate its material\n"; return 12;
    }
    auto reloadedUdim=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(a,SdfPath("/Looks/UdimA")));
    if (!reloadedUdim||reloadedUdim==aMdl||
        reloadedUdim->get_target_code().program!=aMdl->get_target_code().program) {
        std::cerr << "UDIM reload did not retain its compatible program\n"; return 13;
    }
    udimHit.u=0.5f;
    const auto oldTileAfterReload=aMdl->evaluate(
        udimHit,Float3(0.0f,1.0f,0.0f));
    const auto newTileAfterReload=reloadedUdim->evaluate(
        udimHit,Float3(0.0f,1.0f,0.0f));
    if (!oldTileAfterReload.valid||!newTileAfterReload.valid||
        distance(tile1001,oldTileAfterReload)>1e-6f||
        distance(tile1001,newTileAfterReload)<1e-4f) {
        std::cerr << "UDIM reload mutated old binding or missed new tile\n"; return 14;
    }
    std::cout << "material stats: wall=" << stats.batchWallSeconds
              << " worker_cpu=" << stats.workerCpuSeconds
              << " conversion=" << stats.conversionSeconds
              << " generation=" << stats.generationSeconds
              << " class_compile=" << stats.classCompilationSeconds
              << " native_jit=" << stats.nativeJitSeconds << "\n";
    service.Invalidate(redPath);
    const auto redResolvedAgain=service.Resolve(red, redPath);
    if (redResolvedAgain != redMaterial) {
        std::cerr << "content cache was not reused after path invalidation\n";
        return 8;
    }
    // Missing source files are a normal asset-edit state. Invalidation must
    // still advance their generation and clear associations without throwing.
    std::error_code missingTileError;
    std::filesystem::remove(editedTile,missingTileError);
    if (missingTileError) {
        std::cerr << "Could not remove UDIM tile for invalidation regression\n";
        return 15;
    }
    // Folded compilation. A group keeps as arguments only the template values
    // its members disagree on (base_color, specular_roughness) and folds the
    // rest (metalness, coat); a material compiled alone folds everything.
    // Each result must shade like its own unfolded class compilation.
    const char* configuredMode=std::getenv("PRISM_MDL_COMPILE_MODE");
    const std::optional<std::string> originalMode=configuredMode
        ? std::optional<std::string>(configuredMode) : std::nullopt;
    const GfVec3f foldColors[3]={GfVec3f(0.8f,0.3f,0.1f),GfVec3f(0.1f,0.5f,0.7f),
                                 GfVec3f(0.4f,0.7f,0.2f)};
    const float foldRoughness[3]={0.2f,0.55f,0.35f};
    std::vector<HdContainerDataSourceHandle> foldSources, foldReferences;
    std::vector<MaterialService::Request> foldRequests, referenceRequests;
    std::vector<SdfPath> foldPaths, referencePaths;
    auto addFoldMaterial=[&](const std::string& name,const GfVec3f& color,
                             float roughness,float metalness,float sheen) {
        foldPaths.emplace_back("/Looks/Fold"+name);
        foldSources.push_back(MakeFoldingMaterial(foldPaths.back(),color,roughness,
                                                  metalness,sheen));
        foldRequests.push_back({foldSources.back(),foldPaths.back()});
        referencePaths.emplace_back("/Looks/FoldReference"+name);
        foldReferences.push_back(MakeFoldingMaterial(referencePaths.back(),color,
                                                     roughness,metalness,sheen,true));
        referenceRequests.push_back({foldReferences.back(),referencePaths.back()});
    };
    addFoldMaterial("Unique",GfVec3f(0.6f,0.6f,0.2f),0.4f,0.3f,0.45f);
    for (int i=0;i<3;++i)
        addFoldMaterial("Group"+std::to_string(i),foldColors[i],foldRoughness[i],0.2f,-1.0f);
    // Same structure as the group; metalness was folded into its template.
    addFoldMaterial("Mismatch",foldColors[0],foldRoughness[1],0.7f,-1.0f);
    SetCompileMode("auto");
    const auto foldBefore=service.GetStats();
    service.Prefetch(std::vector<MaterialService::Request>(
        foldRequests.begin(),foldRequests.begin()+4));
    const auto foldAfter=service.GetStats();
    if (foldAfter.nativeJits!=foldBefore.nativeJits+2 ||
        foldAfter.compilations!=foldBefore.compilations+2 ||
        foldAfter.templateCacheHits!=foldBefore.templateCacheHits+2 ||
        foldAfter.foldedCompilations!=foldBefore.foldedCompilations+2 ||
        foldAfter.failures!=foldBefore.failures) {
        std::cerr << "folded batch work differs: jits="
                  << foldAfter.nativeJits-foldBefore.nativeJits
                  << " compilations=" << foldAfter.compilations-foldBefore.compilations
                  << " template_hits=" << foldAfter.templateCacheHits-foldBefore.templateCacheHits
                  << " folded=" << foldAfter.foldedCompilations-foldBefore.foldedCompilations
                  << " failures=" << foldAfter.failures-foldBefore.failures << "\n";
        SetCompileMode(originalMode ? originalMode->c_str() : nullptr);
        return 30;
    }
    std::vector<std::shared_ptr<MDLMaterial>> folded;
    for (size_t i=0;i<4;++i)
        folded.push_back(std::dynamic_pointer_cast<MDLMaterial>(
            service.Resolve(foldSources[i],foldPaths[i])));
    const auto groupProgram=folded[1] ? folded[1]->get_target_code().program : nullptr;
    bool foldedShape=groupProgram && folded[0] && folded[0]->is_valid() &&
        folded[0]->has_folded_parameters() && folded[1]->has_folded_parameters() &&
        folded[0]->get_target_code().program!=groupProgram;
    for (size_t i=2;i<4;++i)
        foldedShape=foldedShape && folded[i] && folded[i]->is_valid() &&
            folded[i]->get_target_code().program==groupProgram;
    // Only the two varying values remain arguments; nothing is texture-backed.
    foldedShape=foldedShape && groupProgram->parameters.size()==2 &&
        std::count_if(groupProgram->parameters.begin(),groupProgram->parameters.end(),
            [](const auto& parameter) { return parameter.type_name=="color"; })==1 &&
        std::count_if(groupProgram->parameters.begin(),groupProgram->parameters.end(),
            [](const auto& parameter) { return parameter.type_name=="float"; })==1 &&
        folded[0]->get_target_code().program->parameters.empty();
    if (!foldedShape) {
        std::cerr << "folded group did not share one folded program, or the unique "
                     "material was not compiled folded\n";
        SetCompileMode(originalMode ? originalMode->c_str() : nullptr);
        return 31;
    }
    // Unfolded references, compiled as today's class programs.
    SetCompileMode("class");
    const auto referenceBefore=service.GetStats();
    service.Prefetch(referenceRequests);
    const auto referenceAfter=service.GetStats();
    std::vector<std::shared_ptr<MDLMaterial>> references;
    for (size_t i=0;i<referenceRequests.size();++i)
        references.push_back(std::dynamic_pointer_cast<MDLMaterial>(
            service.Resolve(foldReferences[i],referencePaths[i])));
    if (referenceAfter.foldedCompilations!=referenceBefore.foldedCompilations ||
        std::any_of(references.begin(),references.end(),[](const auto& reference) {
            return !reference || !reference->is_valid() || reference->has_folded_parameters();
        })) {
        std::cerr << "class compile mode folded parameters or failed a reference\n";
        SetCompileMode(originalMode ? originalMode->c_str() : nullptr);
        return 32;
    }
    for (size_t i=0;i<4;++i) {
        std::string detail;
        if (!SameShading(*folded[i],*references[i],&detail)) {
            std::cerr << "folded material " << foldPaths[i]
                      << " differs from its unfolded compilation: " << detail << "\n";
            SetCompileMode(originalMode ? originalMode->c_str() : nullptr);
            return 33;
        }
    }
    RTCRayHit foldEmbreeHit{};
    foldEmbreeHit.hit.geomID=RTC_INVALID_GEOMETRY_ID;
    RayHit foldHit(foldEmbreeHit);
    foldHit.p=Float3(0.0f); foldHit.d=Float3(0.0f,-1.0f,0.0f);
    foldHit.n=foldHit.n_g=Float3(0.0f,1.0f,0.0f); foldHit.front_face=true;
    for (size_t i=1;i<4;++i) for (size_t j=i+1;j<4;++j)
        if (evalDistance(folded[i]->evaluate(foldHit,Float3(0.0f,1.0f,0.0f)),
                         folded[j]->evaluate(foldHit,Float3(0.0f,1.0f,0.0f)))<1e-4f) {
            std::cerr << "folded group bindings did not preserve distinct values\n";
            SetCompileMode(originalMode ? originalMode->c_str() : nullptr);
            return 34;
        }
    // A later material of the same structure whose folded metalness differs
    // must be rejected by the group template and compiled on its own.
    SetCompileMode("auto");
    const auto mismatchBefore=service.GetStats();
    service.Prefetch({foldRequests[4]});
    const auto mismatchAfter=service.GetStats();
    auto mismatch=std::dynamic_pointer_cast<MDLMaterial>(
        service.Resolve(foldSources[4],foldPaths[4]));
    SetCompileMode(originalMode ? originalMode->c_str() : nullptr);
    std::string mismatchDetail;
    if (mismatchAfter.templateCacheHits!=mismatchBefore.templateCacheHits ||
        mismatchAfter.compilations!=mismatchBefore.compilations+1 ||
        mismatchAfter.foldedCompilations!=mismatchBefore.foldedCompilations+1 ||
        mismatchAfter.failures!=mismatchBefore.failures ||
        !mismatch || !mismatch->is_valid() ||
        mismatch->get_target_code().program==groupProgram) {
        std::cerr << "folded-value mismatch was served by the group template: template_hits="
                  << mismatchAfter.templateCacheHits-mismatchBefore.templateCacheHits
                  << " compilations=" << mismatchAfter.compilations-mismatchBefore.compilations
                  << " folded=" << mismatchAfter.foldedCompilations-mismatchBefore.foldedCompilations
                  << "\n";
        return 35;
    }
    if (!SameShading(*mismatch,*references[4],&mismatchDetail)) {
        std::cerr << "rejected-template recompilation differs from its unfolded compilation: "
                  << mismatchDetail << "\n";
        return 36;
    }
    std::cout << "folded compilation: group jit=1 binds=2, unique and rejected variant folded\n";
    service.InvalidateAll();
    std::error_code udimCleanup; std::filesystem::remove_all(udimRoot,udimCleanup);
    std::error_code multiCleanup; std::filesystem::remove_all(multiRoot,multiCleanup);
    std::cout << "Parallel material loading and class-code reuse passed\n";
    return 0;
}
