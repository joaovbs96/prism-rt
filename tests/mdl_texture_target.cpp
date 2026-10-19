#include "../src/mdl/backend_compiler.h"
#include "../src/mdl/context.h"
#include "../src/mdl/material_compiler.h"
#include "../src/mdl/texture_runtime.h"
#include "../src/materialx/mdl_generator.h"

#include <mi/mdl_sdk.h>
#include <MaterialXCore/Document.h>
#include <MaterialXCore/Material.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

// Element types of the MDL state's matrix and texture-result pointers
// (tct_float4_a16 in MDL SDK 2026, tct_float4 before).
using MdlStateFloat4 = std::remove_const_t<std::remove_pointer_t<
    decltype(mi::neuraylib::Shading_state_material_with_derivs::text_results)>>;
using MdlStateMatrixRow = std::remove_const_t<std::remove_pointer_t<
    decltype(mi::neuraylib::Shading_state_material_with_derivs::world_to_object)>>;

void require(bool condition, std::string const &message)
{
    if (!condition) throw std::runtime_error(message);
}

struct ScratchDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("prism-mdl-target-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchDirectory() { std::filesystem::create_directories(path); }
    ~ScratchDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void writePpm(std::filesystem::path const &path,
              std::array<unsigned char, 3> rgb)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(bool(file), "cannot create texture fixture");
    file << "P6\n2 2\n255\n";
    for (int i = 0; i != 4; ++i)
        file.write(reinterpret_cast<char const *>(rgb.data()), 3);
}

void writeCheckerPpm(std::filesystem::path const& path,int size)
{
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    require(bool(file),"cannot create checker texture fixture");
    file<<"P6\n"<<size<<' '<<size<<"\n255\n";
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        const unsigned char value=((x+y)&1)?255:0;
        const unsigned char rgb[3]{value,value,value};
        file.write(reinterpret_cast<char const*>(rgb),3);
    }
    require(bool(file),"cannot write checker texture fixture");
}

GeneratedMDL texturedMaterial(std::string module, std::string resource,
                              std::string selector = {})
{
    const std::string texture = selector.empty()
        ? "texture_2d(\"" + resource + "\")"
        : "texture_2d(\"" + resource + "\", tex::gamma_default, \"" +
              selector + "\")";
    GeneratedMDL generated;
    generated.module_name = "::" + std::move(module);
    generated.texture_paths = {resource};
    generated.success = true;
    generated.mdl_source =
        "mdl 1.7;\n"
        "import ::df::*;\n"
        "import ::tex::*;\n"
        "import ::state::*;\n"
        "export material main(uniform texture_2d image = " + texture + ") = material(\n"
        "  surface: material_surface(scattering: df::diffuse_reflection_bsdf(\n"
        "    tint: tex::lookup_color(image, float2(state::texture_coordinate(0).x, "
        "state::texture_coordinate(0).y)))));\n";
    return generated;
}

GeneratedMDL stringMaterial(std::string module, std::string value)
{
    GeneratedMDL generated;
    generated.module_name = "::" + std::move(module);
    generated.success = true;
    generated.mdl_source =
        "mdl 1.7;\n"
        "import ::df::*;\n"
        "export material main(uniform string choice = \"" + value + "\") = material(\n"
        "  surface: material_surface(scattering: df::diffuse_reflection_bsdf(\n"
        "    tint: choice == \"red\" ? color(1,0,0) : color(0,1,0))));\n";
    return generated;
}

// Like MaterialX geompropvalue: the primvar name is a uniform string
// parameter, so class compilation sees it only as an argument.
GeneratedMDL sceneDataMaterial(std::string module, std::string primvar)
{
    GeneratedMDL generated;
    generated.module_name = "::" + std::move(module);
    generated.success = true;
    generated.mdl_source =
        "mdl 1.8;\n"
        "import ::df::*;\n"
        "import ::scene::*;\n"
        "color lookup(uniform string name, color fallback) {\n"
        "  return scene::data_lookup_color(name, fallback);\n"
        "}\n"
        "export material main(uniform string primvar = \"" + primvar + "\") = material(\n"
        "  surface: material_surface(scattering: df::diffuse_reflection_bsdf(\n"
        "    tint: lookup(primvar, color(0,0,1)))));\n";
    return generated;
}

GeneratedMDL standardSurfaceMaterial(std::string const& libraries,
                                     MaterialX::Color3 color)
{
    MDLGenerator generator(libraries);
    auto document=MaterialX::createDocument();
    document->setDataLibrary(generator.get_stdlib());
    auto surface=document->addNode("standard_surface","Surface","surfaceshader");
    surface->setInputValue("base",1.0f);
    surface->setInputValue("base_color",color);
    surface->setInputValue("specular_roughness",.31f);
    surface->setInputValue("metalness",.17f);
    surface->setInputValue("coat",.23f);
    surface->setInputValue("coat_roughness",.19f);
    surface->setInputValue("sheen",.27f);
    document->addMaterialNode("StandardSurface",surface);
    auto generated=generator.generate(document,"StandardSurface");
    require(generated.success,"standard_surface generation failed: "+generated.error_message);
    generated.module_name += "_"+std::to_string(int(color[0]*10000.0f))+"_"+
        std::to_string(int(color[1]*10000.0f))+"_"+
        std::to_string(int(color[2]*10000.0f));
    return generated;
}

