// prism::usd::LoadMaterialPreview (include/prism/usd.h, docs/material_preview.md):
// a material on the USD-WG StandardShaderBall, shared by the CLI's --material
// and prismrt.preview_material. The stage is resources/material_preview.usda,
// which sublayers the shipped asset (resources/shaderball, unchanged) and
// selects its standard_surface material_model variant; the previewed material
// replaces the binding of the ball's material surface
// (LoadOptions::materialOverrides).

#include <prism/usd.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace prism::usd {
namespace {

// The asset's prims (resources/shaderball/standard_shader_ball_scene.usda).
constexpr const char* kMaterialSurface = "/standard_shader_ball_scene/shader_ball/material_surface";
constexpr const char* kCamera = "/standard_shader_ball_scene/camera";
constexpr const char* kSceneFile = "material_preview.usda";

// The directory of the library holding this function (prism_usd).
std::filesystem::path ModuleDirectory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module))
        return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) || !info.dli_fname) return {};
    return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

std::string Join(std::vector<std::string> const& names)
{
    std::string text;
    for (auto const& name : names) text += (text.empty() ? "" : ", ") + name;
    return text;
}

// The material nodes of a MaterialX document, in document order (the order
// MDLMaterial picks the first of).
std::vector<std::string> DocumentMaterials(MaterialDesc const& material, std::string const& label)
{
    MaterialX::DocumentPtr document = MaterialX::createDocument();
    try {
        if (material.kind == MaterialDesc::Kind::MaterialXFile) {
            const std::filesystem::path path(material.path);
            MaterialX::FileSearchPath search(path.parent_path().string());
            MaterialX::readFromXmlFile(document, path.string(), search);
        } else {
            MaterialX::readFromXmlString(document, material.document);
        }
    } catch (std::exception const& error) {
        throw Error(ErrorCode::InvalidArgument, "cannot read the MaterialX document " + label + ": " + error.what());
    }
    std::vector<std::string> names;
    for (auto const& node : document->getMaterialNodes()) names.push_back(node->getName());
    if (names.empty())
        throw Error(ErrorCode::InvalidArgument, "the MaterialX document " + label + " has no material");
    return names;
}

} // namespace

std::string MaterialPreviewScene()
{
    std::vector<std::filesystem::path> candidates;
    if (const char* overridden = std::getenv("PRISM_MATERIAL_PREVIEW_SCENE"); overridden && *overridden) {
        if (std::filesystem::exists(overridden)) return overridden;
        throw Error(ErrorCode::NotFound, std::string("PRISM_MATERIAL_PREVIEW_SCENE names no file: ") + overridden);
    }
    const std::filesystem::path module = ModuleDirectory();
    if (!module.empty()) {
        candidates.push_back(module / "resources" / kSceneFile);
        candidates.push_back(module.parent_path() / "resources" / kSceneFile);
    }
    std::string searched;
    for (auto const& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate.string();
        searched += (searched.empty() ? "" : ", ") + candidate.string();
    }
    throw Error(ErrorCode::NotFound, "the material preview scene is not installed (looked for " +
                (searched.empty() ? std::string("resources/material_preview.usda beside prism_usd") : searched) +
                "; PRISM_MATERIAL_PREVIEW_SCENE names another)");
}

MaterialPreview LoadMaterialPreview(Scene& scene, MaterialDesc material, MaterialPreviewOptions const& options)
{
    MaterialPreview result;
    const bool materialX = material.kind == MaterialDesc::Kind::MaterialXFile ||
                           material.kind == MaterialDesc::Kind::MaterialXString;
    if (materialX) {
        std::string label = material.kind == MaterialDesc::Kind::MaterialXFile ? "'" + material.path + "'"
                                                                                : std::string("(string)");
        if (material.kind == MaterialDesc::Kind::MaterialXFile) {
            std::error_code error;
            if (material.path.empty() || !std::filesystem::is_regular_file(material.path, error))
                throw Error(ErrorCode::NotFound, "MaterialX document '" + material.path + "' does not exist");
        }
        result.documentMaterials = DocumentMaterials(material, label);
        if (material.materialName.empty()) {
            // MaterialDesc's rule: the document's first material.
            material.materialName = result.documentMaterials.front();
        } else if (std::find(result.documentMaterials.begin(), result.documentMaterials.end(),
                             material.materialName) == result.documentMaterials.end()) {
            throw Error(ErrorCode::NotFound, "the MaterialX document " + label + " has no material '" +
                        material.materialName + "' (its materials: " + Join(result.documentMaterials) + ")");
        }
        result.materialName = material.materialName;
    }
    if (material.name.empty()) material.name = result.materialName.empty() ? "preview" : result.materialName;

    const std::string file = options.scene.empty() ? MaterialPreviewScene() : options.scene;
    pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(file);
    if (!stage) throw Error(ErrorCode::NotFound, "cannot open the material preview scene " + file);
    const pxr::SdfPath surface(kMaterialSurface);
    if (!stage->GetPrimAtPath(surface))
        throw Error(ErrorCode::InvalidArgument, "the material preview scene " + file + " has no " + kMaterialSurface);

    result.material = scene.addMaterial(material);
    LoadOptions load;
    load.time = options.time;
    load.materialOverrides.emplace_back(surface, result.material);
    result.load = Load(scene, stage, load);
    result.camera = LoadCamera(stage, pxr::SdfPath(kCamera), options.time);
    return result;
}

} // namespace prism::usd
