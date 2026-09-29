#include "materials.h"

#include "../mdl/context.h"
#include "../materialx/usd_network.h"
#include "../materialx/captured_material.h"
#include "../materialx/resolved_resource.h"
#include "../materialx/prepared_template.h"
#include "../mdl_material.h"
#include "../resource_limits.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/base/vt/value.h>
#include <pxr/base/gf/vec3f.h>
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
#include <OpenImageIO/hash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE
namespace mx = MaterialX;

namespace prism::hydra {
namespace {
bool TemplateDebug() {
    const char* value = std::getenv("PRISM_MATERIAL_TEMPLATE_DEBUG");
    return value && *value && std::string_view(value) != "0";
}

std::atomic<uint64_t> gResourcePublicationId{0};

// Protect cache publication and deduplicate material requests. Each compiler
// worker owns its transaction, backend and mutable generator state.
std::mutex gMaterialCacheMutex;
std::condition_variable gMaterialCacheCondition;
std::unordered_set<std::string> gMaterialCompiling;
struct SharedCacheEntry {
    std::string xml;
    std::shared_ptr<Material> material;
    uint64_t bytes = 0;
    uint64_t lastUse = 0;
};
// Target-code/material handles are owned by the process-lifetime MDL runtime.
// Retain this warm cache deliberately: destroying it from DLL/process teardown
// can release SDK interfaces after the runtime has detached under loader lock.
auto& gMaterialCache = *new std::unordered_map<std::string, SharedCacheEntry>();
uint64_t gCacheClock = 0;
uint64_t gCacheEvictions = 0;

void TrimUnusedMaterialsLocked() {
    const uint64_t limit = prism::ResourceLimit("PRISM_MATERIAL_CACHE_MB", 512, 1048576) * 1024 * 1024;
    uint64_t unused = 0;
    std::vector<std::string> candidates;
    for (const auto& [key, entry] : gMaterialCache) {
        if (entry.material.use_count() == 1) {
            unused += entry.bytes;
            candidates.push_back(key);
        }
    }
    if (unused <= limit) return;
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return gMaterialCache.at(a).lastUse < gMaterialCache.at(b).lastUse;
    });
    for (const auto& key : candidates) {
        if (unused <= limit) break;
        const auto entry = gMaterialCache.find(key);
        unused -= entry->second.bytes;
        gMaterialCache.erase(entry);
        ++gCacheEvictions;
    }
}

using NetworkSnapshot = prism::materialx::CapturedMaterialGraph;
using FileDependency = prism::materialx::CapturedFileDependency;

std::string ResourcePathKey(const std::filesystem::path& path)
{
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    std::string key = (error ? path.lexically_normal() : canonical).generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

struct EarlyCacheEntry {
    SdfPath path;
    std::string libraries, mdlPath;
    uint64_t resourceRevision = 0;
    ArResolverContext resolverContext;
    NetworkSnapshot network;
    std::vector<FileDependency> dependencies;
    std::weak_ptr<Material> material;
};
auto& gEarlyMaterialCache = *new std::vector<EarlyCacheEntry>();
constexpr size_t kEarlyMaterialCacheLimit = 2048;
std::atomic<uint64_t> gEarlyCacheReloadGeneration{0};
std::unordered_map<uint64_t, uint64_t> gAssociationReloadGenerations;
std::unordered_map<uint64_t,
    std::unordered_map<SdfPath, uint64_t, SdfPath::Hash>> gAssociationGenerations;
std::unordered_map<uint64_t, uint64_t> gAssociationScopeUsers;
std::atomic<mdl::Context*> gSharedMdlContext{nullptr};

struct ConversionArtifact {
    std::string libraries, mdlPath;
    uint64_t resourceRevision = 0;
    ArResolverContext resolverContext;
    NetworkSnapshot network;
    std::vector<FileDependency> dependencies;
    std::string xml, artifactKey, materialName;
    std::filesystem::path mtlxPath;
};
auto& gConversionArtifacts = *new std::vector<ConversionArtifact>();
constexpr size_t kConversionArtifactLimit = 2048;
using CompatibleTemplate = prism::materialx::CompatibleMaterialTemplate;
using CompatibleBinding = prism::materialx::CompatibleBinding;

bool IsTemplateValue(const prism::materialx::CapturedParameter& parameter)
{
    return prism::materialx::IsCompatibleTemplateValue(parameter);
}

bool TemplateStructure(NetworkSnapshot graph, NetworkSnapshot* structure)
{
    return prism::materialx::CompatibleTemplateStructure(
        std::move(graph), structure);
}

NetworkSnapshot CaptureNetwork(HdMaterialNetworkInterface& net,
                               const TfToken& context,
                               const SdfPath& materialPath)
{
    return prism::materialx::CaptureMaterialGraph(net, context, materialPath);
}

bool CaptureDependencies(const NetworkSnapshot& network,
                         std::vector<FileDependency>* result,
                         const ArResolverContext& resolverContext,
                         uint64_t resourceRevision)
{
    return prism::materialx::CaptureFileDependencies(network, result,
        resolverContext.GetDebugString(), std::to_string(resourceRevision));
}
double CurrentThreadCpuSeconds()
{
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        return static_cast<double>(k.QuadPart + u.QuadPart) * 1.0e-7;
    }
#endif
    return 0.0;
}

