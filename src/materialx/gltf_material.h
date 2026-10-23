#pragma once

// glTF 2.0 materials as MaterialX gltf_pbr documents (docs/gltf.md).
//
// ConvertGltfMaterial builds, for one glTF material, an in-memory document
// with a gltf_pbr surface shader and a surfacematerial, mapping the
// metallic-roughness model and the KHR_materials_* / KHR_texture_transform
// extensions onto gltf_pbr's inputs. The document then goes through the
// regular MaterialX -> MDL pipeline (DocumentLoader, MDLGenerator,
// mdl::MaterialCompiler), like any .mtlx file.
//
// Only MaterialXCore and tinygltf are used here: the caller decides where an
// image's file lives (imageFile) and how the document is compiled.
//
// Conventions the mesh loader must follow for the document to read its
// geometry correctly:
// - texture coordinates are stored with V flipped (v' = 1 - v): MaterialX
//   puts the texture origin bottom-left, glTF top-left (the gltf_image
//   nodes' pivot (0, 1) expects exactly this);
// - TEXCOORD_1 is a vector2 primvar named kTexcoord1Primvar (also V flipped);
// - COLOR_0 is a color4 primvar named kColor0Primvar, already in the
//   working space (glTF vertex colours are linear Rec.709).

#include <MaterialXCore/Document.h>
#include <tiny_gltf.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace prism::gltf {

inline constexpr const char* kTexcoord1Primvar = "TEXCOORD_1";
inline constexpr const char* kColor0Primvar = "COLOR_0";
inline constexpr const char* kMaterialName = "GltfMaterial";

// How glTF materials load: through MaterialX (default) or as the legacy C++
// materials (src/material.h).
enum class MaterialMode { MaterialX, Legacy };
// "materialx" or "legacy"; false for anything else.
bool ParseMaterialMode(const std::string& text, MaterialMode* mode);
// PRISM_GLTF_MATERIALS, else MaterialX. An invalid value warns and uses
// MaterialX.
MaterialMode MaterialModeFromEnvironment();

struct MaterialConversionOptions {
    // The file name written into the document for a glTF image index (for
    // example the name of a materialized copy beside the cached .mtlx).
    // Empty: the image is unavailable and the texture is dropped (warning).
    std::function<std::string(int image)> imageFile;
    // Some primitive drawn with the material has COLOR_0: base colour and
    // alpha are multiplied by the kColor0Primvar primvar.
    bool vertexColor = false;
    // Linear Rec.709 into the working space, for the uniform colour inputs
    // (attenuation_color) the generator cannot transform. Empty: identity.
    std::function<std::array<float, 3>(std::array<float, 3>)> fromLinearRec709;
};

struct MaterialConversion {
    MaterialX::DocumentPtr document;
    std::vector<std::string> warnings;
    // The document reads the kTexcoord1Primvar / kColor0Primvar primvars.
    bool usesTexcoord1 = false;
    bool usesVertexColor = false;
};

// material < 0 converts the glTF default material (no material on the
// primitive). Throws std::runtime_error on an invalid material index.
MaterialConversion ConvertGltfMaterial(const tinygltf::Model& model, int material,
                                       const MaterialConversionOptions& options);

// Material extensions the converter maps (some partly; docs/gltf.md).
bool IsSupportedMaterialExtension(const std::string& extension);
// Extensions of model.extensionsUsed neither the converter nor the mesh
// loader handles, for a one-line warning each.
std::vector<std::string> UnsupportedExtensions(const tinygltf::Model& model);

} // namespace prism::gltf
