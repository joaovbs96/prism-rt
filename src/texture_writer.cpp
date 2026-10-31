#include "texture_writer.h"

#include <OpenImageIO/filesystem.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace prism::texture_writer {
namespace {

// A write-only file whose writes never fail as far as OpenEXR can tell: the
// first failure is recorded and every later write is dropped. See the header
// comment for why a failure must not reach OpenEXR's tile writer.
class SafeOutputFile final : public OIIO::Filesystem::IOProxy {
public:
    explicit SafeOutputFile(std::filesystem::path const &path)
        : IOProxy(path.string(), Write)
    {
        // Tests inject a write failure after this many bytes.
        if (const char *limit = std::getenv("PRISM_TEST_TEXTURE_WRITE_FAIL_AFTER"); limit && *limit)
            m_failAfter = std::strtoll(limit, nullptr, 10);
#ifdef _WIN32
        m_file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) fail("cannot create");
#else
        m_file = std::fopen(path.c_str(), "wb");
        if (!m_file) fail("cannot create");
#endif
    }
    ~SafeOutputFile() override { close(); }
    const char *proxytype() const override { return "prism-safe-file"; }
    void close() override
    {
#ifdef _WIN32
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
#else
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
#endif
    }
    bool opened() const override { return !m_failed; }
#if OIIO_VERSION >= OIIO_MAKE_VERSION(3, 0, 0)
    int64_t tell() const override { return m_pos; }
#else
    int64_t tell() override { return m_pos; }
#endif
    bool seek(int64_t offset) override
    {
        m_pos = offset;
        return true;
    }
    size_t write(const void *buf, size_t size) override
    {
        const size_t written = pwrite(buf, size, m_pos);
        m_pos += static_cast<int64_t>(size);
        return written;
    }
    size_t pwrite(const void *buf, size_t size, int64_t offset) override
    {
        if (m_failed || size == 0) return size;
        if (m_failAfter >= 0 && offset + static_cast<int64_t>(size) > m_failAfter) {
            fail("injected failure writing");
            return size;
        }
#ifdef _WIN32
        const char *bytes = static_cast<const char *>(buf);
        size_t done = 0;
        while (done < size) {
            OVERLAPPED at{};
            const uint64_t position = static_cast<uint64_t>(offset) + done;
            at.Offset = static_cast<DWORD>(position);
            at.OffsetHigh = static_cast<DWORD>(position >> 32);
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 30));
            DWORD written = 0;
            if (!WriteFile(m_file, bytes + done, chunk, &written, &at) || written == 0) {
                fail("cannot write");
                return size;
            }
            done += written;
        }
#else
        if (fseeko(m_file, offset, SEEK_SET) != 0 || std::fwrite(buf, 1, size, m_file) != size) {
            fail("cannot write");
            return size;
        }
#endif
        m_size = std::max(m_size, offset + static_cast<int64_t>(size));
        return size;
    }
    size_t size() const override { return static_cast<size_t>(m_size); }
#if OIIO_VERSION >= OIIO_MAKE_VERSION(3, 0, 0)
    void flush() override {}
#else
    void flush() const override {}
#endif
    // Makes the written bytes durable; false if any write failed.
    bool finish()
    {
        if (m_failed) return false;
#ifdef _WIN32
        if (!FlushFileBuffers(m_file)) fail("cannot flush");
#else
        if (std::fflush(m_file) != 0 || fsync(fileno(m_file)) != 0) fail("cannot flush");
#endif
        close();
        return !m_failed;
    }
    bool failed() const { return m_failed; }
    std::string const &failure() const { return m_failure; }

private:
    void fail(char const *what)
    {
        if (m_failed) return;
        m_failed = true;
        m_failure = std::string(what) + " " + filename();
#ifdef _WIN32
        m_failure += " (Windows error " + std::to_string(GetLastError()) + ")";
#endif
    }
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE;
#else
    FILE *m_file = nullptr;
#endif
    int64_t m_pos = 0, m_size = 0, m_failAfter = -1;
    bool m_failed = false;
    std::string m_failure;
};

