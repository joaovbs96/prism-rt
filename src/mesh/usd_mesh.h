#pragma once

// STD Lib deps
#include <iostream>
#include <filesystem>

// Open USD Deps
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/tokens.h>

PXR_NAMESPACE_USING_DIRECTIVE

// internal deps
#include "geometry.h"

class USDMesh : public Mesh {
    public:
        USDMesh(const std::filesystem::path &file_path) {
            // Open the USD stage
            pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(file_path.string());
            if (!stage) {
                std::cout << "Failed to open USD file: " << file_path << std::endl;
                return;
            }

            std::cout << "Successfully opened USD stage: " << file_path << std::endl;

            // Use a specific time code (default time)
            pxr::UsdTimeCode timeCode = pxr::UsdTimeCode::Default();

            // Create transform cache for efficient transform computation with the correct time
            pxr::UsdGeomXformCache xformCache(timeCode);

            // Traverse all prims in the stage
            pxr::UsdPrimRange range = stage->Traverse();
            for (auto iter = range.begin(); iter != range.end(); ++iter) {
                const pxr::UsdPrim& prim = *iter;

                // Check if this prim is a mesh
                if (prim.IsA<pxr::UsdGeomMesh>()) {
                    pxr::UsdGeomMesh mesh(prim);
                    processMesh(mesh, xformCache, timeCode, true);
                }
            }

            std::cout << "Finished processing USD file" << std::endl;
            std::cout << "Total vertices: " << vertices.size() << std::endl;
            std::cout << "Total triangles: " << globalTriangleCount << std::endl;
            std::cout << "Total materials: " << materials.size() << std::endl;
        }

    void processMesh(const pxr::UsdGeomMesh& mesh, pxr::UsdGeomXformCache& xformCache,
                     pxr::UsdTimeCode timeCode, bool applyTransforms) {
        std::string prim_name = mesh.GetPrim().GetPath().GetString();
        std::cout << "Processing mesh: " << prim_name << std::endl;

        // Get the primvars API for the mesh
        pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh.GetPrim());

        // Get the displayColor primvar
        pxr::UsdGeomPrimvar displayColorPrimvar = primvarsAPI.GetPrimvar(TfToken("displayColor"));

        // Check if the primvar exists and is valid
        Color3 mesh_color(0.7f, 0.7f, 0.7f); // Default color
        if (displayColorPrimvar) {
            // Get the values at a specific time
            pxr::VtValue displayColorValue;
            if (displayColorPrimvar.Get(&displayColorValue, timeCode)) {
                // displayColor is typically a VtVec3fArray
                if (displayColorValue.IsHolding<pxr::VtVec3fArray>()) {
                    pxr::VtVec3fArray colorArray = displayColorValue.Get<pxr::VtVec3fArray>();
                    if (colorArray.size() > 0) {
                        pxr::GfVec3f& color = colorArray[0];
                        mesh_color = Color3(color[0], color[1], color[2]);
                    }
                }
            }
        }

        // Create Texture from color value
        ColorTexture* color_texture = new ColorTexture(mesh_color);
        color_texture->gammaCorrect(1.0 / 2.2); // TODO: is this needed?
        Texture<Color3>* texture = color_texture;
        // Assign material ID
        int mat_id = materials.size();
        materials[mat_id] = new Diffuse(texture);

        std::cout << "Created material " << mat_id << " with color (" << mesh_color << ")" << std::endl;

        // Get mesh attributes
        pxr::VtArray<pxr::GfVec3f> points;
        pxr::VtArray<int> faceVertexIndices;
        pxr::VtArray<int> faceVertexCounts;

        // Read points (vertices)
        pxr::UsdAttribute pointsAttr = mesh.GetPointsAttr();
        if (!pointsAttr.Get(&points, timeCode)) {
            std::cout << "Failed to get points for mesh: " << mesh.GetPrim().GetPath() << std::endl;
            return;
        }

        // Read face vertex indices
        pxr::UsdAttribute faceVertexIndicesAttr = mesh.GetFaceVertexIndicesAttr();
        if (!faceVertexIndicesAttr.Get(&faceVertexIndices, timeCode)) {
            std::cout << "Failed to get face vertex indices for mesh: " << mesh.GetPrim().GetPath() << std::endl;
            return;
        }

        // Read face vertex counts
        pxr::UsdAttribute faceVertexCountsAttr = mesh.GetFaceVertexCountsAttr();
        if (!faceVertexCountsAttr.Get(&faceVertexCounts, timeCode)) {
            std::cout << "Failed to get face vertex counts for mesh: " << mesh.GetPrim().GetPath() << std::endl;
            return;
        }

