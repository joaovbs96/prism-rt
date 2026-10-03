#include "../../src/scene.h"
#include <iostream>
#include <stdexcept>

namespace {
using Outcome = SceneManager::PrimaryOutcome;
void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
struct Fixture {
    RTCDevice device = rtcNewDevice(nullptr);
    SceneManager scene{device};
    ColorTexture white{Color3(1)};
    ConstantColorBackground sky{&white};
    Fixture() { scene.environment=&sky; scene.max_depth=8; scene.rr_min_depth=32; }
    ~Fixture() {
        rtcReleaseScene(scene.scene);
        for (auto const& [id, shape] : scene.geometry) rtcReleaseGeometry(shape->geom);
        rtcReleaseDevice(device);
    }
    void plane(float height, Material* material) {
        scene.add_quad(Float3(-2,height,-2),Float3(0,0,4),Float3(4,0,0),material);
    }
};
class Cutout : public Diffuse {
public:
    float opacity;
    bool scatter=true;
    Cutout(ColorTexture* color,float value) : Diffuse(color), opacity(value) {}
    BRDFSample sample(const RayHit hit,unsigned& seed) override {
        const bool hole = rnd(seed) >= opacity;
        BRDFSample s;
        s.wi=hit.d; s.color=Color3(1); s.emission=Color3(0);
        s.scattered=scatter; s.null_event=hole; s.delta=true;
        return s;
    }
};
Outcome check(SceneManager& scene,unsigned initial) {
    const RTCRay ray=initialize_ray(Float3(0,1,0),Float3(0,-1,0));
    unsigned oldSeed=initial,reportedSeed=initial;
    const Color3 old=scene.trace(ray,0,Color3(1),oldSeed);
    Outcome outcome=Outcome::Unresolved;
    const Color3 reported=scene.trace(ray,0,Color3(1),reportedSeed,&outcome);
    require(oldSeed==reportedSeed,"Coverage reporter changes RNG sequence");
    require(old.x()==reported.x() && old.y()==reported.y() && old.z()==reported.z(),
            "Coverage reporter changes radiance");
    return outcome;
}
}
int main() {
    try {
        for (bool mis : {false,true}) {
            Fixture empty; empty.scene.environmentSampling=mis; empty.scene.commitScene();
            require(check(empty.scene,1)==Outcome::Escaped,"Primary miss not escaped");
            empty.scene.max_depth=0;
            require(check(empty.scene,1)==Outcome::Unresolved,"Depth termination became a miss");

            Fixture stochastic; stochastic.scene.environmentSampling=mis;
            Cutout cutout(&stochastic.white,.5f); stochastic.plane(0,&cutout);
            stochastic.scene.commitScene();
            int accepted=0,escaped=0;
            for (unsigned seed=1;seed<=4096;++seed) {
                const Outcome result=check(stochastic.scene,tea<16>(seed,73));
                accepted+=result==Outcome::AcceptedSurface;
                escaped+=result==Outcome::Escaped;
            }
            require(accepted>1700 && accepted<2400 && accepted+escaped==4096,
                    "Stochastic cutout coverage does not follow accepted samples");
            cutout.opacity=0; stochastic.scene.max_depth=1;
            require(check(stochastic.scene,17)==(mis?Outcome::Escaped:Outcome::Unresolved),
                    "Null crossing changed depth semantics");
            stochastic.scene.max_depth=8; cutout.scatter=false;
            require(check(stochastic.scene,17)==Outcome::Unresolved,
                    "Non-scattering null termination became a miss");
            cutout.opacity=1;
            require(check(stochastic.scene,17)==Outcome::AcceptedSurface,
                    "Non-scattering opaque surface lost coverage");

            Fixture capped; capped.scene.environmentSampling=mis;
            Cutout manyHoles(&capped.white,0); capped.scene.max_depth=100;
            capped.scene.rr_min_depth=200;
            for(int layer=0;layer<66;++layer) capped.plane(.9f-.01f*layer,&manyHoles);
            capped.scene.commitScene();
            require(check(capped.scene,19)==(mis?Outcome::Unresolved:Outcome::Escaped),
                    "Null crossing cap became a primary miss");

            Fixture glassFixture; glassFixture.scene.environmentSampling=mis;
            Glass glass(1.5f); glassFixture.plane(0,&glass); glassFixture.scene.commitScene();
            require(check(glassFixture.scene,44)==Outcome::AcceptedSurface,
                    "Glass transmission lost accepted surface coverage");

            Fixture chain; chain.scene.environmentSampling=mis;
            Cutout hole(&chain.white,0); Diffuse opaque(&chain.white);
            chain.plane(.5f,&hole); chain.plane(0,&opaque); chain.scene.commitScene();
            require(check(chain.scene,33)==Outcome::AcceptedSurface,
                    "Accepted surface behind cutout was lost");
            chain.scene.rr_min_depth=0;
            bool unresolved=false;
            for(unsigned seed=1;seed<128;++seed)
                unresolved |= check(chain.scene,seed)==Outcome::Unresolved;
            require(mis || unresolved,"Null roulette termination became an escape");
        }
        std::cout << "Primary coverage preserves radiance and RNG in legacy and MIS paths\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
