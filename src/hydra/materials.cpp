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
#include <mutex>
#include <thread>
#include <unordered_map>
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

std::atomic<uint64_t> gResourcePublicationId{0};

// The MDL SDK has process-wide runtime state; separate renderer instances must
// not enter module loading/backend compilation concurrently.
std::mutex gMaterialCacheMutex;
std::condition_variable gMaterialCacheCondition;
std::unordered_set<std::string> gMaterialCompiling;
struct SharedCacheEntry {
    std::string xml;
    std::shared_ptr<Material> material;
};
// Target-code/material handles are owned by the process-lifetime MDL runtime.
// Retain this warm cache deliberately: destroying it from DLL/process teardown
// can release SDK interfaces after the runtime has detached under loader lock.
auto& gMaterialCache = *new std::unordered_map<std::string, SharedCacheEntry>();

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
                                 const std::filesystem::path& cacheDir,
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
            if (bytesRead) *bytesRead += static_cast<uint64_t>(size);
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
                const std::filesystem::path temporary = materialized.string() + "." +
                    std::to_string(gResourcePublicationId.fetch_add(1)) + ".tmp";
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
                    output.close();
                    std::filesystem::remove(temporary, ec);
                    continue;
                }
                output.close();
                std::filesystem::rename(temporary, materialized, ec);
                if (ec) {
                    // Another service may have published the same content first.
                    std::error_code existsError;
                    const bool published = std::filesystem::exists(materialized, existsError);
                    std::filesystem::remove(temporary, existsError);
                    if (!published) continue;
                    ec.clear();
                }
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