uint64_t CurrentProcessIdValue()
{
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::string EnvOr(const char* name, std::string value)
{
    if (value.empty()) {
        if (const char* env = std::getenv(name)) value = env;
    }
    return value;
}

std::string HashText(const std::string& text)
{
    return OIIO::SHA1::digest(text.data(), text.size());
}

bool PublishImmutableText(const std::filesystem::path& cacheDir,
                          const std::string& key, const std::string& text,
                          std::filesystem::path* published)
{
    if (!published) return false;
    std::error_code error;
    std::filesystem::create_directories(cacheDir, error);
    if (error) return false;
    auto candidate = cacheDir / (key + ".mtlx");
    if (std::filesystem::exists(candidate, error) && !error) {
        std::ifstream input(candidate, std::ios::binary);
        const std::string existing((std::istreambuf_iterator<char>(input)), {});
        if (input && existing == text) { *published = candidate; return true; }
        // A corrupted or legacy colliding artifact is never consumed. Keep it
        // for diagnosis and publish this immutable payload under a unique name.
        candidate = cacheDir / (key + "-repair-" +
            std::to_string(CurrentProcessIdValue()) + "-" +
            std::to_string(gResourcePublicationId.fetch_add(1)) + ".mtlx");
    }
    const auto temporary = std::filesystem::path(candidate.string() + "." +
        std::to_string(CurrentProcessIdValue()) + "." +
        std::to_string(gResourcePublicationId.fetch_add(1)) + ".tmp");
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) { std::filesystem::remove(temporary, error); return false; }
    std::filesystem::rename(temporary, candidate, error);
    if (error) {
        std::ifstream input(candidate, std::ios::binary);
        const std::string existing((std::istreambuf_iterator<char>(input)), {});
        std::filesystem::remove(temporary, error);
        if (!input || existing != text) return false;
    }
    *published = candidate;
    return true;
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
                                 const std::filesystem::path& cacheDir,
                                 const ArResolverContext& resolverContext,
                                 const std::string& sourceVersion,
                                 uint64_t* bytesRead)
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

            std::string extension = std::filesystem::path(authored).extension().string();
            if (extension.empty()) extension = ".bin";
            if (authored.find("<UDIM>") != std::string::npos ||
                preResolved.find("<UDIM>") != std::string::npos) {
                try {
                    auto family = prism::materialx::PrepareMaterialUdimResource(
                        authored, preResolved, resolverContext, sourceVersion,
                        cacheDir, extension);
                    if (!family.mdlPath.empty()) {
                        if (bytesRead) *bytesRead += family.bytesRead;
                        for (const auto& tile : family.tiles) {
                            dependencyFingerprint += tile->fingerprint;
                            dependencyFingerprint += std::to_string(tile->sourceBytes);
                        }
                        data.value = family.mdlPath.substr(1);
                        data.typeName = TfToken("asset");
                        net.SetNodeParameterData(node, param, data);
                        continue;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "[Hydra Material] Cannot materialize UDIM texture '"
                              << authored << "': " << error.what() << "\n";
                }
                // Preserve the authored token for the stock SDK path when a
                // custom resolver cannot enumerate an owned tile family.
                continue;
            }

            const ArResolvedPath resolved = !preResolved.empty()
                ? ArResolvedPath(preResolved) : resolver.Resolve(authored);
            if (resolved.empty()) {
                std::cerr << "[Hydra Material] Could not resolve texture '"
                          << authored << "' on node " << node << "\n";
                continue;
            }
            prism::materialx::MaterialResourceResult preparedResource;
            try {
                preparedResource = prism::materialx::PrepareMaterialResource(
                    resolved, resolverContext, sourceVersion, cacheDir, extension);
            } catch (const std::exception& error) {
                std::cerr << "[Hydra Material] Cannot materialize texture '"
                          << resolved.GetPathString() << "': " << error.what() << "\n";
                continue;
            }
            if (!preparedResource.resource) continue;
            if (bytesRead) *bytesRead += preparedResource.bytesRead;
            dependencyFingerprint += preparedResource.resource->fingerprint;
            dependencyFingerprint += std::to_string(
                preparedResource.resource->sourceBytes);
            // Keep resource URIs relative to the generated .mtlx file. The
            // MDL SDK rejects Windows drive prefixes in resource literals;
            // MaterialCompiler registers this document's directory as a
            // resource search path before compilation.
            data.value = preparedResource.resource->path.filename().string();
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


} // namespace

mdl::Context& SharedMdlContext()
{
    // Intentionally process-lifetime: MDL target code and backend objects use
    // interfaces owned by INeuray, including across renderer replacement.
    static mdl::Context* context = [] {
        mdl::Context* result = new mdl::Context();
        gSharedMdlContext.store(result, std::memory_order_release);
        return result;
    }();
    return *context;
}

