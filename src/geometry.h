#pragma once


// STD Lib deps
#include <iostream>
#include <filesystem>

// External Deps
#include <embree4/rtcore.h>

// Internal Deps
#include "float3.h"
#include "float2.h"
#include "transforms.h"
#include "material.h"
#include "aabb.h"

class Geometry {
    public:
        Geometry () {}

        virtual unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) = 0;

        virtual Material* getMaterial(unsigned int primid, unsigned int instid) = 0;

        // Apply the current transform matrix to all vertices
        virtual void applyTransform() {
            if (!geom) {
                std::cerr << "Error: Geometry not initialized" << std::endl;
                return;
            }

            if (transform->isIdentity())
                return;

            // TODO: this only transforms positions. If a rotation/scale ever
            // goes through here, normals (inverse-transpose) and tangents
            // (linear part) must be transformed as well.

            // Get the vertex buffer data
            float* vb = (float*)rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, 0);
            if (!vb) {
                std::cerr << "Error: Could not get vertex buffer data" << std::endl;
                return;
            }

            // Transform each vertex
            for (int i = 0; i < vertex_count; i++) {
                // Get current vertex position
                Float3 vertex(vb[i * 3 + 0], vb[i * 3 + 1], vb[i * 3 + 2]);

                // Apply transform matrix
                Float3 transformed_vertex = transform->transformPoint(vertex);

                // Write back transformed vertex
                vb[i * 3 + 0] = transformed_vertex.x();
                vb[i * 3 + 1] = transformed_vertex.y();
                vb[i * 3 + 2] = transformed_vertex.z();
            }

            // Mark geometry as modified and recommit
            rtcUpdateGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0);
            rtcCommitGeometry(geom);
        }

        // Update the AABB based on current vertices
        virtual void updateAABB() = 0;

        // Get the bounding box center
        Float3 getBoundingBoxCenter() const {
            return bounding_box.getCenter();
        }

        // Get the minimum point (bottom corner) of the bounding box
        Float3 getBoundingBoxMin() const {
            return bounding_box.getMin();
        }

        // Get the maximum point (top corner) of the bounding box
        Float3 getBoundingBoxMax() const {
            return bounding_box.getMax();
        }

        // Set position by translating mesh so the bottom of its bounding box is at the specified position
        void setPosition(const Float3& target_position) {
            Float3 current_min = bounding_box.getMin();
            Float3 translation = target_position - current_min;

            // TODO: save temporary transform variable for current transform if it's diff than identify

            // Apply translation to the transform matrix
            transform->translate(translation.x(), translation.y(), translation.z());

            // Apply the transform to update the vertices
            applyTransform();

            // Update AABB after transformation
            updateAABB();

            // Reset transform matrix to identity after applying
            transform->identity();
        }

        int vertex_count;
        RTCGeometryType type;
        RTCGeometry geom;
        unsigned int geomid;
        TransformMatrix* transform = new TransformMatrix();
        AABB bounding_box;
};

class Box : public Geometry {
    public:
        Box(const Float3& a, const Float3& b, Material* material_) {
            Float3 min = a.min(b);
            Float3 max = a.max(b);

            // Create the 8 unique vertices of the box
            vertices.push_back(Float3(min.x(), min.y(), min.z())); // 0: min corner
            vertices.push_back(Float3(max.x(), min.y(), min.z())); // 1: +X from min
            vertices.push_back(Float3(max.x(), max.y(), min.z())); // 2: +X+Y from min
            vertices.push_back(Float3(min.x(), max.y(), min.z())); // 3: +Y from min
            vertices.push_back(Float3(min.x(), min.y(), max.z())); // 4: +Z from min
            vertices.push_back(Float3(max.x(), min.y(), max.z())); // 5: +X+Z from min
            vertices.push_back(Float3(max.x(), max.y(), max.z())); // 6: max corner
            vertices.push_back(Float3(min.x(), max.y(), max.z())); // 7: +Y+Z from min

            material = material_;
        }

        unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override {
            type = RTC_GEOMETRY_TYPE_QUAD;
            geom = rtcNewGeometry(device, type);
            float* vb = (float*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3*sizeof(float), 8);
            int* ib = (int*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT4, 4*sizeof(unsigned int), 6);
            vertex_count = 8;

            // Fill vertex buffer with the 8 unique vertices
            for (int i = 0; i < 8; i++) {
                vb[i * 3 + 0] = vertices[i].x();
                vb[i * 3 + 1] = vertices[i].y();
                vb[i * 3 + 2] = vertices[i].z();
            }

            // Define the 6 faces using indices to the 8 vertices
            // Each face is defined as a quad with 4 vertex indices
            unsigned int faces[6][4] = {
                {4, 5, 6, 7}, // Front face (Z = max.z)
                {1, 0, 3, 2}, // Back face (Z = min.z)
                {5, 1, 2, 6}, // Right face (X = max.x)
                {0, 4, 7, 3}, // Left face (X = min.x)
                {3, 7, 6, 2}, // Top face (Y = max.y)
                {0, 1, 5, 4}  // Bottom face (Y = min.y)
            };

            // Fill index buffer
            for (int face = 0; face < 6; face++) {
                for (int vertex = 0; vertex < 4; vertex++) {
                    ib[face * 4 + vertex] = faces[face][vertex];
                }
            }

            // Initialize AABB with current vertices
            updateAABB();

            // commit and release geom handle
            rtcCommitGeometry(geom);
            geomid = rtcAttachGeometry(scene, geom);
            return geomid;
        }

        Material* getMaterial(unsigned int primid, unsigned int instid) override {
            return material;
        }

        void updateAABB() override {
            bounding_box.update(vertices);
        }

        std::vector<Float3> vertices;
        Material* material;
};

class Quad : public Geometry {
    public:
        Quad(const Float3& q, const Float3& u, const Float3& v, Material* material_) {
            c1 = q;
            c2 = q + u;
            c3 = q + v;
            c4 = q + u + v;
            material = material_;
        }

        unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override {
            type = RTC_GEOMETRY_TYPE_QUAD;
            geom = rtcNewGeometry(device, type);
            float* vb = (float*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3*sizeof(float), 4);
            vb[0] = c1.x(); vb[1] = c1.y(); vb[2] = c1.z();
            vb[3] = c2.x(); vb[4] = c2.y(); vb[5] = c2.z();
            vb[6] = c3.x(); vb[7] = c3.y(); vb[8] = c3.z();
            vb[9] = c4.x(); vb[10] = c4.y(); vb[11] = c4.z();
            vertex_count = 4;

            int* ib = (int*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT4, 4*sizeof(unsigned int), 1);
            ib[0] = 0; ib[1] = 1; ib[2] = 3; ib[3] = 2;

            // Initialize AABB with current vertices
            updateAABB();

            // commit and release geom handle
            rtcCommitGeometry(geom);
            geomid = rtcAttachGeometry(scene, geom);
            return geomid;
        }

        Material* getMaterial(unsigned int primid, unsigned int instid) override {
            return material;
        }

        void updateAABB() override {
            bounding_box.update({c1, c2, c3, c4});
        }

        Float3 c1, c2, c3, c4;
        Material* material;
};

class Sphere : public Geometry {
    public:
        Sphere(const Float3& pos, float r, Material* material) : material(material), pos(pos), r(r) {}

        unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override {
            type = RTC_GEOMETRY_TYPE_SPHERE_POINT;
            geom = rtcNewGeometry(device, type);
            float* vb = (float*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4*sizeof(float), 1);
            vb[0] = pos.x(); vb[1] = pos.y(); vb[2] = pos.z(); vb[3] = r;
            vertex_count = 1; // ??

            // Initialize AABB with current vertices
            updateAABB();

            // commit and release geom handle
            rtcCommitGeometry(geom);
            geomid = rtcAttachGeometry(scene, geom);
            return geomid;
        }

        // Apply the current transform matrix to sphere
        void applyTransform() override {
            if (!geom) {
                std::cerr << "Error: Geometry not initialized" << std::endl;
                return;
            }

            if (transform->isIdentity())
                return;

            // Get the vertex buffer data
            float* vb = (float*)rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, 0);
            if (!vb) {
                std::cerr << "Error: Could not get vertex buffer data" << std::endl;
                return;
            }

            // Transform sphere position (note: Float4 format with stride 4)
            Float4 vertex(vb[0], vb[1], vb[2], vb[3]);
            Float4 transformed_vertex = transform->transformPoint(vertex);

            // Write back transformed vertex
            vb[0] = transformed_vertex.x();
            vb[1] = transformed_vertex.y();
            vb[2] = transformed_vertex.z();
            vb[3] = transformed_vertex.w();

            // Update pos member variable
            pos = Float3(vb[0], vb[1], vb[2]);

            // Update AABB after transformation
            updateAABB();

