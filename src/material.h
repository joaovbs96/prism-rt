#pragma once

#include "float3.h"
#include "sampling.h"
#include "rng.h"
#include "ray.h"
#include "texture.h"

struct BRDFSample {
    Float3 wi;
    Color3 color;
    Color3 emission;
    bool scattered;
};

class Material {
    public:
        Material() {}

        // All materials need to define:
        // 1) How to sample the ray direction for the next bounce
        // 2) The color that the material contributes to the ray

        virtual BRDFSample sample(const RayHit ray_hit, unsigned int &seed) {
            BRDFSample s;
            s.wi = sample_direction(ray_hit, seed);
            s.color = evaluate_color(ray_hit.p, ray_hit.n, -ray_hit.d, s.wi, ray_hit.u, ray_hit.v, seed);
            s.emission = evaluate_emission(ray_hit);
            s.scattered = !s.wi.is_zero();
            return s;
        }

        virtual Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) = 0;

        virtual Color3 evaluate_color(
            const Float3& p,    // surface hit point
            const Float3& n,    // surface normal vector
            const Float3& wo,   // ray outgoing towards the camera
            Float3& wi,         // ray incoming from the light
            float u, float v,   // UV coordinates
            unsigned int &seed  // random number generator seed
        ) = 0;

        virtual Color3 evaluate_emission(const RayHit ray_hit) {
            return Color3(0.0f);
        }

        Float3 reflect(const Float3& i, const Float3& n) {
            return i - 2 * dot(i, n) * n;
        }

        Float3 refract(const Float3& wi, const Float3& n, const float ior) {
            float cost_theta = std::fminf(dot(-wi, n), 1.0f);
            Float3 r_1 = ior * (wi + cost_theta * n);
            Float3 r_2 = -std::sqrtf(std::fabsf(1.0f - r_1.length_squared())) * n;
            return r_1 + r_2;
        }

        float schlick(const float cos, float ior) {
            float r0 = (1.0f - ior) / (1.0f + ior);
            r0 = r0 * r0;
            return r0 + (1.0f - r0) * std::powf(1.0f - cos, 5.0f);
        }

        void setNormalTexture(Texture<Float3>* normal_texture_){
            normal_texture = normal_texture_;
        }

        Float3 getShadingNormal(const RayHit ray_hit){
            if (normal_texture == nullptr)
                return Float3(0.0f);

            return normal_texture->sample(ray_hit.u, ray_hit.v, ray_hit.p);
        }

        void perturbNormal(RayHit& ray_hit) {
            ray_hit.n_g = ray_hit.n.normalize();
            ray_hit.n_s = getShadingNormal(ray_hit);
            if (ray_hit.n_s.is_zero())
                return;

            // Convert shading normal from [0,1] range to [-1,1] range
            // (assuming n_s comes from a texture in [0,1] range)
            Float3 tangent_n_s = ray_hit.n_s * 2.0f - 1.0f;

            // Create tangent and bitangent vectors
            // Choose an arbitrary vector that's not parallel to N
            Float3 up = (std::fabsf(ray_hit.n_g.y()) < 0.999f) ? Float3(0, 1, 0) : Float3(1, 0, 0);
            Float3 T = cross(up, ray_hit.n_g);  // Tangent
            Float3 B = cross(ray_hit.n_g, T);   // Bitangent

            // Transform the tangent-space normal to world space
            Float3 perturbed_n = T * tangent_n_s.x() + B * tangent_n_s.y() + ray_hit.n_g * tangent_n_s.z();
            ray_hit.n = perturbed_n.normalize();
        }

        Texture<Float3>* normal_texture = nullptr;
};

class Diffuse : public Material {
    public:
        Diffuse(Texture<Float3>* texture) : diffuse_color(texture) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            // note that the ray incoming from the light (wi) is the next/scattered ray
            // don't forget that a ray tracer works "in reverse"
            Float3 wi = ray_hit.n + random_unit_vector(seed);

            // if the incoming ray is near zero, we take the normal instead
            if (wi.is_near_zero())
                wi = ray_hit.n;

            return wi;
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return diffuse_color->sample(u, v, p);
        }

        Texture<Float3>* diffuse_color;
};

class Metal : public Material {
    public:
        Metal(Texture<Float3>* specular_color, Texture<float>* specular_roughness)
        : specular_color(specular_color), specular_roughness(specular_roughness) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            // note that the ray incoming from the light (wi) is the next/scattered ray
            // don't forget that a ray tracer works "in reverse"
            Float3 wi = reflect(ray_hit.d, ray_hit.n).normalize() + specular_roughness->sample(ray_hit.u, ray_hit.v, ray_hit.p) * random_unit_vector(seed);
            return wi.normalize();
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return specular_color->sample(u, v, p);
        }

        Texture<Float3>* specular_color;
        Texture<float>* specular_roughness;
};

