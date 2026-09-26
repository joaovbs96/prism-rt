#include "materials.h"

#include "../mdl/context.h"
#include "../mdl_material.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/XmlIo.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE
namespace mx = MaterialX;

namespace prism::hydra {
namespace {

// The MDL SDK has process-wide runtime state; separate renderer instances must
// not enter module loading/backend compilation concurrently.
std::mutex gMdlRuntimeMutex;
struct SharedCacheEntry {
    std::string xml;
    std::shared_ptr<Material> material;
};
std::unordered_map<std::string, SharedCacheEntry> gMaterialCache;

std::string EnvOr(const char* name, std::string value)
{
    if (value.empty()) {
        if (const char* env = std::getenv(name)) value = env;
    }
    return value;
}

// FNV-1a is used only to name immutable temporary inputs within this local
// renderer cache. The complete network XML is still compared before reusing
// a compiled material, so a hash collision cannot silently select another shader.
std::string HashText(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(hash));
    return buffer;
}

TfToken SurfaceTerminalName(HdMaterialNetworkInterface& net)
{
    const TfTokenVector terminals = net.GetTerminalNames();
    for (const TfToken& terminal : terminals) {
        if (terminal == TfToken("surface") || terminal == TfToken("surfaceshader"))
            return terminal;
    }
    for (const TfToken& terminal : terminals) {
        const auto connection = net.GetTerminalConnection(terminal);
        if (connection.first &&
            HdMtlxGetMxTerminalName(&net, connection.second.upstreamNodeName) == "surfaceshader")
            return terminal;
    }
    return {};
}

bool IsPreviewSurfaceNetwork(HdMaterialNetworkInterface& net)
{
    const TfToken terminalName = SurfaceTerminalName(net);
    if (terminalName.IsEmpty()) return false;
    const auto terminal = net.GetTerminalConnection(terminalName);
    if (!terminal.first) return false;
    const TfToken surfaceType = net.GetNodeType(terminal.second.upstreamNodeName);
    // The universal USD context is supported when it is a Preview Surface
    // graph. hdMtlx supplies the installed UsdPreviewSurface/UsdUVTexture
    // definitions and performs graph conversion, including texture outputs.
    if (surfaceType == TfToken("UsdPreviewSurface") ||
        surfaceType == TfToken("ND_UsdPreviewSurface_surfaceshader"))
        return true;
    return false;
}

bool RewritePreviewSurfaceNetwork(HdMaterialNetworkInterface& net)
{
    const TfToken terminalName = SurfaceTerminalName(net);
    if (terminalName.IsEmpty()) return false;
    const auto terminal = net.GetTerminalConnection(terminalName);
    if (!terminal.first ||
        (net.GetNodeType(terminal.second.upstreamNodeName) != TfToken("UsdPreviewSurface") &&
         net.GetNodeType(terminal.second.upstreamNodeName) != TfToken("ND_UsdPreviewSurface_surfaceshader")))
        return false;

    auto mappedType = [](const TfToken& type) -> TfToken {
        const std::string value = type.GetString();
        if (value == "UsdPreviewSurface") return TfToken("ND_UsdPreviewSurface_surfaceshader");
        if (value == "UsdUVTexture") return TfToken("ND_UsdUVTexture_23");
        if (value == "UsdTransform2d") return TfToken("ND_UsdTransform2d");
        const std::string readerPrefix = "UsdPrimvarReader_";
        if (value.rfind(readerPrefix, 0) == 0) {
            std::string typeName = value.substr(readerPrefix.size());
            if (typeName == "float2") typeName = "vector2";
            else if (typeName == "float3") typeName = "vector3";
            else if (typeName == "float4") typeName = "vector4";
            return TfToken("ND_UsdPrimvarReader_" + typeName);
        }
        return {};
    };
    auto mappedOutput = [](const TfToken& type, const TfToken& output) {
        if ((type == TfToken("UsdPreviewSurface") ||
             type == TfToken("ND_UsdPreviewSurface_surfaceshader") ||
             type == TfToken("UsdTransform2d") ||
             type == TfToken("ND_UsdTransform2d") ||
             type == TfToken("UsdPrimvarReader_float") ||
             type == TfToken("UsdPrimvarReader_float2") ||
             type == TfToken("UsdPrimvarReader_float3") ||
             type == TfToken("UsdPrimvarReader_float4") ||
             type == TfToken("ND_UsdPrimvarReader_float") ||
             type == TfToken("ND_UsdPrimvarReader_vector2") ||
             type == TfToken("ND_UsdPrimvarReader_vector3") ||
             type == TfToken("ND_UsdPrimvarReader_vector4")) &&
            (output == TfToken("surface") || output == TfToken("result")))
            return TfToken("out");
        return output;
    };

    for (const TfToken& node : net.GetNodeNames()) {
        const TfToken sourceType = net.GetNodeType(node);
        const TfToken translated = mappedType(sourceType);
        if (!translated.IsEmpty()) net.SetNodeType(node, translated);
        else if (sourceType.GetString().rfind("ND_", 0) != 0) {
            std::cerr << "[Hydra Material] Unsupported node '" << sourceType
                      << "' in UsdPreviewSurface graph\n";
            return false;
        }

        for (const TfToken& input : net.GetNodeInputConnectionNames(node)) {
            auto connections = net.GetNodeInputConnection(node, input);
            for (auto& connection : connections) {
                const TfToken upstreamType = net.GetNodeType(connection.upstreamNodeName);
                connection.upstreamOutputName =
                    mappedOutput(upstreamType, connection.upstreamOutputName);
            }
            net.SetNodeInputConnection(node, input, connections);
        }
    }

    HdMaterialNetworkInterface::InputConnection translatedTerminal = terminal.second;
    translatedTerminal.upstreamOutputName = mappedOutput(
        net.GetNodeType(translatedTerminal.upstreamNodeName),
        translatedTerminal.upstreamOutputName);
    net.SetTerminalConnection(terminalName, translatedTerminal);
    return true;
}

std::string ResolveTextureAssets(HdMaterialNetworkInterface& net,
                                 const std::filesystem::path& cacheDir)
{
    ArResolver& resolver = ArGetResolver();
    std::string dependencyFingerprint;
    for (const TfToken& node : net.GetNodeNames()) {
        // A connected input must be represented by the connection alone.
        // Keeping the authored/default value alongside its connection creates
        // invalid MaterialX with two bindings for one input.
        for (const TfToken& input : net.GetNodeInputConnectionNames(node)) {
            if (!net.GetNodeInputConnection(node, input).empty())
                net.DeleteNodeParameter(node, input);
        }
        const TfTokenVector params = net.GetAuthoredNodeParameterNames(node);
        for (const TfToken& param : params) {
            if (param == TfToken("wrapS") || param == TfToken("wrapT")) {
                HdMaterialNetworkInterface::NodeParamData wrap =
                    net.GetNodeParameterData(node, param);
                if (wrap.value.IsHolding<TfToken>() &&
                    wrap.value.UncheckedGet<TfToken>() == TfToken("repeat")) {
                    wrap.value = TfToken("periodic");
                    net.SetNodeParameterData(node, param, wrap);
                }
            }
            if (param != TfToken("file") && param != TfToken("filename")) continue;
            HdMaterialNetworkInterface::NodeParamData data =
                net.GetNodeParameterData(node, param);
            std::string authored;
            std::string preResolved;
            if (data.value.IsHolding<SdfAssetPath>()) {
                const SdfAssetPath& asset = data.value.UncheckedGet<SdfAssetPath>();
                authored = asset.GetAssetPath();
                preResolved = asset.GetResolvedPath();
            } else if (data.value.IsHolding<std::string>()) {
                authored = data.value.UncheckedGet<std::string>();
            }
            if (authored.empty()) continue;

            const ArResolvedPath resolved = !preResolved.empty()
                ? ArResolvedPath(preResolved) : resolver.Resolve(authored);
            if (resolved.empty()) {
                std::cerr << "[Hydra Material] Could not resolve texture '"
                          << authored << "' on node " << node << "\n";
                continue;
            }
            auto asset = resolver.OpenAsset(resolved);
            if (!asset) {
                std::cerr << "[Hydra Material] Cannot open resolved texture '"
                          << resolved.GetPathString() << "'\n";
                continue;
            }
            const auto bytes = asset->GetBuffer();
            if (!bytes) continue;
            const size_t size = asset->GetSize();
            std::string content(bytes.get(), size);
            const std::string contentHash = HashText(content);
            dependencyFingerprint += contentHash;
            dependencyFingerprint += std::to_string(size);
            std::error_code ec;
            std::filesystem::create_directories(cacheDir, ec);
            if (ec) continue;
            std::string extension = std::filesystem::path(authored).extension().string();
            if (extension.empty()) extension = ".bin";
            const std::filesystem::path materialized = cacheDir / (contentHash + extension);
            if (!std::filesystem::exists(materialized, ec)) {
                std::ofstream output(materialized, std::ios::binary | std::ios::trunc);
                if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size())))
                    continue;
            }
            // Keep resource URIs relative to the generated .mtlx file. The
            // MDL SDK rejects Windows drive prefixes in resource literals;
            // MaterialCompiler registers this document's directory as a
            // resource search path before compilation.
            data.value = materialized.filename().string();
            data.typeName = TfToken("asset");
            net.SetNodeParameterData(node, param, data);
        }
    }
    return dependencyFingerprint;
}