            // Mark geometry as modified and recommit
            rtcUpdateGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0);
            rtcCommitGeometry(geom);
        }

        Material* getMaterial(unsigned int primid, unsigned int instid) override {
            return material;
        }

        void updateAABB() override {
            bounding_box.update({pos - Float3(r, r, r), pos + Float3(r, r, r)});
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


            // add new vertex buffer to geometry
            // rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices.data(), 0, sizeof(Float3), vertices.size());
            Float3* vtx = (Float3*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Float3), vertices.size());
            for (unsigned int i = 0; i < vertices.size(); i++) vtx[i] = vertices[i];
            vertex_count = vertices.size();

            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, indices.data(), 0, 3 * sizeof(unsigned int), int(indices.size() / 3));

            // Tangents: keep loader-provided ones (e.g. glTF TANGENT),
            // otherwise derive them from the UV layout. RayHit reads
            // slot 2 unconditionally, so this must always be registered.
            if (tangents.empty())
                computeTangents();

            rtcSetGeometryVertexAttributeCount(geom, 3);
            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTC_FORMAT_FLOAT3, normals.data(), 0, sizeof(Float3), normals.size());
            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTC_FORMAT_FLOAT2, texcoords.data(), 0, sizeof(Float2), texcoords.size());
            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, RTC_FORMAT_FLOAT4, tangents.data(), 0, 4 * sizeof(float), vertices.size());


            // Initialize AABB with current vertices
            updateAABB();

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
            return materials.at(material_mapping.at(primid));
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

        void updateAABB() override {
            bounding_box.update(vertices);
        }

        std::vector<Float3> vertices;
        std::vector<Float3> normals;
        std::vector<Float2> texcoords;
        std::vector<unsigned int> indices;
        // Per-vertex float4 tangents (xyz = dP/du, w = uv handedness) for
        // vertex attribute slot 2. Layout matches glTF TANGENT, so loaders
        // with authored tangents can fill this directly.
        std::vector<float> tangents;
        std::unordered_map<int, Material*> materials;
        std::unordered_map<int, int> material_mapping;
        Material* material_override = nullptr;

    private:
        // Smooth per-vertex tangents derived from the UV layout, built the
        // same way smooth normals are: solve dP/du per triangle
        //   [e1 e2] = [du1 dv1; du2 dv2] * [T; B]
        // then accumulate into vertices and normalize. Meshes without
        // usable UVs get a zero-filled buffer, which shading treats as
        // "no tangent" and falls back to a derived frame.
        void computeTangents() {
            const size_t vcount = vertices.size();
            tangents.assign(vcount * 4, 0.0f);

            if (texcoords.size() != vcount)
                return; // no per-vertex UVs -> zero tangents (fallback)

            std::vector<Float3> accum(vcount, Float3(0.0f));
            std::vector<float> sign(vcount, 0.0f);

            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                const unsigned int i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];

                const Float3 e1 = vertices[i1] - vertices[i0];
                const Float3 e2 = vertices[i2] - vertices[i0];
                const float du1 = texcoords[i1].u() - texcoords[i0].u();
                const float dv1 = texcoords[i1].v() - texcoords[i0].v();
                const float du2 = texcoords[i2].u() - texcoords[i0].u();
                const float dv2 = texcoords[i2].v() - texcoords[i0].v();

                const float det = du1 * dv2 - du2 * dv1;
                if (std::abs(det) < 1e-12f)
                    continue; // degenerate uvs -> skip triangle

                const Float3 t = (1.0f / det) * (dv2 * e1 - dv1 * e2); // dP/du
                const Float3 b = (1.0f / det) * (du1 * e2 - du2 * e1); // dP/dv
                const float s = dot(cross(cross(e1, e2), t), b) < 0.0f ? -1.0f : 1.0f;

                accum[i0] = accum[i0] + t;  accum[i1] = accum[i1] + t;  accum[i2] = accum[i2] + t;
                sign[i0] += s;              sign[i1] += s;              sign[i2] += s;
            }

            for (size_t i = 0; i < vcount; ++i) {
                if (dot(accum[i], accum[i]) > 1e-12f) {
                    const Float3 t = accum[i].normalize();
                    tangents[i * 4 + 0] = t.x();
                    tangents[i * 4 + 1] = t.y();
                    tangents[i * 4 + 2] = t.z();
                }
                tangents[i * 4 + 3] = sign[i] < 0.0f ? -1.0f : 1.0f;
            }
        }
};