// A textured standard_surface with emission and partial opacity, so folding
// is compared on BSDF, EDF and cutout while a resource stays an argument.
GeneratedMDL foldingParityMaterial(std::string const& libraries,
                                   std::string const& texture)
{
    MDLGenerator generator(libraries);
    auto document=MaterialX::createDocument();
    document->setDataLibrary(generator.get_stdlib());
    auto image=document->addNode("image","Image","color3");
    image->setInputValue("file",texture,"filename");
    auto surface=document->addNode("standard_surface","Surface","surfaceshader");
    surface->addInput("base_color","color3")->setConnectedNode(image);
    surface->setInputValue("base",.9f);
    surface->setInputValue("specular_roughness",.36f);
    surface->setInputValue("metalness",.21f);
    surface->setInputValue("coat",.3f);
    surface->setInputValue("coat_roughness",.12f);
    surface->setInputValue("sheen",.2f);
    surface->setInputValue("emission",.4f);
    surface->setInputValue("emission_color",MaterialX::Color3(.9f,.5f,.2f));
    surface->setInputValue("opacity",MaterialX::Color3(.65f,.65f,.65f));
    document->addMaterialNode("FoldingParity",surface);
    auto generated=generator.generate(document,"FoldingParity");
    require(generated.success,"folding parity generation failed: "+generated.error_message);
    generated.module_name += "_folding_parity";
    return generated;
}

GeneratedMDL openPbrMaterial(std::string const& libraries)
{
    MDLGenerator generator(libraries);
    auto document=MaterialX::createDocument();
    document->setDataLibrary(generator.get_stdlib());
    auto surface=document->addNode("open_pbr_surface","Surface","surfaceshader");
    surface->setInputValue("base_weight",1.0f);
    surface->setInputValue("base_color",MaterialX::Color3(.28f,.46f,.71f));
    surface->setInputValue("base_diffuse_roughness",.34f);
    surface->setInputValue("specular_weight",.63f);
    surface->setInputValue("specular_roughness",.27f);
    // OpenPBR's fuzz lobe causes the SDK's MaterialX implementation to inject
    // its unnamed (DFK_INVALID) microflake-sheen lookup table.
    surface->setInputValue("fuzz_weight",.41f);
    surface->setInputValue("fuzz_color",MaterialX::Color3(.81f,.34f,.17f));
    surface->setInputValue("fuzz_roughness",.38f);
    document->addMaterialNode("OpenPbr",surface);
    auto generated=generator.generate(document,"OpenPbr");
    require(generated.success,"OpenPBR generation failed: "+generated.error_message);
    generated.module_name += "_openpbr_microflake_parity";
    return generated;
}

void setResourceMode(char const* mode)
{
#ifdef _WIN32
    _putenv_s("PRISM_MDL_RESOURCE_MODE",mode ? mode : "");
#else
    if(mode)setenv("PRISM_MDL_RESOURCE_MODE",mode,1);
    else unsetenv("PRISM_MDL_RESOURCE_MODE");
#endif
}

struct CompiledTarget {
    mdl::CompiledMaterial compiled;
    mdl::TargetCode target;
    bool cacheHit = false;
    CompiledTarget() = default;
    CompiledTarget(CompiledTarget const &) = delete;
    CompiledTarget &operator=(CompiledTarget const &) = delete;
    CompiledTarget(CompiledTarget &&) = default;
    CompiledTarget &operator=(CompiledTarget &&) = default;
    void releaseTarget() {
        // Argument blocks and handlers refer into target code. Release them
        // in dependency order rather than using memberwise assignment, which
        // assigns TargetCode members in declaration order.
        target.binding.reset();
        target.program.reset();
    }
    void release() {
        releaseTarget();
        target = {};
        compiled.release();
    }
    ~CompiledTarget() { release(); }
};

CompiledTarget compile(mdl::Context &context, std::filesystem::path const &root,
                       GeneratedMDL generated,
                       mdl::CompileSpecification const &specification = {})
{
    std::string mdlLibraries;
    if(const char* configured=std::getenv("PRISM_MATERIALX_MDL");configured&&*configured)
        mdlLibraries=configured;
    else if(const char* libraries=std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
            libraries&&*libraries)
        mdlLibraries=(std::filesystem::path(libraries)/"mdl").string();
    mdl::MaterialCompiler compiler(context, mdlLibraries);
    CompiledTarget result;
    result.compiled = compiler.compile(generated, (root / "fixture.mtlx").string(),
                                       specification);
    require(result.compiled.success, "material compile failed: " + result.compiled.error_message);
    mdl::BackendCompiler backend(context);
    result.target = backend.generate_target_code(result.compiled);
    require(result.target.success, "native target failed: " + result.target.error_message);
    result.cacheHit = result.target.cacheHit;
    // Production closes the material transaction immediately after target
    // generation. All execution below must therefore rely only on TargetCode's
    // owned code, argument bytes and resource runtime.
    result.compiled.release();
    return result;
}

struct State {
    mi::neuraylib::Shading_state_material_with_derivs derivative{};
    mi::neuraylib::tct_deriv_float3 uv{};
    mi::Float32_3 tangentU{1, 0, 0}, tangentV{0, 1, 0};
    MdlStateMatrixRow identity[4]{};
    alignas(16) MdlStateFloat4 results[128]{};

