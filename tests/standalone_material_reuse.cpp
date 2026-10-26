#include "../src/mesh/usd_mesh.h"

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
uint64_t hashBlock(const mi::neuraylib::ITarget_argument_block* block) {
    uint64_t hash=14695981039346656037ull;
    if (!block) return hash;
    for(mi::Size i=0;i<block->get_size();++i) {
        hash^=static_cast<unsigned char>(block->get_data()[i]); hash*=1099511628211ull;
    }
    return hash;
}
std::array<float,3> evaluate(const mdl::TargetCode& target) {
    mi::neuraylib::Shading_state_material_with_derivs derivatives{};
    auto& state=reinterpret_cast<mi::neuraylib::Shading_state_material&>(derivatives);
    mi::neuraylib::tct_deriv_float3 uv{}; uv.val=mi::Float32_3(.37f,.61f,0);
    mi::Float32_3 tangentU(1,0,0),tangentV(0,1,0);
    MdlStateMatrixRow identity[4]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    alignas(16) MdlStateFloat4 results[128]{};
    derivatives.normal=derivatives.geom_normal=mi::Float32_3(0,0,1);
    derivatives.tangent_u=&tangentU; derivatives.tangent_v=&tangentV;
    derivatives.text_coords=&uv;
    derivatives.text_results=results; derivatives.world_to_object=identity;
    derivatives.object_to_world=identity;
    derivatives.ro_data_segment=target.program->ro_data_segment;
    derivatives.meters_per_scene_unit=1;
    require(target.program->target_code->execute_init(target.program->functions.init_function_index,
        state,target.texture_handler(),target.argument_data())==0,"material init failed");
    mi::neuraylib::Bsdf_evaluate_data<mi::neuraylib::DF_HSM_NONE> data{};
    data.k1=data.k2=mi::Float32_3(0,0,1); data.ior1=mi::Float32_3(1);
    data.ior2=mi::Float32_3(MI_NEURAYLIB_BSDF_USE_MATERIAL_IOR);
    require(target.program->target_code->execute_bsdf_evaluate(
        target.program->functions.surface_bsdf_function_index+
            mdl::MaterialFunctions::DF_EVALUATE,&data,state,target.texture_handler(),
        target.argument_data())==0,"material evaluation failed");
    return {data.bsdf_diffuse.x+data.bsdf_glossy.x,
            data.bsdf_diffuse.y+data.bsdf_glossy.y,
            data.bsdf_diffuse.z+data.bsdf_glossy.z};
}
void writePpm(const std::filesystem::path& path,int i) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    const unsigned char rgb[3]{static_cast<unsigned char>(17+i*7),
        static_cast<unsigned char>(239-i*5),static_cast<unsigned char>(31+i*3)};
    out<<"P6\n2 2\n255\n";
    for(int p=0;p<4;++p)out.write(reinterpret_cast<const char*>(rgb),3);
}
void writeStage(const std::filesystem::path& path,
                const std::filesystem::path& directory) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out<<"#usda 1.0\n\ndef Xform \"World\" {\n def Scope \"Looks\" {\n";
    for(int i=0;i<65;++i) {
        const auto texture=(directory/("t"+std::to_string(i%32)+".ppm")).generic_string();
        out<<"  def Material \"M"<<i<<"\" {\n"
              "   token outputs:mtlx:surface.connect = </World/Looks/M"<<i<<"/Surface.outputs:out>\n"
              "   def Shader \"Image\" {\n"
              "    uniform token info:id = \"ND_image_color3\"\n"
              "    asset inputs:file = @"<<texture<<"@\n"
              "    color3f outputs:out\n   }\n"
              "   def Shader \"Surface\" {\n"
              "    uniform token info:id = \"ND_open_pbr_surface_surfaceshader\"\n"
              "    float inputs:base_weight = "<<(0.2+0.01*i)<<"\n"
              "    color3f inputs:base_color.connect = </World/Looks/M"<<i<<"/Image.outputs:out>\n"
              "    float inputs:specular_weight = 0\n"
              "    token outputs:out\n   }\n  }\n";
    }
    out<<" }\n";
    for(int i=0;i<65;++i) out<<
        " def Mesh \"G"<<i<<"\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
        "  int[] faceVertexCounts = [4]\n  int[] faceVertexIndices = [0,1,2,3]\n"
        "  point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]\n"
        "  uniform token subdivisionScheme = \"none\"\n"
        "  rel material:binding = </World/Looks/M"<<i<<">\n"
        "  double3 xformOp:translate = ("<<(i%13)*2<<","<<(i/13)*2<<",0)\n"
        "  uniform token[] xformOpOrder = [\"xformOp:translate\"]\n }\n";
    out<<"}\n";
    require(bool(out),"cannot write standalone reuse USD fixture");
}
void writeClampStage(const std::filesystem::path& path,
                     const std::vector<GfVec3f>& colors) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out<<"#usda 1.0\n\ndef Xform \"World\" { def Scope \"Looks\" {\n";
    for(size_t i=0;i<colors.size();++i) out<<
        " def Material \"C"<<i<<"\" { token outputs:mtlx:surface.connect = </World/Looks/C"<<i<<"/Surface.outputs:out>\n"
        "  def Shader \"Surface\" { uniform token info:id = \"ND_standard_surface_surfaceshader\"\n"
        "   float inputs:base = 1\n   color3f inputs:base_color = ("
        <<colors[i][0]<<","<<colors[i][1]<<","<<colors[i][2]<<")\n"
        "   float inputs:specular = 0\n   token outputs:out\n  }\n }\n";
    out<<" }\n";
    for(size_t i=0;i<colors.size();++i) out<<
        " def Mesh \"Q"<<i<<"\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
        "  int[] faceVertexCounts=[4]\n  int[] faceVertexIndices=[0,1,2,3]\n"
        "  point3f[] points=[(0,0,0),(1,0,0),(1,1,0),(0,1,0)]\n"
        "  uniform token subdivisionScheme=\"none\"\n"
        "  rel material:binding=</World/Looks/C"<<i<<">\n }\n";
    out<<"}\n";
}
void writeSingleTextureStage(const std::filesystem::path& path,
                             const std::filesystem::path& texture,float weight) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out << "#usda 1.0\n\ndef Xform \"World\" { def Scope \"Looks\" {\n"
        " def Material \"M\" { token outputs:mtlx:surface.connect = </World/Looks/M/Surface.outputs:out>\n"
        "  def Shader \"Image\" { uniform token info:id=\"ND_image_color3\"\n"
        "   asset inputs:file=@" << texture.generic_string() << "@\n   color3f outputs:out\n  }\n"
        "  def Shader \"Surface\" { uniform token info:id=\"ND_open_pbr_surface_surfaceshader\"\n"
        "   float inputs:base_weight=" << weight << "\n   color3f inputs:base_color.connect=</World/Looks/M/Image.outputs:out>\n"
        "   float inputs:specular_weight=0\n   token outputs:out\n  }\n }\n }\n"
        " def Mesh \"G\" (prepend apiSchemas=[\"MaterialBindingAPI\"]) {\n"
        "  int[] faceVertexCounts=[4]\n  int[] faceVertexIndices=[0,1,2,3]\n"
        "  point3f[] points=[(0,0,0),(1,0,0),(1,1,0),(0,1,0)]\n"
        "  uniform token subdivisionScheme=\"none\"\n  rel material:binding=</World/Looks/M>\n }\n}\n";
}
}

