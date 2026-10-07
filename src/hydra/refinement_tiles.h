#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace prism::hydra {

inline constexpr unsigned RefinementTileSize = 32;

struct RefinementTile {
    unsigned x, y, width, height;
    size_t index;
};

// Reverse the interleaved coordinate bits so early tiles visit separated
// screen regions. The schedule consumes no random numbers and clips edge tiles.
inline std::vector<RefinementTile> MakeRefinementTiles(unsigned width, unsigned height)
{
    const unsigned nx = width / RefinementTileSize + (width % RefinementTileSize != 0);
    const unsigned ny = height / RefinementTileSize + (height % RefinementTileSize != 0);
    unsigned bits = 0;
    for (uint64_t extent = 1; extent < std::max(nx, ny); extent <<= 1) ++bits;
    auto key = [bits](unsigned x, unsigned y) {
        uint64_t value = 0;
        for (unsigned bit = 0; bit < bits; ++bit) {
            value = (value << 1) | ((x >> bit) & 1);
            value = (value << 1) | ((y >> bit) & 1);
        }
        return value;
    };
    std::vector<RefinementTile> result;
    result.reserve(static_cast<size_t>(nx) * ny);
    for (unsigned y = 0; y < ny; ++y)
        for (unsigned x = 0; x < nx; ++x)
            result.push_back({x * RefinementTileSize, y * RefinementTileSize,
                std::min(RefinementTileSize, width - x * RefinementTileSize),
                std::min(RefinementTileSize, height - y * RefinementTileSize),
                static_cast<size_t>(y) * nx + x});
    std::sort(result.begin(), result.end(), [&](auto const &a, auto const &b) {
        return key(a.x / RefinementTileSize, a.y / RefinementTileSize) <
               key(b.x / RefinementTileSize, b.y / RefinementTileSize);
    });
    return result;
}

} // namespace prism::hydra
