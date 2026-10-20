#include "texture_gpu_file.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace prism::gpu_texture {
namespace {

constexpr char kMagic[8] = {'P', 'R', 'I', 'S', 'M', 'G', 'T', '1'};
constexpr uint32_t kVersion = 2;
constexpr size_t kDigestBlock = size_t(1) << 20;

constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;

inline uint64_t Rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
inline uint64_t Round(uint64_t acc, uint64_t word) { return Rotl(acc + word * P2, 31) * P1; }
inline uint64_t Mix(uint64_t h)
{
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    return h ^ (h >> 32);
}

uint64_t BlockDigest(uint8_t const *data, size_t size, uint64_t seed)
{
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        uint64_t w[4];
        std::memcpy(w, data + i, 32);
        a = Round(a, w[0]);
        b = Round(b, w[1]);
        c = Round(c, w[2]);
        d = Round(d, w[3]);
    }
    uint64_t h = Rotl(a, 1) + Rotl(b, 7) + Rotl(c, 12) + Rotl(d, 18) + size;
    for (; i < size; ++i) h = Rotl(h ^ (uint64_t(data[i]) * P3), 11) * P1;
    return Mix(h);
}

#ifdef _WIN32
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
#endif

bool ReadExactly(std::filesystem::path const &path, uint64_t offset, void *destination, uint64_t bytes,
                 uint64_t *fileBytes)
{
#ifdef _WIN32
    File file;
    file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size)) return false;
    if (fileBytes) *fileBytes = uint64_t(size.QuadPart);
    LARGE_INTEGER at{};
    at.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(file.handle, at, nullptr, FILE_BEGIN)) return false;
    auto *out = static_cast<char *>(destination);
    uint64_t done = 0;
    while (done < bytes) {
        const DWORD chunk = DWORD(std::min<uint64_t>(bytes - done, uint64_t(64) << 20));
        DWORD read = 0;
        if (!ReadFile(file.handle, out + done, chunk, &read, nullptr) || read == 0) return false;
        done += read;
    }
    return true;
#else
    std::unique_ptr<FILE, int (*)(FILE *)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) return false;
    if (fileBytes) {
        if (fseeko(file.get(), 0, SEEK_END) != 0) return false;
        *fileBytes = uint64_t(ftello(file.get()));
    }
    if (fseeko(file.get(), off_t(offset), SEEK_SET) != 0) return false;
    return std::fread(destination, 1, size_t(bytes), file.get()) == size_t(bytes);
#endif
}

bool WriteDurably(std::filesystem::path const &path, Header const &header, void const *payload)
{
#ifdef _WIN32
    File file;
    file.handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) return false;
    auto write = [&](void const *data, uint64_t bytes) {
        auto const *in = static_cast<char const *>(data);
        uint64_t done = 0;
        while (done < bytes) {
            const DWORD chunk = DWORD(std::min<uint64_t>(bytes - done, uint64_t(64) << 20));
            DWORD written = 0;
            if (!WriteFile(file.handle, in + done, chunk, &written, nullptr) || written == 0) return false;
            done += written;
        }
        return true;
    };
    return write(&header, sizeof(header)) && write(payload, header.payloadBytes) && FlushFileBuffers(file.handle);
#else
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        std::fwrite(payload, 1, size_t(header.payloadBytes), file) == size_t(header.payloadBytes) &&
        std::fflush(file) == 0;
    ok = fsync(fileno(file)) == 0 && ok;
    return std::fclose(file) == 0 && ok;
#endif
}

bool Valid(Header const &h, std::string *error)
{
    auto fail = [&](char const *what) {
        if (error) *error = what;
        return false;
    };
    if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return fail("not a GPU texture file");
    if (h.version != kVersion || h.headerBytes != sizeof(Header)) return fail("GPU texture file version");
    if (h.width == 0 || h.height == 0 || h.channels < 1 || h.channels > 4) return fail("GPU texture size");
    if (h.component != Component::Unorm8 && h.component != Component::Unorm16 &&
        h.component != Component::Float16 && h.component != Component::Float32)
        return fail("GPU texture component");
    if (h.levelCount < 1 || h.levelCount > kMaxLevels || h.levelCount > FullChainLevels(h.width, h.height))
        return fail("GPU texture levels");
    uint64_t offset = 0;
    uint32_t w = h.width, hh = h.height;
    for (uint32_t l = 0; l < h.levelCount; ++l) {
        Level const &level = h.levels[l];
        const uint64_t bytes = uint64_t(w) * hh * h.channels * ComponentBytes(h.component);
        if (level.width != w || level.height != hh || level.offset != offset || level.bytes != bytes)
            return fail("GPU texture level table");
        offset += bytes;
        w = std::max(1u, w / 2);
        hh = std::max(1u, hh / 2);
    }
    if (h.payloadBytes != offset) return fail("GPU texture payload size");
    return true;
}

