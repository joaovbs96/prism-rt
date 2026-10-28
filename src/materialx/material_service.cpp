#include "material_service.h"

#include "../mdl/context.h"
#include "../materialx/usd_network.h"
#include "../materialx/captured_material.h"
#include "../materialx/resolved_resource.h"
#include "../materialx/prepared_template.h"
#include "../materialx/preview_surface.h"
#include "../mdl_material.h"
#include "../resource_limits.h"
#include "../load_profile.h"
#include "../progress.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/diagnostic.h>
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
#include <pxr/imaging/hd/materialConnectionSchema.h>
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
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <numeric>
#include <functional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
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

namespace prism::materials {
namespace {

using prism::materialx::AddDisplacementShader;
using prism::materialx::FixPreviewSurfaceNormalOutput;
using prism::materialx::IsPreviewSurfaceNetwork;
using prism::materialx::RewritePreviewSurfaceNetwork;
using prism::materialx::SurfaceTerminalName;
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
    // The runtime's budget: the largest any renderer asked for.
    const uint64_t limit = prism::Runtime::Instance().settings().materialCacheMiB * 1024 * 1024;
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
    // MaterialSettings::Identity of the service that compiled the material.
    std::string materialIdentity;
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

using TemplateValueKey = std::pair<TfToken, TfToken>;

// Template values on which at least two captured group members disagree. All
// members share one template structure, so their nodes and parameters align.
std::vector<TemplateValueKey> VaryingTemplateValues(
    const std::vector<const NetworkSnapshot*>& members)
{
    std::vector<TemplateValueKey> result;
    if (members.empty()) return result;
    const NetworkSnapshot& first = *members.front();
    for (size_t n = 0; n < first.nodes.size(); ++n) {
        const auto& node = first.nodes[n];
        for (size_t p = 0; p < node.parameters.size(); ++p) {
            const auto& parameter = node.parameters[p];
            if (!prism::materialx::IsCompatibleTemplateValue(node, parameter)) continue;
            const bool varies = std::any_of(members.begin() + 1, members.end(),
                [&](const NetworkSnapshot* member) {
                    return n >= member->nodes.size() ||
                        p >= member->nodes[n].parameters.size() ||
                        member->nodes[n].parameters[p].name != parameter.name ||
                        member->nodes[n].parameters[p].data.value != parameter.data.value;
                });
            if (varies) result.emplace_back(node.name, parameter.name);
        }
    }
    return result;
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

// RenderMan's own texture format (.tex files starting 0xFACE) cannot be
// read: an image of the same name in another format, next to it or in a
// sibling directory, stands in for it. Empty when none is needed or found.
std::string ReadableImageSubstitute(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    unsigned char magic[2] = {0, 0};
    if (!file.read(reinterpret_cast<char*>(magic), 2) || magic[0] != 0xce || magic[1] != 0xfa) return {};
    const std::filesystem::path source(path);
    const std::string stem = source.stem().string();
    static const char* const extensions[] = {".exr", ".tx", ".tif", ".tiff", ".png", ".jpg", ".hdr"};
    std::vector<std::filesystem::path> directories{source.parent_path()};
    std::error_code error;
    for (auto const& entry : std::filesystem::directory_iterator(source.parent_path().parent_path(), error))
        if (entry.is_directory(error) && entry.path() != source.parent_path()) directories.push_back(entry.path());
    for (auto const& directory : directories)
        for (const char* extension : extensions) {
            const auto candidate = directory / (stem + extension);
            if (std::filesystem::is_regular_file(candidate, error)) return candidate.string();
        }
    return {};
}

// Material conversions run on several workers. They read hdMtlx's shared
// standard library (HdMtlxStdLibraries), which hdMtlx extends itself when a
// network uses a node definition the library lacks (an Sdr-registered
// asset implementation, or a v1.38 swizzle). Readers share this lock; a
// network that may extend the library converts alone.
std::shared_mutex& HdMtlxLibraryMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

// Call with HdMtlxLibraryMutex held at least shared.
bool MayExtendHdMtlxLibrary(HdMaterialNetworkInterface& net, std::string* why = nullptr)
{
    const mx::DocumentPtr& library = HdMtlxStdLibraries();
    for (const TfToken& node : net.GetNodeNames()) {
        const std::string type = net.GetNodeType(node).GetString();
        // hdMtlx looks definitions up under their current names.
        if (type.starts_with("ND_swizzle_") ||
            !library->getNodeDef(HdMtlxGetNodeDefName(type))) {
            if (why) *why = type;
            return true;
        }
    }
    return false;
}

// The parameters ResolveTextureAssets materializes: image nodes' file (or
// filename) and any other asset-valued input, such as triplanarprojection's
// filex, filey and filez (left as authored, a relative path fails the MDL
// module: C293 "strict relative file paths ... are not supported").
bool IsTextureParameter(TfToken const& name, VtValue const& value)
{
    return name == TfToken("file") || name == TfToken("filename") || value.IsHolding<SdfAssetPath>();
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
            HdMaterialNetworkInterface::NodeParamData data =
                net.GetNodeParameterData(node, param);
            if (!IsTextureParameter(param, data.value)) continue;
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
                    TF_WARN("[Hydra Material] Cannot materialize UDIM texture '%s': %s",
                            authored.c_str(), error.what());
                }
                // A file-system pattern without tiles (an optional texture
                // package not installed) would fail the whole MDL module:
                // drop the texture so the image node uses its default. Other
                // patterns keep the authored token for the stock SDK path
                // when a custom resolver cannot enumerate a tile family.
                if (preResolved.empty() || std::filesystem::path(preResolved).is_absolute()) {
                    // Once per renderer (the calling thread's logger).
                    if (prism::logging::Current().once("udim tiles\n" + authored))
                        TF_WARN("[Hydra Material] No UDIM tiles found for '%s'; the texture is skipped",
                                authored.c_str());
                    data.value = std::string();
                    data.typeName = TfToken("asset");
                    net.SetNodeParameterData(node, param, data);
                }
                continue;
            }