void FixPreviewSurfaceNormalOutput(const mx::DocumentPtr& doc)
{
    if (!doc) return;
    for (const mx::NodePtr& node : doc->getNodes()) {
        if (node->getCategory() == "UsdPreviewSurface") {
            if (mx::InputPtr normalInput = node->getInput("normal"))
                normalInput->setType("vector3");
        }
    }
    for (const mx::NodeGraphPtr& graph : doc->getNodeGraphs()) {
        mx::OutputPtr normal = graph->getOutput("normal_rgb");
        if (!normal || normal->getType() != "color3" || !normal->hasNodeName())
            continue;
        const std::string upstreamName = normal->getNodeName();
        const std::string upstreamOutput = normal->getOutputString();
        mx::NodePtr convert = graph->addNode(
            "convert", "normal_rgb_to_vector3", "vector3");
        mx::InputPtr input = convert->addInput("in", "color3");
        input->setNodeName(upstreamName);
        input->setOutputString(upstreamOutput);
        normal->setNodeName(convert->getName());
        normal->removeAttribute("output");
        normal->setType("vector3");
    }
}

std::string SerializeDocument(const mx::DocumentPtr& doc)
{
    return mx::writeToXmlString(doc);
}

} // namespace

mdl::Context& SharedMdlContext()
{
    // Intentionally process-lifetime: MDL target code and backend objects use
    // interfaces owned by INeuray, including across renderer replacement.
    static mdl::Context* context = new mdl::Context();
    return *context;
}

