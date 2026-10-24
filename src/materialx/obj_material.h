#pragma once

// Wavefront OBJ / MTL materials as MaterialX open_pbr_surface documents
// (docs/obj.md).
//
// ConvertObjMaterial builds, for one MTL material (as tinyobjloader parsed
// it), an in-memory document with an open_pbr_surface shader and a
// surfacematerial. Phong-style materials (Kd/Ks/Ns, d/Tr, Ni, illum, Ke and
// their maps) and the PBR extension (Pr/Pm/Ps/Pc/Pcr/aniso and their maps)
// map onto OpenPBR's inputs; the mapping follows Adobe's USD file-format
// plugin for OBJ (docs/obj.md). The document then goes through the regular
// MaterialX -> MDL pipeline (DocumentLoader, MDLGenerator,
// mdl::MaterialCompiler), like any .mtlx file.
//
// Only MaterialXCore and tinyobjloader are used here: the caller decides
// where a map's file lives (imageFile) and how the document is compiled.
// OBJ texture coordinates already have MaterialX's convention (origin at the
// bottom left): the mesh stores them unchanged.

#include <MaterialXCore/Document.h>
#include <tiny_obj_loader.h>

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace prism::obj {

inline constexpr const char* kMaterialName = "ObjMaterial";

// The statements of the MTL materials of one file, by material name: each
// statement's keyword in lower case ("kd", "map_pr"), plus "<keyword>
// -imfchan" for a map that names its channel. tinyobjloader fills unauthored
// values with defaults; this tells them apart.
using MtlStatements = std::map<std::string, std::set<std::string>>;
MtlStatements ScanMtlStatements(const std::string& text);

struct MaterialConversionOptions {
    // The file name written into the document for a map as the MTL names it
    // (for example the name of a published copy beside the cached .mtlx).
    // Empty: the file is unavailable and the map is dropped (warning).
    std::function<std::string(const std::string& map)> imageFile;
    // Whether a map's image has an alpha channel: map_d without -imfchan
    // then reads alpha (a cut-out mask in the colour map's alpha is common),
    // otherwise red. Empty: red.
    std::function<bool(const std::string& map)> hasAlpha;
    // A map's size in texels, for bump's finite differences. Empty, or
    // {0, 0}: 1024 x 1024.
    std::function<std::array<int, 2>(const std::string& map)> imageSize;
    // The material's statements (ScanMtlStatements). Null: every value
    // differing from tinyobjloader's default counts as authored.
    const std::set<std::string>* statements = nullptr;
};

struct MaterialConversion {
    MaterialX::DocumentPtr document;
    // About this material (a missing map, a value out of range).
    std::vector<std::string> warnings;
    // Statements and texture options the mapping ignores ("refl", "illum 10",
    // "-mm"), for one warning per load each.
    std::set<std::string> unsupported;
    // disp became the material's displacementshader.
    bool displacement = false;
};

MaterialConversion ConvertObjMaterial(const tinyobj::material_t& material,
                                      const MaterialConversionOptions& options);

// The roughness of a Phong specular colour and exponent:
// sqrt(2 / (Ns * luminance(Ks) + 2)), and 0 (a mirror) below 0.08.
float PhongRoughness(float ns, const float ks[3]);

} // namespace prism::obj