class Emissive : public Material {
    public:
        Emissive(Texture<Float3>* emissive_color, float emissive_multiplier = 1.0f)
        : emissive_color(emissive_color), emissive_multiplier(emissive_multiplier) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            return Float3(0.0f);
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return Color3(0.0f);
        }

        Color3 evaluate_emission(const RayHit ray_hit) override {
            return emissive_color->sample(ray_hit.u, ray_hit.v, ray_hit.p) * emissive_multiplier;
        }

        Texture<Color3>* emissive_color;
        float emissive_multiplier;
};

class Glass : public Material {
    public:
        Glass (const float ior) : ior(ior) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            float ior_ = ray_hit.front_face ? 1.0f / ior : ior;

            Float3 unit_direction = ray_hit.d.normalize();
            float cos_theta = std::fminf(dot(-unit_direction, ray_hit.n), 1.0f);
            float sin_theta = std::sqrtf(1.0f - cos_theta * cos_theta);

            // ray is exiting the object
            // std::cout << "Test - " << "Front:" << is_front << "IOR:" << ior_ << "Theta:" << sin_theta << std::endl;
            if (ior_ * sin_theta > 1.0f || schlick(cos_theta, ior_) > rnd(seed)) {
                return reflect(unit_direction, ray_hit.n);
            } else { // ray is entering the object
                return refract(unit_direction, ray_hit.n, ior_);
            }
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return Color3(1.0f);
        }

    private:
        float ior;
};

class Dispersive : public Glass {
public:
    Dispersive(float ior, float abbe_number = 60.0f)
        : Glass(ior),  // Use provided IOR as base (d-line, 587nm yellow)
          n_d(ior),
          V_d(abbe_number) {

        // Calculate IORs for red (C-line, 656nm) and blue (F-line, 486nm)
        // using the Abbe number definition: V_d = (n_d - 1) / (n_F - n_C)
        // Rearranging: (n_F - n_C) = (n_d - 1) / V_d

        float delta_nFC = (n_d - 1.0f) / V_d;

        // For typical glass, the relationship is approximately:
        // n_d is midway between n_F and n_C
        // So: n_F = n_d + 0.5 * delta_nFC
        //     n_C = n_d - 0.5 * delta_nFC

        n_F = n_d + 0.5f * delta_nFC;  // Blue wavelength (higher IOR)
        n_C = n_d - 0.5f * delta_nFC;  // Red wavelength (lower IOR)
    }

    BRDFSample sample(const RayHit ray_hit, unsigned int &seed) override {
        BRDFSample s;

        // Randomly select which wavelength to trace (R, G, or B)
        float wavelength_choice = rnd(seed);
        float selected_ior;

        if (wavelength_choice < 0.333f) {
            // Red wavelength (C-line, 656nm)
            selected_ior = n_C;
            s.color = Color3(1.0f, 0.0f, 0.0f);
        } else if (wavelength_choice < 0.666f) {
            // Yellow/Green wavelength (d-line, 587nm)
            selected_ior = n_d;
            s.color = Color3(0.0f, 1.0f, 0.0f);
        } else {
            // Blue wavelength (F-line, 486nm)
            selected_ior = n_F;
            s.color = Color3(0.0f, 0.0f, 1.0f);
        }

        // Refract/reflect using the selected wavelength's IOR
        float ior_ = ray_hit.front_face ? 1.0f / selected_ior : selected_ior;

        Float3 unit_direction = ray_hit.d.normalize();
        float cos_theta = std::fminf(dot(-unit_direction, ray_hit.n), 1.0f);
        float sin_theta = std::sqrtf(1.0f - cos_theta * cos_theta);

        if (ior_ * sin_theta > 1.0f || schlick(cos_theta, ior_) > rnd(seed)) {
            s.wi = reflect(unit_direction, ray_hit.n);
        } else {
            s.wi = refract(unit_direction, ray_hit.n, ior_);
        }

        s.emission = Color3(0.0f);
        s.scattered = true;

        return s;
    }

private:
    float n_d;  // IOR at d-line (587nm, yellow - reference)
    float n_F;  // IOR at F-line (486nm, blue)
    float n_C;  // IOR at C-line (656nm, red)
    float V_d;  // Abbe number
};

////////////////////////////////////////////////////////////////////////////
////////////////////////////// glTF PBR Lobes //////////////////////////////
////////////////////////////////////////////////////////////////////////////

