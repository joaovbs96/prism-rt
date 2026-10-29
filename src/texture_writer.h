#pragma once

// Writing prepared textures: the mip chain OIIO's make_texture computes, and
// a tiled, mipmapped OpenEXR writer that stops at the first failed write.
//
// OpenEXR's TiledOutputFile::writeTiles waits for a tile buffer's semaphore,
// writes the compressed tile and posts the semaphore again. If that write
// throws (a full disk, an I/O error, an allocation failure under a memory
// cap), the semaphore is never posted. OIIO catches the exception, returns
// false from write_tiles and its write_image goes on to the next row of
// tiles, whose first task then waits on the leaked buffer forever: the
// "every IlmThread worker idle" hang inside make_texture. Here the output
// stream never reports a failed write to OpenEXR (it records it instead),
// so no tile buffer leaks, each row's result is checked, and nothing is
// written after a failure.

#include "internal_export.h"
#include <OpenImageIO/imageio.h>

#include <filesystem>
#include <string>
#include <vector>

namespace prism::texture_writer {

struct Level {
    int width = 0, height = 0;
    std::vector<float> pixels; // width * height * channels, row major
};

// Appends the mip levels below levels.back() (the top level on entry) until
// a 1x1 level, exactly as make_texture's box filter (resize_block) computes
// them: a 2x2 average when both dimensions are even, otherwise a bilinear
// sample of the previous level at each texel center with clamped edges.
PRISM_INTERNAL_API void BuildMipChain(std::vector<Level> &levels, int channels);

// Writes `levels` (a full chain from BuildMipChain) to `path` as a tiled,
// mipmapped float OpenEXR texture with `spec`'s channel names, alpha
// channel and attributes. The file is flushed to disk before returning.
// Returns false, with `error` set, after the first failure; the file is
// then incomplete and the caller removes it.
PRISM_INTERNAL_API bool WriteMipmappedExr(std::filesystem::path const &path, OIIO::ImageSpec const &spec,
                       std::vector<Level> const &levels, std::string *error);

// Writes a whole image (scanline or tiled, as `spec` says) through the same
// failure-safe stream; for OpenEXR files written outside texture
// preparation.
bool WriteImageSafely(std::filesystem::path const &path, OIIO::ImageSpec const &spec,
                      void const *pixels, OIIO::TypeDesc format, std::string *error);

// The compression of prepared textures: configured (the texture service's
// TextureServiceOptions::compression, PRISM_TEXTURE_COMPRESSION), default
// "zip:1" (OpenEXR's zip at deflate level 1).
PRISM_INTERNAL_API std::string PreparedCompression(std::string const &configured = {});

} // namespace prism::texture_writer
