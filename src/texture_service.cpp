#include "texture_service.h"
#include "resource_limits.h"
#include "load_profile.h"
#include "texture_writer.h"
#include "texture_gpu_file.h"
#include "fingerprint_records.h"
#include "color/color_management.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/hash.h>
#include <OpenImageIO/filesystem.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace prism {
namespace {

std::mutex singletonMutex;
std::weak_ptr<TextureService> singleton;
std::atomic<uint64_t> texturePinSerial{0};
// Prepared resources released so far; a trim can only evict what was released.
std::atomic<uint64_t> releasedResources{0};

uint64_t elapsedNanoseconds(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}

// Between full trims the cache size is estimated from this process's own
// additions; the interval bounds how late growth by other processes is seen.
constexpr auto kDiskTrimInterval = std::chrono::seconds(30);

// OIIO rejects JPEGs whose first Photoshop resource is not IPTC ("Corrupted
// IPTC data"), although the image data is fine. Returns the file with every
// APP13 segment removed, or empty if it is not a JPEG, has none, or is malformed.
std::vector<unsigned char> jpegWithoutApp13(std::filesystem::path const &path)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>()};
    if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) return {};
    std::vector<unsigned char> out(data.begin(), data.begin() + 2);
    bool removed = false;
    size_t pos = 2;
    while (pos < data.size()) {
        if (pos + 4 > data.size() || data[pos] != 0xFF) return {};
        const unsigned char marker = data[pos + 1];
        if (marker == 0xFF) return {};
        const size_t length = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
        if (length < 2 || pos + 2 + length > data.size()) return {};
        const size_t end = pos + 2 + length;
        if (marker == 0xED) removed = true;
        else out.insert(out.end(), data.begin() + pos, data.begin() + end);
        pos = end;
        if (marker == 0xDA) {
            // Entropy-coded data follows the scan header; copy it verbatim.
            out.insert(out.end(), data.begin() + pos, data.end());
            return removed ? out : std::vector<unsigned char>{};
        }
    }
    return {};
}

std::filesystem::path cacheRoot()
{
    return prism::PersistentCacheRoot();
}

uint64_t physicalMemoryMB()
{
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) return status.ullTotalPhys >> 20;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0)
        return (static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize)) >> 20;
#endif
    return 0;
}

// OIIO's tile cache. Too small a cache for the scene's texture working set
// makes render threads wait on tile eviction and rereads: ALab rendered 21x
// slower with 512 MiB than with 4 GiB. The default is a sixteenth of physical
// memory, between 512 MiB and 4 GiB.
size_t memoryBudgetMB()
{
    const uint64_t fallback = std::clamp<uint64_t>(physicalMemoryMB() / 16, 512, 4096);
    return static_cast<size_t>(prism::ResourceLimit("PRISM_TEXTURE_CACHE_MB", fallback, 65536));
}

// Files the TextureSystem keeps open. OIIO's default of 100 makes a scene
// with thousands of textures close and reopen files on nearly every lookup,
// and each reopen is serialized inside OIIO. The limit stays well inside the
// process's file limit: OIIO reads through C stdio, which on Windows allows
// 512 streams unless raised.
int openFileLimit()
{
    int available = 0;
#ifdef _WIN32
    available = std::max(_getmaxstdio(), _setmaxstdio(8192));
#else
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
        available = limit.rlim_cur == RLIM_INFINITY ? 8192
            : static_cast<int>(std::min<rlim_t>(limit.rlim_cur, 8192));
#endif
    const int fallback = std::clamp(available - 512, 100, 7680);
    return static_cast<int>(prism::ResourceLimit("PRISM_TEXTURE_OPEN_FILES",
        static_cast<uint64_t>(fallback), 8192));
}

// Memory admitted to cold preparations at once. A preparation holds the
// decoded source, its float copy and the mip chain while it runs (about
// 2.5 x 4WHC; see resource_limits.h), so callers that prepare
// many images concurrently, such as a UDIM family, wait here instead of
// exhausting memory. An image larger than the whole budget runs alone.
class PreparationBudget {
public:
    static PreparationBudget &instance()
    {
        static PreparationBudget budget;
        return budget;
    }
    class Reservation {
    public:
        Reservation(PreparationBudget &budget, uint64_t bytes) : m_budget(budget), m_bytes(bytes)
        {
            std::unique_lock lock(m_budget.m_mutex);
            m_budget.m_changed.wait(lock, [&] {
                return m_budget.m_used == 0 || m_budget.m_used + m_bytes <= m_budget.m_limit;
            });
            m_budget.m_used += m_bytes;
        }
        ~Reservation()
        {
            {
                std::lock_guard lock(m_budget.m_mutex);
                m_budget.m_used -= m_bytes;
            }
            m_budget.m_changed.notify_all();
        }
        Reservation(Reservation const &) = delete;
        Reservation &operator=(Reservation const &) = delete;
    private:
        PreparationBudget &m_budget;
        uint64_t m_bytes;
    };
private:
    PreparationBudget()
    {
        const uint64_t physical = physicalMemoryMB();
        const uint64_t fallback = std::clamp<uint64_t>(physical / 8, 2048, 16384);
        m_limit = prism::ResourceLimit("PRISM_TEXTURE_PREPARE_MB", fallback, 1u << 20) << 20;
    }
    std::mutex m_mutex;
    std::condition_variable m_changed;
    uint64_t m_used = 0, m_limit = 0;
};

uint64_t processId()
{
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

double diskBudgetGB()
{
    return static_cast<double>(prism::TextureArtifactBudgetBytes()) /
        (1024.0 * 1024.0 * 1024.0);
}

std::string contentKey(std::filesystem::path const &path,
                       TexturePrepareOptions const &options,
                       std::string const &trustedFingerprint = {},
                       uint64_t generation = 0)
{
    OIIO::SHA1 hash;
    const std::string recipe = std::string("PrismTextureService-v5;OIIO-") +
        OIIO_VERSION_STRING + ";64;float;mips;noresize;openexr;";
    hash.append(recipe.data(), recipe.size());
    if (!trustedFingerprint.empty()) {
        hash.append(trustedFingerprint.data(), trustedFingerprint.size());
    } else {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read texture source: " + path.string());
        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) hash.append(buffer.data(), static_cast<size_t>(count));
        }
        if (!input.eof()) throw std::runtime_error("error reading texture source: " + path.string());
    }
    hash.append(&generation, sizeof(generation));
    hash.append(&options.gamma, sizeof(options.gamma));
    hash.append(&options.exposure, sizeof(options.exposure));
    hash.append(&options.firstChannel, sizeof(options.firstChannel));
    hash.append(&options.channelCount, sizeof(options.channelCount));
    hash.append(&options.wrapS, sizeof(options.wrapS));
    hash.append(&options.wrapT, sizeof(options.wrapT));
    hash.append(&options.alphaChannel, sizeof(options.alphaChannel));
    hash.append(&options.unassociateAlpha, sizeof(options.unassociateAlpha));
    auto appendString = [&hash](std::string const &value) {
        const uint64_t size = value.size();
        hash.append(&size, sizeof(size));
        hash.append(value.data(), value.size());
    };
    appendString(options.resolverIdentity);
    appendString(options.sourceVersion);
    appendString(options.colorSpace);
    // Absent before colour conversion: unconverted textures keep their digests.
    if (!options.workingSpace.empty()) appendString(options.workingSpace);
    return hash.digest();
}

// File time ticks as a 64-bit integer. libc++ stores file times as __int128
// nanoseconds, which streams and std::to_string do not accept.
long long fileTimeTicks(std::filesystem::file_time_type const &time)
{
    return static_cast<long long>(time.time_since_epoch().count());
}

std::string sourceStamp(std::filesystem::path const &path,
                        TexturePrepareOptions const &options, uint64_t generation)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot stat texture source: " + path.string());
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) throw std::runtime_error("cannot stat texture source time: " + path.string());
    std::ostringstream out;
    out << std::filesystem::weakly_canonical(path).generic_string() << '\n'
        << options.resolverIdentity << '\n' << options.sourceVersion << '\n'
        << size << '\n' << fileTimeTicks(modified) << '\n' << generation << '\n'
        << std::setprecision(9) << options.gamma << ',' << options.exposure << ','
        << options.firstChannel << ',' << options.channelCount << ','
        << static_cast<int>(options.wrapS) << ',' << static_cast<int>(options.wrapT) << ','
        << options.alphaChannel << ',' << options.unassociateAlpha << ',' << options.colorSpace;
    if (!options.workingSpace.empty()) out << '\n' << options.workingSpace;
    return out.str();
}

std::string fileMetadataKey(std::filesystem::path const &path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path);
    const auto size = std::filesystem::file_size(canonical, ec);
    if (ec) throw std::runtime_error("cannot stat trusted texture source: " + canonical.string());
    const auto modified = std::filesystem::last_write_time(canonical, ec);
    if (ec) throw std::runtime_error("cannot stat trusted texture source time: " + canonical.string());
    return canonical.generic_string() + "\n" + std::to_string(size) + "\n" +
        std::to_string(fileTimeTicks(modified));
}

std::string trustedFingerprintToken(TrustedTextureFingerprint const &fingerprint)
{
    std::string algorithm = fingerprint.algorithm;
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (algorithm == "sha1" && fingerprint.version == "1")
        return "sha1:v1:" + fingerprint.digest;
    return "trusted:" + algorithm + ":" + fingerprint.version + ":" + fingerprint.digest;
}

std::string rawSourceStamp(std::filesystem::path const &path,
                           TexturePrepareOptions const &options, uint64_t generation)
{
    return fileMetadataKey(path) + "\n" + options.resolverIdentity + "\n" +
        options.sourceVersion + "\n" + std::to_string(generation);
}

std::string &lookupError() noexcept
{
    thread_local std::string error;
    return error;
}

OIIO::TextureOpt::Wrap wrap(TextureWrap value)
{
    using W = OIIO::TextureOpt::Wrap;
    switch (value) {
    case TextureWrap::Black: return OIIO::TextureOpt::WrapBlack;
    case TextureWrap::Clamp: return OIIO::TextureOpt::WrapClamp;
    case TextureWrap::Periodic: return OIIO::TextureOpt::WrapPeriodic;
    case TextureWrap::Mirror: return OIIO::TextureOpt::WrapMirror;
    case TextureWrap::PeriodicPow2: return W::PeriodicPow2;
    case TextureWrap::Shadow: return OIIO::TextureOpt::WrapBlack;
    default: return OIIO::TextureOpt::WrapDefault;
    }
}

// Prepared textures are tiled, mipmapped OpenEXR files. They used to be named
// .tx, which OIIO reads with its TIFF plugin first and then by trying every
// other format: an expensive, serialized probe on each reopen. The .exr suffix
// selects the OpenEXR reader directly; legacy .tx entries are still trimmed.
constexpr char kPreparedSuffix[] = ".tx.exr";
// GPU-ready copies of prepared textures; see texture_gpu_file.h.
constexpr char kGpuSuffix[] = ".gpu";

