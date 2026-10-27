#include "mdl_generator.h"
#include "generated_cache.h"
#include "../color/color_management.h"
#include "../resource_limits.h"
#include <MaterialXCore/Generated.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderStage.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenMdl/MdlShaderGenerator.h>
#include <iostream>
#include <map>
#include <cstdlib>
#include <mutex>
#include <filesystem>
#include <array>
#include <unordered_map>
#include <set>
#include <MaterialXGenShader/TypeDesc.h>
#include <MaterialXGenShader/Syntax.h>

namespace {
bool VerboseMdl() {
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string(value) != "0";
}

MaterialX::DocumentPtr SharedStandardLibrary(const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string, MaterialX::DocumentPtr> libraries;
    const auto key = std::filesystem::weakly_canonical(path).generic_string();
    std::lock_guard lock(mutex);
    auto& library = libraries[key];
    if (!library) {
        auto loaded = MaterialX::createDocument();
        MaterialX::FileSearchPath search;
        search.append(MaterialX::FilePath(path));
        MaterialX::loadLibraries({}, search, loaded);
        // Documents are immutable after publication. MaterialX protects its
        // internal lookup-cache construction; generator/GenContext stay local.
        library = std::move(loaded);
    }
    return library;
}

std::string SharedLibraryDigest(const std::string& path) {
    static std::mutex mutex;
    static std::map<std::string,std::string> digests;
    const auto key=std::filesystem::weakly_canonical(path).generic_string();
    std::lock_guard lock(mutex);
    auto& digest=digests[key];
    if(digest.empty()) digest=prism::materialx::MaterialXLibraryDigest(path);
    return digest;
}

std::string GeneratorOptionsIdentity(MaterialX::GenOptions const& o) {
    return std::to_string(int(o.shaderInterfaceType))+"|"+
        std::to_string(o.fileTextureVerticalFlip)+"|"+o.targetColorSpaceOverride+"|"+
        o.targetDistanceUnit+"|"+std::to_string(o.addUpstreamDependencies)+"|"+
        o.libraryPrefix.asString()+"|"+std::to_string(o.emitColorTransforms)+"|"+
        std::to_string(o.hwTransparency)+"|"+
        std::to_string(int(o.hwSpecularEnvironmentMethod))+"|"+
        std::to_string(int(o.hwDirectionalAlbedoMethod))+"|"+
        std::to_string(int(o.hwTransmissionRenderMethod))+"|"+
        std::to_string(o.hwSrgbEncodeOutput)+"|"+
        std::to_string(o.hwWriteDepthMoments)+"|"+std::to_string(o.hwShadowMap)+"|"+
        std::to_string(o.hwAmbientOcclusion)+"|"+
        std::to_string(o.hwMaxActiveLightSources)+"|"+
        std::to_string(o.hwNormalizeUdimTexCoords)+"|"+
        std::to_string(o.hwWriteAlbedoTable)+"|"+
        std::to_string(o.hwWriteEnvPrefilter)+"|"+
        std::to_string(o.hwImplicitBitangents);
}

// MaterialX's default colour management only has transforms into lin_rec709
// (cmlib's ND_<space>_to_lin_rec709_*); with another target it finds none and
// silently leaves tagged colours and textures undecoded. For other working
// spaces this library adds ND_<space>_to_<target>_color3/4: the cmlib decode
// to lin_rec709 followed by the working space's matrix, plus lin_rec709 to
// the target. Spaces already in the target get no transform, so every tagged
// value converts exactly once. The standard library stays the data library.
MaterialX::DocumentPtr ColorTransformLibrary(MaterialX::DocumentPtr const& stdlib,
                                             prism::color::ColorManagement const& color) {
    const std::string target=color.materialXSpace();
    if(target=="lin_rec709") return stdlib;
    static std::mutex mutex;
    static std::map<std::pair<const void*,std::string>,MaterialX::DocumentPtr> libraries;
    std::lock_guard lock(mutex);
    auto& library=libraries[{stdlib.get(),color.identity()+"|"+target}];
    if(library) return library;
    auto doc=MaterialX::createDocument();
    doc->setDataLibrary(stdlib);
    // transformmatrix multiplies a row vector (cmlib's matrices are the
    // transposes of the usual column-vector ones).
    const auto& a=color.fromRec709();
    const MaterialX::Matrix33 matrix(a[0],a[3],a[6],a[1],a[4],a[7],a[2],a[5],a[8]);
    std::set<std::string> sources{"lin_rec709"};
    const std::string suffix="_to_lin_rec709";
    for(const auto& nodeDef:stdlib->getNodeDefs()) {
        const std::string node=nodeDef->getNodeString();
        if(node.size()>suffix.size()&&node.ends_with(suffix))
            sources.insert(node.substr(0,node.size()-suffix.size()));
    }
    for(const auto& source:sources) {
        if(source==target) continue;
        const std::string node=source+"_to_"+target;
        for(const std::string type:{"color3","color4"}) {
            const std::string nodeDefName="ND_"+node+"_"+type;
            auto nodeDef=doc->addNodeDef(nodeDefName,type,node);
            nodeDef->setNodeGroup("colortransform");
            nodeDef->addInput("in",type)->setValueString(type=="color3"?"0, 0, 0":"0, 0, 0, 1");
            auto graph=doc->addNodeGraph("NG_"+node+"_"+type);
            graph->setNodeDefString(nodeDefName);
            if(type=="color3") {
                MaterialX::NodePtr linear;
                if(source!="lin_rec709") {
                    linear=graph->addNode(source+suffix,"decode","color3");
                    linear->addInput("in","color3")->setInterfaceName("in");
                }
                auto constant=graph->addNode("constant","mat","matrix33");
                constant->setInputValue("value",matrix);
                auto vector=graph->addNode("convert","asVec","vector3");
                auto vectorIn=vector->addInput("in","color3");
                if(linear) vectorIn->setConnectedNode(linear); else vectorIn->setInterfaceName("in");
                auto transform=graph->addNode("transformmatrix","transform","vector3");
                transform->addInput("in","vector3")->setConnectedNode(vector);
                transform->addInput("mat","matrix33")->setConnectedNode(constant);
                auto colorOut=graph->addNode("convert","asColor","color3");
                colorOut->addInput("in","vector3")->setConnectedNode(transform);
                graph->addOutput("out","color3")->setConnectedNode(colorOut);
            } else {
                auto rgb=graph->addNode("convert","asColor3","color3");
                rgb->addInput("in","color4")->setInterfaceName("in");
                auto transform=graph->addNode(node,"transform","color3");
                transform->addInput("in","color3")->setConnectedNode(rgb);
                auto alpha=graph->addNode("extract","alpha","float");
                alpha->addInput("in","color4")->setInterfaceName("in");
                alpha->setInputValue("index",3);
                auto rgba=graph->addNode("combine2","asColor4","color4");
                rgba->addInput("in1","color3")->setConnectedNode(transform);
                rgba->addInput("in2","float")->setConnectedNode(alpha);
                graph->addOutput("out","color4")->setConnectedNode(rgba);
            }
        }
    }
    std::string errors;
    if(!doc->validate(&errors))
        throw std::runtime_error("PrismRT: invalid colour transform library for "+target+": "+errors);
    library=doc;
    return library;
}

bool HasExternalImplementationSources(MaterialX::DocumentPtr const& doc) {
    const auto documentSource=doc->getSourceUri();
    for(const auto& element:doc->traverseTree()) {
        const auto source=element->getSourceUri();
        // XmlIo serializes these children as XIncludes, so the normalized XML
        // contains only their URI rather than the implementation content.
        if(!source.empty()&&source!=documentSource)return true;
    }
    return false;
}

struct ColorTemplate {
    GeneratedMDL generated;
    std::vector<std::pair<std::string, std::pair<size_t,size_t>>> spans;
    size_t bytes = 0;
    uint64_t lastUse = 0;
};
std::mutex templateMutex;
std::array<std::mutex,64> templateKeyMutexes;
std::unordered_map<std::string, ColorTemplate> colorTemplates;
size_t templateBytes = 0;
uint64_t templateClock = 0;

// Restrict the first template fast path to exposed color defaults. Graph,
// scalar controls, node definitions, colorspace transforms and connections stay
// in the signature; unsupported/nonpublic defaults take normal generation.
std::string ColorTemplateKey(MaterialX::DocumentPtr const& doc,
                             std::vector<MaterialX::InputPtr>& colors) {
    auto normalized=doc->copy();
    for (const auto& node : doc->getNodes()) {
        if (node->getCategory()!="open_pbr_surface" && node->getCategory()!="standard_surface") continue;
        for (const auto& input : node->getInputs()) {
            if (input->getType()!="color3" || !input->getValue() ||
                input->hasNodeName() || input->hasNodeGraphString() ||
                input->hasInterfaceName()) continue;
            colors.push_back(input);
            auto replacement=normalized->getDescendant(input->getNamePath())->asA<MaterialX::Input>();
            replacement->setValue(MaterialX::Color3(0.0f));
        }
    }
    return colors.empty() ? std::string{} : MaterialX::writeToXmlString(normalized);
}

bool FindColorSpans(const std::string& source,
                    const std::vector<MaterialX::InputPtr>& colors, ColorTemplate& result) {
    const auto material=source.find("export material ");
    const auto open=source.find('(',material);
    if(material==std::string::npos || open==std::string::npos) return false;
    size_t end=open+1; int depth=1; bool quoted=false,escaped=false;
    for(;end<source.size() && depth;++end) {
        const char c=source[end];
        if(quoted) { if(escaped) escaped=false; else if(c=='\\') escaped=true; else if(c=='"') quoted=false; continue; }
        if(c=='"') quoted=true; else if(c=='(') ++depth; else if(c==')') --depth;
    }
    if(depth) return false;
    for(const auto& color : colors) {
        const auto marker=std::string("materialx::core::origin(\"")+color->getNamePath()+"\")";
        const auto origin=source.find(marker,open);
        if(origin==std::string::npos || origin>=end || source.find(marker,origin+1)!=std::string::npos) return false;
        const auto annotation=source.rfind("[[",origin);
        const auto equal=source.rfind(" = ",annotation);
        if(annotation==std::string::npos || equal==std::string::npos || equal<=open) return false;
        const size_t begin=equal+3;
        size_t finish=annotation;
        while(finish>begin && std::isspace(static_cast<unsigned char>(source[finish-1]))) --finish;
        if(source.compare(begin,6,"color(")!=0) return false;
        result.spans.push_back({color->getNamePath(),{begin,finish}});
    }
    std::sort(result.spans.begin(),result.spans.end(),[](const auto& a,const auto& b){return a.second.first>b.second.first;});
    return true;
}
}
#include <mutex>

