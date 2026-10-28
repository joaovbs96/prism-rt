#pragma once

// STD Lib deps
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>

// External Deps
#include <tiny_obj_loader.h>
#include <MaterialXFormat/XmlIo.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>

#include <prism/material.h>

// internal deps
#include "../geometry.h"
#include "../float3.h"
#include "../float2.h"
#include "../transforms.h"
#include "../material.h"
#include "../mdl_material.h"
#include "../resource_limits.h"
#include "../materialx/obj_material.h"
#include "../materialx/cached_document.h"
#include "../materialx/material_resource.h"
#include "../render_defaults.h"
#include "mesh_conversion.h"

// Wavefront OBJ scenes, read for prism::obj::Load (src/obj/obj_loader.cpp):
// per-corner triangles and their MTL materials as MaterialX
// open_pbr_surface documents in the material cache (docs/obj.md), which the
// scene compiles through MDL at commit (and displaces the faces of materials
// with MTL disp). A material whose conversion fails renders with the magenta
// error material, after a one-line warning; faces without a material are
// grey (Diffuse 0.5).
class ObjMesh : public Mesh {
    public:
        // The document of an MTL material (-1: faces without one, a grey
        // Diffuse), or why it has none.
        struct MaterialSource {
            std::optional<prism::MaterialDesc> desc;
            std::string failure;
        };

        // materialSettings: the renderer's colour management (materials convert
        // into its working space) and cache directory. Without MaterialX
        // libraries every MTL material is the error material.
        ObjMesh(const std::filesystem::path &file_path, bool materialx,
                prism::MaterialSettings materialSettings)
            : m_path(file_path), m_materials(std::move(materialSettings))
        {
            if (!m_materials.color) throw std::invalid_argument("ObjMesh needs a colour management");
            // based on: https://github.com/yumcyaWiz/embree-tinyobj-example
            std::cout << "Loading File: " << file_path << std::endl;
            std::ifstream stream(file_path, std::ios::binary);
            if (!stream) {
                std::cout << "Unable to open OBJ file (" << file_path.filename() << ")" << std::endl;
                return;
            }
            MaterialFiles reader(file_path.parent_path());
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> obj_materials;
            std::string warn, err;
            const bool loaded = tinyobj::LoadObj(&attrib, &shapes, &obj_materials, &warn, &err, &stream, &reader,
                                                 /*triangulate*/ true, /*default_vcols_fallback*/ true);
            if (!warn.empty()) std::cout << "Warning during OBJ load: " << warn << std::endl;
            if (!loaded) {
                std::cout << "Unable to load OBJ file (" << file_path.filename() << "): " << err << std::endl;
                return;
            }

            std::cout << "Finished loading file. " << std::endl;

            int index = 0, n_faces = 0;
            std::set<int> used;
            for (size_t s = 0; s < shapes.size(); ++s)
            {
                // for each face of the current mesh
                for (size_t f = 0; f < shapes[s].mesh.indices.size() / 3; f++)
                {
                    // Get the three indexes of the face (all faces are triangular)
                    tinyobj::index_t idx0 = shapes[s].mesh.indices[3 * f + 0];
                    tinyobj::index_t idx1 = shapes[s].mesh.indices[3 * f + 1];
                    tinyobj::index_t idx2 = shapes[s].mesh.indices[3 * f + 2];

                    // get the vertex coordinates
                    make_Float3<std::vector<tinyobj::real_t>>(vertices, attrib.vertices, idx0.vertex_index);
                    make_Float3<std::vector<tinyobj::real_t>>(vertices, attrib.vertices, idx1.vertex_index);
                    make_Float3<std::vector<tinyobj::real_t>>(vertices, attrib.vertices, idx2.vertex_index);

                    const Float3 geometric = cross(vertices[vertices.size()-2]-vertices[vertices.size()-3],
                        vertices.back()-vertices[vertices.size()-3]).normalize();
                    for (const auto& corner : {idx0, idx1, idx2}) {
                        if (corner.normal_index >= 0)
                            make_Float3(normals, attrib.normals, corner.normal_index);
                        else normals.push_back(geometric);
                        if (corner.texcoord_index >= 0)
                            make_Float2(texcoords, attrib.texcoords, corner.texcoord_index);
                        else texcoords.emplace_back(0, 0);
                    }

                    // set index vector
                    indices.push_back(index++);
                    indices.push_back(index++);
                    indices.push_back(index++);

                    // per-face material
                    int material = shapes[s].mesh.material_ids[f];
                    if (material >= int(obj_materials.size())) material = -1;
                    material_mapping[n_faces++] = material;
                    used.insert(material);
                }
            }
            std::cout << "Finished creating mesh data. " << std::endl;

            buildMaterials(obj_materials, used, reader, materialx);
            std::cout << "OBJ: " << vertices.size() << " vertices, " << indices.size() / 3 << " triangles, "
                      << m_sources.size() << " material(s)\n";
        }

