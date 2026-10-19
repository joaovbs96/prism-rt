#include "../../src/hydra/environment_cache.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value,const char* reason) {
    if(!value) throw std::runtime_error(reason);
}
bool exact(Float3 a,Float3 b) {
    for(int i=0;i<3;++i)
        if(std::bit_cast<uint32_t>(a[i])!=std::bit_cast<uint32_t>(b[i])) return false;
    return true;
}
void compare(Environment const& cached,Environment const& reference) {
    for(auto filtering:{prism::TextureFiltering::Bilinear,prism::TextureFiltering::Filtered}) {
        const prism::DirectionFootprint footprint{.07f,filtering};
        for(unsigned i=0;i<4096;++i) {
            unsigned a=tea<16>(i,17),b=a;
            const auto sample=cached.sampleDirection(a,footprint);
            const auto expected=reference.sampleDirection(b,footprint);
            require(a==b && exact(sample.wi,expected.wi) && exact(sample.radiance,expected.radiance)
                    && std::bit_cast<uint32_t>(sample.pdf)==std::bit_cast<uint32_t>(expected.pdf),
                    "Cached transform changes filtered sample or RNG bits");
            require(exact(cached.radiance(sample.wi),reference.radiance(sample.wi)) &&
                    cached.directionPdf(sample.wi)==reference.directionPdf(sample.wi),
                    "Cached transform changes radiance or PDF");
            a=tea<16>(i,91); b=a;
            const auto plain=cached.sampleDirection(a),old=reference.sampleDirection(b);
            require(a==b && exact(plain.wi,old.wi) && exact(plain.radiance,old.radiance)
                    && plain.pdf==old.pdf,"Cached transform changes unfiltered sample");
        }
    }
}
}
int main() {
    try {
        std::vector<unsigned char> pixels(16*8*3);
        for(size_t i=0;i<pixels.size();++i) pixels[i]=static_cast<unsigned char>((i*17)%256);
        auto texture=std::make_shared<ImageTexture<Color3>>(&pixels,16,8,3);
        auto sameResource=std::make_shared<ImageTexture<Color3>>(*texture);
        prism::hydra::EnvironmentCache cache;
        const Color3 multiplier(.7f,1.2f,2.3f);
        auto local=cache.Get(texture,multiplier);
        require(cache.Get(sameResource,multiplier)==local,"Prepared resource identity not reused");
        const std::array<std::array<float,9>,4> transforms{{
            {1,0,0,0,1,0,0,0,1},
            {0,0,1,0,1,0,-1,0,0},
            {2,0,0,0,.5f,0,0,0,3},
            {1,.2f,0,0,1,.3f,.1f,0,1}}};
        for(auto const& transform:transforms) {
            prism::hydra::CachedTransformedEnvironment cached(cache.Get(texture,multiplier),transform);
            TransformedEnvironmentMap reference(texture.get(),multiplier,transform);
            compare(cached,reference);
            require(cached.importanceBytes()==local->importanceBytes() && cached.importanceBytes()>0,
                    "Shared distribution memory accounting omitted");
        }
        require(cache.BuildCount()==1 && cache.HitCount()==5,"Rotation or bank reuse rebuilt distribution");
        require(cache.LiveImportanceBytes()==local->importanceBytes(),
                "Shared distribution counted once per wrapper instead of once per resource");
        const std::array<float,9> singular{0,0,0,0,0,0,0,0,0};
        prism::hydra::CachedTransformedEnvironment singularCached(local,singular);
        TransformedEnvironmentMap singularReference(texture.get(),multiplier,singular);
        compare(singularCached,singularReference);
        auto changed=cache.Get(texture,Color3(.7f,1.2f,2.300001f));
        require(changed!=local && cache.BuildCount()==2,"Multiplier bits did not invalidate distribution");
        const float edit[3]={100,200,300}; sameResource->buffer.setpixel(0,0,edit);
        sameResource->publishEdits();
        auto reloaded=cache.Get(sameResource,multiplier);
        require(reloaded!=local && cache.BuildCount()==3,"Prepared resource edit reused old distribution");
        prism::hydra::CachedTransformedEnvironment retained(local,transforms[0]);
        ImageTexture<Color3> frozen(*texture);
        texture->buffer.setpixel(0,0,edit); texture->publishEdits();
        TransformedEnvironmentMap unchanged(&frozen,multiplier,transforms[0]);
        compare(retained,unchanged);
        auto constant=std::make_shared<ColorTexture>(Color3(.4f));
        auto constantLocal=cache.Get(constant,multiplier);
        prism::hydra::CachedTransformedEnvironment fallback(constantLocal,transforms[3]);
        TransformedEnvironmentMap originalFallback(constant.get(),multiplier,transforms[3]);
        compare(fallback,originalFallback);
        require(fallback.importanceBytes()==0,"Constant sky accounted nonexistent sampling tables");
        auto unpublished=std::make_shared<ImageTexture<Color3>>(frozen);
        unpublished->resource.reset();
        std::weak_ptr<ImageTexture<Color3>> sourceIdentity=unpublished;
        auto unpublishedLocal=cache.Get(unpublished,multiplier);
        unpublished.reset();
        require(!sourceIdentity.expired(),"Unprepared image key identity can be recycled");
        {
            // Hydra statistics read the cache on the host's UI thread while a
            // render job builds a sky's tables; the reads must not wait for it.
            std::vector<unsigned char> sky(size_t(4096)*2048*3);
            for(size_t i=0;i<sky.size();++i) sky[i]=static_cast<unsigned char>((i*131)%251);
            auto large=std::make_shared<ImageTexture<Color3>>(&sky,4096,2048,3);
            prism::hydra::EnvironmentCache busy;
            std::atomic<bool> done{false};
            double buildMs=0;
            std::thread builder([&] {
                const auto start=std::chrono::steady_clock::now();
                (void)busy.Get(large,multiplier);
                buildMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                done=true;
            });
            double worstMs=0;
            while(!done) {
                const auto start=std::chrono::steady_clock::now();
                (void)busy.BuildCount(); (void)busy.LiveImportanceBytes();
                worstMs=std::max(worstMs,std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-start).count());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            builder.join();
            std::cout << "Large sky build " << buildMs << " ms; slowest concurrent statistics read "
                      << worstMs << " ms\n";
            require(busy.BuildCount()==1,"Large sky not cached");
            require(buildMs<100 || worstMs<buildMs/4,"Statistics reads waited for a distribution build");
        }
        std::cout << "One distribution build for five resource/transform reuses; exact RGB/PDF/RNG parity\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