// One destination texel of make_texture's resize_block_ (bilinear sample at
// the texel center, clamped). The arithmetic follows OIIO's interppixel_NDC,
// floorfrac and bilerp operation for operation.
inline void SampleBilinear(Level const &src, int channels, int dstWidth, int dstHeight, int x, int y,
                           float *out)
{
    const float xscale = 1.0f / static_cast<float>(dstWidth);
    const float yscale = 1.0f / static_cast<float>(dstHeight);
    const float t = (static_cast<float>(y) + 0.5f) * yscale + 0.0f;
    const float s = (static_cast<float>(x) + 0.5f) * xscale + 0.0f;
    float fx = 0.0f + s * static_cast<float>(src.width);
    float fy = 0.0f + t * static_cast<float>(src.height);
    fx -= 0.5f;
    fy -= 0.5f;
    const float floorX = std::floor(fx), floorY = std::floor(fy);
    const int xtexel = static_cast<int>(floorX), ytexel = static_cast<int>(floorY);
    const float xfrac = fx - floorX, yfrac = fy - floorY;
    auto texel = [&](int tx, int ty) {
        tx = std::clamp(tx, 0, src.width - 1);
        ty = std::clamp(ty, 0, src.height - 1);
        return src.pixels.data() + (static_cast<size_t>(ty) * src.width + tx) * channels;
    };
    const float *p0 = texel(xtexel, ytexel), *p1 = texel(xtexel + 1, ytexel);
    const float *p2 = texel(xtexel, ytexel + 1), *p3 = texel(xtexel + 1, ytexel + 1);
    const float s1 = 1.0f - xfrac, t1 = 1.0f - yfrac;
    for (int c = 0; c < channels; ++c)
        out[c] = t1 * (p0[c] * s1 + p1[c] * xfrac) + yfrac * (p2[c] * s1 + p3[c] * xfrac);
}

bool WriteLevelTiles(OIIO::ImageOutput &out, OIIO::ImageSpec const &spec, Level const &level,
                     SafeOutputFile const &file, std::string *error)
{
    const int channels = spec.nchannels;
    const OIIO::stride_t xstride = static_cast<OIIO::stride_t>(channels * sizeof(float));
    const OIIO::stride_t ystride = xstride * level.width;
    for (int y = 0; y < level.height; y += spec.tile_height) {
        const int yend = std::min(y + spec.tile_height, level.height);
        const float *row = level.pixels.data() + static_cast<size_t>(y) * level.width * channels;
        if (!out.write_tiles(0, level.width, y, yend, 0, 1, OIIO::TypeDesc::FLOAT, row, xstride, ystride) ||
            file.failed()) {
            if (error) *error = file.failed() ? file.failure() : out.geterror();
            return false;
        }
    }
    return true;
}

} // namespace

std::string PreparedCompression(std::string const &configured)
{
    return configured.empty() ? std::string("zip:1") : configured;
}

void BuildMipChain(std::vector<Level> &levels, int channels)
{
    while (levels.back().width > 1 || levels.back().height > 1) {
        Level const &src = levels.back();
        Level dst;
        dst.width = src.width > 1 ? src.width / 2 : src.width;
        dst.height = src.height > 1 ? src.height / 2 : src.height;
        dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * channels);
        // resize_block's fast path: both source dimensions even (so the
        // destination is exactly half of each). It averages horizontal pairs
        // first, then the two rows.
        const bool halve = 2 * dst.width == src.width && 2 * dst.height == src.height;
        tbb::parallel_for(tbb::blocked_range<int>(0, dst.height, 16), [&](tbb::blocked_range<int> const &rows) {
            for (int y = rows.begin(); y < rows.end(); ++y) {
                float *d = dst.pixels.data() + static_cast<size_t>(y) * dst.width * channels;
                if (halve) {
                    const float *s0 = src.pixels.data() + static_cast<size_t>(2 * y) * src.width * channels;
                    const float *s1 = s0 + static_cast<size_t>(src.width) * channels;
                    for (int x = 0; x < dst.width; ++x)
                        for (int c = 0; c < channels; ++c) {
                            const size_t a = (static_cast<size_t>(2 * x)) * channels + c;
                            const float top = 0.5f * (s0[a] + s0[a + channels]);
                            const float bottom = 0.5f * (s1[a] + s1[a + channels]);
                            d[static_cast<size_t>(x) * channels + c] = 0.5f * (top + bottom);
                        }
                } else {
                    for (int x = 0; x < dst.width; ++x)
                        SampleBilinear(src, channels, dst.width, dst.height, x, y,
                                       d + static_cast<size_t>(x) * channels);
                }
            }
        });
        levels.push_back(std::move(dst));
    }
}