class MaterialService::Impl {
public:
    struct Prepared {
        HdContainerDataSourceHandle source;
        SdfPath path;
        std::string xml, key, materialName;
        std::filesystem::path mtlxPath;
        std::shared_ptr<Material> cached;
        std::shared_ptr<MDLMaterial> templatePrototype;
        std::vector<CompatibleBinding> templateBindings;
        NetworkSnapshot network;
        // Provenance graph for the converted document. It can differ only in
        // source node names when a canonical graph reused another frontend's
        // conversion artifact.
        NetworkSnapshot conversionNetwork;
        ArResolverContext resolverContext;
        std::vector<FileDependency> dependencies;
        std::unique_ptr<prism::materialx::CapturedMaterialRegistry::ConversionTicket>
            conversionTicket;
        bool earlyCacheEligible = false;
        bool capturedOnly = false;
        uint64_t resourceRevision = 0;
        uint64_t reloadGeneration = 0;
        uint64_t associationReloadGeneration = 0;
        uint64_t associationGeneration = 0;
        bool valid = false;
    };
    void PublishEarlyLocked(const Prepared& prepared, const std::shared_ptr<Material>& material)
    {
        if (!prepared.earlyCacheEligible || !material ||
            prepared.reloadGeneration !=
                gEarlyCacheReloadGeneration.load(std::memory_order_acquire)) return;
        {
            const auto& generations = gAssociationGenerations[associationScope];
            const auto found = generations.find(prepared.path);
            const uint64_t current = found == generations.end()
                ? 0 : found->second;
            if (gAssociationReloadGenerations[associationScope] !=
                    prepared.associationReloadGeneration ||
                current != prepared.associationGeneration) return;
        }
        gEarlyMaterialCache.erase(std::remove_if(
            gEarlyMaterialCache.begin(), gEarlyMaterialCache.end(),
            [&](const EarlyCacheEntry& entry) {
                return entry.libraries == libraries && entry.mdlPath == mdlPath &&
                    entry.resourceRevision == prepared.resourceRevision &&
                    entry.resolverContext == prepared.resolverContext &&
                    entry.network == prepared.network;
            }), gEarlyMaterialCache.end());
        if (gEarlyMaterialCache.size() >= kEarlyMaterialCacheLimit)
            gEarlyMaterialCache.erase(gEarlyMaterialCache.begin());
        gEarlyMaterialCache.push_back({prepared.path, libraries, mdlPath,
            prepared.resourceRevision,
            prepared.resolverContext, prepared.network, prepared.dependencies, material});
    }
    Impl(mdl::Context* context, std::string libraries, std::string mdlPath,
         uint64_t requestedAssociationScope)
        : context(context)
        , associationScope(requestedAssociationScope ? requestedAssociationScope :
              static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(context)))
        , libraries(EnvOr("PRISM_MATERIALX_LIBRARY_PATH", std::move(libraries)))
        , mdlPath(EnvOr("PRISM_MATERIALX_MDL", std::move(mdlPath)))
    {
        if (this->mdlPath.empty() && !this->libraries.empty())
            this->mdlPath = (std::filesystem::path(this->libraries) / "mdl").string();
        if (const char* cache = std::getenv("PRISM_CACHE_DIR"); cache && *cache)
            cacheDir = std::filesystem::path(cache) / "materials";
        else if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
            cacheDir = std::filesystem::path(local) / "PrismRT" / "cache" / "materials";
        else
            cacheDir = std::filesystem::temp_directory_path() / "prism-rt" / "materials";
        std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
        ++gAssociationScopeUsers[associationScope];
    }

    Prepared Prepare(
        const HdContainerDataSourceHandle& primDataSource,
        const SdfPath& materialPath,
        bool allowTemplate = true,
        bool captureOnly = false)
    {
        Prepared result;
        result.source = primDataSource;
        result.path = materialPath;
        ++requests;
        if (!context || !context->is_valid() || libraries.empty() || !primDataSource)
            return result;
        uint64_t currentAssociationReload = 0, currentAssociation = 0;
        {
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            currentAssociationReload = gAssociationReloadGenerations[associationScope];
            currentAssociation = gAssociationGenerations[associationScope][materialPath];
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = pathCache.find(materialPath);
            // Scene-index wrappers need not preserve data-source identity.
            // Hydra dirty notices explicitly invalidate this path; comparing
            // wrapper pointers here defeats the cache after every GetPrim().
            if (found != pathCache.end() && found->second.reloadGeneration ==
                    gEarlyCacheReloadGeneration.load(std::memory_order_acquire) &&
                found->second.associationReloadGeneration == currentAssociationReload &&
                found->second.associationGeneration == currentAssociation) {
                ++pathCacheHits;
                result.cached = found->second.material;
                result.valid = true;
                return result;
            }
            if (found != pathCache.end()) pathCache.erase(found);
        }

        const auto conversionStart = std::chrono::steady_clock::now();

        HdMaterialSchema material = HdMaterialSchema::GetFromParent(primDataSource);
        if (!material) return result;

        TfToken contextName("mtlx");
        HdMaterialNetworkSchema network = material.GetMaterialNetwork(contextName);
        if (!network) {
            contextName = HdMaterialSchemaTokens->universalRenderContext;
            network = material.GetMaterialNetwork(contextName);
        }
        if (!network) return result;

        HdDataSourceMaterialNetworkInterface netInterface(
            materialPath, network.GetContainer(), primDataSource);
        result.network = CaptureNetwork(netInterface, contextName, materialPath);
        result.conversionNetwork = result.network;
        result.resolverContext = ArGetResolver().GetCurrentContext();
        result.resourceRevision = resourceRevision;
        result.reloadGeneration =
            gEarlyCacheReloadGeneration.load(std::memory_order_acquire);
        result.associationReloadGeneration = currentAssociationReload;
        result.associationGeneration = currentAssociation;
        const bool capturedDependencies = CaptureDependencies(
            result.network, &result.dependencies,
            result.resolverContext, result.resourceRevision);
        result.earlyCacheEligible = context ==
                gSharedMdlContext.load(std::memory_order_acquire) && capturedDependencies;
        {
            std::lock_guard lock(mutex);
            auto& previous = pathDependencies[materialPath];
            for (const auto& key : previous) {
                auto found = resourceDependents.find(key);
                if (found == resourceDependents.end()) continue;
                found->second.erase(materialPath);
                if (found->second.empty()) {
                    resourceDependents.erase(found);
                    resourceSourcePaths.erase(key);
                }
            }
            previous.clear();
            for (const auto& dependency : result.dependencies) {
                const auto key = ResourcePathKey(dependency.path);
                previous.insert(key);
                resourceDependents[key].insert(materialPath);
                resourceSourcePaths[key]=dependency.path;
            }
        }
        if (result.earlyCacheEligible) {
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            for (auto it = gEarlyMaterialCache.rbegin(); it != gEarlyMaterialCache.rend(); ++it) {
                auto cachedMaterial = it->material.lock();
                if (cachedMaterial && it->libraries == libraries &&
                    it->mdlPath == mdlPath &&
                    it->resourceRevision == result.resourceRevision &&
                    it->resolverContext == result.resolverContext &&
                    it->network == result.network &&
                    it->dependencies == result.dependencies) {
                    ++earlyCacheHits;
                    std::lock_guard<std::mutex> lock(mutex);
                    pathCache[materialPath] = {primDataSource, cachedMaterial,
                        result.reloadGeneration, result.associationReloadGeneration,
                        result.associationGeneration};
                    result.cached = std::move(cachedMaterial);
                    result.valid = true;
                    return result;
                }
            }
            // Conversion artifacts are published before native compilation,
            // so later requests in the same Prefetch batch can bypass texture
            // materialization and hdMtlx conversion without waiting on a
            // worker that has not started yet.
            for (auto it = gConversionArtifacts.rbegin();
                 it != gConversionArtifacts.rend(); ++it) {
                if (it->libraries != libraries || it->mdlPath != mdlPath ||
                    it->resourceRevision != result.resourceRevision ||
                    it->resolverContext != result.resolverContext ||
                    it->network != result.network ||
                    it->dependencies != result.dependencies) continue;
                result.xml = it->xml;
                result.materialName = it->materialName;
                result.mtlxPath = it->mtlxPath;
                result.conversionNetwork = it->network;
                result.key = std::to_string(
                    reinterpret_cast<std::uintptr_t>(context)) + "-" +
                    it->artifactKey;
                result.valid = true;
                ++earlyCacheHits;
                return result;
            }
            NetworkSnapshot structure;
            if (allowTemplate && TemplateStructure(result.network, &structure)) {
                CompatibleTemplate entry;
                if (prism::materialx::CompatibleTemplateRegistry::Shared().find(
                        context, libraries, mdlPath, structure, &entry)) {
                    result.templatePrototype = entry.prototype;
                    result.templateBindings = entry.bindings;
                    result.valid = true;
                    ++earlyCacheHits;
                    return result;
                }
            }
        }
        if (captureOnly) {
            result.capturedOnly = true;
            return result;
        }
        if (result.earlyCacheEligible) {
            prism::materialx::CapturedConversionArtifact sharedKey;
            sharedKey.libraries = libraries;
            sharedKey.resolverIdentity = result.resolverContext.GetDebugString();
            sharedKey.resolverContext = result.resolverContext;
            sharedKey.sourceVersion = std::to_string(result.resourceRevision);
            sharedKey.graph = result.network;
            sharedKey.dependencies = result.dependencies;
            prism::materialx::CapturedConversionArtifact shared;
            auto useShared = [&](const prism::materialx::CapturedConversionArtifact& artifact) {
                if (artifact.xml.empty()) return false;
                const auto& shared = artifact;
                result.conversionNetwork = artifact.graph;
                result.xml = shared.xml;
                result.materialName = shared.materialName;
                const std::string artifactKey = HashText(
                    "PrismSharedMaterial-v1\n" + libraries + "\n" + result.xml);
                if (PublishImmutableText(cacheDir, artifactKey, result.xml,
                                         &result.mtlxPath)) {
                    result.key = std::to_string(
                        reinterpret_cast<std::uintptr_t>(context)) + "-" + artifactKey;
                    result.valid = true;
                    ++earlyCacheHits;
                    return true;
                }
                return false;
            };
            auto& registry = prism::materialx::CapturedMaterialRegistry::Shared();
            if (registry.find(sharedKey, &shared) && useShared(shared)) return result;
            auto ticket = registry.acquire(sharedKey);
            if (!ticket.isOwner()) {
                if (ticket.wait(&shared) && useShared(shared)) return result;
            } else {
                result.conversionTicket = std::make_unique<
                    prism::materialx::CapturedMaterialRegistry::ConversionTicket>(
                        std::move(ticket));
            }
        }
        const TfToken terminalName = SurfaceTerminalName(netInterface);
        if (terminalName.IsEmpty()) return result;
        const auto terminal = netInterface.GetTerminalConnection(terminalName);
        if (!terminal.first) return result;

        const bool previewSurface = IsPreviewSurfaceNetwork(netInterface);
        if (contextName == HdMaterialSchemaTokens->universalRenderContext &&
            !previewSurface) {
            std::cerr << "[Hydra Material] Unsupported universal material network at "
                      << materialPath << "\n";
            return result;
        }

        if (previewSurface && !RewritePreviewSurfaceNetwork(netInterface))
            return result;
        uint64_t resolvedTextureBytes = 0;
        const auto resourceStart = std::chrono::steady_clock::now();
        const std::string resourceFingerprint =
            ResolveTextureAssets(netInterface, cacheDir, result.resolverContext,
                std::to_string(result.resourceRevision),
                &resolvedTextureBytes);
        resourceNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - resourceStart).count();
        textureBytes += resolvedTextureBytes;
        HdMtlxTexturePrimvarData mtlxData;
        ++conversionCalls;
        mx::DocumentPtr doc = prism::materialx::SimpleOpenPbrDocument(netInterface, terminal.second.upstreamNodeName, HdMtlxStdLibraries());
        const bool simpleDocument = bool(doc);
        if (!doc) {
            prism::materialx::CapturedMaterialRegistry::Shared().recordStockConversion();
            doc = HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
                &netInterface,
                terminal.second.upstreamNodeName,
                netInterface.GetNodeInputConnectionNames(terminal.second.upstreamNodeName),
                HdMtlxStdLibraries(), &mtlxData);
        }
        if (!doc) {
            std::cerr << "[Hydra Material] MaterialX conversion failed at "
                      << materialPath << "\n";
            return result;
        }
        if (previewSurface) FixPreviewSurfaceNormalOutput(doc);

        // hdMtlx derives the terminal material name from the USD prim path.
        // That makes otherwise identical M0/M1 graphs generate different MDL
        // wrapper and temporary names, defeating the SDK's class hash. The
        // terminal material has no downstream references, so canonicalize it
        // before serialization/generation while preserving all graph nodes.
        const auto materialNodes = doc->getMaterialNodes();
        if (!materialNodes.empty()) materialNodes.front()->setName("PrismMaterial");

        // Import MaterialX USD definitions from the same installation used by
        // the native generator. hdMtlx's definitions are also imported for
        // Hydra's texture/primvar node aliases.
        if (!simpleDocument) doc->importLibrary(HdMtlxStdLibraries());
        result.xml = prism::materialx::SerializeDocument(doc, libraries);
        conversionNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - conversionStart).count();
        // Native target-code handles are owned by one MDL runtime. Keep the
        // process cache partitioned when tests or standalone clients create a
        // second Context with otherwise identical material inputs.
        const std::string artifactKey = HashText(
            "PrismHydraMaterial-v2\n" + libraries + "\n" + mdlPath + "\n" +
            resourceFingerprint + "\n" + result.xml);
        const std::string baseKey =
            std::to_string(reinterpret_cast<std::uintptr_t>(context)) + "-" + artifactKey;
        result.key = baseKey;
        {
            std::unique_lock<std::mutex> cacheLock(gMaterialCacheMutex);
            gMaterialCacheCondition.wait(cacheLock, [&] {
                return !gMaterialCompiling.count(result.key);
            });
            auto found = gMaterialCache.find(result.key);
            unsigned collision = 0;
            while (found != gMaterialCache.end() && found->second.xml != result.xml) {
                result.key = baseKey + "-" + std::to_string(++collision);
                found = gMaterialCache.find(result.key);
            }
            if (found != gMaterialCache.end()) {
                ++contentCacheHits;
                found->second.lastUse = ++gCacheClock;
                PublishEarlyLocked(result, found->second.material);
                std::lock_guard<std::mutex> lock(mutex);
                pathCache[materialPath] = {primDataSource, found->second.material,
                    result.reloadGeneration, result.associationReloadGeneration,
                    result.associationGeneration};
                result.cached = found->second.material;
                result.valid = true;
                return result;
            }
        }

        std::error_code ec;
        if (!PublishImmutableText(cacheDir, artifactKey, result.xml,
                                  &result.mtlxPath)) {
            std::cerr << "[Hydra Material] Cannot create material cache '"
                      << cacheDir.string() << "'\n";
            return result;
        }

        const auto mxMaterials = doc->getMaterialNodes();
        if (!mxMaterials.empty()) result.materialName = mxMaterials.front()->getName();
        if (result.earlyCacheEligible) {
            prism::materialx::CapturedConversionArtifact shared;
            shared.libraries = libraries;
            shared.resolverIdentity = result.resolverContext.GetDebugString();
            shared.resolverContext = result.resolverContext;
            shared.sourceVersion = std::to_string(result.resourceRevision);
            shared.graph = result.network;
            shared.dependencies = result.dependencies;
            shared.document = doc;
            shared.xml = result.xml;
            shared.materialName = result.materialName;
            if (result.conversionTicket) {
                result.conversionTicket->publish(std::move(shared));
                result.conversionTicket.reset();
            } else {
                prism::materialx::CapturedMaterialRegistry::Shared().publish(
                    std::move(shared));
            }
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            gConversionArtifacts.erase(std::remove_if(
                gConversionArtifacts.begin(), gConversionArtifacts.end(),
                [&](const ConversionArtifact& entry) {
                    return entry.libraries == libraries && entry.mdlPath == mdlPath &&
                        entry.resourceRevision == result.resourceRevision &&
                        entry.resolverContext == result.resolverContext &&
                        entry.network == result.network &&
                        entry.dependencies == result.dependencies;
                }), gConversionArtifacts.end());
            if (gConversionArtifacts.size() >= kConversionArtifactLimit)
                gConversionArtifacts.erase(gConversionArtifacts.begin());
            gConversionArtifacts.push_back({libraries, mdlPath,
                result.resourceRevision, result.resolverContext, result.network,
                result.dependencies, result.xml, artifactKey,
                result.materialName, result.mtlxPath});
        }
        result.valid = true;
        return result;
    }

    std::shared_ptr<Material> Compile(Prepared const& prepared,
                                      bool* needsFallback = nullptr)
    {
        if (!prepared.valid) return nullptr;
        if (prepared.cached) {
            std::lock_guard<std::mutex> lock(mutex);
            pathCache[prepared.path] = {prepared.source, prepared.cached,
                prepared.reloadGeneration, prepared.associationReloadGeneration,
                prepared.associationGeneration};
            return prepared.cached;
        }
        if (prepared.templatePrototype) {
            CompatibleTemplate templateValue{context, libraries, mdlPath, {},
                prepared.templatePrototype, prepared.templateBindings};
            prism::materialx::CompatibleBindResult bound;
            try {
                bound = prism::materialx::BindCompatibleTemplate(
                    templateValue, prepared.network, *context,
                    prepared.resolverContext,
                    std::to_string(prepared.resourceRevision), cacheDir);
            } catch (const std::exception& error) {
                bound.error = error.what();
            }
            textureBytes += bound.bytesRead;
            if (bound.material) {
                    ++templateCacheHits;
                    ++bindingCreations;
                    auto material = std::move(bound.material);
                    {
                        std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
                        PublishEarlyLocked(prepared, material);
                    }
                    const auto& published = material->get_target_code();
                    if (published.fallbackTranslations ||
                        !published.textureFallbackReason.empty())
                        ++sdkTextureFallbacks;
                    else if (published.binding && published.binding->texture_runtime)
                        ++customTextureTargets;
                    std::lock_guard<std::mutex> lock(mutex);
                    pathCache[prepared.path] = {prepared.source, material,
                        prepared.reloadGeneration,
                        prepared.associationReloadGeneration,
                        prepared.associationGeneration};
                    return material;
            }
            if (TemplateDebug() && !bound.error.empty())
                std::cerr << "[MaterialTemplate] binding rejected "
                          << prepared.path << ": " << bound.error << "\n";
            if (needsFallback) *needsFallback = true;
            return nullptr;
        }
        {
            std::unique_lock<std::mutex> cacheLock(gMaterialCacheMutex);
            gMaterialCacheCondition.wait(cacheLock, [&] {
                return !gMaterialCompiling.count(prepared.key);
            });
            auto found = gMaterialCache.find(prepared.key);
            if (found != gMaterialCache.end() && found->second.xml == prepared.xml) {
                ++contentCacheHits;
                found->second.lastUse = ++gCacheClock;
                PublishEarlyLocked(prepared, found->second.material);
                std::lock_guard<std::mutex> lock(mutex);
                pathCache[prepared.path] = {prepared.source, found->second.material,
                    prepared.reloadGeneration, prepared.associationReloadGeneration,
                    prepared.associationGeneration};
                return found->second.material;
            }
            gMaterialCompiling.insert(prepared.key);
        }
        const auto compilationStart = std::chrono::steady_clock::now();
        MDLMaterial::CompilationTimings timings;
        std::shared_ptr<MDLMaterial> candidate;
        try {
            candidate = std::make_shared<MDLMaterial>(
                prepared.mtlxPath.string(), *context, libraries, mdlPath,
                prepared.materialName, &timings);
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
                gMaterialCompiling.erase(prepared.key);
            }
            gMaterialCacheCondition.notify_all();
            ++failures;
            std::cerr << "[Hydra Material] MDL compile threw for "
                      << prepared.path << ": " << error.what() << "\n";
            return nullptr;
        } catch (...) {
            {
                std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
                gMaterialCompiling.erase(prepared.key);
            }
            gMaterialCacheCondition.notify_all();
            ++failures;
            std::cerr << "[Hydra Material] MDL compile threw for "
                      << prepared.path << "\n";
            return nullptr;
        }
        compilationNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - compilationStart).count();
        ++compilations;
        if (timings.persistentGenerationCacheHit) ++persistentGenerationCacheHits;
        generationCalls += timings.generationCalls;
        if (timings.templateCacheHit) ++templateCacheHits;
        generationNanoseconds += static_cast<int64_t>(timings.generationSeconds * 1.0e9);
        classCompilationNanoseconds += static_cast<int64_t>(
            timings.classCompilationSeconds * 1.0e9);
        nativeJitNanoseconds += static_cast<int64_t>(timings.nativeJitSeconds * 1.0e9);
        const auto& target = candidate->get_target_code();
        if (!target.cacheHit) ++nativeJits;
        else ++programCacheHits;
        sdkTranslationCalls += target.translationCalls;
        fallbackTranslations += target.fallbackTranslations;
        programWaitNanoseconds += static_cast<int64_t>(target.programWaitSeconds * 1.0e9);
        if (target.binding) ++bindingCreations;
        if(candidate->is_valid()) {
            if(target.binding && target.binding->texture_runtime) ++customTextureTargets;
            else if (target.fallbackTranslations ||
                     !target.textureFallbackReason.empty()) ++sdkTextureFallbacks;
        }
        if (candidate->is_valid() && prepared.earlyCacheEligible) {
            NetworkSnapshot structure;
            std::vector<CompatibleBinding> bindings;
            bool compatible = TemplateStructure(prepared.network, &structure);
            if (compatible) compatible = prism::materialx::BuildCompatibleBindings(
                prepared.conversionNetwork, candidate->get_generated_uniforms(), &bindings);
            if (compatible && !bindings.empty()) {
                prism::materialx::CompatibleTemplateRegistry::Shared().publish(
                    {context, libraries, mdlPath, std::move(structure),
                     candidate, std::move(bindings)});
                if (TemplateDebug()) std::cerr << "[MaterialTemplate] published "
                    << prepared.path << " inputs="
                    << candidate->get_generated_uniforms().size() << "\n";
            } else if (TemplateDebug()) {
                std::cerr << "[MaterialTemplate] rejected " << prepared.path
                          << " uniforms=" << candidate->get_generated_uniforms().size()
                          << " bindings=" << bindings.size() << "\n";
            }
        }
        if (!candidate->is_valid()) {
            ++failures;
            std::cerr << "[Hydra Material] MDL compile failed for "
                      << prepared.path << "; using its magenta error material\n";
        }
        std::shared_ptr<Material> compiled = candidate;
        {
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            auto [it, inserted] = gMaterialCache.emplace(
                prepared.key, SharedCacheEntry{prepared.xml, candidate,
                    uint64_t(prepared.xml.size() + sizeof(MDLMaterial)) +
                        (target.program && target.program->target_code
                            ? target.program->target_code->get_code_size() : 0) +
                        (target.program ? target.program->ro_data_segment_size : 0) +
                        (target.binding && target.binding->argument_block
                            ? target.binding->argument_block->get_size() : 0),
                    ++gCacheClock});
            if (!inserted && it->second.xml == prepared.xml) compiled = it->second.material;
            gMaterialCompiling.erase(prepared.key);
        }
        gMaterialCacheCondition.notify_all();
        {
            std::lock_guard<std::mutex> lock(mutex);
            pathCache[prepared.path] = {prepared.source, compiled,
                prepared.reloadGeneration, prepared.associationReloadGeneration,
                prepared.associationGeneration};
        }
        if (prepared.earlyCacheEligible) {
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            PublishEarlyLocked(prepared, compiled);
        }
        return compiled;
    }

    std::shared_ptr<Material> Resolve(
        const HdContainerDataSourceHandle& source, const SdfPath& path)
    {
        auto prepared = Prepare(source, path);
        auto material = Compile(prepared);
        if (!material && prepared.templatePrototype)
            material = Compile(Prepare(source, path, false));
        return material;
    }

    mdl::Context* context = nullptr;
    std::string libraries;
    std::string mdlPath;
    std::filesystem::path cacheDir;
    uint64_t resourceRevision = 0;
    uint64_t associationScope = 0;
    std::mutex mutex;
    struct PathEntry {
        HdContainerDataSourceHandle source;
        std::shared_ptr<Material> material;
        uint64_t reloadGeneration = 0;
        uint64_t associationReloadGeneration = 0;
        uint64_t associationGeneration = 0;
    };
    std::unordered_map<SdfPath, PathEntry, SdfPath::Hash> pathCache;
    std::unordered_map<SdfPath, std::unordered_set<std::string>, SdfPath::Hash> pathDependencies;
    std::unordered_map<std::string, std::unordered_set<SdfPath, SdfPath::Hash>> resourceDependents;
    std::unordered_map<std::string,std::filesystem::path> resourceSourcePaths;
    std::atomic<uint64_t> requests{0}, pathCacheHits{0}, contentCacheHits{0}, earlyCacheHits{0};
    std::atomic<uint64_t> conversionCalls{0};
    std::atomic<uint64_t> compilations{0}, nativeJits{0}, jobs{0};
    std::atomic<uint64_t> sdkTranslationCalls{0}, programCacheHits{0};
    std::atomic<uint64_t> bindingCreations{0}, fallbackTranslations{0};
    std::atomic<uint64_t> sdkTextureFallbacks{0}, customTextureTargets{0};
    std::atomic<uint64_t> failures{0}, textureBytes{0};
    std::atomic<int64_t> conversionNanoseconds{0}, compilationNanoseconds{0};
    std::atomic<int64_t> generationNanoseconds{0}, classCompilationNanoseconds{0};
    std::atomic<uint64_t> persistentGenerationCacheHits{0};
    std::atomic<uint64_t> generationCalls{0}, templateCacheHits{0};
    std::atomic<int64_t> nativeJitNanoseconds{0};
    std::atomic<int64_t> programWaitNanoseconds{0};
    std::atomic<int64_t> resourceNanoseconds{0}, batchWallNanoseconds{0};
    std::atomic<int64_t> workerCpuNanoseconds{0};
};

