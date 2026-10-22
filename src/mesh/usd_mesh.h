#pragma once

// STD Lib deps
#include <iostream>
#include <fstream>
#include <atomic>
#include <OpenImageIO/hash.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <array>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <mutex>

// Open USD Deps
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/stringUtils.h>

// Hydra / hdMtlx deps for UsdShade -> MaterialX recovery
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/usdImaging/usdImaging/materialParamUtils.h>

// MaterialX deps
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include "../materialx/captured_material.h"

PXR_NAMESPACE_USING_DIRECTIVE
namespace mx = MaterialX;

// internal deps
#include "../geometry.h"
#include "../hydra/fallback_material.h"
#include "../mdl_material.h"
#include "../materialx/usd_network.h"
#include "../materialx/prepared_template.h"

// ============================================================================
// MaterialX material recovery from USD stages
// ============================================================================

// The material bound for the first of `purposes` with a binding (the empty
// token is allPurpose), as Hydra resolves UsdRenderSettings
// materialBindingPurposes. Empty `purposes`: full, then allPurpose.
inline UsdShadeMaterial ComputeBoundMaterialFor(UsdPrim const& prim,
                                                std::vector<TfToken> const& purposes)
{
    static const std::vector<TfToken> defaults{UsdShadeTokens->full, UsdShadeTokens->allPurpose};
    for (TfToken const& purpose : purposes.empty() ? defaults : purposes) {
        UsdRelationship binding;
        UsdShadeMaterial material = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(purpose, &binding);
        if (!material) continue;
        if (purpose.IsEmpty()) return material;
        // ComputeBoundMaterial falls back to allPurpose bindings; here only
        // bindings of this purpose count (material:binding:<purpose> or
        // material:binding:collection:<purpose>:<name>).
        const std::vector<std::string> parts = TfStringSplit(binding.GetName(), ":");
        const bool collection = parts.size() > 2 && parts[2] == "collection";
        const std::string bound = collection ? (parts.size() == 5 ? parts[3] : std::string())
                                             : (parts.size() == 3 ? parts[2] : std::string());
        if (bound == purpose.GetString()) return material;
    }
    return UsdShadeMaterial(UsdPrim());
}

/// Describes how a MaterialX material was found in the USD stage.
inline bool UsdMaterialVerbose() {
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string_view(value) != "0";
}

enum class MtlxSourceKind {
    ReferencedFile,   // A .mtlx file directly referenced into the stage
    UsdShadeEncoded   // MaterialX nodes encoded as a UsdShade network
};

