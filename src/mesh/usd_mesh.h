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
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
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

            // Get the default time code
            pxr::UsdTimeCode timeCode = pxr::UsdTimeCode::Default();

            // Create transform cache for efficient transform computation with proper time
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

            std::cout << "Finished processing USD file:" << std::endl;
            std::cout << "    Total vertices: " << vertices.size() << std::endl;
            std::cout << "    Total triangles: " << globalTriangleCount << std::endl;
            std::cout << "    Total materials: " << materials.size() << std::endl;
        }

    void processMesh(const pxr::UsdGeomMesh& mesh, pxr::UsdGeomXformCache& xformCache,
                     pxr::UsdTimeCode timeCode, bool applyTransforms) {
        std::string prim_name = mesh.GetPrim().GetPath().GetString();
        // std::cout << "Processing mesh: " << prim_name << std::endl;

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
        Texture<Color3>* texture = color_texture;
        // Assign material ID
        int mat_id = materials.size();
        materials[mat_id] = new Diffuse(texture);

        // std::cout << "Created material " << mat_id << " with color (" << mesh_color << ")" << std::endl;

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

        // Get transform matrices if needed
        pxr::GfMatrix4d transform(1.0);  // Identity matrix
        pxr::GfMatrix4d normalTransform(1.0);  // Identity matrix
        bool flipNormals = false;

        if (applyTransforms) {
            // Get the full double precision transform
            transform = xformCache.GetLocalToWorldTransform(mesh.GetPrim());

            // For normals, we need the inverse transpose of the upper 3x3 part
            normalTransform = transform.GetInverse().GetTranspose();

            // Check if transform flips normals (negative determinant)
            double det = transform.GetDeterminant();
            flipNormals = det < 0.0;

            // std::cout << "Transform determinant: " << det << " (flip normals: " << flipNormals << ")" << std::endl;
        }

        // Store the starting vertex offset for this mesh
        size_t vertexOffset = vertices.size();

        // Process vertices with double precision
        for (const pxr::GfVec3f& point : points) {
            pxr::GfVec3f transformedPoint = point;

            if (applyTransforms) {
                // Convert to double precision for accurate transformation
                pxr::GfVec3d pointDouble(point[0], point[1], point[2]);
                pxr::GfVec3d transformedDouble = transform.Transform(pointDouble);
                transformedPoint = pxr::GfVec3f(transformedDouble[0], transformedDouble[1], transformedDouble[2]);
            }

            vertices.push_back(Float3(transformedPoint[0], transformedPoint[1], transformedPoint[2]));
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

        // Process normals
        processNormals(mesh, timeCode, normalTransform, applyTransforms, vertexOffset, flipNormals);

        // Process texture coordinates
        processTextureCoordinates(mesh, timeCode, vertexOffset);

        // std::cout << "Prim: " << points.size() << " vertices, " << faceVertexCounts.size() << " faces, and " << meshTriangleCount << " triangles" << std::endl;
    }

    void processNormals(const pxr::UsdGeomMesh& mesh, pxr::UsdTimeCode timeCode,
                       const pxr::GfMatrix4d& normalTransform, bool applyTransforms,
                       size_t vertexOffset, bool flipNormals) {

        // std::cout << "=== Processing normals for mesh: " << mesh.GetPrim().GetPath().GetString() << " ===" << std::endl;

        // Check if transform flips normals (negative determinant)
        double det = normalTransform.GetDeterminant();
        // std::cout << "Normal transform determinant: " << det << " (flip needed: " << flipNormals << ")" << std::endl;

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

                for (size_t i = 0; i < meshNormals.size(); ++i) {
                    const pxr::GfVec3f& normal = meshNormals[i];
                    pxr::GfVec3f transformedNormal = normal;

                    if (applyTransforms) {
                        // Transform normal using double precision
                        pxr::GfVec3d normalDouble(normal[0], normal[1], normal[2]);

                        // Use TransformDir for normals
                        pxr::GfVec3d transformedDouble = normalTransform.TransformDir(normalDouble);

                        // Apply flip if needed
                        if (flipNormals)
                            transformedDouble = -transformedDouble;

                        // Normalize the result
                        double length = transformedDouble.GetLength();
                        if (length > 0.0)
                            transformedDouble = transformedDouble / length;

                        transformedNormal = pxr::GfVec3f(transformedDouble[0], transformedDouble[1], transformedDouble[2]);
                    }

                    normals.push_back(Float3(transformedNormal[0], transformedNormal[1], transformedNormal[2]));
                }
            } else if (normalsInterpolation == pxr::UsdGeomTokens->faceVarying) {
                // Face-varying normals detected, generating from geometry
                // TODO: Face-varying normals need to be mapped to vertices properly
                generateGeometricNormals(mesh, timeCode, normalTransform, applyTransforms, vertexOffset, flipNormals);

            } else {
                // Unknown or invalidd normal interpolation, generate from geometry
                generateGeometricNormals(mesh, timeCode, normalTransform, applyTransforms, vertexOffset, flipNormals);
            }
        } else {
            // No normals found in mesh, generate from geometry
            generateGeometricNormals(mesh, timeCode, normalTransform, applyTransforms, vertexOffset, flipNormals);
        }

        // Ensure normals array matches vertices array size
        while (normals.size() < vertices.size()) {
            normals.push_back(Float3(0.0f, 1.0f, 0.0f));
        }
    }

    void generateGeometricNormals(const pxr::UsdGeomMesh& mesh, pxr::UsdTimeCode timeCode,
                                 const pxr::GfMatrix4d& normalTransform, bool applyTransforms,
                                 size_t vertexOffset, bool flipNormals) {

        // Get mesh data
        pxr::VtArray<pxr::GfVec3f> points;
        pxr::VtArray<int> faceVertexIndices;
        pxr::VtArray<int> faceVertexCounts;

        mesh.GetPointsAttr().Get(&points, timeCode);
        mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, timeCode);
        mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, timeCode);

        // Initialize per-vertex normals to zero
        std::vector<pxr::GfVec3f> vertexNormals(points.size(), pxr::GfVec3f(0.0f));

        // Compute face normals and accumulate to vertices
        size_t faceIndexOffset = 0;
        for (size_t faceIdx = 0; faceIdx < faceVertexCounts.size(); ++faceIdx) {
            int faceVertexCount = faceVertexCounts[faceIdx];

            if (faceVertexCount >= 3) {
                // Get first three vertices of the face
                int i0 = faceVertexIndices[faceIndexOffset + 0];
                int i1 = faceVertexIndices[faceIndexOffset + 1];
                int i2 = faceVertexIndices[faceIndexOffset + 2];

                pxr::GfVec3f v0 = points[i0];
                pxr::GfVec3f v1 = points[i1];
                pxr::GfVec3f v2 = points[i2];

                // Compute face normal using cross product
                pxr::GfVec3f edge1 = v1 - v0;
                pxr::GfVec3f edge2 = v2 - v0;
                pxr::GfVec3f faceNormal = edge1 ^ edge2; // Cross product

                // Check if we need to flip based on winding order
                // USD typically uses counter-clockwise winding
                if (flipNormals) {
                    faceNormal = -faceNormal;
                }

                float length = faceNormal.GetLength();
                if (length > 0.0f) {
                    faceNormal = faceNormal / length;
                }

                // Accumulate to all vertices of this face
                for (int v = 0; v < faceVertexCount; ++v) {
                    int vertexIndex = faceVertexIndices[faceIndexOffset + v];
                    vertexNormals[vertexIndex] += faceNormal;
                }
            }

            faceIndexOffset += faceVertexCount;
        }

        // Normalize accumulated normals and transform
        for (size_t i = 0; i < vertexNormals.size(); ++i) {
            pxr::GfVec3f normal = vertexNormals[i];

            // Normalize
            float length = normal.GetLength();
            if (length > 0.0f) {
                normal = normal / length;
            } else {
                normal = pxr::GfVec3f(0.0f, 1.0f, 0.0f); // Default up
            }

            if (applyTransforms) {
                // Transform normal
                pxr::GfVec3d normalDouble(normal[0], normal[1], normal[2]);
                pxr::GfVec3d transformedDouble = normalTransform.TransformDir(normalDouble);

                // Normalize after transform
                double transformedLength = transformedDouble.GetLength();
                if (transformedLength > 0.0) {
                    transformedDouble = transformedDouble / transformedLength;
                }

                normal = pxr::GfVec3f(transformedDouble[0], transformedDouble[1], transformedDouble[2]);
            }

            normals.push_back(Float3(normal[0], normal[1], normal[2]));
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