bool isPreparedFile(std::filesystem::path const &path)
{
    const std::string name = path.filename().string();
    auto endsWith = [&](std::string_view suffix) {
        return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return path.extension() == ".tx" || endsWith(kPreparedSuffix) || endsWith(kGpuSuffix);
}

bool processAlive(unsigned long pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
    CloseHandle(process);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

// A temporary file a preparation writes before renaming it into place
// (<key>.<pid>.<serial>.tmp.exr, <key>.gpu.<pid>.<serial>.tmp) whose process
// has exited: a crash or a kill left it behind.
bool abandonedTemporary(std::string const &filename)
{
    std::vector<std::string> parts;
    for (size_t begin = 0;;) {
        const size_t dot = filename.find('.', begin);
        parts.push_back(filename.substr(begin, dot - begin));
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    const auto tmp = std::find(parts.begin(), parts.end(), "tmp");
    if (tmp == parts.end() || tmp - parts.begin() < 3) return false;
    const std::string &pid = *(tmp - 2);
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) return false;
    try { return !processAlive(std::stoul(pid)); } catch (...) { return false; }
}

// Returns the bytes of prepared textures left in the cache.
uintmax_t trimDiskCache(std::filesystem::path const &root, double maxGB,
                        std::unordered_map<std::string, bool> const &pinned,
                        OIIO::TextureSystem *system)
{
    const uintmax_t limit = static_cast<uintmax_t>(maxGB * 1024.0 * 1024.0 * 1024.0);
    struct Entry { std::filesystem::path path; uintmax_t bytes; std::filesystem::file_time_type time; };
    std::vector<Entry> entries;
    uintmax_t total = 0;
    std::error_code ec;
    for (auto const &item : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!item.is_regular_file(ec) || !isPreparedFile(item.path())) continue;
        const auto size = item.file_size(ec);
        if (ec) { ec.clear(); continue; }
        total += size;
        entries.push_back({item.path(), size, item.last_write_time(ec)});
        ec.clear();
    }
    std::unordered_map<std::string, bool> processPinned;
    for (auto const &item : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!item.is_regular_file(ec)) { ec.clear(); continue; }
        const std::string filename = item.path().filename().string();
        if (abandonedTemporary(filename)) {
            std::filesystem::remove(item.path(), ec);
            ec.clear();
            continue;
        }
        const size_t marker = filename.rfind(".pin.");
        if (marker == std::string::npos) continue;
        bool active = true;
        try {
            const auto pid = static_cast<unsigned long>(std::stoul(filename.substr(marker + 5)));
#ifdef _WIN32
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
            if (process) {
                active = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
                CloseHandle(process);
            }
#else
            if (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) active = true;
            else if (errno == ESRCH) active = false;
#endif
        } catch (...) { active = false; }
        if (active) {
            processPinned.emplace(filename.substr(0, marker) + kPreparedSuffix, true);
            processPinned.emplace(filename.substr(0, marker) + kGpuSuffix, true);
            processPinned.emplace(filename.substr(0, marker) + ".tx", true);
        }
        else std::filesystem::remove(item.path(), ec);
        ec.clear();
    }
    if (total <= limit) return total;
    std::sort(entries.begin(), entries.end(), [](Entry const &a, Entry const &b) {
        return a.time < b.time;
    });
    for (auto const &entry : entries) {
        if (total <= limit) break;
        if (pinned.contains(entry.path.filename().string()) ||
            processPinned.contains(entry.path.filename().string())) continue;
        if (system) system->invalidate(OIIO::ustring(entry.path.string()), true);
        std::filesystem::remove(entry.path, ec);
        if (!ec) total -= entry.bytes;
        ec.clear();
    }
    return total;
}

class CrossProcessCacheLock {
public:
    explicit CrossProcessCacheLock(std::filesystem::path const &root,
                                   std::string const &identity = {})
    {
#ifdef _WIN32
        const std::string path = std::filesystem::absolute(root).lexically_normal().string() + "\\" + identity;
        const std::string digest = OIIO::SHA1::digest(path.data(), path.size());
        const std::wstring name = L"Local\\PrismTextureCache-" +
            std::wstring(digest.begin(), digest.end());
        m_handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!m_handle) throw std::runtime_error("cannot create texture cache mutex");
        const DWORD wait = WaitForSingleObject(m_handle, INFINITE);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(m_handle);
            m_handle = nullptr;
            throw std::runtime_error("cannot acquire texture cache mutex");
        }
#else
        const auto lockDir = root / ".locks";
        std::error_code ec;
        std::filesystem::create_directories(lockDir, ec);
        const auto path = identity.empty() ? root / ".prism-rt-cache.lock"
            : lockDir / (OIIO::SHA1::digest(identity.data(), identity.size()) + ".lock");
        m_fd = open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
            if (m_fd >= 0) close(m_fd);
            m_fd = -1;
            throw std::runtime_error("cannot acquire texture cache lock");
        }
#endif
    }
    ~CrossProcessCacheLock()
    {
#ifdef _WIN32
        if (m_handle) { ReleaseMutex(m_handle); CloseHandle(m_handle); }
#else
        if (m_fd >= 0) { flock(m_fd, LOCK_UN); close(m_fd); }
#endif
    }
    CrossProcessCacheLock(CrossProcessCacheLock const &) = delete;
    CrossProcessCacheLock &operator=(CrossProcessCacheLock const &) = delete;
private:
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#else
    int m_fd = -1;
#endif
};

// A texture source opened for inspection. JPEG files with Photoshop APP13
// segments OIIO rejects are read from a copy without them.
struct SourceImage {
    std::unique_ptr<OIIO::ImageInput> input;
    OIIO::ImageSpec spec;
    std::string format;
    std::vector<unsigned char> strippedJpeg;
    std::unique_ptr<OIIO::Filesystem::IOMemReader> strippedReader;
    void closeInspected()
    {
        if (input) input->close();
        input.reset();
    }
};

SourceImage OpenSource(std::filesystem::path const &absolute)
{
    SourceImage source;
    source.input = OIIO::ImageInput::open(absolute.string());
    if (!source.input) {
        const std::string inspectError = OIIO::geterror();
        source.strippedJpeg = jpegWithoutApp13(absolute);
        if (!source.strippedJpeg.empty()) {
            source.strippedReader = std::make_unique<OIIO::Filesystem::IOMemReader>(
                source.strippedJpeg.data(), source.strippedJpeg.size());
            source.input = OIIO::ImageInput::open(absolute.string(), nullptr, source.strippedReader.get());
            if (!source.input) (void)OIIO::geterror();
        }
        if (!source.input)
            throw std::runtime_error("cannot inspect texture " + absolute.string() + ": " + inspectError);
    }
    source.spec = source.input->spec();
    source.format = source.input->format_name();
    return source;
}

// What a cold preparation of `spec` holds at once (see PreparationBudget).
uint64_t PreparationBytes(OIIO::ImageSpec const &spec)
{
    return static_cast<uint64_t>(std::max(spec.width, spec.full_width)) *
        static_cast<uint64_t>(std::max(spec.height, spec.full_height)) *
        static_cast<uint64_t>(spec.nchannels) * sizeof(float) * 5 / 2;
}

// A source decoded for preparation: the normalized float texels the
// prepared .tx.exr stores and, when the GPU copy keeps the source's own
// storage, those texels in it.
struct DecodedImage {
    int width = 0, height = 0, channels = 0;
    OIIO::ImageSpec normalizedSpec;
    std::vector<float> pixels;
    gpu_texture::Component component = gpu_texture::Component::Float32;
    std::vector<uint8_t> gpuNative; // the GPU payload unless component is Float32
    uint32_t gpuFlags = 0;
    float gpuPeak = 0.0f;
    bool unassociateAlpha = false;
    uint64_t decodeNanoseconds = 0, pixelNanoseconds = 0;
};