/// Result of resolving a single material prim's MaterialX content.
struct ResolvedMtlxMaterial {
    SdfPath             materialPath;
    MtlxSourceKind      sourceKind;
    std::string         mtlxFilePath;     // non-empty only for ReferencedFile
    mx::DocumentPtr     document;         // recovered mx::Document (for UsdShade)
    std::string         materialName;     // name of the material node inside the doc
    prism::materialx::CapturedMaterialGraph capturedGraph;
    prism::materialx::CapturedMaterialGraph conversionGraph;
    std::vector<prism::materialx::CapturedFileDependency> dependencies;
    ArResolverContext resolverContext;
    std::string sourceVersion;
    std::shared_ptr<HdMaterialNetwork2> capturedNetwork;
    SdfPath terminalNodePath;
    mx::DocumentPtr libraries;
    bool conversionReusable = false;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Check if a material prim's subtree was composed from a .mtlx file
/// reference. If so, return the resolved file path.
static std::optional<std::string>
FindReferencedMtlxFile(const UsdPrim& materialPrim)
{
    auto primSpec = materialPrim.GetPrimStack();
    for (const auto& spec : primSpec) {
        SdfReferenceListOp refs;
        if (spec->HasField(SdfFieldKeys->References, &refs)) {

            auto processRefs = [&](const std::vector<SdfReference>& refList) -> std::optional<std::string> {
                for (const SdfReference& ref : refList) {
                    const std::string& assetPath = ref.GetAssetPath();
                    if (assetPath.ends_with(".mtlx")) {
                        SdfLayerHandle layer = spec->GetLayer();
                        if (layer) {
                            std::string resolved = layer->ComputeAbsolutePath(assetPath);
                            if (!resolved.empty()) return resolved;
                        }
                        return assetPath;
                    }
                }
                return std::nullopt;
            };

            if (auto r = processRefs(refs.GetExplicitItems()))  return r;
            if (auto r = processRefs(refs.GetPrependedItems())) return r;
            if (auto r = processRefs(refs.GetAppendedItems()))  return r;
        }
    }
    return std::nullopt;
}

/// Walk up the prim's ancestor chain looking for a .mtlx reference.
/// The reference is typically authored on a container Scope above the
/// individual material prims (e.g. /Pawn/Materials rather than
/// /Pawn/Materials/M_Pawn_Body_B).
static std::optional<std::string>
FindReferencedMtlxFileInAncestors(const UsdPrim& prim)
{
    UsdPrim current = prim;
    while (current.IsValid()) {
        if (auto result = FindReferencedMtlxFile(current))
            return result;
        current = current.GetParent();
    }
    return std::nullopt;
}

/// Check whether a material prim has MaterialX-originated UsdShade nodes
/// by looking at the "mtlx" render context surface output.
static bool
HasMtlxSurfaceOutput(const UsdShadeMaterial& material)
{
    const TfTokenVector contextVector = { TfToken("mtlx") };
    UsdShadeShader surfaceSource = material.ComputeSurfaceSource(contextVector);
    return static_cast<bool>(surfaceSource);
}

// ---------------------------------------------------------------------------
// Single-material conversion via Hydra hdMtlx
// ---------------------------------------------------------------------------

static mx::DocumentPtr
RecoverMtlxDocumentFromUsdShade(const UsdPrim& materialPrim,
                                const mx::DocumentPtr& mtlxLibraries,
                                const std::string& librariesPath,
                                prism::materialx::CapturedMaterialGraph* capturedOut=nullptr,
                                std::vector<prism::materialx::CapturedFileDependency>* dependenciesOut=nullptr)
{
    const TfToken mtlxContext("mtlx");
    const TfTokenVector contextVector    = { mtlxContext };
    const TfTokenVector shaderSourceTypes = { mtlxContext };

    SdfPath materialPath = materialPrim.GetPath();
    const UsdShadeShader surface = UsdShadeMaterial(materialPrim).ComputeSurfaceSource(contextVector);
    if (!surface)
        return nullptr;

    HdMaterialNetworkMap networkMap;
    UsdImagingBuildHdMaterialNetworkFromTerminal(
        surface.GetPrim(),
        HdMaterialTerminalTokens->surface,
        shaderSourceTypes,
        contextVector,
        &networkMap,
        UsdTimeCode::Default()
    );

    bool isVolume = false;
    HdMaterialNetwork2 network2 =
        HdConvertToHdMaterialNetwork2(networkMap, &isVolume);

    auto surfaceTerminalIt = network2.terminals.find(HdMaterialTerminalTokens->surface);
    if (surfaceTerminalIt == network2.terminals.end()) {
        std::cerr << "[MtlxRecover] No surface terminal for " << materialPath << "\n";
        return nullptr;
    }

    const SdfPath& terminalNodePath = surfaceTerminalIt->second.upstreamNode;
    auto terminalNodeIt = network2.nodes.find(terminalNodePath);
    if (terminalNodeIt == network2.nodes.end()) {
        std::cerr << "[MtlxRecover] Terminal node not found for " << materialPath << "\n";
        return nullptr;
    }

    // Stage ingestion remains independent of Hydra scene synchronization, but
    // both frontends share conversion of simple parameter-only OpenPBR graphs.
    HdMaterialNetwork2Interface networkInterface(materialPath, &network2);
    const auto captured = prism::materialx::CaptureMaterialGraph(
        networkInterface, mtlxContext, materialPath);
    std::vector<prism::materialx::CapturedFileDependency> dependencies;
    const bool reusable = prism::materialx::CaptureFileDependencies(
        captured, &dependencies,
        ArGetResolver().GetCurrentContext().GetDebugString(), "0");
    if (capturedOut) *capturedOut=captured;
    if (dependenciesOut) *dependenciesOut=dependencies;
    prism::materialx::CapturedConversionArtifact sharedKey;
    std::optional<prism::materialx::CapturedMaterialRegistry::ConversionTicket>
        conversionTicket;
    if (reusable) {
        sharedKey.libraries = librariesPath;
        sharedKey.resolverIdentity = ArGetResolver().GetCurrentContext().GetDebugString();
        sharedKey.resolverContext = ArGetResolver().GetCurrentContext();
        sharedKey.sourceVersion = "0";
        sharedKey.graph = captured;
        sharedKey.dependencies = dependencies;
        prism::materialx::CapturedConversionArtifact shared;
        if (prism::materialx::CapturedMaterialRegistry::Shared().find(
                sharedKey, &shared)) {
            if (shared.document) return shared.document->copy();
            if (!shared.xml.empty()) {
                auto document = mx::createDocument();
                mx::readFromXmlString(document, shared.xml);
                document->setDataLibrary(mtlxLibraries);
                return document;
            }
        }
        conversionTicket.emplace(
            prism::materialx::CapturedMaterialRegistry::Shared().acquire(sharedKey));
        if (!conversionTicket->isOwner()) {
            prism::materialx::CapturedConversionArtifact shared;
            if (!conversionTicket->wait(&shared)) return nullptr;
            if (shared.document) return shared.document->copy();
            if (!shared.xml.empty()) {
                auto document=mx::createDocument();
                mx::readFromXmlString(document,shared.xml);
                document->setDataLibrary(mtlxLibraries);
                return document;
            }
            return nullptr;
        }
    }
    if (auto simple = prism::materialx::SimpleOpenPbrDocument(
            networkInterface, terminalNodePath.GetToken(), mtlxLibraries)) {
        if (reusable) {
            auto shared = sharedKey;
            shared.document = simple;
            shared.xml = prism::materialx::SerializeDocument(simple, librariesPath);
            const auto materials = simple->getMaterialNodes();
            if (!materials.empty()) shared.materialName = materials.front()->getName();
            if (!conversionTicket || !conversionTicket->publish(std::move(shared)))
                return nullptr;
        }
        return simple;
    }

    HdMtlxTexturePrimvarData mxHdData;
    auto converted = HdMtlxCreateMtlxDocumentFromHdNetwork(
        network2,
        terminalNodeIt->second,
        terminalNodePath,
        materialPath,
        mtlxLibraries,
        &mxHdData
    );
    prism::materialx::ShortenHdMtlxNetworkNodeNames(converted, network2.nodes);
    if (converted && reusable) {
        auto shared = sharedKey;
        shared.document = converted;
        shared.xml = prism::materialx::SerializeDocument(converted, librariesPath);
        const auto materials = converted->getMaterialNodes();
        if (!materials.empty()) shared.materialName = materials.front()->getName();
        if (!conversionTicket || !conversionTicket->publish(std::move(shared)))
            return nullptr;
    }
    return converted;
}

// ---------------------------------------------------------------------------
// Batch collection: all MaterialX materials in a stage
// ---------------------------------------------------------------------------

static std::map<SdfPath, ResolvedMtlxMaterial>
CollectMaterialXMaterials(const UsdStageRefPtr& stage,
                          const std::string& mx_libraries_path,
                          std::vector<TfToken> const& bindingPurposes = {})
{
    std::map<SdfPath, ResolvedMtlxMaterial> result;

    // Same stdlib instance the material compilations below will use.
    mx::DocumentPtr mtlxStdLibraries = MDLGenerator::shared(mx_libraries_path)->get_stdlib();

    // Discover bindings on supported geometry first. Instance proxies and
    // point-instancer prototype subtrees remain included; no camera/frustum
    // test is involved, so offscreen shadow/reflection materials stay live.
    std::set<SdfPath> required;
    for (const UsdPrim& geometry : UsdPrimRange(stage->GetPseudoRoot(), UsdTraverseInstanceProxies())) {
        if (!geometry.IsA<UsdGeomMesh>() && !geometry.IsA<UsdGeomSphere>()) continue;
        if (geometry.IsA<UsdGeomSphere>()) {
            UsdGeomSphere sphere(geometry);
            if (sphere.ComputeVisibility() == UsdGeomTokens->invisible) continue;
            const TfToken purpose = sphere.ComputePurpose();
            if (purpose == UsdGeomTokens->guide || purpose == UsdGeomTokens->proxy) continue;
        }
        if (auto material = ComputeBoundMaterialFor(geometry, bindingPurposes))
            required.insert(material.GetPath());
    }
    for (const SdfPath& materialPath : required) {
        const UsdPrim prim = stage->GetPrimAtPath(materialPath);
        UsdShadeMaterial material(prim);
        if (!material) continue;

        // --- Case 1: Direct .mtlx file reference ---
        auto mtlxFile = FindReferencedMtlxFileInAncestors(prim);
        if (mtlxFile.has_value()) {
            ResolvedMtlxMaterial resolved;
            resolved.materialPath = materialPath;
            resolved.sourceKind   = MtlxSourceKind::ReferencedFile;
            resolved.mtlxFilePath = mtlxFile.value();
            resolved.materialName = materialPath.GetName();

            if (UsdMaterialVerbose()) std::cout << "[MtlxCollect] Found referenced .mtlx: "
                      << resolved.mtlxFilePath << " at " << materialPath << "\n";

            result[materialPath] = std::move(resolved);
            continue;
        }

        // --- Case 2: UsdShade-encoded MaterialX ---
        if (HasMtlxSurfaceOutput(material)) {
            ResolvedMtlxMaterial resolved;
            const TfToken mtlxContext("mtlx");
            const TfTokenVector contexts{mtlxContext};
            const UsdShadeShader surface=material.ComputeSurfaceSource(contexts);
            if (!surface) continue;
            HdMaterialNetworkMap networkMap;
            UsdImagingBuildHdMaterialNetworkFromTerminal(surface.GetPrim(),
                HdMaterialTerminalTokens->surface,contexts,contexts,&networkMap,
                UsdTimeCode::Default());
            bool isVolume=false;
            auto network=std::make_shared<HdMaterialNetwork2>(
                HdConvertToHdMaterialNetwork2(networkMap,&isVolume));
            const auto terminal=network->terminals.find(HdMaterialTerminalTokens->surface);
            if (terminal==network->terminals.end() ||
                !network->nodes.contains(terminal->second.upstreamNode)) continue;
            HdMaterialNetwork2Interface networkInterface(materialPath,network.get());
            resolved.capturedGraph=prism::materialx::CaptureMaterialGraph(
                networkInterface,mtlxContext,materialPath);
            resolved.conversionGraph=resolved.capturedGraph;
            resolved.conversionReusable=prism::materialx::CaptureFileDependencies(
                resolved.capturedGraph,&resolved.dependencies,
                stage->GetPathResolverContext().GetDebugString(),"0");
            if (!resolved.conversionReusable)
                resolved.dependencies.clear();

            resolved.materialPath = materialPath;
            resolved.sourceKind   = MtlxSourceKind::UsdShadeEncoded;
            resolved.resolverContext=stage->GetPathResolverContext();
            resolved.sourceVersion="0";
            resolved.capturedNetwork=std::move(network);
            resolved.terminalNodePath=terminal->second.upstreamNode;
            resolved.libraries=mtlxStdLibraries;
            resolved.materialName="PrismMaterial";

            if (UsdMaterialVerbose()) std::cout << "[MtlxCollect] Recovered UsdShade-encoded MaterialX: "
                      << materialPath << " (material: '"
                      << resolved.materialName << "')\n";

            result[materialPath] = std::move(resolved);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Compile a single resolved material into an MDLMaterial
// ---------------------------------------------------------------------------

static std::shared_ptr<MDLMaterial>
CompileMtlxMaterial(const ResolvedMtlxMaterial& resolved,
                    mdl::Context& mdl_context,
                    const std::string& mx_libraries_path,
                    const std::string& mx_mdl_path)
{
    if (resolved.sourceKind == MtlxSourceKind::ReferencedFile) {
        return std::make_shared<MDLMaterial>(
            resolved.mtlxFilePath,
            mdl_context,
            mx_libraries_path,
            mx_mdl_path,
            resolved.materialName
        );
    }

    // Use immutable content names: material edits and concurrent CLI processes
    // must not overwrite a document still referenced by an MDL transaction.
    std::filesystem::path cache;
    if (const char* configured = std::getenv("PRISM_CACHE_DIR"); configured && *configured)
        cache = configured;
    else if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        cache = std::filesystem::path(local) / "PrismRT" / "cache";
    else
        cache = std::filesystem::temp_directory_path() / "prism-rt";
    cache /= "materials";
    prism::materialx::CapturedMaterialGraph templateStructure;
    const bool templateEligible=resolved.sourceKind==MtlxSourceKind::UsdShadeEncoded &&
        prism::materialx::CompatibleTemplateStructure(
            resolved.capturedGraph,&templateStructure);
    if (templateEligible) {
        prism::materialx::CompatibleMaterialTemplate prepared;
        if (prism::materialx::CompatibleTemplateRegistry::Shared().find(
                &mdl_context,mx_libraries_path,mx_mdl_path,
                templateStructure,&prepared)) {
            prism::materialx::CompatibleBindResult bound;
            try {
                bound=prism::materialx::BindCompatibleTemplate(
                    prepared,resolved.capturedGraph,mdl_context,
                    resolved.resolverContext,resolved.sourceVersion,cache);
            } catch (const std::exception& error) { bound.error=error.what(); }
            if (bound.material && bound.material->is_valid()) return bound.material;
            if (UsdMaterialVerbose())
                std::cerr << "[CompileMtlx] Template binding rejected for "
                          << resolved.materialPath << ": " << bound.error << "\n";
        }
    }
    mx::DocumentPtr document=resolved.document;
    auto conversionGraph=resolved.conversionGraph.nodes.empty()
        ? resolved.capturedGraph : resolved.conversionGraph;
    if (!document && resolved.capturedNetwork && resolved.libraries) {
        prism::materialx::CapturedConversionArtifact key;
        key.libraries=mx_libraries_path;
        key.resolverIdentity=resolved.resolverContext.GetDebugString();
        key.resolverContext=resolved.resolverContext;
        key.sourceVersion=resolved.sourceVersion;
        key.graph=resolved.capturedGraph;
        key.dependencies=resolved.dependencies;
        auto& registry=prism::materialx::CapturedMaterialRegistry::Shared();
        prism::materialx::CapturedConversionArtifact shared;
        if (resolved.conversionReusable && registry.find(key,&shared)) {
            conversionGraph=shared.graph;
            if (shared.document) document=shared.document->copy();
            else if (!shared.xml.empty()) {
                document=mx::createDocument(); mx::readFromXmlString(document,shared.xml);
                document->setDataLibrary(resolved.libraries);
            }
        }
        std::optional<prism::materialx::CapturedMaterialRegistry::ConversionTicket> ticket;
        if (!document && resolved.conversionReusable) {
            ticket.emplace(registry.acquire(key));
            if (!ticket->isOwner()) {
                if (!ticket->wait(&shared)) return nullptr;
                conversionGraph=shared.graph;
                if (shared.document) document=shared.document->copy();
                else if (!shared.xml.empty()) {
                    document=mx::createDocument(); mx::readFromXmlString(document,shared.xml);
                    document->setDataLibrary(resolved.libraries);
                }
            }
        }
        if (!document) {
            auto conversionNetwork = std::make_shared<HdMaterialNetwork2>(
                *resolved.capturedNetwork);
            for (auto& [nodePath, node] : conversionNetwork->nodes) {
                for (auto& [name, value] : node.parameters) {
                    if (name != TfToken("file") && name != TfToken("filename")) continue;
                    std::string authored, resolvedPattern;
                    if (value.IsHolding<SdfAssetPath>()) {
                        const auto& asset = value.UncheckedGet<SdfAssetPath>();
                        authored = asset.GetAssetPath();
                        resolvedPattern = asset.GetResolvedPath();
                    } else if (value.IsHolding<std::string>()) {
                        authored = value.UncheckedGet<std::string>();
                    }
                    const std::string extension=
                        std::filesystem::path(authored).extension().string();
                    if (authored.find("<UDIM>") != std::string::npos ||
                        resolvedPattern.find("<UDIM>") != std::string::npos) {
                        auto family = prism::materialx::PrepareMaterialUdimResource(
                            authored, resolvedPattern, resolved.resolverContext,
                            resolved.sourceVersion, cache, extension);
                        if (!family.mdlPath.empty())
                            value = VtValue(SdfAssetPath(family.mdlPath.substr(1)));
                        continue;
                    }
                    PXR_NS::ArResolverContextBinder binder(resolved.resolverContext);
                    const auto source = !resolvedPattern.empty()
                        ? PXR_NS::ArResolvedPath(resolvedPattern)
                        : PXR_NS::ArGetResolver().Resolve(authored);
                    if (source.empty()) continue;
                    auto resource=prism::materialx::PrepareMaterialResource(
                        source,resolved.resolverContext,resolved.sourceVersion,
                        cache,extension.empty()?".bin":extension);
                    if (resource.resource)
                        value=VtValue(SdfAssetPath(
                            resource.resource->path.filename().generic_string()));
                }
            }
            HdMaterialNetwork2Interface networkInterface(
                resolved.materialPath,conversionNetwork.get());
            document=prism::materialx::SimpleOpenPbrDocument(
                networkInterface,resolved.terminalNodePath.GetToken(),resolved.libraries);
            if (!document) {
                const auto node=conversionNetwork->nodes.find(resolved.terminalNodePath);
                if (node!=conversionNetwork->nodes.end()) {
                    registry.recordStockConversion();
                    HdMtlxTexturePrimvarData data;
                    document=HdMtlxCreateMtlxDocumentFromHdNetwork(
                        *conversionNetwork,node->second,resolved.terminalNodePath,
                        resolved.materialPath,resolved.libraries,&data);
                    prism::materialx::ShortenHdMtlxNetworkNodeNames(
                        document,conversionNetwork->nodes);
                }
            }
            if (document) {
                const auto materials=document->getMaterialNodes();
                if (!materials.empty()) materials.front()->setName("PrismMaterial");
            }
            if (document && ticket) {
                shared=key; shared.document=document;
                shared.xml=prism::materialx::SerializeDocument(document,mx_libraries_path);
                shared.materialName="PrismMaterial";
                if (!ticket->publish(std::move(shared))) return nullptr;
            }
        }
    }
    if (!document) {
        std::cerr << "[CompileMtlx] No document for " << resolved.materialPath << "\n";
        return nullptr;
    }
    const std::string xml = prism::materialx::SerializeDocument(document, mx_libraries_path);
    struct ExactCompiled {
        mdl::Context* context = nullptr;
        std::string libraries, mdlPath, xml, materialName;
        std::weak_ptr<MDLMaterial> material;
    };
    static std::mutex compiledMutex;
    static std::vector<ExactCompiled> compiled;
    {
        std::lock_guard lock(compiledMutex);
        for (auto it = compiled.rbegin(); it != compiled.rend(); ++it) {
            if (it->context == &mdl_context && it->libraries == mx_libraries_path &&
                it->mdlPath == mx_mdl_path && it->xml == xml &&
                it->materialName == resolved.materialName) {
                if (auto material = it->material.lock()) return material;
            }
        }
    }
    const std::filesystem::path tempMtlx = cache /
        ("standalone_" + OIIO::SHA1::digest(xml.data(), xml.size()) + ".mtlx");
    try {
        std::filesystem::create_directories(cache);
        if (!std::filesystem::exists(tempMtlx)) {
            static std::atomic<unsigned long long> serial{0};
#ifdef _WIN32
            const auto process = _getpid();
#else
            const auto process = getpid();
#endif
            const auto staging = std::filesystem::path(tempMtlx.string() + ".tmp-" +
                std::to_string(process) + "-" + std::to_string(serial++));
            struct Cleanup {
                std::filesystem::path path;
                ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
            } cleanup{staging};
            {
                std::ofstream output(staging, std::ios::binary | std::ios::trunc);
                output << xml;
                output.close();
                if (!output) throw std::runtime_error("Cannot write recovered MaterialX document");
            }
            std::error_code error;
            std::filesystem::rename(staging, tempMtlx, error);
            // A concurrent process may have published identical content first.
            if (error && !std::filesystem::exists(tempMtlx))
                throw std::filesystem::filesystem_error("Publish recovered MaterialX", staging, tempMtlx, error);
        }
    } catch (const std::exception& e) {
        std::cerr << "[CompileMtlx] Failed to cache .mtlx: " << e.what() << "\n";
        return nullptr;
    }

    auto material = std::make_shared<MDLMaterial>(
        tempMtlx.string(),
        mdl_context,
        mx_libraries_path,
        mx_mdl_path,
        resolved.materialName
    );
    if (material->is_valid()) {
        if (templateEligible) {
            std::vector<prism::materialx::CompatibleBinding> bindings;
            if (prism::materialx::BuildCompatibleBindings(
                    conversionGraph,material->get_generated_uniforms(),&bindings))
                prism::materialx::CompatibleTemplateRegistry::Shared().publish(
                    {&mdl_context,mx_libraries_path,mx_mdl_path,
                     std::move(templateStructure),material,std::move(bindings)});
        }
        std::lock_guard lock(compiledMutex);
        std::erase_if(compiled, [](const ExactCompiled& entry) {
            return entry.material.expired();
        });
        if (compiled.size() >= 2048) compiled.erase(compiled.begin());
        compiled.push_back({&mdl_context, mx_libraries_path, mx_mdl_path,
            xml, resolved.materialName, material});
    }
    return material;
}

// ============================================================================
// Per-face-corner vertex layout
//
// USD meshes with faceVarying UVs (the common case) require that geometry is
// expanded so that each face corner becomes its own vertex entry. This struct
// carries the fully-expanded, renderer-ready data for one mesh prim and is
// built by buildMeshData() before anything is written into the global buffers.
// ============================================================================

// USD displayColor and displayOpacity of one mesh, for its fallback material:
// constant, or one value per face (uniform), point (vertex, varying) or face
// corner (faceVarying), indices already applied. A primvar whose size does
// not fit its interpolation counts as constant (its first value), as in the
// Hydra delegate.
struct DisplayPrimvar {
    std::vector<float> values;
    int components = 0;
    pxr::TfToken interpolation = pxr::UsdGeomTokens->constant;
    bool authored() const { return !values.empty(); }
    bool varying() const { return interpolation != pxr::UsdGeomTokens->constant; }
    // The value at a corner (point index, corner index, face index).
    float const* at(size_t point, size_t corner, size_t face) const {
        size_t element = 0;
        if (interpolation == pxr::UsdGeomTokens->uniform) element = face;
        else if (interpolation == pxr::UsdGeomTokens->faceVarying) element = corner;
        else if (interpolation != pxr::UsdGeomTokens->constant) element = point;
        return values.data() + element * size_t(components);
    }
};

struct DisplayPrimvars {
    DisplayPrimvar color, opacity;
    bool varying() const { return color.varying() || opacity.varying(); }
    // Per-face or per-corner values need one vertex per face corner.
    bool expands() const {
        auto expanding = [](DisplayPrimvar const& p) {
            return p.interpolation == pxr::UsdGeomTokens->uniform ||
                p.interpolation == pxr::UsdGeomTokens->faceVarying;
        };
        return expanding(color) || expanding(opacity);
    }
    // RGB colour and opacity at a corner (grey and opaque where unauthored).
    void append(std::vector<float>& out, size_t point, size_t corner, size_t face) const {
        static const float grey[3] = {0.7f, 0.7f, 0.7f}, opaque = 1.0f;
        float const* c = color.authored() ? color.at(point, corner, face) : grey;
        float const* o = opacity.authored() ? opacity.at(point, corner, face) : &opaque;
        out.insert(out.end(), {c[0], c[1], c[2], o[0]});
    }
};

struct MeshData {
    std::vector<Float3>        positions;
    std::vector<Float3>        normals;
    std::vector<Float2>        texcoords;
    std::vector<unsigned int>  indices;   // into the above, base-0 for this mesh
    int                        mat_id = -1;
    // displayColor (3) and displayOpacity (1) per vertex, when either varies
    // over the mesh; otherwise empty and displayConstant holds them.
    std::vector<float>         display;
    std::array<float, 4>       displayConstant{0.7f, 0.7f, 0.7f, 1.0f};
};

// ============================================================================
// USDMesh — loads geometry + detects and compiles MaterialX materials
// ============================================================================

class USDMesh : public Mesh {
public:
    struct AttachedSphere {
        unsigned int id;
        Sphere* geometry;
        pxr::GfMatrix4d world;
        Material* material;
    };

    size_t sphereCount() const { return m_spherePlacements.size(); }

    // Spheres retain their local analytic shape. Affine Embree instances also
    // represent nonuniformly scaled spheres without baking or tessellating them.
    std::vector<AttachedSphere> attachSphereInstances(RTCDevice device, RTCScene target) {
        std::vector<AttachedSphere> result;
        for (const auto& placement : m_spherePlacements) {
            auto& prototype = m_spherePrototypes[placement.radius];
            if (!prototype) prototype = std::make_unique<SpherePrototype>(device, placement.radius);
            RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
            rtcSetGeometryInstancedScene(instance, prototype->scene);
            float matrix[16];
            for (int c=0; c<4; ++c) for (int r=0; r<4; ++r)
                matrix[c*4+r] = float(placement.world[c][r]);
            rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, matrix);
            rtcCommitGeometry(instance);
            const unsigned id = rtcAttachGeometry(target, instance);
            rtcReleaseGeometry(instance);
            result.push_back({id, &prototype->geometry, placement.world,
                material_override ? material_override : materials.at(placement.materialId)});
        }
        return result;
    }

    // Basic constructor — no MDL/MaterialX support, uses displayColor fallback
    USDMesh(const std::filesystem::path& file_path)
        : USDMesh(file_path, nullptr, "", "")
    {}

    // Full constructor with MDL/MaterialX material support. Material bindings
    // resolve for the first of bindingPurposes that has one (UsdRenderSettings
    // materialBindingPurposes; empty: full, then allPurpose).
    USDMesh(const std::filesystem::path& file_path,
            mdl::Context* mdl_context,
            const std::string& mx_libraries_path,
            const std::string& mx_mdl_path,
            std::vector<pxr::TfToken> bindingPurposes = {})
        : m_bindingPurposes(std::move(bindingPurposes))
    {
        pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(file_path.string());
        if (!stage) {
            std::cerr << "Failed to open USD file: " << file_path << "\n";
            return;
        }
        std::cout << "Successfully opened USD stage: " << file_path << "\n";

        // ── Phase 1: Detect and compile MaterialX materials ────────────────
        if (mdl_context && !mx_libraries_path.empty()) {
            ArResolverContextBinder materialResolver(
                stage->GetPathResolverContext());
            // Template hits can bind a prepared texture before any full
            // MDLMaterial construction has registered the document directory.
            // Configure the shared material cache once on this ingestion
            // thread before entering the per-material loop.
            mdl_context->add_resource_path(
                (prism::PersistentCacheRoot() / "materials").string());
            std::cout << "[USDMesh] Scanning for MaterialX materials...\n";

            auto mtlxMaterials = CollectMaterialXMaterials(stage, mx_libraries_path, m_bindingPurposes);

            // Cache only within this mesh load: SDK context, libraries and loader
            // options are identical here. USD prim paths remain separate bindings.
            // Recovered UsdShade documents may contain different composed values,
            // so they must not be merged merely because their names match.
            using MaterialKey = std::pair<std::string, std::string>;
            std::map<MaterialKey, int> compiled_files;
            size_t reused_materials = 0;

            for (auto& [matPath, resolved] : mtlxMaterials) {
                std::optional<MaterialKey> cache_key;
                if (resolved.sourceKind == MtlxSourceKind::ReferencedFile) {
                    std::error_code error;
                    auto path = std::filesystem::weakly_canonical(resolved.mtlxFilePath, error);
                    if (!error) {
                        cache_key = MaterialKey(path.generic_string(), resolved.materialName);
                        auto cached = compiled_files.find(*cache_key);
                        if (cached != compiled_files.end()) {
                            m_mdl_material_lookup[matPath] = cached->second;
                            ++reused_materials;
                            if (UsdMaterialVerbose()) std::cout << "[USDMesh] Reused MaterialX material '"
                                      << matPath << "' -> mat_id " << cached->second << "\n";
                            continue;
                        }
                    }
                }
                auto mdlMat = CompileMtlxMaterial(
                    resolved, *mdl_context, mx_libraries_path, mx_mdl_path);

                if (mdlMat && mdlMat->is_valid()) {
                    int mat_id = static_cast<int>(m_owned_mdl_materials.size());
                    m_mdl_material_lookup[matPath] = mat_id;
                    materials[mat_id] = mdlMat.get();
                    m_owned_mdl_materials.push_back(std::move(mdlMat));
                    // Never cache failures; ownership remains in the vector and
                    // all reused bindings resolve to the same stable material ID.
                    if (cache_key) compiled_files.emplace(*cache_key, mat_id);

                    if (UsdMaterialVerbose()) std::cout << "[USDMesh] Compiled MaterialX material '"
                              << matPath << "' -> mat_id " << mat_id << "\n";
                } else {
                    std::cerr << "[USDMesh] Failed to compile MaterialX material: "
                              << matPath << "\n";
                }
            }

            std::cout << "[USDMesh] Compiled " << m_owned_mdl_materials.size()
                      << " MaterialX material(s), reused " << reused_materials
                      << ", bound " << m_mdl_material_lookup.size() << " USD material(s)\n";
        }

        // ── Phase 2: Process geometry ──────────────────────────────────────
        pxr::UsdTimeCode timeCode = pxr::UsdTimeCode::Default();
        pxr::UsdGeomXformCache xformCache(timeCode);

        // We do a single traversal. PointInstancers are handled inline;
        // their prototype subtrees are skipped by the traversal iterator
        // so we won't double-process prototype meshes.
        pxr::UsdPrimRange range(stage->GetPseudoRoot(), pxr::UsdTraverseInstanceProxies());
        for (auto iter = range.begin(); iter != range.end(); ++iter) {
            const pxr::UsdPrim& prim = *iter;

            if (prim.IsA<pxr::UsdGeomPointInstancer>()) {
                processPointInstancer(
                    pxr::UsdGeomPointInstancer(prim),
                    xformCache, timeCode);

                // Skip into the instancer's subtree — prototypes live there
                // and must not be processed as stand-alone meshes.
                iter.PruneChildren();
                continue;
            }

            if (prim.IsA<pxr::UsdGeomMesh>()) {
                MeshData data = buildMeshData(
                    pxr::UsdGeomMesh(prim), xformCache, timeCode,
                    xformCache.GetLocalToWorldTransform(prim));
                appendMeshData(data);
            } else if (prim.IsA<pxr::UsdGeomSphere>()) {
                captureSphere(pxr::UsdGeomSphere(prim), timeCode,
                    xformCache.GetLocalToWorldTransform(prim));
            }
        }

        installDisplayPrimvars();

        std::cout << "Finished processing USD file:\n"
                  << "    Total vertices:  " << vertices.size()  << "\n"
                  << "    Total triangles: " << globalTriangleCount << "\n"
                  << "    Total materials: " << materials.size() << "\n";
    }

private:

    struct SpherePlacement {
        float radius;
        pxr::GfMatrix4d world;
        int materialId;
    };
    struct SpherePrototype {
        Sphere geometry;
        RTCScene scene;
        SpherePrototype(RTCDevice device, float radius)
            : geometry(Float3(0), radius, nullptr), scene(rtcNewScene(device)) {
            geometry.setupEmbree(device, scene);
            rtcCommitScene(scene);
        }
        ~SpherePrototype() {
            rtcReleaseScene(scene);
            if (geometry.geom) rtcReleaseGeometry(geometry.geom);
            delete geometry.transform;
        }
    };
    std::vector<SpherePlacement> m_spherePlacements;
    std::map<float, std::unique_ptr<SpherePrototype>> m_spherePrototypes;

    void captureSphere(const pxr::UsdGeomSphere& sphere,
                       pxr::UsdTimeCode time, const pxr::GfMatrix4d& transform) {
        if (sphere.ComputeVisibility(time) == pxr::UsdGeomTokens->invisible) return;
        const TfToken purpose = sphere.ComputePurpose();
        if (purpose == pxr::UsdGeomTokens->guide || purpose == pxr::UsdGeomTokens->proxy) return;
        double radius = 1;
        sphere.GetRadiusAttr().Get(&radius, time);
        if (!(radius > 0) || !std::isfinite(radius) ||
            !std::isfinite(float(radius)) || std::abs(transform.GetDeterminant()) < 1e-15) {
            std::cerr << "[USDMesh] Invalid analytic sphere: " << sphere.GetPath() << '\n';
            return;
        }
        m_spherePlacements.push_back({float(radius), transform, resolveMaterialId(sphere.GetPrim(), time)});
    }

    int globalTriangleCount = 0;

    std::vector<std::shared_ptr<MDLMaterial>> m_owned_mdl_materials;
    std::vector<std::unique_ptr<ColorTexture>> m_owned_fallback_textures;
    std::vector<std::unique_ptr<Diffuse>> m_owned_fallback_materials;
    std::map<SdfPath, int>                    m_mdl_material_lookup;
    std::vector<pxr::TfToken>                 m_bindingPurposes;

    // displayColor / displayOpacity of the appended meshes: constant spans,
    // and per-vertex values of the meshes where they vary. They become the
    // mesh primvars `displayColor` and `displayOpacity` (as in the Hydra
    // delegate) only if some mesh varies.
    struct DisplaySpan {
        size_t first = 0, count = 0;
        std::array<float, 4> constant{};
        std::vector<float> values; // 4 per vertex, or empty: the constant
    };
    std::vector<DisplaySpan> m_displaySpans;
    bool m_displayVaries = false;

    void installDisplayPrimvars()
    {
        if (!m_displayVaries) {
            m_displaySpans.clear();
            return;
        }
        primvars = {{"displayColor", 3, 0}, {"displayOpacity", 1, 3}};
        primvarStride = 4;
        primvarValues.assign(vertices.size() * 4, 0.0f);
        for (auto const& span : m_displaySpans) {
            float* out = primvarValues.data() + span.first * 4;
            if (!span.values.empty())
                std::copy(span.values.begin(), span.values.end(), out);
            else
                for (size_t v = 0; v < span.count; ++v) std::copy(span.constant.begin(), span.constant.end(), out + 4 * v);
        }
        m_displaySpans.clear();
    }

    // displayColor (components 3) or displayOpacity (1) of a mesh prim with
    // the given counts of points, faces and face corners.
    static DisplayPrimvar readDisplayPrimvar(const pxr::UsdPrim& prim, const char* name, int components,
                                             pxr::UsdTimeCode time, size_t points, size_t faces, size_t corners)
    {
        DisplayPrimvar result;
        result.components = components;
        const pxr::UsdGeomPrimvar primvar = pxr::UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken(name));
        pxr::VtValue value;
        if (!primvar || !primvar.ComputeFlattened(&value, time)) return result;
        if (components == 3 && value.IsHolding<pxr::VtVec3fArray>()) {
            for (auto const& c : value.UncheckedGet<pxr::VtVec3fArray>())
                result.values.insert(result.values.end(), {c[0], c[1], c[2]});
        } else if (components == 1 && value.IsHolding<pxr::VtFloatArray>()) {
            for (float o : value.UncheckedGet<pxr::VtFloatArray>()) result.values.push_back(o);
        }
        if (result.values.empty()) return result;
        const TfToken interpolation = primvar.GetInterpolation();
        const size_t count = result.values.size() / size_t(components);
        size_t expected = 0;
        if (interpolation == pxr::UsdGeomTokens->uniform) expected = faces;
        else if (interpolation == pxr::UsdGeomTokens->faceVarying) expected = corners;
        else if (interpolation == pxr::UsdGeomTokens->vertex || interpolation == pxr::UsdGeomTokens->varying)
            expected = points;
        if (expected && count == expected) result.interpolation = interpolation;
        return result;
    }

    // -----------------------------------------------------------------------
    // appendMeshData — commit a fully-built MeshData into the global buffers
    // -----------------------------------------------------------------------

    void appendMeshData(const MeshData& data)
    {
        if (data.positions.empty()) return;

        const size_t vertexOffset = vertices.size();
        DisplaySpan span;
        span.first = vertexOffset;
        span.count = data.positions.size();
        span.constant = data.displayConstant;
        if (data.display.size() == 4 * data.positions.size()) {
            span.values = data.display;
            m_displayVaries = true;
        }
        m_displaySpans.push_back(std::move(span));

        // Append geometry
        vertices.insert(vertices.end(),   data.positions.begin(),  data.positions.end());
        normals.insert(normals.end(),     data.normals.begin(),    data.normals.end());
        texcoords.insert(texcoords.end(), data.texcoords.begin(),  data.texcoords.end());

        // Append indices (rebased to global vertex buffer)
        for (unsigned int idx : data.indices) {
            indices.push_back(static_cast<unsigned int>(vertexOffset) + idx);
        }

        // Assign material to every triangle
        size_t triCount = data.indices.size() / 3;
        for (size_t t = 0; t < triCount; ++t) {
            material_mapping[globalTriangleCount++] = data.mat_id;
        }
    }

    // -----------------------------------------------------------------------
    // resolveMaterialId — look up or create a fallback material for a prim
    // -----------------------------------------------------------------------

    int resolveMaterialId(const pxr::UsdPrim& prim,
                          pxr::UsdTimeCode timeCode,
                          DisplayPrimvars const* display = nullptr)
    {
        if (!m_mdl_material_lookup.empty()) {
            pxr::UsdShadeMaterial boundMat = ComputeBoundMaterialFor(prim, m_bindingPurposes);
            if (boundMat) {
                auto it = m_mdl_material_lookup.find(
                    boundMat.GetPrim().GetPath());
                if (it != m_mdl_material_lookup.end())
                    return it->second;
            }
        }

        // Fallback: Diffuse from displayColor
        pxr::UsdGeomPrimvarsAPI primvarsAPI(prim);
        pxr::UsdGeomPrimvar displayColorPrimvar =
            primvarsAPI.GetPrimvar(TfToken("displayColor"));

        Color3 mesh_color(0.7f, 0.7f, 0.7f);
        if (displayColorPrimvar) {
            pxr::VtValue val;
            if (displayColorPrimvar.Get(&val, timeCode) &&
                val.IsHolding<pxr::VtVec3fArray>())
            {
                auto arr = val.Get<pxr::VtVec3fArray>();
                if (!arr.empty())
                    mesh_color = Color3(arr[0][0], arr[0][1], arr[0][2]);
            }
        }

        // displayOpacity, and the mesh primvars where either varies (the
        // Hydra delegate's FallbackMaterial).
        float opacity = 1.0f;
        if (display) {
            if (display->color.authored())
                mesh_color = Color3(display->color.values[0], display->color.values[1], display->color.values[2]);
            if (display->opacity.authored()) opacity = display->opacity.values.front();
        } else {
            // Other prims (spheres): a constant displayOpacity only.
            pxr::UsdGeomPrimvar opacityPrimvar = primvarsAPI.GetPrimvar(TfToken("displayOpacity"));
            pxr::VtValue val;
            if (opacityPrimvar && opacityPrimvar.GetInterpolation() == pxr::UsdGeomTokens->constant &&
                opacityPrimvar.Get(&val, timeCode) && val.IsHolding<pxr::VtFloatArray>() &&
                !val.UncheckedGet<pxr::VtFloatArray>().empty())
                opacity = val.UncheckedGet<pxr::VtFloatArray>()[0];
        }
        const bool primvarColor = display && display->color.varying();
        const bool primvarOpacity = display && display->opacity.varying();

        int mat_id = static_cast<int>(materials.size());
        auto texture = std::make_unique<ColorTexture>(mesh_color);
        std::unique_ptr<Diffuse> material;
        if (primvarColor || primvarOpacity || prism::hydra::NormalizeFallbackOpacity(opacity) < 1.0f)
            material = std::make_unique<prism::hydra::FallbackMaterial>(texture.get(), opacity, primvarColor,
                                                                        primvarOpacity);
        else
            material = std::make_unique<Diffuse>(texture.get());
        materials[mat_id] = material.get();
        m_owned_fallback_textures.push_back(std::move(texture));
        m_owned_fallback_materials.push_back(std::move(material));
        return mat_id;
    }

    // -----------------------------------------------------------------------
    // buildMeshData — two-pass builder
    //
    // Pass 1: read all primvars and decide on the vertex layout.
    //         faceVarying UVs require one vertex per face corner.
    //         vertex/varying UVs keep the original indexed layout.
    //
    // Pass 2: emit expanded geometry into MeshData.
    //
    // The transform is applied here so that the same function works for both
    // regular meshes (world transform from xformCache) and instancer copies
    // (prototype transform composed with the instance transform).
    // -----------------------------------------------------------------------

    MeshData buildMeshData(const pxr::UsdGeomMesh& mesh,
                           pxr::UsdGeomXformCache& xformCache,
                           pxr::UsdTimeCode timeCode,
                           const pxr::GfMatrix4d& worldTransform)
    {
        MeshData out;

        // ── Read raw mesh attributes ───────────────────────────────────────
        pxr::VtArray<pxr::GfVec3f> points;
        pxr::VtArray<int>          faceVertexIndices;
        pxr::VtArray<int>          faceVertexCounts;

        if (!mesh.GetPointsAttr().Get(&points, timeCode) ||
            !mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, timeCode) ||
            !mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, timeCode))
        {
            std::cerr << "[USDMesh] Missing required attributes on "
                      << mesh.GetPrim().GetPath() << "\n";
            out.mat_id = resolveMaterialId(mesh.GetPrim(), timeCode);
            return out;
        }

        // ── displayColor / displayOpacity, and the material ───────────────
        DisplayPrimvars display;
        display.color = readDisplayPrimvar(mesh.GetPrim(), "displayColor", 3, timeCode, points.size(),
                                           faceVertexCounts.size(), faceVertexIndices.size());
        display.opacity = readDisplayPrimvar(mesh.GetPrim(), "displayOpacity", 1, timeCode, points.size(),
                                             faceVertexCounts.size(), faceVertexIndices.size());
        out.mat_id = resolveMaterialId(mesh.GetPrim(), timeCode, &display);
        if (display.color.authored())
            std::copy_n(display.color.values.begin(), 3, out.displayConstant.begin());
        if (display.opacity.authored()) out.displayConstant[3] = display.opacity.values.front();

        // ── Compute transform-derived values ───────────────────────────────
        pxr::GfMatrix4d normalMatrix = worldTransform.GetInverse().GetTranspose();
        bool flipNormals = worldTransform.GetDeterminant() < 0.0;

        // ── Pass 1: UV primvar discovery ───────────────────────────────────
        pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh.GetPrim());
        pxr::UsdGeomPrimvar uvPrimvar;
        for (const pxr::UsdGeomPrimvar& pv : primvarsAPI.GetPrimvars()) {
            TfToken name = pv.GetPrimvarName();
            if (name == "st" || name == "uv" || name == "UV") {
                uvPrimvar = pv;
                break;
            }
        }