int main() {
    try {
        const char* libraries=std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        const char* mdlPath=std::getenv("PRISM_MATERIALX_MDL");
        if(!libraries||!*libraries||!mdlPath||!*mdlPath)return 77;
        const auto root=std::filesystem::temp_directory_path()/
            ("prism-standalone-reuse-"+std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        struct Cleanup { std::filesystem::path p; ~Cleanup(){std::error_code e;
            std::filesystem::remove_all(p,e);} } cleanup{root};
        for(int i=0;i<32;++i)writePpm(root/("t"+std::to_string(i)+".ppm"),i);
        const auto usd=root/"reuse.usda"; writeStage(usd,root);
        // The loader compiles through the material service, whose templates
        // and process caches serve the process-lifetime runtime (as the CLI).
        mdl::Context& context=prism::materials::SharedMdlContext();
        require(context.is_valid(),"MDL context initialization failed");
        prism::materialx::CompatibleTemplateRegistry::Shared().clear();
        prism::materialx::CapturedMaterialRegistry::Shared().clear();
        prism::materialx::CapturedMaterialRegistry::Shared().resetStats();
        auto mesh=std::make_unique<USDMesh>(usd,&context,libraries,mdlPath);
        require(mesh->materials.size()==65,"standalone USD ingest lost material bindings");
        std::set<const mdl::CompiledProgram*> programs;
        std::set<uint64_t> argumentBlocks;
        std::set<std::array<int,3>> shadingValues;
        size_t fullyGenerated=0;
        for(const auto& [id,base] : mesh->materials) {
            auto* material=dynamic_cast<MDLMaterial*>(base);
            require(material&&material->is_valid(),"standalone material is not compiled MDL");
            const auto& target=material->get_target_code();
            programs.insert(target.program.get());
            argumentBlocks.insert(hashBlock(target.argument_data()));
            const auto color=evaluate(target);
            shadingValues.insert({int(std::lround(color[0]*100000)),
                int(std::lround(color[1]*100000)),int(std::lround(color[2]*100000))});
            fullyGenerated+=!material->get_generated_uniforms().empty();
        }
        require(programs.size()==1,
                "compatible standalone variants did not share one native program");
        require(fullyGenerated==1,
                "standalone variants performed more than one full MaterialX generation");
        require(prism::materialx::CapturedMaterialRegistry::Shared()
                    .stats().stockConversions==1,
                "standalone variants performed more than one stock USD conversion");
        require(argumentBlocks.size()>=32,
                "texture and scalar variants reused one argument binding");
        require(shadingValues.size()>=32,
                "standalone resource variants did not produce distinct shading");

        // M1 (t1, base_weight 0.21) is bound to the template of M0. Released
        // before the reference, which the process caches would otherwise
        // serve with the same bound material.
        auto* bound=dynamic_cast<MDLMaterial*>(mesh->materials.at(1));
        require(bound&&bound->get_generated_uniforms().empty(),
                "the second variant was not bound to the template");
        const auto templatedTextureValue=evaluate(bound->get_target_code());
        mesh.reset();
        prism::materialx::CompatibleTemplateRegistry::Shared().clear();
        const auto textureReference=root/"texture-reference.usda";
        writeSingleTextureStage(textureReference,root/"t1.ppm",0.21f);
        USDMesh textureFull(textureReference,&context,libraries,mdlPath);
        auto* fullTexture=dynamic_cast<MDLMaterial*>(textureFull.materials.at(0));
        require(fullTexture&&!fullTexture->get_generated_uniforms().empty(),
                "texture reference did not take the full generation path");
        const auto fullTextureValue=evaluate(fullTexture->get_target_code());
        for(size_t channel=0;channel<3;++channel)
            require(std::abs(templatedTextureValue[channel]-fullTextureValue[channel])<1e-6f,
                    "template texture gamma differs from full MaterialX generation");

        const std::vector<GfVec3f> clampColors{{.2f,.3f,.4f},{1.4f,.5f,.2f},
                                                {-.4f,.7f,.1f}};
        prism::materialx::CompatibleTemplateRegistry::Shared().clear();
        const auto clampVariants=root/"clamp-variants.usda";
        writeClampStage(clampVariants,clampColors);
        auto variants=std::make_unique<USDMesh>(clampVariants,&context,libraries,mdlPath);
        require(variants->materials.size()==3,"clamp variant fixture did not load");
        std::array<std::array<float,3>,2> boundValues{};
        for(size_t i=0;i<2;++i) {
            auto* material=dynamic_cast<MDLMaterial*>(variants->materials.at(int(i+1)));
            require(material&&material->get_generated_uniforms().empty(),
                    "clamp variant was not bound to the template");
            boundValues[i]=evaluate(material->get_target_code());
        }
        variants.reset();
        for(size_t i=0;i<2;++i) {
            prism::materialx::CompatibleTemplateRegistry::Shared().clear();
            const auto single=root/("clamp-full-"+std::to_string(i)+".usda");
            writeClampStage(single,{clampColors[i+1]});
            USDMesh full(single,&context,libraries,mdlPath);
            auto* material=dynamic_cast<MDLMaterial*>(full.materials.at(0));
            require(material&&!material->get_generated_uniforms().empty(),
                    "clamp reference did not take the full generation path");
            const auto value=evaluate(material->get_target_code());
            for(size_t channel=0;channel<3;++channel)
                require(std::abs(boundValues[i][channel]-value[channel])<1e-5f,
                        "shared binder did not preserve full-compile clamp boundaries");
        }
        std::cout<<"standalone 65-material template reuse passed programs="
                 <<programs.size()<<" bindings="<<argumentBlocks.size()<<'\n';
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"standalone_material_reuse: "<<error.what()<<'\n'; return 1;
    }
}