class MaterialService::Impl {
public:
    Impl(mdl::Context* context, std::string libraries, std::string mdlPath)
        : context(context)
        , libraries(EnvOr("PRISM_MATERIALX_LIBRARY_PATH", std::move(libraries)))
        , mdlPath(EnvOr("PRISM_MATERIALX_MDL", std::move(mdlPath)))
    {
        if (this->mdlPath.empty() && !this->libraries.empty())
            this->mdlPath = (std::filesystem::path(this->libraries) / "mdl").string();
        cacheDir = std::filesystem::temp_directory_path() / "prism-rt" / "hydra-materials";
    }

    std::shared_ptr<Material> Resolve(
        const HdContainerDataSourceHandle& primDataSource,
        const SdfPath& materialPath)
    {
        std::lock_guard<std::mutex> runtimeLock(gMdlRuntimeMutex);
        std::lock_guard<std::mutex> lock(mutex);
        if (!context || !context->is_valid() || libraries.empty() || !primDataSource)
            return nullptr;

        HdMaterialSchema material = HdMaterialSchema::GetFromParent(primDataSource);
        if (!material) return nullptr;

        TfToken contextName("mtlx");
        HdMaterialNetworkSchema network = material.GetMaterialNetwork(contextName);
        if (!network) {
            contextName = HdMaterialSchemaTokens->universalRenderContext;
            network = material.GetMaterialNetwork(contextName);
        }
        if (!network) return nullptr;

        HdDataSourceMaterialNetworkInterface netInterface(
            materialPath, network.GetContainer(), primDataSource);
        const TfToken terminalName = SurfaceTerminalName(netInterface);
        if (terminalName.IsEmpty()) return nullptr;
        const auto terminal = netInterface.GetTerminalConnection(terminalName);
        if (!terminal.first) return nullptr;

        const bool previewSurface = IsPreviewSurfaceNetwork(netInterface);
        if (contextName == HdMaterialSchemaTokens->universalRenderContext &&
            !previewSurface) {
            std::cerr << "[Hydra Material] Unsupported universal material network at "
                      << materialPath << "\n";
            return nullptr;
        }

        if (previewSurface && !RewritePreviewSurfaceNetwork(netInterface))
            return nullptr;
        const std::string resourceFingerprint =
            ResolveTextureAssets(netInterface, cacheDir);
        HdMtlxTexturePrimvarData mtlxData;
        mx::DocumentPtr doc = HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
            &netInterface,
            terminal.second.upstreamNodeName,
            netInterface.GetNodeInputConnectionNames(terminal.second.upstreamNodeName),
            HdMtlxStdLibraries(), &mtlxData);
        if (!doc) {
            std::cerr << "[Hydra Material] MaterialX conversion failed at "
                      << materialPath << "\n";
            return nullptr;
        }
        if (previewSurface) FixPreviewSurfaceNormalOutput(doc);