class MDLGenerator::Impl {
public:
    Impl(const std::string& mx_libraries_path, prism::color::ColorManagement const* color)
        : m_generator(MaterialX::MdlShaderGenerator::create())
        , m_context(m_generator)
    {
        // mx_libraries_path = ".../MaterialX/install/libraries"
        MaterialX::FilePath librariesPath(mx_libraries_path);
        MaterialX::FilePath installRoot = librariesPath.getParentPath();

        // loadLibraries needs the libraries folder as the search root
        MaterialX::FileSearchPath librariesSearchPath;
        librariesSearchPath.append(librariesPath);

        m_stdlib = SharedStandardLibrary(mx_libraries_path);
        m_libraryDigest = SharedLibraryDigest(mx_libraries_path);

        // Ready for color (else for the first generation's).
        if (color) syncColorManagement(*color);
        m_diskCache=std::make_unique<prism::materialx::GeneratedTemplateDiskCache>(
            prism::PersistentCacheRoot(),prism::GeneratedArtifactBudgetBytes());

        if (VerboseMdl()) std::cout << "[DEBUG] stdlib elements loaded: "
                << m_stdlib->getChildren().size() << "\n";

        // registerSourceCodeSearchPath needs the libraries folder itself
        // so the generator can resolve e.g. stdlib/genmdl/mx_stdlib.mdl
        // Register source code search paths for MDL implementation files
        MaterialX::FileSearchPath sourceSearchPath;
        sourceSearchPath.append(librariesPath);
        sourceSearchPath.append(librariesPath / MaterialX::FilePath("mdl/materialx"));
        m_context.registerSourceCodeSearchPath(sourceSearchPath);

        if (VerboseMdl()) std::cout << "[DEBUG] Source search root: " << installRoot.asString() << "\n";
    }