MaterialService::MaterialService(mdl::Context* context,
                                 std::string materialXLibrariesPath,
                                 std::string materialXMdlPath,
                                 uint64_t associationScope)
    : m_impl(std::make_unique<Impl>(context, std::move(materialXLibrariesPath),
                                    std::move(materialXMdlPath), associationScope))
{}

MaterialService::~MaterialService() {
    // Release per-path references before deciding which warm records are unused.
    const auto context = m_impl->context;
    const uint64_t associationScope = m_impl->associationScope;
    m_impl.reset();
    std::lock_guard lock(gMaterialCacheMutex);
    // Only the explicitly process-lived runtime may retain SDK handles after
    // its service dies. Scoped test/client contexts must not leave warm entries
    // behind for a subsequent Context allocated at the same address.
    if (context != gSharedMdlContext.load(std::memory_order_acquire)) {
        const auto prefix = std::to_string(reinterpret_cast<std::uintptr_t>(context)) + "-";
        for (auto it = gMaterialCache.begin(); it != gMaterialCache.end();) {
            if (it->first.starts_with(prefix)) it = gMaterialCache.erase(it);
            else ++it;
        }
        prism::materialx::CompatibleTemplateRegistry::Shared().eraseContext(context);
    }
    TrimUnusedMaterialsLocked();
    std::erase_if(gEarlyMaterialCache, [](const auto& entry) { return entry.material.expired(); });
    auto users = gAssociationScopeUsers.find(associationScope);
    if (users != gAssociationScopeUsers.end() && --users->second == 0) {
        gAssociationScopeUsers.erase(users);
        gAssociationReloadGenerations.erase(associationScope);
        gAssociationGenerations.erase(associationScope);
    }
}