DecodedImage DecodeSource(std::filesystem::path const &absolute, TexturePrepareOptions const &options,
                          SourceImage &source)
{
    DecodedImage result;
    OIIO::ImageSpec const &spec = source.spec;
    auto stageStart = std::chrono::steady_clock::now();
    std::unique_ptr<OIIO::ImageInput> decodeInput;
    if (source.input) {
        decodeInput = std::move(source.input);
    } else if (source.strippedReader) {
        OIIO::ImageSpec config;
        if (options.unassociateAlpha) config.attribute("oiio:UnassociatedAlpha", 1);
        // With a config, the reader takes its proxy from this hint.
        OIIO::Filesystem::IOProxy *proxy = source.strippedReader.get();
        config.attribute("oiio:ioproxy", OIIO::TypeDesc::PTR, &proxy);
        source.strippedReader->seek(0);
        decodeInput = OIIO::ImageInput::open(absolute.string(), &config, source.strippedReader.get());
        if (!decodeInput)
            throw std::runtime_error("cannot read texture " + absolute.string() + ": " + OIIO::geterror());
    } else {
        // Exactly what an image-cache backed ImageBuf read passes to the
        // reader: the cache's unassociatedalpha setting as an explicit
        // hint. Reading directly skips the cache's own header parse.
        OIIO::ImageSpec config;
        config.attribute("oiio:UnassociatedAlpha", options.unassociateAlpha ? 1 : 0);
        decodeInput = OIIO::ImageInput::open(absolute.string(), &config);
        if (!decodeInput)
            throw std::runtime_error("cannot read texture " + absolute.string() + ": " + OIIO::geterror());
    }
    // Decoded in the source's own storage when the GPU copy can keep it.
    // read_image converts to float with OIIO's convert_pixel_values after
    // decoding natively, so converting here gives the same floats.
    const OIIO::TypeDesc stored = spec.format;
    const bool keepNative = spec.channelformats.empty() &&
        (stored == OIIO::TypeDesc::UINT8 || stored == OIIO::TypeDesc::UINT16 || stored == OIIO::TypeDesc::HALF);
    const OIIO::TypeDesc decodeType = keepNative ? stored : OIIO::TypeDesc::FLOAT;
    const size_t values = static_cast<size_t>(spec.width) * spec.height * spec.nchannels;
    std::vector<float> sourcePixels(values);
    std::vector<uint8_t> native;
    if (keepNative) native.resize(values * decodeType.size());
    void *decodeBuffer = keepNative ? static_cast<void *>(native.data()) : sourcePixels.data();
    if (!decodeInput->read_image(0, 0, 0, spec.nchannels, decodeType, decodeBuffer)) {
        const std::string error = decodeInput->geterror();
        throw std::runtime_error("cannot decode texture pixels: " + error);
    }
    decodeInput->close();
    if (keepNative) {
        constexpr size_t chunk = size_t(1) << 18;
        tbb::parallel_for(size_t(0), (values + chunk - 1) / chunk, [&](size_t c) {
            const size_t begin = c * chunk, count = std::min(chunk, values - begin);
            OIIO::convert_pixel_values(decodeType, native.data() + begin * decodeType.size(),
                                       OIIO::TypeDesc::FLOAT, sourcePixels.data() + begin, static_cast<int>(count));
        });
    }
    result.decodeNanoseconds = elapsedNanoseconds(stageStart);
    stageStart = std::chrono::steady_clock::now();
    const int displayWidth = spec.full_width > 0 ? spec.full_width : spec.width;
    const int displayHeight = spec.full_height > 0 ? spec.full_height : spec.height;
    const int outChannels = options.channelCount ? options.channelCount
        : spec.nchannels - options.firstChannel;
    OIIO::ImageSpec normalizedSpec(displayWidth, displayHeight, outChannels, OIIO::TypeDesc::FLOAT);
    normalizedSpec.full_x = normalizedSpec.x = 0;
    normalizedSpec.full_y = normalizedSpec.y = 0;
    normalizedSpec.full_width = displayWidth;
    normalizedSpec.full_height = displayHeight;
    const int originX = spec.x - spec.full_x;
    const int originY = spec.y - spec.full_y;
    int sourceAlpha = options.alphaChannel;
    if (sourceAlpha == -1) {
        sourceAlpha = spec.alpha_channel;
        if (sourceAlpha < 0 && spec.nchannels == 2) sourceAlpha = 1;
    }
    // OIIO's unassociatedalpha read hint above keeps straight-alpha data
    // from being premultiplied during decode. PNG uses straight alpha by
    // format convention; other sources are governed by explicit metadata.
    const bool inputUnassociated =
        spec.get_int_attribute("oiio:UnassociatedAlpha", 0) != 0 || source.format == "png";
    const bool unassociateAlpha = options.unassociateAlpha && sourceAlpha >= 0 && !inputUnassociated;
    const int outputAlpha = sourceAlpha >= options.firstChannel &&
        sourceAlpha < options.firstChannel + outChannels
            ? sourceAlpha - options.firstChannel : -1;
    normalizedSpec.alpha_channel = outputAlpha;
    if (outputAlpha >= 0 && (unassociateAlpha || inputUnassociated))
        normalizedSpec.attribute("oiio:UnassociatedAlpha", 1);
    const std::string sourceColorSpace = options.colorSpace.empty()
        ? std::string(spec.get_string_attribute("oiio:ColorSpace", "")) : options.colorSpace;
    // Colour conversion into the working space (TexturePrepareOptions::workingSpace).
    std::shared_ptr<const prism::color::ColorManagement> conversion;
    if (!options.workingSpace.empty() && !options.colorSpace.empty()) {
        conversion = options.colorManagement;
        if (!conversion || conversion->identity() != options.workingSpace)
            throw std::runtime_error("texture working space has no matching colour management: " +
                                     absolute.string());
        if (!conversion->NeedsConversion(options.colorSpace)) {
            if (conversion->Resolve(options.colorSpace).empty())
                throw std::runtime_error("unknown texture colour space '" + options.colorSpace + "': " +
                                         absolute.string());
            conversion.reset();
        }
    }
    if (conversion)
        normalizedSpec.attribute("oiio:ColorSpace", conversion->workingSpace());
    else if (!sourceColorSpace.empty())
        normalizedSpec.attribute("oiio:ColorSpace", sourceColorSpace);
    normalizedSpec.channelnames.clear();
    for (int c = 0; c < outChannels; ++c) {
        const int sourceChannel = options.firstChannel + c;
        normalizedSpec.channelnames.push_back(sourceChannel < spec.nchannels
            ? std::string(spec.channel_name(sourceChannel)) : "channel" + std::to_string(c));
    }
    // Without a channel, window or value change the loop below would
    // copy every decoded value unchanged; use the decoded buffer instead.
    const bool identity = !conversion && !unassociateAlpha && options.gamma == 1.0f &&
        options.exposure == 0.0f && options.firstChannel == 0 &&
        outChannels == spec.nchannels && originX == 0 && originY == 0 &&
        spec.width == displayWidth && spec.height == displayHeight;
    std::vector<float> outputPixels;
    if (!identity) {
        outputPixels.assign(static_cast<size_t>(displayWidth) * displayHeight * outChannels, 0.0f);
        const int alpha = sourceAlpha;
        const int colorChannels = std::max(1, std::min(3, spec.nchannels - (alpha >= 0 ? 1 : 0)));
        const float gain = std::exp2(options.exposure);
        // Every source row writes its own destination row with the same
        // per-value arithmetic, so the result does not depend on threads.
        tbb::parallel_for(tbb::blocked_range<int>(0, spec.height),
            [&](tbb::blocked_range<int> const &rows) {
            for (int srcY = rows.begin(); srcY < rows.end(); ++srcY) {
                const int dstY = originY + srcY;
                if (dstY < 0 || dstY >= displayHeight) continue;
                for (int srcX = 0; srcX < spec.width; ++srcX) {
                    const int dstX = originX + srcX;
                    if (dstX < 0 || dstX >= displayWidth) continue;
                    const size_t p = static_cast<size_t>(srcY) * spec.width + srcX;
                    for (int c = 0; c < outChannels; ++c) {
                        const int sourceChannel = options.firstChannel + c;
                        if (sourceChannel >= spec.nchannels) continue;
                        float value = sourcePixels[p * spec.nchannels + sourceChannel];
                        const bool color = sourceChannel != alpha &&
                            sourceChannel - (alpha >= 0 && sourceChannel > alpha ? 1 : 0) < colorChannels;
                        if (color) {
                            if (unassociateAlpha) {
                                const float alphaValue = sourcePixels[p * spec.nchannels + alpha];
                                value = alphaValue != 0.0f ? value / alphaValue : 0.0f;
                            }
                            if (options.gamma != 1.0f)
                                value = std::pow(std::max(0.0f, value), options.gamma);
                            if (options.exposure != 0.0f) value *= gain;
                        }
                        outputPixels[(static_cast<size_t>(dstY) * displayWidth + dstX) * outChannels + c] = value;
                    }
                }
            }
        });
        std::vector<float>().swap(sourcePixels);
        // RGB in the first three output channels (alpha, if any, after them).
        if (conversion && outChannels >= 3 && (outputAlpha < 0 || outputAlpha >= 3))
            conversion->ToWorkingSpace(options.colorSpace, outputPixels.data(),
                                  static_cast<size_t>(displayWidth) * displayHeight, outChannels);
    }
    result.pixels = identity ? std::move(sourcePixels) : std::move(outputPixels);
    // The GPU copy: unchanged 8- and 16-bit integer and half texels keep
    // their storage. 16-bit texels that are all multiples of 257 hold 8-bit
    // values (k * 257 / 65535 == k / 255), which 8 bits store exactly.
    if (identity && keepNative && decodeType == OIIO::TypeDesc::UINT16) {
        auto const *words = reinterpret_cast<uint16_t const *>(native.data());
        const bool eightBit = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, values, size_t(1) << 16), true,
            [&](tbb::blocked_range<size_t> const &range, bool all) {
                for (size_t i = range.begin(); all && i != range.end(); ++i) all = words[i] % 257 == 0;
                return all;
            },
            [](bool a, bool b) { return a && b; });
        if (eightBit) {
            std::vector<uint8_t> bytes(values);
            tbb::parallel_for(size_t(0), values, [&](size_t i) { bytes[i] = uint8_t(words[i] / 257); });
            native = std::move(bytes);
            result.component = gpu_texture::Component::Unorm8;
        } else {
            result.component = gpu_texture::Component::Unorm16;
        }
        result.gpuNative = std::move(native);
    } else if (identity && keepNative && decodeType == OIIO::TypeDesc::UINT8) {
        result.component = gpu_texture::Component::Unorm8;
        result.gpuNative = std::move(native);
    } else if (identity && keepNative && decodeType == OIIO::TypeDesc::HALF) {
        result.component = gpu_texture::Component::Float16;
        result.gpuNative = std::move(native);
    }
    if (result.component == gpu_texture::Component::Float16 || result.component == gpu_texture::Component::Float32)
        gpu_texture::FloatStatistics(result.pixels.data(), result.pixels.size(), &result.gpuFlags, &result.gpuPeak);
    result.width = displayWidth;
    result.height = displayHeight;
    result.channels = outChannels;
    result.normalizedSpec = std::move(normalizedSpec);
    result.unassociateAlpha = unassociateAlpha;
    result.pixelNanoseconds = elapsedNanoseconds(stageStart);
    return result;
}

bool WriteGpuFile(std::filesystem::path const &path, DecodedImage const &decoded, std::string *error)
{
    // The whole mip chain, so devices can read only the levels they need.
    const auto width = static_cast<uint32_t>(decoded.width), height = static_cast<uint32_t>(decoded.height);
    const auto channels = static_cast<uint32_t>(decoded.channels);
    const auto header = gpu_texture::MakeHeader(width, height, channels, decoded.component, decoded.gpuFlags,
                                                decoded.gpuPeak, 0);
    const void *level0 = decoded.component == gpu_texture::Component::Float32
        ? static_cast<const void *>(decoded.pixels.data()) : static_cast<const void *>(decoded.gpuNative.data());
    const std::vector<uint8_t> payload = gpu_texture::BuildMipChain(level0, width, height, channels,
                                                                    decoded.component);
    return gpu_texture::Write(path, header, payload.data(), error);
}

// A prepared .tx.exr that a background writer is still producing: lookups
// and file readers of its resource wait for it.
struct PendingTexture {
    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false;
    std::string error;
    void finish(std::string failure)
    {
        {
            std::lock_guard lock(mutex);
            finished = true;
            error = std::move(failure);
        }
        changed.notify_all();
    }
    // The writer's error, empty on success.
    std::string const &wait()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return finished; });
        return error;
    }
    bool done()
    {
        std::lock_guard lock(mutex);
        return finished;
    }
};

// Threads writing prepared .tx.exr files after their GPU-ready copies were
// published, so a GPU device need not wait for them. At most
// Limits::TexturePreparationThreads() run at once; queued jobs hold their
// preparation-budget reservation, which bounds their memory. Destruction
// finishes every queued job.
// Writers run beside the decodes that feed them and the MDL compiles;
// fewer than the hardware threads made the budget, held by queued jobs, the
// bottleneck of cold loads. PRISM_TEXTURE_WRITER_THREADS overrides.
size_t WriterThreads()
{
    static const size_t count = static_cast<size_t>(prism::ResourceLimit("PRISM_TEXTURE_WRITER_THREADS",
        std::max(1u, std::thread::hardware_concurrency()), 256));
    return count;
}

class BackgroundWriters {
public:
    ~BackgroundWriters()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_work.notify_all();
        for (auto &thread : m_threads) thread.join();
    }
    void submit(std::function<void()> job)
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(job));
            if (m_idle == 0 && m_threads.size() < WriterThreads())
                m_threads.emplace_back([this] { run(); });
        }
        m_work.notify_one();
    }
private:
    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(m_mutex);
                ++m_idle;
                m_work.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
                --m_idle;
                if (m_queue.empty()) return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            job();
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_work;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_threads;
    size_t m_idle = 0;
    bool m_stopping = false;
};

// PRISM_TEXTURE_BACKGROUND_WRITES=0 writes each .tx.exr before preparation
// returns, as before GPU-ready copies.
bool BackgroundWritesEnabled()
{
    const char *value = std::getenv("PRISM_TEXTURE_BACKGROUND_WRITES");
    return !value || std::string_view(value) != "0";
}

} // namespace

