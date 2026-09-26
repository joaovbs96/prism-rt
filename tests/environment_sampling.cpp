#include "../src/environment.h"

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
        for (int i=0;i<samples;++i) {
            EnvironmentSample s=brightMap.sampleDirection(seed);
            require(std::fabs(s.pdf-brightMap.directionPdf(s.wi)) < 1e-5f,
                    "sample and queried PDFs differ");
            float phi=std::atan2f(s.wi.z(),s.wi.x()); if(phi<0)phi+=2*PI;
            int x=std::min(int(phi*8/(2*PI)),7);
            int y=std::min(int(std::acosf(std::clamp(s.wi.y(),-1.f,1.f))*4/PI),3);
            if(y*8+x==bright) ++observed;
        }
        const double expected=brightMap.texelProbability[bright];
        const double sigma=std::sqrt(expected*(1-expected)/samples);
        require(std::fabs(double(observed)/samples-expected) < 5*sigma+1e-4,
                "alias frequencies do not match target distribution");

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
        for (int i=0;i<10000;++i) {
            const auto s=rotated.sampleDirection(seed);
            require(std::fabs(s.pdf-rotated.directionPdf(s.wi))<2e-5f,
                    "Rotation changed the sampling density");
            require((s.radiance-brightMap.radiance(Float3(s.wi.z(),s.wi.y(),-s.wi.x()))).length()<1e-5f,
                    "Dome rotation changed radiance orientation");
        }
        std::vector<unsigned char> black(8*4*3,0);
        auto blackTexture=image(black,8,4);
        EnvironmentMap blackMap(&blackTexture);
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