    MaterialX::DocumentPtr get_stdlib() const { return m_stdlib; }

    // Decode authored color spaces in the graph into the active working
    // space, leaving scalar/vector data textures (roughness, metallic,
    // normals) and untagged colours as they are. Called with generate_mutex
    // held (or from the constructor), before each generation.
    void syncColorManagement(prism::color::ColorManagement const& colorManagement) {
        const auto* color=&colorManagement;
        if(color->identity()==m_colorIdentity) return;
        auto color_management = MaterialX::DefaultColorManagementSystem::create(
            m_generator->getTarget());
        color_management->loadLibrary(ColorTransformLibrary(m_stdlib,*color));
        m_generator->setColorManagementSystem(color_management);
        m_context.clearNodeImplementations();
        m_context.getOptions().targetColorSpaceOverride = color->materialXSpace();
        m_optionsIdentity = GeneratorOptionsIdentity(m_context.getOptions());
        // Matrices of a working space MaterialX has no name for differ in the
        // generated source only; linear Rec.709 keeps its earlier identity.
        if(!color->rec709()) m_optionsIdentity += "|"+color->identity();
        m_colorIdentity=color->identity();
    }

GeneratedMDL generateCached(MaterialX::DocumentPtr doc, const std::string& material_name, bool useCache) {
    std::vector<MaterialX::InputPtr> colors;
    const auto structural=doc ? ColorTemplateKey(doc,colors) : std::string{};
    if(structural.empty() || m_libraryDigest.empty() ||
       doc->getDataLibrary()!=m_stdlib || HasExternalImplementationSources(doc) ||
       !useCache) return generate(doc,material_name);
    const auto persistentKey=prism::materialx::GeneratedTemplateKey({
        "prism-generated-mdl-color-template-v1",
        std::to_string(MATERIALX_MAJOR_VERSION)+"."+
            std::to_string(MATERIALX_MINOR_VERSION)+"."+
            std::to_string(MATERIALX_BUILD_VERSION),
        m_generator->getTarget(),m_optionsIdentity,m_libraryDigest,
        material_name,structural});
    const auto& key=persistentKey;
    std::lock_guard keyLock(templateKeyMutexes[std::hash<std::string>{}(key)%templateKeyMutexes.size()]);
    ColorTemplate entry;
    bool found=false;
    bool diskFound=false;
    { std::lock_guard lock(templateMutex); const auto it=colorTemplates.find(key); if(it!=colorTemplates.end()) {it->second.lastUse=++templateClock; entry=it->second; found=true;} }
    if(!found) {
        if(auto artifact=m_diskCache->load(persistentKey)) {
            entry.generated.module_name=std::move(artifact->moduleName);
            entry.generated.mdl_source=std::move(artifact->source);
            entry.generated.texture_paths=std::move(artifact->texturePaths);
            for (auto& uniform : artifact->uniforms) {
                auto element = doc->getDescendant(uniform.path);
                auto input = element ? element->asA<MaterialX::Input>() : nullptr;
                entry.generated.uniforms.push_back({std::move(uniform.name),
                    std::move(uniform.path), std::move(uniform.type),
                    input ? input->getValue() : MaterialX::ValuePtr{}});
            }
            entry.generated.success=true;
            entry.spans=std::move(artifact->spans);
            found=diskFound=true;
        }
    }
    if(found) {
        for(const auto& [path,span]:entry.spans) {
            const auto element=doc->getDescendant(path);
            const auto input=element?element->asA<MaterialX::Input>():nullptr;
            if(!input||input->getType()!="color3"||!input->getValue()) {
                found=false;break;
            }
        }
        for (const auto& uniform : entry.generated.uniforms) {
            const auto element = doc->getDescendant(uniform.path);
            const auto input = element ? element->asA<MaterialX::Input>() : nullptr;
            // Input-block metadata can include ports inherited from the data
            // library, which are intentionally absent from this small user
            // document. The library digest and structural key validate those
            // ports. If the path is authored locally, its type must still
            // agree; its value may be a connection or an inherited default.
            if (input && input->getType() != uniform.type) {
                found = false; break;
            }
        }
    }
    if(found) {
        // Older/on-disk artifacts predate the validated uniform map. A source
        // template alone is insufficient for direct SDK argument binding, so
        // generate one seed to publish authoritative ShaderPort metadata.
        if (entry.generated.uniforms.empty())
            return generate(doc, material_name);
        if(diskFound) {
            size_t bytes=key.size()+entry.generated.mdl_source.size()+sizeof(ColorTemplate);
            for(const auto& span:entry.spans)bytes+=sizeof(span)+span.first.size();
            constexpr size_t limit=64*1024*1024;
            if(bytes<=limit) {
                std::lock_guard lock(templateMutex);
                while(templateBytes>limit-bytes&&!colorTemplates.empty()) {
                    const auto oldest=std::min_element(colorTemplates.begin(),colorTemplates.end(),
                        [](const auto& a,const auto& b){return a.second.lastUse<b.second.lastUse;});
                    templateBytes-=oldest->second.bytes;colorTemplates.erase(oldest);
                }
                entry.bytes=bytes;entry.lastUse=++templateClock;
                colorTemplates.emplace(key,entry);templateBytes+=bytes;
            }
        }
        auto result=entry.generated;
        // Match MdlShaderGenerator::generate's thread-local formatting scope,
        // including its rounding. This also makes generated source identical.
        MaterialX::ScopedFloatFormatting formatting(MaterialX::Value::FloatFormatFixed);
        for(const auto& [path,span] : entry.spans) {
            auto input=doc->getDescendant(path)->asA<MaterialX::Input>();
            const auto text=m_generator->getSyntax().getValue(MaterialX::Type::COLOR3,*input->getValue(),true);
            result.mdl_source.replace(span.first,span.second-span.first,text);
        }
        result.generationCacheHit=true;
        result.generationPersistentCacheHit=diskFound;
        return result;
    }
    auto result=generate(doc,material_name);
    if(result.success && FindColorSpans(result.mdl_source,colors,entry)) {
        entry.generated=result;
        prism::materialx::GeneratedTemplateArtifact artifact{
            result.module_name,result.mdl_source,result.texture_paths,entry.spans};
        for (const auto& uniform : result.uniforms)
            artifact.uniforms.push_back({uniform.name, uniform.path, uniform.type});
        m_diskCache->store(persistentKey,artifact);
        std::lock_guard lock(templateMutex);
        size_t bytes=key.size()+result.mdl_source.size()+sizeof(ColorTemplate);
        for (const auto& span : entry.spans) bytes += sizeof(span) + span.first.size();
        // Entries are copied before use; evict only the least recently used
        // source strings, without invalidating a worker's template or SDK code.
        constexpr size_t limit=64*1024*1024;
        if (bytes <= limit) {
            while (templateBytes > limit-bytes && !colorTemplates.empty()) {
                const auto oldest=std::min_element(colorTemplates.begin(),colorTemplates.end(),
                    [](const auto& a,const auto& b) { return a.second.lastUse < b.second.lastUse; });
                templateBytes -= oldest->second.bytes;
                colorTemplates.erase(oldest);
            }
            entry.bytes=bytes;
            entry.lastUse=++templateClock;
            colorTemplates.emplace(key,std::move(entry));
            templateBytes+=bytes;
        }
    }
    return result;
}

GeneratedMDL generate(MaterialX::DocumentPtr doc, const std::string& material_name) {
    GeneratedMDL result;
    result.success = false;

    if (!doc) {
        result.error_message = "Null document";
        return result;
    }

    // Debug: print all nodes in the document
    if (VerboseMdl()) { std::cout << "[DEBUG] All nodes in document:\n";
    for (auto& node : doc->getNodes()) {
        std::cout << "  node: '" << node->getName()
                  << "' category: '" << node->getCategory()
                  << "' type: '" << node->getType() << "'\n";
        for (auto& input : node->getInputs()) {
            std::cout << "    input: '" << input->getName()
                      << "' type: '" << input->getType()
                      << "' nodename: '" << input->getNodeName() << "'\n";
        }
    } }

    // 1. Try a top-level output directly
    MaterialX::OutputPtr output = doc->getOutput(material_name);
    if (output)
        return generate_from_output(output, material_name);

    // 2. Try a surfacematerial node
    MaterialX::NodePtr material_node = doc->getNode(material_name);
    if (material_node) {
        // Generate directly from the material node — the MDL generator
        // handles surfacematerial as the top-level entry point
        return generate_from_node(material_node, material_name);
    }

    result.error_message = "Material/output not found: " + material_name;
    return result;
}

