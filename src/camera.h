#pragma once

#include <embree4/rtcore.h>

#include "math.h"
#include "float3.h"
#include "rng.h"
#include "ray.h"

class Camera {
    public:
        Camera() {}

        Camera(const int image_width, const int image_height)
        {
            auto focal_length = 1.0;
            auto viewport_height = 2.0;
            auto viewport_width = viewport_height * (double(image_width)/image_height);
            camera_center = Float3(0, 0, 0);

            // Calculate the vectors across the horizontal and down the vertical viewport edges.
            auto viewport_u = Float3(viewport_width, 0, 0);
            auto viewport_v = Float3(0, -viewport_height, 0);

            // Calculate the horizontal and vertical delta vectors from pixel to pixel.
            pixel_delta_u = viewport_u / image_width;
            pixel_delta_v = viewport_v / image_height;

            // Calculate the location of the upper left pixel.
            auto viewport_upper_left = camera_center
                                    - Float3(0, 0, focal_length) - viewport_u/2 - viewport_v/2;
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
        Float3 pixel_delta_u;
        Float3 pixel_delta_v;
};