    explicit State(mdl::TargetCode const &target, float u, float v,
                   float dudx=0,float dvdx=0,float dudy=0,float dvdy=0)
    {
        identity[0] = {1,0,0,0}; identity[1] = {0,1,0,0};
        identity[2] = {0,0,1,0}; identity[3] = {0,0,0,1};
        uv.val = mi::Float32_3(u, v, 0);
        uv.dx = mi::Float32_3(dudx,dvdx,0);
        uv.dy = mi::Float32_3(dudy,dvdy,0);
        auto &s = derivative;
        s.normal = s.geom_normal = mi::Float32_3(0,0,1);
        s.tangent_u = &tangentU; s.tangent_v = &tangentV;
        s.text_coords = &uv; s.text_results = results;
        s.world_to_object = identity; s.object_to_world = identity;
        s.ro_data_segment = target.program->ro_data_segment;
        s.meters_per_scene_unit = 1.0f;
    }
    mi::neuraylib::Shading_state_material &base() {
        return reinterpret_cast<mi::neuraylib::Shading_state_material &>(derivative);
    }
};

std::array<float, 3> evaluate(mdl::TargetCode const &target, float u, float v,
                             float dudx=0,float dvdx=0,
                             float dudy=0,float dvdy=0)
{
    State state(target,u,v,dudx,dvdx,dudy,dvdy);
    require(target.program->target_code->execute_init(target.program->functions.init_function_index,
        state.base(), target.texture_handler(), target.argument_data()) == 0,
        "target init failed");
    mi::neuraylib::Bsdf_evaluate_data<mi::neuraylib::DF_HSM_NONE> data{};
    data.k1 = data.k2 = mi::Float32_3(0,0,1);
    data.ior1 = mi::Float32_3(1.0f);
    data.ior2 = mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
    require(target.program->target_code->execute_bsdf_evaluate(
        target.program->functions.surface_bsdf_function_index +
            mdl::MaterialFunctions::DF_EVALUATE,
        &data, state.base(), target.texture_handler(), target.argument_data()) == 0,
        "BSDF evaluation failed");
    return {data.bsdf_diffuse.x + data.bsdf_glossy.x,
            data.bsdf_diffuse.y + data.bsdf_glossy.y,
            data.bsdf_diffuse.z + data.bsdf_glossy.z};
}

bool dominant(std::array<float,3> const &value, int channel)
{
    return value[channel] > 0.1f && value[channel] > value[(channel+1)%3] * 4.0f &&
        value[channel] > value[(channel+2)%3] * 4.0f;
}

struct SampleResult {
    mi::Float32_3 k2{},weight{};
    float pdf=0;
    mi::Uint32 event=mi::neuraylib::BSDF_EVENT_ABSORB;
};
SampleResult sample(mdl::TargetCode const& target,std::array<float,4> xi)
{
    State state(target,.37f,.61f);
    require(target.program->target_code->execute_init(target.program->functions.init_function_index,
        state.base(),target.texture_handler(),target.argument_data())==0,"sample init failed");
    mi::neuraylib::Bsdf_sample_data data{};
    data.k1=mi::Float32_3(0,0,1);data.ior1=mi::Float32_3(1);
    data.ior2=mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
    data.xi.x=xi[0]; data.xi.y=xi[1]; data.xi.z=xi[2]; data.xi.w=xi[3];
    require(target.program->target_code->execute_bsdf_sample(
        target.program->functions.surface_bsdf_function_index,&data,state.base(),
        target.texture_handler(),target.argument_data())==0,"BSDF sample failed");
    return {data.k2,data.bsdf_over_pdf,data.pdf,data.event_type};
}
float pdf(mdl::TargetCode const& target,mi::Float32_3 k2)
{
    State state(target,.37f,.61f);
    require(target.program->target_code->execute_init(target.program->functions.init_function_index,
        state.base(),target.texture_handler(),target.argument_data())==0,"pdf init failed");
    mi::neuraylib::Bsdf_pdf_data data{};
    data.k1=mi::Float32_3(0,0,1);data.k2=k2;data.ior1=mi::Float32_3(1);
    data.ior2=mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
    require(target.program->target_code->execute_bsdf_pdf(
        target.program->functions.surface_bsdf_function_index+
            mdl::MaterialFunctions::DF_PDF,&data,state.base(),target.texture_handler(),
        target.argument_data())==0,"BSDF pdf failed");
    return data.pdf;
}
std::array<float,3> emission(mdl::TargetCode const& target,float u,float v)
{
    State state(target,u,v);
    require(target.program->target_code->execute_init(target.program->functions.init_function_index,
        state.base(),target.texture_handler(),target.argument_data())==0,"emission init failed");
    mi::Float32_3 intensity(0,0,0);
    require(target.program->target_code->execute(
        target.program->functions.surface_emission_intensity_function_index,state.base(),
        target.texture_handler(),target.argument_data(),&intensity)==0,
        "emission intensity failed");
    mi::neuraylib::Edf_evaluate_data<mi::neuraylib::DF_HSM_NONE> data{};
    data.k1=mi::Float32_3(0,0,1);
    require(target.program->functions.surface_edf_function_index!=~mi::Size(0)&&
        target.program->target_code->execute_edf_evaluate(
            target.program->functions.surface_edf_function_index+
                mdl::MaterialFunctions::DF_EVALUATE,&data,state.base(),
            target.texture_handler(),target.argument_data())==0,"EDF evaluation failed");
    return {intensity.x*data.edf.x,intensity.y*data.edf.y,intensity.z*data.edf.z};
}
float cutout(mdl::TargetCode const& target,float u,float v)
{
    State state(target,u,v);
    require(target.program->target_code->execute_init(target.program->functions.init_function_index,
        state.base(),target.texture_handler(),target.argument_data())==0,"cutout init failed");
    mi::Float32 opacity=-1;
    require(target.program->target_code->execute(
        target.program->functions.cutout_opacity_function_index,state.base(),
        target.texture_handler(),target.argument_data(),&opacity)==0,
        "cutout opacity failed");
    return opacity;
}

