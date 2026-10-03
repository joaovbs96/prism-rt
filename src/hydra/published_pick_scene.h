#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace prism::hydra {

struct PublishedPickHit {
    int primId = -1;
    int instanceId = -1;
    int elementId = -1;
    pxr::GfVec3d worldPoint;
    pxr::GfVec3f worldNormal;
    float normalizedDepth = 1.0f;
};

// A published frame owns this immutable scene and its camera. Coordinates use
// the renderer's top-left raster convention and full published resolution.
class PublishedPickScene {
public:
    virtual ~PublishedPickScene() = default;
    virtual bool PickPixel(unsigned int x, unsigned int y,
                           PublishedPickHit* hit) const = 0;
};

struct PickRasterBounds {
    int xBegin = 0, xEnd = 0, yBegin = 0, yEnd = 0;
};

// Restrict ray queries to the pick frustum in the published camera's raster.
// A frustum crossing the camera plane conservatively uses the whole image.
// These bounds use the render buffer's bottom-left convention.
inline PickRasterBounds GetPickRasterBounds(
    pxr::GfMatrix4d const& publishedToClip,
    pxr::GfMatrix4d const& pickToWorld, int width, int height) {
    const PickRasterBounds full{0,width,0,height};
    if (width <= 0 || height <= 0) return {};
    double minX=std::numeric_limits<double>::infinity(),minY=minX;
    double maxX=-minX,maxY=-minX;
    for(int z : {-1,1}) for(int y : {-1,1}) for(int x : {-1,1}) {
        auto world=pxr::GfVec4d(x,y,z,1)*pickToWorld;
        if (!std::isfinite(world[3]) || std::abs(world[3])<1e-12) return full;
        world/=world[3];
        auto clip=world*publishedToClip;
        if (!std::isfinite(clip[3]) || clip[3]<=1e-12) return full;
        const double nx=clip[0]/clip[3],ny=clip[1]/clip[3];
        if (!std::isfinite(nx) || !std::isfinite(ny)) return full;
        minX=std::min(minX,nx); maxX=std::max(maxX,nx);
        minY=std::min(minY,ny); maxY=std::max(maxY,ny);
    }
    // Include boundary centers conservatively; the exact hit/frustum test in
    // the task decides whether each resulting sample is actually inside.
    const auto lower=[](double ndc,int size) {
        return static_cast<int>(std::floor(std::clamp((ndc+1)*.5*size-.5,0.0,double(size))));
    };
    const auto upper=[](double ndc,int size) {
        return static_cast<int>(std::ceil(std::clamp((ndc+1)*.5*size+.5,0.0,double(size))));
    };
    return {lower(minX,width),upper(maxX,width),lower(minY,height),upper(maxY,height)};
}

} // namespace prism::hydra
