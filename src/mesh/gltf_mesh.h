#pragma once

// STD Lib deps
#include <iostream>
#include <filesystem>
#include <stdexcept>

// External Deps
#include <tiny_gltf.h>

// internal deps
#include "../geometry.h"
#include "../float3.h"
#include "../float2.h"
#include "../transforms.h"
#include "../material.h"

class glTFMesh : public Mesh {
    private:
        std::vector<TransformMatrix> nodeTransforms;  // Store transforms for each node

        void processSceneNodes(const tinygltf::Model& model) {
            // Initialize transforms for all nodes
            nodeTransforms.resize(model.nodes.size());
            for (size_t i = 0; i < model.nodes.size(); ++i) {
                nodeTransforms[i].identity();
            }

            std::cout << "Scene info: " << model.scenes.size() << " scenes, " << model.nodes.size() << " nodes" << std::endl;

            // Process default scene (or first scene if no default)
            int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
            if (sceneIndex < model.scenes.size()) {
                const tinygltf::Scene& scene = model.scenes[sceneIndex];
                std::cout << "Processing scene " << sceneIndex << " with " << scene.nodes.size() << " root nodes" << std::endl;

                // Process root nodes
                for (int nodeIndex : scene.nodes) {
                    TransformMatrix parentTransform;  // Identity for root nodes
                    processNode(model, nodeIndex, parentTransform);
                }
            } else {
                std::cout << "Warning: No valid scene found!" << std::endl;
            }
        }

        void processNode(const tinygltf::Model& model, int nodeIndex, const TransformMatrix& parentTransform) {
            if (nodeIndex < 0 || nodeIndex >= model.nodes.size()) return;

            const tinygltf::Node& node = model.nodes[nodeIndex];

            // Calculate local transform for this node
            TransformMatrix localTransform;
            getNodeTransform(node, localTransform);

            // Debug output for non-identity transforms
            if (!localTransform.isIdentity()) {
                std::cout << "Node " << nodeIndex << " has local transform:" << std::endl;
                if (node.translation.size() == 3) {
                    std::cout << "  Translation: (" << node.translation[0] << ", " << node.translation[1] << ", " << node.translation[2] << ")" << std::endl;
                }
                if (node.rotation.size() == 4) {
                    std::cout << "  Rotation: (" << node.rotation[0] << ", " << node.rotation[1] << ", " << node.rotation[2] << ", " << node.rotation[3] << ")" << std::endl;
                }
                if (node.scale.size() == 3) {
                    std::cout << "  Scale: (" << node.scale[0] << ", " << node.scale[1] << ", " << node.scale[2] << ")" << std::endl;
                }
                if (node.matrix.size() == 16) {
                    std::cout << "  Has 4x4 matrix" << std::endl;
                }
            }

            // Calculate world transform (parent * local)
            nodeTransforms[nodeIndex] = parentTransform;
            nodeTransforms[nodeIndex].multiply(localTransform);

            // Process child nodes recursively
            for (int childIndex : node.children) {
                processNode(model, childIndex, nodeTransforms[nodeIndex]);
            }
        }

        void getNodeTransform(const tinygltf::Node& node, TransformMatrix& transform) {
            transform.identity();

            if (node.matrix.size() == 16) {
                // Node has a transformation matrix (glTF uses column-major, we need row-major)
                float* m = transform.getMatrix();
                for (int row = 0; row < 4; ++row) {
                    for (int col = 0; col < 4; ++col) {
                        m[row * 4 + col] = static_cast<float>(node.matrix[col * 4 + row]);
                    }
                }
            } else {
                // Node has TRS (Translation, Rotation, Scale) components
                // glTF specifies T * R * S order, so we apply in reverse: S, then R, then T

                // Apply translation first
                if (node.translation.size() == 3) {
                    transform.translate(
                        static_cast<float>(node.translation[0]),
                        static_cast<float>(node.translation[1]),
                        static_cast<float>(node.translation[2])
                    );
                }

                // Apply rotation second (quaternion)
                if (node.rotation.size() == 4) {
                    applyQuaternionRotation(transform,
                        static_cast<float>(node.rotation[0]), // x
                        static_cast<float>(node.rotation[1]), // y
                        static_cast<float>(node.rotation[2]), // z
                        static_cast<float>(node.rotation[3])  // w
                    );
                }

                // Apply scale last
                if (node.scale.size() == 3) {
                    transform.scale(
                        static_cast<float>(node.scale[0]),
                        static_cast<float>(node.scale[1]),
                        static_cast<float>(node.scale[2])
                    );
                }

            }
        }

