#pragma once

#include "float3.h"
#include "sampling.h"
#include "rng.h"
#include "ray.h"
#include "texture.h"
#include <cstddef>
#include <cstdint>
#include <optional>

struct BRDFSample {
    Float3 wi;
    Color3 color;
    Color3 emission;
    bool scattered;
    float pdf = 0.0f;
    bool delta = false;
    bool null_event = false;
    bool nee_compatible = false;
    bool transmitted = false;   // crossed the interface (enters or leaves the medium)
};

// The homogeneous medium behind a surface, per scene unit (MDL material_volume).
struct SurfaceMedium {
    Color3 absorption = Color3(0.0f);
    Color3 scattering = Color3(0.0f);
    float anisotropy = 0.0f;    // Henyey-Greenstein g
};

struct BRDFEvaluation {
    Color3 value = Color3(0.0f); // BSDF times abs(n.wi), in solid-angle measure
    float pdf = 0.0f;
    bool valid = false;
    Color3 diffuse = Color3(0.0f); // the diffuse lobes' part of value (the rest is glossy)
};

// Emission that is the same at every point and in every direction of a
// material (emissive triangles are then sampled as lights).
struct ConstantEmission {
    Color3 radiance = Color3(0.0f);
    bool twoSided = false;   // false: the front face only
};

class Material {
    public:
        struct PreparedShadingContext {
            alignas(16) std::byte storage[3072];
            const Material* owner = nullptr;
            bool valid = false;
            // Optional sample point for the BSDF sample of materials that
            // take one (MDL); otherwise they draw from the generator.
            bool hasXi = false;
            float xi[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        };
        static_assert(alignof(PreparedShadingContext) >= 16);
        Material() {}
        virtual ~Material() = default;

        // All materials need to define:
        // 1) How to sample the ray direction for the next bounce
        // 2) The color that the material contributes to the ray

        virtual BRDFSample sample(const RayHit ray_hit, unsigned int &seed) {
            BRDFSample s;
            s.wi = sample_direction(ray_hit, seed);
            s.color = evaluate_color(ray_hit.p, ray_hit.n, -ray_hit.d, s.wi,
                ray_hit.u, ray_hit.v, seed, ray_hit.texture_footprint);
            s.emission = evaluate_emission(ray_hit);
            s.scattered = !s.wi.is_zero();
            return s;
        }

        virtual BRDFEvaluation evaluate(const RayHit&, const Float3&) { return {}; }
        virtual bool prepare(const RayHit&, PreparedShadingContext&) const { return false; }
        virtual BRDFSample samplePrepared(const RayHit h,unsigned int& seed,
                                          PreparedShadingContext&) { return sample(h,seed); }
        virtual BRDFEvaluation evaluatePrepared(const RayHit& h,const Float3& wi,
                                                PreparedShadingContext&) {
            return evaluate(h,wi);
        }
        virtual bool supportsNee(const RayHit&) const { return false; }
        virtual float cutoutOpacity(const RayHit&) { return 1.0f; }
        virtual bool provenOpaque() const { return false; }
        // The BSDF may transmit (refract): back faces of single-sided geometry
        // with such a material are never culled (prism:honorDoubleSided).
        virtual bool transmissive() const { return false; }
        virtual std::optional<ConstantEmission> constantEmission() const { return std::nullopt; }
        // Displacement of the surface (applied to the geometry when it is built).
        virtual bool hasDisplacement() const { return false; }
        virtual Float3 displacement(const RayHit&) const { return Float3(0.0f); }
        // The medium entered by transmission through the front face (after prepare).
        virtual std::optional<SurfaceMedium> medium(const RayHit&, PreparedShadingContext&) const {
            return std::nullopt;
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
        virtual Color3 evaluate_color(
            const Float3& p, const Float3& n, const Float3& wo, Float3& wi,
            float u, float v, unsigned int &seed,
            prism::TextureFootprint const&) {
            return evaluate_color(p,n,wo,wi,u,v,seed);
        }

        virtual Color3 evaluate_emission(const RayHit ray_hit) {
            return Color3(0.0f);
        }

        // The geometric normal of the hit, before shading (the interpolated
        // normal stays the shading normal: no material perturbs it here).
        void perturbNormal(RayHit& ray_hit) {
            ray_hit.n_g = ray_hit.n.normalize();
            ray_hit.n_s = Float3(0.0f);
        }
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
        Color3 evaluate_color(const Float3& p, const Float3&, const Float3&,
            Float3&, float u, float v, unsigned int&,
            prism::TextureFootprint const& footprint) override {
            return diffuse_color->sample(u,v,p,footprint);
        }

        BRDFSample sample(const RayHit ray_hit, unsigned int &seed) override {
            BRDFSample s = Material::sample(ray_hit, seed);
            const Float3 wi = s.wi.normalize();
            s.pdf = std::max(0.0f, dot(ray_hit.n, wi)) / PI;
            s.nee_compatible = s.pdf > 0.0f;
            return s;
        }
        BRDFEvaluation evaluate(const RayHit& hit, const Float3& wi) override {
            const float cosine = std::max(0.0f, dot(hit.n, wi.normalize()));
            BRDFEvaluation e;
            e.value = diffuse_color->sample(
                hit.u, hit.v, hit.p, hit.texture_footprint) * (cosine / PI);
            e.pdf = cosine / PI; e.valid = cosine > 0.0f; e.diffuse = e.value; return e;
        }
        bool supportsNee(const RayHit&) const override { return true; }

        Texture<Float3>* diffuse_color;
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
            return emissive_color->sample(ray_hit.u, ray_hit.v, ray_hit.p,
                ray_hit.texture_footprint) * emissive_multiplier;
        }

        std::optional<ConstantEmission> constantEmission() const override {
            auto const* value = dynamic_cast<ValueTexture<Color3> const*>(emissive_color);
            if (!value) return std::nullopt;
            return ConstantEmission{value->value * emissive_multiplier, true};
        }

        Texture<Color3>* emissive_color;
        float emissive_multiplier;
};

// The material of a material that failed to load or compile: magenta
// emission without scattering, as an invalid MDLMaterial renders. It is not a
// light (no constant emission), so it never brightens its surroundings.
class ErrorMaterial : public Emissive {
    public:
        ErrorMaterial() : Emissive(&m_magenta) {}
        std::optional<ConstantEmission> constantEmission() const override { return std::nullopt; }
        // One instance serves every failed material.
        static ErrorMaterial* Shared() { static ErrorMaterial material; return &material; }

    private:
        ValueTexture<Color3> m_magenta{Color3(1.0f, 0.0f, 1.0f)};
};
