#include "../src/materialx/captured_material.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/ar/defaultResolverContext.h>

#include <iostream>
#include <future>
#include <fstream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

HdMaterialNetwork2 makeNetwork(const SdfPath& root,
                               const TfToken& surfaceName,
                               const TfToken& textureName)
{
    HdMaterialNetwork2 network;
    const SdfPath surface=root.AppendChild(surfaceName);
    const SdfPath texture=root.AppendChild(textureName);
    auto& image=network.nodes[texture];
    image.nodeTypeId=TfToken("ND_image_color3");
    image.parameters[TfToken("file")]=VtValue(
        SdfAssetPath("textures/albedo.exr","C:/resolved/a/albedo.exr"));
    image.parameters[TfToken("typeName:file")]=VtValue(TfToken("filename"));
    image.parameters[TfToken("colorSpace:file")]=VtValue(TfToken("lin_rec709"));
    auto& shader=network.nodes[surface];
    shader.nodeTypeId=TfToken("ND_open_pbr_surface_surfaceshader");
    shader.parameters[TfToken("base_weight")]=VtValue(1.0f);
    shader.parameters[TfToken("base_color")]=VtValue(GfVec3f(.2f,.3f,.4f));
    shader.parameters[TfToken("typeName:base_color")]=VtValue(TfToken("color3f"));
    shader.parameters[TfToken("colorSpace:base_color")]=VtValue(TfToken("lin_rec709"));
    shader.inputConnections[TfToken("base_color")].push_back({texture,TfToken("out")});
    network.terminals[TfToken("surface")]=HdMaterialConnection2{surface,TfToken("out")};
    return network;
}

// Models the scene-index adapter's logical parameter enumeration while using
// the same retained network payload. The legacy Network2 adapter additionally
// enumerates its typeName:/colorSpace: storage keys.
class SceneIndexParameterInterface final : public HdMaterialNetwork2Interface {
public:
    using HdMaterialNetwork2Interface::HdMaterialNetwork2Interface;
    TfTokenVector GetAuthoredNodeParameterNames(const TfToken& node) const override
    {
        auto names=HdMaterialNetwork2Interface::GetAuthoredNodeParameterNames(node);
        std::erase_if(names,[](const TfToken& name) {
            const std::string& text=name.GetString();
            return text.starts_with("typeName:") || text.starts_with("colorSpace:");
        });
        return names;
    }
};

prism::materialx::CapturedMaterialGraph capture(HdMaterialNetwork2& network,
                                                 const SdfPath& root)
{
    HdMaterialNetwork2Interface networkInterface(root,&network);
    return prism::materialx::CaptureMaterialGraph(
        networkInterface,TfToken("mtlx"),root);
}
}