    GeneratedMDL generate_from_output(MaterialX::OutputPtr output,
                                      const std::string& name) {
        GeneratedMDL result;
        result.success = false;

        try {
            MaterialX::ShaderPtr shader = m_generator->generate(name, output, m_context);

            if (!shader) {
                result.error_message = "Failed to generate shader from output";
                return result;
            }

            const MaterialX::ShaderStage& pixelStage = shader->getStage(MaterialX::Stage::PIXEL);
            result.mdl_source  = pixelStage.getSourceCode();
            result.module_name = "::materialx::" + name;
            extract_textures(shader, result);
            result.success = true;

        } catch (const std::exception& e) {
            result.error_message = std::string("Code generation failed: ") + e.what();
        }

        return result;
    }

    GeneratedMDL generate_from_node(MaterialX::NodePtr node, const std::string& name) {
        GeneratedMDL result;
        result.success = false;

        try {
            // Debug: check nodedef resolution before generating
            MaterialX::NodeDefPtr nodedef = node->getNodeDef();
            if (!nodedef && VerboseMdl()) {
                std::cout << "[DEBUG] getNodeDef() returned null for '"
                        << node->getName() << "' (category: '"
                        << node->getCategory() << "')\n";
                std::cout << "[DEBUG] Document has "
                        << node->getDocument()->getNodeDefs().size()
                        << " nodedefs total\n";
                // List first few nodedefs to see what's actually loaded
                int count = 0;
                for (auto& nd : node->getDocument()->getNodeDefs()) {
                    std::cout << "  nodedef: '" << nd->getName()
                            << "' node: '" << nd->getNodeString() << "'\n";
                    if (++count >= 5) { std::cout << "  ...\n"; break; }
                }
            } else if (VerboseMdl()) {
                std::cout << "[DEBUG] nodedef found: '" << nodedef->getName() << "'\n";
            }

            MaterialX::ShaderPtr shader = m_generator->generate(name, node, m_context);

            if (!shader) {
                result.error_message = "Failed to generate shader from node";
                return result;
            }

            const MaterialX::ShaderStage& pixelStage = shader->getStage(MaterialX::Stage::PIXEL);
            result.mdl_source  = pixelStage.getSourceCode();
            result.module_name = "::materialx::" + name;
            extract_textures(shader, result);
            result.success = true;

        } catch (const std::exception& e) {
            result.error_message = std::string("Code generation failed: ") + e.what();
        }

        return result;
    }