struct TextureResource::Impl {
    std::filesystem::path source, prepared;
    int width = 0, height = 0, channels = 0;
    std::shared_ptr<OIIO::TextureSystem> system;
    std::shared_ptr<TextureService> serviceOwner;
    // Opened on first use (TextureService::readyHandle); an alias uses its
    // base's.
    std::atomic<OIIO::TextureSystem::TextureHandle *> handle{nullptr};
    std::once_flag handleOnce;
    std::string handleError;
    // Set while the .tx.exr is written in the background.
    std::shared_ptr<PendingTexture> pending;
    // An alias samples its base resource's OIIO handle, so it keeps the base
    // alive: otherwise the base could expire and a later preparation of the
    // same content would invalidate the file while the alias samples it.
    std::shared_ptr<const TextureResource> base;
    std::filesystem::path pinPath;
    ResourceVersion version;
    // The GPU-ready copy (texture_gpu_file.h), and what to write it from if
    // the cache lacks it: the source (empty for an in-memory one) and the
    // preparation options.
    std::filesystem::path gpu;
    TexturePrepareOptions options;
    ~Impl() {
        if (!pinPath.empty()) { std::error_code ec; std::filesystem::remove(pinPath, ec); }
        releasedResources.fetch_add(1, std::memory_order_relaxed);
    }
};

struct TextureService::Impl {
    std::shared_ptr<OIIO::TextureSystem> system;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<const TextureResource>> resources;
    std::unordered_map<std::string, std::weak_ptr<const TextureResource>> resourceAliases;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> pending;
    std::unordered_map<std::string, std::string> sourceFingerprints;
    std::unordered_map<std::string, std::string> trustedSourceFingerprints;
    std::unordered_map<std::string, uint64_t> generations;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> sourceGates;
    std::unordered_map<std::string, std::vector<std::string>> sourcePreparedKeys;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sourceKeyStamps;
    struct DiskUsage {
        uint64_t bytes = 0, added = 0, addedAtTrim = 0, releasedAtTrim = 0;
        std::chrono::steady_clock::time_point lastTrim;
        bool known = false;
    };
    // Per cache root; see kDiskTrimInterval.
    std::unordered_map<std::string, DiskUsage> diskUsage;
    std::atomic<uint64_t> prepareCalls{0}, memoryCacheHits{0}, diskCacheHits{0};
    std::atomic<uint64_t> resourceHashCalls{0}, sourceBytesHashed{0}, sourceWaitNanoseconds{0};
    std::atomic<uint64_t> contentWaitNanoseconds{0}, preparationNanoseconds{0};
    std::atomic<uint64_t> coldPreparations{0}, inspectNanoseconds{0}, decodeNanoseconds{0};
    std::atomic<uint64_t> pixelNanoseconds{0}, mipNanoseconds{0}, publishNanoseconds{0};
    std::atomic<uint64_t> trimNanoseconds{0}, lockWaitNanoseconds{0};
    std::atomic<uint64_t> gpuWriteNanoseconds{0}, gpuFilesWritten{0};
    std::atomic<uint64_t> aheadRequests{0}, aheadPreparations{0}, aheadMemoryHits{0};
    // Keys whose resource a prepareAhead call produced; guarded by mutex.
    std::unordered_set<std::string> aheadKeys;
    // .tx.exr files being written in the background, by content key, with
    // the size of their resources; guarded by mutex.
    struct InFlight {
        std::weak_ptr<PendingTexture> pending;
        int width = 0, height = 0, channels = 0;
    };
    std::unordered_map<std::string, InFlight> inFlight;
    std::atomic<uint64_t> backgroundWrites{0}, backgroundWaitNanoseconds{0};
    // Last: destroyed first, finishing its jobs while the rest is alive.
    BackgroundWriters writers;
};

TextureResource::TextureResource(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
int TextureResource::width() const noexcept { return m_impl->width; }
int TextureResource::height() const noexcept { return m_impl->height; }
int TextureResource::channels() const noexcept { return m_impl->channels; }
std::filesystem::path const &TextureResource::sourcePath() const noexcept { return m_impl->source; }
std::filesystem::path const &TextureResource::gpuCopyPath() const noexcept { return m_impl->gpu; }

std::filesystem::path const &TextureResource::preparedPath() const noexcept
{
    // Readers of the file need it finished (see PendingTexture).
    for (Impl const *impl = m_impl.get(); impl; impl = impl->base ? impl->base->m_impl.get() : nullptr)
        if (impl->pending) impl->pending->wait();
    return m_impl->prepared;
}
ResourceVersion const &TextureResource::resourceVersion() const noexcept { return m_impl->version; }

std::shared_ptr<TextureService> TextureService::Acquire()
{
    std::lock_guard lock(singletonMutex);
    if (auto current = singleton.lock()) return current;
    auto created = std::shared_ptr<TextureService>(new TextureService);
    singleton = created;
    return created;
}

TextureService::TextureService() : m_impl(std::make_unique<Impl>())
{
    m_impl->system = OIIO::TextureSystem::create(false);
    if (!m_impl->system) throw std::runtime_error("cannot create OIIO TextureSystem");
    if (!m_impl->system->attribute("max_memory_MB", static_cast<int>(memoryBudgetMB())))
        throw std::runtime_error("cannot configure OIIO texture cache budget");
    m_impl->system->attribute("max_open_files", openFileLimit());
}
TextureService::~TextureService() = default;

namespace {
std::mutex gPreparedObserversMutex;
auto &gPreparedObservers =
    *new std::map<int, std::shared_ptr<const TextureService::PreparedObserver>>();
int gNextPreparedObserver = 1;

// Tells the observers about a prepared resource with a GPU copy.
std::shared_ptr<const TextureResource> Published(std::shared_ptr<const TextureResource> resource)
{
    if (!resource || resource->gpuCopyPath().empty()) return resource;
    std::vector<std::shared_ptr<const TextureService::PreparedObserver>> observers;
    {
        std::lock_guard lock(gPreparedObserversMutex);
        for (auto const &[token, observer] : gPreparedObservers) observers.push_back(observer);
    }
    for (auto const &observer : observers) (*observer)(resource);
    return resource;
}
} // namespace

int TextureService::AddPreparedObserver(PreparedObserver observer)
{
    std::lock_guard lock(gPreparedObserversMutex);
    const int token = gNextPreparedObserver++;
    gPreparedObservers.emplace(token, std::make_shared<const PreparedObserver>(std::move(observer)));
    return token;
}

void TextureService::RemovePreparedObserver(int token)
{
    std::lock_guard lock(gPreparedObserversMutex);
    gPreparedObservers.erase(token);
}

std::shared_ptr<const TextureResource> TextureService::prepare(
    std::filesystem::path const &source, TexturePrepareOptions const &options)
{
    return Published(prepareImpl(source, options, {}));
}

std::shared_ptr<const TextureResource> TextureService::prepareTrusted(
    std::filesystem::path const &source,
    TrustedTextureFingerprint const &fingerprint,
    TexturePrepareOptions const &options)
{
    if (fingerprint.algorithm.empty() || fingerprint.version.empty() || fingerprint.digest.empty())
        throw std::invalid_argument("trusted texture fingerprint requires algorithm, version, and digest");
    registerTrustedSource(source, fingerprint);
    const std::string token = trustedFingerprintToken(fingerprint);
    return Published(prepareImpl(source, options, token));
}

std::shared_ptr<const TextureResource> TextureService::prepareAhead(
    std::filesystem::path const &source, TexturePrepareOptions const &options)
{
    return Published(prepareImpl(source, options, {}, true));
}

void TextureService::registerTrustedSource(std::filesystem::path const &source,
                                          TrustedTextureFingerprint const &fingerprint)
{
    if (fingerprint.algorithm.empty() || fingerprint.version.empty() || fingerprint.digest.empty())
        throw std::invalid_argument("trusted texture fingerprint requires algorithm, version, and digest");
    const std::string token = trustedFingerprintToken(fingerprint);
    const std::string metadataKey = fileMetadataKey(std::filesystem::absolute(source));
    std::lock_guard lock(m_impl->mutex);
    m_impl->trustedSourceFingerprints[metadataKey] = token;
}

std::string TextureService::sourceIdentity(std::filesystem::path const &source,
                                           TexturePrepareOptions const &options) const
{
    const auto absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(source));
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_impl->mutex);
        generation = m_impl->generations[absolute.generic_string()];
    }
    return sourceStamp(absolute, options, generation);
}

std::string TextureService::sourceIdentityTrusted(
    std::filesystem::path const &source,
    TrustedTextureFingerprint const &fingerprint,
    TexturePrepareOptions const &options) const
{
    if (fingerprint.algorithm.empty() || fingerprint.version.empty() || fingerprint.digest.empty())
        throw std::invalid_argument("trusted texture fingerprint requires algorithm, version, and digest");
    return sourceIdentity(source, options) + "\n" + trustedFingerprintToken(fingerprint);
}

