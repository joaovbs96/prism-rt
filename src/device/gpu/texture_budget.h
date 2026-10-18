#pragma once

// Fitting a scene's images into the GPU memory available for them: the
// largest images are halved (repeatedly) until the estimate fits. Shared by
// the GPU devices; environment images keep full resolution.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <queue>
#include <vector>

namespace prism::device::gpu {

struct ImageFootprint {
    int width = 0, height = 0;
    int storedChannels = 4;   // after padding three channels to four
    int componentBytes = 1;
    bool keepResolution = false;
};

inline uint64_t ImageBytes(ImageFootprint const& image, int shift)
{
    const uint64_t w = uint64_t(std::max(1, image.width >> shift));
    const uint64_t h = uint64_t(std::max(1, image.height >> shift));
    return w * h * uint64_t(image.storedChannels) * uint64_t(image.componentBytes) * 4 / 3; // with mips
}

// Budget in bytes: PRISM_GPU_TEXTURE_BUDGET_MIB, else `fraction` of `available`.
inline uint64_t TextureBudget(uint64_t available, double fraction = 0.45)
{
    if (const char* value = std::getenv("PRISM_GPU_TEXTURE_BUDGET_MIB"); value && *value)
        return uint64_t(std::max(64ll, std::atoll(value))) << 20;
    return uint64_t(double(available) * fraction);
}

// Per image, how many times to halve its resolution (0: full resolution).
inline std::vector<int> FitImages(std::vector<ImageFootprint> const& images, uint64_t budget,
                                  int minimumSize = 256)
{
    std::vector<int> shift(images.size(), 0);
    uint64_t total = 0;
    for (auto const& image : images) total += ImageBytes(image, 0);
    using Entry = std::pair<uint64_t, size_t>; // current bytes, image
    std::priority_queue<Entry> largest;
    for (size_t i = 0; i < images.size(); ++i)
        if (!images[i].keepResolution) largest.push({ImageBytes(images[i], 0), i});
    while (total > budget && !largest.empty()) {
        const auto [bytes, i] = largest.top();
        largest.pop();
        auto const& image = images[i];
        if (std::max(image.width, image.height) >> (shift[i] + 1) < minimumSize) continue;
        ++shift[i];
        const uint64_t smaller = ImageBytes(image, shift[i]);
        total -= bytes - smaller;
        largest.push({smaller, i});
    }
    return shift;
}

// Box-filters `pixels` (width x height x channels) down by 2^shift in place;
// returns the new size.
inline void Downsample(std::vector<float>& pixels, int& width, int& height, int channels, int shift)
{
    for (int s = 0; s < shift && (width > 1 || height > 1); ++s) {
        const int w = std::max(1, width / 2), h = std::max(1, height / 2);
        std::vector<float> out(size_t(w) * h * channels, 0.0f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < channels; ++c) {
                    float sum = 0.0f;
                    int count = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(width - 1, 2 * x + dx), sy = std::min(height - 1, 2 * y + dy);
                            sum += pixels[(size_t(sy) * width + sx) * channels + c];
                            ++count;
                        }
                    out[(size_t(y) * w + x) * channels + c] = sum / float(count);
                }
        pixels = std::move(out);
        width = w;
        height = h;
    }
}

} // namespace prism::device::gpu
