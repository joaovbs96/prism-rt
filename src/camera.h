#pragma once

#include <embree4/rtcore.h>

#include "math.h"
#include "float3.h"
#include "rng.h"
#include "ray.h"

class Camera {
    public:
        Camera() {}

        Camera(
            const Float3& position_,
            const Float3& target_,
            const Float3& up_,
            const int image_width,
            const int image_height,
            const float aspect_ratio_,
            const float vfov_)
        {
            aspect_ratio = aspect_ratio_;
            vfov = vfov_;

            float theta = deg2rad(vfov);
            float h = std::tanf(theta / 2);

            float focal_length = (position_ - target_).length();
            float viewport_height = 2.0f * h *  focal_length;
            float viewport_width = viewport_height * aspect_ratio;
            camera_center = position_;

            // calculate u,v,w unit basis vector for the camera frame
            w = (position_ - target_).normalize();
            u = cross(up_, w).normalize();
            v = cross(w, u);

            // Calculate the vectors across the horizontal and down the vertical viewport edges.
            Float3 viewport_u = viewport_width * u;
            Float3 viewport_v = viewport_height * -v;

            // Calculate the horizontal and vertical delta vectors from pixel to pixel.
            pixel_delta_u = viewport_u / image_width;
            pixel_delta_v = viewport_v / image_height;

            // Calculate the location of the upper left pixel.
            auto viewport_upper_left = camera_center
                                    - (focal_length * w) - viewport_u/2 - viewport_v/2;
            pixel00_loc = viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);
        }

        Float3 sample_square() const {
            return Float3(rnd() - 0.5, rnd() - 0.5, 0);
        }

        RTCRay get_ray(float s, float t) {
            auto pixel_offset = sample_square();
            auto pixel_center = pixel00_loc + ((s + pixel_offset.x()) * pixel_delta_u) + ((t + pixel_offset.y()) * pixel_delta_v);
            auto ray_origin = camera_center;
            auto ray_direction = pixel_center - camera_center;
            return initialize_ray(ray_origin, ray_direction);
        }

    private:
        Float3 camera_center;
        Float3 pixel00_loc;
        Float3 pixel_delta_u, pixel_delta_v;
        Float3 u, v, w;
        float vfov;
        float aspect_ratio;
};
