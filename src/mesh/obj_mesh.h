#pragma once

// STD Lib deps
#include <iostream>
#include <filesystem>

// External Deps
#include <tiny_obj_loader.h>

// internal deps
#include "geometry.h"
#include "float3.h"
#include "float2.h"
#include "transforms.h"
#include "material.h"


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
