#pragma once

// GPU-ready texture cache files (<content key>.gpu next to the prepared
// .tx.exr). TextureService writes one while it prepares a texture, from the
// same decode, at the source's precision: 8- and 16-bit integer sources stay
// 8- and 16-bit (UNORM), half stays half and float stays float, with only the
// texture's own channels. A GPU device reads it straight into an upload
// buffer, without decoding the float .tx.exr.
//
// Layout (little endian): a 256-byte header, then each mip level's texels,
// rows top to bottom, channels interleaved, without padding. Only level 0 is
// stored today (the devices generate the rest); the level table leaves room
// for stored mips and per-level streaming. The header carries a digest of the
// payload, checked on every read, so a file cut short or zeroed by a crash is
// rejected and prepared again.

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

constexpr uint32_t kMaxLevels = 8;

struct Level {
    uint64_t offset = 0, bytes = 0; // within the payload
    uint32_t width = 0, height = 0;
};

// Statistics of a Float32 payload, which a device needs to pick a storage
// format exactly as it did from the float .tx.exr: all values k/255 in
// [0, 1] (an 8-bit image after a lossless transform), and the largest finite
// magnitude.
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
    uint64_t digest;
    Level levels[kMaxLevels];
};
static_assert(sizeof(Level) == 24, "level entry layout");
static_assert(sizeof(Header) == 256, "header layout");

struct Image {
    Header header{};
    std::vector<uint8_t> payload;
};

// The payload digest (64-bit, block-parallel; not cryptographic).
uint64_t Digest(void const *data, size_t bytes);

// Fills a header for one level of `width` x `height` x `channels` texels.
Header MakeHeader(uint32_t width, uint32_t height, uint32_t channels, Component component, uint32_t flags,
                  float peak);

// Writes header and payload to a temporary sibling of `path`, flushes it to
// disk and renames it into place. False on any failure (nothing is left).
bool Write(std::filesystem::path const &path, Header header, void const *payload, std::string *error);

// Reads and validates `path`: header fields, file size and digest. With
// `payload` null only the header is read and checked.
bool Read(std::filesystem::path const &path, Header *header, std::vector<uint8_t> *payload, std::string *error);

// Reads the payload of a file whose `header` Read returned into
// `destination` (header.payloadBytes long) and checks its digest.
bool ReadPayload(std::filesystem::path const &path, Header const &header, void *destination, std::string *error);

// Statistics of float texels for a Float32 header.
void FloatStatistics(float const *values, size_t count, uint32_t *flags, float *peak);

} // namespace prism::gpu_texture
