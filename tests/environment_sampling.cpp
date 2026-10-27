#include "../src/environment.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

ImageTexture<Color3> image(std::vector<unsigned char>& pixels, int w, int h) {
    return ImageTexture<Color3>(&pixels, w, h, 3);
}
}

int main() {
    try {
        // A constant lat-long map must produce a uniform solid-angle density,
        // despite polar rows covering less of the sphere.
        std::vector<unsigned char> white(8 * 4 * 3, 255);
        auto constantTexture = image(white, 8, 4);
        EnvironmentMap constant(&constantTexture, Color3(1.0f));
        unsigned seed = 17;
        double inversePdfMean = 0.0;
        for (int i=0; i<200000; ++i) {
            EnvironmentSample s = constant.sampleDirection(seed);
            require(s.pdf > 0.0f && std::isfinite(s.pdf), "sample PDF must be finite");
            require(std::fabs(s.pdf - 1.0f/(4.0f*PI)) < 2e-5f,
                    "constant environment must be uniform on the sphere");
            inversePdfMean += 1.0 / s.pdf;
        }
        inversePdfMean /= 200000.0;
        require(std::fabs(inversePdfMean - 4.0*PI) < 0.01,
                "environment PDF must integrate to one");

        // A bright texel should receive its luminance-weighted solid-angle
        // probability, and sampleDirection/directionPdf must agree.
        std::vector<unsigned char> pixels(8 * 4 * 3, 1);
        const int bright = 2 * 8 + 3;
        pixels[bright*3]=pixels[bright*3+1]=pixels[bright*3+2]=255;
        auto brightTexture = image(pixels, 8, 4);
        EnvironmentMap brightMap(&brightTexture, Color3(1.0f));
        for (float p : brightMap.texelProbability)
            require(p > 0, "Filtered HDR sampling must retain full support");
        int observed=0; const int samples=250000;
        Float3 brightDirection(0.0f);
        for (int i=0;i<samples;++i) {
            EnvironmentSample s=brightMap.sampleDirection(seed);
            require(std::fabs(s.pdf-brightMap.directionPdf(s.wi)) < 1e-5f,
                    "sample and queried PDFs differ");
            int x=std::min(int(LatLongS(s.wi)*8),7);
            int y=std::min(int(std::acosf(std::clamp(s.wi.y(),-1.f,1.f))*4/PI),3);
            if(y*8+x==bright) { ++observed; brightDirection += s.wi; }
        }
        const double expected=brightMap.texelProbability[bright];
        const double sigma=std::sqrt(expected*(1-expected)/samples);
        require(std::fabs(double(observed)/samples-expected) < 5*sigma+1e-4,
                "alias frequencies do not match target distribution");
        // UsdLux's lat-long convention: column 3 of 8 (s 0.375 to 0.5) lies
        // between longitudes pi/4 and 0, i.e. between +Z and +X, nearer +Z;
        // row 2 of 4 is just below the horizon.
        brightDirection = brightDirection.normalize();
        require(brightDirection.z() > brightDirection.x() && brightDirection.x() > 0.1f &&
                brightDirection.y() < 0.0f && brightDirection.y() > -0.6f,
                "the bright texel is not sampled toward UsdLux's longitude for it");

        // Lookups follow UsdLux (the OpenEXR lat-long convention): the image's
        // centre column is +Z, its quarter +X, its three quarters -X and its
        // edges -Z; the top row is +Y. Two columns of eight per direction, the
        // top and bottom rows of four their own colours.
        {
            std::vector<unsigned char> bands(8*4*3, 0);
            auto set = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b) {
                unsigned char* p = &bands[(size_t(y)*8+x)*3]; p[0]=r; p[1]=g; p[2]=b;
            };
            for (int y=0;y<4;++y)
                for (int x=0;x<8;++x) {
                    if (y==0) set(x,y,0,255,255);          // +Y: cyan
                    else if (y==3) set(x,y,255,0,255);     // -Y: magenta
                    else if (x==7 || x==0) set(x,y,255,0,0); // -Z: red
                    else if (x<=2) set(x,y,0,255,0);       // +X: green
                    else if (x<=4) set(x,y,0,0,255);       // +Z: blue
                    else set(x,y,255,255,0);               // -X: yellow
                }
            auto bandTexture = image(bands, 8, 4);
            EnvironmentMap bandMap(&bandTexture, Color3(1.0f));
            struct Probe { Float3 direction; Color3 colour; const char* what; };
            const Probe probes[] = {
                {Float3(0,0,-1), Color3(1,0,0), "-Z must read the image's left and right edges"},
                {Float3(1,0,0), Color3(0,1,0), "+X must read the image's first quarter"},
                {Float3(0,0,1), Color3(0,0,1), "+Z must read the image's centre"},
                {Float3(-1,0,0), Color3(1,1,0), "-X must read the image's third quarter"},
                {Float3(0,1,0), Color3(0,1,1), "+Y must read the image's top row"},
                {Float3(0,-1,0), Color3(1,0,1), "-Y must read the image's bottom row"},
            };
            for (auto const& probe : probes) {
                const Color3 value = bandMap.radiance(probe.direction);
                require((value - probe.colour).length() < 1e-3f, probe.what);
            }
        }

        // Non-uniform transforms exercise the solid-angle Jacobian. E[1/pdf]
        // remains the sphere area when sample and PDF transforms agree.
        TransformedEnvironmentMap transformed(&constantTexture, Color3(1),
            {2,0,0, 0,.5f,0, 0,0,1.5f});
        inversePdfMean=0;
        for(int i=0;i<250000;++i) {
            EnvironmentSample s=transformed.sampleDirection(seed);
            require(std::fabs(s.pdf-transformed.directionPdf(s.wi)) < 2e-5f,
                    "transformed sample and queried PDFs differ");
            inversePdfMean += 1.0/s.pdf;
        }
        inversePdfMean/=250000.0;
        require(std::fabs(inversePdfMean-4.0*PI) < 0.08,
                "transformed PDF is not normalized");
        TransformedEnvironmentMap rotated(&brightTexture, Color3(1),
            {0,0,1, 0,1,0, -1,0,0});
        // Filtering changes radiance, not sampled directions, PDFs or RNG.
        // Both MIS techniques must evaluate an identical outgoing footprint.
        for (Environment* environment : std::vector<Environment*>{&brightMap, &transformed, &rotated}) {
            for (float radius : {0.0f, 0.001f, 0.15f}) {
                const prism::DirectionFootprint footprint{radius, prism::TextureFiltering::Filtered};
                unsigned referenceSeed=1928, filteredSeed=referenceSeed;
                for (int i=0;i<1024;++i) {
                    const auto reference=environment->sampleDirection(referenceSeed);
                    const auto filtered=environment->sampleDirection(filteredSeed,footprint);
                    require(referenceSeed==filteredSeed && (reference.wi-filtered.wi).is_zero() && reference.pdf==filtered.pdf,
                            "Filtering changed environment sampling distribution or RNG");
                    const auto queried=environment->radiance(filtered.wi,footprint);
                    require((queried-filtered.radiance).is_zero(),
                            "NEE and BSDF techniques filter the same environment direction differently");
                }
                for (auto direction : {Float3(0,1,0),Float3(0,-1,0),Float3(1,0,0)}) {
                    const auto value=environment->radiance(direction,footprint);
                    require(std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z()),
                            "Environment footprint produced non-finite pole or seam radiance");
                }
            }
        }
        for (int i=0;i<10000;++i) {
            const auto s=rotated.sampleDirection(seed);
            require(std::fabs(s.pdf-rotated.directionPdf(s.wi))<2e-5f,
                    "Rotation changed the sampling density");
            require((s.radiance-brightMap.radiance(Float3(s.wi.z(),s.wi.y(),-s.wi.x()))).length()<1e-5f,
                    "Dome rotation changed radiance orientation");
        }
        std::vector<unsigned char> black(8*4*3,0);
        auto blackTexture=image(black,8,4);
        EnvironmentMap blackMap(&blackTexture, Color3(1.0f));
        auto blackSample=blackMap.sampleDirection(seed);
        require(blackSample.radiance.is_zero() && std::isfinite(blackSample.pdf) && blackSample.pdf>0,
                "Black HDR must fall back to a valid uniform distribution");
        std::vector<unsigned char> tall(2*8192*3,255);
        auto tallTexture=image(tall,2,8192);
        EnvironmentMap poles(&tallTexture,Color3(1));
        require(std::isfinite(poles.directionPdf(Float3(0,1,0))) &&
                poles.directionPdf(Float3(0,1,0))>0,
                "High resolution pole density must remain finite");
        std::vector<unsigned char> polarPixels(2*16384*3,0);
        for (int row=0;row<3;++row)
            for(int i=0;i<6;++i) polarPixels[row*6+i]=static_cast<unsigned char>(64*(row+1));
        auto polarTexture=image(polarPixels,2,16384);
        EnvironmentMap polarMap(&polarTexture,Color3(1));
        for(int i=0;i<20000;++i) {
            const auto s=polarMap.sampleDirection(seed);
            require(std::abs(s.pdf-polarMap.directionPdf(s.wi)) < 1e-3f*s.pdf,
                    "Polar HDR sample lost its angular row to float rounding");
        }
        std::cout << "environment sampling tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