std::shared_ptr<Material> MaterialService::Resolve(
    const HdContainerDataSourceHandle& materialPrimDataSource,
    const SdfPath& materialPath)
{
    return m_impl->Resolve(materialPrimDataSource, materialPath);
}

void MaterialService::Prefetch(const std::vector<Request>& requests,
                               const std::function<bool()>& cancelled)
{
    if (requests.empty()) return;
    // Finish shared MDL SDK configuration before worker transactions begin.
    // Generated texture references are materialized as absolute paths during
    // Prepare, so workers do not need to mutate resource search paths.
    if (m_impl->context) {
        if (!m_impl->mdlPath.empty()) {
            m_impl->context->add_mdl_path(m_impl->mdlPath);
            m_impl->context->add_resource_path(m_impl->mdlPath);
        }
        m_impl->context->add_resource_path(m_impl->cacheDir.string());
    }
    const auto batchStart = std::chrono::steady_clock::now();
    std::vector<Request> unique;
    unique.reserve(requests.size());
    std::unordered_set<SdfPath, SdfPath::Hash> seen;
    for (const Request& request : requests)
        if (!request.path.IsEmpty() && seen.insert(request.path).second)
            unique.push_back(request);
    if (unique.empty()) return;
    m_impl->jobs += unique.size();
    // Capture immutable identities first. Repeated compatible structures need
    // one seed compiled before their variants are prepared, otherwise the old
    // prepare-all ordering would convert every variant before a template could
    // be published.
    std::vector<Impl::Prepared> captured;
    captured.reserve(unique.size());
    for (const Request& request : unique) {
        if (cancelled && cancelled()) return;
        captured.push_back(m_impl->Prepare(
            request.dataSource, request.path, true, true));
    }
    struct Group { NetworkSnapshot structure; std::vector<size_t> indices; };
    std::vector<Group> groups;
    std::vector<bool> grouped(unique.size(), false);
    for (size_t i = 0; i < captured.size(); ++i) {
        if (!captured[i].capturedOnly) continue;
        NetworkSnapshot structure;
        if (!TemplateStructure(captured[i].network, &structure)) continue;
        auto found = std::find_if(groups.begin(), groups.end(),
            [&](const Group& group) { return group.structure == structure; });
        if (found == groups.end()) {
            groups.push_back({std::move(structure), {i}});
        } else found->indices.push_back(i);
    }
    if (TemplateDebug()) {
        std::cerr << "[MaterialTemplate] captured=" << captured.size()
                  << " groups=" << groups.size();
        for (const auto& group : groups)
            std::cerr << " " << group.indices.size();
        std::cerr << "\n";
    }

    // Hydra data sources are consumed on this calling sync thread. Workers see
    // only serialized MaterialX, bound templates and materialized resources.
    std::vector<Impl::Prepared> prepared;
    prepared.reserve(unique.size());
    std::vector<Impl::Prepared> seeds;
    std::vector<size_t> seedGroupIndices;
    std::vector<std::shared_ptr<Material>> groupSeedMaterials(groups.size());
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Group& group = groups[groupIndex];
        if (group.indices.size() < 2) continue;
        if (cancelled && cancelled()) return;
        const size_t seedIndex = group.indices.front();
        seeds.push_back(m_impl->Prepare(unique[seedIndex].dataSource,
                                       unique[seedIndex].path));
        seedGroupIndices.push_back(groupIndex);
        grouped[seedIndex] = true;
        for (size_t j = 1; j < group.indices.size(); ++j)
            grouped[group.indices[j]] = true;
    }
    std::vector<std::shared_ptr<Material>> seedMaterials(seeds.size());
    if (!seeds.empty()) {
        const unsigned seedWorkers = std::min<unsigned>(
            prism::MaterialWorkerCount(), static_cast<unsigned>(seeds.size()));
        std::atomic<size_t> seedNext{0};
        std::vector<std::thread> seedPool;
        seedPool.reserve(seedWorkers);
        for (unsigned i = 0; i < seedWorkers; ++i) {
            seedPool.emplace_back([&] {
                const double cpuStart = CurrentThreadCpuSeconds();
                for (;;) {
                    if (cancelled && cancelled()) break;
                    const size_t index = seedNext.fetch_add(1);
                    if (index >= seeds.size()) break;
                    seedMaterials[index] = m_impl->Compile(seeds[index]);
                }
                m_impl->workerCpuNanoseconds += static_cast<int64_t>(
                    (CurrentThreadCpuSeconds() - cpuStart) * 1.0e9);
            });
        }
        for (auto& worker : seedPool) worker.join();
        for (size_t i = 0; i < seeds.size(); ++i) {
            if (cancelled && cancelled()) return;
            if (seedMaterials[i] || !seeds[i].templatePrototype) continue;
            auto fallback = m_impl->Prepare(
                seeds[i].source, seeds[i].path, false);
            seedMaterials[i] = m_impl->Compile(fallback);
        }
    }
    for (size_t i = 0; i < seeds.size(); ++i) {
        groupSeedMaterials[seedGroupIndices[i]] = seedMaterials[i];
        seeds[i].cached = std::move(seedMaterials[i]);
        prepared.push_back(std::move(seeds[i]));
    }
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Group& group = groups[groupIndex];
        if (group.indices.size() < 2) continue;
        for (size_t j = 1; j < group.indices.size(); ++j) {
            const size_t index = group.indices[j];
            // The capture already owns every value needed by argument
            // binding. Attach the seed published above directly so variants
            // never enter conversion/generation merely to rediscover it.
            NetworkSnapshot structure;
            CompatibleTemplate entry;
            auto variant = std::move(captured[index]);
            const auto& seedCapture = captured[group.indices.front()];
            if (groupSeedMaterials[groupIndex] &&
                variant.resolverContext == seedCapture.resolverContext &&
                variant.resourceRevision == seedCapture.resourceRevision &&
                variant.network == seedCapture.network &&
                variant.dependencies == seedCapture.dependencies) {
                variant.cached = groupSeedMaterials[groupIndex];
                variant.valid = true;
                ++m_impl->earlyCacheHits;
                prepared.push_back(std::move(variant));
            } else if (TemplateStructure(variant.network, &structure) &&
                prism::materialx::CompatibleTemplateRegistry::Shared().find(
                    m_impl->context, m_impl->libraries, m_impl->mdlPath,
                    structure, &entry)) {
                variant.templatePrototype = std::move(entry.prototype);
                variant.templateBindings = std::move(entry.bindings);
                variant.valid = true;
                ++m_impl->earlyCacheHits;
                prepared.push_back(std::move(variant));
            } else {
                prepared.push_back(m_impl->Prepare(
                    unique[index].dataSource, unique[index].path));
            }
        }
    }
    for (size_t i = 0; i < unique.size(); ++i) {
        if (grouped[i]) continue;
        if (captured[i].cached) prepared.push_back(std::move(captured[i]));
        else prepared.push_back(m_impl->Prepare(unique[i].dataSource, unique[i].path));
    }
    unsigned workers = prism::MaterialWorkerCount();
    workers = std::min<unsigned>(workers, static_cast<unsigned>(prepared.size()));
    std::atomic<size_t> next{0};
    std::vector<uint8_t> needsFallback(prepared.size(), 0);
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        pool.emplace_back([&] {
            const double cpuStart = CurrentThreadCpuSeconds();
            for (;;) {
                if (cancelled && cancelled()) break;
                const size_t index = next.fetch_add(1);
                if (index >= prepared.size()) break;
                bool fallback = false;
                m_impl->Compile(prepared[index], &fallback);
                needsFallback[index] = fallback ? 1 : 0;
            }
            m_impl->workerCpuNanoseconds += static_cast<int64_t>(
                (CurrentThreadCpuSeconds() - cpuStart) * 1.0e9);
        });
    }
    for (std::thread& worker : pool) worker.join();
    // Rare mapping/resource incompatibilities return to the Prefetch caller,
    // which owns the captured data-source lifetime. Workers never recapture or
    // traverse a caller-provided source.
    for (size_t i = 0; i < prepared.size(); ++i) {
        if (cancelled && cancelled()) break;
        if (!needsFallback[i]) continue;
        auto fallback = m_impl->Prepare(
            prepared[i].source, prepared[i].path, false);
        m_impl->Compile(fallback);
    }
    m_impl->batchWallNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - batchStart).count();
}

