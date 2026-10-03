#pragma once

#include "../environment.h"
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <pxr/base/tf/diagnostic.h>

namespace prism::hydra {

// Own both the immutable texture and its local-space importance distribution.
// Transform wrappers share this object without rebuilding or copying its tables.
struct LocalEnvironment {
    // Non-image keys use the source object's identity; retain it so an address
    // cannot be recycled while a cached entry is live.
    std::shared_ptr<const Texture<Color3>> sourceIdentity;
    std::shared_ptr<Texture<Color3>> texture;
    EnvironmentMap map;
    LocalEnvironment(std::shared_ptr<Texture<Color3>> texture, Color3 multiplier)
        : texture(std::move(texture)), map(this->texture.get(), multiplier) {}
    size_t importanceBytes() const {
        return map.texelProbability.capacity()*sizeof(float) +
            map.probability.capacity()*sizeof(float) + map.alias.capacity()*sizeof(size_t);
    }
};

class EnvironmentCache {
public:
    std::shared_ptr<const LocalEnvironment> Get(
        std::shared_ptr<Texture<Color3>> texture, Color3 multiplier) {
        const auto* image = dynamic_cast<ImageTexture<Color3>*>(texture.get());
        const Key key{image && image->resource ? static_cast<const void*>(image->resource.get())
                                             : static_cast<const void*>(texture.get()),
            {std::bit_cast<uint32_t>(multiplier.x()), std::bit_cast<uint32_t>(multiplier.y()),
             std::bit_cast<uint32_t>(multiplier.z())}};
        std::lock_guard lock(mutex);
        auto& entry=entries[key];
        if (auto existing=entry.lock()) { ++hits; return existing; }
        const auto sourceIdentity=texture;
        // Freeze the texture handle as well as the tables: publishing edits on
        // a caller's ImageTexture must not change a previously cached sky.
        if (image) texture=std::make_shared<ImageTexture<Color3>>(*image);
        else if (const auto* color=dynamic_cast<ColorTexture*>(texture.get()))
            texture=std::make_shared<ColorTexture>(*color);
        auto result=std::make_shared<LocalEnvironment>(std::move(texture),multiplier);
        if (!image || !image->resource) result->sourceIdentity=sourceIdentity;
        entry=result;
        ++builds;
        // Expired keys must not accumulate during repeated resource edits.
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.expired()) it=entries.erase(it); else ++it;
        return result;
    }
    uint64_t BuildCount() const { std::lock_guard lock(mutex); return builds; }
    uint64_t HitCount() const { std::lock_guard lock(mutex); return hits; }
    uint64_t LiveImportanceBytes() const {
        std::lock_guard lock(mutex);
        uint64_t bytes=0;
        for(auto const& [key,entry]:entries)
            if(auto local=entry.lock()) bytes+=local->importanceBytes();
        return bytes;
    }
private:
    struct Key {
        const void* resource;
        std::array<uint32_t,3> multiplier;
        bool operator==(Key const&) const = default;
    };
    struct Hash {
        size_t operator()(Key const& key) const {
            size_t value=std::hash<const void*>{}(key.resource);
            for(auto component:key.multiplier)
                value^=std::hash<uint32_t>{}(component)+0x9e3779b9+(value<<6)+(value>>2);
            return value;
        }
    };
    mutable std::mutex mutex;
    std::unordered_map<Key,std::weak_ptr<const LocalEnvironment>,Hash> entries;
    uint64_t builds=0,hits=0;
};

class CachedTransformedEnvironment : public Environment {
public:
    CachedTransformedEnvironment(std::shared_ptr<const LocalEnvironment> local,
        std::array<float, 9> worldToEnvironment)
        : local(std::move(local)), m(worldToEnvironment) { invert(); }

    size_t importanceBytes() const { return local->importanceBytes(); }
    const LocalEnvironment* distributionIdentity() const { return local.get(); }

    Color3 radiance(const Float3& world) const override {
        return this->local->map.radiance(apply(m, world).normalize());
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
        return this->local->map.radiance(local, transformed);
    }
    Color3 sample(const RayHit& hit) override { return radiance(hit.d); }
    EnvironmentSample sampleDirection(unsigned int& seed) const override {
        EnvironmentSample s = this->local->map.sampleLocalDirection(seed);
        const Float3 local = s.wi;
        s.wi = apply(inv, local).normalize();
        s.radiance = radiance(s.wi);
        s.pdf *= localPerWorldJacobian(s.wi);
        return s;
    }
    EnvironmentSample sampleDirection(unsigned int& seed, const prism::DirectionFootprint& footprint) const override {
        EnvironmentSample s = this->local->map.sampleLocalDirection(seed);
        s.wi = apply(inv, s.wi).normalize();
        s.radiance = radiance(s.wi, footprint);
        s.pdf *= localPerWorldJacobian(s.wi);
        return s;
    }
    float directionPdf(const Float3& world) const override {
        const Float3 transformed = apply(m, world);
        if (transformed.is_zero()) return 0.0f;
        return this->local->map.directionPdf(transformed.normalize()) * localPerWorldJacobian(world.normalize());
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
            PXR_NAMESPACE_USING_DIRECTIVE
            TF_WARN("PrismRT: invalid or singular dome transform; using identity");
            m={1,0,0,0,1,0,0,0,1}; det=1;
        }
        const float q=1/det;
        inv={ (m[4]*m[8]-m[5]*m[7])*q,(m[2]*m[7]-m[1]*m[8])*q,(m[1]*m[5]-m[2]*m[4])*q,
             (m[5]*m[6]-m[3]*m[8])*q,(m[0]*m[8]-m[2]*m[6])*q,(m[2]*m[3]-m[0]*m[5])*q,
             (m[3]*m[7]-m[4]*m[6])*q,(m[1]*m[6]-m[0]*m[7])*q,(m[0]*m[4]-m[1]*m[3])*q };
    }
    std::shared_ptr<const LocalEnvironment> local;
    std::array<float,9> m, inv{};
    float det=1.0f;
};

} // namespace prism::hydra
