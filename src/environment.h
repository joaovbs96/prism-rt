#pragma once

#include "float3.h"
#include "texture.h"
#include "ray.h"
#include "rng.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <array>
#include <iostream>

struct EnvironmentSample {
    Float3 wi = Float3(0.0f);
    Color3 radiance = Color3(0.0f);
    float pdf = 0.0f;
};

class Environment {
    public:
    Environment() {}
    virtual ~Environment() = default;

    virtual Color3 sample(const RayHit& ray_hit) = 0;
    virtual Color3 radiance(const Float3& direction) const {
        RTCRayHit rh{};
        rh.ray.dir_x = direction.x(); rh.ray.dir_y = direction.y(); rh.ray.dir_z = direction.z();
        rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        return const_cast<Environment*>(this)->sample(RayHit(rh));
    }
    virtual EnvironmentSample sampleDirection(unsigned int& seed) const {
        const float y = 1.0f - 2.0f * rnd(seed);
        const float phi = 2.0f * PI * rnd(seed);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        EnvironmentSample s;
        s.wi = Float3(r * std::cos(phi), y, r * std::sin(phi));
        s.radiance = radiance(s.wi);
        s.pdf = 1.0f / (4.0f * PI);
        return s;
    }
    virtual float directionPdf(const Float3&) const { return 1.0f / (4.0f * PI); }
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
    EnvironmentMap(Texture<Color3>* texture, Color3 multiplier = Color3(2.0f)) : texture(texture), multiplier(multiplier) { buildDistribution(); }

    Color3 sample(const RayHit& ray_hit) override {
        return radiance(ray_hit.d);
    }

    Color3 radiance(const Float3& direction) const override {
        Float3 d = direction.normalize();
        float lon = std::atan2f(d.z(), d.x());
        if (lon < 0.f) lon += 2.f * PI;
        const float lat = std::atan2(std::hypot(d.x(), d.z()), d.y());
        Color3 result;
        if (auto* image = dynamic_cast<ImageTexture<Color3>*>(texture);
            image && image->width > 0 && image->height > 0) {
            // Texel centers match the importance distribution. Longitude
            // repeats; latitude clamps at the poles instead of wrapping.
            const float x = lon / (2.f * PI) * image->width - .5f;
            const float y = lat / PI * image->height - .5f;
            const int ix = int(std::floor(x)), iy = int(std::floor(y));
            const float tx = x-ix, ty = y-iy;
            auto texel = [&](int px, int py) {
                px = (px % image->width + image->width) % image->width;
                py = std::clamp(py, 0, image->height-1);
                float pixel[3]{}; image->buffer.getpixel(px, py, pixel, 3);
                return Color3(pixel[0],pixel[1],pixel[2]);
            };
            result = multiplier * ((1-ty)*((1-tx)*texel(ix,iy)+tx*texel(ix+1,iy)) +
                                   ty*((1-tx)*texel(ix,iy+1)+tx*texel(ix+1,iy+1)));
        } else {
            result = multiplier * texture->sampleInterp(lon / (2.f * PI),
                std::min(lat / PI, std::nextafter(1.0f, 0.0f)), Float3(0.f));
        }
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(result[i]) || result[i] < 0) result[i] = 0;
        return result;
    }

    EnvironmentSample sampleDirection(unsigned int& seed) const override {
        if (probability.empty()) return Environment::sampleDirection(seed);
        const size_t n = probability.size();
        const double x = (double(rnd(seed)) + double(rnd(seed)) / 16777216.0) * n;
        const size_t column = std::min(static_cast<size_t>(x), n - 1);
        const size_t index = (x - std::floor(x) < probability[column]) ? column : alias[column];
        const int px = static_cast<int>(index % width), py = static_cast<int>(index / width);
        const float phi = 2.0f * PI * (static_cast<float>(px) + rnd(seed)) / width;
        constexpr double pi = 3.14159265358979323846;
        const double theta0 = pi * py / height;
        const double theta1 = pi * (py + 1) / height;
        const double cosTheta = std::cos(theta0) + rnd(seed) * (std::cos(theta1) - std::cos(theta0));
        // Keep polar angular detail even when the float y component rounds to
        // +/-1. atan2(xz,y) below can still recover the correct HDR texel row.
        const float sinTheta = float(std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta)));
        EnvironmentSample s;
        s.wi = Float3(sinTheta * std::cos(phi), float(cosTheta), sinTheta * std::sin(phi));
        s.radiance = radiance(s.wi);
        s.pdf = texelProbability[index] / texelSolidAngle(py);
        return s;
    }

    float directionPdf(const Float3& direction) const override {
        if (texelProbability.empty()) return Environment::directionPdf(direction);
        Float3 d = direction.normalize();
        float phi = std::atan2f(d.z(), d.x()); if (phi < 0) phi += 2.f * PI;
        int x = std::min(static_cast<int>(phi * width / (2.f * PI)), width - 1);
        int y = std::min(static_cast<int>(std::atan2(std::hypot(d.x(), d.z()), d.y()) * height / PI), height - 1);
        return texelProbability[static_cast<size_t>(y) * width + x] / texelSolidAngle(y);
    }

    void buildDistribution() {
        texelProbability.clear(); probability.clear(); alias.clear();
        auto* image = dynamic_cast<ImageTexture<Color3>*>(texture);
        if (!image || image->width <= 0 || image->height <= 0) return;
        width = image->width; height = image->height;
        const size_t count = static_cast<size_t>(width) * height;
        std::vector<float> weights(count); double total = 0.0;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            float pixel[4] = {};
            image->buffer.getpixel(x, y, pixel);
            Color3 c = multiplier * Color3(pixel[0], pixel[1], pixel[2]);
            float lum = std::max(0.0f, 0.2126f*c.x() + 0.7152f*c.y() + 0.0722f*c.z());
            if (!std::isfinite(lum)) lum = 0.0f;
            weights[static_cast<size_t>(y)*width+x] = lum * texelSolidAngle(y); total += weights[static_cast<size_t>(y)*width+x];
        }
        if (!(total > 0.0) || !std::isfinite(total)) return;
        texelProbability.resize(count); probability.resize(count); alias.resize(count);
        std::vector<float> scaled(count); std::vector<size_t> under, over;
        // Bilinear filtering can emit radiance outside nonzero source texels.
        // Retain full support, including those neighboring black texels.
        constexpr double uniformMixture = 0.001;
        for (size_t i=0;i<count;++i) {
            texelProbability[i] = float((1-uniformMixture)*weights[i]/total +
                uniformMixture*texelSolidAngle(int(i/width))/(4.0*3.14159265358979323846));
            scaled[i]=texelProbability[i]*count;
            (scaled[i]<1?under:over).push_back(i);
        }
        while (!under.empty() && !over.empty()) { size_t a=under.back();under.pop_back(); size_t b=over.back();over.pop_back(); probability[a]=scaled[a];alias[a]=b; scaled[b]-=1-scaled[a]; (scaled[b]<1?under:over).push_back(b); }
        for(size_t i:over){probability[i]=1;alias[i]=i;} for(size_t i:under){probability[i]=1;alias[i]=i;}
    }

    Texture<Color3>* texture;
    Color3 multiplier;
    float exposure;
    int width = 0, height = 0;
    std::vector<float> texelProbability, probability;
    std::vector<size_t> alias;