        // Get transform matrices - keep as double precision until final conversion
        pxr::GfMatrix4d transformMatrix;
        transformMatrix.SetIdentity();
        pxr::GfMatrix3d normalTransformMatrix;
        normalTransformMatrix.SetIdentity();

        if (applyTransforms) {
            // Get the full world transform (double precision)
            transformMatrix = xformCache.GetLocalToWorldTransform(mesh.GetPrim());

            // For normals, we need the inverse transpose of the upper-left 3x3 matrix
            // Extract the 3x3 portion (removing translation)
            pxr::GfMatrix3d rotationScaleMatrix = transformMatrix.ExtractRotationMatrix();

            // Get the inverse transpose for normal transformation
            normalTransformMatrix = rotationScaleMatrix.GetInverse().GetTranspose();
        }

        // Store the starting vertex offset for this mesh
        size_t vertexOffset = vertices.size();

        // Process vertices with double precision transforms
        for (const pxr::GfVec3f& point : points) {
            pxr::GfVec3d transformedPoint(point[0], point[1], point[2]);

            if (applyTransforms) {
                // Apply transformation using double precision
                // Convert to homogeneous coordinates and multiply
                pxr::GfVec4d homogenousPoint(transformedPoint[0], transformedPoint[1], transformedPoint[2], 1.0);
                pxr::GfVec4d transformedHomogenous = transformMatrix * homogenousPoint;
                transformedPoint = pxr::GfVec3d(transformedHomogenous[0], transformedHomogenous[1], transformedHomogenous[2]);
            }

            // Convert to float for final storage
            vertices.push_back(Float3(
                static_cast<float>(transformedPoint[0]),
                static_cast<float>(transformedPoint[1]),
                static_cast<float>(transformedPoint[2])
            ));
        }

        // Process faces and triangulate if necessary
        size_t faceIndexOffset = 0;
        int meshTriangleCount = 0; // Local count for this mesh

        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int faceVertexCount = faceVertexCounts[faceIdx];

