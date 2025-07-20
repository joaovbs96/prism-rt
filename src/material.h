#pragma once

#include "float3.h"
#include "sampling.h"
#include "rng.h"

struct BRDFSample {
    Float3 wi;
    Color3 color;
};

class Material {
    public:
        Material() {}
        
        // All materials need to define:
        // 1) How to sample the ray direction for the next bounce
        // 2) The color that the material contributes to the ray

        virtual BRDFSample sample(const Float3& p, const Float3& n, const Float3& wo) {
            BRDFSample s;
            s.wi = sample_direction(p, n, wo);
            s.color = evaluate_color(p, n, wo, s.wi);
            return s;
        }

        virtual Float3 sample_direction(const Float3& p, const Float3& n, const Float3& wo) = 0;

        virtual Color3 evaluate_color(
            const Float3& p,    // surface hit point
            const Float3& n,    // surface normal vector
            const Float3& wo,   // ray outgoing towards the camera
            Float3& wi    // ray incoming from the light
        ) = 0;

        Float3 reflect(const Float3& i, const Float3& n) {
            return i - 2 * dot(i, n) * n;
        }

        Float3 refract(const Float3& wi, const Float3& n, const float ior) {
            float cost_theta = std::fminf(dot(-wi, n), 1.0f);
            Float3 r_1 = ior * (wi + cost_theta * n);
            Float3 r_2 = -std::sqrtf(std::fabsf(1.0f - r_1.length_squared())) * n;
            return r_1 + r_2;
        }

        bool is_front_facing(const Float3& d, const Float3& n) {
            return dot(d, n) > 0.0f;
        }

        float schlick(const float cos, float ior) {
            float r0 = (1.0f - ior) / (1.0f + ior);
            r0 = r0 * r0;
            return r0 + (1.0f - r0) * std::powf(1.0f - cos, 5.0f);
        }
};

class Diffuse : public Material {
    public:
        Diffuse(const Color3&diffuse_color) : diffuse_color(diffuse_color) {}

        Float3 sample_direction(const Float3& p, const Float3& n, const Float3& wo) override {
            // note that the ray incoming from the light (wi) is the next/scattered ray
            // don't forget that a ray tracer works "in reverse"
            Float3 wi = n + random_unit_vector();

            // if the incoming ray is near zero, we take the normal instead
            if (wi.is_near_zero())
                wi = n;

            return wi;
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi) override {
            return diffuse_color;
        }

        Color3 diffuse_color;
};

class Metal : public Material {
    public:
        Metal(const Color3&specular_color, const float specular_roughness) : specular_color(specular_color), specular_roughness(specular_roughness) {}

        Float3 sample_direction(const Float3& p, const Float3& n, const Float3& wo) override {
            // note that the ray incoming from the light (wi) is the next/scattered ray
            // don't forget that a ray tracer works "in reverse"
            Float3 wi = reflect(-wo, n).normalize() + specular_roughness * random_unit_vector();
            return wi;
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi) override {
            return specular_color;
        }

        Color3 specular_color;
        float specular_roughness;
};

class Glass : public Material {
    public:
        Glass (const float ior) : ior(ior) {}

        Float3 sample_direction(const Float3& p, const Float3& n, const Float3& wo) override {
            Float3 direction = -wo;
            bool is_front = is_front_facing(direction, n);
            Float3 n_ = is_front ? n : -n;
            float ior_ = is_front ? 1.0f / ior : ior;

            Float3 unit_direction = direction.normalize();
            float cos_theta = std::fminf(dot(-unit_direction, n_), 1.0f);
            float sin_theta = std::sqrtf(1.0f - cos_theta * cos_theta);

            // ray is exiting the object
            if (ior_ * sin_theta > 1.0f || schlick(cos_theta, ior_) > rnd()) {
                return reflect(unit_direction, n_);
            } else { // ray is entering the object
                return refract(unit_direction, n_, ior_);
            }
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi) override {
            return Color3(1.0f);
        }

    private:
        float ior;
};

class gltfPBR : public Material {
    public:
        gltfPBR(const Color3&base_color_, const float metalness_, const float roughness_) {
            base_color = base_color_;
            metalness = metalness_;
            roughness = roughness_;

            diffuse_brdf = new Diffuse(base_color);
            metal_brdf = new Metal(base_color, roughness);
        }

        BRDFSample sample(const Float3& p, const Float3& n, const Float3& wo) override {
            float prob = rnd();
            if (prob <= metalness){
                return metal_brdf->sample(p, n, wo);
            } else {
                return diffuse_brdf->sample(p, n, wo);
            }
        }

        // we won't need these: we will sample directly fomr the brdfs
        Float3 sample_direction(const Float3& p, const Float3& n, const Float3& wo) override { return Float3(); }
        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi) override {  return Color3(); }

    Color3 base_color;
    float metalness;
    float roughness;
    Diffuse* diffuse_brdf;
    Metal* metal_brdf;
};