std::string SerializeDocument(const mx::DocumentPtr& doc,
                              const std::string& installedLibraries)
{
    mx::XmlWriteOptions options;
    std::string root = std::filesystem::path(installedLibraries).generic_string();
#ifdef _WIN32
    std::transform(root.begin(), root.end(), root.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    if (!root.empty() && root.back() != '/') root.push_back('/');
    options.elementPredicate = [root](mx::ConstElementPtr element) {
        if (root.empty()) return true;
        std::string source = std::filesystem::path(
            element->getSourceUri()).generic_string();
#ifdef _WIN32
        std::transform(source.begin(), source.end(), source.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        // Worker-local generators already own this immutable standard library.
        // Keep user and hdMtlx includes whose source URIs are outside it.
        return source.empty() || source.rfind(root, 0) != 0;
    };
    return mx::writeToXmlString(doc, &options);
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
    struct Prepared {
        HdContainerDataSourceHandle source;
        SdfPath path;
        std::string xml, key, materialName;
        std::filesystem::path mtlxPath;
        std::shared_ptr<Material> cached;
        bool valid = false;
    };
    Impl(mdl::Context* context, std::string libraries, std::string mdlPath)
        : context(context)
        , libraries(EnvOr("PRISM_MATERIALX_LIBRARY_PATH", std::move(libraries)))
        , mdlPath(EnvOr("PRISM_MATERIALX_MDL", std::move(mdlPath)))
    {
        if (this->mdlPath.empty() && !this->libraries.empty())
            this->mdlPath = (std::filesystem::path(this->libraries) / "mdl").string();
        cacheDir = std::filesystem::temp_directory_path() / "prism-rt" /
            "hydra-materials" / std::to_string(CurrentProcessIdValue());
    }

    Prepared Prepare(
        const HdContainerDataSourceHandle& primDataSource,
        const SdfPath& materialPath)
    {
        Prepared result;
        result.source = primDataSource;
        result.path = materialPath;
        ++requests;
        if (!context || !context->is_valid() || libraries.empty() || !primDataSource)
            return result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = pathCache.find(materialPath);
            // Scene-index wrappers need not preserve data-source identity.
            // Hydra dirty notices explicitly invalidate this path; comparing
            // wrapper pointers here defeats the cache after every GetPrim().
            if (found != pathCache.end()) {
                ++pathCacheHits;
                result.cached = found->second.material;
                result.valid = true;
                return result;
            }
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
            ResolveTextureAssets(netInterface, cacheDir, &resolvedTextureBytes);
        resourceNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - resourceStart).count();
        textureBytes += resolvedTextureBytes;
        HdMtlxTexturePrimvarData mtlxData;
        mx::DocumentPtr doc = HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
            &netInterface,
            terminal.second.upstreamNodeName,
            netInterface.GetNodeInputConnectionNames(terminal.second.upstreamNodeName),
            HdMtlxStdLibraries(), &mtlxData);
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
        doc->importLibrary(HdMtlxStdLibraries());
        result.xml = SerializeDocument(doc, libraries);
        conversionNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - conversionStart).count();
        // Native target-code handles are owned by one MDL runtime. Keep the
        // process cache partitioned when tests or standalone clients create a
        // second Context with otherwise identical material inputs.
        const std::string baseKey = HashText(
            std::to_string(reinterpret_cast<std::uintptr_t>(context)) + "\n" +
            libraries + "\n" + mdlPath + "\n" + resourceFingerprint + "\n" +
            result.xml);
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
                std::lock_guard<std::mutex> lock(mutex);
                pathCache[materialPath] = {primDataSource, found->second.material};
                result.cached = found->second.material;
                result.valid = true;
                return result;
            }
        }

        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        if (ec) {
            std::cerr << "[Hydra Material] Cannot create material cache '"
                      << cacheDir.string() << "': " << ec.message() << "\n";
            return result;
        }
        result.mtlxPath = cacheDir / (result.key + ".mtlx");
        {
            // Content-addressed documents are immutable. Publish through a
            // same-directory rename and never truncate a file a worker may
            // already be reading.
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            if (!std::filesystem::exists(result.mtlxPath, ec)) {
                const std::filesystem::path temporary = result.mtlxPath.string() + "." +
                    std::to_string(gResourcePublicationId.fetch_add(1)) + ".tmp";
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out || !(out << result.xml)) {
                    out.close();
                    std::filesystem::remove(temporary, ec);
                    std::cerr << "[Hydra Material] Cannot write converted network '"
                              << result.mtlxPath.string() << "'\n";
                    return result;
                }
                out.close();
                std::filesystem::rename(temporary, result.mtlxPath, ec);
                if (ec) {
                    std::error_code existsError;
                    const bool published = std::filesystem::exists(result.mtlxPath, existsError);
                    std::filesystem::remove(temporary, existsError);
                    if (!published) {
                        std::cerr << "[Hydra Material] Cannot publish converted network '"
                                  << result.mtlxPath.string() << "': " << ec.message() << "\n";
                        return result;
                    }
                    ec.clear();
                }
            }
        }

        const auto mxMaterials = doc->getMaterialNodes();
        if (!mxMaterials.empty()) result.materialName = mxMaterials.front()->getName();
        result.valid = true;
        return result;
    }

    std::shared_ptr<Material> Compile(Prepared const& prepared)
    {
        if (!prepared.valid) return nullptr;
        if (prepared.cached) return prepared.cached;
        {
            std::unique_lock<std::mutex> cacheLock(gMaterialCacheMutex);
            gMaterialCacheCondition.wait(cacheLock, [&] {
                return !gMaterialCompiling.count(prepared.key);
            });
            auto found = gMaterialCache.find(prepared.key);
            if (found != gMaterialCache.end() && found->second.xml == prepared.xml) {
                ++contentCacheHits;
                std::lock_guard<std::mutex> lock(mutex);
                pathCache[prepared.path] = {prepared.source, found->second.material};
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
        generationNanoseconds += static_cast<int64_t>(timings.generationSeconds * 1.0e9);
        classCompilationNanoseconds += static_cast<int64_t>(
            timings.classCompilationSeconds * 1.0e9);
        nativeJitNanoseconds += static_cast<int64_t>(timings.nativeJitSeconds * 1.0e9);
        if (!candidate->get_target_code().cacheHit) ++nativeJits;
        if (!candidate->is_valid()) {
            ++failures;
            std::cerr << "[Hydra Material] MDL compile failed for "
                      << prepared.path << "; using its magenta error material\n";
        }
        std::shared_ptr<Material> compiled = candidate;
        {
            std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
            auto [it, inserted] = gMaterialCache.emplace(
                prepared.key, SharedCacheEntry{prepared.xml, candidate});
            if (!inserted && it->second.xml == prepared.xml) compiled = it->second.material;
            gMaterialCompiling.erase(prepared.key);
        }
        gMaterialCacheCondition.notify_all();
        {
            std::lock_guard<std::mutex> lock(mutex);
            pathCache[prepared.path] = {prepared.source, compiled};
        }
        return compiled;
    }

    std::shared_ptr<Material> Resolve(
        const HdContainerDataSourceHandle& source, const SdfPath& path)
    { return Compile(Prepare(source, path)); }

    mdl::Context* context = nullptr;
    std::string libraries;
    std::string mdlPath;
    std::filesystem::path cacheDir;
    std::mutex mutex;
    struct PathEntry {
        HdContainerDataSourceHandle source;
        std::shared_ptr<Material> material;
    };
    std::unordered_map<SdfPath, PathEntry, SdfPath::Hash> pathCache;
    std::atomic<uint64_t> requests{0}, pathCacheHits{0}, contentCacheHits{0};
    std::atomic<uint64_t> compilations{0}, nativeJits{0}, jobs{0};
    std::atomic<uint64_t> failures{0}, textureBytes{0};
    std::atomic<int64_t> conversionNanoseconds{0}, compilationNanoseconds{0};
    std::atomic<int64_t> generationNanoseconds{0}, classCompilationNanoseconds{0};
    std::atomic<int64_t> nativeJitNanoseconds{0};
    std::atomic<int64_t> resourceNanoseconds{0}, batchWallNanoseconds{0};
    std::atomic<int64_t> workerCpuNanoseconds{0};
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

void MaterialService::Prefetch(const std::vector<Request>& requests)
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
    // Hydra data sources are consumed on the calling sync thread. Workers see
    // only serialized MaterialX and materialized resource paths.
    std::vector<Impl::Prepared> prepared;
    prepared.reserve(unique.size());
    for (const Request& request : unique)
        prepared.push_back(m_impl->Prepare(request.dataSource, request.path));
    unsigned workers = std::max(1u, std::min(4u,
        std::max(1u, std::thread::hardware_concurrency()) / 2u));
    if (const char* value = std::getenv("PRISM_MATERIAL_THREADS")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed > 0)
            workers = static_cast<unsigned>(std::min<unsigned long>(parsed, 64));
    }
    workers = std::min<unsigned>(workers, static_cast<unsigned>(prepared.size()));
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        pool.emplace_back([&] {
            const double cpuStart = CurrentThreadCpuSeconds();
            for (;;) {
                const size_t index = next.fetch_add(1);
                if (index >= prepared.size()) break;
                m_impl->Compile(prepared[index]);
            }
            m_impl->workerCpuNanoseconds += static_cast<int64_t>(
                (CurrentThreadCpuSeconds() - cpuStart) * 1.0e9);
        });
    }
    for (std::thread& worker : pool) worker.join();
    m_impl->batchWallNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - batchStart).count();
}

