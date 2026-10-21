#pragma once

// STD Lib deps
#include <iostream>
#include <filesystem>

// External Deps
#include <tiny_obj_loader.h>

// internal deps
#include "../geometry.h"
#include "../float3.h"
#include "../float2.h"
#include "../transforms.h"
#include "../material.h"


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
                const Color3 diffuse(mp.diffuse[0], mp.diffuse[1], mp.diffuse[2]);
                if (mp.diffuse_texname.length() > 0) {
                    texture = new ImageMixTexture<Color3>(file_path.parent_path() / mp.diffuse_texname, diffuse);
                } else
                    texture = new ColorTexture(diffuse);
                // MTL colours and maps are read as linear Rec.709.
                texture->convertToWorkingSpace(prism::color::kLightColorSpace);

                // Assign texture index to the Material's name
                materials[m] = new Diffuse(texture);
                if (mp.illum > 2 || mp.dissolve < 1 || mp.emission[0] != 0 || mp.emission[1] != 0 || mp.emission[2] != 0)
                    std::cerr << "PrismRT OBJ: only diffuse MTL shading is supported for " << mp.name << '\n';
            }
            materials[-1] = new Diffuse(new ColorTexture(Color3(.5f)));
        }
};
