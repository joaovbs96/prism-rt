#include "../src/scene.h"
#include <array>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Fixture {
    RTCDevice device = rtcNewDevice(nullptr);
    SceneManager scene{device};
    Fixture() { scene.max_depth=8; scene.rr_min_depth=32; }
    ~Fixture() {
        rtcReleaseScene(scene.scene);
        for (auto const& [id, shape] : scene.geometry) rtcReleaseGeometry(shape->geom);
        rtcReleaseDevice(device);
    }
};
double estimate(SceneManager& scene, bool mis, unsigned seed, int samples) {
    scene.environmentSampling=mis;
    double sum=0;
    for (int i=0;i<samples;++i) {
        const auto value=scene.trace(initialize_ray(Float3(0,1,0),Float3(0,-1,0)),0,Color3(1),seed);
        require(std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()), "Nonfinite radiance");
        sum += value.x();
    }
    return sum/samples;
}
class Hole : public Diffuse {
public:
    Hole(ColorTexture* color) : Diffuse(color) {}
    float cutoutOpacity(const RayHit&) override { return 0; }
    BRDFSample sample(const RayHit hit, unsigned&) override {
        BRDFSample s; s.wi=hit.d; s.color=Color3(1); s.emission=Color3(0);
        s.scattered=true; s.delta=true; s.null_event=true; return s;
    }
};
}
int main() {
    try {
        constexpr int width=64,height=32;
        std::vector<unsigned char> data(width*height*3,0);
        ImageTexture<Color3> texture(&data,width,height,3);
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            float v=(x==12 && y==7) ? 1000.f : .02f;
            const float pixel[3]={v,v,v}; texture.buffer.setpixel(x,y,pixel);
        }
        EnvironmentMap sky(&texture,Color3(1));
        ColorTexture gray(Color3(.5f)); Diffuse diffuse(&gray);
        Fixture fixture;
        fixture.scene.environment=&sky;
        fixture.scene.add_quad(Float3(-100,0,-100),Float3(0,0,200),Float3(200,0,0),&diffuse);
        fixture.scene.commitScene();
        // Independent midpoint quadrature of incident irradiance, with no
        // reference to the importance distribution or renderer BSDF sampler.
        double reference=0;
        constexpr int nPhi=2048,nCos=512;
        for(int y=0;y<nCos;++y) {
            const float cosine=(y+.5f)/nCos, sine=std::sqrt(1-cosine*cosine);
            for(int x=0;x<nPhi;++x) {
                const float phi=2*PI*(x+.5f)/nPhi;
                reference += .5*sky.radiance(Float3(sine*std::cos(phi),cosine,sine*std::sin(phi))).x()*cosine/PI;
            }
        }
        reference *= 2*PI/(double(nPhi)*nCos);
        std::array<double,8> mis{},bsdf{};
        double misError=0,bsdfError=0;
        for(unsigned run=0;run<mis.size();++run) {
            const unsigned seed=tea<64>(run,123);
            mis[run]=estimate(fixture.scene,true,seed,8192);
            bsdf[run]=estimate(fixture.scene,false,seed,8192);
            misError+=(mis[run]-reference)*(mis[run]-reference);
            bsdfError+=(bsdf[run]-reference)*(bsdf[run]-reference);
        }
        const double mean=std::accumulate(mis.begin(),mis.end(),0.)/mis.size();
        double variance=0; for(double x:mis) variance+=(x-mean)*(x-mean);
        const double standardError=std::sqrt(variance/(mis.size()*(mis.size()-1)));
        require(std::abs(mean-reference)<3*standardError+.01*reference, "MIS loses or adds energy");
        require(misError<.5*bsdfError, "HDR importance sampling failed to reduce equal-spp error");
        const double deterministic=estimate(fixture.scene,true,111,2048);
        require(deterministic==estimate(fixture.scene,true,111,2048), "MIS rerun not deterministic");

        ColorTexture white(Color3(1)); ConstantColorBackground constantSky(&white);
        fixture.scene.environment=&constantSky;
        Hole hole(&white);
        fixture.scene.add_quad(Float3(-100,.5f,-100),Float3(0,0,200),Float3(200,0,0),&hole);
        fixture.scene.commitScene();
        require(std::abs(estimate(fixture.scene,true,127,65536)-.5)<.005,
                "Transparent coverage changes diffuse illumination / null MIS state");
        fixture.scene.max_depth=1;
        require(estimate(fixture.scene,true,1,32)==0 && estimate(fixture.scene,false,1,32)==0,
                "MIS depth boundary differs from truncated BSDF transport");

        Fixture glassFixture; Glass glass(1.5f);
        glassFixture.scene.environment=&constantSky;
        glassFixture.scene.add_sphere(Float3(0,-1,0),.5f,&glass);
        glassFixture.scene.max_depth=32; glassFixture.scene.commitScene();
        require(std::abs(estimate(glassFixture.scene,true,44,8192)-1)<.001,
                "Delta glass path incorrectly MIS weighted");
        std::cout << "HDR reference=" << reference << " MIS mean=" << mean
                  << " standardError=" << standardError << " MSE ratio=" << misError/bsdfError << '\n';
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
