#pragma once

#define TINYOBJLOADER_IMPLEMENTATION // define this in only *one* .cc

#include <iostream>
#include <filesystem>

#include <embree4/rtcore.h>
#include <tiny_obj_loader.h>
#include <tiny_gltf.h>

#include "float3.h"
#include "float2.h"
#include "material.h"

class Geometry {
    public:
        Geometry () {}

        virtual unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) = 0;

        virtual Material* getMaterial(unsigned int primid, unsigned int instid) = 0;

        RTCGeometryType type;
        RTCGeometry geom;
        unsigned int geomid;
};

class Sphere : public Geometry {
    public:
        Sphere(const Float3& pos, float r, Material* material) : material(material), pos(pos), r(r) {}

        unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override {
            type = RTC_GEOMETRY_TYPE_SPHERE_POINT;
            geom = rtcNewGeometry(device, type);
            float* vb = (float*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4*sizeof(float), 1);
            vb[0] = pos.x(); vb[1] = pos.y(); vb[2] = pos.z(); vb[3] = r;

            // commit and release geom handle
            rtcCommitGeometry(geom);
            geomid = rtcAttachGeometry(scene, geom);
            return geomid;
        }

        Material* getMaterial(unsigned int primid, unsigned int instid) override {
            return material;
        }

        Float3 pos;
        float r;
        Material* material;
};

class Mesh : public Geometry {
    public:
        Mesh() {}

        unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override {
            // new geometry pointer
            type = RTC_GEOMETRY_TYPE_TRIANGLE;
            geom = rtcNewGeometry(device, type);

            std::cout << "Finished creating mesh pointer. " << std::endl;

            // add new vertex buffer to geometry
            // rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices.data(), 0, sizeof(Float3), vertices.size());
            Float3* vtx = (Float3*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Float3), vertices.size());
            for (unsigned int i = 0; i < vertices.size(); i++) vtx[i] = vertices[i];

            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, indices.data(), 0, 3 * sizeof(unsigned int), int(indices.size() / 3));

            rtcSetGeometryVertexAttributeCount(geom, 2);
            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTC_FORMAT_FLOAT3, normals.data(), 0, sizeof(Float3), normals.size());
            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTC_FORMAT_FLOAT2, texcoords.data(), 0, sizeof(Float2), texcoords.size());

            std::cout << "Finished creating mesh buffers. " << std::endl;

            // commit and release geom handle
            rtcCommitGeometry(geom);
            geomid = rtcAttachGeometry(scene, geom);
            // rtcReleaseGeometry(geom);

            return geomid;
        }

        Material* getMaterial(unsigned int primid, unsigned int instid) override {
            if (material_override != nullptr)
                return material_override;

            // TODO: make materials on a per prim, per instance basis?
            // we may need a two dimensional unordered map, with primid and instid as keys
            return materials[material_mapping[primid]];
        }

        // Make a float3 vertex out of a vector and an index
        template <class T>
        void make_Float3(std::vector<Float3> &vertex_list, const T &attribs, int index)
        {
            if (index >= 0)
            {
                float x = attribs[3 * index + 0];
                float y = attribs[3 * index + 1];
                float z = attribs[3 * index + 2];
                vertex_list.push_back(Float3(x, y, z));
            }
        }

        // Make a float2 vertex out of a vector and an index
        template <class T>
        void make_Float2(std::vector<Float2> &vertex_list, const T &attribs, int index)
        {
            if (index >= 0)
            {
                float x = attribs[2 * index + 0];
                float y = attribs[2 * index + 1];
                vertex_list.push_back(Float2(x, y));
            }
        }

        std::vector<Float3> vertices;
        std::vector<Float3> normals;
        std::vector<Float2> texcoords;
        std::vector<unsigned int> indices;
        std::unordered_map<int, Material*> materials;
        std::unordered_map<int, int> material_mapping;
        Material* material_override = nullptr;
};

