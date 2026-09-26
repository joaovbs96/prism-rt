#include <OpenImageIO/imagebuf.h>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3 && !(argc == 4 && std::string(argv[3]) == "--exact")) {
        std::cerr << "usage: prism_compare_images reference.exr actual.exr [--exact]\n";
        return 2;
    }
    OIIO::ImageBuf reference(argv[1]), actual(argv[2]);
    if (!reference.read(0, 0, true, OIIO::TypeDesc::FLOAT) ||
        !actual.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::cerr << "Cannot read images: " << reference.geterror() << ' ' << actual.geterror() << '\n';
        return 2;
    }
    const auto& a = reference.spec();
    const auto& b = actual.spec();
    if (a.width != b.width || a.height != b.height || a.nchannels < 3 || b.nchannels < 3) {
        std::cerr << "Image dimensions/channels differ\n";
        return 1;
    }
    double error = 0, energy = 0, referenceLuminance = 0, actualLuminance = 0;
    std::vector<float> p(a.nchannels), q(b.nchannels);
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            reference.getpixel(x, y, p.data()); actual.getpixel(x, y, q.data());
            for (int c = 0; c < 3; ++c) {
                if (!std::isfinite(p[c]) || !std::isfinite(q[c])) {
                    std::cerr << "Non-finite pixel at " << x << ',' << y << '\n';
                    return 1;
                }
                error += std::pow(double(p[c])-q[c], 2);
                energy += double(p[c])*p[c];
            }
            referenceLuminance += .2126*p[0]+.7152*p[1]+.0722*p[2];
            actualLuminance += .2126*q[0]+.7152*q[1]+.0722*q[2];
        }
    }
    const double nrmse = std::sqrt(error / std::max(energy, 1e-20));
    const double luminanceDelta = std::abs(actualLuminance-referenceLuminance) / std::max(std::abs(referenceLuminance), 1e-20);
    std::cout << "Normalized RMSE: " << nrmse << "; relative mean luminance error: " << luminanceDelta << '\n';
    const double tolerance = argc == 4 ? 0.0 : .01;
    return nrmse <= tolerance && luminanceDelta <= tolerance ? 0 : 1;
}