void MaterialService::Invalidate(const SdfPath& path, bool subtree)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (auto it = m_impl->pathCache.begin(); it != m_impl->pathCache.end();) {
        const bool match = subtree ? it->first.HasPrefix(path) : it->first == path;
        if (match) it = m_impl->pathCache.erase(it);
        else ++it;
    }
}

void MaterialService::InvalidateAll()
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->pathCache.clear();
}

MaterialService::MaterialStats MaterialService::GetStats() const
{
    MaterialStats result;
    result.requests = m_impl->requests.load();
    result.pathCacheHits = m_impl->pathCacheHits.load();
    result.contentCacheHits = m_impl->contentCacheHits.load();
    result.compilations = m_impl->compilations.load();
    result.nativeJits = m_impl->nativeJits.load();
    result.jobs = m_impl->jobs.load();
    result.failures = m_impl->failures.load();
    result.textureBytes = m_impl->textureBytes.load();
    result.conversionSeconds = m_impl->conversionNanoseconds.load() / 1.0e9;
    result.compilationSeconds = m_impl->compilationNanoseconds.load() / 1.0e9;
    result.generationSeconds = m_impl->generationNanoseconds.load() / 1.0e9;
    result.classCompilationSeconds =
        m_impl->classCompilationNanoseconds.load() / 1.0e9;
    result.nativeJitSeconds = m_impl->nativeJitNanoseconds.load() / 1.0e9;
    result.resourceReadSeconds = m_impl->resourceNanoseconds.load() / 1.0e9;
    result.batchWallSeconds = m_impl->batchWallNanoseconds.load() / 1.0e9;
    result.workerCpuSeconds = m_impl->workerCpuNanoseconds.load() / 1.0e9;
    return result;
}

} // namespace prism::hydra
