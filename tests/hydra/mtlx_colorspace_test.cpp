// The document colour space of a .mtlx file referenced into a USD stage
// (src/usd/mtlx_color_space.h): read from the composed prim (the CLI), from
// the UsdImaging scene index through the delegate's MaterialXConfigAPI
// adapter (Hydra; needs hdPrism on PXR_PLUGINPATH_NAME), and applied to the
// untagged colour parameters of the material network only.
//
// prism_mtlx_colorspace_test <fixtures dir> (tests/hydra/fixtures)

#include "../../src/usd/mtlx_color_space.h"

#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <filesystem>
#include <iostream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;

void Expect(bool condition, std::string const& what)
{
    if (condition) return;
    std::cerr << "FAILED: " << what << "\n";
    ++failures;
}

// The material prim as the delegate receives it (UsdImaging's scene index
// chain, which must outlive the prim's data sources: they read through it).
HdContainerDataSourceHandle HydraMaterial(UsdStageRefPtr const& stage, SdfPath const& path,
                                          UsdImagingSceneIndices* indices)
{
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    *indices = UsdImagingCreateSceneIndices(info);
    return indices->finalSceneIndex->GetPrim(path).dataSource;
}

TfToken ParameterColorSpace(HdMaterialNetworkInterface& net, std::string const& nodeName,
                            std::string const& parameter, bool* found)
{
    for (TfToken const& node : net.GetNodeNames()) {
        if (node.GetString() != nodeName) continue;
        *found = true;
        return net.GetNodeParameterData(node, TfToken(parameter)).colorSpace;
    }
    *found = false;
    std::cerr << "no node '" << nodeName << "' among:";
    for (TfToken const& node : net.GetNodeNames()) std::cerr << " " << node;
    std::cerr << "\n";
    return {};
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: prism_mtlx_colorspace_test <tests/hydra/fixtures>\n";
        return 2;
    }
    const std::filesystem::path fixtures = argv[1];
    const SdfPath material("/World/Looks/Tinted");

    // A referenced document with colorspace="lin_rec709".
    auto referenced = UsdStage::Open((fixtures / "mtlx_document_colorspace.usda").string());
    Expect(bool(referenced), "open mtlx_document_colorspace.usda");
    if (!referenced) return 1;
    Expect(prism::usd::MtlxDocumentColorSpace(referenced->GetPrimAtPath(material)) == TfToken("lin_rec709"),
           "the composed Material prim reports the document's lin_rec709");
    // A prim not from a .mtlx layer has none.
    Expect(prism::usd::MtlxDocumentColorSpace(referenced->GetPrimAtPath(SdfPath("/World/Box"))).IsEmpty(),
           "a prim without a .mtlx layer has no document colour space");

    UsdImagingSceneIndices referencedIndices;
    const auto prim = HydraMaterial(referenced, material, &referencedIndices);
    Expect(prism::usd::MtlxDocumentColorSpace(prim) == TfToken("lin_rec709"),
           "the Hydra material prim carries prismMtlxDocument.colorSpace (MaterialXConfigAPI adapter)");

    // Applied to the network: untagged colours get it, tagged ones keep theirs,
    // non-colour values stay untagged.
    const auto network = HdMaterialSchema::GetFromParent(prim).GetMaterialNetwork(TfToken("mtlx"));
    Expect(bool(network), "the material has an mtlx network");
    if (network) {
        HdDataSourceMaterialNetworkInterface net(material, network.GetContainer(), prim);
        const size_t tagged = prism::usd::ApplyMtlxDocumentColorSpace(net, TfToken("lin_rec709"));
        Expect(tagged == 2, "two parameters tagged (tint, specular_color), got " + std::to_string(tagged));
        bool found = false;
        Expect(ParameterColorSpace(net, "NG_tint/tinted", "in1", &found) == TfToken("lin_rec709") && found,
               "the graph's untagged tint takes lin_rec709");
        Expect(ParameterColorSpace(net, "NG_tint/tinted", "in2", &found) == TfToken("acescg") && found,
               "the multiplier keeps its own acescg");
        Expect(ParameterColorSpace(net, "SR_tinted", "specular_color", &found) == TfToken("lin_rec709") && found,
               "the shader's untagged specular_color takes lin_rec709");
        Expect(ParameterColorSpace(net, "SR_tinted", "specular_roughness", &found).IsEmpty() && found,
               "a float stays untagged");
    }

    // The same network authored in UsdShade with colorSpace metadata: no
    // document, and USD's own tags reach the network.
    auto authored = UsdStage::Open((fixtures / "mtlx_document_colorspace" / "tagged.usda").string());
    Expect(bool(authored), "open tagged.usda");
    if (authored) {
        Expect(prism::usd::MtlxDocumentColorSpace(authored->GetPrimAtPath(material)).IsEmpty(),
               "a UsdShade material has no document colour space");
        UsdImagingSceneIndices authoredIndices;
        const auto authoredPrim = HydraMaterial(authored, material, &authoredIndices);
        Expect(prism::usd::MtlxDocumentColorSpace(authoredPrim).IsEmpty(),
               "no prismMtlxDocument on a UsdShade material");
        const auto authoredNetwork = HdMaterialSchema::GetFromParent(authoredPrim).GetMaterialNetwork(TfToken("mtlx"));
        if (authoredNetwork) {
            HdDataSourceMaterialNetworkInterface net(material, authoredNetwork.GetContainer(), authoredPrim);
            bool found = false;
            Expect(ParameterColorSpace(net, "SR_tinted", "specular_color", &found) == TfToken("lin_rec709") && found,
                   "USD colorSpace metadata reaches the network");
        } else {
            Expect(false, "the UsdShade material has an mtlx network");
        }
    }

    // One scope referencing four documents (the shader ball's examples): each
    // material takes its own document's colour space, and bubblegum.mtlx has
    // none.
    auto examples = UsdStage::Open((fixtures / ".." / ".." / ".." / "resources" / "material_preview.usda").string());
    Expect(bool(examples), "open resources/material_preview.usda");
    if (examples) {
        const SdfPath scope("/standard_shader_ball_scene/materials/examples/mtlx");
        const auto gold = examples->GetPrimAtPath(scope.AppendChild(TfToken("gold")));
        const auto bubblegum = examples->GetPrimAtPath(scope.AppendChild(TfToken("bubblegum")));
        Expect(gold && bubblegum, "the shader ball's gold and bubblegum materials exist");
        Expect(prism::usd::MtlxDocumentColorSpace(gold) == TfToken("lin_rec709"),
               "gold.mtlx's lin_rec709");
        Expect(bubblegum && prism::usd::MtlxDocumentColorSpace(bubblegum).IsEmpty(),
               "bubblegum.mtlx (no document colorspace) stays untagged");
    }

    if (failures) return 1;
    std::cout << "mtlx document colour space: all checks passed\n";
    return 0;
}
