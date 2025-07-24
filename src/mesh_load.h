#pragma once

#define TINYOBJLOADER_IMPLEMENTATION // define this in only *one* .cc

#include <iostream>
#include <filesystem>

#include <embree4/rtcore.h>
#include <tiny_obj_loader.h>
#include <tiny_gltf.h>

#include "float3.h"
#include "float2.h"


class Mesh {
    public:
        Mesh() {}

        int setupEmbree(RTCDevice &device, RTCScene &scene) {
            // new geometry pointer
            RTCGeometry mesh = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);

            std::cout << "Finished creating mesh pointer. " << std::endl;

            // add new vertex buffer to geometry
            // rtcSetSharedGeometryBuffer(mesh, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices.data(), 0, sizeof(Float3), vertices.size());
            Float3* vtx = (Float3*)rtcSetNewGeometryBuffer(mesh, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Float3), vertices.size());
            for (unsigned int i = 0; i < vertices.size(); i++) vtx[i] = vertices[i];

            rtcSetSharedGeometryBuffer(mesh, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, indices.data(), 0, 3 * sizeof(unsigned int), int(indices.size() / 3));

            rtcSetGeometryVertexAttributeCount(mesh, 2);
            rtcSetSharedGeometryBuffer(mesh, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTC_FORMAT_FLOAT3, normals.data(), 0, sizeof(Float3), normals.size());
            rtcSetSharedGeometryBuffer(mesh, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTC_FORMAT_FLOAT2, texcoords.data(), 0, sizeof(Float2), texcoords.size());

            std::cout << "Finished creating mesh buffers. " << std::endl;

            // commit and release geom handle
            rtcCommitGeometry(mesh);
            geomID = rtcAttachGeometry(scene, mesh);
            rtcReleaseGeometry(mesh);

            return geomID;
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
                float x = attribs[3 * index + 0];
                float y = attribs[3 * index + 1];
                vertex_list.push_back(Float2(x, y));
            }
        }

        std::vector<Float3> vertices;
        std::vector<Float3> normals;
        std::vector<Float2> texcoords;
        std::vector<unsigned int> indices;
        unsigned int geomID;
        
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
            const auto &materials = reader.GetMaterials();

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
                    // make_Float2<std::vector<tinyobj::real_t>>(texcoords, attrib.texcoords, idx0.texcoord_index);
                    // make_Float2<std::vector<tinyobj::real_t>>(texcoords, attrib.texcoords, idx1.texcoord_index);
                    // make_Float2<std::vector<tinyobj::real_t>>(texcoords, attrib.texcoords, idx2.texcoord_index);

                    // set index vector
                    indices.push_back(index++);
                    indices.push_back(index++);
                    indices.push_back(index++);
                }
            }
            std::cout << "Finished creating mesh data. " << std::endl;

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
                    for (int i = 0; i < vertexCount; ++i)
                        make_Float3<const float*>(vertices, positions, i);
                    
                    // Extract indices
                    if (primitive.indices >= 0) {
                        const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
                        const tinygltf::BufferView& indexBufferView = model.bufferViews[indexAccessor.bufferView];
                        const tinygltf::Buffer& indexBuffer = model.buffers[indexBufferView.buffer];
                        
                        const unsigned char* indexData = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];
                        
                        int triangleCount = indexAccessor.count / 3;
                        
                        for (int i = 0; i < triangleCount; i++) {
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
                                index += static_cast<unsigned int>(vertexOffset);
                                
                                indices.push_back(index++);
                                indices.push_back(index++);
                                indices.push_back(index++);
                            }
                        }
                    } else {
                        // Non-indexed geometry - create indices automatically
                        int triangleCount = vertexCount / 3;
                        for (int i = 0; i < triangleCount; ++i) {
                            indices.push_back(static_cast<unsigned int>(vertexOffset + i * 3 + 0));
                            indices.push_back(static_cast<unsigned int>(vertexOffset + i * 3 + 1));
                            indices.push_back(static_cast<unsigned int>(vertexOffset + i * 3 + 2));
                        }
                    }
                }
                
                std::cout << "Mesh " << mesh_idx << ": " << vertices.size() << " vertices, " << indices.size() << " triangles" << std::endl;
            }
        }
};