class ObjMesh : public Mesh {
    public:
        ObjMesh(const std::filesystem::path &file_path)
        {
            // based on: https://github.com/yumcyaWiz/embree-tinyobj-example
            std::cout << "Loading File: " << file_path << std::endl;
            tinyobj::ObjReaderConfig reader_config;
            reader_config.triangulate = true;

            tinyobj::ObjReader reader;
            if (!reader.ParseFromFile(file_path.string(), reader_config))
            {
                if (!reader.Error().empty())
                {
                    std::cout << "Unable to load OBJ file (" << file_path.filename() << "): " << reader.Error() << std::endl;
                }
                return;
            }

            if (!reader.Warning().empty())
            {
                std::cout << "Warning during OBJ load: " << reader.Warning() << std::endl;
            }

            std::cout << "Finished loading file. " << std::endl;

            const auto &attrib = reader.GetAttrib();
            const auto &shapes = reader.GetShapes();
            const auto &obj_materials = reader.GetMaterials();

            int index = 0, n_faces = 0;
            for (size_t s = 0; s < shapes.size(); ++s)
            {
                // for each face of the current mesh
                for (size_t f = 0; f < shapes[s].mesh.indices.size() / 3; f++)
                {
                    // std::cout << f << std::endl;
                    // Get the three indexes of the face (all faces are triangular)
                    tinyobj::index_t idx0 = shapes[s].mesh.indices[3 * f + 0];
                    tinyobj::index_t idx1 = shapes[s].mesh.indices[3 * f + 1];
                    tinyobj::index_t idx2 = shapes[s].mesh.indices[3 * f + 2];

                    // get the vertex coordinates
                    make_Float3<std::vector<tinyobj::real_t>>(vertices, attrib.vertices, idx0.vertex_index);
                    make_Float3<std::vector<tinyobj::real_t>>(vertices, attrib.vertices, idx1.vertex_index);
                    make_Float3<std::vector<tinyobj::real_t>>(vertices, attrib.vertices, idx2.vertex_index);

                    // get the normal coordinates
                    make_Float3<std::vector<tinyobj::real_t>>(normals, attrib.normals, idx0.normal_index);
                    make_Float3<std::vector<tinyobj::real_t>>(normals, attrib.normals, idx1.normal_index);
                    make_Float3<std::vector<tinyobj::real_t>>(normals, attrib.normals, idx2.normal_index);

                    // // get the vertex tex coordinates
                    make_Float2<std::vector<tinyobj::real_t>>(texcoords, attrib.texcoords, idx0.texcoord_index);
                    make_Float2<std::vector<tinyobj::real_t>>(texcoords, attrib.texcoords, idx1.texcoord_index);
                    make_Float2<std::vector<tinyobj::real_t>>(texcoords, attrib.texcoords, idx2.texcoord_index);

                    // set index vector
                    indices.push_back(index++);

                    // per-face material
                    material_mapping[n_faces++] = shapes[s].mesh.material_ids[f];
                }
            }
            std::cout << "Finished creating mesh data. " << std::endl;

            // Convert Materials from MTL file
            // for each material in the MTL file
            for (int m = 0; m < obj_materials.size(); m++) {
                tinyobj::material_t mp = obj_materials[m];

                // Create Texture from image file or color value
                Texture<Color3>* texture;
                std::cout << mp.diffuse_texname << std::endl;
                if (mp.diffuse_texname.length() > 0) {
                    std::cout << Color3(*mp.diffuse) << std::endl;
                    texture = new ImageMixTexture<Color3>(file_path.parent_path().concat("/").concat(mp.diffuse_texname), Color3(*mp.diffuse));
                } else
                    texture = new ColorTexture(Color3(*mp.diffuse));

                // Assign texture index to the Material's name
                materials[m] = new Diffuse(texture);
            }
        }
};

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
                } else {
                    base_texture = new ColorTexture(base_factor);
                }

                // Assign texture index to the Material's name
                materials[m] = new Diffuse(base_texture);
            }
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

        tinygltf::Image getTextureImage(tinygltf::Model& model, tinygltf::TextureInfo& texinfo) {
            tinygltf::Texture& texture = model.textures[texinfo.index];
            return model.images[texture.source];
        }
};