bool WriteMipmappedExr(std::filesystem::path const &path, OIIO::ImageSpec const &spec,
                       std::vector<Level> const &levels, std::string *error)
{
    try {
        auto out = OIIO::ImageOutput::create("openexr");
        if (!out) {
            if (error) *error = "no OpenEXR writer: " + OIIO::geterror();
            return false;
        }
        SafeOutputFile file(path);
        if (file.failed()) {
            if (error) *error = file.failure();
            return false;
        }
        out->set_ioproxy(&file);
        OIIO::ImageSpec levelSpec = spec;
        levelSpec.set_format(OIIO::TypeDesc::FLOAT);
        bool ok = true;
        for (size_t l = 0; l < levels.size() && ok; ++l) {
            Level const &level = levels[l];
            if (l > 0) {
                // make_texture's lower levels carry the top level's spec
                // without its metadata, at the level's size.
                OIIO::ImageSpec lower(level.width, level.height, spec.nchannels, OIIO::TypeDesc::FLOAT);
                lower.channelnames = spec.channelnames;
                lower.alpha_channel = spec.alpha_channel;
                lower.tile_width = spec.tile_width;
                lower.tile_height = spec.tile_height;
                lower.tile_depth = 1;
                levelSpec = lower;
            }
            ok = out->open(path.string(), levelSpec, l == 0 ? OIIO::ImageOutput::Create
                                                             : OIIO::ImageOutput::AppendMIPLevel);
            if (!ok) {
                if (error) *error = file.failed() ? file.failure() : out->geterror();
                break;
            }
            ok = WriteLevelTiles(*out, levelSpec, level, file, error);
        }
        if (ok && !out->close()) {
            if (error) *error = out->geterror();
            ok = false;
        }
        // Destroying the writer writes the tile offset table.
        out.reset();
        if (ok && !file.finish()) {
            if (error) *error = file.failure();
            ok = false;
        }
        return ok;
    } catch (std::bad_alloc const &) {
        if (error) *error = "out of memory writing " + path.string();
        return false;
    } catch (std::exception const &failure) {
        if (error) *error = failure.what();
        return false;
    }
}

bool WriteImageSafely(std::filesystem::path const &path, OIIO::ImageSpec const &spec, void const *pixels,
                      OIIO::TypeDesc format, std::string *error)
{
    try {
        auto out = OIIO::ImageOutput::create(path.string());
        if (!out) {
            if (error) *error = "no image writer for " + path.string() + ": " + OIIO::geterror();
            return false;
        }
        SafeOutputFile file(path);
        if (file.failed()) {
            if (error) *error = file.failure();
            return false;
        }
        if (!out->set_ioproxy(&file)) {
            if (error) *error = "image writer for " + path.string() + " cannot write through a stream";
            return false;
        }
        bool ok = out->open(path.string(), spec);
        const OIIO::stride_t xstride = static_cast<OIIO::stride_t>(spec.nchannels * format.size());
        const OIIO::stride_t ystride = xstride * spec.width;
        if (ok) {
            // One chunk at a time, so nothing is written after a failure.
            const int step = spec.tile_height ? spec.tile_height : 16;
            for (int y = 0; y < spec.height && ok; y += step) {
                const int yend = std::min(y + step, spec.height);
                const auto *row = static_cast<const char *>(pixels) + static_cast<size_t>(y) * ystride;
                ok = spec.tile_width
                    ? out->write_tiles(spec.x, spec.x + spec.width, spec.y + y, spec.y + yend, 0, 1, format, row,
                                       xstride, ystride)
                    : out->write_scanlines(spec.y + y, spec.y + yend, 0, format, row, xstride);
                ok = ok && !file.failed();
            }
        }
        if (!ok && error) *error = file.failed() ? file.failure() : out->geterror();
        if (ok && !out->close()) {
            if (error) *error = out->geterror();
            ok = false;
        }
        out.reset();
        if (ok && !file.finish()) {
            if (error) *error = file.failure();
            ok = false;
        }
        return ok;
    } catch (std::exception const &failure) {
        if (error) *error = failure.what();
        return false;
    }
}

} // namespace prism::texture_writer