        // Materials converted to MaterialX, and those rendered with the error
        // material.
        size_t materialxMaterialCount() const { return m_materialxCount; }
        size_t errorMaterialCount() const { return m_errorCount; }
        // By MTL material (-1: faces without one), the materials faces use.
        std::map<int, MaterialSource> const& materialSources() const { return m_sources; }

    private:
        // tinyobjloader's file reader for mtllib, which also keeps each MTL
        // file's statements (tinyobjloader cannot tell its defaults from
        // authored values) and the directory its maps resolve against.
        class MaterialFiles : public tinyobj::MaterialReader {
        public:
            explicit MaterialFiles(std::filesystem::path base) : m_base(std::move(base)) {}

            bool operator()(const std::string& name, std::vector<tinyobj::material_t>* materials,
                            std::map<std::string, int>* matMap, std::string* warn, std::string* err) override
            {
                std::filesystem::path path = std::filesystem::path(name);
                if (path.is_relative()) path = m_base / path;
                std::ifstream in(path, std::ios::binary);
                if (!in) {
                    if (warn) *warn += "Material file [ " + name + " ] not found in " + m_base.string() + "\n";
                    return false;
                }
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                for (auto& [material, statements] : prism::obj::ScanMtlStatements(text))
                    this->statements[material].insert(statements.begin(), statements.end());
                std::istringstream mtl(text);
                tinyobj::LoadMtl(matMap, materials, &mtl, warn, err);
                directories.resize(materials->size(), path.parent_path());
                return true;
            }

            prism::obj::MtlStatements statements;
            // By material index: the directory of its MTL file.
            std::vector<std::filesystem::path> directories;

        private:
            std::filesystem::path m_base;
        };

        std::filesystem::path m_path;
        prism::MaterialSettings m_materials;
        std::map<int, MaterialSource> m_sources;
        size_t m_materialxCount = 0, m_errorCount = 0;

