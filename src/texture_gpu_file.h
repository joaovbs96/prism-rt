#pragma once

// GPU-ready texture cache files (<content key>.gpu next to the prepared
// .tx.exr). TextureService writes one while it prepares a texture, from the
// same decode, at the source's precision: 8- and 16-bit integer sources stay
// 8- and 16-bit (UNORM), half stays half and float stays float, with only the
// texture's own channels. A GPU device reads it straight into an upload
// buffer, without decoding the float .tx.exr.
//
// Layout (little endian): a 1 KiB header, then each mip level's texels,
// rows top to bottom, channels interleaved, without padding, finest level
// first. The service stores the whole mip chain (BuildMipChain: the filter of
// vkCmdBlitImage with linear filtering, level by level at the stored
// precision), so a device can read any range of levels without the others
// (texture streaming). Each level carries its own digest, checked on every
// read of it, so a file cut short or zeroed by a crash is rejected and
// prepared again. Version 1 files (one level, 256-byte header) fail
// validation and are written again from the source.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace prism::gpu_texture {

enum class Component : uint32_t { Unorm8 = 1, Unorm16 = 2, Float16 = 3, Float32 = 4 };

inline uint32_t ComponentBytes(Component c)
{
    switch (c) {
    case Component::Unorm8: return 1;
    case Component::Unorm16: case Component::Float16: return 2;
    default: return 4;
    }
}

constexpr uint32_t kMaxLevels = 16; // up to 32768 texels per side

struct Level {
    uint64_t offset = 0, bytes = 0; // within the payload
    uint32_t width = 0, height = 0;
    uint64_t digest = 0;            // of this level's bytes
};

// Statistics of a Float32 payload, which a device needs to pick a storage
// format exactly as it did from the float .tx.exr: all values k/255 in
// [0, 1] (an 8-bit image after a lossless transform), and the largest finite
// magnitude. They describe level 0.
enum Flags : uint32_t { kQuantized8 = 1u };

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t headerBytes;
    uint32_t width, height;
    uint32_t channels;
    Component component;
    uint32_t levelCount;
    uint32_t flags;
    float peak;
    uint32_t reserved0;
    uint64_t payloadBytes;
    uint64_t digest;          // of the level digests
    Level levels[kMaxLevels];
    uint8_t reserved[448];
};
static_assert(sizeof(Level) == 32, "level entry layout");
static_assert(sizeof(Header) == 1024, "header layout");

struct Image {
    Header header{};
    std::vector<uint8_t> payload;
};

// The number of levels of a full mip chain (down to 1 x 1, halving with
// floor as Vulkan and Metal do).
uint32_t FullChainLevels(uint32_t width, uint32_t height);

// The payload digest (64-bit, block-parallel; not cryptographic).
uint64_t Digest(void const *data, size_t bytes);

// Fills a header for `levelCount` levels of a `width` x `height` x
// `channels` image (1: level 0 only; 0: the full chain).
Header MakeHeader(uint32_t width, uint32_t height, uint32_t channels, Component component, uint32_t flags,
                  float peak, uint32_t levelCount = 1);

// The payload of the full mip chain of `level0` (width x height x channels
// texels of `component`), as MakeHeader(..., 0) lays it out. Each level is
// filtered from the previous one as vkCmdBlitImage with VK_FILTER_LINEAR
// does (bilinear at each destination texel's center, clamped to the edge;
// a 2 x 2 box for even sizes) and rounded to the component.
std::vector<uint8_t> BuildMipChain(void const *level0, uint32_t width, uint32_t height, uint32_t channels,
                                   Component component);

// Writes header and payload to a temporary sibling of `path`, flushes it to
// disk and renames it into place. False on any failure (nothing is left).
bool Write(std::filesystem::path const &path, Header header, void const *payload, std::string *error);

// Reads and validates `path`: header fields, file size and digests. With
// `payload` null only the header is read and checked.
bool Read(std::filesystem::path const &path, Header *header, std::vector<uint8_t> *payload, std::string *error);

// Reads the payload of a file whose `header` Read returned into
// `destination` (header.payloadBytes long) and checks its digests.
bool ReadPayload(std::filesystem::path const &path, Header const &header, void *destination, std::string *error);

// Reads levels [first, first + count) of a file whose `header` Read returned
// into `destination` (their bytes, contiguous as in the payload) and checks
// their digests. Only those levels are read.
bool ReadLevels(std::filesystem::path const &path, Header const &header, uint32_t first, uint32_t count,
                void *destination, std::string *error);

// The bytes of levels [first, levelCount).
uint64_t LevelRangeBytes(Header const &header, uint32_t first, uint32_t count);

// Statistics of float texels for a Float32 header.
void FloatStatistics(float const *values, size_t count, uint32_t *flags, float *peak);

// IEEE binary16 conversions (round to nearest even), as the devices use them.
uint16_t FloatToHalf(float value);
float HalfToFloat(uint16_t half);

} // namespace prism::gpu_texture