uint64_t TableDigest(Header const &h)
{
    uint64_t d = Mix(uint64_t(h.levelCount) ^ P3);
    for (uint32_t l = 0; l < h.levelCount; ++l) d = Mix(Round(d, h.levels[l].digest));
    return d;
}

// Checks the digests of levels [first, first + count) read to `data`.
bool CheckLevels(Header const &h, uint32_t first, uint32_t count, uint8_t const *data, std::string *error)
{
    std::atomic<bool> ok{true};
    const uint64_t base = h.levels[first].offset;
    tbb::parallel_for(first, first + count, [&](uint32_t l) {
        Level const &level = h.levels[l];
        if (Digest(data + (level.offset - base), size_t(level.bytes)) != level.digest) ok = false;
    });
    if (!ok && error) *error = "GPU texture digest mismatch";
    return ok;
}

// Bilinear weights of a vkCmdBlitImage destination texel along one axis:
// the two source texels around the center of destination texel x (clamped
// to the edge) and the weight of the second.
struct Tap {
    uint32_t i0, i1;
    float w;
};

std::vector<Tap> BlitTaps(uint32_t source, uint32_t destination)
{
    std::vector<Tap> taps(destination);
    for (uint32_t x = 0; x < destination; ++x) {
        const float u = (float(x) + 0.5f) * float(source) / float(destination) - 0.5f;
        const float base = std::floor(u);
        const int i0 = int(base);
        const float w = u - base;
        taps[x] = {uint32_t(std::clamp(i0, 0, int(source) - 1)), uint32_t(std::clamp(i0 + 1, 0, int(source) - 1)),
                   w};
    }
    return taps;
}

inline float Decode(Component c, uint8_t const *texel)
{
    switch (c) {
    // UNORM texels filter in their integer domain, so ties round exactly.
    case Component::Unorm8: return float(*texel);
    case Component::Unorm16: {
        uint16_t v;
        std::memcpy(&v, texel, 2);
        return float(v);
    }
    case Component::Float16: {
        uint16_t v;
        std::memcpy(&v, texel, 2);
        return HalfToFloat(v);
    }
    default: {
        float v;
        std::memcpy(&v, texel, 4);
        return v;
    }
    }
}

inline void Encode(Component c, float v, uint8_t *texel)
{
    switch (c) {
    case Component::Unorm8: *texel = uint8_t(std::lround(std::clamp(v, 0.0f, 255.0f))); break;
    case Component::Unorm16: {
        const uint16_t u = uint16_t(std::lround(std::clamp(v, 0.0f, 65535.0f)));
        std::memcpy(texel, &u, 2);
        break;
    }
    case Component::Float16: {
        const uint16_t h = FloatToHalf(v);
        std::memcpy(texel, &h, 2);
        break;
    }
    default: std::memcpy(texel, &v, 4);
    }
}

} // namespace

uint64_t Digest(void const *data, size_t bytes)
{
    auto const *in = static_cast<uint8_t const *>(data);
    const size_t blocks = (bytes + kDigestBlock - 1) / kDigestBlock;
    std::vector<uint64_t> digests(blocks);
    tbb::parallel_for(size_t(0), blocks, [&](size_t b) {
        const size_t begin = b * kDigestBlock;
        digests[b] = BlockDigest(in + begin, std::min(kDigestBlock, bytes - begin), b);
    });
    uint64_t h = Mix(bytes ^ P3);
    for (uint64_t d : digests) h = Mix(Round(h, d));
    return h;
}

uint32_t FullChainLevels(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    for (uint32_t size = std::max(width, height); size > 1; size /= 2) ++levels;
    return levels;
}

