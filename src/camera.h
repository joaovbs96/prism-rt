#pragma once

#include <embree4/rtcore.h>

#include "math.h"
#include "float3.h"
#include "rng.h"
#include "ray.h"
#include "sampling.h"

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
            const float vfov_,
            const float defocus_angle_,
            const float focus_dist_
        )
        {
            defocus_angle = defocus_angle_;
            camera_center = position_;

            float theta = deg2rad(vfov_);
            float h = std::tanf(theta / 2);

            float viewport_height = 2.0f * h *  focus_dist_;
            float viewport_width = viewport_height * aspect_ratio_;

            // calculate u,v,w unit basis vector for the camera frame
            w = (camera_center - target_).normalize();
            u = cross(up_, w).normalize();
            v = cross(w, u);

            // Calculate the vectors across the horizontal and down the vertical viewport edges.
            Float3 viewport_u = viewport_width * u;
            Float3 viewport_v = viewport_height * -v;

            // Calculate the horizontal and vertical delta vectors from pixel to pixel.
            pixel_delta_u = viewport_u / image_width;
            pixel_delta_v = viewport_v / image_height;

            // Calculate the location of the upper left pixel.
            auto viewport_upper_left = camera_center - (focus_dist_ * w) - viewport_u/2 - viewport_v/2;
            pixel00_loc = viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);

            // Calculate the camera defocus disk basis vectors
            float defocus_radius = focus_dist_ * std::tanf(deg2rad(defocus_angle / 2));
            defocus_disk_u = u * defocus_radius;
            defocus_disk_v = v * defocus_radius;
        }

        Float3 sample_square(int s_s, int s_t, int strat_factor, unsigned int &seed) const {
            // Returns the vector to a random point in the square sub-pixel specified by grid
            // indices s_i and s_j, for an idealized unit square pixel [-.5,-.5] to [+.5,+.5].
            float x = (s_s + rnd(seed)) * strat_factor;
            float y = (s_t + rnd(seed)) * strat_factor;
            return Float3(x - 0.5, y - 0.5, 0);
        }

        Float3 sample_disk(unsigned int &seed) const {
            // Returns a random point in the camera defocus disk.
            Float3 p = random_in_unit_disk(seed);
            return camera_center + (p[0] * defocus_disk_u) + (p[1] * defocus_disk_v);
        }

        RTCRay get_ray(int s, int t, int s_s, int s_t, int strat_factor, unsigned int &seed) {
            // Construct a camera ray originating from the defocus disk and directed at a randomly
            // sampled point around the pixel location s, t.
            Float3 pixel_offset = sample_square(s_s, s_t, strat_factor, seed);
            Float3 pixel_center = pixel00_loc + ((s + pixel_offset.x()) * pixel_delta_u) + ((t + pixel_offset.y()) * pixel_delta_v);
            Float3 ray_origin = (defocus_angle <= 0) ? camera_center : sample_disk(seed);
            Float3 ray_direction = pixel_center - ray_origin;
            return initialize_ray(ray_origin, ray_direction);
        }

    private:
        Float3 camera_center;
        Float3 pixel00_loc;
        Float3 pixel_delta_u, pixel_delta_v;
        Float3 u, v, w;
        Float3 defocus_disk_u, defocus_disk_v;
        float defocus_angle;
};