private:
    float texelSolidAngle(int y) const {
        constexpr double pi = 3.14159265358979323846;
        return float((4*pi/width)*std::sin(pi*(y+0.5)/height)*std::sin(pi/(2*height)));
    }
};

// Lat-long environment with a row-major world-to-environment linear transform.
// The solid-angle Jacobian keeps PDFs correct even when authored transforms
// contain scale or shear.
class TransformedEnvironmentMap : public EnvironmentMap {
public:
    TransformedEnvironmentMap(Texture<Color3>* texture, Color3 multiplier,
        std::array<float, 9> worldToEnvironment)
        : EnvironmentMap(texture, multiplier), m(worldToEnvironment) { invert(); }

    Color3 radiance(const Float3& world) const override {
        return EnvironmentMap::radiance(apply(m, world).normalize());
    }
    Color3 sample(const RayHit& hit) override { return radiance(hit.d); }
    EnvironmentSample sampleDirection(unsigned int& seed) const override {
        EnvironmentSample s = EnvironmentMap::sampleDirection(seed);
        const Float3 local = s.wi;
        s.wi = apply(inv, local).normalize();
        s.radiance = radiance(s.wi);
        s.pdf *= localPerWorldJacobian(s.wi);
        return s;
    }
    float directionPdf(const Float3& world) const override {
        const Float3 transformed = apply(m, world);
        if (transformed.is_zero()) return 0.0f;
        return EnvironmentMap::directionPdf(transformed.normalize()) * localPerWorldJacobian(world.normalize());
    }
private:
    static Float3 apply(const std::array<float,9>& a, const Float3& v) {
        return {a[0]*v.x()+a[1]*v.y()+a[2]*v.z(), a[3]*v.x()+a[4]*v.y()+a[5]*v.z(), a[6]*v.x()+a[7]*v.y()+a[8]*v.z()};
    }
    float localPerWorldJacobian(const Float3& world) const {
        const float length = apply(m, world).length();
        return length > 0.0f ? std::fabs(det) / (length*length*length) : 0.0f;
    }
    void invert() {
        det=m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6]);
        if (!std::isfinite(det) || std::fabs(det)<1e-12f) {
            std::cerr << "PrismRT: invalid or singular dome transform; using identity\n";
            m={1,0,0,0,1,0,0,0,1}; det=1;
        }
        const float q=1/det;
        inv={ (m[4]*m[8]-m[5]*m[7])*q,(m[2]*m[7]-m[1]*m[8])*q,(m[1]*m[5]-m[2]*m[4])*q,
             (m[5]*m[6]-m[3]*m[8])*q,(m[0]*m[8]-m[2]*m[6])*q,(m[2]*m[3]-m[0]*m[5])*q,
             (m[3]*m[7]-m[4]*m[6])*q,(m[1]*m[6]-m[0]*m[7])*q,(m[0]*m[4]-m[1]*m[3])*q };
    }
    std::array<float,9> m, inv{};
    float det=1.0f;
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

        return 2.f * texture->sampleInterp(u, v, Float3(0.f));
    }

    Texture<Color3>* texture;
};