    void extract_textures(MaterialX::ShaderPtr shader, GeneratedMDL& result) {
        const MaterialX::ShaderStage& pixelStage = shader->getStage(MaterialX::Stage::PIXEL);

        auto capture = [&](const auto& blocks) {
        for (const auto& [blockName, block] : blocks) {
            for (size_t i = 0; i < block->size(); ++i) {
                const MaterialX::ShaderPort* variable = (*block)[i];
                if (!variable) continue;

                result.uniforms.push_back({variable->getVariable(),
                    variable->getPath(), variable->getType().getName(),
                    variable->getValue()});

                if (variable->getType().getName() == "filename") {
                    auto value = variable->getValue();
                    if (value) {
                        result.texture_paths.push_back(value->getValueString());
                    }
                }
            }
        }};
        // MdlShaderGenerator publishes material parameters in its input block;
        // hardware generators generally use uniform blocks. Capture both and
        // let template publication reject duplicate/ambiguous paths.
        capture(pixelStage.getInputBlocks());
        capture(pixelStage.getUniformBlocks());
    }

    // The GenContext is shared state, so a shared generator must not run
    // two generations at once.
    std::mutex generate_mutex;

private:
    MaterialX::ShaderGeneratorPtr m_generator;
    MaterialX::GenContext         m_context;
    MaterialX::DocumentPtr        m_stdlib;
    std::string m_libraryDigest;
    std::string m_optionsIdentity;
    std::string m_colorIdentity;
    std::unique_ptr<prism::materialx::GeneratedTemplateDiskCache> m_diskCache;
};