// Requires two targets to shade identically, within the tolerance used for
// the custom/builtin sample and PDF comparisons, at several shading states.
void requireSameShading(mdl::TargetCode const& a,mdl::TargetCode const& b,
                        std::string const& what)
{
    auto close=[](float x,float y){return std::abs(x-y)<
        5e-4f*std::max({1.0f,std::abs(x),std::abs(y)});};
    auto closeColor=[&](std::array<float,3> const& x,std::array<float,3> const& y) {
        return close(x[0],y[0])&&close(x[1],y[1])&&close(x[2],y[2]);
    };
    for(auto uv:std::array{std::array{.37f,.61f},std::array{10.5f/256,10.5f/256},
                           std::array{11.5f/256,10.5f/256},std::array{.83f,.14f}}) {
        const auto x=evaluate(a,uv[0],uv[1]),y=evaluate(b,uv[0],uv[1]);
        require(closeColor(x,y),what+": BSDF evaluation differs: "+std::to_string(x[0])+
            ","+std::to_string(x[1])+","+std::to_string(x[2])+" vs "+std::to_string(y[0])+
            ","+std::to_string(y[1])+","+std::to_string(y[2]));
        require(closeColor(emission(a,uv[0],uv[1]),emission(b,uv[0],uv[1])),
                what+": emission differs");
        require(close(cutout(a,uv[0],uv[1]),cutout(b,uv[0],uv[1])),
                what+": cutout opacity differs");
    }
    for(auto xi:std::array{std::array{.01f,.19f,.43f,.71f},
                           std::array{.37f,.53f,.79f,.97f},
                           std::array{.999f,.001f,.5f,.25f}}) {
        const auto x=sample(a,xi),y=sample(b,xi);
        require(x.event==y.event,what+": sample selected a different event");
        require(close(x.k2.x,y.k2.x)&&close(x.k2.y,y.k2.y)&&close(x.k2.z,y.k2.z)&&
                    close(x.weight.x,y.weight.x)&&close(x.weight.y,y.weight.y)&&
                    close(x.weight.z,y.weight.z)&&close(x.pdf,y.pdf),
                what+": sample differs");
        require(close(pdf(a,x.k2),pdf(b,y.k2)),what+": PDF differs");
    }
}

void verifyRepeatedSdkLifetimes(ScratchDirectory const& scratch)
{
    std::uintptr_t previousContextAddress = 0;
    for (int lifetime = 0; lifetime != 2; ++lifetime) {
        mdl::Context context;
        require(context.is_valid(), "cannot initialize repeated MDL SDK lifetime: " + context.get_error());
        const auto contextAddress = reinterpret_cast<std::uintptr_t>(&context);
        if (previousContextAddress != 0)
            require(previousContextAddress == contextAddress,
                    "repeated MDL Context was not constructed at the same stack location");
        previousContextAddress = contextAddress;

        {
            auto target = compile(context, scratch.path,
                texturedMaterial("target_repeated_sdk_lifetime", "/red.ppm"));
            require(target.target.success && target.target.program && target.target.binding &&
                        target.target.binding->texture_runtime,
                    "repeated SDK lifetime did not produce a valid native texture target: " +
                        target.target.error_message);
            require(!target.cacheHit && target.target.translationCalls == 1,
                    "target-code cache survived the previous SDK context lifetime");
            require(dominant(evaluate(target.target, .25f, .25f), 0),
                    "repeated SDK lifetime target did not evaluate the red texture");
            // Argument blocks, handlers and target code must all be released
            // before Context teardown clears its cache and shuts down neuray.
            target.release();
        }
    }
}

} // namespace