bool TextureService::invalidate(std::filesystem::path const &source,
                                TexturePrepareOptions const &options)
{
    std::error_code pathError;
    auto absolute = std::filesystem::absolute(source,pathError);
    if (pathError) { pathError.clear(); absolute=source.lexically_normal(); }
    const auto canonical=std::filesystem::weakly_canonical(absolute,pathError);
    if (!pathError) absolute=canonical;
    else { pathError.clear(); absolute=absolute.lexically_normal(); }
    const std::string pathKey = absolute.generic_string();
    const auto root = cacheRoot();
    std::shared_ptr<std::mutex> sourceGate;
    {
        std::lock_guard lock(m_impl->mutex);
        auto &slot = m_impl->sourceGates[pathKey];
        if (!slot) slot = std::make_shared<std::mutex>();
        sourceGate = slot;
    }
    const auto sourceWaitStart = std::chrono::steady_clock::now();
    std::lock_guard sourceLock(*sourceGate);
    m_impl->sourceWaitNanoseconds.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - sourceWaitStart).count()), std::memory_order_relaxed);
    uint64_t oldGeneration = 0;
    { std::lock_guard lock(m_impl->mutex); oldGeneration = m_impl->generations[pathKey]; }
    std::string rawFingerprint;
    std::vector<std::string> keys;
    try {
        const std::string oldStamp = sourceStamp(absolute, options, oldGeneration);
        const std::string oldRawStamp = rawSourceStamp(absolute, options, oldGeneration);
        std::lock_guard lock(m_impl->mutex);
        if (auto it = m_impl->sourceFingerprints.find(oldRawStamp); it != m_impl->sourceFingerprints.end())
            rawFingerprint = it->second;
        if (auto it = m_impl->sourcePreparedKeys.find(pathKey); it != m_impl->sourcePreparedKeys.end())
            for (const auto &key : it->second) {
                auto sourceStamps = m_impl->sourceKeyStamps.find(pathKey);
                if (sourceStamps != m_impl->sourceKeyStamps.end()) {
                    auto stampIt = sourceStamps->second.find(key);
                    if (stampIt != sourceStamps->second.end()) {
                        bool matchesOptions = false;
                        for (uint64_t version = 0; version <= oldGeneration; ++version)
                            if (stampIt->second == sourceStamp(absolute, options, version)) {
                                matchesOptions = true;
                                break;
                            }
                        if (matchesOptions) keys.push_back(key);
                    }
                }
            }
        m_impl->generations[pathKey] = oldGeneration + 1;
        for (auto it = m_impl->sourceFingerprints.begin(); it != m_impl->sourceFingerprints.end();) {
            if (it->first.rfind(pathKey + "\n", 0) == 0) it = m_impl->sourceFingerprints.erase(it);
            else ++it;
        }
        for (auto it = m_impl->trustedSourceFingerprints.begin();
             it != m_impl->trustedSourceFingerprints.end();) {
            if (it->first.rfind(pathKey + "\n", 0) == 0) it = m_impl->trustedSourceFingerprints.erase(it);
            else ++it;
        }
    } catch (...) {
        std::lock_guard lock(m_impl->mutex);
        m_impl->generations[pathKey] = oldGeneration + 1;
        // A missing source has no current metadata stamp. Invalidate trusted
        // digests as well: a later replacement may deliberately restore the
        // old size and timestamp, but must still establish its own content.
        for (auto it = m_impl->sourceFingerprints.begin(); it != m_impl->sourceFingerprints.end();) {
            if (it->first.rfind(pathKey + "\n", 0) == 0) it = m_impl->sourceFingerprints.erase(it);
            else ++it;
        }
        for (auto it = m_impl->trustedSourceFingerprints.begin();
             it != m_impl->trustedSourceFingerprints.end();) {
            if (it->first.rfind(pathKey + "\n", 0) == 0) it = m_impl->trustedSourceFingerprints.erase(it);
            else ++it;
        }
    }
    if (keys.empty() && !rawFingerprint.empty())
        keys.push_back(contentKey(absolute, options, rawFingerprint, oldGeneration));
    if (keys.empty()) return false;
    {
        std::lock_guard lock(m_impl->mutex);
        for (const auto &key : keys) {
            if (auto found = m_impl->resources.find(key); found != m_impl->resources.end()) {
                if (!found->second.expired()) return false;
                m_impl->resources.erase(found);
            }
        }
    }
    std::error_code setupError;
    std::filesystem::create_directories(root,setupError);
    if (setupError) {
        std::cerr << "Texture cache invalidation could not access '" << root
                  << "': " << setupError.message() << '\n';
        return false;
    }
    try {
    CrossProcessCacheLock processLock(root);
    std::error_code ec;
    bool allRemoved = true;
    for (const std::string &key : keys) {
        const auto tx = root / (key + kPreparedSuffix);
        bool pinned = false;
        for (auto const &item : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            const auto filename = item.path().filename().string();
            const size_t marker = filename.rfind(".pin.");
            if (marker == std::string::npos || filename.substr(0, marker) != key) continue;
            bool active = true;
            try {
                const auto pid = static_cast<unsigned long>(std::stoul(filename.substr(marker + 5)));
#ifdef _WIN32
                HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
                if (process) { active = WaitForSingleObject(process, 0) != WAIT_OBJECT_0; CloseHandle(process); }
#else
                if (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) active = true;
                else if (errno == ESRCH) active = false;
#endif
            } catch (...) { active = false; }
            if (active) pinned = true;
        }
        if (pinned) { allRemoved = false; continue; }
        m_impl->system->invalidate(OIIO::ustring(tx.string()), true);
        if (std::filesystem::exists(tx) && !std::filesystem::remove(tx, ec)) allRemoved = false;
        ec.clear();
        const auto gpu = root / (key + kGpuSuffix);
        if (std::filesystem::exists(gpu) && !std::filesystem::remove(gpu, ec)) allRemoved = false;
        ec.clear();
    }
    return allRemoved;
    } catch (const std::exception& error) {
        // The source generation was already advanced above. Cache cleanup is
        // best-effort and must not make a missing source fatal to the renderer.
        std::cerr << "Texture cache invalidation failed for '" << absolute
                  << "': " << error.what() << '\n';
        return false;
    }
}