void MaterialService::SetResourceRevision(uint64_t revision)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->resourceRevision == revision) return;
    m_impl->resourceRevision = revision;
    m_impl->pathCache.clear();
}

void MaterialService::Invalidate(const SdfPath& path, bool subtree)
{
    InvalidateResources(path, subtree);
}

void MaterialService::InvalidateAssociations(const SdfPath& path, bool subtree)
{
    {
        std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
        if (path.IsEmpty()) {
            ++gAssociationReloadGenerations[m_impl->associationScope];
            gAssociationGenerations[m_impl->associationScope].clear();
        } else if (subtree) {
            // Capture registers every material path before releasing the
            // owner thread, so this also cancels matching in-flight work.
            for (auto& [cachedPath, generation] :
                 gAssociationGenerations[m_impl->associationScope])
                if (cachedPath.HasPrefix(path)) ++generation;
        } else {
            ++gAssociationGenerations[m_impl->associationScope][path];
        }
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (path.IsEmpty()) {
        m_impl->pathCache.clear();
        return;
    }
    for (auto it = m_impl->pathCache.begin(); it != m_impl->pathCache.end();) {
        const bool match = subtree ? it->first.HasPrefix(path) : it->first == path;
        if (match) it = m_impl->pathCache.erase(it);
        else ++it;
    }
}

void MaterialService::InvalidateResources(const SdfPath& path, bool subtree)
{
    InvalidateAssociations(path, subtree);
    // Exact entries are immutable content records. A recaptured graph and its
    // resource stamps must match before reuse, so a dirty association does not
    // destroy useful content shared by another path.
}

std::vector<SdfPath> MaterialService::ReloadResource(const std::string& resolvedPath)
{
    const auto key = ResourcePathKey(resolvedPath);
    std::vector<SdfPath> affected;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->resourceDependents.find(key);
        if (found != m_impl->resourceDependents.end())
            affected.assign(found->second.begin(), found->second.end());
    }
    // Advance source identity before the next capture. Old snapshots retain
    // their immutable prepared resources and argument blocks.
    prism::TextureService::Acquire()->invalidate(resolvedPath);
    std::sort(affected.begin(), affected.end());
    for (const auto& path : affected) InvalidateAssociations(path);
    return affected;
}

