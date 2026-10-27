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
#include "../materialx/resolved_resource.h"
#include "../render_defaults.h"
#include "mesh_conversion.h"
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/types.h>

// Wavefront OBJ scenes. MTL materials load as MaterialX open_pbr_surface
// documents compiled through MDL (docs/obj.md). A material whose conversion
// or compilation fails (or that has no MDL context to compile with) renders
// with the magenta error material, after a one-line warning; faces without a
// material are grey (Diffuse 0.5).
class ObjMesh : public Mesh {
    public:
        // Faces whose material displaces them (MTL disp) are diced and
        // displaced at load time with `subdivision`'s displacement level and
        // face limit, as USD meshes are. materialSettings: the renderer's colour
        // management (materials convert into its working space) and cache
        // directory.
        ObjMesh(const std::filesystem::path &file_path, mdl::Context* mdl_context,
                const std::string& mx_libraries_path, const std::string& mx_mdl_path,
                prism::MaterialSettings materialSettings,
                prism::mesh::SubdivisionSettings subdivision = DefaultSubdivision())
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

            buildMaterials(obj_materials, used, reader, mdl_context, mx_libraries_path, mx_mdl_path);
            displace(subdivision);
            std::cout << "OBJ: " << vertices.size() << " vertices, " << indices.size() / 3 << " triangles, "
                      << materials.size() << " material(s)\n";
        }

        // Materials that compiled through MaterialX, and those rendered with
        // the error material.
        size_t materialxMaterialCount() const { return m_materialxCount; }
        size_t errorMaterialCount() const { return m_errorCount; }

        static prism::mesh::SubdivisionSettings DefaultSubdivision()
        {
            auto const& defaults = prism::defaults::FromEnvironment();
            prism::mesh::SubdivisionSettings result;
            result.displacementLevel = defaults.displacementLevel;
            result.displacementMaxFaces = size_t(defaults.displacementMaxFaces);
            return result;
        }

    private:
        // Faces of displacing materials, by the shared conversion: each
        // material's faces become a mesh (corners at one position welded, so
        // the surface stays closed), refined bilinearly to the displacement
        // level within its face limit, and displaced.
        void displace(prism::mesh::SubdivisionSettings const& subdivision)
        {
            std::map<int, std::vector<size_t>> displaced; // material -> triangles
            const size_t triangles = indices.size() / 3;
            for (size_t t = 0; t < triangles; ++t) {
                auto material = materials.find(material_mapping.at(int(t)));
                if (material != materials.end() && material->second && material->second->hasDisplacement())
                    displaced[material->first].push_back(t);
            }
            if (displaced.empty()) return;
            std::vector<Float3> keptVertices, keptNormals;
            std::vector<Float2> keptTexcoords;
            std::vector<unsigned> keptIndices;
            std::unordered_map<int, int> keptMaterials;
            int face = 0;
            for (size_t t = 0; t < triangles; ++t) {
                const int material = material_mapping.at(int(t));
                if (displaced.contains(material)) continue;
                for (int k = 0; k < 3; ++k) {
                    const unsigned v = indices[3 * t + k];
                    keptIndices.push_back(unsigned(keptVertices.size()));
                    keptVertices.push_back(vertices[v]);
                    keptNormals.push_back(normals[v]);
                    keptTexcoords.push_back(texcoords[v]);
                }
                keptMaterials[face++] = material;
            }
            size_t refinedTriangles = 0;
            for (auto const& [material, faces] : displaced) {
                prism::mesh::MeshSource source;
                pxr::VtVec3fArray points;
                pxr::VtVec2fArray uvs;
                std::map<std::array<float, 3>, int> welded;
                for (size_t t : faces) {
                    source.faceVertexCounts.push_back(3);
                    for (int k = 0; k < 3; ++k) {
                        const unsigned v = indices[3 * t + k];
                        const std::array<float, 3> key{vertices[v].x(), vertices[v].y(), vertices[v].z()};
                        auto [found, added] = welded.emplace(key, int(points.size()));
                        if (added) points.push_back(pxr::GfVec3f(key[0], key[1], key[2]));
                        source.faceVertexIndices.push_back(found->second);
                        uvs.push_back(pxr::GfVec2f(texcoords[v].u(), texcoords[v].v()));
                    }
                }
                source.points.value = pxr::VtValue(std::move(points));
                source.points.interpolation = pxr::TfToken("vertex");
                source.uvs.value = pxr::VtValue(std::move(uvs));
                source.uvs.interpolation = pxr::TfToken("faceVarying");
                source.doubleSided = true;
                prism::mesh::MeshConversion conversion =
                    prism::mesh::ConvertMesh(source, subdivision, materials.at(material));
                if (!conversion.mesh) continue;
                Mesh const& mesh = *conversion.mesh;
                const unsigned base = unsigned(keptVertices.size());
                keptVertices.insert(keptVertices.end(), mesh.vertices.begin(), mesh.vertices.end());
                keptNormals.insert(keptNormals.end(), mesh.normals.begin(), mesh.normals.end());
                keptTexcoords.insert(keptTexcoords.end(), mesh.texcoords.begin(), mesh.texcoords.end());
                for (unsigned index : mesh.indices) keptIndices.push_back(base + index);
                for (size_t t = 0; t < mesh.indices.size() / 3; ++t) keptMaterials[face++] = material;
                refinedTriangles += mesh.indices.size() / 3;
            }
            vertices = std::move(keptVertices);
            normals = std::move(keptNormals);
            texcoords = std::move(keptTexcoords);
            indices = std::move(keptIndices);
            material_mapping = std::move(keptMaterials);
            tangents.clear();
            std::cout << "OBJ: displaced the faces of " << displaced.size() << " material(s) into "
                      << refinedTriangles << " triangles\n";
        }

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
        std::vector<std::shared_ptr<MDLMaterial>> m_ownedMdl;
        std::unique_ptr<Texture<Color3>> m_greyTexture;
        std::unique_ptr<Material> m_grey;
        size_t m_materialxCount = 0, m_errorCount = 0;

        void buildMaterials(const std::vector<tinyobj::material_t>& source, const std::set<int>& used,
                            const MaterialFiles& files, mdl::Context* mdl_context, const std::string& libraries,
                            const std::string& mdlLibraries)
        {
            if (used.contains(-1)) {
                // Faces without a material.
                m_greyTexture = std::make_unique<ColorTexture>(Color3(.5f));
                m_grey = std::make_unique<Diffuse>(m_greyTexture.get());
                materials[-1] = m_grey.get();
            }
            if (!mdl_context || libraries.empty()) {
                for (int m : used)
                    if (m >= 0) useErrorMaterial(source, m, "no MDL context or MaterialX libraries");
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            const std::filesystem::path cache = m_materials.cacheDirectory / "materials";
            // Maps resolve beside the cached documents (as for USD and glTF).
            mdl_context->add_resource_path(cache.string());
            std::map<std::string, std::shared_ptr<MDLMaterial>> compiled; // by document
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
                std::shared_ptr<MDLMaterial> compiledMaterial;
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
                    if (auto found = compiled.find(xml); found != compiled.end()) {
                        compiledMaterial = found->second;
                        ++reused;
                    } else {
                        const auto path = prism::materialx::PublishMaterialXDocument(xml, cache, "obj_");
                        compiledMaterial = std::make_shared<MDLMaterial>(path.string(), *mdl_context, libraries,
                                                                         mdlLibraries, m_materials,
                                                                         prism::obj::kMaterialName);
                        if (compiledMaterial->is_valid()) compiled.emplace(xml, compiledMaterial);
                        else failure = "MDL compilation failed (" + path.string() + ")";
                    }
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                if (compiledMaterial && compiledMaterial->is_valid()) {
                    materials[m] = compiledMaterial.get();
                    m_ownedMdl.push_back(std::move(compiledMaterial));
                    ++m_materialxCount;
                } else {
                    useErrorMaterial(source, m, failure);
                }
            }
            for (auto const& [statement, material] : unsupported)
                std::cerr << "PrismRT OBJ: MTL " << statement << " is not supported; ignored (first in material '"
                          << material << "')\n";
            std::cout << "OBJ: " << m_materialxCount << " MaterialX material(s) (" << compiled.size()
                      << " compiled, " << reused << " reused), " << m_errorCount << " failed, in "
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
                    PXR_NS::ArResolvedPath(std::filesystem::absolute(path).string()), PXR_NS::ArResolverContext(), "",
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
            materials[m] = ErrorMaterial::Shared();
            ++m_errorCount;
        }
};
