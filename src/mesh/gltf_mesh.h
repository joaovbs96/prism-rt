#pragma once

// STD Lib deps
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

// External Deps
#include <tiny_gltf.h>
#include <MaterialXFormat/XmlIo.h>

#include <prism/material.h>

// internal deps
#include "../geometry.h"
#include "../float3.h"
#include "../float2.h"
#include "../transforms.h"
#include "../material.h"
#include "../mdl_material.h"
#include "../resource_limits.h"
#include "../color/color_management.h"
#include "../materialx/gltf_material.h"
#include "../materialx/cached_document.h"
#include "../materialx/material_resource.h"

// glTF 2.0 scenes (.gltf, .glb), read for prism::gltf::Load
// (src/gltf/gltf_loader.cpp): the merged world-space triangles and their
// materials as MaterialX gltf_pbr documents in the material cache
// (docs/gltf.md), which the scene compiles through MDL at commit. A material
// whose conversion fails renders with the magenta error material, after a
// one-line warning.
class glTFMesh : public Mesh {
    public:
        // The document of a glTF material (-1: the default material), or why it
        // has none.
        struct MaterialSource {
            std::optional<prism::MaterialDesc> desc;
            std::string failure;
        };

        // materialSettings: the renderer's colour management (vertex colours and
        // material colours convert into its working space) and cache directory.
        // Without MaterialX libraries every material is the error material.
        glTFMesh(const std::filesystem::path &file_path, bool materialx,
                 prism::MaterialSettings materialSettings)
            : m_path(file_path), m_materials(std::move(materialSettings))
        {
            if (!m_materials.color) throw std::invalid_argument("glTFMesh needs a colour management");
            tinygltf::TinyGLTF loader;
            // Materials read the encoded image files (materialized into the
            // cache); pixels are never decoded here.
            loader.SetImagesAsIs(true);
            std::string err, warn;
            const bool ret = file_path.extension() == ".glb"
                ? loader.LoadBinaryFromFile(&m_model, &err, &warn, file_path.string())
                : loader.LoadASCIIFromFile(&m_model, &err, &warn, file_path.string());
            if (!warn.empty()) std::cout << "Warning: " << warn << std::endl;
            if (!err.empty()) std::cout << "Error: " << err << std::endl;
            if (!ret) throw std::runtime_error("Failed to parse glTF file: " + file_path.string());

            for (auto const& extension : prism::gltf::UnsupportedExtensions(m_model))
                std::cerr << "PrismRT glTF: extension " << extension << " is not supported; ignored\n";

            // Process scene hierarchy and calculate node transforms
            processSceneNodes(m_model);

            // Materials first: the geometry's texture coordinates follow
            // the convention of the material drawn with them.
            buildMaterials(materialx);

            int n_faces = 0;
            for (size_t node_idx = 0; node_idx < m_model.nodes.size(); ++node_idx) {
                const tinygltf::Node& node = m_model.nodes[node_idx];
                if (node.mesh >= 0 && node.mesh < int(m_model.meshes.size())) {
                    const tinygltf::Mesh& mesh = m_model.meshes[size_t(node.mesh)];
                    processMeshPrimitives(m_model, mesh, nodeTransforms[node_idx], n_faces);
                }
            }
            if (std::all_of(single_sided.begin(), single_sided.end(), [](uint8_t s) { return s != 0; })) {
                single_sided_all = !single_sided.empty();
                single_sided.clear();
            } else if (std::none_of(single_sided.begin(), single_sided.end(), [](uint8_t s) { return s != 0; })) {
                single_sided.clear();
            }
            std::cout << "glTF: " << vertices.size() << " vertices, " << indices.size() / 3 << " triangles, "
                      << m_sources.size() << " material(s)\n";
            // Encoded and decoded images are no longer needed.
            m_model = tinygltf::Model();
        }