Header MakeHeader(uint32_t width, uint32_t height, uint32_t channels, Component component, uint32_t flags,
                  float peak, uint32_t levelCount)
{
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.headerBytes = sizeof(Header);
    h.width = width;
    h.height = height;
    h.channels = channels;
    h.component = component;
    const uint32_t full = std::min(kMaxLevels, FullChainLevels(width, height));
    h.levelCount = levelCount == 0 ? full : std::min(levelCount, full);
    h.flags = flags;
    h.peak = peak;
    uint64_t offset = 0;
    uint32_t w = width, hh = height;
    for (uint32_t l = 0; l < h.levelCount; ++l) {
        h.levels[l] = {offset, uint64_t(w) * hh * channels * ComponentBytes(component), w, hh, 0};
        offset += h.levels[l].bytes;
        w = std::max(1u, w / 2);
        hh = std::max(1u, hh / 2);
    }
    h.payloadBytes = offset;
    return h;
}

std::vector<uint8_t> BuildMipChain(void const *level0, uint32_t width, uint32_t height, uint32_t channels,
                                   Component component)
{
    const Header h = MakeHeader(width, height, channels, component, 0, 0.0f, 0);
    std::vector<uint8_t> payload(size_t(h.payloadBytes));
    if (h.levels[0].bytes) std::memcpy(payload.data(), level0, size_t(h.levels[0].bytes));
    const uint32_t componentBytes = ComponentBytes(component);
    const uint32_t texelBytes = channels * componentBytes;
    for (uint32_t l = 1; l < h.levelCount; ++l) {
        Level const &src = h.levels[l - 1];
        Level const &dst = h.levels[l];
        const std::vector<Tap> tx = BlitTaps(src.width, dst.width), ty = BlitTaps(src.height, dst.height);
        uint8_t const *in = payload.data() + src.offset;
        uint8_t *out = payload.data() + dst.offset;
        tbb::parallel_for(tbb::blocked_range<uint32_t>(0, dst.height, 16),
            [&](tbb::blocked_range<uint32_t> const &rows) {
                for (uint32_t y = rows.begin(); y != rows.end(); ++y) {
                    Tap const &ry = ty[y];
                    uint8_t const *row0 = in + size_t(ry.i0) * src.width * texelBytes;
                    uint8_t const *row1 = in + size_t(ry.i1) * src.width * texelBytes;
                    for (uint32_t x = 0; x < dst.width; ++x) {
                        Tap const &rx = tx[x];
                        for (uint32_t c = 0; c < channels; ++c) {
                            const size_t o0 = size_t(rx.i0) * texelBytes + c * componentBytes;
                            const size_t o1 = size_t(rx.i1) * texelBytes + c * componentBytes;
                            const float top = Decode(component, row0 + o0) * (1.0f - rx.w) +
                                Decode(component, row0 + o1) * rx.w;
                            const float bottom = Decode(component, row1 + o0) * (1.0f - rx.w) +
                                Decode(component, row1 + o1) * rx.w;
                            Encode(component, top * (1.0f - ry.w) + bottom * ry.w,
                                   out + (size_t(y) * dst.width + x) * texelBytes + c * componentBytes);
                        }
                    }
                }
            });
    }
    return payload;
}

bool Write(std::filesystem::path const &path, Header header, void const *payload, std::string *error)
{
    if (!Valid(header, error)) return false;
    auto const *bytes = static_cast<uint8_t const *>(payload);
    tbb::parallel_for(uint32_t(0), header.levelCount, [&](uint32_t l) {
        header.levels[l].digest = Digest(bytes + header.levels[l].offset, size_t(header.levels[l].bytes));
    });
    header.digest = TableDigest(header);
    static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
    const auto process = uint64_t(_getpid());
#else
    const auto process = uint64_t(getpid());
#endif
    const auto temporary = std::filesystem::path(path.string() + "." + std::to_string(process) + "." +
                                                 std::to_string(++serial) + ".tmp");
    std::error_code ignored;
    if (!WriteDurably(temporary, header, payload)) {
        std::filesystem::remove(temporary, ignored);
        if (error) *error = "cannot write " + temporary.string();
        return false;
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError) {
        std::filesystem::remove(temporary, ignored);
        // Another process published the same content first.
        if (std::filesystem::exists(path, ignored)) return true;
        if (error) *error = "cannot publish " + path.string() + ": " + renameError.message();
        return false;
    }
    return true;
}

