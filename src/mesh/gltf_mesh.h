#pragma once

// STD Lib deps
#include <iostream>
#include <filesystem>

// External Deps
#include <tiny_gltf.h>

// internal deps
#include "geometry.h"
#include "float3.h"
#include "float2.h"
#include "transforms.h"
#include "material.h"

class glTFMesh : public Mesh {
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
                std::cout << "Failed to parse glTF file" << std::endl;
                return;
            }

            int n_faces = 0;
            for (size_t mesh_idx = 0; mesh_idx < model.meshes.size(); ++mesh_idx) {
                const tinygltf::Mesh& mesh = model.meshes[mesh_idx];
                std::cout << "Processing mesh: " << mesh.name << std::endl;

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

                    // Copy vertices
                    for (int i = 0; i < vertexCount; ++i) {
                        make_Float3<const float*>(vertices, positions, i);
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

                            for (size_t i = 0; i < normalAccessor.count; ++i)
                                make_Float3<const float*>(normals, normals_data, i);
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

                std::cout << "Mesh " << mesh_idx << ": " << vertices.size() << " vertices, " << indices.size() / 3 << " triangles" << std::endl;
            }

            // Convert Materials from gltf/glb file
            // for each material in the scene file
            for (int m = 0; m < model.materials.size(); m++) {
                tinygltf::Material mp = model.materials[m];
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