std::shared_ptr<const TextureResource> TextureService::prepareImpl(
    std::filesystem::path const &source, TexturePrepareOptions const &options,
    std::string const &trustedFingerprint, bool ahead)
{
    m_impl->prepareCalls.fetch_add(1, std::memory_order_relaxed);
    if (ahead) m_impl->aheadRequests.fetch_add(1, std::memory_order_relaxed);
    const auto preparationStart = std::chrono::steady_clock::now();
    struct Timer {
        std::atomic<uint64_t> &counter;
        std::chrono::steady_clock::time_point start;
        ~Timer() {
            counter.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
    } timer{m_impl->preparationNanoseconds, preparationStart};
    prism::load_profile::Scope profile("texture.prepare");
    profile.detail(source.filename().string() + " memory");
    if (!(options.gamma > 0.0f) || !std::isfinite(options.gamma) ||
        !std::isfinite(options.exposure) ||
        options.firstChannel < 0 || options.channelCount < 0 || options.alphaChannel < -2)
        throw std::invalid_argument("invalid texture preparation options");
    const auto absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(source));
    const std::string pathKey = absolute.generic_string();
    std::shared_ptr<std::mutex> sourceGate;
    {
        std::lock_guard lock(m_impl->mutex);
        auto &slot = m_impl->sourceGates[pathKey];
        if (!slot) slot = std::make_shared<std::mutex>();
        sourceGate = slot;
    }
    const auto sourceGateStart = std::chrono::steady_clock::now();
    std::lock_guard sourceLock(*sourceGate);
    m_impl->lockWaitNanoseconds.fetch_add(elapsedNanoseconds(sourceGateStart),
                                          std::memory_order_relaxed);
    uint64_t generation = 0;
    { std::lock_guard lock(m_impl->mutex); generation = m_impl->generations[pathKey]; }
    const std::string stamp = sourceStamp(absolute, options, generation);
    const std::string rawStamp = rawSourceStamp(absolute, options, generation);
    std::string rawFingerprint;
    if (!trustedFingerprint.empty()) rawFingerprint = trustedFingerprint;
    else {
        std::shared_ptr<std::mutex> fingerprintGate;
        {
            std::lock_guard lock(m_impl->mutex);
            if (auto trusted = m_impl->trustedSourceFingerprints.find(fileMetadataKey(absolute));
                trusted != m_impl->trustedSourceFingerprints.end())
                rawFingerprint = trusted->second;
            if (auto it = m_impl->sourceFingerprints.find(rawStamp); it != m_impl->sourceFingerprints.end())
                if (rawFingerprint.empty()) rawFingerprint = it->second;
            if (rawFingerprint.empty()) {
                auto &slot = m_impl->pending["fingerprint:" + stamp];
                if (!slot) slot = std::make_shared<std::mutex>();
                fingerprintGate = slot;
            }
        }
        if (fingerprintGate) {
            const auto sourceWaitStart = std::chrono::steady_clock::now();
            std::lock_guard fingerprintLock(*fingerprintGate);
            const uint64_t fingerprintWait = elapsedNanoseconds(sourceWaitStart);
            m_impl->sourceWaitNanoseconds.fetch_add(fingerprintWait, std::memory_order_relaxed);
            m_impl->lockWaitNanoseconds.fetch_add(fingerprintWait, std::memory_order_relaxed);
            {
                std::lock_guard lock(m_impl->mutex);
                if (auto it = m_impl->sourceFingerprints.find(rawStamp); it != m_impl->sourceFingerprints.end())
                    rawFingerprint = it->second;
            }
            // A source an earlier process hashed, unchanged since (see
            // fingerprint_records.h).
            const std::string recordIdentity = "texture-source\n" + rawStamp + "\n" +
                fingerprint_records::FileChangeStamp(absolute);
            if (rawFingerprint.empty()) {
                fingerprint_records::Record record;
                if (fingerprint_records::Read(cacheRoot(), recordIdentity, &record) &&
                    record.artifactStamp == fingerprint_records::FileStamp(absolute)) {
                    rawFingerprint = record.fingerprint;
                    std::lock_guard lock(m_impl->mutex);
                    m_impl->sourceFingerprints[rawStamp] = rawFingerprint;
                }
            }
            if (rawFingerprint.empty()) {
                // Hash once per stable metadata/version snapshot. A post-hash
                // stat check prevents publishing a digest for a changing file.
                m_impl->resourceHashCalls.fetch_add(1, std::memory_order_relaxed);
                std::ifstream input(absolute, std::ios::binary);
                if (!input) throw std::runtime_error("cannot read texture source: " + absolute.string());
                OIIO::SHA1 hash;
                std::array<char, 64 * 1024> buffer{};
                uint64_t bytes = 0;
                while (input) {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = input.gcount();
                    if (count > 0) { hash.append(buffer.data(), static_cast<size_t>(count)); bytes += static_cast<uint64_t>(count); }
                }
                if (!input.eof()) throw std::runtime_error("error reading texture source: " + absolute.string());
                if (rawSourceStamp(absolute, options, generation) != rawStamp)
                    throw std::runtime_error("texture source changed while fingerprinting: " + absolute.string());
                rawFingerprint = "sha1:v1:" + hash.digest();
                m_impl->sourceBytesHashed.fetch_add(bytes, std::memory_order_relaxed);
                fingerprint_records::Write(cacheRoot(), recordIdentity,
                    {rawFingerprint, absolute.filename().string(), fingerprint_records::FileStamp(absolute)});
                std::lock_guard lock(m_impl->mutex);
                m_impl->sourceFingerprints[rawStamp] = rawFingerprint;
            } else {
                std::lock_guard lock(m_impl->mutex);
                m_impl->sourceFingerprints[rawStamp] = rawFingerprint;
            }
        }
    }
    const std::string key = contentKey(absolute, options, rawFingerprint, generation);
    auto aliasForSource = [&](std::shared_ptr<const TextureResource> const &cached) {
        const bool embeddedSource = absolute.filename().string().rfind("embedded-", 0) == 0;
        const std::string aliasIdentity = key + "\n" + pathKey + "\n" + stamp;
        {
            std::lock_guard lock(m_impl->mutex);
            if (auto found = m_impl->resourceAliases.find(aliasIdentity);
                found != m_impl->resourceAliases.end())
                if (auto alias = found->second.lock()) return alias;
        }
        if (embeddedSource ||
            (cached->resourceVersion().resolvedIdentifier == absolute.generic_string() &&
            cached->resourceVersion().interpretationIdentity == stamp)
            )
            return cached;
        auto aliasImpl = std::make_shared<TextureResource::Impl>();
        aliasImpl->source = absolute;
        aliasImpl->prepared = cached->m_impl->prepared;
        aliasImpl->gpu = cached->m_impl->gpu;
        aliasImpl->options = cached->m_impl->options;
        aliasImpl->width = cached->width();
        aliasImpl->height = cached->height();
        aliasImpl->channels = cached->channels();
        aliasImpl->system = cached->m_impl->system;
        aliasImpl->serviceOwner = shared_from_this();
        aliasImpl->base = cached;
        aliasImpl->version = {absolute.generic_string(), stamp, rawFingerprint, generation};
        const auto preparedRoot = aliasImpl->prepared.parent_path();
        const auto pinWaitStart = std::chrono::steady_clock::now();
        CrossProcessCacheLock pinLock(preparedRoot);
        m_impl->lockWaitNanoseconds.fetch_add(elapsedNanoseconds(pinWaitStart),
                                              std::memory_order_relaxed);
        aliasImpl->pinPath = preparedRoot / (key + ".pin." + std::to_string(processId()) + "." +
                                              std::to_string(++texturePinSerial));
        {
            std::ofstream pin(aliasImpl->pinPath, std::ios::binary | std::ios::trunc);
            if (!pin) throw std::runtime_error("cannot pin aliased texture: " + aliasImpl->prepared.string());
            pin << aliasImpl->prepared.string();
        }
        auto alias = std::shared_ptr<const TextureResource>(new TextureResource(std::move(aliasImpl)));
        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->resourceAliases[aliasIdentity] = alias;
            auto &sourceKeys = m_impl->sourcePreparedKeys[pathKey];
            if (std::find(sourceKeys.begin(), sourceKeys.end(), key) == sourceKeys.end())
                sourceKeys.push_back(key);
            m_impl->sourceKeyStamps[pathKey][key] = stamp;
        }
        return alias;
    };
    std::shared_ptr<const TextureResource> cachedResource;
    {
        std::lock_guard lock(m_impl->mutex);
        if (auto found = m_impl->resources.find(key); found != m_impl->resources.end())
            if (auto resource = found->second.lock(); resource &&
                resource->m_impl->prepared.parent_path() == cacheRoot()) {
                m_impl->memoryCacheHits.fetch_add(1, std::memory_order_relaxed);
                if (!ahead && m_impl->aheadKeys.contains(key))
                    m_impl->aheadMemoryHits.fetch_add(1, std::memory_order_relaxed);
                cachedResource = std::move(resource);
            }
    }
    if (cachedResource) return aliasForSource(cachedResource);
    std::shared_ptr<std::mutex> gate;
    {
        std::lock_guard lock(m_impl->mutex);
        auto &slot = m_impl->pending[key];
        if (!slot) slot = std::make_shared<std::mutex>();
        gate = slot;
    }
    const std::weak_ptr<std::mutex> pendingGate = gate;
    auto pendingCleanup = std::unique_ptr<int, std::function<void(int *)>>(
        new int(0), [this, key, pendingGate](int *value) {
            delete value;
            std::lock_guard lock(m_impl->mutex);
            auto found = m_impl->pending.find(key);
            auto expected = pendingGate.lock();
            if (found != m_impl->pending.end() && found->second == expected &&
                found->second.use_count() <= 3)
                m_impl->pending.erase(found);
        });
    const auto contentWaitStart = std::chrono::steady_clock::now();
    std::lock_guard preparation(*gate);
    const uint64_t contentWait = elapsedNanoseconds(contentWaitStart);
    m_impl->contentWaitNanoseconds.fetch_add(contentWait, std::memory_order_relaxed);
    m_impl->lockWaitNanoseconds.fetch_add(contentWait, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_impl->mutex);
        if (auto found = m_impl->resources.find(key); found != m_impl->resources.end())
            if (auto resource = found->second.lock(); resource &&
                resource->m_impl->prepared.parent_path() == cacheRoot()) {
                m_impl->memoryCacheHits.fetch_add(1, std::memory_order_relaxed);
                if (!ahead && m_impl->aheadKeys.contains(key))
                    m_impl->aheadMemoryHits.fetch_add(1, std::memory_order_relaxed);
                cachedResource = std::move(resource);
            }
    }
    if (cachedResource) return aliasForSource(cachedResource);

    const auto root = cacheRoot();
    std::filesystem::create_directories(root);
    auto lockWaitStart = std::chrono::steady_clock::now();
    CrossProcessCacheLock processLock(root, key);
    const auto tx = root / (key + kPreparedSuffix);
    auto publicationLock = std::make_unique<CrossProcessCacheLock>(root);
    m_impl->lockWaitNanoseconds.fetch_add(elapsedNanoseconds(lockWaitStart),
                                          std::memory_order_relaxed);
    // Owns the pin from the moment it is written, so any failure below
    // removes it again.
    auto impl = std::make_shared<TextureResource::Impl>();
    auto writePin = [&] {
        // Trims and invalidations skip pinned entries and hold the whole-root
        // lock while they decide. A pin written under that lock is therefore
        // all the entry needs; opening it afterwards needs no lock.
        impl->pinPath = root / (key + ".pin." + std::to_string(processId()) + "." +
                                std::to_string(++texturePinSerial));
        std::ofstream pin(impl->pinPath, std::ios::binary | std::ios::trunc);
        if (!pin) throw std::runtime_error("cannot pin prepared texture: " + tx.string());
        pin << tx.string();
    };
    uint64_t publishNanoseconds = 0;
    const bool cacheFilePresent = std::filesystem::exists(tx);
    profile.detail(source.filename().string() + (cacheFilePresent ? " disk" : " created"));
    auto stageStart = std::chrono::steady_clock::now();
    if (cacheFilePresent) {
        m_impl->diskCacheHits.fetch_add(1, std::memory_order_relaxed);
        writePin();
        publishNanoseconds += elapsedNanoseconds(stageStart);
    }
    publicationLock.reset();
    stageStart = std::chrono::steady_clock::now();
    SourceImage sourceImage = OpenSource(absolute);
    const OIIO::ImageSpec &spec = sourceImage.spec;
    const std::string sourceInputFormat = sourceImage.format;
    // The JPEG reader ignores the unassociated-alpha hint a decode passes, so
    // the inspected input decodes exactly what a reopened one would. Opening
    // a large JPEG parses all of its metadata and costs more than the decode.
    if (cacheFilePresent || sourceInputFormat != "jpeg") sourceImage.closeInspected();
    m_impl->inspectNanoseconds.fetch_add(elapsedNanoseconds(stageStart), std::memory_order_relaxed);
    if (options.firstChannel >= spec.nchannels)
        throw std::invalid_argument("texture channel range is outside source image");
    if (options.alphaChannel >= spec.nchannels)
        throw std::invalid_argument("texture alpha channel is outside source image");
    const auto gpuPath = root / (key + kGpuSuffix);
    int preparedWidth = 0, preparedHeight = 0, preparedChannels = 0;
    // A .tx.exr this process is still writing for the same content (its
    // resource expired meanwhile): wait on that writer instead of decoding
    // again.
    std::shared_ptr<PendingTexture> pendingWrite;
    if (!cacheFilePresent) {
        std::lock_guard lock(m_impl->mutex);
        if (auto found = m_impl->inFlight.find(key); found != m_impl->inFlight.end()) {
            if (auto live = found->second.pending.lock(); live && !live->done()) {
                pendingWrite = std::move(live);
                preparedWidth = found->second.width;
                preparedHeight = found->second.height;
                preparedChannels = found->second.channels;
            } else {
                m_impl->inFlight.erase(found);
            }
        }
    }
    if (pendingWrite) {
        writePin();
    } else if (!cacheFilePresent) {
        stageStart = std::chrono::steady_clock::now();
        auto reservation = std::make_shared<PreparationBudget::Reservation>(
            PreparationBudget::instance(), PreparationBytes(spec));
        m_impl->lockWaitNanoseconds.fetch_add(elapsedNanoseconds(stageStart), std::memory_order_relaxed);
        DecodedImage decoded = DecodeSource(absolute, options, sourceImage);
        m_impl->decodeNanoseconds.fetch_add(decoded.decodeNanoseconds, std::memory_order_relaxed);
        m_impl->pixelNanoseconds.fetch_add(decoded.pixelNanoseconds, std::memory_order_relaxed);
        // The GPU-ready copy, from the same decode (texture_gpu_file.h).
        stageStart = std::chrono::steady_clock::now();
        std::string gpuError;
        if (!gpu_texture::Read(gpuPath, nullptr, nullptr, nullptr)) {
            if (WriteGpuFile(gpuPath, decoded, &gpuError))
                m_impl->gpuFilesWritten.fetch_add(1, std::memory_order_relaxed);
            else
                std::cerr << "PrismRT: cannot write the GPU copy of " << absolute.filename().string() << ": "
                          << gpuError << '\n';
        }
        decoded.gpuNative = {};
        const uint64_t gpuNanosecondsThis = elapsedNanoseconds(stageStart);
        m_impl->gpuWriteNanoseconds.fetch_add(gpuNanosecondsThis, std::memory_order_relaxed);
        preparedWidth = decoded.width;
        preparedHeight = decoded.height;
        preparedChannels = decoded.channels;
        // The file make_texture would write for this image (a plain float
        // texture: black wrap modes in the file, box-filtered mips, 64x64
        // tiles), written by texture_writer instead; see texture_writer.h.
        // A lookup with the default wrap mode uses the file's.
        OIIO::ImageSpec textureSpec = decoded.normalizedSpec;
        textureSpec.tile_width = textureSpec.tile_height = 64;
        textureSpec.tile_depth = 1;
        textureSpec.attribute("compression", texture_writer::PreparedCompression());
        textureSpec.attribute("planarconfig", "contig");
        textureSpec.attribute("wrapmodes", "black,black");
        textureSpec.attribute("textureformat", "Plain Texture");
        textureSpec.attribute("fovcot", static_cast<float>(decoded.width) / static_cast<float>(decoded.height));
        textureSpec.attribute("openexr:roundingmode", 0);
        static std::atomic<uint64_t> serial{0};
        struct Job {
            std::shared_ptr<PreparationBudget::Reservation> reservation;
            std::vector<texture_writer::Level> levels;
            OIIO::ImageSpec spec;
            std::filesystem::path temp, tx, root, source;
            std::string detail;
            std::shared_ptr<PendingTexture> pending;
            std::shared_ptr<OIIO::TextureSystem> system;
        };
        auto job = std::make_shared<Job>();
        job->reservation = std::move(reservation);
        job->levels.resize(1);
        job->levels[0].width = decoded.width;
        job->levels[0].height = decoded.height;
        job->levels[0].pixels = std::move(decoded.pixels);
        job->spec = std::move(textureSpec);
        job->temp = root / (key + "." + std::to_string(processId()) + "." + std::to_string(++serial) + ".tmp.exr");
        job->tx = tx;
        job->root = root;
        job->source = absolute;
        job->pending = std::make_shared<PendingTexture>();
        job->system = m_impl->system;
        if (prism::load_profile::Enabled())
            job->detail = std::string("name=") + absolute.filename().string() +
                " format=" + sourceInputFormat + " type=" + spec.format.c_str() +
                " size=" + std::to_string(spec.width) + "x" + std::to_string(spec.height) +
                " channels=" + std::to_string(spec.nchannels) + " out_channels=" + std::to_string(decoded.channels) +
                " gamma=" + std::to_string(options.gamma) + " unassociate=" + std::to_string(decoded.unassociateAlpha) +
                " decode_ms=" + std::to_string(decoded.decodeNanoseconds / 1000000) +
                " pixel_ms=" + std::to_string(decoded.pixelNanoseconds / 1000000) +
                " gpu_ms=" + std::to_string(gpuNanosecondsThis / 1000000) +
                " gpu_component=" + std::to_string(static_cast<int>(decoded.component));
        // Mips, the .tx.exr and its publication. Runs on a background writer
        // unless PRISM_TEXTURE_BACKGROUND_WRITES=0.
        Impl *service = m_impl.get();
        const std::string usageKey = root.generic_string();
        auto write = [job, service, usageKey]() {
            const auto start = std::chrono::steady_clock::now();
            std::string error;
            try {
                texture_writer::BuildMipChain(job->levels, job->spec.nchannels);
                const uint64_t mipNanoseconds = elapsedNanoseconds(start);
                const auto writeStart = std::chrono::steady_clock::now();
                if (!texture_writer::WriteMipmappedExr(job->temp, job->spec, job->levels, &error))
                    throw std::runtime_error(error);
                const uint64_t writeNanoseconds = elapsedNanoseconds(writeStart);
                std::vector<texture_writer::Level>().swap(job->levels);
                std::error_code sizeError;
                const auto bytes = std::filesystem::file_size(job->temp, sizeError);
                {
                    CrossProcessCacheLock publication(job->root);
                    std::error_code ec;
                    std::filesystem::rename(job->temp, job->tx, ec);
                    if (ec) {
                        std::filesystem::remove(job->temp, ec);
                        if (!std::filesystem::exists(job->tx))
                            throw std::runtime_error("cannot publish prepared texture: " + ec.message());
                    }
                }
                // A previous file at this content path may have been evicted
                // and recreated while this process kept its TextureSystem
                // alive; nothing samples this one yet.
                job->system->invalidate(OIIO::ustring(job->tx.string()), true);
                service->mipNanoseconds.fetch_add(elapsedNanoseconds(start), std::memory_order_relaxed);
                {
                    std::lock_guard lock(service->mutex);
                    auto &usage = service->diskUsage[usageKey];
                    usage.bytes += sizeError ? 0 : bytes;
                    usage.added += sizeError ? 0 : bytes;
                }
                if (!job->detail.empty())
                    prism::load_profile::Mark("texture.cold", job->detail +
                        " make_ms=" + std::to_string(elapsedNanoseconds(start) / 1000000) +
                        " mip_s=" + std::to_string(mipNanoseconds / 1e9) +
                        " write_s=" + std::to_string(writeNanoseconds / 1e9) +
                        " hash_s=0 stats_s=0 tx_mb=" + std::to_string((sizeError ? 0 : bytes) >> 20));
            } catch (std::exception const &failure) {
                error = failure.what();
                std::error_code ignored;
                std::filesystem::remove(job->temp, ignored);
                std::cerr << "PrismRT: cannot prepare texture " << job->source.string() << ": " << error << '\n';
                if (error.empty()) error = "texture preparation failed";
            }
            job->reservation.reset();
            job->pending->finish(error);
        };
        pendingWrite = job->pending;
        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->inFlight[key] = {pendingWrite, preparedWidth, preparedHeight, preparedChannels};
        }
        // Trims and invalidations skip pinned entries, so the file this
        // writer publishes is pinned from the start.
        lockWaitStart = std::chrono::steady_clock::now();
        publicationLock = std::make_unique<CrossProcessCacheLock>(root);
        m_impl->lockWaitNanoseconds.fetch_add(elapsedNanoseconds(lockWaitStart), std::memory_order_relaxed);
        writePin();
        publicationLock.reset();
        if (BackgroundWritesEnabled()) {
            m_impl->backgroundWrites.fetch_add(1, std::memory_order_relaxed);
            m_impl->writers.submit(std::move(write));
        } else {
            write();
            if (!pendingWrite->error.empty())
                throw std::runtime_error("cannot prepare texture " + absolute.string() + ": " + pendingWrite->error);
            pendingWrite.reset();
        }
    }
    stageStart = std::chrono::steady_clock::now();
    if (!pendingWrite) {
        auto preparedInput = OIIO::ImageInput::open(tx.string());
        if (!preparedInput)
            throw std::runtime_error("cannot inspect prepared texture: " + OIIO::geterror());
        const OIIO::ImageSpec preparedSpec = preparedInput->spec();
        preparedInput->close();
        preparedWidth = preparedSpec.width;
        preparedHeight = preparedSpec.height;
        preparedChannels = preparedSpec.nchannels;
    }
    impl->source = absolute.filename().string().rfind("embedded-", 0) == 0
        ? std::filesystem::path{} : absolute;
    impl->prepared = tx;
    impl->gpu = gpuPath;
    impl->options = options;
    impl->width = preparedWidth;
    impl->height = preparedHeight;
    impl->channels = preparedChannels;
    impl->pending = pendingWrite;
    const bool embeddedSource = impl->source.empty();
    impl->version.resolvedIdentifier = embeddedSource ? "embedded:" + key : absolute.generic_string();
    impl->version.interpretationIdentity = embeddedSource ? key : stamp;
    impl->version.fingerprint = rawFingerprint;
    impl->version.generation = generation;
    impl->system = m_impl->system;
    impl->serviceOwner = shared_from_this();
    if (!pendingWrite) {
        // A file written just now (synchronously) may replace an evicted one
        // this process's TextureSystem still has open; an existing one may be
        // sampled by render threads right now, and invalidating it under them
        // clears its level table mid-lookup.
        if (!cacheFilePresent) m_impl->system->invalidate(OIIO::ustring(tx.string()), true);
        TextureResource probe(impl);
        if (!readyHandle(probe))
            throw std::runtime_error("cannot open prepared texture: " + impl->handleError);
    }
    std::error_code accessError;
    std::filesystem::last_write_time(tx, std::filesystem::file_time_type::clock::now(), accessError);
    std::filesystem::last_write_time(gpuPath, std::filesystem::file_time_type::clock::now(), accessError);
    publishNanoseconds += elapsedNanoseconds(stageStart);
    m_impl->publishNanoseconds.fetch_add(publishNanoseconds, std::memory_order_relaxed);
    uintmax_t addedBytes = 0;
    if (!cacheFilePresent) {
        m_impl->coldPreparations.fetch_add(1, std::memory_order_relaxed);
        std::error_code sizeError;
        addedBytes = std::filesystem::file_size(tx, sizeError);
        if (sizeError) addedBytes = 0;
    }
    const std::string resourceSourceKey = impl->source.generic_string();
    auto resource = std::shared_ptr<const TextureResource>(new TextureResource(std::move(impl)));
    const double budgetGB = diskBudgetGB();
    const auto limit = static_cast<uintmax_t>(budgetGB * 1024.0 * 1024.0 * 1024.0);
    const auto now = std::chrono::steady_clock::now();
    std::unordered_map<std::string, bool> pinned;
    bool trim = false;
    uint64_t addedBeforeTrim = 0;
    const std::string usageKey = root.generic_string();
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->resources[key] = resource;
        if (ahead) {
            m_impl->aheadKeys.insert(key);
            m_impl->aheadPreparations.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_impl->aheadKeys.erase(key);
        }
        if (!resourceSourceKey.empty()) {
            auto &sourceKeys = m_impl->sourcePreparedKeys[resourceSourceKey];
            if (std::find(sourceKeys.begin(), sourceKeys.end(), key) == sourceKeys.end())
                sourceKeys.push_back(key);
            m_impl->sourceKeyStamps[resourceSourceKey][key] = stamp;
        }
        // A full trim enumerates the whole cache directory. This process
        // tracks what it adds, so it trims as soon as its own additions cross
        // the budget; growth by other processes and stale pins of exited ones
        // are caught by the periodic trim within kDiskTrimInterval.
        auto &usage = m_impl->diskUsage[usageKey];
        usage.bytes += addedBytes;
        usage.added += addedBytes;
        // When live textures alone exceed the budget a trim cannot get under
        // it, and trimming again on every publication serializes preparation
        // on a full directory scan. Over budget, trim again only after this
        // process added a further slice of the budget, or once a resource was
        // released (it may now be evictable).
        const uint64_t released = releasedResources.load(std::memory_order_relaxed);
        const uint64_t trimStep = std::max<uint64_t>(limit / 32, 64ull * 1024 * 1024);
        trim = !usage.known || now - usage.lastTrim >= kDiskTrimInterval ||
            (usage.bytes > limit && (released != usage.releasedAtTrim ||
                                     usage.added - usage.addedAtTrim >= trimStep));
        if (trim) {
            usage.known = true;
            usage.lastTrim = now;
            usage.addedAtTrim = usage.added;
            usage.releasedAtTrim = released;
            addedBeforeTrim = usage.added;
            for (auto const &[resourceKey, weak] : m_impl->resources) {
                (void)resourceKey;
                if (auto live = weak.lock()) {
                    pinned.emplace(live->m_impl->prepared.filename().string(), true);
                    pinned.emplace(live->m_impl->gpu.filename().string(), true);
                }
            }
        }
    }
    if (trim) {
        stageStart = std::chrono::steady_clock::now();
        lockWaitStart = std::chrono::steady_clock::now();
        CrossProcessCacheLock trimLock(root);
        const uint64_t trimLockWait = elapsedNanoseconds(lockWaitStart);
        m_impl->lockWaitNanoseconds.fetch_add(trimLockWait, std::memory_order_relaxed);
        const uintmax_t remaining = trimDiskCache(root, budgetGB, pinned, m_impl->system.get());
        {
            // Files published while this trim ran may or may not be in
            // remaining; counting them again only makes the estimate high.
            std::lock_guard lock(m_impl->mutex);
            auto &usage = m_impl->diskUsage[usageKey];
            usage.bytes = remaining + (usage.added - addedBeforeTrim);
        }
        m_impl->trimNanoseconds.fetch_add(elapsedNanoseconds(stageStart) - trimLockWait,
                                          std::memory_order_relaxed);
    }
    return resource;
}