        void applyQuaternionRotation(TransformMatrix& transform, float x, float y, float z, float w) {
            // Convert quaternion to rotation matrix
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

        void processMeshPrimitives(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
                                  TransformMatrix& nodeTransform, int& n_faces) {
            for (const auto& primitive : mesh.primitives) {
                if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                    std::cout << "Skipping non-triangle primitive" << std::endl;
                    continue;
                }

                // Extract vertex positions
                auto posIt = primitive.attributes.find("POSITION");
                if (posIt == primitive.attributes.end()) {
                    std::cout << "No POSITION attribute found" << std::endl;
                    continue;
                }

                const tinygltf::Accessor& posAccessor = model.accessors[posIt->second];
                const tinygltf::BufferView& posBufferView = model.bufferViews[posAccessor.bufferView];
                const tinygltf::Buffer& posBuffer = model.buffers[posBufferView.buffer];

                if (posAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    std::cout << "Position data is not float type" << std::endl;
                    continue;
                }

                const float* positions = reinterpret_cast<const float*>(&posBuffer.data[posBufferView.byteOffset + posAccessor.byteOffset]);

                size_t vertexOffset = vertices.size();
                int vertexCount = posAccessor.count;

                // Copy and transform vertices
                std::cout << "  Processing " << vertexCount << " vertices..." << std::endl;
                std::cout << "  Node transform isIdentity: " << (nodeTransform.isIdentity() ? "YES" : "NO") << std::endl;

                for (int i = 0; i < vertexCount; ++i) {
                    Float3 vertex(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);

                    // Apply node transformation
                    if (true) {
                    // if (!nodeTransform.isIdentity()) {
                        Float3 originalVertex = vertex;
                        vertex = nodeTransform.transformPoint(vertex);

                        // Debug output for first few vertex transformations
                        if (i < 3) {
                            std::cout << "    Vertex " << i << ": (" << originalVertex.x() << ", " << originalVertex.y() << ", " << originalVertex.z()
                                      << ") -> (" << vertex.x() << ", " << vertex.y() << ", " << vertex.z() << ")" << std::endl;
                        }
                    } else {
                        // Double-check: if it's supposed to be identity, verify no transformation occurs
                        if (i < 3) {
                            Float3 testTransformed = nodeTransform.transformPoint(vertex);
                            bool actuallyIdentity = (testTransformed.x() == vertex.x() &&
                                                   testTransformed.y() == vertex.y() &&
                                                   testTransformed.z() == vertex.z());

                            std::cout << "    Vertex " << i << ": (" << vertex.x() << ", " << vertex.y() << ", " << vertex.z()
                                      << ") (no transform applied)" << std::endl;

                            if (!actuallyIdentity) {
                                std::cout << "    ERROR: Identity transform actually transforms to ("
                                          << testTransformed.x() << ", " << testTransformed.y() << ", " << testTransformed.z() << ")" << std::endl;
                                std::cout << "    Matrix marked as identity but isn't:" << std::endl;
                                nodeTransform.print();
                            }
                        }
                    }

                    vertices.push_back(vertex);
                }

                // Extract normals
                auto normalIt = primitive.attributes.find("NORMAL");
                if (normalIt != primitive.attributes.end()) {
                    const tinygltf::Accessor& normalAccessor = model.accessors[normalIt->second];
                    const tinygltf::BufferView& normalBufferView = model.bufferViews[normalAccessor.bufferView];
                    const tinygltf::Buffer& normalBuffer = model.buffers[normalBufferView.buffer];

                    if (normalAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        const float* normals_data = reinterpret_cast<const float*>(
                            &normalBuffer.data[normalBufferView.byteOffset + normalAccessor.byteOffset]);

                        for (size_t i = 0; i < normalAccessor.count; ++i) {
                            Float3 normal(normals_data[i * 3 + 0], normals_data[i * 3 + 1], normals_data[i * 3 + 2]);

                            // Transform normal (using vector transformation)
                            if (!nodeTransform.isIdentity()) {
                                // Transform normal as a vector (w=0)
                                Float4 normalH = Float4(normal.x(), normal.y(), normal.z(), 0.0f);
                                Float4 transformedNormal = nodeTransform.transformPoint(normalH);
                                normal = Float3(transformedNormal.x(), transformedNormal.y(), transformedNormal.z()).normalize();
                            }

                            normals.push_back(normal);
                        }
                    }
                } else {
                    // If no normals, fill with dummy values to match vertex count
                    size_t currentVertexCount = vertices.size() - normals.size();
                    for (size_t i = 0; i < currentVertexCount; ++i) {
                        normals.push_back(Float3(0.0f, 1.0f, 0.0f)); // Default up normal
                    }
                }

                // Extract texture coordinates
                auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
                if (texCoordIt != primitive.attributes.end()) {
                    const tinygltf::Accessor& texAccessor = model.accessors[texCoordIt->second];
                    const tinygltf::BufferView& texBufferView = model.bufferViews[texAccessor.bufferView];
                    const tinygltf::Buffer& texBuffer = model.buffers[texBufferView.buffer];

                    if (texAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        const float* texcoords_data = reinterpret_cast<const float*>(
                            &texBuffer.data[texBufferView.byteOffset + texAccessor.byteOffset]);

                        for (size_t i = 0; i < texAccessor.count; ++i) {
                            texcoords.push_back(Float2(
                                texcoords_data[i * 2 + 0],
                                texcoords_data[i * 2 + 1]
                            ));
                        }
                    }
                } else {
                    // If no texture coordinates, fill with dummy values
                    size_t currentVertexCount = vertices.size() - texcoords.size();
                    for (size_t i = 0; i < currentVertexCount; ++i) {
                        texcoords.push_back(Float2(0.0f, 0.0f)); // Default UV
                    }
                }

                // Extract indices
                if (primitive.indices >= 0) {
                    const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
                    const tinygltf::BufferView& indexBufferView = model.bufferViews[indexAccessor.bufferView];
                    const tinygltf::Buffer& indexBuffer = model.buffers[indexBufferView.buffer];

                    const unsigned char* indexData = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];

                    int triangleCount = indexAccessor.count / 3;

                    for (int i = 0; i < triangleCount; i++) {
                        material_mapping[n_faces] = primitive.material;
                        n_faces++;

                        // Handle different index component types
                        for (int j = 0; j < 3; ++j) {
                            unsigned int index = 0;
                            int indexOffset = i * 3 + j;

                            switch (indexAccessor.componentType) {
                                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                                    index = static_cast<unsigned int>(indexData[indexOffset]);
                                    break;
                                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                                    index = static_cast<unsigned int>(reinterpret_cast<const unsigned short*>(indexData)[indexOffset]);
                                    break;
                                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                                    index = reinterpret_cast<const unsigned int*>(indexData)[indexOffset];
                                    break;
                                default:
                                    std::cout << "Unsupported index component type" << std::endl;
                                    continue;
                            }

                            // Add vertex offset for this primitive
                            indices.push_back(index + vertexOffset);
                        }
                    }
                } else {
                  // Non-indexed geometry - create indices automatically
                    size_t currentVertexCount = vertices.size() - vertexOffset;
                    for (size_t i = 0; i < currentVertexCount; ++i) {
                        indices.push_back(static_cast<unsigned int>(vertexOffset + i));
                        // TODO: this might be wrong? What would be an example of non-indexed geom?
                        if (i % 3 == 0){
                            material_mapping[n_faces] = primitive.material;
                            n_faces++;
                        }
                    }
                }
            }
        }

    public:
        glTFMesh(const std::filesystem::path &file_path) {
            tinygltf::TinyGLTF loader;
            tinygltf::Model model;
            std::string err;
            std::string warn;

            bool ret = false;
            if (file_path.extension() == ".glb") {
                ret = loader.LoadBinaryFromFile(&model, &err, &warn, file_path.string());
            } else {
                ret = loader.LoadASCIIFromFile(&model, &err, &warn, file_path.string());
            }

            if (!warn.empty()) {
                std::cout << "Warning: " << warn << std::endl;
            }

            if (!err.empty()) {
                std::cout << "Error: " << err << std::endl;
            }

            if (!ret) {
                throw std::runtime_error("Failed to parse glTF file: " + file_path.string());
            }

            for (const auto& extension : model.extensionsUsed) {
                if (extension != "KHR_materials_emissive_strength" &&
                    extension != "KHR_materials_transmission")
                    std::cerr << "PrismRT: unsupported glTF extension " << extension
                              << "; using the existing core material/geometry behavior\n";
            }

            // Process scene hierarchy and calculate node transforms
            processSceneNodes(model);

            int n_faces = 0;

            // Process meshes through their node references
            for (size_t node_idx = 0; node_idx < model.nodes.size(); ++node_idx) {
                const tinygltf::Node& node = model.nodes[node_idx];

                if (node.mesh >= 0 && node.mesh < model.meshes.size()) {
                    const tinygltf::Mesh& mesh = model.meshes[node.mesh];
                    std::cout << "Processing mesh: " << mesh.name << " (Node: " << node_idx << ")" << std::endl;

                    processMeshPrimitives(model, mesh, nodeTransforms[node_idx], n_faces);

                    std::cout << "Mesh " << node.mesh << " at node " << node_idx << ": "
                              << vertices.size() << " total vertices, " << indices.size() / 3 << " total triangles" << std::endl;
                }
            }

            // Convert Materials from gltf/glb file
            // for each material in the scene file
            for (int m = 0; m < model.materials.size(); m++) {
                tinygltf::Material mp = model.materials[m];
                if (mp.alphaMode != "OPAQUE" || mp.pbrMetallicRoughness.baseColorFactor[3] != 1.0)
                    std::cerr << "PrismRT: glTF material '" << mp.name
                              << "' alpha/cutout settings are not supported by the legacy glTF shader\n";
                if (mp.occlusionTexture.index >= 0)
                    std::cerr << "PrismRT: glTF material '" << mp.name
                              << "' authored occlusion texture is ignored\n";
                if (mp.pbrMetallicRoughness.baseColorTexture.texCoord > 0 ||
                    mp.pbrMetallicRoughness.metallicRoughnessTexture.texCoord > 0 ||
                    mp.normalTexture.texCoord > 0 || mp.emissiveTexture.texCoord > 0)
                    std::cerr << "PrismRT: glTF material '" << mp.name
                              << "' requests additional UV sets; only TEXCOORD_0 is supported\n";
                gltfPBR* gltf_material = new gltfPBR();

                // Create Texture from image file or color value
                tinygltf::PbrMetallicRoughness pbr = mp.pbrMetallicRoughness;
                Texture<Color3>* base_texture;
                Color3 base_factor = Color3(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2]);
                if (hasImage(model, pbr.baseColorTexture)) {
                    tinygltf::Image image = getTextureImage(model, pbr.baseColorTexture);

                    if (image.uri.length() > 0) {
                        base_texture = new ImageMixTexture<Color3>(file_path.parent_path().concat("/").concat(image.uri), base_factor);
                    } else {
                        base_texture = new ImageMixTexture<Color3>(&image.image, image.width, image.height, image.component, base_factor);
                    }
                    base_texture->gammaCorrect(1.0 / 2.2);
                } else {
                    base_texture = new ColorTexture(base_factor);
                }
                gltf_material->setBaseColor(base_texture);

                // TODO: occlusion is currently ignored
                Texture<Float3>* orm_texture;
                Float3 orm_factor = Float3(0.0f, pbr.roughnessFactor, pbr.metallicFactor);
                if (hasImage(model, pbr.metallicRoughnessTexture)) {
                    tinygltf::Image image = getTextureImage(model, pbr.metallicRoughnessTexture);

                    if (image.uri.length() > 0) {
                        orm_texture = new ImageMixTexture<Float3>(file_path.parent_path().concat("/").concat(image.uri), orm_factor);
                    } else {
                        orm_texture = new ImageMixTexture<Float3>(&image.image, image.width, image.height, image.component, orm_factor);
                    }
                } else {
                    orm_texture = new ValueTexture<Float3>(orm_factor);
                }
                gltf_material->setORM(orm_texture);

                Texture<Float3>* normal_texture;
                if (hasImage(model, mp.normalTexture)) {
                    tinygltf::Image image = getTextureImage(model, mp.normalTexture);

                    if (image.uri.length() > 0) {
                        normal_texture = new ImageMixTexture<Float3>(file_path.parent_path().concat("/").concat(image.uri), orm_factor);
                    } else {
                        normal_texture = new ImageMixTexture<Float3>(&image.image, image.width, image.height, image.component, orm_factor);
                    }

                    gltf_material->setNormalTexture(normal_texture);
                }

                Texture<Color3>* emission_texture;
                Color3 emission_factor = Color3(mp.emissiveFactor[0], mp.emissiveFactor[1], mp.emissiveFactor[2]);
                float emission_strength = 1.0;
                if (mp.extensions.contains("KHR_materials_emissive_strength")) {
                    tinygltf::Value::Object emission = mp.extensions["KHR_materials_emissive_strength"].Get<tinygltf::Value::Object>();
                    emission_strength = getExtensionFactor(emission, "emissiveStrength", 1.0);
                }
                if (hasImage(model, mp.emissiveTexture)) {
                    tinygltf::Image image = getTextureImage(model, mp.emissiveTexture);

                    if (image.uri.length() > 0) {
                        emission_texture = new ImageMixTexture<Float3>(file_path.parent_path().concat("/").concat(image.uri), emission_factor);
                    } else {
                        emission_texture = new ImageMixTexture<Float3>(&image.image, image.width, image.height, image.component, emission_factor);
                    }
                    emission_texture->gammaCorrect(1.0 / 2.2);
                } else {
                    emission_texture = new ValueTexture<Color3>(emission_factor);
                }
                gltf_material->setEmission(emission_texture, emission_strength);

                if (mp.extensions.contains("KHR_materials_transmission")) {
                    tinygltf::Value::Object transmission = mp.extensions["KHR_materials_transmission"].Get<tinygltf::Value::Object>();

                    // Extract transmissionFactor (required parameter)
                    float transmission_factor = getExtensionFactor(transmission, "transmissionFactor", 0.0f);
                    Texture<float>* transmission_texture;
                    if (hasImage(model, transmission, "transmissionTexture")) {
                        tinygltf::Image image = getTextureImage(model, transmission, "transmissionTexture");

                        if (image.uri.length() > 0) {
                            transmission_texture = new ImageMixTexture<float>(file_path.parent_path().concat("/").concat(image.uri), transmission_factor);
                        } else {
                            transmission_texture = new ImageMixTexture<float>(&image.image, image.width, image.height, image.component, transmission_factor);
                        }
                    } else {
                        transmission_texture = new ValueTexture<float>(transmission_factor);
                    }
                    gltf_material->setTransmission(transmission_texture);
                }

                gltf_material->createBRDFs();

                // Assign material
                materials[m] = gltf_material;
            }
        }

        float getExtensionFactor(tinygltf::Value::Object gltf_obj, const char* key, float default_value) {
            // Extract transmissionFactor (required parameter)
            auto iterator_ = gltf_obj.find(key);
            if (iterator_ != gltf_obj.end() && iterator_->second.IsNumber()) {
                return float(iterator_->second.Get<double>());
            } else {
                return default_value;
            }
        }

        bool hasImage(tinygltf::Model& model, tinygltf::NormalTextureInfo& texinfo) {
            if (texinfo.index >= 0 && texinfo.index < model.textures.size()) {
                tinygltf::Texture& normal_texture = model.textures[texinfo.index];
                if (normal_texture.source >= 0 && normal_texture.source < model.images.size()) {
                    return true;
                }
            }
            return false;
        }

        bool hasImage(tinygltf::Model& model, tinygltf::TextureInfo& texinfo) {
            if (texinfo.index >= 0 && texinfo.index < model.textures.size()){
                tinygltf::Texture& texture = model.textures[texinfo.index];
                if (texture.source >= 0 && texture.source < model.images.size()) {
                    return true;
                }
            }
            return false;
        }

        bool hasImage(tinygltf::Model& model, tinygltf::Value::Object ext_obj, const char* key) {
            // Extract transmissionFactor (required parameter)
            auto iterator_ = ext_obj.find(key);
            if (iterator_ != ext_obj.end() && iterator_->second.IsObject()) {
                const tinygltf::Value::Object& texinfo = iterator_->second.Get<tinygltf::Value::Object>();
                // Get texture index
                auto index_it_ = texinfo.find("index");
                if (index_it_ != texinfo.end() && index_it_->second.IsInt()) {
                    int tex_index = index_it_->second.Get<int>();
                    tinygltf::Texture& texture = model.textures[tex_index];
                    if (texture.source >= 0 && texture.source < model.images.size()) {
                        return true;
                    }
                }
            }
            return false;
        }

        tinygltf::Image getTextureImage(tinygltf::Model& model, tinygltf::NormalTextureInfo& texinfo) {
            tinygltf::Texture& texture = model.textures[texinfo.index];
            return model.images[texture.source];
        }

        tinygltf::Image getTextureImage(tinygltf::Model& model, tinygltf::TextureInfo& texinfo) {
            tinygltf::Texture& texture = model.textures[texinfo.index];
            return model.images[texture.source];
        }

        tinygltf::Image getTextureImage(tinygltf::Model& model, tinygltf::Value::Object ext_obj, const char* key) {
            auto iterator_ = ext_obj.find(key);
            const tinygltf::Value::Object& texinfo = iterator_->second.Get<tinygltf::Value::Object>();
            auto index_it_ = texinfo.find("index");
            int tex_index = index_it_->second.Get<int>();
            tinygltf::Texture& texture = model.textures[tex_index];
            return model.images[texture.source];
        }
};
