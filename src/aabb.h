#pragma once

#include "float3.h"
#include <vector>
#include <limits>

class AABB {
    public:
        AABB() : min_point(Float3(0, 0, 0)), max_point(Float3(0, 0, 0)), is_valid(false) {}

        // Construct AABB from a list of vertices
        AABB(const std::vector<Float3>& vertices) {
            if (vertices.empty()) {
                min_point = Float3(0, 0, 0);
                max_point = Float3(0, 0, 0);
                is_valid = false;
                return;
            }

            min_point = vertices[0];
            max_point = vertices[0];

            for (const auto& vertex : vertices) {
                min_point = min_point.min(vertex);
                max_point = max_point.max(vertex);
            }

            is_valid = true;
        }

        // Get the minimum point (bottom corner) of the bounding box
        Float3 getMin() const {
            return min_point;
        }

        // Get the maximum point (top corner) of the bounding box
        Float3 getMax() const {
            return max_point;
        }

        // Get the center of the bounding box
        Float3 getCenter() const {
            return Float3(
                (min_point.x() + max_point.x()) * 0.5f,
                (min_point.y() + max_point.y()) * 0.5f,
                (min_point.z() + max_point.z()) * 0.5f
            );
        }

        // Get the size/dimensions of the bounding box
        Float3 getSize() const {
            return Float3(
                max_point.x() - min_point.x(),
                max_point.y() - min_point.y(),
                max_point.z() - min_point.z()
            );
        }

        // Get the diagonal length of the bounding box
        float getDiagonal() const {
            Float3 size = getSize();
            return sqrt(size.x() * size.x() + size.y() * size.y() + size.z() * size.z());
        }

        // Update the AABB with a new set of vertices
        void update(const std::vector<Float3>& vertices) {
            if (vertices.empty()) {
                is_valid = false;
                return;
            }

            min_point = vertices[0];
            max_point = vertices[0];

            for (const auto& vertex : vertices) {
                min_point = min_point.min(vertex);
                max_point = max_point.max(vertex);
            }

            is_valid = true;
        }

        // Check if the AABB is valid (has been initialized with vertices)
        bool isValid() const {
            return is_valid;
        }

    private:
        Float3 min_point;
        Float3 max_point;
        bool is_valid;
};