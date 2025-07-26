#pragma once

#include "float3.h"
#include "texture.h"
#include "ray.h"

class Environment {
    public:
    Environment() {}

    virtual Color3 sample(const RayHit& ray_hit) = 0;
};

class ConstantColorBackground : public Environment {
    public:
    ConstantColorBackground(ColorTexture* texture) : texture(texture) {}

    Color3 sample(const RayHit& ray_hit) override {
        return texture->sample(0, 0, Float3(0.0f));
    }

    ColorTexture* texture;
};

class GradientBackground : public Environment {
    public:
    GradientBackground(Texture<Color3>* t0, Texture<Color3>* t1) : t0(t0), t1(t1) {}

    Color3 sample(const RayHit& ray_hit) override {
        Float3 d = ray_hit.d.normalize();
        const float t = 0.5f * (d.y() + 1.f);

        // make gradient color
        Float3 c = (1.f - t) * t0->sample(0, 0, Float3(0.f));
        c += t * t1->sample(0, 0, Float3(0.f));
        return c;
    }

    Texture<Color3>* t0;
    Texture<Color3>* t1;
};


class EnvironmentMap : public Environment {
    public:
    EnvironmentMap(Texture<Color3>* texture) : texture(texture) {}

    Color3 sample(const RayHit& ray_hit) override {
        // spherical HDRI mapping
        // https://www.gamedev.net/forums/topic/637220-equirectangular-environment-map/
        Float3 d = ray_hit.d.normalize();
        float lon = std::atan2f(d.z(), d.x());
        float lat = std::acosf(d.y() / d.length());

        float u = lon / (PI * 2.f);
        float v = lat / PI;

        return 2.f * texture->sample(u, v, Float3(0.f));
    }

    Texture<Color3>* texture;
};

class CylindricalEnvironmentMap : public Environment {
    public:
    CylindricalEnvironmentMap(Texture<Color3>* texture) : texture(texture) {}

    Color3 sample(const RayHit& ray_hit) override {
        // Y is up, swap x for y and z for x
        Float3 d = ray_hit.d.normalize();
        float lon = std::atan2f(d.z(), d.x());
        float lat = std::acosf(d.y());

        // wrap around full circle if negative
        lon = lon < 0.f ? lon + (2.f * PI) : lon;

        // map theta and phi to u and v texturecoordinates in [0,1] x [0,1] range
        float u = 1.f - (lon / (2.f * PI));
        float v = lat / PI;

        return 2.f * texture->sample(u, v, Float3(0.f));
    }

    Texture<Color3>* texture;
};