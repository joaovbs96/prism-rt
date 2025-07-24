#include <iostream>
#include <vector>
#include <string>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <tiny_gltf.h>

class GLTFLoader {
public:
    bool loadModel(const std::filesystem::path &file_path) {
        tinygltf::TinyGLTF loader;
        std::string err;
        std::string warn;
        
        std::cout << file_path.extension() << std::endl;
        bool ret;
        if (file_path.extension() == ".gltf"){
            std::cout << "Reading gltf file..." << std::endl;
            ret = loader.LoadASCIIFromFile(&model, &err, &warn, file_path.string());
        } else {
            std::cout << "Reading glb file..." << std::endl;
            ret = loader.LoadBinaryFromFile(&model, &err, &warn, file_path.string());
        }
        
        if (!warn.empty()) {
            std::cout << "Warning: " << warn << std::endl;
        }
        
        if (!err.empty()) {
            std::cout << "Error: " << err << std::endl;
        }
        
        if (!ret) {
            std::cout << "Failed to parse glTF file" << std::endl;
            return false;
        }
        
        return true;
    }
    
    void printModelInfo() {
        std::cout << "=== Model Information ===" << std::endl;
        std::cout << "Scenes: " << model.scenes.size() << std::endl;
        std::cout << "Nodes: " << model.nodes.size() << std::endl;
        std::cout << "Meshes: " << model.meshes.size() << std::endl;
        std::cout << "Materials: " << model.materials.size() << std::endl;
        std::cout << "Textures: " << model.textures.size() << std::endl;
        std::cout << "Images: " << model.images.size() << std::endl;
        std::cout << "Accessors: " << model.accessors.size() << std::endl;
        std::cout << "BufferViews: " << model.bufferViews.size() << std::endl;
        std::cout << "Buffers: " << model.buffers.size() << std::endl;
    }
    
    void iterateScenes() {
        std::cout << "\n=== Scenes ===" << std::endl;
        for (size_t i = 0; i < model.scenes.size(); ++i) {
            const tinygltf::Scene& scene = model.scenes[i];
            std::cout << "Scene " << i << ": " << scene.name << std::endl;
            std::cout << "  Root nodes: ";
            for (int nodeIndex : scene.nodes) {
                std::cout << nodeIndex << " ";
            }
            std::cout << std::endl;
        }
    }
    