void MaterialService::InvalidateAll()
{
    std::vector<std::filesystem::path> resources;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->pathCache.clear();
        resources.reserve(m_impl->resourceDependents.size());
        for (const auto& [resource, paths] : m_impl->resourceDependents) {
            const auto actual=m_impl->resourceSourcePaths.find(resource);
            resources.push_back(actual==m_impl->resourceSourcePaths.end()
                ? std::filesystem::path(resource) : actual->second);
        }
    }
    const auto textures = prism::TextureService::Acquire();
    for (const auto& resource : resources) {
        try { textures->invalidate(resource); }
        catch (const std::exception& error) {
            std::cerr << "Material resource invalidation failed for '" << resource
                      << "': " << error.what() << '\n';
        }
    }
    std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
    gEarlyCacheReloadGeneration.fetch_add(1, std::memory_order_acq_rel);
    ++gAssociationReloadGenerations[m_impl->associationScope];
    gAssociationGenerations[m_impl->associationScope].clear();
    gEarlyMaterialCache.clear();
    gConversionArtifacts.clear();
    prism::materialx::CompatibleTemplateRegistry::Shared().clear();
    prism::materialx::CapturedMaterialRegistry::Shared().clear();
}

MaterialService::MaterialStats MaterialService::GetStats() const
{
    MaterialStats result;
    {
        std::lock_guard lock(gMaterialCacheMutex);
        result.cacheEvictions = gCacheEvictions;
        for (const auto& [key, entry] : gMaterialCache) {
            result.cacheAccountedBytes += entry.bytes;
            if (entry.material.use_count() == 1) result.unusedCacheBytes += entry.bytes;
        }
    }
    result.preparationPeakReservedBytes = prism::PreparationAdmission::Shared().peakReservedBytes();
    result.requests = m_impl->requests.load();
    result.pathCacheHits = m_impl->pathCacheHits.load();
    result.contentCacheHits = m_impl->contentCacheHits.load();
    result.earlyCacheHits = m_impl->earlyCacheHits.load();
    result.conversionCalls = m_impl->conversionCalls.load();
    result.compilations = m_impl->compilations.load();
    result.nativeJits = m_impl->nativeJits.load();
    result.sdkTranslationCalls = m_impl->sdkTranslationCalls.load();
    result.programCacheHits = m_impl->programCacheHits.load();
    result.bindingCreations = m_impl->bindingCreations.load();
    result.fallbackTranslations = m_impl->fallbackTranslations.load();
    result.persistentGenerationCacheHits =
        m_impl->persistentGenerationCacheHits.load();
    result.generationCalls = m_impl->generationCalls.load();
    result.templateCacheHits = m_impl->templateCacheHits.load();
    result.sdkTextureFallbacks = m_impl->sdkTextureFallbacks.load();
    result.customTextureTargets = m_impl->customTextureTargets.load();
    result.jobs = m_impl->jobs.load();
    result.failures = m_impl->failures.load();
    result.textureBytes = m_impl->textureBytes.load();
    result.conversionSeconds = m_impl->conversionNanoseconds.load() / 1.0e9;
    result.compilationSeconds = m_impl->compilationNanoseconds.load() / 1.0e9;
    result.generationSeconds = m_impl->generationNanoseconds.load() / 1.0e9;
    result.classCompilationSeconds =
        m_impl->classCompilationNanoseconds.load() / 1.0e9;
    result.nativeJitSeconds = m_impl->nativeJitNanoseconds.load() / 1.0e9;
    result.programWaitSeconds = m_impl->programWaitNanoseconds.load() / 1.0e9;
    result.resourceReadSeconds = m_impl->resourceNanoseconds.load() / 1.0e9;
    result.batchWallSeconds = m_impl->batchWallNanoseconds.load() / 1.0e9;
    result.workerCpuSeconds = m_impl->workerCpuNanoseconds.load() / 1.0e9;
    return result;
}

} // namespace prism::hydra
