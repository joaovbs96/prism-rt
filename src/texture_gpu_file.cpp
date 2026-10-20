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
constexpr uint32_t kVersion = 1;
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
    if (h.levelCount < 1 || h.levelCount > kMaxLevels) return fail("GPU texture levels");
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

Header MakeHeader(uint32_t width, uint32_t height, uint32_t channels, Component component, uint32_t flags,
                  float peak)
{
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.headerBytes = sizeof(Header);
    h.width = width;
    h.height = height;
    h.channels = channels;
    h.component = component;
    h.levelCount = 1;
    h.flags = flags;
    h.peak = peak;
    h.levels[0] = {0, uint64_t(width) * height * channels * ComponentBytes(component), width, height};
    h.payloadBytes = h.levels[0].bytes;
    return h;
}

bool Write(std::filesystem::path const &path, Header header, void const *payload, std::string *error)
{
    if (!Valid(header, error)) return false;
    header.digest = Digest(payload, size_t(header.payloadBytes));
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
        if (Digest(payload->data(), payload->size()) != h.digest) {
            if (error) *error = "GPU texture digest mismatch";
            return false;
        }
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
    if (Digest(destination, size_t(header.payloadBytes)) != header.digest) {
        if (error) *error = "GPU texture digest mismatch";
        return false;
    }
    return true;
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
