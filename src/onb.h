#pragma once

#include "float3.h"

// orthonormal basis
class ONB {
    public:
        ONB(const Float3 &n) {
            axis[2] = n.normalize();
            Float3 a = (std::fabsf(axis[2].x()) > 0.9f) ? Float3(0.0f, 1.0f, 0.0f) : Float3(1.0f, 0.0f, 0.0f);
            axis[1] = cross(axis[2], a).normalize();
            axis[0] = cross(axis[2], axis[1]);
        }

        const Float3& u() const { return axis[0]; }
        const Float3& v() const { return axis[1]; }
        const Float3& w() const { return axis[2]; }

        Float3 transform(float a, float b, float c) const {
            // Transform from basis coordinates to local space.
            return a * u() + b * v() + c * w();
        }

        Float3 transform(const Float3 &a) const {
            // Transform from basis coordinates to local space.
            return a.x() * u() + a.y() * v() + a.z() * w();
        }

        Float3 axis[3];
};