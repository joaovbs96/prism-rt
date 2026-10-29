#pragma once

#include <string>
#include <array>
#include <vector>


// STD Lib deps
#include <iostream>
#include <filesystem>
#include <cmath>

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
        virtual ~Geometry() = default;

        virtual unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) = 0;

        virtual Material* getMaterial(unsigned int primid, unsigned int instid) = 0;

        virtual bool analyticSphere(unsigned int, Float3 *, float *) const {
            return false;
        }

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
        RTCGeometry geom = nullptr;
        unsigned int geomid;
        TransformMatrix* transform = new TransformMatrix();
        AABB bounding_box;
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

        bool analyticSphere(unsigned int primid, Float3 *center,
                            float *radius) const override {
            if (primid != 0 || !(r > 0.0f) || !std::isfinite(r)) return false;
            if (center) *center = pos;
            if (radius) *radius = r;
            return true;
        }

        void updateAABB() override {
            bounding_box.update({pos - Float3(r, r, r), pos + Float3(r, r, r)});
        }

        Float3 pos;
        float r;
        Material* material;
};

// Instance-rate primvars of one placement (PointInstancer primvars, the
// inherited primvars of native instances): constant over the placement.
// MDL scene data lookups read them before the mesh's own primvars.
struct InstancePrimvars {
    struct Value {
        std::string name;
        int components = 0;
        std::array<float, 4> value{};
    };
    std::vector<Value> values;
    bool lookup(const char* name, float* out, int count) const {
        for (auto const& v : values) {
            if (v.name != name) continue;
            for (int c = 0; c < count; ++c) out[c] = c < v.components ? v.value[size_t(c)] : 0.0f;
            return true;
        }
        return false;
    }
};

class Mesh : public Geometry {
    public:
        Mesh() {}

        unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override {
            createEmbreeGeometry(device);
            geomid = rtcAttachGeometry(scene, geom);
            // rtcReleaseGeometry(geom);

            return geomid;
        }

        // setupEmbree without attaching: creates and commits `geom`, which
        // the caller attaches (to one or more scenes, at ids of its choice).
        void createEmbreeGeometry(RTCDevice &device) {
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

        // Named primvars for materials (MaterialX geompropvalue), per vertex:
        // primvarStride floats per vertex in primvarValues, each primvar at
        // its offset.
        struct Primvar {
            std::string name;
            int components = 0;
            int offset = 0;
        };
        std::vector<Primvar> primvars;
        // USD doubleSided false per triangle (empty: none), for the CLI's
        // merged mesh (SceneManager::culledBackFace).
        std::vector<uint8_t> single_sided;
        // Every triangle is single-sided (the delegate's meshes: USD
        // doubleSided false).
        bool single_sided_all = false;
        int primvarStride = 0;
        std::vector<float> primvarValues;

        // The primvar at barycentrics (u, v) of triangle prim (Embree's
        // convention: u weighs the second vertex, v the third). Absent
        // components are zero; false if the mesh has no such primvar.
        bool interpolatePrimvar(const char* name, unsigned prim, float u, float v, float* out, int count) const {
            if (!name || !primvarStride || 3 * size_t(prim) + 2 >= indices.size()) return false;
            for (auto const& primvar : primvars) {
                if (primvar.name != name) continue;
                const unsigned* tri = &indices[3 * size_t(prim)];
                const float w[3] = {1.0f - u - v, u, v};
                for (int c = 0; c < count; ++c) {
                    float value = 0.0f;
                    if (c < primvar.components)
                        for (int k = 0; k < 3; ++k)
                            value += w[k] * primvarValues[size_t(tri[k]) * primvarStride + primvar.offset + c];
                    out[c] = value;
                }
                return true;
            }
            return false;
        }

        // Finalize the vertex tangent stream before geometry fingerprinting.
        // setupEmbree() performs the same operation, but scene synchronization
        // needs stable derived data before deciding whether prototypes share
        // storage.
        void ensureTangents() {
            if (tangents.empty()) computeTangents();
        }

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