        pxr::VtArray<pxr::GfVec2f> uvValues;
        pxr::VtArray<int>          uvIndices;
        pxr::TfToken               uvInterpolation;
        bool                       hasUvIndices = false;
        bool                       isFaceVarying = false;

        if (uvPrimvar) {
            uvPrimvar.Get(&uvValues, timeCode);
            hasUvIndices   = uvPrimvar.GetIndices(&uvIndices, timeCode);
            uvInterpolation = uvPrimvar.GetInterpolation();
            isFaceVarying  = (uvInterpolation == pxr::UsdGeomTokens->faceVarying);
        }

        // ── Pass 1: normals discovery ──────────────────────────────────────
        pxr::VtArray<pxr::GfVec3f> meshNormals;
        pxr::TfToken               normalsInterpolation;
        bool                       hasMeshNormals = false;
        bool                       normalsAreFaceVarying = false;

        pxr::UsdAttribute normalsAttr = mesh.GetNormalsAttr();
        if (normalsAttr && normalsAttr.Get(&meshNormals, timeCode)) {
            hasMeshNormals = true;
            normalsInterpolation = mesh.GetNormalsInterpolation();
            normalsAreFaceVarying = normalsInterpolation == pxr::UsdGeomTokens->faceVarying;
            pxr::UsdGeomPrimvar normalsPrimvar =
                primvarsAPI.GetPrimvar(pxr::UsdGeomTokens->normals);
            if (normalsPrimvar) {
                normalsInterpolation = normalsPrimvar.GetInterpolation();
                normalsAreFaceVarying =
                    (normalsInterpolation == pxr::UsdGeomTokens->faceVarying);
            }
        }