std::shared_ptr<const TextureResource> TextureService::prepare(
    OIIO::ImageBuf const &source, TexturePrepareOptions const &options)
{
    const auto hashStart = std::chrono::steady_clock::now();
    const auto root = cacheRoot();
    std::filesystem::create_directories(root);
    const OIIO::ImageSpec spec = source.spec();
    if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0 ||
        options.firstChannel < 0 || options.firstChannel >= spec.nchannels ||
        options.channelCount < 0 || options.alphaChannel < -2 ||
        options.alphaChannel >= spec.nchannels || !(options.gamma > 0.0f) ||
        !std::isfinite(options.gamma) || !std::isfinite(options.exposure))
        throw std::invalid_argument("invalid in-memory texture or preparation options");
    OIIO::SHA1 digest;
    m_impl->resourceHashCalls.fetch_add(1, std::memory_order_relaxed);
    const std::string recipe = "PrismTextureImageBufInput-v1;";
    digest.append(recipe.data(), recipe.size());
    const std::array<int, 7> geometry{spec.x, spec.y, spec.width, spec.height,
        spec.full_x, spec.full_y, spec.nchannels};
    digest.append(geometry.data(), sizeof(geometry));
    digest.append(&spec.full_width, sizeof(spec.full_width));
    digest.append(&spec.full_height, sizeof(spec.full_height));
    digest.append(&spec.alpha_channel, sizeof(spec.alpha_channel));
    const auto formatName = spec.format.c_str();
    digest.append(formatName, std::strlen(formatName));
    for (int c = 0; c < spec.nchannels; ++c) {
        const auto name = spec.channel_name(c);
        digest.append(name.data(), name.size());
        const char separator = '\0';
        digest.append(&separator, 1);
    }
    const bool sourceIsUnassociated = spec.get_int_attribute("oiio:UnassociatedAlpha", 0) != 0 ||
        source.file_format_name() == "png";
    const int sourceUnassociatedAlpha = sourceIsUnassociated ? 1 : 0;
    digest.append(&sourceUnassociatedAlpha, sizeof(sourceUnassociatedAlpha));
    const OIIO::ROI roi(spec.x, spec.x + spec.width, spec.y,
        spec.y + spec.height, 0, 1, 0, spec.nchannels);
    std::vector<float> sourcePixels(static_cast<size_t>(spec.width) * spec.height * spec.nchannels);
    if (!source.get_pixels(roi, OIIO::TypeDesc::FLOAT, sourcePixels.data()))
        throw std::runtime_error("cannot read in-memory texture pixels: " + source.geterror());
    m_impl->sourceBytesHashed.fetch_add(sourcePixels.size() * sizeof(float), std::memory_order_relaxed);
    digest.append(sourcePixels.data(), sourcePixels.size() * sizeof(float));
    const std::string key = "oiio-imagebuf:v1:" + digest.digest();
    m_impl->preparationNanoseconds.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - hashStart).count()),
        std::memory_order_relaxed);
    static std::atomic<uint64_t> serial{0};
    const auto materialized = root / ("embedded-" + std::to_string(processId()) + "-" +
        std::to_string(++serial) + ".exr");
    // The pixels read above, written in the source's own format (which the
    // OpenEXR writer stores as it would the ImageBuf's), through the
    // failure-safe writer: OIIO's ImageBuf::write keeps writing after a
    // failed chunk.
    OIIO::ImageSpec materializedSpec = spec;
    materializedSpec.tile_width = materializedSpec.tile_height = materializedSpec.tile_depth = 0;
    if (sourceIsUnassociated)
        materializedSpec.attribute("oiio:UnassociatedAlpha", 1);
    std::string materializeError;
    if (!texture_writer::WriteImageSafely(materialized, materializedSpec, sourcePixels.data(),
                                          OIIO::TypeDesc::FLOAT, &materializeError)) {
        std::error_code ignored;
        std::filesystem::remove(materialized, ignored);
        throw std::runtime_error("cannot materialize in-memory texture: " + materializeError);
    }
    try {
        auto resource = prepareImpl(materialized, options, key);
        std::error_code ec;
        std::filesystem::remove(materialized, ec);
        return resource;
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(materialized, ec);
        throw;
    }
}