int main()
{
    try {
        ScratchDirectory scratch;
        writePpm(scratch.path / "red.ppm", {255, 8, 4});
        writePpm(scratch.path / "green.ppm", {4, 255, 8});
        writePpm(scratch.path / "tiles_1001.ppm", {255, 8, 4});
        writePpm(scratch.path / "tiles_1002.ppm", {4, 255, 8});
        writeCheckerPpm(scratch.path / "checker.ppm",256);

        verifyRepeatedSdkLifetimes(scratch);

        mdl::Context context;
        require(context.is_valid(), "cannot initialize MDL SDK: " + context.get_error());

        auto red = compile(context, scratch.path,
            texturedMaterial("target_red", "/red.ppm"));
        require(bool(red.target.binding->texture_runtime),
                "ordinary 2D texture unexpectedly used SDK fallback: " +
                    red.target.textureFallbackReason);
        require(dominant(evaluate(red.target, 0.25f, 0.25f), 0),
                "custom 2D target did not execute the red texture lookup");

        auto green = compile(context, scratch.path,
            texturedMaterial("target_green", "/green.ppm"));
        require(green.target.cacheHit,
                "same material class with a different resource did not reuse native code");
        require(dominant(evaluate(green.target, 0.25f, 0.25f), 1),
                "cached target reused the first argument block's texture resource");
        require(dominant(evaluate(red.target, 0.25f, 0.25f), 0),
                "publishing the second argument block mutated the first resource registry");

        // Bind a new texture directly against the published class program.
        // This is the production template path: no module load or JIT is
        // allowed, and the original binding must remain immutable.
        {
            mi::base::Handle<mi::neuraylib::IScope> scope(
                context.get_database()->get_global_scope());
            mi::base::Handle<mi::neuraylib::ITransaction> transaction(
                scope->create_transaction());
            mi::base::Handle<mi::neuraylib::IMdl_execution_context> execution(
                context.get_factory()->create_execution_context());
            mi::base::Handle<mi::neuraylib::IValue_texture> texture(
                context.get_factory()->create_texture(transaction.get(),"/green.ppm",
                    mi::neuraylib::IType_texture::TS_2D,0.0f,nullptr,true,execution.get()));
            mi::base::Handle<mi::neuraylib::IValue_factory> factory(
                context.get_factory()->create_value_factory(transaction.get()));
            mi::base::Handle<mi::neuraylib::IValue_list> overrides(factory->create_value_list());
            require(texture&&overrides->add_value("image",texture.get())==0,
                    "cannot create texture argument override");
            mdl::BackendCompiler backend(context);
            auto rebound=backend.bind_arguments(red.target,overrides.get(),transaction.get());
            require(rebound.success&&rebound.translationCalls==0&&
                        rebound.program==red.target.program,
                    "texture binding unexpectedly translated another program: "+rebound.error_message);
            transaction->commit();transaction.reset();
            require(dominant(evaluate(rebound,.25f,.25f),1)&&
                        dominant(evaluate(red.target,.25f,.25f),0),
                    "direct texture binding did not retain independent registries");
        }

        auto udim = compile(context, scratch.path,
            texturedMaterial("target_udim", "/tiles_<UDIM>.ppm"));
        require(udim.target.cacheHit,
                "UDIM resource did not reuse the compatible class target");
        require(bool(udim.target.binding->texture_runtime),
                "UDIM texture unexpectedly used SDK fallback: " +
                    udim.target.textureFallbackReason);
        require(dominant(evaluate(udim.target, 0.25f, 0.25f), 0) &&
                    dominant(evaluate(udim.target, 1.25f, 0.25f), 1),
                "custom target did not select distinct UDIM tiles");

        auto selector = compile(context, scratch.path,
            texturedMaterial("target_selector", "/red.ppm", "R"));
        require(!selector.target.binding->texture_runtime,
                "unsupported selector incorrectly mixed custom and SDK handlers");
        require(!selector.target.textureFallbackReason.empty(),
                "whole-target SDK fallback did not report its reason");
        const auto fallback = evaluate(selector.target, 0.25f, 0.25f);
        require(fallback[0]>.1f&&fallback[1]>.1f&&fallback[2]>.1f,
                "SDK selector fallback did not expand the red R channel to color");
        auto selectorGreen = compile(context,scratch.path,
            texturedMaterial("target_selector_green","/green.ppm","R"));
        require(!selectorGreen.target.binding->texture_runtime&&
                    !selectorGreen.target.textureFallbackReason.empty()&&
                    selectorGreen.target.translationCalls == 1,
                "second selector target did not use coherent SDK fallback");
        const auto fallbackGreen=evaluate(selectorGreen.target,.25f,.25f);
        for(int channel=0;channel<3;++channel)
            require(fallback[channel]>fallbackGreen[channel]*8.0f,
                    "SDK selector fallback reused the other argument resource");
        const auto fallbackAgain=evaluate(selector.target,.25f,.25f);
        for(int channel=0;channel<3;++channel)
            require(std::abs(fallbackAgain[channel]-fallback[channel])<1e-6f,
                    "second fallback target mutated the first resource binding");

        auto checker=compile(context,scratch.path,
            texturedMaterial("target_checker","/checker.ppm"));
        require(bool(checker.target.binding->texture_runtime),
                "checker unexpectedly used SDK fallback: "+
                    checker.target.textureFallbackReason);
        const auto point=evaluate(checker.target,10.5f/256,10.5f/256);
        const auto filtered=evaluate(checker.target,10.5f/256,10.5f/256,
                                     .5f,0,0,.5f);
        constexpr float expected=.5f/std::numbers::pi_v<float>;
        for(float channel:filtered)
            require(std::abs(channel-expected)<.03f,
                    "real JIT derivative lookup differs from checker mip reference");
        require(std::abs(point[0]-filtered[0])>.08f,
                "real JIT checker fixture does not discriminate zero derivatives");

        auto stringRed = compile(context,scratch.path,
            stringMaterial("target_string_red","red"));
        require(dominant(evaluate(stringRed.target,.25f,.25f),0),
                "string argument red branch did not execute");
        auto stringGreen = compile(context,scratch.path,
            stringMaterial("target_string_green","green"));
        require(!stringGreen.target.cacheHit &&
                    stringGreen.target.translationCalls==1,
                "changed string constant incorrectly reused a cached program");
        require(dominant(evaluate(stringGreen.target,.25f,.25f),1),
                "fresh target did not preserve changed string argument");

        // Scene data named by a class argument (MaterialX geompropvalue): the
        // name appears nowhere in the code, so as an argument it reached the
        // program as string id 0 and every lookup returned its default (blue).
        // Two materials of one class that differ only in the primvar name.
        int dummyMesh=0;
        mdl::NativeTextureRuntime::primvarContext={&dummyMesh,0,0,0,
            [](const void*,const char* name,unsigned,float,float,float* out,int count) {
                const std::string_view primvar(name?name:"");
                if(count!=3||(primvar!="displayColor"&&primvar!="otherColor"))return false;
                const bool red=primvar=="displayColor";
                out[0]=red?1.f:0.f; out[1]=red?0.f:1.f; out[2]=0.f;
                return true;
            }};
        auto sceneDataRed=compile(context,scratch.path,
            sceneDataMaterial("target_scene_data_red","displayColor"));
        require(dominant(evaluate(sceneDataRed.target,.25f,.25f),0),
                "class-compiled scene data lookup by argument name returned its default");
        auto sceneDataGreen=compile(context,scratch.path,
            sceneDataMaterial("target_scene_data_green","otherColor"));
        require(dominant(evaluate(sceneDataGreen.target,.25f,.25f),1),
                "class-compiled scene data lookup used another material's primvar name");
        require(dominant(evaluate(sceneDataRed.target,.25f,.25f),0),
                "scene data variant changed an earlier material's lookup");
        mdl::NativeTextureRuntime::primvarContext={};

        // Exercise a real MaterialX standard_surface target. Its glossy/sheen
        // implementation causes the SDK to inject internal BSDF data textures;
        // comparing complete JIT evaluation verifies the callback ABI rather
        // than merely probing the texture table in isolation.
        const char* libraryPath=std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        require(libraryPath&&*libraryPath,
                "PRISM_MATERIALX_LIBRARY_PATH is required for standard_surface parity");
        auto standardGenerated=standardSurfaceMaterial(
            libraryPath,MaterialX::Color3(.31f,.47f,.73f));
        mdl::ClearTargetCodeCache(&context);
        setResourceMode("custom");
        auto standardCustom=compile(context,scratch.path,standardGenerated);
        require(bool(standardCustom.target.binding->texture_runtime),
                "standard_surface did not use the custom BSDF-table runtime: "+
                    standardCustom.target.textureFallbackReason);
        bool hasDfTable=false;
        for(mi::Size i=1;i<standardCustom.target.program->target_code->get_texture_count();++i)
            hasDfTable |= standardCustom.target.program->target_code->get_texture_shape(i)==
                mi::neuraylib::ITarget_code::Texture_shape_bsdf_data;
        require(hasDfTable,"standard_surface fixture did not contain an SDK BSDF data table");
        const auto customValue=evaluate(standardCustom.target,.37f,.61f);
        {
            auto parameter=std::find_if(standardCustom.target.program->parameters.begin(),
                standardCustom.target.program->parameters.end(),[](auto const& p) {
                    return p.type_name=="color"&&p.name.find("base_color")!=std::string::npos;
                });
            require(parameter!=standardCustom.target.program->parameters.end(),
                    "standard_surface did not publish a base-color class parameter");
            mi::base::Handle<mi::neuraylib::IScope> scope(
                context.get_database()->get_global_scope());
            mi::base::Handle<mi::neuraylib::ITransaction> transaction(scope->create_transaction());
            mi::base::Handle<mi::neuraylib::IValue_factory> factory(
                context.get_factory()->create_value_factory(transaction.get()));
            mi::base::Handle<mi::neuraylib::IValue_list> overrides(factory->create_value_list());
            mi::base::Handle<mi::neuraylib::IValue_color> color(factory->create_color(.9f,.08f,.03f));
            require(overrides->add_value(parameter->name.c_str(),color.get())==0,
                    "cannot create color argument override");
            mdl::BackendCompiler backend(context);
            mi::base::Handle<mi::neuraylib::IValue_list> invalid(factory->create_value_list());
            mi::base::Handle<mi::neuraylib::IValue_float> wrong(factory->create_float(.5f));
            require(invalid->add_value(parameter->name.c_str(),wrong.get())==0&&
                    !backend.bind_arguments(standardCustom.target,invalid.get(),transaction.get()).success,
                    "binding accepted an argument with the wrong MDL type");
            auto rebound=backend.bind_arguments(
                standardCustom.target,overrides.get(),transaction.get());
            require(rebound.success&&rebound.translationCalls==0&&
                        rebound.program==standardCustom.target.program,
                    "color binding unexpectedly translated another program: "+rebound.error_message);
            transaction->commit();transaction.reset();
            const auto changed=evaluate(rebound,.37f,.61f);
            require(std::abs(changed[0]-customValue[0])>.01f,
                    "color binding did not change standard_surface shading");
        }

        mdl::ClearTargetCodeCache(&context);
        setResourceMode("builtin");
        auto standardBuiltin=compile(context,scratch.path,standardGenerated);
        require(!standardBuiltin.target.binding->texture_runtime,
                "forced builtin standard_surface unexpectedly installed custom callbacks");
        const auto builtinValue=evaluate(standardBuiltin.target,.37f,.61f);
        for(int channel=0;channel<3;++channel)
            require(std::abs(customValue[channel]-builtinValue[channel])<
                        3e-5f*std::max(1.0f,std::abs(builtinValue[channel])),
                    "custom BSDF-table evaluation differs from SDK builtin target: custom="+
                    std::to_string(customValue[channel])+" builtin="+
                    std::to_string(builtinValue[channel]));
        for(auto xi:std::array{std::array{.01f,.19f,.43f,.71f},
                               std::array{.37f,.53f,.79f,.97f},
                               std::array{.999f,.001f,.5f,.25f}}) {
            const auto a=sample(standardCustom.target,xi);
            const auto b=sample(standardBuiltin.target,xi);
            require(a.event==b.event,"custom BSDF-table sample selected a different event");
            // Builtin native targets inline the SDK table path, while custom
            // targets call our scalar trilinear callback. They are not
            // bitwise-identical floating-point programs; keep the full JIT
            // sample/PDF agreement below 0.05% at regular and grazing samples.
            auto close=[](float x,float y){return std::abs(x-y)<
                5e-4f*std::max({1.0f,std::abs(x),std::abs(y)});};
            require(close(a.k2.x,b.k2.x)&&close(a.k2.y,b.k2.y)&&close(a.k2.z,b.k2.z)&&
                    close(a.weight.x,b.weight.x)&&close(a.weight.y,b.weight.y)&&
                    close(a.weight.z,b.weight.z)&&close(a.pdf,b.pdf),
                    "custom BSDF-table sampling differs from SDK builtin target: k2="+
                    std::to_string(a.k2.x)+","+std::to_string(a.k2.y)+","+
                    std::to_string(a.k2.z)+" vs "+std::to_string(b.k2.x)+","+
                    std::to_string(b.k2.y)+","+std::to_string(b.k2.z)+" pdf="+
                    std::to_string(a.pdf)+" vs "+std::to_string(b.pdf));
            require(close(pdf(standardCustom.target,a.k2),
                          pdf(standardBuiltin.target,b.k2)),
                    "custom BSDF-table PDF differs from SDK builtin target");
        }

        // OpenPBR exercises a distinct SDK path: its fuzz implementation
        // publishes a complete 3D microflake table whose Df_data_kind is
        // DFK_INVALID. The shape-5 callback contract is nevertheless the same
        // generic 3D texture ABI, so compare the complete generated BSDF in
        // both resource modes rather than accepting the table by layout alone.
        auto openPbrGenerated=openPbrMaterial(libraryPath);
        mdl::ClearTargetCodeCache(&context);
        setResourceMode("custom");
        auto openPbrCustom=compile(context,scratch.path,openPbrGenerated);
        bool hasUnnamedMicroflake=false;
        for(mi::Size i=1;i<openPbrCustom.target.program->target_code->get_texture_count();++i) {
            const auto* code=openPbrCustom.target.program->target_code.get();
            const char* name=code->get_texture(i);
            hasUnnamedMicroflake |=
                code->get_texture_shape(i)==
                    mi::neuraylib::ITarget_code::Texture_shape_bsdf_data &&
                code->get_texture_df_data_kind(i)==mi::neuraylib::DFK_INVALID &&
                name && std::string_view(name).find("microflake_sheen")!=std::string_view::npos;
        }
        require(hasUnnamedMicroflake,
                "OpenPBR fixture did not contain the unnamed microflake-sheen table");
        require(bool(openPbrCustom.target.binding->texture_runtime),
                "OpenPBR did not use the custom BSDF-table runtime: "+
                    openPbrCustom.target.textureFallbackReason);
        mdl::ClearTargetCodeCache(&context);
        setResourceMode("builtin");
        auto openPbrBuiltin=compile(context,scratch.path,openPbrGenerated);
        require(!openPbrBuiltin.target.binding->texture_runtime,
                "forced builtin OpenPBR unexpectedly installed custom callbacks");
        auto closeOpenPbr=[](float x,float y) {
            return std::abs(x-y)<5e-4f*std::max({1.0f,std::abs(x),std::abs(y)});
        };
        const auto openPbrCustomValue=evaluate(openPbrCustom.target,.37f,.61f);
        const auto openPbrBuiltinValue=evaluate(openPbrBuiltin.target,.37f,.61f);
        for(int channel=0;channel<3;++channel)
            require(closeOpenPbr(openPbrCustomValue[channel],openPbrBuiltinValue[channel]),
                    "OpenPBR custom evaluation differs from SDK builtin target");
        for(auto xi:std::array{std::array{.07f,.23f,.41f,.67f},
                               std::array{.43f,.59f,.83f,.13f},
                               std::array{.997f,.003f,.57f,.31f}}) {
            const auto a=sample(openPbrCustom.target,xi);
            const auto b=sample(openPbrBuiltin.target,xi);
            require(a.event==b.event,"OpenPBR custom sample selected a different event");
            require(closeOpenPbr(a.k2.x,b.k2.x)&&closeOpenPbr(a.k2.y,b.k2.y)&&
                        closeOpenPbr(a.k2.z,b.k2.z)&&
                        closeOpenPbr(a.weight.x,b.weight.x)&&
                        closeOpenPbr(a.weight.y,b.weight.y)&&
                        closeOpenPbr(a.weight.z,b.weight.z)&&closeOpenPbr(a.pdf,b.pdf),
                    "OpenPBR custom sample differs from SDK builtin target");
            require(closeOpenPbr(pdf(openPbrCustom.target,a.k2),
                                 pdf(openPbrBuiltin.target,b.k2)),
                    "OpenPBR custom PDF differs from SDK builtin target");
        }

        // Concurrent bindings must publish one immutable custom program while
        // retaining independent argument values and resource registries.
        mdl::ClearTargetCodeCache(&context);
        setResourceMode("custom");
        std::array<std::future<CompiledTarget>,4> concurrent;
        for(size_t i=0;i<concurrent.size();++i) concurrent[i]=std::async(
            std::launch::async,[&,i] {
                const float red=.15f+.18f*float(i);
                return compile(context,scratch.path,standardSurfaceMaterial(
                    libraryPath,MaterialX::Color3(red,.29f,.41f)));
            });
        std::array<CompiledTarget,4> bindings;
        uint64_t translations=0;
        for(size_t i=0;i<bindings.size();++i) {
            bindings[i]=concurrent[i].get();
            require(bool(bindings[i].target.binding->texture_runtime),
                    "concurrent standard_surface binding lost custom runtime");
            translations+=bindings[i].target.translationCalls;
        }
        require(translations==1,
                "singleflight translated the same standard_surface program more than once");
        for(size_t i=1;i<bindings.size();++i)
            require(bindings[i].target.program==bindings[0].target.program,
                    "concurrent standard_surface bindings did not share immutable program");
        const auto firstValue=evaluate(bindings.front().target,.37f,.61f);
        const auto lastValue=evaluate(bindings.back().target,.37f,.61f);
        require(std::abs(firstValue[0]-lastValue[0])>.01f,
                "independent standard_surface bindings reused one argument block");
        setResourceMode(nullptr);

        // Folding every non-resource class parameter bakes the values into
        // the program. It must not change what the material computes, and
        // the texture must stay an argument of the custom runtime.
        {
            auto parityGenerated=foldingParityMaterial(libraryPath,"/checker.ppm");
            mdl::ClearTargetCodeCache(&context);
            auto unfolded=compile(context,scratch.path,parityGenerated);
            mdl::CompileSpecification foldAll;
            foldAll.foldParameters=true;
            foldAll.shareProgram=false;
            auto folded=compile(context,scratch.path,parityGenerated,foldAll);
            require(unfolded.compiled.folded_parameters.empty()&&
                        !folded.compiled.folded_parameters.empty(),
                    "folded compile did not fold any class parameter");
            require(!folded.cacheHit&&folded.target.program!=unfolded.target.program&&
                        folded.target.program->parameters.size()<
                            unfolded.target.program->parameters.size(),
                    "folded compile reused or kept the unfolded class program");
            require(!folded.target.program->parameters.empty(),
                    "folded compile dropped the texture argument");
            for(auto const& parameter:folded.target.program->parameters)
                require(parameter.type_name=="texture_2d",
                        "folded program kept a non-resource argument: "+parameter.name);
            require(bool(folded.target.binding->texture_runtime),
                    "folded program lost the custom texture runtime: "+
                        folded.target.textureFallbackReason);
            require(cutout(unfolded.target,.37f,.61f)<.9f,
                    "folding parity fixture is unexpectedly opaque");
            requireSameShading(unfolded.target,folded.target,"folded standard_surface");
            // A program folded for one material must not occupy the bounded
            // target-code cache that shareable class programs rely on.
            auto foldedAgain=compile(context,scratch.path,parityGenerated,foldAll);
            require(!foldedAgain.cacheHit&&foldedAgain.target.translationCalls==1,
                    "unshared folded program entered the target-code cache");
            std::cout << "folding parity: class parameters="
                      << unfolded.target.program->parameters.size()
                      << " translate=" << unfolded.target.nativeJitSeconds << "s; folded parameters="
                      << folded.target.program->parameters.size()
                      << " translate=" << folded.target.nativeJitSeconds << "s\n";
        }

        // The target-code cache keys a program by its compiled-material hash
        // and argument interface. Two materials that differ only in a folded
        // constant have the same interface, so only the hash can keep them
        // apart; sharing a program would render one with the other's value.
        {
            auto amberGenerated=standardSurfaceMaterial(
                libraryPath,MaterialX::Color3(.83f,.41f,.12f));
            auto tealGenerated=standardSurfaceMaterial(
                libraryPath,MaterialX::Color3(.12f,.52f,.61f));
            mdl::CompileSpecification foldShared;
            foldShared.foldParameters=true;
            auto amberFolded=compile(context,scratch.path,amberGenerated,foldShared);
            auto tealFolded=compile(context,scratch.path,tealGenerated,foldShared);
            const auto& amberProgram=*amberFolded.target.program;
            const auto& tealProgram=*tealFolded.target.program;
            require(std::any_of(tealFolded.compiled.folded_parameters.begin(),
                        tealFolded.compiled.folded_parameters.end(),
                        [](auto const& name){return name.find("base_color")!=std::string::npos;}),
                    "standard_surface base color was not folded");
            require(amberProgram.parameters.size()==tealProgram.parameters.size(),
                    "folded fixtures unexpectedly differ in their argument interface");
            std::cout << "folded program keys:\n  " << amberProgram.identity
                      << "\n  " << tealProgram.identity << "\n";
            require(!tealFolded.cacheHit&&tealFolded.target.translationCalls==1&&
                        &tealProgram!=&amberProgram&&
                        tealProgram.identity!=amberProgram.identity,
                    "materials with different folded constants shared a target-code cache key");
            auto amberAgain=compile(context,scratch.path,amberGenerated,foldShared);
            require(amberAgain.cacheHit&&amberAgain.target.program==amberFolded.target.program,
                    "shared folded program was not reused for identical folded constants");
            auto amberUnfolded=compile(context,scratch.path,amberGenerated);
            auto tealUnfolded=compile(context,scratch.path,tealGenerated);
            requireSameShading(amberUnfolded.target,amberFolded.target,"folded amber");
            requireSameShading(tealUnfolded.target,tealFolded.target,"folded teal");
            const auto amberValue=evaluate(amberFolded.target,.37f,.61f);
            const auto tealValue=evaluate(tealFolded.target,.37f,.61f);
            require(std::abs(amberValue[0]-tealValue[0])>.01f,
                    "folded base colors did not change standard_surface shading");
        }

        std::cout << "custom MDL 2D/UDIM/BSDF-table targets, immutable bindings and SDK parity passed\n";
        mdl::ClearTargetCodeCache(&context);
        openPbrBuiltin.releaseTarget(); openPbrCustom.releaseTarget();
        standardBuiltin.releaseTarget(); standardCustom.releaseTarget();
        checker.releaseTarget(); selectorGreen.releaseTarget();
        stringGreen.releaseTarget(); stringRed.releaseTarget();
        selector.releaseTarget(); udim.releaseTarget();
        green.releaseTarget(); red.releaseTarget();
        stringGreen.compiled.release(); stringRed.compiled.release();
        selector.compiled.release(); udim.compiled.release();
        green.compiled.release(); red.compiled.release();
        checker.compiled.release(); selectorGreen.compiled.release();
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "mdl_texture_target: " << error.what() << '\n';
        return 1;
    }
}