class DiffuseGltf : public Material {
    public:
        DiffuseGltf(Texture<Color3>* base_color) : base_color(base_color) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            // note that the ray incoming from the light (wi) is the next/scattered ray
            // don't forget that a ray tracer works "in reverse"
            Float3 wi = ray_hit.n + random_unit_vector(seed);

            // if the incoming ray is near zero, we take the normal instead
            if (wi.is_near_zero())
                wi = ray_hit.n;

            return wi;
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return base_color->sample(u, v, p);
        }

        Texture<Color3>* base_color;
};

class MetalGltf : public Material {
    public:
        MetalGltf(Texture<Float3>* base_color, Texture<Float3>* orm) : base_color(base_color), orm(orm) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            // note that the ray incoming from the light (wi) is the next/scattered ray
            // don't forget that a ray tracer works "in reverse"
            float roughness = orm->sample(ray_hit.u, ray_hit.v, ray_hit.p).y();
            Float3 wi = reflect(ray_hit.d, ray_hit.n).normalize() + (roughness * roughness) * random_unit_vector(seed);
            return wi.normalize();
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return base_color->sample(u, v, p);
        }

        Texture<Float3>* base_color;
        Texture<Float3>* orm;
};

class SpecularBTDFglTF : public Material {
    public:
        SpecularBTDFglTF (Texture<Float3>* base_color, const float ior) : base_color(base_color), ior(ior) {}

        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override {
            float ior_ = ray_hit.front_face ? 1.0f / ior : ior;

            Float3 unit_direction = ray_hit.d.normalize();
            float cos_theta = std::fminf(dot(-unit_direction, ray_hit.n), 1.0f);
            float sin_theta = std::sqrtf(1.0f - cos_theta * cos_theta);

            // ray is exiting the object
            // std::cout << "Test - " << "Front:" << is_front << "IOR:" << ior_ << "Theta:" << sin_theta << std::endl;
            if (ior_ * sin_theta > 1.0f || schlick(cos_theta, ior_) > rnd(seed)) {
                return reflect(unit_direction, ray_hit.n);
            } else { // ray is entering the object
                return refract(unit_direction, ray_hit.n, ior_);
            }
        }

        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {
            return base_color->sample(u, v, p);
        }

    Texture<Float3>* base_color;
    float ior;
};

class gltfPBR : public Material {
    public:
        gltfPBR(){}

        void setBaseColor(Texture<Float3>* base_color_){
            base_color = base_color_;
        }

        void setORM(Texture<Float3>* orm_){
            orm = orm_;
        }

        void setTransmission(Texture<float>* transmission_){
            transmission = transmission_;
        }

        void setEmission(Texture<Float3>* emissive_color_, float emissive_multiplier_){
            emissive_color = emissive_color_;
            emissive_multiplier = emissive_multiplier_;
        }

        void createBRDFs() {
            if (base_color == nullptr)
                base_color = new ColorTexture(Color3(1.0f));
            if (orm == nullptr)
                orm = new ValueTexture<Float3>(1.0f);
            if (transmission == nullptr)
                transmission = new ValueTexture<float>(0.0f);

            diffuse_brdf = new Diffuse(base_color);
            specular_btdf = new SpecularBTDFglTF(base_color, 1.5f);
            metal_brdf = new MetalGltf(base_color, orm);
        }

        BRDFSample sample(const RayHit ray_hit, unsigned int &seed) override {
            float prob = rnd(seed);
            if (prob <= orm->sample(ray_hit.u, ray_hit.v, ray_hit.p).b()){
                return metal_brdf->sample(ray_hit, seed);
            } else {
                if (prob <= transmission->sample(ray_hit.u, ray_hit.v, ray_hit.p)){
                    return specular_btdf->sample(ray_hit, seed);
                } else {
                    return diffuse_brdf->sample(ray_hit, seed);
                }
            }
        }

        Color3 evaluate_emission(const RayHit ray_hit) override {
            return emissive_color->sample(ray_hit.u, ray_hit.v, ray_hit.p) * emissive_multiplier;
        }

        // we won't need these: we will sample directly fomr the brdfs
        Float3 sample_direction(const RayHit ray_hit, unsigned int &seed) override { return Float3(); }
        Color3 evaluate_color(const Float3& p, const Float3& n, const Float3& wo, Float3& wi, float u, float v, unsigned int &seed) override {  return Color3(); }

    // Textures and factors
    Texture<Color3>* base_color = nullptr;
    Texture<Float3>* orm = nullptr;
    Texture<float>* transmission = nullptr;
    Texture<Color3>* emissive_color;
    float emissive_multiplier;

    // BRDF lobes
    Diffuse* diffuse_brdf = nullptr;
    MetalGltf* metal_brdf = nullptr;
    SpecularBTDFglTF* specular_btdf = nullptr;
};