bool TextureService::lookup(TextureResource const &resource, TextureLookup const &query,
                            int channelCount, float *result) const
{
    if (!result || channelCount <= 0 || query.firstChannel < 0) return false;
    OIIO::TextureOpt options;
    options.swrap = wrap(query.wrapS);
    options.twrap = wrap(query.wrapT);
    options.firstchannel = query.firstChannel;
    options.mipmode = query.filtering == TextureFiltering::Bilinear
        ? OIIO::TextureOpt::MipModeNoMIP : OIIO::TextureOpt::MipModeAniso;
    options.interpmode = query.nearest ? OIIO::TextureOpt::InterpClosest
                                       : OIIO::TextureOpt::InterpBilinear;
    auto *handle = readyHandle(resource);
    if (!handle) {
        lookupError() = resource.m_impl->base ? resource.m_impl->base->m_impl->handleError
                                               : resource.m_impl->handleError;
        return false;
    }
    if (resource.m_impl->system->texture(handle,
            resource.m_impl->system->get_perthread_info(), options, query.s, query.t,
            query.dsdx, query.dtdx, query.dsdy, query.dtdy, channelCount, result))
        return true;
    lookupError() = resource.m_impl->system->geterror();
    return false;
}

std::string const &TextureService::lastError() noexcept { return lookupError(); }

OIIO::TextureSystem::TextureHandle *TextureService::readyHandle(TextureResource const &resource) const
{
    // An alias samples its base's handle.
    TextureResource::Impl *impl = resource.m_impl.get();
    while (impl->base) impl = impl->base->m_impl.get();
    if (auto *handle = impl->handle.load(std::memory_order_acquire)) return handle;
    if (impl->pending) {
        const auto waitStart = std::chrono::steady_clock::now();
        std::string const &error = impl->pending->wait();
        m_impl->backgroundWaitNanoseconds.fetch_add(elapsedNanoseconds(waitStart), std::memory_order_relaxed);
        if (!error.empty()) {
            std::call_once(impl->handleOnce, [&] { impl->handleError = error; });
            return nullptr;
        }
    }
    std::call_once(impl->handleOnce, [&] {
        OIIO::TextureSystem &system = *impl->system;
        const OIIO::ustring name(impl->prepared.string());
        auto *handle = system.get_texture_handle(name);
        if (handle && !system.good(handle)) {
            // A file OIIO once failed to open stays broken until invalidated.
            // Nothing samples a broken file, so retrying it is safe.
            (void)system.geterror();
            system.invalidate(name, true);
            handle = system.get_texture_handle(name);
        }
        if (!handle || !system.good(handle)) {
            impl->handleError = "cannot open prepared texture " + impl->prepared.string() + ": " + system.geterror();
            return;
        }
        impl->handle.store(handle, std::memory_order_release);
    });
    return impl->handle.load(std::memory_order_acquire);
}

bool TextureService::readPixels(TextureResource const &resource, int xb, int yb,
                                int xe, int ye, int firstChannel, int channelCount,
                                float *result) const
{
    if (!result || xb < 0 || yb < 0 || xe > resource.width() || ye > resource.height() ||
        xe <= xb || ye <= yb || firstChannel < 0 || channelCount <= 0 ||
        firstChannel + channelCount > resource.channels()) return false;
    OIIO::TextureOpt options;
    auto *handle = readyHandle(resource);
    if (!handle) {
        lookupError() = "prepared texture unavailable";
        return false;
    }
    if (resource.m_impl->system->get_texels(handle,
            resource.m_impl->system->get_perthread_info(), options, 0,
            xb, xe, yb, ye, 0, 1, firstChannel, firstChannel + channelCount,
            OIIO::TypeDesc::FLOAT, result))
        return true;
    lookupError() = resource.m_impl->system->geterror();
    return false;
}

std::filesystem::path TextureService::gpuImage(TextureResource const &resource, std::string *error) const
{
    TextureResource::Impl const &impl = *resource.m_impl;
    if (impl.gpu.empty()) {
        if (error) *error = "no GPU copy for this texture";
        return {};
    }
    std::string readError;
    if (gpu_texture::Read(impl.gpu, nullptr, nullptr, &readError)) return impl.gpu;
    // A cache prepared before GPU copies existed, or a copy that was evicted
    // or damaged: write it from the source, if that is still what the
    // resource was prepared from.
    std::error_code ec;
    if (impl.source.empty() || !std::filesystem::is_regular_file(impl.source, ec)) {
        if (error) *error = readError + " (and no source file to write it from)";
        return {};
    }
    try {
        if (sourceStamp(impl.source, impl.options, impl.version.generation) != impl.version.interpretationIdentity) {
            if (error) *error = "the source changed since it was prepared";
            return {};
        }
        std::shared_ptr<std::mutex> gate;
        {
            std::lock_guard lock(m_impl->mutex);
            auto &slot = m_impl->pending["gpu:" + impl.gpu.generic_string()];
            if (!slot) slot = std::make_shared<std::mutex>();
            gate = slot;
        }
        std::lock_guard writing(*gate);
        if (gpu_texture::Read(impl.gpu, nullptr, nullptr, nullptr)) return impl.gpu;
        const auto start = std::chrono::steady_clock::now();
        SourceImage source = OpenSource(impl.source);
        if (source.format != "jpeg") source.closeInspected();
        PreparationBudget::Reservation reservation(PreparationBudget::instance(), PreparationBytes(source.spec));
        DecodedImage decoded = DecodeSource(impl.source, impl.options, source);
        if (decoded.width != impl.width || decoded.height != impl.height || decoded.channels != impl.channels) {
            if (error) *error = "the source no longer matches its prepared texture";
            return {};
        }
        if (!WriteGpuFile(impl.gpu, decoded, error)) return {};
        m_impl->gpuFilesWritten.fetch_add(1, std::memory_order_relaxed);
        m_impl->gpuWriteNanoseconds.fetch_add(elapsedNanoseconds(start), std::memory_order_relaxed);
        return impl.gpu;
    } catch (std::exception const &failure) {
        if (error) *error = failure.what();
        return {};
    }
}

OIIO::TextureSystem *TextureService::textureSystem() const noexcept { return m_impl->system.get(); }

TextureServiceStats TextureService::stats() const noexcept
{
    TextureServiceStats result;
    result.prepareCalls = m_impl->prepareCalls.load(std::memory_order_relaxed);
    result.memoryCacheHits = m_impl->memoryCacheHits.load(std::memory_order_relaxed);
    result.diskCacheHits = m_impl->diskCacheHits.load(std::memory_order_relaxed);
    result.resourceHashCalls = m_impl->resourceHashCalls.load(std::memory_order_relaxed);
    result.sourceBytesHashed = m_impl->sourceBytesHashed.load(std::memory_order_relaxed);
    result.sourceWaitNanoseconds = m_impl->sourceWaitNanoseconds.load(std::memory_order_relaxed);
    result.contentWaitNanoseconds = m_impl->contentWaitNanoseconds.load(std::memory_order_relaxed);
    result.preparationNanoseconds = m_impl->preparationNanoseconds.load(std::memory_order_relaxed);
    result.coldPreparations = m_impl->coldPreparations.load(std::memory_order_relaxed);
    result.inspectNanoseconds = m_impl->inspectNanoseconds.load(std::memory_order_relaxed);
    result.decodeNanoseconds = m_impl->decodeNanoseconds.load(std::memory_order_relaxed);
    result.pixelNanoseconds = m_impl->pixelNanoseconds.load(std::memory_order_relaxed);
    result.mipNanoseconds = m_impl->mipNanoseconds.load(std::memory_order_relaxed);
    result.publishNanoseconds = m_impl->publishNanoseconds.load(std::memory_order_relaxed);
    result.trimNanoseconds = m_impl->trimNanoseconds.load(std::memory_order_relaxed);
    result.lockWaitNanoseconds = m_impl->lockWaitNanoseconds.load(std::memory_order_relaxed);
    result.gpuWriteNanoseconds = m_impl->gpuWriteNanoseconds.load(std::memory_order_relaxed);
    result.gpuFilesWritten = m_impl->gpuFilesWritten.load(std::memory_order_relaxed);
    result.backgroundWrites = m_impl->backgroundWrites.load(std::memory_order_relaxed);
    result.backgroundWaitNanoseconds = m_impl->backgroundWaitNanoseconds.load(std::memory_order_relaxed);
    result.aheadRequests = m_impl->aheadRequests.load(std::memory_order_relaxed);
    result.aheadPreparations = m_impl->aheadPreparations.load(std::memory_order_relaxed);
    result.aheadMemoryHits = m_impl->aheadMemoryHits.load(std::memory_order_relaxed);
    return result;
}

std::string TextureService::textureStatistics() const
{
    return m_impl->system->getstats(2, true);
}

} // namespace prism