        // Materials converted to MaterialX, and those rendered with the error
        // material.
        size_t materialxMaterialCount() const { return m_materialxCount; }
        size_t errorMaterialCount() const { return m_errorCount; }
        // By glTF material (-1: the default), the materials primitives use.
        std::map<int, MaterialSource> const& materialSources() const { return m_sources; }

    private:
        std::filesystem::path m_path;
        prism::MaterialSettings m_materials;
        tinygltf::Model m_model;
        std::vector<TransformMatrix> nodeTransforms;
        std::map<int, MaterialSource> m_sources;
        std::map<int, std::string> m_imageFiles;
        size_t m_materialxCount = 0, m_errorCount = 0;
        // Primvar layout (Mesh::primvars), -1: absent.
        int m_texcoord1Offset = -1, m_color0Offset = -1;

        // ---------------------------------------------------------------
        // Materials
        // ---------------------------------------------------------------

        void buildMaterials(bool materialx)
        {
            // The materials primitives draw with (-1: the glTF default
            // material), and whether one of them has vertex colours.
            std::map<int, bool> used;
            for (auto const& mesh : m_model.meshes)
                for (auto const& primitive : mesh.primitives) {
                    if (primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;
                    const int material = primitive.material < int(m_model.materials.size()) ? primitive.material : -1;
                    used[material] = used[material] || primitive.attributes.contains("COLOR_0");
                }
            if (!materialx) {
                for (auto const& [m, vertexColor] : used) useErrorMaterial(m, "no MDL context or MaterialX libraries");
                return;
            }

            const auto start = std::chrono::steady_clock::now();
            const std::filesystem::path cache = m_materials.cacheDirectory / "materials";
            std::set<std::string> documents;
            std::set<std::string> warned;
            size_t reused = 0;
            bool texcoord1 = false, color0 = false;
            for (auto const& [m, vertexColor] : used) {
                std::optional<prism::MaterialDesc> material;
                std::string failure;
                try {
                    prism::gltf::MaterialConversionOptions options;
                    options.vertexColor = vertexColor;
                    options.imageFile = [&](int image) { return imageFile(image, cache); };
                    options.fromLinearRec709 = [colors = m_materials.color](std::array<float, 3> c) {
                        const Float3 w = colors->FromRec709(Float3(c[0], c[1], c[2]));
                        return std::array<float, 3>{w.x(), w.y(), w.z()};
                    };
                    auto conversion = prism::gltf::ConvertGltfMaterial(m_model, m, options);
                    for (auto const& warning : conversion.warnings)
                        if (warned.insert(warning).second) std::cerr << "PrismRT glTF: " << warning << "\n";
                    const std::string xml = MaterialX::writeToXmlString(conversion.document);
                    // Equal documents publish to one file, which the loader
                    // adds once.
                    const auto path = prism::materialx::PublishMaterialXDocument(xml, cache, "gltf_");
                    if (!documents.insert(path.string()).second) ++reused;
                    material.emplace();
                    material->kind = prism::MaterialDesc::Kind::MaterialXFile;
                    material->path = path.string();
                    material->materialName = prism::gltf::kMaterialName;
                    // Textures resolve beside the cached documents (as for USD).
                    material->searchPath = cache.string();
                    material->name = materialLabel(m);
                    texcoord1 = texcoord1 || conversion.usesTexcoord1;
                    color0 = color0 || conversion.usesVertexColor;
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                if (material) {
                    m_sources[m].desc = std::move(material);
                    ++m_materialxCount;
                } else {
                    useErrorMaterial(m, failure);
                }
            }
            if (texcoord1) addPrimvar(prism::gltf::kTexcoord1Primvar, 2, &m_texcoord1Offset);
            if (color0) addPrimvar(prism::gltf::kColor0Primvar, 4, &m_color0Offset);
            std::cout << "glTF: " << m_materialxCount << " MaterialX material(s) (" << documents.size()
                      << " documents, " << reused << " reused), " << m_errorCount << " failed to convert, in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s\n";
        }

        // A material that cannot load renders magenta (as a failed USD
        // material does), with a one-line warning naming it and the reason.
        void useErrorMaterial(int m, const std::string& reason)
        {
            std::cerr << "PrismRT glTF: material " << materialLabel(m) << " failed to load ("
                      << (reason.empty() ? std::string("unknown error") : reason)
                      << "); rendering it with the magenta error material\n";
            m_sources[m].failure = reason.empty() ? std::string("unknown error") : reason;
            ++m_errorCount;
        }

        void addPrimvar(const char* name, int components, int* offset)
        {
            *offset = primvarStride;
            primvars.push_back({name, components, primvarStride});
            primvarStride += components;
        }

        std::string materialLabel(int m) const
        {
            return m >= 0 ? std::to_string(m) + " '" + m_model.materials[size_t(m)].name + "'" : "(default)";
        }

        // The cached copy of an image's encoded bytes (file name beside the
        // cached documents), empty when it has none.
        std::string imageFile(int index, const std::filesystem::path& cache)
        {
            if (auto found = m_imageFiles.find(index); found != m_imageFiles.end()) return found->second;
            std::string name;
            const tinygltf::Image& image = m_model.images[size_t(index)];
            if (!image.image.empty()) {
                std::string extension = image.mimeType == "image/jpeg" ? ".jpg"
                    : image.mimeType == "image/png" ? ".png" : image.mimeType == "image/webp" ? ".webp" : "";
                if (extension.empty() && !image.uri.empty() && image.uri.rfind("data:", 0) != 0)
                    extension = std::filesystem::path(image.uri).extension().string();
                auto resource = prism::materialx::PublishMaterialResourceBytes(
                    reinterpret_cast<const char*>(image.image.data()), image.image.size(),
                    extension.empty() ? ".bin" : extension, cache);
                name = resource->path.filename().generic_string();
            }
            m_imageFiles.emplace(index, name);
            return name;
        }

        // ---------------------------------------------------------------
        // Geometry
        // ---------------------------------------------------------------

        void processSceneNodes(const tinygltf::Model& model) {
            nodeTransforms.resize(model.nodes.size());
            for (size_t i = 0; i < model.nodes.size(); ++i)
                nodeTransforms[i].identity();
            const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
            if (sceneIndex < int(model.scenes.size())) {
                for (int nodeIndex : model.scenes[size_t(sceneIndex)].nodes) {
                    TransformMatrix parentTransform;  // Identity for root nodes
                    processNode(model, nodeIndex, parentTransform);
                }
            } else {
                std::cout << "Warning: No valid scene found!" << std::endl;
            }
        }

        void processNode(const tinygltf::Model& model, int nodeIndex, const TransformMatrix& parentTransform) {
            if (nodeIndex < 0 || nodeIndex >= int(model.nodes.size())) return;
            const tinygltf::Node& node = model.nodes[size_t(nodeIndex)];
            TransformMatrix localTransform;
            getNodeTransform(node, localTransform);
            nodeTransforms[size_t(nodeIndex)] = parentTransform;
            nodeTransforms[size_t(nodeIndex)].multiply(localTransform);
            for (int childIndex : node.children)
                processNode(model, childIndex, nodeTransforms[size_t(nodeIndex)]);
        }

        void getNodeTransform(const tinygltf::Node& node, TransformMatrix& transform) {
            transform.identity();
            if (node.matrix.size() == 16) {
                // glTF uses column-major, we need row-major
                float* m = transform.getMatrix();
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 4; ++col)
                        m[row * 4 + col] = static_cast<float>(node.matrix[col * 4 + row]);
            } else {
                // glTF specifies T * R * S
                if (node.translation.size() == 3)
                    transform.translate(float(node.translation[0]), float(node.translation[1]), float(node.translation[2]));
                if (node.rotation.size() == 4)
                    applyQuaternionRotation(transform, float(node.rotation[0]), float(node.rotation[1]),
                                            float(node.rotation[2]), float(node.rotation[3]));
                if (node.scale.size() == 3)
                    transform.scale(float(node.scale[0]), float(node.scale[1]), float(node.scale[2]));
            }
        }

        void applyQuaternionRotation(TransformMatrix& transform, float x, float y, float z, float w) {
            TransformMatrix rotMatrix;
            float* m = rotMatrix.getMatrix();
            float xx = x * x, yy = y * y, zz = z * z;
            float xy = x * y, xz = x * z, yz = y * z;
            float wx = w * x, wy = w * y, wz = w * z;
            m[0] = 1 - 2 * (yy + zz);   m[1] = 2 * (xy - wz);       m[2] = 2 * (xz + wy);       m[3] = 0;
            m[4] = 2 * (xy + wz);       m[5] = 1 - 2 * (xx + zz);   m[6] = 2 * (yz - wx);       m[7] = 0;
            m[8] = 2 * (xz - wy);       m[9] = 2 * (yz + wx);       m[10] = 1 - 2 * (xx + yy);  m[11] = 0;
            m[12] = 0;                  m[13] = 0;                  m[14] = 0;                  m[15] = 1;
            transform.multiply(rotMatrix);
        }

        // An accessor as `components` floats per element: any component
        // type (normalized integers mapped as the spec says), byteStride and
        // sparse substitution honoured. Missing components are zero. False
        // (with a warning) when the accessor is unusable.
        static bool readAccessor(const tinygltf::Model& model, int index, int components, std::vector<float>* out)
        {
            if (index < 0 || index >= int(model.accessors.size())) return false;
            const tinygltf::Accessor& accessor = model.accessors[size_t(index)];
            const int available = tinygltf::GetNumComponentsInType(uint32_t(accessor.type));
            const int componentSize = tinygltf::GetComponentSizeInBytes(uint32_t(accessor.componentType));
            if (available <= 0 || componentSize <= 0) return false;
            const size_t count = accessor.count;
            out->assign(count * size_t(components), 0.0f);
            auto decode = [&](const unsigned char* element, float* target) {
                for (int c = 0; c < std::min(available, components); ++c) {
                    const unsigned char* p = element + size_t(c) * size_t(componentSize);
                    float value = 0.0f;
                    switch (accessor.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_FLOAT: std::memcpy(&value, p, 4); break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        value = accessor.normalized ? *p / 255.0f : float(*p); break;
                    case TINYGLTF_COMPONENT_TYPE_BYTE: {
                        const float v = float(*reinterpret_cast<const int8_t*>(p));
                        value = accessor.normalized ? std::max(v / 127.0f, -1.0f) : v; break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                        uint16_t v; std::memcpy(&v, p, 2);
                        value = accessor.normalized ? v / 65535.0f : float(v); break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_SHORT: {
                        int16_t v; std::memcpy(&v, p, 2);
                        value = accessor.normalized ? std::max(v / 32767.0f, -1.0f) : float(v); break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                        uint32_t v; std::memcpy(&v, p, 4); value = float(v); break;
                    }
                    default: break;
                    }
                    target[c] = value;
                }
            };
            const size_t elementSize = size_t(available) * size_t(componentSize);
            auto view = [&](int bufferView, size_t byteOffset, size_t stride, size_t elements,
                            const unsigned char** data) {
                if (bufferView < 0 || bufferView >= int(model.bufferViews.size())) return false;
                const tinygltf::BufferView& bv = model.bufferViews[size_t(bufferView)];
                if (bv.buffer < 0 || bv.buffer >= int(model.buffers.size())) return false;
                const tinygltf::Buffer& buffer = model.buffers[size_t(bv.buffer)];
                const size_t begin = bv.byteOffset + byteOffset;
                if (elements && begin + stride * (elements - 1) + elementSize > buffer.data.size()) return false;
                *data = buffer.data.data() + begin;
                return true;
            };
            if (accessor.bufferView >= 0) {
                const tinygltf::BufferView& bv = model.bufferViews[size_t(accessor.bufferView)];
                const size_t stride = bv.byteStride ? bv.byteStride : elementSize;
                const unsigned char* data = nullptr;
                if (!view(accessor.bufferView, accessor.byteOffset, stride, count, &data)) {
                    std::cerr << "PrismRT glTF: accessor " << index << " is out of bounds; ignored\n";
                    return false;
                }
                for (size_t i = 0; i < count; ++i)
                    decode(data + i * stride, out->data() + i * size_t(components));
            }
            if (accessor.sparse.isSparse) {
                const auto& sparse = accessor.sparse;
                const int indexSize = tinygltf::GetComponentSizeInBytes(uint32_t(sparse.indices.componentType));
                const unsigned char* indexData = nullptr;
                const unsigned char* valueData = nullptr;
                const size_t sparseCount = size_t(sparse.count);
                if (indexSize <= 0 ||
                    !view(sparse.indices.bufferView, sparse.indices.byteOffset, size_t(indexSize), 0, &indexData) ||
                    !view(sparse.values.bufferView, sparse.values.byteOffset, elementSize, sparseCount, &valueData)) {
                    std::cerr << "PrismRT glTF: sparse accessor " << index << " is invalid; ignored\n";
                    return false;
                }
                for (size_t i = 0; i < sparseCount; ++i) {
                    uint32_t target = 0;
                    if (indexSize == 1) target = indexData[i];
                    else if (indexSize == 2) { uint16_t v; std::memcpy(&v, indexData + 2 * i, 2); target = v; }
                    else std::memcpy(&target, indexData + 4 * i, 4);
                    if (target < count) decode(valueData + i * elementSize, out->data() + size_t(target) * size_t(components));
                }
            }
            return true;
        }

        void processMeshPrimitives(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
                                   TransformMatrix& nodeTransform, int& n_faces) {
            for (const auto& primitive : mesh.primitives) {
                if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                    std::cout << "Skipping non-triangle primitive" << std::endl;
                    continue;
                }
                auto attribute = [&](const char* name) {
                    auto found = primitive.attributes.find(name);
                    return found == primitive.attributes.end() ? -1 : found->second;
                };
                std::vector<float> positions;
                if (!readAccessor(model, attribute("POSITION"), 3, &positions)) {
                    std::cout << "No usable POSITION attribute found" << std::endl;
                    continue;
                }
                const int material = primitive.material < int(model.materials.size()) ? primitive.material : -1;
                const size_t vertexOffset = vertices.size();
                const size_t vertexCount = positions.size() / 3;
                const bool identity = nodeTransform.isIdentity();

                for (size_t i = 0; i < vertexCount; ++i)
                    vertices.push_back(nodeTransform.transformPoint(
                        Float3(positions[3 * i], positions[3 * i + 1], positions[3 * i + 2])));

                std::vector<float> data;
                if (readAccessor(model, attribute("NORMAL"), 3, &data) && data.size() == 3 * vertexCount) {
                    for (size_t i = 0; i < vertexCount; ++i) {
                        Float3 normal(data[3 * i], data[3 * i + 1], data[3 * i + 2]);
                        if (!identity) {
                            Float4 transformed = nodeTransform.transformPoint(Float4(normal.x(), normal.y(), normal.z(), 0.0f));
                            normal = Float3(transformed.x(), transformed.y(), transformed.z()).normalize();
                        }
                        normals.push_back(normal);
                    }
                } else {
                    normals.resize(vertices.size(), Float3(0.0f, 1.0f, 0.0f));
                }

                // MaterialX materials take V up (gltf_material.h).
                auto uv = [](float u, float v) { return Float2(u, 1.0f - v); };
                if (readAccessor(model, attribute("TEXCOORD_0"), 2, &data) && data.size() == 2 * vertexCount) {
                    for (size_t i = 0; i < vertexCount; ++i) texcoords.push_back(uv(data[2 * i], data[2 * i + 1]));
                } else {
                    texcoords.resize(vertices.size(), Float2(0.0f, 0.0f));
                }

                if (primvarStride > 0) {
                    primvarValues.resize(vertices.size() * size_t(primvarStride), 0.0f);
                    float* values = primvarValues.data() + vertexOffset * size_t(primvarStride);
                    if (m_texcoord1Offset >= 0 && readAccessor(model, attribute("TEXCOORD_1"), 2, &data) &&
                        data.size() == 2 * vertexCount) {
                        for (size_t i = 0; i < vertexCount; ++i) {
                            const Float2 t = uv(data[2 * i], data[2 * i + 1]);
                            values[i * size_t(primvarStride) + size_t(m_texcoord1Offset)] = t.u();
                            values[i * size_t(primvarStride) + size_t(m_texcoord1Offset) + 1] = t.v();
                        }
                    }
                    if (m_color0Offset >= 0) {
                        const bool authored = readAccessor(model, attribute("COLOR_0"), 4, &data) &&
                            data.size() == 4 * vertexCount;
                        const auto accessor = attribute("COLOR_0");
                        const bool rgb = accessor >= 0 && model.accessors[size_t(accessor)].type == TINYGLTF_TYPE_VEC3;
                        auto const& colors = m_materials.color;
                        for (size_t i = 0; i < vertexCount; ++i) {
                            float* target = values + i * size_t(primvarStride) + size_t(m_color0Offset);
                            if (!authored) { target[0] = target[1] = target[2] = target[3] = 1.0f; continue; }
                            // Linear Rec.709 into the working space, once.
                            const Float3 c = colors ? colors->FromRec709(Float3(data[4 * i], data[4 * i + 1], data[4 * i + 2]))
                                                    : Float3(data[4 * i], data[4 * i + 1], data[4 * i + 2]);
                            target[0] = c.x(); target[1] = c.y(); target[2] = c.z();
                            target[3] = rgb ? 1.0f : data[4 * i + 3];
                        }
                    }
                }

                const bool singleSided = material >= 0 && !model.materials[size_t(material)].doubleSided;
                auto addTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
                    if (a >= vertexCount || b >= vertexCount || c >= vertexCount) return;
                    material_mapping[n_faces++] = material;
                    single_sided.push_back(singleSided ? 1 : 0);
                    indices.push_back(uint32_t(vertexOffset) + a);
                    indices.push_back(uint32_t(vertexOffset) + b);
                    indices.push_back(uint32_t(vertexOffset) + c);
                };
                if (primitive.indices >= 0) {
                    const tinygltf::Accessor& accessor = model.accessors[size_t(primitive.indices)];
                    const int size = tinygltf::GetComponentSizeInBytes(uint32_t(accessor.componentType));
                    if (accessor.bufferView < 0 || size <= 0) continue;
                    const tinygltf::BufferView& bv = model.bufferViews[size_t(accessor.bufferView)];
                    const tinygltf::Buffer& buffer = model.buffers[size_t(bv.buffer)];
                    const size_t begin = bv.byteOffset + accessor.byteOffset;
                    if (begin + accessor.count * size_t(size) > buffer.data.size()) {
                        std::cerr << "PrismRT glTF: index accessor " << primitive.indices << " is out of bounds; skipped\n";
                        continue;
                    }
                    const unsigned char* bytes = buffer.data.data() + begin;
                    auto index = [&](size_t i) -> uint32_t {
                        if (size == 1) return bytes[i];
                        if (size == 2) { uint16_t v; std::memcpy(&v, bytes + 2 * i, 2); return v; }
                        uint32_t v; std::memcpy(&v, bytes + 4 * i, 4); return v;
                    };
                    for (size_t i = 0; i + 2 < accessor.count; i += 3)
                        addTriangle(index(i), index(i + 1), index(i + 2));
                } else {
                    for (size_t i = 0; i + 2 < vertexCount; i += 3)
                        addTriangle(uint32_t(i), uint32_t(i + 1), uint32_t(i + 2));
                }
            }
        }
};