        // ── Pass 2: emit geometry ──────────────────────────────────────────
        //
        // If UVs (or normals) are faceVarying we must expand: one vertex
        // per face corner. Otherwise we keep the compact indexed layout.
        //
        if (isFaceVarying || normalsAreFaceVarying || display.expands()) {
            buildFaceVaryingLayout(
                out, points, faceVertexIndices, faceVertexCounts,
                uvValues, uvIndices, hasUvIndices,
                meshNormals, normalsAreFaceVarying, hasMeshNormals,
                worldTransform, normalMatrix, flipNormals, display);
        } else {
            buildIndexedLayout(
                out, points, faceVertexIndices, faceVertexCounts,
                uvValues, uvInterpolation,
                meshNormals, normalsInterpolation, hasMeshNormals,
                worldTransform, normalMatrix, flipNormals, display);
        }

        return out;
    }

    // -----------------------------------------------------------------------
    // buildFaceVaryingLayout
    //
    // Expands the mesh so every face corner is its own vertex. This is
    // required when UVs (or normals) are authored with faceVarying
    // interpolation, since the same position vertex may need different
    // UV/normal values depending on which face it belongs to.
    // -----------------------------------------------------------------------

    void buildFaceVaryingLayout(
        MeshData& out,
        const pxr::VtArray<pxr::GfVec3f>& points,
        const pxr::VtArray<int>&           faceVertexIndices,
        const pxr::VtArray<int>&           faceVertexCounts,
        const pxr::VtArray<pxr::GfVec2f>& uvValues,
        const pxr::VtArray<int>&           uvIndices,
        bool                               hasUvIndices,
        const pxr::VtArray<pxr::GfVec3f>& meshNormals,
        bool                               normalsAreFaceVarying,
        bool                               hasMeshNormals,
        const pxr::GfMatrix4d&             worldTransform,
        const pxr::GfMatrix4d&             normalMatrix,
        bool                               flipNormals,
        const DisplayPrimvars&             display)
    {
        // Pre-compute per-vertex geometric normals for fallback / fan normal
        std::vector<pxr::GfVec3f> geometricNormals =
            computeGeometricNormals(points, faceVertexIndices, faceVertexCounts, flipNormals);

        size_t faceCorner = 0;

        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int fvc = faceVertexCounts[faceIdx];

            // Collect per-corner expanded vertex indices for triangulation
            std::vector<unsigned int> cornerVerts;
            cornerVerts.reserve(fvc);

            for (int c = 0; c < fvc; ++c) {
                int posIdx = faceVertexIndices[faceCorner + c];

                unsigned int expandedIdx = static_cast<unsigned int>(out.positions.size());

                // Position
                pxr::GfVec3d p(points[posIdx][0], points[posIdx][1], points[posIdx][2]);
                pxr::GfVec3d tp = worldTransform.Transform(p);
                out.positions.push_back(Float3(
                    static_cast<float>(tp[0]),
                    static_cast<float>(tp[1]),
                    static_cast<float>(tp[2])));

                // UV
                if (!uvValues.empty()) {
                    int uvIdx = hasUvIndices
                        ? uvIndices[faceCorner + c]
                        : static_cast<int>(faceCorner + c);
                    const pxr::GfVec2f& uv = uvValues[uvIdx];
                    out.texcoords.push_back(Float2(uv[0], uv[1]));
                } else {
                    out.texcoords.push_back(Float2(0.f, 0.f));
                }

                // Normal
                pxr::GfVec3f rawNormal(0.f, 1.f, 0.f);
                if (hasMeshNormals && normalsAreFaceVarying &&
                    (faceCorner + c) < meshNormals.size())
                {
                    rawNormal = meshNormals[faceCorner + c];
                } else if (hasMeshNormals && !normalsAreFaceVarying &&
                           posIdx < static_cast<int>(meshNormals.size()))
                {
                    rawNormal = meshNormals[posIdx];
                } else if (posIdx < static_cast<int>(geometricNormals.size())) {
                    rawNormal = geometricNormals[posIdx];
                }

                out.normals.push_back(
                    transformNormal(rawNormal, normalMatrix, flipNormals));

                if (display.varying())
                    display.append(out.display, size_t(posIdx), faceCorner + c, faceIdx);

                cornerVerts.push_back(expandedIdx);
            }

            // Fan triangulate
            triangulate(out.indices, cornerVerts);

            faceCorner += fvc;
        }
    }

    // -----------------------------------------------------------------------
    // buildIndexedLayout
    //
    // Keeps the original compact indexed layout for vertex/varying/uniform
    // interpolated data. Normals and UVs are mapped 1:1 with points.
    // -----------------------------------------------------------------------

    void buildIndexedLayout(
        MeshData& out,
        const pxr::VtArray<pxr::GfVec3f>& points,
        const pxr::VtArray<int>&           faceVertexIndices,
        const pxr::VtArray<int>&           faceVertexCounts,
        const pxr::VtArray<pxr::GfVec2f>& uvValues,
        const pxr::TfToken&                uvInterpolation,
        const pxr::VtArray<pxr::GfVec3f>& meshNormals,
        const pxr::TfToken&                normalsInterpolation,
        bool                               hasMeshNormals,
        const pxr::GfMatrix4d&             worldTransform,
        const pxr::GfMatrix4d&             normalMatrix,
        bool                               flipNormals,
        const DisplayPrimvars&             display)
    {
        // Positions
        out.positions.reserve(points.size());
        for (const pxr::GfVec3f& p : points) {
            pxr::GfVec3d pd(p[0], p[1], p[2]);
            pxr::GfVec3d tp = worldTransform.Transform(pd);
            out.positions.push_back(Float3(
                static_cast<float>(tp[0]),
                static_cast<float>(tp[1]),
                static_cast<float>(tp[2])));
        }

        // Normals (per-vertex)
        if (hasMeshNormals &&
            (normalsInterpolation == pxr::UsdGeomTokens->vertex ||
             normalsInterpolation == pxr::UsdGeomTokens->varying))
        {
            out.normals.reserve(meshNormals.size());
            for (const pxr::GfVec3f& n : meshNormals) {
                out.normals.push_back(
                    transformNormal(n, normalMatrix, flipNormals));
            }
        } else {
            // Generate geometric normals
            auto geom = computeGeometricNormals(
                points, faceVertexIndices, faceVertexCounts, flipNormals);
            out.normals.reserve(geom.size());
            for (const pxr::GfVec3f& n : geom) {
                out.normals.push_back(
                    transformNormal(n, normalMatrix, flipNormals));
            }
        }

        // UVs
        if (!uvValues.empty() &&
            (uvInterpolation == pxr::UsdGeomTokens->vertex ||
             uvInterpolation == pxr::UsdGeomTokens->varying))
        {
            out.texcoords.reserve(uvValues.size());
            for (const pxr::GfVec2f& uv : uvValues) {
                out.texcoords.push_back(Float2(uv[0], uv[1]));
            }
        }

        // Pad normals / texcoords to match positions
        while (out.normals.size()    < out.positions.size())
            out.normals.push_back(Float3(0.f, 1.f, 0.f));
        while (out.texcoords.size()  < out.positions.size())
            out.texcoords.push_back(Float2(0.f, 0.f));

        // Display primvars per point (constant, vertex or varying here).
        if (display.varying())
            for (size_t p = 0; p < points.size(); ++p) display.append(out.display, p, 0, 0);

        // Indices — triangulate faces
        size_t faceCorner = 0;
        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int fvc = faceVertexCounts[faceIdx];
            std::vector<unsigned int> cornerVerts(fvc);
            for (int c = 0; c < fvc; ++c)
                cornerVerts[c] = static_cast<unsigned int>(
                    faceVertexIndices[faceCorner + c]);
            triangulate(out.indices, cornerVerts);
            faceCorner += fvc;
        }
    }

    // -----------------------------------------------------------------------
    // Shared helpers
    // -----------------------------------------------------------------------

    /// Fan-triangulate a face given its corner vertex indices (into MeshData).
    static void triangulate(std::vector<unsigned int>& outIndices,
                            const std::vector<unsigned int>& cornerVerts)
    {
        int fvc = static_cast<int>(cornerVerts.size());
        if (fvc < 3) return;

        if (fvc == 3) {
            outIndices.push_back(cornerVerts[0]);
            outIndices.push_back(cornerVerts[1]);
            outIndices.push_back(cornerVerts[2]);
        } else if (fvc == 4) {
            outIndices.push_back(cornerVerts[0]);
            outIndices.push_back(cornerVerts[1]);
            outIndices.push_back(cornerVerts[2]);
            outIndices.push_back(cornerVerts[0]);
            outIndices.push_back(cornerVerts[2]);
            outIndices.push_back(cornerVerts[3]);
        } else {
            // Fan from vertex 0 for n-gons
            for (int i = 1; i < fvc - 1; ++i) {
                outIndices.push_back(cornerVerts[0]);
                outIndices.push_back(cornerVerts[i]);
                outIndices.push_back(cornerVerts[i + 1]);
            }
        }
    }

    /// Compute area-weighted per-vertex geometric normals.
    static std::vector<pxr::GfVec3f>
    computeGeometricNormals(const pxr::VtArray<pxr::GfVec3f>& points,
                            const pxr::VtArray<int>&           faceVertexIndices,
                            const pxr::VtArray<int>&           faceVertexCounts,
                            bool                               flipNormals)
    {
        std::vector<pxr::GfVec3f> normals(points.size(), pxr::GfVec3f(0.f));

        size_t faceCorner = 0;
        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int fvc = faceVertexCounts[faceIdx];
            if (fvc >= 3) {
                int i0 = faceVertexIndices[faceCorner + 0];
                int i1 = faceVertexIndices[faceCorner + 1];
                int i2 = faceVertexIndices[faceCorner + 2];

                pxr::GfVec3f e1 = points[i1] - points[i0];
                pxr::GfVec3f e2 = points[i2] - points[i0];
                pxr::GfVec3f fn = e1 ^ e2; // cross product, area-weighted

                if (flipNormals) fn = -fn;

                for (int v = 0; v < fvc; ++v)
                    normals[faceVertexIndices[faceCorner + v]] += fn;
            }
            faceCorner += fvc;
        }

        for (auto& n : normals) {
            float len = n.GetLength();
            n = (len > 0.f) ? (n / len) : pxr::GfVec3f(0.f, 1.f, 0.f);
        }
        return normals;
    }

    /// Transform and normalize a normal vector.
    static Float3 transformNormal(const pxr::GfVec3f&    n,
                                  const pxr::GfMatrix4d& normalMatrix,
                                  bool                   flip)
    {
        pxr::GfVec3d nd(n[0], n[1], n[2]);
        pxr::GfVec3d tn = normalMatrix.TransformDir(nd);
        if (flip) tn = -tn;
        double len = tn.GetLength();
        if (len > 0.0) tn /= len;
        return Float3(
            static_cast<float>(tn[0]),
            static_cast<float>(tn[1]),
            static_cast<float>(tn[2]));
    }

    // -----------------------------------------------------------------------
    // PointInstancer support
    //
    // For each instance, compose the instance transform (position + orientation
    // + scale) with the prototype mesh's own local-to-world transform and call
    // buildMeshData with the composed transform. This produces a correct,
    // fully-transformed copy of the prototype geometry at each instance
    // location. Actual GPU instancing can be added later.
    // -----------------------------------------------------------------------

    void processPointInstancer(const pxr::UsdGeomPointInstancer& instancer,
                            pxr::UsdGeomXformCache&           xformCache,
                            pxr::UsdTimeCode                  timeCode)
    {
        pxr::VtArray<pxr::GfVec3f> positions;
        pxr::VtArray<pxr::GfQuath> orientations;
        pxr::VtArray<pxr::GfVec3f> scales;
        pxr::VtArray<int>          protoIndices;

        instancer.GetPositionsAttr().Get(&positions,     timeCode);
        instancer.GetOrientationsAttr().Get(&orientations, timeCode);
        instancer.GetScalesAttr().Get(&scales,           timeCode);
        instancer.GetProtoIndicesAttr().Get(&protoIndices, timeCode);

        if (positions.empty() || protoIndices.empty()) {
            std::cerr << "[PointInstancer] No instances on "
                    << instancer.GetPrim().GetPath() << "\n";
            return;
        }

        pxr::SdfPathVector protoPaths;
        instancer.GetPrototypesRel().GetTargets(&protoPaths);
        if (protoPaths.empty()) {
            std::cerr << "[PointInstancer] No prototypes on "
                    << instancer.GetPrim().GetPath() << "\n";
            return;
        }

        pxr::UsdStageRefPtr stage = instancer.GetPrim().GetStage();
        pxr::GfMatrix4d instancerWorld =
            xformCache.GetLocalToWorldTransform(instancer.GetPrim());

        std::cout << "[PointInstancer] Processing " << protoIndices.size()
                << " instance(s) on " << instancer.GetPrim().GetPath() << "\n";

        // invisibleIds (and inactiveIds): the instances to skip, as Hydra's
        // instancer topology mask does.
        const std::vector<bool> mask = instancer.ComputeMaskAtTime(timeCode);

        for (size_t i = 0; i < protoIndices.size(); ++i) {
            if (i < mask.size() && !mask[i]) continue;
            int protoIdx = protoIndices[i];
            if (protoIdx < 0 || protoIdx >= static_cast<int>(protoPaths.size())) {
                std::cerr << "[PointInstancer] Invalid proto index " << protoIdx
                        << " at instance " << i << "\n";
                continue;
            }

            pxr::UsdPrim protoPrim = stage->GetPrimAtPath(protoPaths[protoIdx]);
            if (!protoPrim.IsValid()) {
                std::cerr << "[PointInstancer] Prototype prim not found: "
                        << protoPaths[protoIdx] << "\n";
                continue;
            }

            // Build per-instance local transform: T * R * S
            pxr::GfVec3f pos = (i < positions.size())
                ? positions[i] : pxr::GfVec3f(0.f);
            pxr::GfQuath ori = (i < orientations.size())
                ? orientations[i] : pxr::GfQuath(1.f, 0.f, 0.f, 0.f);
            pxr::GfVec3f scl = (i < scales.size())
                ? scales[i] : pxr::GfVec3f(1.f, 1.f, 1.f);

            pxr::GfMatrix4d scaleMat(1.0);
            scaleMat.SetScale(pxr::GfVec3d(scl[0], scl[1], scl[2]));

            pxr::GfQuatf orif(
                static_cast<float>(ori.GetReal()),
                static_cast<float>(ori.GetImaginary()[0]),
                static_cast<float>(ori.GetImaginary()[1]),
                static_cast<float>(ori.GetImaginary()[2]));
            pxr::GfQuatd orid(
                orif.GetReal(),
                pxr::GfVec3d(orif.GetImaginary()[0],
                            orif.GetImaginary()[1],
                            orif.GetImaginary()[2]));
            pxr::GfMatrix4d rotMat(1.0);
            rotMat.SetRotate(orid);

            pxr::GfMatrix4d transMat(1.0);
            transMat.SetTranslate(pxr::GfVec3d(pos[0], pos[1], pos[2]));

            pxr::GfMatrix4d instanceLocal = scaleMat * rotMat * transMat;

            // The prototype's world transform already has instancerWorld baked in
            // (it's a child of the instancer). Strip it out to get only the
            // proto's own local contribution relative to the instancer.
            pxr::GfMatrix4d protoRelative =
                xformCache.GetLocalToWorldTransform(protoPrim) *
                instancerWorld.GetInverse();

            // Final: instancerWorld brings us from instancer-local to world,
            // instanceLocal places this specific instance, protoRelative applies
            // any local transform the prototype prim itself has.
            pxr::GfMatrix4d finalTransform =
                protoRelative * instanceLocal * instancerWorld;

            processPrimSubtreeWithTransform(
                protoPrim, xformCache, timeCode, finalTransform);
        }
    }

    /// Walk a prim subtree, applying a caller-supplied world transform to every
    /// mesh found. Used to process instancer prototypes with their composed
    /// instance transform rather than the prim's own world transform.
    void processPrimSubtreeWithTransform(const pxr::UsdPrim&       root,
                                         pxr::UsdGeomXformCache&   xformCache,
                                         pxr::UsdTimeCode          timeCode,
                                         const pxr::GfMatrix4d&    worldTransform)
    {
        if (root.IsA<pxr::UsdGeomMesh>()) {
            MeshData data = buildMeshData(
                pxr::UsdGeomMesh(root), xformCache, timeCode, worldTransform);
            appendMeshData(data);
        } else if (root.IsA<pxr::UsdGeomSphere>()) {
            captureSphere(pxr::UsdGeomSphere(root), timeCode, worldTransform);
        }

        for (const pxr::UsdPrim& child : root.GetChildren()) {
            // Compose child's local transform on top of the supplied world
            pxr::GfMatrix4d childLocal =
                xformCache.GetLocalToWorldTransform(child) *
                xformCache.GetLocalToWorldTransform(root).GetInverse();
            pxr::GfMatrix4d childWorld = childLocal * worldTransform;

            processPrimSubtreeWithTransform(child, xformCache, timeCode, childWorld);
        }
    }
};