int main()
{
    try {
        const SdfPath firstRoot("/World/Looks/First");
        const SdfPath secondRoot("/Other/Looks/Renamed");
        auto first=makeNetwork(firstRoot,TfToken("SurfaceA"),TfToken("ImageA"));
        auto renamed=makeNetwork(secondRoot,TfToken("ShaderZ"),TfToken("TextureZ"));
        const auto canonical=capture(first,firstRoot);
        const auto canonicalRenamed=capture(renamed,secondRoot);
        require(canonical==canonicalRenamed,
                "equivalent graphs depend on USD paths or node names");
        SceneIndexParameterInterface sceneIndexInterface(firstRoot,&first);
        require(canonical==prism::materialx::CaptureMaterialGraph(
                    sceneIndexInterface,TfToken("mtlx"),firstRoot),
                "legacy metadata pseudo-parameters changed cross-interface identity");

        auto topology=renamed;
        topology.nodes[secondRoot.AppendChild(TfToken("ShaderZ"))]
            .inputConnections.clear();
        require(!(canonical==capture(topology,secondRoot)),
                "different graph topology compared equal");
        auto nodeType=renamed;
        nodeType.nodes[secondRoot.AppendChild(TfToken("TextureZ"))].nodeTypeId=
            TfToken("ND_constant_color3");
        require(!(canonical==capture(nodeType,secondRoot)),
                "different node types compared equal");
        auto value=renamed;
        value.nodes[secondRoot.AppendChild(TfToken("ShaderZ"))]
            .parameters[TfToken("base_weight")]=VtValue(.5f);
        require(!(canonical==capture(value,secondRoot)),
                "different typed values compared equal");
        auto colorSpace=renamed;
        colorSpace.nodes[secondRoot.AppendChild(TfToken("ShaderZ"))]
            .parameters[TfToken("colorSpace:base_color")]=VtValue(TfToken("acescg"));
        require(!(canonical==capture(colorSpace,secondRoot)),
                "different color spaces compared equal");
        auto unit=canonicalRenamed;
        unit.nodes.front().typeInfo.emplace_back(TfToken("unit"),VtValue(TfToken("meter")));
        require(!(canonical==unit),"different unit metadata compared equal");
        auto config=canonicalRenamed;
        config.config.emplace_back(TfToken("materialTag"),VtValue(TfToken("masked")));
        require(!(canonical==config),"different material configuration compared equal");
        auto resolved=renamed;
        resolved.nodes[secondRoot.AppendChild(TfToken("TextureZ"))]
            .parameters[TfToken("file")]=VtValue(
                SdfAssetPath("textures/albedo.exr","D:/resolved/b/albedo.exr"));
        require(!(canonical==capture(resolved,secondRoot)),
                "relative assets with different resolved paths aliased");

        auto& registry=prism::materialx::CapturedMaterialRegistry::Shared();
        registry.clear();
        prism::materialx::CapturedConversionArtifact artifact;
        artifact.libraries="libraries-v1";
        artifact.resolverIdentity="resolver-a";
        artifact.resolverContext=ArResolverContext(
            ArDefaultResolverContext({"C:/search/a"}));
        artifact.sourceVersion="source-v1";
        artifact.graph=canonical;
        artifact.materialName="Published";
        artifact.xml="<materialx/>";
        registry.publish(artifact);
        prism::materialx::CapturedConversionArtifact found;
        require(registry.find(artifact,&found),"published artifact was not found");
        auto differentContext=artifact;
        differentContext.resolverIdentity="resolver-b";
        differentContext.resolverContext=ArResolverContext(
            ArDefaultResolverContext({"C:/search/b"}));
        require(!registry.find(differentContext,&found),
                "registry aliased distinct resolver contexts");
        auto differentVersion=artifact;
        differentVersion.sourceVersion="source-v2";
        require(!registry.find(differentVersion,&found),
                "registry aliased distinct source versions");

        registry.clear();
        auto owner=registry.acquire(artifact);
        auto waiter=registry.acquire(artifact);
        require(owner.isOwner()&&!waiter.isOwner(),
                "same conversion key did not singleflight");
        auto waited=std::async(std::launch::async,[ticket=std::move(waiter)]() mutable {
            prism::materialx::CapturedConversionArtifact result;
            return std::pair{ticket.wait(&result),result.materialName};
        });
        require(owner.publish(artifact),"singleflight owner could not publish");
        const auto shared=waited.get();
        require(shared.first&&shared.second==artifact.materialName,
                "singleflight waiter did not receive the published artifact");
        // Model a caller whose find() missed just before the owner published.
        // Its subsequent acquisition must consume the completed result.
        auto lateWaiter=registry.acquire(artifact);
        require(!lateWaiter.isOwner()&&lateWaiter.wait(&found)&&
                    found.materialName==artifact.materialName,
                "find/acquire publication race repeated material conversion");

        auto keyA=artifact, keyB=artifact;
        keyA.sourceVersion="parallel-a"; keyB.sourceVersion="parallel-b";
        auto parallelA=registry.acquire(keyA);
        auto parallelB=registry.acquire(keyB);
        require(parallelA.isOwner()&&parallelB.isOwner(),
                "different conversion keys incorrectly serialized");
        auto publishA=std::async(std::launch::async,
            [ticket=std::move(parallelA),keyA]() mutable { return ticket.publish(keyA); });
        auto publishB=std::async(std::launch::async,
            [ticket=std::move(parallelB),keyB]() mutable { return ticket.publish(keyB); });
        require(publishA.get()&&publishB.get(),
                "independent conversion publication failed");

        auto failedKey=artifact;
        failedKey.sourceVersion="failure-retry";
        auto failedWaiter=registry.acquire(failedKey);
        {
            auto failedOwner=std::move(failedWaiter);
            require(failedOwner.isOwner(),"failure fixture did not acquire ownership");
            auto waiting=registry.acquire(failedKey);
            failedOwner.fail();
            require(!waiting.wait(nullptr),"failed conversion reported success");
        }
        auto retry=registry.acquire(failedKey);
        require(retry.isOwner()&&retry.publish(failedKey),
                "failed conversion could not be retried");

        auto staleKey=artifact;
        staleKey.sourceVersion="pre-clear";
        auto staleOwner=registry.acquire(staleKey);
        auto staleWaiter=registry.acquire(staleKey);
        registry.clear();
        require(!staleWaiter.wait(nullptr),"clear did not cancel a waiting conversion");
        require(!staleOwner.publish(staleKey),
                "pre-clear owner republished into the new generation");
        require(!registry.find(staleKey,&found),
                "pre-clear artifact survived registry clear");
        auto freshOwner=registry.acquire(staleKey);
        require(freshOwner.isOwner()&&freshOwner.publish(staleKey),
                "post-clear generation could not publish");
        registry.clear();

        const auto udimRoot=std::filesystem::temp_directory_path()/
            "prism_captured_material_udim";
        std::filesystem::create_directories(udimRoot/"cache");
        for (const int tile : {1001,1002}) {
            std::ofstream out(udimRoot/("tile."+std::to_string(tile)+".ppm"),
                              std::ios::binary|std::ios::trunc);
            out << "P3\n1 1\n255\n" << (tile==1001?"255 0 0\n":"0 255 0\n");
        }
        const auto pattern=(udimRoot/"tile.<UDIM>.ppm").string();
        const auto family=prism::materialx::PrepareMaterialUdimResource(
            pattern,pattern,ArResolverContext(),"0",udimRoot/"cache",".ppm");
        require(family.tiles.size()==2&&family.mdlPath.find("<UDIM>")!=std::string::npos,
                "two-tile UDIM family was not preserved");
        for (const int tile : {1001,1002}) {
            auto path=udimRoot/"cache"/
                (family.mdlPath.substr(1,family.mdlPath.find("<UDIM>")-1)+
                 std::to_string(tile)+".ppm");
            require(std::filesystem::is_regular_file(path),
                    "materialized UDIM tile is missing");
        }
        std::error_code cleanup;
        std::filesystem::remove_all(udimRoot,cleanup);
        std::cout << "captured material graph canonical identity passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "captured_material_graph: " << error.what() << '\n';
        return 1;
    }
}
