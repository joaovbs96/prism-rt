#include "../src/camera_rays.h"
#include "../src/scene.h"

#include <OpenImageIO/imagebuf.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

int main()
{
    prism::CameraRayParameters camera;
    camera.inverseProjection = pxr::GfMatrix4d(1.0);
    camera.cameraToWorld = pxr::GfMatrix4d(1.0);
    unsigned seedA=17,seedB=17;
    const auto rays=prism::GenerateCameraRays(camera,31,11,127,53,seedA);
    // Footprint construction must not consume random numbers beyond the main
    // camera sample (and optional single lens sample).
    (void)rnd(seedB); // GenerateCameraRays consumes pixel x/y jitter first.
    (void)rnd(seedB);
    if (seedA!=seedB || !rays.footprint.exact || !(rays.footprint.spread>0.0f) ||
        !std::isfinite(rays.footprint.spread)) {
        std::cerr << "invalid perspective footprint\n"; return 1;
    }
    prism::CameraRayParameters ortho=camera; ortho.orthographic=true;
    unsigned seed=3;
    const auto orthographic=prism::GenerateCameraRays(
        ortho,3,4,64,32,seed);
    if (!(orthographic.footprint.width>0.0f) ||
        orthographic.footprint.spread!=0.0f) {
        std::cerr << "invalid orthographic footprint\n"; return 1;
    }
    // Reconstruct the sampled lens point independently. Neighboring pixels
    // must use this same point, not consume independent lens samples.
    camera.apertureRadius=.15;
    camera.focusDistance=4;
    unsigned actualSeed=291, expectedSeed=actualSeed;
    const float sx=13.f+rnd(expectedSeed), sy=17.f+rnd(expectedSeed);
    const auto lens=random_in_unit_disk(expectedSeed);
    const auto primary=prism::MakeCameraRayWithLens(camera,sx,sy,80,60,&lens);
    const auto neighborX=prism::MakeCameraRayWithLens(camera,sx+1,sy,80,60,&lens);
    const auto neighborY=prism::MakeCameraRayWithLens(camera,sx,sy+1,80,60,&lens);
    const auto thinLens=prism::GenerateCameraRays(camera,13,17,80,60,actualSeed);
    const auto d=[](RTCRay r) { return Float3(r.dir_x,r.dir_y,r.dir_z); };
    const auto o=[](RTCRay r) { return Float3(r.org_x,r.org_y,r.org_z); };
    if (actualSeed!=expectedSeed || !(o(thinLens.sampled)-o(primary)).is_zero() ||
        !(d(thinLens.sampled)-d(primary)).is_zero() ||
        !(thinLens.footprint.dDdx-(d(neighborX)-d(primary))).is_zero() ||
        !(thinLens.footprint.dDdy-(d(neighborY)-d(primary))).is_zero() ||
        !thinLens.footprint.dOdx.is_zero() || !thinLens.footprint.dOdy.is_zero()) {
        std::cerr << "thin lens neighbors consumed a different lens sample\n"; return 1;
    }
    unsigned fineSeed=291;
    camera.apertureRadius=0;
    const auto coarse=prism::GenerateCameraRays(camera,13,17,80,60,fineSeed);
    fineSeed=291;
    const auto fine=prism::GenerateCameraRays(camera,26,34,160,120,fineSeed);
    if (!(fine.footprint.spread < .55f*coarse.footprint.spread &&
          fine.footprint.spread > .45f*coarse.footprint.spread)) {
        std::cerr << "primary footprint does not scale with resolution\n"; return 1;
    }

    // A valid derivative may span more than one repeat of a texture.  Mip
    // selection needs that full derivative; replacing it with a unit-width
    // fallback aliases highly tiled textures instead of integrating them.
    RTCDevice device=rtcNewDevice(nullptr);
    if (!device) {
        std::cerr << "cannot create Embree device\n"; return 1;
    }
    {
        SceneManager scene(device);
        Mesh mesh;
        mesh.type=RTC_GEOMETRY_TYPE_TRIANGLE;
        mesh.vertices={Float3(0,0,0),Float3(1,0,0),Float3(0,1,0)};
        mesh.indices={0,1,2};
        mesh.texcoords={Float2(0,0),Float2(64,0),Float2(0,64)};

        RTCRayHit raw{};
        raw.hit.geomID=RTC_INVALID_GEOMETRY_ID;
        RayHit hit(raw);
        hit.p=Float3(.25f,.25f,0);
        hit.o=Float3(.25f,.25f,1);
        hit.d=Float3(0,0,-1);
        hit.n_g=Float3(0,0,1);
        hit.t=1;
        hit.primid=0;
        hit.dpdx=Float3(.1f,0,0);
        hit.dpdy=Float3(0,.1f,0);
        hit.ray_footprint.width=.1f;
        scene.prepareFootprint(hit,&mesh,nullptr);
        if (!hit.texture_footprint.valid || hit.texture_footprint.fallback ||
            std::abs(hit.texture_footprint.dudx-6.4f)>1.0e-4f ||
            std::abs(hit.texture_footprint.dvdy-6.4f)>1.0e-4f) {
            std::cerr << "multi-repeat UV footprint was clipped\n";
            return 1;
        }

        constexpr int checkerSize=256;
        OIIO::ImageSpec checkerSpec(
            checkerSize,checkerSize,1,OIIO::TypeDesc::FLOAT);
        OIIO::ImageBuf checker(checkerSpec);
        std::vector<float> pixels(checkerSize*checkerSize);
        for (int y=0;y<checkerSize;++y)
            for (int x=0;x<checkerSize;++x)
                pixels[y*checkerSize+x]=((x+y)&1) ? 1.0f : 0.0f;
        if (!checker.set_pixels(
                OIIO::ROI(0,checkerSize,0,checkerSize,0,1,0,1),
                OIIO::TypeDesc::FLOAT,pixels.data())) {
            std::cerr << "cannot create checker reference\n";
            return 1;
        }
        const auto cache=std::filesystem::temp_directory_path()/
            "prism-ray-footprint-cache";
#ifdef _WIN32
        _putenv_s("PRISM_CACHE_DIR",cache.string().c_str());
#else
        setenv("PRISM_CACHE_DIR",cache.string().c_str(),1);
#endif
        auto service=prism::TextureService::Acquire();
        auto checkerResource=service->prepare(checker);
        prism::TextureLookup lookup;
        lookup.s=10.5f/checkerSize;
        lookup.t=10.5f/checkerSize;
        lookup.dsdx=hit.texture_footprint.dudx;
        lookup.dtdx=hit.texture_footprint.dvdx;
        lookup.dsdy=hit.texture_footprint.dudy;
        lookup.dtdy=hit.texture_footprint.dvdy;
        lookup.wrapS=prism::TextureWrap::Periodic;
        lookup.wrapT=prism::TextureWrap::Periodic;
        float filtered=0;
        if (!service->lookup(*checkerResource,lookup,1,&filtered) ||
            std::abs(filtered-.5f)>.03f) {
            std::cerr << "filtered checker differs from supersampled reference\n";
            return 1;
        }
        lookup.filtering=prism::TextureFiltering::Bilinear;
        float pointSample=0;
        if (!service->lookup(*checkerResource,lookup,1,&pointSample) ||
            std::abs(pointSample-.5f)<.2f) {
            std::cerr << "checker fixture does not discriminate minification\n";
            return 1;
        }
        rtcReleaseScene(scene.scene);
        scene.scene=nullptr;
    }
    rtcReleaseDevice(device);
    return 0;
}
