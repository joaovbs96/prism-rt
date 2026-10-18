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
    virtual Color3 radiance(const Float3& direction, const prism::DirectionFootprint&) const {
        return radiance(direction);
    }
    virtual EnvironmentSample sampleDirection(unsigned int& seed,
                                               const prism::DirectionFootprint& footprint) const {
        auto s = sampleDirection(seed);
        s.radiance = radiance(s.wi, footprint);
        return s;
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

    // sampleDirection(seed, footprint) for the sample point u in [0,1)^4
    // instead of the generator; uses as many coordinates as the generator
    // version draws numbers, in the same order.
    virtual EnvironmentSample sampleDirectionUniform(const float u[4],
                                                     const prism::DirectionFootprint& footprint) const {
        const float y = 1.0f - 2.0f * u[0];
        const float phi = 2.0f * PI * u[1];
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        EnvironmentSample s;
        s.wi = Float3(r * std::cos(phi), y, r * std::sin(phi));
        s.radiance = radiance(s.wi, footprint);
        s.pdf = 1.0f / (4.0f * PI);
        return s;
    }

    // The integral of the radiance's luminance over the sphere (light
    // selection). Estimated from a fixed set of directions by default.
    virtual float integratedLuminance() const {
        constexpr int count = 256;
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            // Fibonacci sphere.
            const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
            const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float phi = float(i) * 2.39996322972865332f;
            const Color3 c = radiance(Float3(r * std::cos(phi), y, r * std::sin(phi)));
            const float lum = 0.2126f * c.x() + 0.7152f * c.y() + 0.0722f * c.z();
            if (std::isfinite(lum) && lum > 0.0f) sum += lum;
        }
        return float(sum / count * 4.0 * 3.14159265358979323846);
    }
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
        return EnvironmentMap::radiance(direction, prism::DirectionFootprint{});
    }
    Color3 radiance(const Float3& direction, const prism::DirectionFootprint& footprint) const override {
        Float3 d = direction.normalize();
        float lon = std::atan2f(d.z(), d.x());
        if (lon < 0.f) lon += 2.f * PI;
        const float lat = std::atan2(std::hypot(d.x(), d.z()), d.y());
        Color3 result;
        if (auto* image = dynamic_cast<ImageTexture<Color3>*>(texture);
            image && image->width > 0 && image->height > 0) {
            prism::TextureLookup lookup{};
            lookup.s = lon / (2.f * PI); lookup.t = lat / PI;
            lookup.wrapS = prism::TextureWrap::Periodic;
            lookup.wrapT = prism::TextureWrap::Clamp;
            lookup.filtering = footprint.filtering;
            const float angle = footprint.angularRadius;
            if (angle > 0 && std::isfinite(angle)) {
                Float3 tangent = cross(std::fabs(d.y()) < .99f ? Float3(0,1,0) : Float3(1,0,0), d).normalize();
                Float3 bitangent = cross(d, tangent);
                const auto derivatives = [&](const Float3& offset, float& ds, float& dt) {
                    const Float3 q = (d + angle * offset).normalize();
                    float qlon = std::atan2(q.z(),q.x());
                    if (qlon < 0) qlon += 2.f * PI;
                    ds = qlon/(2.f*PI) - lookup.s;
                    ds -= std::round(ds);
                    dt = std::atan2(std::hypot(q.x(),q.z()),q.y())/PI - lookup.t;
                };
                derivatives(tangent, lookup.dsdx, lookup.dtdx);
                derivatives(bitangent, lookup.dsdy, lookup.dtdy);
                if (std::hypot(d.x(),d.z()) < angle) {
                    lookup.dsdx = .5f;
                    lookup.dtdx = 0;
                    lookup.dsdy = 0;
                    lookup.dtdy = angle / PI;
                }
            }
            result = multiplier * image->lookup(lookup);
        } else {
            result = multiplier * texture->sampleInterp(lon / (2.f * PI),
                std::min(lat / PI, std::nextafter(1.0f, 0.0f)), Float3(0.f));
        }
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(result[i]) || result[i] < 0) result[i] = 0;
        return result;
    }

    EnvironmentSample sampleDirection(unsigned int& seed) const override {
        auto s = sampleLocalDirection(seed);
        s.radiance = radiance(s.wi);
        return s;
    }
    EnvironmentSample sampleDirection(unsigned int& seed, const prism::DirectionFootprint& footprint) const override {
        auto s = sampleLocalDirection(seed);
        s.radiance = radiance(s.wi, footprint);
        return s;
    }
    EnvironmentSample sampleDirectionUniform(const float u[4],
                                             const prism::DirectionFootprint& footprint) const override {
        auto s = sampleLocalDirection(u);
        s.radiance = radiance(s.wi, footprint);
        return s;
    }

    // Sampling remains independent of texture filtering and its memory cache.
    EnvironmentSample sampleLocalDirection(unsigned int& seed) const {
        float u[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const int count = probability.empty() ? 2 : 4;
        for (int i = 0; i < count; ++i) u[i] = rnd(seed);
        return sampleLocalDirection(u);
    }

    EnvironmentSample sampleLocalDirection(const float u[4]) const {
        if (probability.empty()) {
            const float y = 1.0f - 2.0f * u[0];
            const float phi = 2.0f * PI * u[1];
            const float r = std::sqrt(std::max(0.0f, 1.0f-y*y));
            return {Float3(r*std::cos(phi),y,r*std::sin(phi)), Color3(0), 1.0f/(4.0f*PI)};
        }
        const size_t n = probability.size();
        const double x = (double(u[0]) + double(u[1]) / 16777216.0) * n;
        const size_t column = std::min(static_cast<size_t>(x), n - 1);
        const size_t index = (x - std::floor(x) < probability[column]) ? column : alias[column];
        const int px = static_cast<int>(index % width), py = static_cast<int>(index / width);
        const float phi = 2.0f * PI * (static_cast<float>(px) + u[2]) / width;
        constexpr double pi = 3.14159265358979323846;
        const double theta0 = pi * py / height;
        const double theta1 = pi * (py + 1) / height;
        const double cosTheta = std::cos(theta0) + u[3] * (std::cos(theta1) - std::cos(theta0));
        // Keep polar angular detail even when the float y component rounds to
        // +/-1. atan2(xz,y) below can still recover the correct HDR texel row.
        const float sinTheta = float(std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta)));
        EnvironmentSample s;
        s.wi = Float3(sinTheta * std::cos(phi), float(cosTheta), sinTheta * std::sin(phi));
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
        std::vector<float> row(static_cast<size_t>(width) * 3);
        for (int y = 0; y < height; ++y) {
            if (!image->readRow(y, row.data(), 3))
                throw std::runtime_error("Cannot read environment sampling distribution");
            for (int x = 0; x < width; ++x) {
                const float* pixel = row.data() + 3*x;
                Color3 c = multiplier * Color3(pixel[0], pixel[1], pixel[2]);
                float lum = std::max(0.0f, 0.2126f*c.x() + 0.7152f*c.y() + 0.0722f*c.z());
                if (!std::isfinite(lum)) lum = 0.0f;
                weights[static_cast<size_t>(y)*width+x] = lum * texelSolidAngle(y);
                total += weights[static_cast<size_t>(y)*width+x];
            }
        }
        if (!(total > 0.0) || !std::isfinite(total)) return;
        luminanceIntegral = float(total);
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

    float integratedLuminance() const override {
        return luminanceIntegral > 0.0f ? luminanceIntegral : Environment::integratedLuminance();
    }

    Texture<Color3>* texture;
    Color3 multiplier;
    float exposure;
    float luminanceIntegral = 0.0f;
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

    // Read-only views for render devices that mirror the environment.
    const std::array<float, 9>& worldToEnvironment() const { return m; }
    const std::array<float, 9>& environmentToWorld() const { return inv; }
    float transformDeterminant() const { return det; }

    Color3 radiance(const Float3& world) const override {
        return EnvironmentMap::radiance(apply(m, world).normalize());
    }
    Color3 radiance(const Float3& world, const prism::DirectionFootprint& footprint) const override {
        const Float3 local = apply(m, world).normalize();
        auto transformed = footprint;
        if (footprint.angularRadius > 0) {
            const Float3 d = world.normalize();
            const Float3 t = cross(std::fabs(d.y()) < .99f ? Float3(0,1,0) : Float3(1,0,0), d).normalize();
            const Float3 b = cross(d,t);
            transformed.angularRadius = std::max(
                (apply(m,d+footprint.angularRadius*t).normalize()-local).length(),
                (apply(m,d+footprint.angularRadius*b).normalize()-local).length());
        }
        return EnvironmentMap::radiance(local, transformed);
    }
    Color3 sample(const RayHit& hit) override { return radiance(hit.d); }
    EnvironmentSample sampleDirection(unsigned int& seed) const override {
        EnvironmentSample s = sampleLocalDirection(seed);
        const Float3 local = s.wi;
        s.wi = apply(inv, local).normalize();
        s.radiance = radiance(s.wi);
        s.pdf *= localPerWorldJacobian(s.wi);
        return s;
    }
    EnvironmentSample sampleDirection(unsigned int& seed, const prism::DirectionFootprint& footprint) const override {
        EnvironmentSample s = sampleLocalDirection(seed);
        s.wi = apply(inv, s.wi).normalize();
        s.radiance = radiance(s.wi, footprint);
        s.pdf *= localPerWorldJacobian(s.wi);
        return s;
    }
    EnvironmentSample sampleDirectionUniform(const float u[4],
                                             const prism::DirectionFootprint& footprint) const override {
        EnvironmentSample s = sampleLocalDirection(u);
        s.wi = apply(inv, s.wi).normalize();
        s.radiance = radiance(s.wi, footprint);
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

// Several environments lit at once (multiple dome lights): radiance is their
// sum, and sampling picks one of them by integrated luminance, so the
// density is the mixture of theirs.
class CompositeEnvironment : public Environment {
public:
    explicit CompositeEnvironment(std::vector<const Environment*> environments)
        : parts(std::move(environments)) {
        double total = 0.0;
        std::vector<double> power(parts.size());
        for (size_t i = 0; i < parts.size(); ++i) {
            const float p = parts[i]->integratedLuminance();
            power[i] = std::isfinite(p) && p > 0.0f ? p : 0.0;
            total += power[i];
        }
        weights.resize(parts.size());
        for (size_t i = 0; i < parts.size(); ++i)
            weights[i] = total > 0.0 ? float(power[i] / total) : 1.0f / float(parts.size());
    }

    Color3 sample(const RayHit& hit) override { return radiance(hit.d); }
    Color3 radiance(const Float3& direction) const override {
        Color3 sum(0.0f);
        for (auto const* part : parts) sum += part->radiance(direction);
        return sum;
    }
    Color3 radiance(const Float3& direction, const prism::DirectionFootprint& footprint) const override {
        Color3 sum(0.0f);
        for (auto const* part : parts) sum += part->radiance(direction, footprint);
        return sum;
    }
    EnvironmentSample sampleDirection(unsigned int& seed, const prism::DirectionFootprint& footprint) const override {
        const float u[4] = {rnd(seed), rnd(seed), rnd(seed), rnd(seed)};
        return sampleDirectionUniform(u, footprint);
    }
    EnvironmentSample sampleDirection(unsigned int& seed) const override {
        return sampleDirection(seed, prism::DirectionFootprint{});
    }
    // u[0] picks the environment and is rescaled for its own sampling.
    EnvironmentSample sampleDirectionUniform(const float u[4],
                                             const prism::DirectionFootprint& footprint) const override {
        size_t chosen = parts.size() - 1;
        float start = 0.0f;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (u[0] < start + weights[i] || i + 1 == parts.size()) { chosen = i; break; }
            start += weights[i];
        }
        float rescaled[4] = {u[0], u[1], u[2], u[3]};
        if (weights[chosen] > 0.0f)
            rescaled[0] = std::clamp((u[0] - start) / weights[chosen], 0.0f, std::nextafter(1.0f, 0.0f));
        EnvironmentSample s = parts[chosen]->sampleDirectionUniform(rescaled, footprint);
        s.radiance = radiance(s.wi, footprint);
        s.pdf = directionPdf(s.wi);
        return s;
    }
    float directionPdf(const Float3& direction) const override {
        float pdf = 0.0f;
        for (size_t i = 0; i < parts.size(); ++i)
            if (weights[i] > 0.0f) pdf += weights[i] * parts[i]->directionPdf(direction);
        return pdf;
    }
    float integratedLuminance() const override {
        float sum = 0.0f;
        for (auto const* part : parts) sum += part->integratedLuminance();
        return sum;
    }

    std::vector<const Environment*> parts;
    std::vector<float> weights; // selection probabilities
};