        // Import MaterialX USD definitions from the same installation used by
        // the native generator. hdMtlx's definitions are also imported for
        // Hydra's texture/primvar node aliases.
        doc->importLibrary(HdMtlxStdLibraries());
        const std::string xml = SerializeDocument(doc);
        const std::string baseKey = HashText(libraries + "\n" + mdlPath + "\n" +
                                              resourceFingerprint + "\n" + xml);
        std::string key = baseKey;
        auto found = gMaterialCache.find(key);
        unsigned collision = 0;
        while (found != gMaterialCache.end() && found->second.xml != xml) {
            key = baseKey + "-" + std::to_string(++collision);
            found = gMaterialCache.find(key);
        }
        if (found != gMaterialCache.end()) return found->second.material;

        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        if (ec) {
            std::cerr << "[Hydra Material] Cannot create material cache '"
                      << cacheDir.string() << "': " << ec.message() << "\n";
            return nullptr;
        }
        const std::filesystem::path mtlxPath = cacheDir / (key + ".mtlx");
        {
            std::ofstream out(mtlxPath, std::ios::binary | std::ios::trunc);
            if (!out || !(out << xml)) {
                std::cerr << "[Hydra Material] Cannot write converted network '"
                          << mtlxPath.string() << "'\n";
                return nullptr;
            }
        }

        std::string materialName;
        const auto mxMaterials = doc->getMaterialNodes();
        if (!mxMaterials.empty()) materialName = mxMaterials.front()->getName();
        auto compiled = std::make_shared<MDLMaterial>(
            mtlxPath.string(), *context, libraries, mdlPath, materialName);
        if (!compiled->is_valid()) {
            std::cerr << "[Hydra Material] MDL compile failed for "
                      << materialPath << "; using its magenta error material\n";
        }
        gMaterialCache[key] = SharedCacheEntry{xml, compiled};
        return compiled;
    }

    mdl::Context* context = nullptr;
    std::string libraries;
    std::string mdlPath;
    std::filesystem::path cacheDir;
    std::mutex mutex;
};

MaterialService::MaterialService(mdl::Context* context,
                                 std::string materialXLibrariesPath,
                                 std::string materialXMdlPath)
    : m_impl(std::make_unique<Impl>(context, std::move(materialXLibrariesPath),
                                    std::move(materialXMdlPath)))
{}

MaterialService::~MaterialService() = default;

std::shared_ptr<Material> MaterialService::Resolve(
    const HdContainerDataSourceHandle& materialPrimDataSource,
    const SdfPath& materialPath)
{
    return m_impl->Resolve(materialPrimDataSource, materialPath);
}

} // namespace prism::hydra
