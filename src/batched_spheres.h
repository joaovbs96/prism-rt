#pragma once

#include "geometry.h"

#include <vector>

// One Embree sphere-point geometry containing independent analytic spheres.
// Hydra owns the per-entry identity and binding tables; this class only owns
// intersection data and the local analytic-sphere parameters used for UV
// derivatives.
class BatchedSpheres final : public Geometry {
public:
    struct Entry {
        Float3 center;
        float radius = 1.0f;
        Material *material = nullptr;
    };

    std::vector<Entry> entries;

    unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override
    {
        type = RTC_GEOMETRY_TYPE_SPHERE_POINT;
        geom = rtcNewGeometry(device, type);
        float *vertices = static_cast<float *>(rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4,
            4 * sizeof(float), entries.size()));
        for (size_t i = 0; i < entries.size(); ++i) {
            vertices[4 * i] = entries[i].center.x();
            vertices[4 * i + 1] = entries[i].center.y();
            vertices[4 * i + 2] = entries[i].center.z();
            vertices[4 * i + 3] = entries[i].radius;
        }
        vertex_count = static_cast<int>(entries.size());
        updateAABB();
        rtcCommitGeometry(geom);
        geomid = rtcAttachGeometry(scene, geom);
        return geomid;
    }

    void applyTransform() override {}

    Material *getMaterial(unsigned int primid, unsigned int) override
    {
        return primid < entries.size() ? entries[primid].material : nullptr;
    }

    bool analyticSphere(unsigned int primid, Float3 *center,
                        float *radius) const override
    {
        if (primid >= entries.size() || !(entries[primid].radius > 0.0f) ||
            !std::isfinite(entries[primid].radius)) return false;
        if (center) *center = entries[primid].center;
        if (radius) *radius = entries[primid].radius;
        return true;
    }

    void updateAABB() override
    {
        std::vector<Float3> bounds;
        bounds.reserve(entries.size() * 2);
        for (Entry const &entry : entries) {
            bounds.push_back(entry.center - Float3(entry.radius));
            bounds.push_back(entry.center + Float3(entry.radius));
        }
        bounding_box.update(bounds);
    }
};