    void iterateNodes() {
        std::cout << "\n=== Nodes ===" << std::endl;
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const tinygltf::Node& node = model.nodes[i];
            std::cout << "Node " << i << ": " << node.name << std::endl;
            
            if (node.mesh >= 0) {
                std::cout << "  Mesh index: " << node.mesh << std::endl;
            }
            
            if (!node.children.empty()) {
                std::cout << "  Children: ";
                for (int child : node.children) {
                    std::cout << child << " ";
                }
                std::cout << std::endl;
            }
            
            // Print transformation matrix or TRS
            if (node.matrix.size() == 16) {
                std::cout << "  Has transformation matrix" << std::endl;
            } else {
                if (node.translation.size() == 3) {
                    std::cout << "  Translation: (" << node.translation[0] 
                              << ", " << node.translation[1] 
                              << ", " << node.translation[2] << ")" << std::endl;
                }
                if (node.rotation.size() == 4) {
                    std::cout << "  Rotation: (" << node.rotation[0] 
                              << ", " << node.rotation[1] 
                              << ", " << node.rotation[2] 
                              << ", " << node.rotation[3] << ")" << std::endl;
                }
                if (node.scale.size() == 3) {
                    std::cout << "  Scale: (" << node.scale[0] 
                              << ", " << node.scale[1] 
                              << ", " << node.scale[2] << ")" << std::endl;
                }
            }
        }
    }
    
    void iterateMeshes() {
        std::cout << "\n=== Meshes ===" << std::endl;
        for (size_t i = 0; i < model.meshes.size(); ++i) {
            const tinygltf::Mesh& mesh = model.meshes[i];
            std::cout << "Mesh " << i << ": " << mesh.name << std::endl;
            std::cout << "  Primitives: " << mesh.primitives.size() << std::endl;
            
            for (size_t j = 0; j < mesh.primitives.size(); ++j) {
                const tinygltf::Primitive& primitive = mesh.primitives[j];
                std::cout << "  Primitive " << j << ":" << std::endl;
                std::cout << "    Mode: " << primitive.mode << std::endl;
                std::cout << "    Material: " << primitive.material << std::endl;
                
                // Print attributes
                std::cout << "    Attributes:" << std::endl;
                for (const auto& attrib : primitive.attributes) {
                    std::cout << "      " << attrib.first << ": accessor " << attrib.second << std::endl;
                }
                
                // Print indices if available
                if (primitive.indices >= 0) {
                    std::cout << "    Indices: accessor " << primitive.indices << std::endl;
                    
                    // Get face count
                    const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
                    int faceCount = indexAccessor.count / 3; // Assuming triangles
                    std::cout << "    Face count: " << faceCount << std::endl;
                }
            }
        }
    }
    
    void iterateFaces() {
        std::cout << "\n=== Face Data ===" << std::endl;
        for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx) {
            const tinygltf::Mesh& mesh = model.meshes[meshIdx];
            std::cout << "Mesh " << meshIdx << " faces:" << std::endl;
            
            for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx) {
                const tinygltf::Primitive& primitive = mesh.primitives[primIdx];
                
                if (primitive.indices < 0) {
                    std::cout << "  Primitive " << primIdx << ": No indices (non-indexed)" << std::endl;
                    continue;
                }
                
                const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
                const tinygltf::BufferView& indexBufferView = model.bufferViews[indexAccessor.bufferView];
                const tinygltf::Buffer& indexBuffer = model.buffers[indexBufferView.buffer];
                
                const unsigned char* indexData = &indexBuffer.data[indexBufferView.byteOffset + indexAccessor.byteOffset];
                
                // Handle different index types
                int faceCount = indexAccessor.count / 3; // Assuming triangles
                std::cout << "  Primitive " << primIdx << " faces (first 5):" << std::endl;
                
                for (int faceIdx = 0; faceIdx < std::min(5, faceCount); ++faceIdx) {
                    std::cout << "    Face " << faceIdx << ": ";
                    
                    for (int vertIdx = 0; vertIdx < 3; ++vertIdx) {
                        int index = 0;
                        int indexOffset = (faceIdx * 3 + vertIdx);
                        
                        switch (indexAccessor.componentType) {
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                                index = static_cast<int>(indexData[indexOffset]);
                                break;
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                                index = static_cast<int>(reinterpret_cast<const unsigned short*>(indexData)[indexOffset]);
                                break;
                            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                                index = static_cast<int>(reinterpret_cast<const unsigned int*>(indexData)[indexOffset]);
                                break;
                        }
                        
                        std::cout << index << " ";
                    }
                    std::cout << std::endl;
                }
                
                if (faceCount > 5) {
                    std::cout << "    ... and " << (faceCount - 5) << " more faces" << std::endl;
                }
            }
        }
    }
    
    void iterateVertexData() {
        std::cout << "\n=== Vertex Data ===" << std::endl;
        for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx) {
            const tinygltf::Mesh& mesh = model.meshes[meshIdx];
            
            for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx) {
                const tinygltf::Primitive& primitive = mesh.primitives[primIdx];
                
                // Look for position attribute
                auto posIt = primitive.attributes.find("POSITION");
                if (posIt != primitive.attributes.end()) {
                    const tinygltf::Accessor& posAccessor = model.accessors[posIt->second];
                    const tinygltf::BufferView& posBufferView = model.bufferViews[posAccessor.bufferView];
                    const tinygltf::Buffer& posBuffer = model.buffers[posBufferView.buffer];
                    
                    const float* positions = reinterpret_cast<const float*>(
                        &posBuffer.data[posBufferView.byteOffset + posAccessor.byteOffset]);
                    
                    int vertexCount = posAccessor.count;
                    std::cout << "Mesh " << meshIdx << ", Primitive " << primIdx 
                              << " vertices (first 5 of " << vertexCount << "):" << std::endl;
                    
                    for (int i = 0; i < std::min(5, vertexCount); ++i) {
                        std::cout << "  Vertex " << i << ": ("
                                  << positions[i * 3] << ", "
                                  << positions[i * 3 + 1] << ", "
                                  << positions[i * 3 + 2] << ")" << std::endl;
                    }
                }
            }
        }
    }
    
    void iterateMaterials() {
        std::cout << "\n=== Materials ===" << std::endl;
        for (size_t i = 0; i < model.materials.size(); ++i) {
            const tinygltf::Material& material = model.materials[i];
            std::cout << "Material " << i << ": " << material.name << std::endl;
            
            // PBR metallic roughness
            const auto& pbr = material.pbrMetallicRoughness;
            std::cout << "  Base Color Factor: ("
                      << pbr.baseColorFactor[0] << ", "
                      << pbr.baseColorFactor[1] << ", "
                      << pbr.baseColorFactor[2] << ", "
                      << pbr.baseColorFactor[3] << ")" << std::endl;
            std::cout << "  Metallic Factor: " << pbr.metallicFactor << std::endl;
            std::cout << "  Roughness Factor: " << pbr.roughnessFactor << std::endl;
            
            if (pbr.baseColorTexture.index >= 0) {
                std::cout << "  Base Color Texture: " << pbr.baseColorTexture.index << std::endl;
            }
            
            if (pbr.metallicRoughnessTexture.index >= 0) {
                std::cout << "  Metallic Roughness Texture: " << pbr.metallicRoughnessTexture.index << std::endl;
            }
        }
    }
    
private:
    tinygltf::Model model;
};