            if (faceVertexCount == 3) {
                // Triangle - add directly
                indices.push_back(static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 0]));
                indices.push_back(static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 1]));
                indices.push_back(static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 2]));

                // Assign material to this triangle using GLOBAL triangle count
                material_mapping[globalTriangleCount++] = mat_id;
                meshTriangleCount++;

            } else if (faceVertexCount == 4) {
                // Quad - triangulate (0,1,2) and (0,2,3)
                unsigned int v0 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 0]);
                unsigned int v1 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 1]);
                unsigned int v2 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 2]);
                unsigned int v3 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 3]);

                // First triangle
                indices.push_back(v0);
                indices.push_back(v1);
                indices.push_back(v2);
                material_mapping[globalTriangleCount++] = mat_id;
                meshTriangleCount++;

                // Second triangle
                indices.push_back(v0);
                indices.push_back(v2);
                indices.push_back(v3);
                material_mapping[globalTriangleCount++] = mat_id;
                meshTriangleCount++;

            } else if (faceVertexCount > 4) {
                // N-gon - fan triangulation from first vertex
                unsigned int v0 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + 0]);
                for (int i = 1; i < faceVertexCount - 1; ++i) {
                    unsigned int v1 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + i]);
                    unsigned int v2 = static_cast<unsigned int>(vertexOffset + faceVertexIndices[faceIndexOffset + i + 1]);

                    indices.push_back(v0);
                    indices.push_back(v1);
                    indices.push_back(v2);
                    material_mapping[globalTriangleCount++] = mat_id;
                    meshTriangleCount++;
                }
            }

            faceIndexOffset += faceVertexCount;
        }

        // Process normals with correct transform
        processNormals(mesh, timeCode, normalTransformMatrix, applyTransforms, vertexOffset);

        // Process texture coordinates (no transform needed)
        processTextureCoordinates(mesh, timeCode, vertexOffset);

        std::cout << "Prim: " << points.size() << " vertices, " << faceVertexCounts.size() << " faces, and " << meshTriangleCount << " triangles" << std::endl;
    }

    void processNormals(const pxr::UsdGeomMesh& mesh, pxr::UsdTimeCode timeCode,
                       const pxr::GfMatrix3d& normalTransform, bool applyTransforms, size_t vertexOffset) {
        // Try to get normals from the mesh
        pxr::UsdAttribute normalsAttr = mesh.GetNormalsAttr();
        pxr::VtArray<pxr::GfVec3f> meshNormals;

        if (normalsAttr && normalsAttr.Get(&meshNormals, timeCode)) {
            // Check normals interpolation
            pxr::TfToken normalsInterpolation;
            pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh.GetPrim());
            pxr::UsdGeomPrimvar normalsPrimvar = primvarsAPI.GetPrimvar(pxr::UsdGeomTokens->normals);
            if (normalsPrimvar) {
                normalsInterpolation = normalsPrimvar.GetInterpolation();
            }

            if (normalsInterpolation == pxr::UsdGeomTokens->vertex ||
                normalsInterpolation == pxr::UsdGeomTokens->varying) {
                // Per-vertex normals
                for (const pxr::GfVec3f& normal : meshNormals) {
                    pxr::GfVec3d transformedNormal(normal[0], normal[1], normal[2]);

                    if (applyTransforms) {
                        // Transform normal using the 3x3 normal transform matrix
                        transformedNormal = normalTransform * transformedNormal;
                        transformedNormal.Normalize();
                    }

                    normals.push_back(Float3(
                        static_cast<float>(transformedNormal[0]),
                        static_cast<float>(transformedNormal[1]),
                        static_cast<float>(transformedNormal[2])
                    ));
                }
            } else if (normalsInterpolation == pxr::UsdGeomTokens->faceVarying) {
                // Face-varying normals - expand to match vertex count
                size_t currentVertexCount = vertices.size() - vertexOffset;
                if (meshNormals.size() > 0) {
                    // Use the first normal for all vertices (placeholder)
                    pxr::GfVec3d defaultNormal(meshNormals[0][0], meshNormals[0][1], meshNormals[0][2]);

                    if (applyTransforms) {
                        defaultNormal = normalTransform * defaultNormal;
                        defaultNormal.Normalize();
                    }

                    for (size_t i = normals.size(); i < vertices.size(); ++i) {
                        normals.push_back(Float3(
                            static_cast<float>(defaultNormal[0]),
                            static_cast<float>(defaultNormal[1]),
                            static_cast<float>(defaultNormal[2])
                        ));
                    }
                }
            }
        } else {
            // No normals found - generate default up normals
            size_t currentVertexCount = vertices.size() - vertexOffset;
            for (size_t i = 0; i < currentVertexCount; ++i) {
                normals.push_back(Float3(0.0f, 1.0f, 0.0f)); // Default up normal
            }
        }

        // Ensure normals array matches vertices array size
        while (normals.size() < vertices.size()) {
            normals.push_back(Float3(0.0f, 1.0f, 0.0f));
        }
    }

    void processTextureCoordinates(const pxr::UsdGeomMesh& mesh, pxr::UsdTimeCode timeCode, size_t vertexOffset) {
        // Look for UV primvars (USD's way of storing texture coordinates)
        pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh.GetPrim());
        std::vector<pxr::UsdGeomPrimvar> primvars = primvarsAPI.GetPrimvars();

        // Look for 'st' or 'uv' primvar (common names for texture coordinates)
        pxr::UsdGeomPrimvar uvPrimvar;
        for (const pxr::UsdGeomPrimvar& primvar : primvars) {
            TfToken name = primvar.GetPrimvarName();
            if (name == "st" || name == "uv" || name == "UV") {
                uvPrimvar = primvar;
                break;
            }
        }

        if (uvPrimvar) {
            pxr::VtArray<pxr::GfVec2f> uvValues;
            if (uvPrimvar.Get(&uvValues, timeCode)) {
                pxr::TfToken interpolation = uvPrimvar.GetInterpolation();

                if (interpolation == pxr::UsdGeomTokens->vertex ||
                    interpolation == pxr::UsdGeomTokens->varying) {
                    // Per-vertex UVs
                    for (const pxr::GfVec2f& uv : uvValues) {
                        texcoords.push_back(Float2(uv[0], uv[1]));
                    }
                } else if (interpolation == pxr::UsdGeomTokens->faceVarying) {
                    // Face-varying UVs - this needs proper unwrapping
                    // For simplicity, use first UV for all vertices
                    if (uvValues.size() > 0) {
                        pxr::GfVec2f defaultUV = uvValues[0];
                        size_t currentVertexCount = vertices.size() - vertexOffset;
                        for (size_t i = texcoords.size(); i < vertices.size(); ++i) {
                            texcoords.push_back(Float2(defaultUV[0], defaultUV[1]));
                        }
                    }
                }
            }
        }

        // Ensure texcoords array matches vertices array size
        while (texcoords.size() < vertices.size()) {
            texcoords.push_back(Float2(0.0f, 0.0f)); // Default UV
        }
    }

    private:
    int globalTriangleCount = 0;
};