        void buildMaterials(const std::vector<tinyobj::material_t>& source, const std::set<int>& used,
                            const MaterialFiles& files, bool materialx)
        {
            if (used.contains(-1)) {
                // Faces without a material.
                prism::MaterialDesc grey;
                grey.kind = prism::MaterialDesc::Kind::Diffuse;
                grey.color = Color3(.5f);
                grey.name = "OBJ faces without a material";
                m_sources[-1].desc = grey;
            }
            if (!materialx) {
                for (int m : used)
                    if (m >= 0) useErrorMaterial(source, m, "no MDL context or MaterialX libraries");
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            const std::filesystem::path cache = m_materials.cacheDirectory / "materials";
            std::set<std::string> documents;
            std::map<std::string, std::string> published;                 // map path -> cached file name
            struct ImageHeader {
                bool alpha = false;
                std::array<int, 2> size{0, 0};
            };
            std::map<std::string, ImageHeader> headers;
            std::map<std::string, std::string> unsupported;               // -> first material
            std::set<std::string> warned;
            size_t reused = 0;
            for (int m : used) {
                if (m < 0) continue;
                const tinyobj::material_t& material = source[size_t(m)];
                const std::filesystem::path directory =
                    size_t(m) < files.directories.size() ? files.directories[size_t(m)] : m_path.parent_path();
                auto locate = [&](const std::string& map) -> std::filesystem::path {
                    std::string name = map;
                    std::replace(name.begin(), name.end(), '\\', '/');
                    std::filesystem::path path(name);
                    if (path.is_absolute()) return std::filesystem::exists(path) ? path : std::filesystem::path();
                    for (auto const& base : {directory, m_path.parent_path()})
                        if (std::filesystem::exists(base / path)) return base / path;
                    return {};
                };
                std::optional<prism::MaterialDesc> compiledMaterial;
                std::string failure;
                try {
                    prism::obj::MaterialConversionOptions options;
                    auto statements = files.statements.find(material.name);
                    static const std::set<std::string> none;
                    options.statements = statements != files.statements.end() ? &statements->second : &none;
                    options.imageFile = [&](const std::string& map) -> std::string {
                        const std::filesystem::path path = locate(map);
                        if (path.empty()) return {};
                        const std::string key = path.lexically_normal().generic_string();
                        if (auto found = published.find(key); found != published.end()) return found->second;
                        const std::string name = publishMap(path, cache);
                        published.emplace(key, name);
                        return name;
                    };
                    // A map's header: alpha channel and size.
                    auto header = [&](const std::string& map) -> ImageHeader {
                        const std::filesystem::path path = locate(map);
                        if (path.empty()) return {};
                        const std::string key = path.lexically_normal().generic_string();
                        if (auto found = headers.find(key); found != headers.end()) return found->second;
                        ImageHeader result;
                        if (auto input = OIIO::ImageInput::open(path.string())) {
                            result.alpha = input->spec().alpha_channel >= 0 || input->spec().nchannels == 4;
                            result.size = {input->spec().width, input->spec().height};
                            input->close();
                        }
                        headers.emplace(key, result);
                        return result;
                    };
                    options.hasAlpha = [&](const std::string& map) { return header(map).alpha; };
                    options.imageSize = [&](const std::string& map) { return header(map).size; };
                    auto conversion = prism::obj::ConvertObjMaterial(material, options);
                    for (auto const& warning : conversion.warnings)
                        if (warned.insert(warning).second) std::cerr << "PrismRT OBJ: " << warning << "\n";
                    for (auto const& statement : conversion.unsupported) unsupported.emplace(statement, material.name);
                    const std::string xml = MaterialX::writeToXmlString(conversion.document);
                    // Equal documents publish to one file, which the loader
                    // adds once.
                    const auto path = prism::materialx::PublishMaterialXDocument(xml, cache, "obj_");
                    if (!documents.insert(path.string()).second) ++reused;
                    compiledMaterial.emplace();
                    compiledMaterial->kind = prism::MaterialDesc::Kind::MaterialXFile;
                    compiledMaterial->path = path.string();
                    compiledMaterial->materialName = prism::obj::kMaterialName;
                    // Maps resolve beside the cached documents (as for USD and glTF).
                    compiledMaterial->searchPath = cache.string();
                    compiledMaterial->name = "'" + material.name + "'";
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                if (compiledMaterial) {
                    m_sources[m].desc = std::move(compiledMaterial);
                    ++m_materialxCount;
                } else {
                    useErrorMaterial(source, m, failure);
                }
            }
            for (auto const& [statement, material] : unsupported)
                std::cerr << "PrismRT OBJ: MTL " << statement << " is not supported; ignored (first in material '"
                          << material << "')\n";
            std::cout << "OBJ: " << m_materialxCount << " MaterialX material(s) (" << documents.size()
                      << " documents, " << reused << " reused), " << m_errorCount << " failed to convert, in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s\n";
        }

        // A map published content-addressed into the material cache (beside
        // the documents); its file name there. Formats MDL's image plugin
        // cannot read (TGA, BMP, ... in older OBJ assets) are transcoded to
        // PNG first; the file itself is never touched.
        static std::string publishMap(const std::filesystem::path& path, const std::filesystem::path& cache)
        {
            std::string extension = path.extension().string();
            for (char& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
            static const std::set<std::string> readable = {".png", ".jpg", ".jpeg", ".exr", ".tif", ".tiff", ".hdr"};
            if (readable.contains(extension)) {
                auto resource = prism::materialx::PrepareMaterialResource(
                    prism::materialx::FileResourceReader(std::filesystem::absolute(path)), "",
                    cache, extension).resource;
                return resource ? resource->path.filename().generic_string() : "";
            }
            OIIO::ImageBuf image(path.string());
            if (!image.read(0, 0, true, OIIO::TypeDesc::UINT8))
                throw std::runtime_error("cannot read map " + path.string() + ": " + image.geterror());
            std::filesystem::create_directories(cache);
            const std::filesystem::path staging = cache /
                (path.filename().string() + ".transcode-" + std::to_string(std::hash<std::string>{}(path.string())) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
            if (!image.write(staging.string(), OIIO::TypeDesc::UINT8, "png"))
                throw std::runtime_error("cannot transcode map " + path.string() + ": " + image.geterror());
            std::ifstream in(staging, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return prism::materialx::PublishMaterialResourceBytes(bytes.data(), bytes.size(), ".png", cache)
                ->path.filename().generic_string();
        }

        // A material that cannot load renders magenta (as a failed USD or
        // glTF material does), with a one-line warning naming it and the reason.
        void useErrorMaterial(const std::vector<tinyobj::material_t>& source, int m, const std::string& reason)
        {
            std::cerr << "PrismRT OBJ: material '" << source[size_t(m)].name << "' failed to load ("
                      << (reason.empty() ? std::string("unknown error") : reason)
                      << "); rendering it with the magenta error material\n";
            m_sources[m].failure = reason.empty() ? std::string("unknown error") : reason;
            ++m_errorCount;
        }
};
