#pragma once

#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include "../texture_service.h"
#include "udim_resource.h"
#include <MaterialXCore/Document.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>
#include <mutex>

namespace prism::materialx {
PXR_NAMESPACE_USING_DIRECTIVE

struct CapturedParameter {
    TfToken name;
    HdMaterialNetworkInterface::NodeParamData data;
    bool operator==(const CapturedParameter& rhs) const {
        return name == rhs.name && data.value == rhs.data.value &&
            data.colorSpace == rhs.data.colorSpace && data.typeName == rhs.data.typeName;
    }
};
struct CapturedConnection {
    TfToken input;
    HdMaterialNetworkInterface::InputConnectionVector connections;
    bool operator==(const CapturedConnection& rhs) const {
        if (input != rhs.input || connections.size() != rhs.connections.size()) return false;
        for (size_t i = 0; i < connections.size(); ++i)
            if (connections[i].upstreamNodeName != rhs.connections[i].upstreamNodeName ||
                connections[i].upstreamOutputName != rhs.connections[i].upstreamOutputName)
                return false;
        return true;
    }
};
struct CapturedNode {
    TfToken name, type;
    // Original frontend node path used only to map stock hdMtlx MaterialX
    // ports back to this topology-canonical node. Deliberately excluded from
    // equality so renamed, otherwise identical graphs remain compatible.
    TfToken sourceName;
    std::vector<std::pair<TfToken, VtValue>> typeInfo;
    std::vector<CapturedParameter> parameters;
    std::vector<CapturedConnection> connections;
    bool operator==(const CapturedNode& rhs) const {
        return name == rhs.name && type == rhs.type && typeInfo == rhs.typeInfo &&
            parameters == rhs.parameters && connections == rhs.connections;
    }
};
struct CapturedMaterialGraph {
    TfToken renderContext;
    std::string modelAssetName;
    std::vector<std::pair<TfToken, VtValue>> config;
    std::vector<CapturedNode> nodes;
    std::vector<std::pair<TfToken, HdMaterialNetworkInterface::InputConnection>> terminals;
    bool operator==(const CapturedMaterialGraph& rhs) const {
        if (renderContext != rhs.renderContext || modelAssetName != rhs.modelAssetName ||
            config != rhs.config || nodes != rhs.nodes || terminals.size() != rhs.terminals.size())
            return false;
        for (size_t i = 0; i < terminals.size(); ++i)
            if (terminals[i].first != rhs.terminals[i].first ||
                terminals[i].second.upstreamNodeName != rhs.terminals[i].second.upstreamNodeName ||
                terminals[i].second.upstreamOutputName != rhs.terminals[i].second.upstreamOutputName)
                return false;
        return true;
    }
};
struct CapturedFileDependency {
    std::filesystem::path path;
    uintmax_t size = 0;
    std::filesystem::file_time_type writeTime;
    std::string sourceIdentity;
    bool operator==(const CapturedFileDependency&) const = default;
};

inline TfToken CanonicalMaterialNodeName(const TfToken& name,
                                         const SdfPath& materialPath) {
    const SdfPath path(name.GetString());
    if (!path.IsEmpty() && path.HasPrefix(materialPath)) {
        const SdfPath relative = path.MakeRelativePath(materialPath);
        return TfToken(relative.IsEmpty() ? std::string(".") : relative.GetString());
    }
    return name;
}

inline CapturedMaterialGraph CaptureMaterialGraph(HdMaterialNetworkInterface& net,
                                                   const TfToken& context,
                                                   const SdfPath& materialPath) {
    (void)materialPath; // Connected local nodes are identified by topology.
    CapturedMaterialGraph result;
    result.renderContext = context;
    result.modelAssetName = net.GetModelAssetName();
    for (const TfToken& key : net.GetMaterialConfigKeys())
        result.config.emplace_back(key, net.GetMaterialConfigValue(key));
    std::sort(result.config.begin(), result.config.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    TfTokenVector names = net.GetNodeNames();
    std::sort(names.begin(), names.end());
    std::map<TfToken, TfToken> canonicalNames;
    TfTokenVector traversal;
    traversal.reserve(names.size());
    auto isInternal = [&](const TfToken& name) {
        return std::binary_search(names.begin(), names.end(), name);
    };
    std::function<void(const TfToken&)> visit = [&](const TfToken& original) {
        if (!isInternal(original) || canonicalNames.contains(original)) return;
        // Publish the ID before following inputs so cycles terminate. Sorted
        // terminal/input names plus authored connection order define traversal;
        // USD prim paths and node names do not.
        canonicalNames.emplace(original,
            TfToken("n" + std::to_string(canonicalNames.size())));
        traversal.push_back(original);
        TfTokenVector inputs = net.GetNodeInputConnectionNames(original);
        std::sort(inputs.begin(), inputs.end());
        for (const TfToken& input : inputs)
            for (const auto& connection : net.GetNodeInputConnection(original, input))
                visit(connection.upstreamNodeName);
    };
    TfTokenVector terminalNames = net.GetTerminalNames();
    std::sort(terminalNames.begin(), terminalNames.end());
    for (const TfToken& name : terminalNames) {
        auto connection = net.GetTerminalConnection(name);
        if (connection.first) visit(connection.second.upstreamNodeName);
    }
    // Disconnected nodes have no topology-rooted identity. Retain their names
    // below so two unrelated hidden subgraphs cannot alias accidentally.
    for (const TfToken& name : names)
        if (!canonicalNames.contains(name)) traversal.push_back(name);
    auto canonicalNode = [&](const TfToken& name) {
        const auto found = canonicalNames.find(name);
        return found != canonicalNames.end() ? found->second
            : name;
    };
    for (const TfToken& original : traversal) {
        CapturedNode node;
        node.name = canonicalNode(original);
        node.type = net.GetNodeType(original);
        node.sourceName = original;
        for (const TfToken& key : net.GetNodeTypeInfoKeys(original))
            node.typeInfo.emplace_back(key, net.GetNodeTypeInfoValue(original, key));
        std::sort(node.typeInfo.begin(), node.typeInfo.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        TfTokenVector params = net.GetAuthoredNodeParameterNames(original);
        std::sort(params.begin(), params.end());
        for (const TfToken& name : params) {
            // HdMaterialNetwork2Interface exposes legacy metadata storage as
            // synthetic parameters. The scene-index interface exposes the
            // same information only through NodeParamData. Capture the latter
            // representation in both cases so frontend choice is not identity.
            const std::string& text = name.GetString();
            if (text.starts_with("typeName:") ||
                text.starts_with("colorSpace:")) continue;
            node.parameters.push_back({name, net.GetNodeParameterData(original, name)});
        }
        TfTokenVector inputs = net.GetNodeInputConnectionNames(original);
        std::sort(inputs.begin(), inputs.end());
        for (const TfToken& input : inputs) {
            auto connections = net.GetNodeInputConnection(original, input);
            for (auto& connection : connections)
                connection.upstreamNodeName = canonicalNode(
                    connection.upstreamNodeName);
            node.connections.push_back({input, std::move(connections)});
        }
        result.nodes.push_back(std::move(node));
    }
    for (const TfToken& name : terminalNames) {
        auto connection = net.GetTerminalConnection(name);
        if (!connection.first) continue;
        connection.second.upstreamNodeName = canonicalNode(
            connection.second.upstreamNodeName);
        result.terminals.emplace_back(name, std::move(connection.second));
    }
    return result;
}

inline bool CaptureFileDependencies(const CapturedMaterialGraph& graph,
                                    std::vector<CapturedFileDependency>* result,
                                    const std::string& resolverIdentity = {},
                                    const std::string& sourceVersion = {}) {
    ArResolver& resolver = ArGetResolver();
    std::shared_ptr<prism::TextureService> textures;
    for (const auto& node : graph.nodes) for (const auto& param : node.parameters) {
        const bool resource = param.name == TfToken("file") ||
                              param.name == TfToken("filename");
        if (!resource) {
            if (param.data.value.IsHolding<SdfAssetPath>()) return false;
            continue;
        }
        std::string authored, resolved;
        if (param.data.value.IsHolding<SdfAssetPath>()) {
            const auto& asset = param.data.value.UncheckedGet<SdfAssetPath>();
            authored = asset.GetAssetPath(); resolved = asset.GetResolvedPath();
        } else if (param.data.value.IsHolding<std::string>()) {
            authored = param.data.value.UncheckedGet<std::string>();
        } else return false;
        if (authored.find("<UDIM>") != std::string::npos ||
            resolved.find("<UDIM>") != std::string::npos) {
            const auto tiles = ResolveUdimSourceTiles(
                authored, resolved, resolver.GetCurrentContext());
            if (tiles.empty()) return false;
            for (const auto& tile : tiles) {
                std::error_code error;
                CapturedFileDependency dependency;
                dependency.path = std::filesystem::path(tile.resolved.GetPathString());
                dependency.size = std::filesystem::file_size(dependency.path, error);
                if (error) return false;
                dependency.writeTime = std::filesystem::last_write_time(dependency.path, error);
                if (error) return false;
                prism::TexturePrepareOptions options;
                options.resolverIdentity = resolverIdentity;
                options.sourceVersion = sourceVersion;
                if (!textures) textures = prism::TextureService::Acquire();
                dependency.sourceIdentity = textures->sourceIdentity(dependency.path, options);
                result->push_back(std::move(dependency));
            }
            continue;
        }
        const ArResolvedPath path = !resolved.empty() ? ArResolvedPath(resolved)
                                                       : resolver.Resolve(authored);
        if (path.empty()) return false;
        std::error_code error;
        CapturedFileDependency dependency;
        dependency.path = std::filesystem::path(path.GetPathString());
        dependency.size = std::filesystem::file_size(dependency.path, error);
        if (error) return false;
        dependency.writeTime = std::filesystem::last_write_time(dependency.path, error);
        if (error) return false;
        prism::TexturePrepareOptions options;
        options.resolverIdentity = resolverIdentity;
        options.sourceVersion = sourceVersion;
        if (!textures) textures = prism::TextureService::Acquire();
        dependency.sourceIdentity = textures->sourceIdentity(dependency.path, options);
        result->push_back(std::move(dependency));
    }
    return true;
}

// Frontend-neutral, bounded conversion registry. Adapters retain their own
// ingestion logic, but an exact owned graph converted by either frontend can
// supply the immutable MaterialX document/XML to the other before stock
// hdMtlx conversion runs.
struct CapturedConversionArtifact {
    std::string libraries;
    std::string resolverIdentity;
    ArResolverContext resolverContext;
    std::string sourceVersion;
    CapturedMaterialGraph graph;
    std::vector<CapturedFileDependency> dependencies;
    MaterialX::ConstDocumentPtr document;
    std::string xml;
    std::string materialName;
};

class CapturedMaterialRegistry {
    struct ConversionJob {
        CapturedConversionArtifact key;
        uint64_t generation = 0;
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        bool success = false;
        CapturedConversionArtifact result;
    };
public:
    struct Stats { uint64_t stockConversions = 0; };
    class ConversionTicket {
    public:
        ConversionTicket() = default;
        ConversionTicket(const ConversionTicket&) = delete;
        ConversionTicket& operator=(const ConversionTicket&) = delete;
        ConversionTicket(ConversionTicket&& rhs) noexcept
            : m_registry(rhs.m_registry), m_job(std::move(rhs.m_job)),
              m_owner(rhs.m_owner), m_active(rhs.m_active) {
            rhs.m_registry = nullptr; rhs.m_owner = rhs.m_active = false;
        }
        ConversionTicket& operator=(ConversionTicket&& rhs) noexcept {
            if (this != &rhs) {
                fail();
                m_registry=rhs.m_registry; m_job=std::move(rhs.m_job);
                m_owner=rhs.m_owner; m_active=rhs.m_active;
                rhs.m_registry=nullptr; rhs.m_owner=rhs.m_active=false;
            }
            return *this;
        }
        ~ConversionTicket() { fail(); }
        bool isOwner() const { return m_owner; }
        bool wait(CapturedConversionArtifact* result) {
            if (!m_job || m_owner) return false;
            std::unique_lock lock(m_job->mutex);
            m_job->ready.wait(lock,[&] { return m_job->done; });
            if (m_job->success && result) *result=m_job->result;
            m_active=false;
            return m_job->success;
        }
        bool publish(CapturedConversionArtifact artifact) {
            if (!m_registry || !m_job || !m_owner || !m_active) return false;
            const bool result=m_registry->_Complete(m_job,std::move(artifact),true);
            m_active=false;
            return result;
        }
        void fail() {
            if (m_registry && m_job && m_owner && m_active)
                m_registry->_Complete(m_job,{},false);
            m_active=false;
        }
    private:
        friend class CapturedMaterialRegistry;
        ConversionTicket(CapturedMaterialRegistry* registry,
                         std::shared_ptr<ConversionJob> job,bool owner)
            : m_registry(registry),m_job(std::move(job)),m_owner(owner),m_active(true) {}
        CapturedMaterialRegistry* m_registry = nullptr;
        std::shared_ptr<ConversionJob> m_job;
        bool m_owner = false, m_active = false;
    };

    static CapturedMaterialRegistry& Shared() {
        static auto* registry = new CapturedMaterialRegistry;
        return *registry;
    }
    bool find(const CapturedConversionArtifact& key,
              CapturedConversionArtifact* result) const {
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            if (it->libraries == key.libraries &&
                it->resolverIdentity == key.resolverIdentity &&
                it->resolverContext == key.resolverContext &&
                it->sourceVersion == key.sourceVersion &&
                it->graph == key.graph && it->dependencies == key.dependencies) {
                *result = *it;
                return true;
            }
        }
        return false;
    }
    ConversionTicket acquire(const CapturedConversionArtifact& key) {
        std::lock_guard lock(m_mutex);
        // find() is only a fast path: another owner can publish before the
        // caller reaches acquire(). Recheck completed entries under the same
        // lock used to register jobs so that this race cannot repeat conversion.
        for (auto it=m_entries.rbegin(); it!=m_entries.rend(); ++it) {
            if (!_SameKey(*it,key)) continue;
            auto completed=std::make_shared<ConversionJob>();
            completed->done=true;
            completed->success=true;
            completed->result=*it;
            return ConversionTicket(this,std::move(completed),false);
        }
        for (const auto& job : m_jobs)
            if (_SameKey(job->key,key)) return ConversionTicket(this,job,false);
        auto job=std::make_shared<ConversionJob>();
        job->key=key; job->generation=m_generation;
        m_jobs.push_back(job);
        return ConversionTicket(this,std::move(job),true);
    }
    void recordStockConversion() {
        m_stockConversions.fetch_add(1,std::memory_order_relaxed);
    }
    Stats stats() const {
        return {m_stockConversions.load(std::memory_order_relaxed)};
    }
    void resetStats() { m_stockConversions.store(0,std::memory_order_relaxed); }
    void publish(CapturedConversionArtifact artifact) {
        std::lock_guard lock(m_mutex);
        _PublishUnlocked(std::move(artifact));
    }
    void clear() {
        std::vector<std::shared_ptr<ConversionJob>> jobs;
        {
            std::lock_guard lock(m_mutex);
            ++m_generation;
            m_entries.clear();
            jobs.swap(m_jobs);
        }
        for (const auto& job : jobs) {
            std::lock_guard lock(job->mutex);
            job->done=true; job->success=false;
            job->ready.notify_all();
        }
    }
private:
    static bool _SameKey(const CapturedConversionArtifact& a,
                         const CapturedConversionArtifact& b) {
        return a.libraries==b.libraries &&
            a.resolverIdentity==b.resolverIdentity &&
            a.resolverContext==b.resolverContext &&
            a.sourceVersion==b.sourceVersion && a.graph==b.graph &&
            a.dependencies==b.dependencies;
    }
    void _PublishUnlocked(CapturedConversionArtifact artifact) {
        std::erase_if(m_entries,[&](const auto& entry) {
            return _SameKey(entry,artifact);
        });
        if (m_entries.size() >= 2048) m_entries.erase(m_entries.begin());
        m_entries.push_back(std::move(artifact));
    }
    bool _Complete(const std::shared_ptr<ConversionJob>& job,
                   CapturedConversionArtifact artifact,bool success) {
        {
            std::lock_guard lock(m_mutex);
            const auto found=std::find(m_jobs.begin(),m_jobs.end(),job);
            if (found==m_jobs.end() || job->generation!=m_generation) return false;
            if (success && !_SameKey(job->key,artifact)) success=false;
            // Complete the job while holding the registry lock. This gives
            // clear() a strict linearization point: it either cancels this job
            // first, or observes its already-published result and clears it.
            std::lock_guard jobLock(job->mutex);
            if (job->done) return false;
            m_jobs.erase(found);
            if (success) _PublishUnlocked(artifact);
            job->success=success;
            if (success) job->result=std::move(artifact);
            job->done=true;
        }
        job->ready.notify_all();
        return success;
    }
    mutable std::mutex m_mutex;
    std::vector<CapturedConversionArtifact> m_entries;
    std::vector<std::shared_ptr<ConversionJob>> m_jobs;
    uint64_t m_generation = 0;
    std::atomic<uint64_t> m_stockConversions{0};
};
}
