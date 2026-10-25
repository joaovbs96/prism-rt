#include <OpenImageIO/imagebuf.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
// The three channels compared: R, G, B (or <layer>.R/G/B, X/Y/Z, U/V), else
// the first three.
bool Channels(OIIO::ImageSpec const& spec, std::string const& layer, int* indices) {
    const std::string prefix = layer.empty() ? std::string() : layer + ".";
    static const char* const color[] = {"R", "G", "B"};
    static const char* const vector[] = {"X", "Y", "Z"};
    for (const char* const* names : {color, vector}) {
        bool found = true;
        for (int c = 0; c < 3 && found; ++c) {
            indices[c] = spec.channelindex(prefix + names[c]);
            found = indices[c] >= 0;
        }
        if (found) return true;
    }
    // A single-channel layer (an id or a depth): that channel three times.
    if (!layer.empty()) {
        int single = -1, count = 0;
        for (int c = 0; c < spec.nchannels; ++c) {
            const std::string& name = spec.channelnames[c];
            if (name == layer || name.rfind(prefix, 0) == 0) { single = c; ++count; }
        }
        if (count != 1) return false;
        for (int c = 0; c < 3; ++c) indices[c] = single;
        return true;
    }
    if (spec.nchannels < 3) return false;
    for (int c = 0; c < 3; ++c) indices[c] = c;
    return true;
}
}

int main(int argc, char** argv) {
    // Both metrics must be within the tolerance: 1% by default, 0 with --exact.
    // --mean-tolerance sets the mean luminance one separately (noisy images
    // whose renderers sample differently).
    // --relabel compares an id layer up to a renumbering: the two images must
    // match pixel for pixel under one bijection of their values (frontends
    // number their prims differently); negative values (misses) must agree.
    double tolerance = .01, meanTolerance = -1;
    std::string layer;
    bool relabel = false;
    bool usage = argc < 3;
    for (int i = 3; i < argc && !usage; ++i) {
        const std::string option = argv[i];
        if (option == "--exact") tolerance = 0.0;
        else if (option == "--tolerance" && i + 1 < argc) tolerance = std::stod(argv[++i]);
        else if (option == "--mean-tolerance" && i + 1 < argc) meanTolerance = std::stod(argv[++i]);
        else if (option == "--layer" && i + 1 < argc) layer = argv[++i];
        else if (option == "--relabel") relabel = true;
        else usage = true;
    }
    if (meanTolerance < 0) meanTolerance = tolerance;
    if (usage || !(tolerance >= 0)) {
        std::cerr << "usage: prism_compare_images reference.exr actual.exr [--exact | --tolerance 0.01]"
                     " [--mean-tolerance 0.01] [--layer NAME [--relabel]]\n";
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
    int ca[3], cb[3];
    if (a.width != b.width || a.height != b.height || !Channels(a, layer, ca) || !Channels(b, layer, cb)) {
        std::cerr << "Image dimensions/channels differ\n";
        return 1;
    }
    std::vector<float> p(a.nchannels), q(b.nchannels);
    if (relabel) {
        std::map<float, float> forward, backward;
        size_t conflicts = 0;
        for (int y = 0; y < a.height; ++y)
            for (int x = 0; x < a.width; ++x) {
                reference.getpixel(a.x + x, a.y + y, p.data()); actual.getpixel(b.x + x, b.y + y, q.data());
                const float u = p[ca[0]], v = q[cb[0]];
                auto [f, fAdded] = forward.emplace(u, v);
                auto [r, rAdded] = backward.emplace(v, u);
                // Misses (-1) are -1 in both.
                conflicts += f->second != v || r->second != u || ((u < 0 || v < 0) && u != v);
            }
        std::cout << "Relabelled values: " << forward.size() << "; conflicting pixels: " << conflicts << '\n';
        return conflicts == 0 ? 0 : 1;
    }
    double error = 0, energy = 0, referenceLuminance = 0, actualLuminance = 0;
    const double weights[3] = {.2126, .7152, .0722};
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            reference.getpixel(a.x + x, a.y + y, p.data()); actual.getpixel(b.x + x, b.y + y, q.data());
            for (int c = 0; c < 3; ++c) {
                const double u = p[ca[c]], v = q[cb[c]];
                if (!std::isfinite(u) || !std::isfinite(v)) {
                    std::cerr << "Non-finite pixel at " << x << ',' << y << '\n';
                    return 1;
                }
                error += (u - v) * (u - v);
                energy += u * u;
                referenceLuminance += weights[c] * u;
                actualLuminance += weights[c] * v;
            }
        }
    }
    const double nrmse = std::sqrt(error / std::max(energy, 1e-20));
    const double luminanceDelta = std::abs(actualLuminance-referenceLuminance) / std::max(std::abs(referenceLuminance), 1e-20);
    std::cout << "Normalized RMSE: " << nrmse << "; relative mean luminance error: " << luminanceDelta
              << " (tolerances " << tolerance << ", " << meanTolerance << ")\n";
    return nrmse <= tolerance && luminanceDelta <= meanTolerance ? 0 : 1;
}