MDLGenerator::MDLGenerator(const std::string& mx_libraries_path,
                           prism::color::ColorManagement const* color)
    : m_impl(std::make_unique<Impl>(mx_libraries_path, color))
{}

MDLGenerator::~MDLGenerator() = default;

MaterialX::DocumentPtr MDLGenerator::get_stdlib() const {
    return m_impl->get_stdlib();
}

std::shared_ptr<MDLGenerator> MDLGenerator::shared(const std::string& mx_libraries_path) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<MDLGenerator>> generators;

    std::lock_guard<std::mutex> lock(mutex);
    auto& generator = generators[mx_libraries_path];
    if (!generator)
        generator = std::make_shared<MDLGenerator>(mx_libraries_path);
    return generator;
}

namespace {
// Generators not owned by any thread, by libraries path. Each is used by one
// thread at a time: workerLocal takes it and the thread's exit returns it.
struct IdleGenerators {
    std::mutex mutex;
    std::map<std::string, std::vector<std::shared_ptr<MDLGenerator>>> byPath;
};
constexpr size_t kIdleGeneratorLimit = 64;

IdleGenerators& Idle() {
    // Leaked: thread-local owners may return generators during process exit.
    static auto& idle = *new IdleGenerators();
    return idle;
}

struct WorkerGenerators {
    std::map<std::string, std::shared_ptr<MDLGenerator>> byPath;
    ~WorkerGenerators() {
        auto& idle = Idle();
        std::lock_guard lock(idle.mutex);
        for (auto& [path, generator] : byPath) {
            auto& pool = idle.byPath[path];
            // A generator a caller still holds could be shared by two threads.
            if (generator && generator.use_count() == 1 && pool.size() < kIdleGeneratorLimit)
                pool.push_back(std::move(generator));
        }
    }
};
}