            ArResolvedPath resolved = !preResolved.empty()
                ? ArResolvedPath(preResolved) : resolver.Resolve(authored);
            if (resolved.empty()) {
                TF_WARN("[Hydra Material] Could not resolve texture '%s' on node %s",
                        authored.c_str(), node.GetText());
                continue;
            }
            if (auto substitute = ReadableImageSubstitute(resolved.GetPathString()); !substitute.empty()) {
                TF_WARN("[Hydra Material] '%s' is a RenderMan texture, which cannot be read; using '%s'",
                        resolved.GetPathString().c_str(), substitute.c_str());
                resolved = ArResolvedPath(substitute);
                extension = std::filesystem::path(substitute).extension().string();
            }
            prism::materialx::MaterialResourceResult preparedResource;
            try {
                preparedResource = prism::materialx::PrepareMaterialResource(
                    resolved, resolverContext, sourceVersion, cacheDir, extension);
            } catch (const std::exception& error) {
                TF_WARN("[Hydra Material] Cannot materialize texture '%s': %s",
                        resolved.GetPathString().c_str(), error.what());
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

// One texture file as ResolveTextureAssets materializes it, owned so that
// worker threads never touch a captured graph or a Hydra data source.
struct TextureWork {
    std::string authored, preResolved;
    ArResolverContext resolverContext;
    std::string sourceVersion, extension;
};

void CollectTextureWork(const NetworkSnapshot& network,
                        const ArResolverContext& resolverContext,
                        const std::string& sourceVersion,
                        std::unordered_set<std::string>* seen,
                        std::vector<TextureWork>* result)
{
    for (const auto& node : network.nodes) {
        for (const auto& parameter : node.parameters) {
            if (!IsTextureParameter(parameter.name, parameter.data.value)) continue;
            // Conversion drops a value whose input is connected.
            if (std::any_of(node.connections.begin(), node.connections.end(),
                    [&](const auto& connection) {
                        return connection.input == parameter.name &&
                            !connection.connections.empty();
                    })) continue;
            TextureWork work;
            const VtValue& value = parameter.data.value;
            if (value.IsHolding<SdfAssetPath>()) {
                work.authored = value.UncheckedGet<SdfAssetPath>().GetAssetPath();
                work.preResolved = value.UncheckedGet<SdfAssetPath>().GetResolvedPath();
            } else if (value.IsHolding<std::string>()) {
                work.authored = value.UncheckedGet<std::string>();
            }
            // UDIM families are materialized tile by tile elsewhere.
            if (work.authored.empty() ||
                work.authored.find("<UDIM>") != std::string::npos ||
                work.preResolved.find("<UDIM>") != std::string::npos) continue;
            work.extension = std::filesystem::path(work.authored).extension().string();
            if (work.extension.empty()) work.extension = ".bin";
            work.resolverContext = resolverContext;
            work.sourceVersion = sourceVersion;
            const std::string identity = work.authored + "\n" + work.preResolved + "\n" +
                resolverContext.GetDebugString() + "\n" + sourceVersion + "\n" + work.extension;
            if (seen->insert(identity).second) result->push_back(std::move(work));
        }
    }
}

// Prepares the union of a batch's textures on a bounded TBB arena while the
// batch converts and compiles, requesting exactly what the MDL texture
// runtime will, so its calls find them in memory or wait on the service's
// single-flight gate. It is only an optimization: failures are dropped and
// the runtime prepares, and reports, on its own.
class TexturePrefetch {
public:
    TexturePrefetch(std::vector<TextureWork> work, std::filesystem::path cacheDir,
                    std::function<bool()> cancelled, std::atomic<uint64_t>* bytesRead,
                    unsigned threads, prism::Progress* progress)
        : m_progress(progress), m_work(std::move(work)), m_cacheDir(std::move(cacheDir)),
          m_cancelled(std::move(cancelled)), m_bytesRead(bytesRead),
          m_results(m_work.size()),
          // No slot is reserved for the calling thread: it goes on to
          // convert and compile while the arena's workers prepare.
          m_arena(static_cast<int>(std::min<size_t>(threads, m_work.size())), 0)
    {
        if (prism::load_profile::Enabled()) {
            m_profile = std::make_unique<prism::load_profile::Scope>("texture.prefetch");
            m_profile->detail("items=" + std::to_string(m_work.size()));
        }
        m_running = m_arena.max_concurrency();
        if (m_progress) {
            m_progress->Begin(prism::Phase::Textures);
            m_progress->SetTotal(prism::Phase::Textures, m_work.size());
        }
        m_arena.execute([this] {
            for (int i = 0; i < m_arena.max_concurrency(); ++i)
                m_group.run([this] {
                    Run();
                    if (m_running.fetch_sub(1) == 1) m_profile.reset();
                });
        });
    }
    // Preparation jobs must finish before Prefetch returns; the MDL SDK and
    // the texture service may be torn down right after it.
    ~TexturePrefetch()
    {
        m_arena.execute([this] { m_group.wait(); });
        if (m_progress) m_progress->End(prism::Phase::Textures);
    }
    TexturePrefetch(const TexturePrefetch&) = delete;
    TexturePrefetch& operator=(const TexturePrefetch&) = delete;

private:
    struct Result {
        std::shared_ptr<const prism::materialx::ResolvedMaterialResource> source;
        // The service indexes resources weakly; holding them until the batch
        // ends is what lets the compiles find them in memory.
        std::shared_ptr<const prism::TextureResource> texture;
    };
    void Run()
    {
        for (;;) {
            if (m_cancelled && m_cancelled()) return;
            const size_t index = m_next.fetch_add(1);
            if (index >= m_work.size()) return;
            try { m_results[index] = Prepare(m_work[index]); }
            catch (...) {}
            if (m_progress) m_progress->Add(prism::Phase::Textures);
        }
    }
    Result Prepare(const TextureWork& work)
    {
        Result result;
        ArResolverContextBinder binder(work.resolverContext);
        const ArResolvedPath resolved = !work.preResolved.empty()
            ? ArResolvedPath(work.preResolved) : ArGetResolver().Resolve(work.authored);
        if (resolved.empty()) return result;
        // The same single-flight registry ResolveTextureAssets and template
        // binding use; whichever thread comes first reads, hashes and copies.
        auto materialized = prism::materialx::PrepareMaterialResource(resolved,
            work.resolverContext, work.sourceVersion, m_cacheDir, work.extension);
        *m_bytesRead += materialized.bytesRead;
        result.source = std::move(materialized.resource);
        if (!result.source) return result;
        // NativeTextureRuntime::prepareImage requests the texture's effective
        // gamma with unassociated alpha. MaterialX-generated MDL declares every
        // file texture tex::gamma_linear, and template binding creates textures
        // with gamma 1, so the effective gamma is always 1.
        prism::TexturePrepareOptions options;
        options.unassociateAlpha = true;
        result.texture = prism::TextureService::Acquire()->prepareAhead(
            result.source->path, options);
        return result;
    }

    prism::Progress* m_progress;
    std::vector<TextureWork> m_work;
    std::filesystem::path m_cacheDir;
    std::function<bool()> m_cancelled;
    std::atomic<uint64_t>* m_bytesRead;
    std::vector<Result> m_results;
    std::atomic<size_t> m_next{0};
    std::atomic<int> m_running{0};
    std::unique_ptr<prism::load_profile::Scope> m_profile;
    tbb::task_arena m_arena;
    tbb::task_group m_group;
};

struct PrewarmResult {
    double seconds = 0.0;
    std::string error;
};

// One pre-warm per context and MaterialX paths for the whole process: the
// initialized state is process-wide, while services come and go per build.
std::mutex gPrewarmMutex;
auto& gPrewarms = *new std::map<std::string, std::shared_future<PrewarmResult>>();
std::atomic<uint64_t> gPrewarmJobs{0};

std::string PrewarmContextPrefix(const mdl::Context* context)
{
    return std::to_string(reinterpret_cast<std::uintptr_t>(context)) + "\n";
}

// Loads the modules generated MDL imports, including the MaterialX support
// modules, which the first generated module would otherwise load as most of
// its class compilation. The names come from a minimal generated module, so
// they follow the installed MaterialX version.
std::string LoadSupportModules(mdl::Context& context, const std::string& libraries,
                               prism::color::ColorManagement const& color)
{
    auto generator = MDLGenerator::shared(libraries);
    mx::DocumentPtr doc = mx::createDocument();
    doc->setDataLibrary(generator->get_stdlib());
    // No authored inputs: generate() bypasses the template caches.
    doc->addMaterialNode("PrismMaterial",
        doc->addNode("standard_surface", "Surface", "surfaceshader"));
    const GeneratedMDL generated = generator->generate(doc, "PrismMaterial", color);
    if (!generated.success) return "support module discovery: " + generated.error_message;
    std::vector<std::string> modules;
    std::istringstream lines(generated.mdl_source);
    for (std::string line; std::getline(lines, line);) {
        // "import ::df::*;" and "using ::materialx::core import *;"
        size_t end = std::string::npos;
        size_t begin = 0;
        if (line.starts_with("import ::")) { begin = 7; end = line.find("::*"); }
        else if (line.starts_with("using ::")) { begin = 6; end = line.find(" import"); }
        if (end != std::string::npos && end > begin)
            modules.push_back(line.substr(begin, end - begin));
    }
    if (modules.empty()) return "generated MDL imports no modules";

    mi::base::Handle<mi::neuraylib::IScope> scope(
        context.get_database()->get_global_scope());
    mi::base::Handle<mi::neuraylib::ITransaction> transaction(
        scope ? scope->create_transaction() : nullptr);
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> execution(
        context.get_factory()->create_execution_context());
    if (!transaction || !execution) return "cannot create an MDL transaction";
    std::string error;
    for (const std::string& module : modules) {
        if (context.get_impexp()->load_module(
                transaction.get(), module.c_str(), execution.get()) >= 0) continue;
        error = "cannot load " + module;
        for (mi::Size i = 0; i < execution->get_messages_count(); ++i) {
            mi::base::Handle<const mi::neuraylib::IMessage> message(execution->get_message(i));
            if (message && message->get_string()) error += std::string(": ") + message->get_string();
        }
        break;
    }
    // Committed even after a failure: modules loaded before it stay valid.
    // No handle into the transaction may outlive the commit.
    execution.reset();
    scope.reset();
    transaction->commit();
    return error;
}

PrewarmResult RunPrewarm(mdl::Context* context, const std::string& libraries,
                         const std::string& mdlPath, const std::filesystem::path& cacheDir,
                         unsigned workers,
                         std::shared_ptr<const prism::color::ColorManagement> const& color)
{
    const auto start = std::chrono::steady_clock::now();
    prism::load_profile::Scope profile("materials.prewarm");
    PrewarmResult result;
    try {
        // Loaded by the first hdMtlx conversion.
        (void)HdMtlxStdLibraries();
        // Loads the generator's standard library and digest, and leaves a
        // generator for each worker of the first batch.
        MDLGenerator::reserveWorkers(libraries, workers, color.get());
        // As Prefetch registers them; loading modules by name needs the path.
        if (!mdlPath.empty()) {
            context->add_mdl_path(mdlPath);
            context->add_resource_path(mdlPath);
        }
        context->add_resource_path(cacheDir.string());
        result.error = LoadSupportModules(*context, libraries, *color);
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "unknown exception";
    }
    result.seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    // Runs once per key, so this is logged once.
    if (!result.error.empty())
        TF_WARN("[Hydra Material] Material pre-warm failed; materials initialize "
                "on first use: %s", result.error.c_str());
    return result;
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
        // Prefetch compile policy: fold every class parameter except resources
        // and the captured template values in keptValues. A shared folded
        // program serves a template group; an unshared one a single material.
        bool foldParameters = false;
        bool shareFoldedProgram = false;
        std::vector<TemplateValueKey> keptValues;
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
                    entry.materialIdentity == materialIdentity &&
                    entry.resourceRevision == prepared.resourceRevision &&
                    entry.resolverContext == prepared.resolverContext &&
                    entry.network == prepared.network;
            }), gEarlyMaterialCache.end());
        if (gEarlyMaterialCache.size() >= kEarlyMaterialCacheLimit)
            gEarlyMaterialCache.erase(gEarlyMaterialCache.begin());
        gEarlyMaterialCache.push_back({prepared.path, libraries, mdlPath, materialIdentity,
            prepared.resourceRevision,
            prepared.resolverContext, prepared.network, prepared.dependencies, material});
    }
    Impl(mdl::Context* context, prism::RendererOptions const& options,
         prism::MaterialSettings settings, uint64_t requestedAssociationScope)
        : context(context)
        , materials(std::move(settings))
        , libraries(options.materials.materialxLibraries)
        , mdlPath(options.materials.materialxMdlPath)
        , cacheDir(prism::Runtime::Instance().settings().CacheRoot() / "materials")
        , associationScope(requestedAssociationScope ? requestedAssociationScope :
              static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(context)))
        , foldParameters(options.materials.compileMode == prism::MaterialOptions::CompileMode::Auto)
        , prewarmEnabled(options.materials.prewarm)
        , parallelTexturePreparation(options.materials.parallelTexturePreparation)
        , materialWorkers(options.limits.MaterialWorkers())
        , textureThreads(options.limits.TexturePreparationThreads())
    {
        if (!materials.color) throw std::invalid_argument("MaterialService needs a colour management");
        materialIdentity = materials.Identity();
        identityKey = HashText(materialIdentity);
        if (this->mdlPath.empty() && !this->libraries.empty())
            this->mdlPath = (std::filesystem::path(this->libraries) / "mdl").string();
        prewarmKey = PrewarmContextPrefix(context) + this->libraries + "\n" + this->mdlPath;
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
                    it->mdlPath == mdlPath && it->materialIdentity == materialIdentity &&
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
                    it->artifactKey + "-" + identityKey;
                result.valid = true;
                ++earlyCacheHits;
                return result;
            }
            NetworkSnapshot structure;
            if (allowTemplate && TemplateStructure(result.network, &structure)) {
                CompatibleTemplate entry;
                if (prism::materialx::CompatibleTemplateRegistry::Shared().find(
                        context, libraries, mdlPath, structure, materialIdentity, &entry)) {
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
                        reinterpret_cast<std::uintptr_t>(context)) + "-" + artifactKey + "-" + identityKey;
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
            TF_WARN("[Hydra Material] Unsupported universal material network at %s",
                    materialPath.GetText());
            return result;
        }

        if (previewSurface && !RewritePreviewSurfaceNetwork(netInterface))
            return result;
        uint64_t resolvedTextureBytes = 0;
        prism::load_profile::Scope conversionProfile("material.convert");
        auto profileClock = std::chrono::steady_clock::now();
        auto profileLap = [&profileClock] {
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - profileClock).count();
            profileClock = now;
            return std::to_string(int(ms));
        };
        std::string profileDetail = materialPath.GetString();
        const auto resourceStart = std::chrono::steady_clock::now();
        const std::string resourceFingerprint =
            ResolveTextureAssets(netInterface, cacheDir, result.resolverContext,
                std::to_string(result.resourceRevision),
                &resolvedTextureBytes);
        resourceNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - resourceStart).count();
        profileDetail += " resources_ms=" + profileLap();
        textureBytes += resolvedTextureBytes;
        HdMtlxTexturePrimvarData mtlxData;
        ++conversionCalls;
        // Conversions run concurrently; see HdMtlxLibraryMutex.
        std::shared_lock sharedLibrary(HdMtlxLibraryMutex());
        std::unique_lock exclusiveLibrary(HdMtlxLibraryMutex(), std::defer_lock);
        std::string extendingType;
        if (MayExtendHdMtlxLibrary(netInterface, &extendingType)) {
            sharedLibrary.unlock();
            exclusiveLibrary.lock();
            profileDetail += " exclusive=" + extendingType;
        }
        const mx::DocumentPtr& library = HdMtlxStdLibraries();
        mx::DocumentPtr doc = prism::materialx::SimpleOpenPbrDocument(netInterface, terminal.second.upstreamNodeName, library);
        const bool simpleDocument = bool(doc);
        if (!doc) {
            prism::materialx::CapturedMaterialRegistry::Shared().recordStockConversion();
            doc = HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
                &netInterface,
                terminal.second.upstreamNodeName,
                netInterface.GetNodeInputConnectionNames(terminal.second.upstreamNodeName),
                library, &mtlxData);
        }
        if (!doc) {
            TF_WARN("[Hydra Material] MaterialX conversion failed at %s",
                    materialPath.GetText());
            return result;
        }
        if (previewSurface) FixPreviewSurfaceNormalOutput(doc);
        else AddDisplacementShader(netInterface, doc, library);
        prism::materialx::ShortenHdMtlxNodeNames(doc,
            prism::materialx::ShortHdMtlxNodeNames(netInterface.GetNodeNames()));
        profileDetail += (simpleDocument ? " simple_ms=" : " hdmtlx_ms=") + profileLap();

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
        if (!simpleDocument) doc->importLibrary(library);
        sharedLibrary = {};
        exclusiveLibrary = {};
        profileDetail += " import_ms=" + profileLap();
        result.xml = prism::materialx::SerializeDocument(doc, libraries);
        profileDetail += " serialize_ms=" + profileLap() +
            " xml_kb=" + std::to_string(result.xml.size() / 1024);
        conversionProfile.detail(profileDetail);
        conversionNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - conversionStart).count();
        // Native target-code handles are owned by one MDL runtime. Keep the
        // process cache partitioned when tests or standalone clients create a
        // second Context with otherwise identical material inputs.
        const std::string artifactKey = HashText(
            "PrismHydraMaterial-v2\n" + libraries + "\n" + mdlPath + "\n" +
            resourceFingerprint + "\n" + result.xml);
        const std::string baseKey =
            std::to_string(reinterpret_cast<std::uintptr_t>(context)) + "-" + artifactKey + "-" + identityKey;
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
            TF_WARN("[Hydra Material] Cannot create material cache '%s'",
                    cacheDir.string().c_str());
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
            // Publish the serialized XML only. This document has the whole
            // standard library imported into it (about 20 MB in memory), and
            // the registry keeps up to 2048 entries; readers parse the XML.
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

    // needsFallback: a template bind was rejected. grownKeepSet: when folded
    // values caused that, the template's kept values plus the differing ones.
    std::shared_ptr<Material> Compile(Prepared const& prepared,
                                      bool* needsFallback = nullptr,
                                      std::vector<TemplateValueKey>* grownKeepSet = nullptr)
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
                    std::to_string(prepared.resourceRevision), cacheDir,
                    materials.varyingEmitters.get());
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
            if (grownKeepSet && !bound.differingValues.empty()) {
                // Keeping what this material changed as arguments lets the
                // next edit of the same value bind instead of compiling.
                *grownKeepSet = bound.differingValues;
                for (const auto& binding : prepared.templateBindings)
                    if (!binding.folded)
                        grownKeepSet->emplace_back(binding.node, binding.parameter);
            }
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
        MDLMaterial::CompileSelector select;
        // Kept values are captured (node, parameter) pairs; the generated
        // uniforms that carry them are only known once generation ran.
        if (prepared.foldParameters) select = [&prepared](const GeneratedMDL& generated) {
            mdl::CompileSpecification specification;
            specification.foldParameters = true;
            specification.shareProgram = prepared.shareFoldedProgram;
            std::vector<CompatibleBinding> bindings;
            if (!prepared.keptValues.empty() && prism::materialx::BuildCompatibleBindings(
                    prepared.conversionNetwork, generated.uniforms, &bindings))
                for (const auto& binding : bindings)
                    if (std::find(prepared.keptValues.begin(), prepared.keptValues.end(),
                            TemplateValueKey(binding.node, binding.parameter)) !=
                        prepared.keptValues.end())
                        specification.keptUniforms.insert(binding.uniform);
            return specification;
        };
        std::shared_ptr<MDLMaterial> candidate;
        prism::load_profile::Scope compileProfile("material.compile");
        try {
            candidate = std::make_shared<MDLMaterial>(
                prepared.mtlxPath.string(), *context, libraries, mdlPath,
                materials, prepared.materialName, &timings, select);
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
                gMaterialCompiling.erase(prepared.key);
            }
            gMaterialCacheCondition.notify_all();
            ++failures;
            TF_WARN("[Hydra Material] MDL compile threw for %s: %s",
                    prepared.path.GetText(), error.what());
            return nullptr;
        } catch (...) {
            {
                std::lock_guard<std::mutex> cacheLock(gMaterialCacheMutex);
                gMaterialCompiling.erase(prepared.key);
            }
            gMaterialCacheCondition.notify_all();
            ++failures;
            TF_WARN("[Hydra Material] MDL compile threw for %s",
                    prepared.path.GetText());
            return nullptr;
        }
        compilationNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - compilationStart).count();
        ++compilations;
        if (timings.foldedParameters) ++foldedCompilations;
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
        if (prism::load_profile::Enabled()) {
            auto ms = [](double seconds) { return std::to_string(int(seconds * 1000.0)); };
            compileProfile.detail(prepared.path.GetString() +
                " load_ms=" + ms(timings.loadSeconds) +
                " generate_ms=" + ms(timings.generationSeconds - timings.loadSeconds) +
                " class_ms=" + ms(timings.classCompilationSeconds) +
                " jit_ms=" + ms(timings.nativeJitSeconds) +
                " program_wait_ms=" + ms(target.programWaitSeconds) +
                (target.cacheHit ? " program=reused" : " program=jit") +
                (timings.templateCacheHit ? " generation=cached" : ""));
        }
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
            if (compatible && candidate->has_folded_parameters())
                compatible = target.program && prism::materialx::RecordFoldedBindings(
                    prepared.network, target.program->parameters, &bindings);
            if (compatible && !bindings.empty()) {
                prism::materialx::CompatibleTemplateRegistry::Shared().publish(
                    {context, libraries, mdlPath, std::move(structure),
                     candidate, std::move(bindings)}, materialIdentity);
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
            TF_WARN("[Hydra Material] MDL compile failed for %s; using its magenta error material",
                    prepared.path.GetText());
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

    std::shared_future<PrewarmResult> FindPrewarm() const
    {
        std::lock_guard lock(gPrewarmMutex);
        const auto found = gPrewarms.find(prewarmKey);
        return found != gPrewarms.end() ? found->second : prewarm;
    }

    // Compiling alongside a running pre-warm would repeat its work and could
    // use the libraries it is still loading.
    void AwaitPrewarm()
    {
        const auto running = FindPrewarm();
        if (!running.valid() ||
            running.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            return;
        const auto start = std::chrono::steady_clock::now();
        running.wait();
        prewarmWaitNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
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
    // The colour management materials compile in, and its identity (the
    // shared caches' key).
    prism::MaterialSettings materials;
    std::string materialIdentity;
    // Its hash: compiled materials are shared by (context, document,
    // identity) through gMaterialCache.
    std::string identityKey;
    std::string libraries;
    std::string mdlPath;
    std::filesystem::path cacheDir;
    uint64_t resourceRevision = 0;
    uint64_t associationScope = 0;
    // Prefetch compile policy (MaterialOptions::compileMode): fold every class
    // parameter that no material of the batch needs to rebind (Auto), or keep
    // all but strings runtime arguments (Class), which is how Resolve and the
    // standalone loader always compile.
    std::atomic<bool> foldParameters{true};
    bool prewarmEnabled = true;
    std::atomic<bool> parallelTexturePreparation{true};
    unsigned materialWorkers = 1;
    unsigned textureThreads = 1;
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
    std::atomic<uint64_t> compilations{0}, foldedCompilations{0}, nativeJits{0}, jobs{0};
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
    std::string prewarmKey;
    // Pre-warm this service started or joined. Its context must outlive it,
    // so the destructor waits; a scoped context may die right after.
    std::shared_future<PrewarmResult> prewarm;
    std::atomic<int64_t> prewarmWaitNanoseconds{0};
};

MaterialService::MaterialService(mdl::Context* context,
                                 prism::RendererOptions const& options,
                                 prism::MaterialSettings materials,
                                 uint64_t associationScope)
    : m_impl(std::make_unique<Impl>(context, options, std::move(materials), associationScope))
{}

MaterialService::~MaterialService() {
    const auto context = m_impl->context;
    if (m_impl->prewarm.valid()) m_impl->prewarm.wait();
    if (context != gSharedMdlContext.load(std::memory_order_acquire)) {
        // A later Context may reuse this address; its key must pre-warm anew.
        // Running entries belong to live services, which wait for them.
        std::lock_guard lock(gPrewarmMutex);
        const auto prefix = PrewarmContextPrefix(context);
        std::erase_if(gPrewarms, [&](const auto& entry) {
            return entry.first.starts_with(prefix) &&
                entry.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
    }
    // Release per-path references before deciding which warm records are unused.
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
    m_impl->AwaitPrewarm();
    return m_impl->Resolve(materialPrimDataSource, materialPath);
}

void MaterialService::Prewarm()
{
    Impl& impl = *m_impl;
    if (!impl.prewarmEnabled || !impl.context || !impl.context->is_valid() ||
        impl.libraries.empty()) return;
    std::lock_guard lock(gPrewarmMutex);
    auto& entry = gPrewarms[impl.prewarmKey];
    if (!entry.valid()) {
        try {
            // Owns copies only: the service may be destroyed while it runs.
            entry = std::async(std::launch::async,
                [context = impl.context, libraries = impl.libraries,
                 mdlPath = impl.mdlPath, cacheDir = impl.cacheDir,
                 workers = impl.materialWorkers, color = impl.materials.color] {
                    return RunPrewarm(context, libraries, mdlPath, cacheDir, workers, color);
                }).share();
            ++gPrewarmJobs;
        } catch (const std::exception& error) {
            gPrewarms.erase(impl.prewarmKey);
            TF_WARN("[Hydra Material] Cannot start the material pre-warm: %s",
                    error.what());
            return;
        }
    }
    impl.prewarm = entry;
}

bool MaterialService::MayDisplace(const HdContainerDataSourceHandle& materialPrimDataSource)
{
    HdMaterialSchema material = HdMaterialSchema::GetFromParent(materialPrimDataSource);
    if (!material) return false;
    for (const TfToken& context : material.GetRenderContexts()) {
        HdMaterialNetworkSchema network = material.GetMaterialNetwork(context);
        if (!network) continue;
        HdMaterialConnectionSchema terminal = network.GetTerminals().Get(TfToken("displacement"));
        if (terminal) return true;
    }
    return false;
}

void MaterialService::WaitForPrewarm()
{
    const auto running = m_impl->FindPrewarm();
    if (running.valid()) running.wait();
}

void MaterialService::Prefetch(const std::vector<Request>& requests,
                               const std::function<bool()>& cancelled)
{
    if (requests.empty()) return;
    m_impl->AwaitPrewarm();
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
    prism::load_profile::Scope profile("materials.prefetch");
    profile.detail("requests=" + std::to_string(unique.size()));
    // The renderer's progress model and logger on this thread and the
    // compile pool's (the calling thread's if the settings carry none).
    prism::Progress* const progressModel =
        m_impl->materials.progress ? m_impl->materials.progress : prism::Progress::Current();
    prism::Logger const* const logger =
        m_impl->materials.logger ? m_impl->materials.logger : &prism::logging::Current();
    prism::progress::Bind progressBinding(progressModel);
    prism::logging::Bind logBinding(logger);
    m_impl->jobs += unique.size();
    prism::progress::Scope progressScope(prism::Phase::Materials);
    prism::progress::AddTotal(prism::Phase::Materials, unique.size());
    // Capture immutable identities first. Repeated compatible structures need
    // one seed compiled before their variants are prepared, otherwise the old
    // prepare-all ordering would convert every variant before a template could
    // be published.
    std::vector<Impl::Prepared> captured;
    captured.reserve(unique.size());
    {
        prism::load_profile::Scope captureProfile("materials.capture");
        for (const Request& request : unique) {
            if (cancelled && cancelled()) return;
            captured.push_back(m_impl->Prepare(
                request.dataSource, request.path, true, true));
        }
    }
    // Every texture of the batch is known now, long before the compile of
    // the material that needs it. Declared after captured so that an early
    // return waits for the preparations first.
    std::unique_ptr<TexturePrefetch> texturePrefetch;
    if (m_impl->parallelTexturePreparation) {
        std::vector<TextureWork> work;
        std::unordered_set<std::string> seenTextures;
        for (const auto& item : captured)
            if (!item.cached)
                CollectTextureWork(item.network, item.resolverContext,
                    std::to_string(item.resourceRevision), &seenTextures, &work);
        if (!work.empty())
            texturePrefetch = std::make_unique<TexturePrefetch>(
                std::move(work), m_impl->cacheDir, cancelled, &m_impl->textureBytes,
                m_impl->textureThreads, prism::Progress::Current());
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

    const bool fold = m_impl->foldParameters;
    // A material compiled alone folds everything except resources; no other
    // material is expected to share its program.
    auto foldAlone = [&](Impl::Prepared value) {
        value.foldParameters = fold;
        value.shareFoldedProgram = false;
        value.keptValues.clear();
        return value;
    };
    // Compiles on a bounded worker pool. An item with a preparer is converted
    // by its worker first, so conversions run in parallel and overlap other
    // items' compilation. Converting reads the retained scene captured for
    // this build, never the live scene, under the resolver context bound on
    // this calling thread.
    using Preparers = std::vector<std::function<Impl::Prepared()>>;
    const ArResolverContext resolverContext = ArGetResolver().GetCurrentContext();
    auto compileInPool = [&](std::vector<Impl::Prepared>& items,
                             std::vector<std::shared_ptr<Material>>* results,
                             std::vector<uint8_t>* needsFallback,
                             std::vector<std::vector<TemplateValueKey>>* grownKeepSets,
                             const Preparers* preparers = nullptr) {
        // The main pass is counted up front; seeds and fallbacks add to it.
        if (!grownKeepSets) prism::progress::AddTotal(prism::Phase::Materials, items.size());
        const unsigned workers = std::min<unsigned>(
            m_impl->materialWorkers, static_cast<unsigned>(items.size()));
        prism::load_profile::Scope poolProfile("materials.compile_pool");
        poolProfile.detail("items=" + std::to_string(items.size()) +
                           " workers=" + std::to_string(workers));
        // Largest graphs first: one large conversion started last would end
        // the pool alone.
        std::vector<size_t> order(items.size());
        std::iota(order.begin(), order.end(), size_t(0));
        if (preparers)
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return items[a].network.nodes.size() > items[b].network.nodes.size();
            });
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            pool.emplace_back([&] {
                prism::progress::Bind workerProgress(progressModel);
                prism::logging::Bind workerLog(logger);
                const double cpuStart = CurrentThreadCpuSeconds();
                for (;;) {
                    if (cancelled && cancelled()) break;
                    const size_t position = next.fetch_add(1);
                    if (position >= items.size()) break;
                    const size_t index = order[position];
                    bool fallback = false;
                    std::shared_ptr<Material> material;
                    // An exception leaving a std::thread terminates the
                    // process; a failed material (for example out of memory)
                    // renders with the fallback material instead.
                    try {
                        if (preparers && (*preparers)[index]) {
                            ArResolverContextBinder binder(resolverContext);
                            items[index] = (*preparers)[index]();
                        }
                        material = m_impl->Compile(
                            items[index], needsFallback ? &fallback : nullptr,
                            grownKeepSets ? &(*grownKeepSets)[index] : nullptr);
                    } catch (const std::exception& error) {
                        ++m_impl->failures;
                        fallback = false;
                        TF_WARN("[Hydra Material] Material preparation failed for %s: %s",
                                items[index].path.GetText(), error.what());
                    }
                    prism::progress::Add(prism::Phase::Materials);
                    if (results) (*results)[index] = std::move(material);
                    if (needsFallback) (*needsFallback)[index] = fallback ? 1 : 0;
                }
                m_impl->workerCpuNanoseconds += static_cast<int64_t>(
                    (CurrentThreadCpuSeconds() - cpuStart) * 1.0e9);
            });
        }
        for (std::thread& worker : pool) worker.join();
    };

    // Hydra data sources are consumed on this calling sync thread. Workers see
    // only serialized MaterialX, bound templates and materialized resources.
    std::vector<Impl::Prepared> prepared;
    prepared.reserve(unique.size());
    std::vector<Impl::Prepared> seeds;
    Preparers seedPreparers;
    std::vector<size_t> seedGroupIndices;
    std::vector<std::shared_ptr<Material>> groupSeedMaterials(groups.size());
    // A placeholder for an item its worker prepares; failures keep the path.
    auto pending = [](const Request& request, const NetworkSnapshot& network) {
        Impl::Prepared value;
        value.source = request.dataSource;
        value.path = request.path;
        // Only orders the pool (see compileInPool); preparation replaces it.
        value.network = network;
        return value;
    };
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const Group& group = groups[groupIndex];
        if (group.indices.size() < 2) continue;
        if (cancelled && cancelled()) return;
        const size_t seedIndex = group.indices.front();
        // The seed keeps as arguments exactly the template values on which
        // group members disagree; its variants bind only those.
        std::vector<const NetworkSnapshot*> members;
        for (const size_t index : group.indices) members.push_back(&captured[index].network);
        seeds.push_back(pending(unique[seedIndex], captured[seedIndex].network));
        seedPreparers.push_back([this, &unique, seedIndex, fold,
                                 keptValues = VaryingTemplateValues(members)] {
            auto seed = m_impl->Prepare(unique[seedIndex].dataSource, unique[seedIndex].path);
            seed.foldParameters = fold;
            seed.shareFoldedProgram = true;
            seed.keptValues = keptValues;
            return seed;
        });
        seedGroupIndices.push_back(groupIndex);
        grouped[seedIndex] = true;
        for (size_t j = 1; j < group.indices.size(); ++j)
            grouped[group.indices[j]] = true;
    }
    std::vector<std::shared_ptr<Material>> seedMaterials(seeds.size());
    if (!seeds.empty()) {
        compileInPool(seeds, &seedMaterials, nullptr, nullptr, &seedPreparers);
        for (size_t i = 0; i < seeds.size(); ++i) {
            if (cancelled && cancelled()) return;
            if (seedMaterials[i] || !seeds[i].templatePrototype) continue;
            auto fallback = m_impl->Prepare(
                seeds[i].source, seeds[i].path, false);
            fallback.foldParameters = seeds[i].foldParameters;
            fallback.shareFoldedProgram = seeds[i].shareFoldedProgram;
            fallback.keptValues = seeds[i].keptValues;
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
                    structure, m_impl->materialIdentity, &entry)) {
                variant.templatePrototype = std::move(entry.prototype);
                variant.templateBindings = std::move(entry.bindings);
                variant.valid = true;
                ++m_impl->earlyCacheHits;
                prepared.push_back(std::move(variant));
            } else {
                prepared.push_back(foldAlone(m_impl->Prepare(
                    unique[index].dataSource, unique[index].path)));
            }
        }
    }
    Preparers preparers(prepared.size());
    for (size_t i = 0; i < unique.size(); ++i) {
        if (grouped[i]) continue;
        if (captured[i].cached) {
            prepared.push_back(std::move(captured[i]));
            preparers.emplace_back();
        } else {
            prepared.push_back(pending(unique[i], captured[i].network));
            preparers.push_back([this, &unique, &foldAlone, i] {
                return foldAlone(m_impl->Prepare(unique[i].dataSource, unique[i].path));
            });
        }
    }
    std::vector<uint8_t> needsFallback(prepared.size(), 0);
    std::vector<std::vector<TemplateValueKey>> grownKeepSets(prepared.size());
    compileInPool(prepared, nullptr, &needsFallback, &grownKeepSets, &preparers);
    // Rejected template bindings (mapping/resource incompatibilities, or a
    // value the template folded) return to the Prefetch caller, which owns the
    // captured data-source lifetime and re-prepares them. Workers never
    // recapture or traverse a caller-provided source; they only compile.
    // Materials rejected over folded values by one template share its
    // structure: one seed keeping the template's arguments plus the union of
    // their differing values replaces the template and serves them all.
    struct Regroup {
        const MDLMaterial* prototype = nullptr;
        std::vector<size_t> indices;
        std::vector<TemplateValueKey> keptValues;
    };
    std::vector<Regroup> regroups;
    std::vector<Impl::Prepared> fallbacks;
    for (size_t i = 0; i < prepared.size(); ++i) {
        if (cancelled && cancelled()) break;
        if (!needsFallback[i]) continue;
        if (fold && !grownKeepSets[i].empty()) {
            auto regroup = std::find_if(regroups.begin(), regroups.end(),
                [&](const Regroup& entry) {
                    return entry.prototype == prepared[i].templatePrototype.get();
                });
            if (regroup == regroups.end())
                regroup = regroups.insert(regroups.end(),
                    Regroup{prepared[i].templatePrototype.get(), {}, {}});
            regroup->indices.push_back(i);
            for (const auto& key : grownKeepSets[i])
                if (std::find(regroup->keptValues.begin(), regroup->keptValues.end(), key) ==
                    regroup->keptValues.end())
                    regroup->keptValues.push_back(key);
            continue;
        }
        fallbacks.push_back(foldAlone(m_impl->Prepare(
            prepared[i].source, prepared[i].path, false)));
    }
    std::vector<size_t> rebinds;
    for (const Regroup& regroup : regroups) {
        const size_t seedIndex = regroup.indices.front();
        auto seed = m_impl->Prepare(
            prepared[seedIndex].source, prepared[seedIndex].path, false);
        seed.foldParameters = true;
        seed.shareFoldedProgram = regroup.indices.size() > 1;
        seed.keptValues = regroup.keptValues;
        fallbacks.push_back(std::move(seed));
        rebinds.insert(rebinds.end(), regroup.indices.begin() + 1, regroup.indices.end());
    }
    if (!fallbacks.empty()) compileInPool(fallbacks, nullptr, nullptr, nullptr);
    // The other members bind against the templates those seeds published. A
    // bind that is still rejected compiles all-folded on its own, so the
    // fallback chain always terminates.
    std::vector<Impl::Prepared> rebound;
    for (const size_t index : rebinds) {
        if (cancelled && cancelled()) break;
        rebound.push_back(foldAlone(m_impl->Prepare(
            prepared[index].source, prepared[index].path)));
    }
    std::vector<uint8_t> stillRejected(rebound.size(), 0);
    if (!rebound.empty()) compileInPool(rebound, nullptr, &stillRejected, nullptr);
    std::vector<Impl::Prepared> lastResort;
    for (size_t i = 0; i < rebound.size(); ++i) {
        if (cancelled && cancelled()) break;
        if (!stillRejected[i]) continue;
        lastResort.push_back(foldAlone(m_impl->Prepare(
            rebound[i].source, rebound[i].path, false)));
    }
    if (!lastResort.empty()) compileInPool(lastResort, nullptr, nullptr, nullptr);
    texturePrefetch.reset();
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

void MaterialService::SetCompileMode(prism::MaterialOptions::CompileMode mode)
{
    m_impl->foldParameters = mode == prism::MaterialOptions::CompileMode::Auto;
}

void MaterialService::SetParallelTexturePreparation(bool parallel)
{
    m_impl->parallelTexturePreparation = parallel;
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
            TF_WARN("Material resource invalidation failed for '%s': %s",
                    resource.c_str(), error.what());
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
    result.foldedCompilations = m_impl->foldedCompilations.load();
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
    const auto prewarm = m_impl->FindPrewarm();
    if (prewarm.valid() &&
        prewarm.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        result.prewarmSeconds = prewarm.get().seconds;
    result.prewarmWaitSeconds = m_impl->prewarmWaitNanoseconds.load() / 1.0e9;
    result.prewarmJobs = gPrewarmJobs.load();
    return result;
}

} // namespace prism::materials