bool Read(std::filesystem::path const &path, Header *header, std::vector<uint8_t> *payload, std::string *error)
{
    Header h{};
    uint64_t fileBytes = 0;
    if (!ReadExactly(path, 0, &h, sizeof(h), &fileBytes)) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    if (!Valid(h, error)) return false;
    if (TableDigest(h) != h.digest) {
        if (error) *error = "GPU texture level table digest";
        return false;
    }
    if (fileBytes != sizeof(Header) + h.payloadBytes) {
        if (error) *error = "GPU texture file size";
        return false;
    }
    if (payload) {
        payload->resize(size_t(h.payloadBytes));
        if (!ReadExactly(path, sizeof(Header), payload->data(), h.payloadBytes, nullptr)) {
            if (error) *error = "cannot read " + path.string();
            return false;
        }
        if (!CheckLevels(h, 0, h.levelCount, payload->data(), error)) return false;
    }
    if (header) *header = h;
    return true;
}

bool ReadPayload(std::filesystem::path const &path, Header const &header, void *destination, std::string *error)
{
    uint64_t fileBytes = 0;
    if (!ReadExactly(path, sizeof(Header), destination, header.payloadBytes, &fileBytes) ||
        fileBytes != sizeof(Header) + header.payloadBytes) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    return CheckLevels(header, 0, header.levelCount, static_cast<uint8_t const *>(destination), error);
}

uint64_t LevelRangeBytes(Header const &header, uint32_t first, uint32_t count)
{
    if (count == 0 || first + count > header.levelCount) return 0;
    Level const &last = header.levels[first + count - 1];
    return last.offset + last.bytes - header.levels[first].offset;
}

bool ReadLevels(std::filesystem::path const &path, Header const &header, uint32_t first, uint32_t count,
                void *destination, std::string *error)
{
    if (count == 0 || first + count > header.levelCount) {
        if (error) *error = "GPU texture level range";
        return false;
    }
    uint64_t fileBytes = 0;
    if (!ReadExactly(path, sizeof(Header) + header.levels[first].offset, destination,
                     LevelRangeBytes(header, first, count), &fileBytes) ||
        fileBytes != sizeof(Header) + header.payloadBytes) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    return CheckLevels(header, first, count, static_cast<uint8_t const *>(destination), error);
}

uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu) return uint16_t(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    const int halfExponent = int(exponent) - 127 + 15;
    if (halfExponent >= 31) return uint16_t(sign | 0x7C00u);
    if (halfExponent <= 0) {
        if (halfExponent < -10) return uint16_t(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - halfExponent;
        uint32_t half = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half & 1u))) ++half;
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(halfExponent) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) ++half;
    return uint16_t(sign | half);
}

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = uint32_t(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits;
    if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalize.
            int e = -1;
            do {
                mantissa <<= 1;
                ++e;
            } while ((mantissa & 0x400u) == 0);
            bits = sign | (uint32_t(127 - 15 - e) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}


void FloatStatistics(float const *values, size_t count, uint32_t *flags, float *peak)
{
    struct Scan {
        bool quantized;
        float peak;
    };
    // The same test the GPU devices applied to the float .tx.exr texels.
    const Scan scan = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, count, size_t(1) << 16), Scan{true, 0.0f},
        [&](tbb::blocked_range<size_t> const &range, Scan s) {
            for (size_t p = range.begin(); p != range.end(); ++p) {
                const float v = values[p];
                if (s.quantized) {
                    const float k = v * 255.0f;
                    if (!(v >= 0.0f && v <= 1.0f) || std::abs(k - std::round(k)) > 1e-3f) s.quantized = false;
                }
                if (std::isfinite(v)) s.peak = std::max(s.peak, std::abs(v));
            }
            return s;
        },
        [](Scan a, Scan b) { return Scan{a.quantized && b.quantized, std::max(a.peak, b.peak)}; });
    *flags = scan.quantized ? kQuantized8 : 0u;
    *peak = scan.peak;
}

} // namespace prism::gpu_texture