std::shared_ptr<MDLGenerator> MDLGenerator::workerLocal(
    const std::string& mx_libraries_path, prism::color::ColorManagement const* color) {
    thread_local WorkerGenerators generators;
    auto& generator = generators.byPath[mx_libraries_path];
    if (!generator) {
        auto& idle = Idle();
        {
            std::lock_guard lock(idle.mutex);
            auto& pool = idle.byPath[mx_libraries_path];
            if (!pool.empty()) {
                generator = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!generator)
            generator = std::make_shared<MDLGenerator>(mx_libraries_path, color);
    }
    return generator;
}

void MDLGenerator::reserveWorkers(const std::string& mx_libraries_path, size_t count,
                                  prism::color::ColorManagement const* color) {
    auto& idle = Idle();
    count = std::min(count, kIdleGeneratorLimit);
    for (size_t built = 0; built < count; ++built) {
        {
            std::lock_guard lock(idle.mutex);
            if (idle.byPath[mx_libraries_path].size() >= count) return;
        }
        // Constructed unlocked: the first one loads the standard library.
        auto generator = std::make_shared<MDLGenerator>(mx_libraries_path, color);
        std::lock_guard lock(idle.mutex);
        idle.byPath[mx_libraries_path].push_back(std::move(generator));
    }
}

GeneratedMDL MDLGenerator::generate(MaterialX::DocumentPtr doc,
                                    const std::string& material_name,
                                    prism::color::ColorManagement const& color,
                                    bool useGenerationCache) {
    std::lock_guard<std::mutex> lock(m_impl->generate_mutex);
    m_impl->syncColorManagement(color);
    return m_impl->generateCached(doc, material_name, useGenerationCache);
}

GeneratedMDL MDLGenerator::generate(MaterialX::OutputPtr output,
                                    prism::color::ColorManagement const& color) {
    std::lock_guard<std::mutex> lock(m_impl->generate_mutex);
    m_impl->syncColorManagement(color);
    std::string name = output->getName();
    return m_impl->generate_from_output(output, name);
}
