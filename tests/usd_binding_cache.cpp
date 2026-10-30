// The CLI's USD loader resolves material bindings through USD's binding
// caches (UsdBindingCaches, src/mesh/usd_mesh.h). They must resolve every
// prim exactly as the uncached UsdShadeMaterialBindingAPI::ComputeBoundMaterial
// does: direct, inherited, collection, purpose-restricted and
// strongerThanDescendants bindings, instance proxies and face subsets, for
// every material binding purpose list.

#include "../src/mesh/usd_mesh.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool value, std::string const& message)
{
    if (!value) throw std::runtime_error(message);
}

// Bindings of every kind the resolution handles.
constexpr char const* kStage = R"(#usda 1.0
def Xform "World" (
    prepend apiSchemas = ["MaterialBindingAPI", "CollectionAPI:shiny"]
)
{
    uniform token collection:shiny:expansionRule = "expandPrims"
    rel collection:shiny:includes = [</World/Group/B>, </World/Instanced>]
    rel collection:shiny:excludes = </World/Group/B/Inner>
    rel material:binding = </Looks/Base>
    rel material:binding:collection:shiny = [</World.collection:shiny>, </Looks/Shiny>]
    rel material:binding:collection:preview:shinyPreview = [</World.collection:shiny>, </Looks/Preview>]

    def Xform "Group" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding:full = </Looks/Full>
        def Mesh "A" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            rel material:binding:preview = </Looks/Preview>
        }
        def Xform "B"
        {
            def Mesh "Inner" {}
            def Mesh "Other" (
                prepend apiSchemas = ["MaterialBindingAPI"]
            )
            {
                rel material:binding = </Looks/Leaf>
                def GeomSubset "Half" (
                    prepend apiSchemas = ["MaterialBindingAPI"]
                )
                {
                    uniform token elementType = "face"
                    uniform token familyName = "materialBind"
                    int[] indices = [0]
                    rel material:binding = </Looks/Subset>
                }
            }
        }
    }
    def Xform "Strong" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </Looks/Strong> (
            bindMaterialAs = "strongerThanDescendants"
        )
        def Mesh "Weak" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            rel material:binding = </Looks/Leaf>
        }
    }
    def Xform "Instanced" (
        instanceable = true
        references = </Prototype>
    )
    {
    }
}
def Xform "Prototype"
{
    def Mesh "Shape" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding:preview = </Looks/Leaf>
    }
    def Mesh "Plain" {}
}
def Scope "Looks"
{
    def Material "Base" {}
    def Material "Shiny" {}
    def Material "Preview" {}
    def Material "Full" {}
    def Material "Leaf" {}
    def Material "Subset" {}
    def Material "Strong" {}
}
)";

size_t Compare(pxr::UsdStageRefPtr const& stage, std::string const& label)
{
    using pxr::TfToken;
    const std::vector<std::vector<TfToken>> purposeLists = {
        {},
        {pxr::UsdShadeTokens->full},
        {pxr::UsdShadeTokens->preview},
        {TfToken()},
        {pxr::UsdShadeTokens->preview, TfToken()},
        {pxr::UsdShadeTokens->full, pxr::UsdShadeTokens->preview, TfToken()}};
    size_t bound = 0;
    for (auto const& purposes : purposeLists) {
        UsdBindingCaches caches;  // shared by the whole stage, as the loader does
        // Twice: the second pass reads only cached bindings.
        for (int pass = 0; pass < 2; ++pass)
            for (pxr::UsdPrim const& prim :
                 pxr::UsdPrimRange(stage->GetPseudoRoot(), pxr::UsdTraverseInstanceProxies())) {
                const pxr::UsdShadeMaterial expected = ComputeBoundMaterialFor(prim, purposes);
                const pxr::UsdShadeMaterial cached = ComputeBoundMaterialFor(prim, purposes, &caches);
                require(expected.GetPath() == cached.GetPath(),
                        label + ": " + prim.GetPath().GetString() + " resolved " + cached.GetPath().GetString() +
                            " with the caches, " + expected.GetPath().GetString() + " without");
                bound += pass == 0 && bool(expected);
            }
    }
    return bound;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        auto stage = pxr::UsdStage::CreateInMemory("bindings.usda");
        require(stage->GetRootLayer()->ImportFromString(kStage), "cannot parse the binding stage");
        const size_t bound = Compare(stage, "bindings");
        require(bound >= 30, "the binding stage resolved only " + std::to_string(bound) + " bindings");
        for (int i = 1; i < argc; ++i) {
            auto fixture = pxr::UsdStage::Open(argv[i]);
            require(bool(fixture), std::string("cannot open ") + argv[i]);
            Compare(fixture, argv[i]);
        }
        std::cout << "USD binding caches resolve " << bound << " bindings as the uncached API\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "usd_binding_cache: " << error.what() << '\n';
        return 1;
    }
}
