#include "texture_service.h"
#include "resource_limits.h"
#include "load_profile.h"

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/hash.h>
#include <OpenImageIO/filesystem.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <functional>
#include <chrono>
#include <cctype>
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

bool isPreparedFile(std::filesystem::path const &path)
{
    const std::string name = path.filename().string();
    const std::string_view suffix(kPreparedSuffix);
    return path.extension() == ".tx" ||
        (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
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

} // namespace

struct TextureResource::Impl {
    std::filesystem::path source, prepared;
    int width = 0, height = 0, channels = 0;
    std::shared_ptr<OIIO::TextureSystem> system;
    std::shared_ptr<TextureService> serviceOwner;
    OIIO::TextureSystem::TextureHandle *handle = nullptr;
    // An alias samples its base resource's OIIO handle, so it keeps the base
    // alive: otherwise the base could expire and a later preparation of the
    // same content would invalidate the file while the alias samples it.
    std::shared_ptr<const TextureResource> base;
    std::filesystem::path pinPath;
    ResourceVersion version;
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
    std::atomic<uint64_t> aheadRequests{0}, aheadPreparations{0}, aheadMemoryHits{0};
    // Keys whose resource a prepareAhead call produced; guarded by mutex.
    std::unordered_set<std::string> aheadKeys;
};

TextureResource::TextureResource(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
int TextureResource::width() const noexcept { return m_impl->width; }
int TextureResource::height() const noexcept { return m_impl->height; }
int TextureResource::channels() const noexcept { return m_impl->channels; }
std::filesystem::path const &TextureResource::sourcePath() const noexcept { return m_impl->source; }
std::filesystem::path const &TextureResource::preparedPath() const noexcept { return m_impl->prepared; }
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

std::shared_ptr<const TextureResource> TextureService::prepare(
    std::filesystem::path const &source, TexturePrepareOptions const &options)
{
    return prepareImpl(source, options, {});
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
    return prepareImpl(source, options, token);
}

std::shared_ptr<const TextureResource> TextureService::prepareAhead(
    std::filesystem::path const &source, TexturePrepareOptions const &options)
{
    return prepareImpl(source, options, {}, true);
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
        aliasImpl->prepared = cached->preparedPath();
        aliasImpl->width = cached->width();
        aliasImpl->height = cached->height();
        aliasImpl->channels = cached->channels();
        aliasImpl->system = cached->m_impl->system;
        aliasImpl->serviceOwner = shared_from_this();
        aliasImpl->handle = cached->m_impl->handle;
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
                resource->preparedPath().parent_path() == cacheRoot()) {
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
                resource->preparedPath().parent_path() == cacheRoot()) {
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
    auto sourceInput = OIIO::ImageInput::open(absolute.string());
    // Kept alive until the end of preparation; readers below borrow it.
    std::vector<unsigned char> strippedJpeg;
    std::unique_ptr<OIIO::Filesystem::IOMemReader> strippedReader;
    if (!sourceInput) {
        const std::string inspectError = OIIO::geterror();
        strippedJpeg = jpegWithoutApp13(absolute);
        if (!strippedJpeg.empty()) {
            strippedReader = std::make_unique<OIIO::Filesystem::IOMemReader>(
                strippedJpeg.data(), strippedJpeg.size());
            sourceInput = OIIO::ImageInput::open(absolute.string(), nullptr, strippedReader.get());
            if (!sourceInput) (void)OIIO::geterror();
        }
        if (!sourceInput)
            throw std::runtime_error("cannot inspect texture " + absolute.string() + ": " + inspectError);
    }
    const OIIO::ImageSpec spec = sourceInput->spec();
    const std::string sourceInputFormat = sourceInput->format_name();
    // The JPEG reader ignores the unassociated-alpha hint a decode passes, so
    // the inspected input decodes exactly what a reopened one would. Opening
    // a large JPEG parses all of its metadata and costs more than the decode.
    const bool decodeInspected = !cacheFilePresent && sourceInputFormat == "jpeg";
    if (!decodeInspected) sourceInput->close();
    m_impl->inspectNanoseconds.fetch_add(elapsedNanoseconds(stageStart), std::memory_order_relaxed);
    if (options.firstChannel >= spec.nchannels)
        throw std::invalid_argument("texture channel range is outside source image");
    if (options.alphaChannel >= spec.nchannels)
        throw std::invalid_argument("texture alpha channel is outside source image");
    if (!cacheFilePresent) {
        stageStart = std::chrono::steady_clock::now();
        std::vector<float> sourcePixels(static_cast<size_t>(spec.width) * spec.height * spec.nchannels);
        std::unique_ptr<OIIO::ImageInput> decodeInput;
        if (decodeInspected) {
            decodeInput = std::move(sourceInput);
        } else if (strippedReader) {
            OIIO::ImageSpec config;
            if (options.unassociateAlpha) config.attribute("oiio:UnassociatedAlpha", 1);
            // With a config, the reader takes its proxy from this hint.
            OIIO::Filesystem::IOProxy *proxy = strippedReader.get();
            config.attribute("oiio:ioproxy", OIIO::TypeDesc::PTR, &proxy);
            strippedReader->seek(0);
            decodeInput = OIIO::ImageInput::open(absolute.string(), &config, strippedReader.get());
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
        if (!decodeInput->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT,
                                     sourcePixels.data())) {
            const std::string error = decodeInput->geterror();
            throw std::runtime_error("cannot decode texture pixels: " + error);
        }
        decodeInput->close();
        m_impl->decodeNanoseconds.fetch_add(elapsedNanoseconds(stageStart), std::memory_order_relaxed);
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
            spec.get_int_attribute("oiio:UnassociatedAlpha", 0) != 0 ||
            sourceInputFormat == "png";
        const bool unassociateAlpha = options.unassociateAlpha && sourceAlpha >= 0 &&
            !inputUnassociated;
        const int outputAlpha = sourceAlpha >= options.firstChannel &&
            sourceAlpha < options.firstChannel + outChannels
                ? sourceAlpha - options.firstChannel : -1;
        normalizedSpec.alpha_channel = outputAlpha;
        if (outputAlpha >= 0 && (unassociateAlpha || inputUnassociated))
            normalizedSpec.attribute("oiio:UnassociatedAlpha", 1);
        const std::string sourceColorSpace = options.colorSpace.empty()
            ? std::string(spec.get_string_attribute("oiio:ColorSpace", "")) : options.colorSpace;
        if (!sourceColorSpace.empty())
            normalizedSpec.attribute("oiio:ColorSpace", sourceColorSpace);
        normalizedSpec.channelnames.clear();
        for (int c = 0; c < outChannels; ++c) {
            const int sourceChannel = options.firstChannel + c;
            normalizedSpec.channelnames.push_back(sourceChannel < spec.nchannels
                ? std::string(spec.channel_name(sourceChannel)) : "channel" + std::to_string(c));
        }
        // Without a channel, window or value change the loop below would
        // copy every decoded value unchanged; use the decoded buffer instead.
        const bool identity = !unassociateAlpha && options.gamma == 1.0f &&
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
        }
        std::vector<float> &pixels = identity ? sourcePixels : outputPixels;
        // make_texture reads a wrapped buffer in place; no copy is needed.
        OIIO::ImageBuf normalized(normalizedSpec, OIIO::span<float>(pixels.data(), pixels.size()));
        m_impl->pixelNanoseconds.fetch_add(elapsedNanoseconds(stageStart), std::memory_order_relaxed);
        stageStart = std::chrono::steady_clock::now();
        static std::atomic<uint64_t> serial{0};
        const auto temp = root / (key + "." + std::to_string(processId()) + "." +
                                  std::to_string(++serial) + ".tmp.exr");
        OIIO::ImageSpec config;
        config.format = OIIO::TypeDesc::FLOAT;
        config.tile_width = config.tile_height = 64;
        config.attribute("maketx:resize", 0);
        config.attribute("maketx:forcefloat", 1);
        config.attribute("maketx:fileformatname", "openexr");
        config.attribute("maketx:nomipmap", 0);
        auto wrapName = [](TextureWrap value) -> char const * {
            switch (value) {
            case TextureWrap::Black: return "black";
            case TextureWrap::Clamp: return "clamp";
            case TextureWrap::Periodic: return "periodic";
            case TextureWrap::Mirror: return "mirror";
            case TextureWrap::PeriodicPow2: return "periodic, pow2";
            case TextureWrap::Shadow: return "black";
            default: return "default";
            }
        };
        config.attribute("maketx:swrap", wrapName(options.wrapS));
        config.attribute("maketx:twrap", wrapName(options.wrapT));
        if (!OIIO::ImageBufAlgo::make_texture(OIIO::ImageBufAlgo::MakeTxTexture,
                                               normalized, temp.string(), config)) {
            std::filesystem::remove(temp);
            throw std::runtime_error("cannot prepare texture " + absolute.string() + ": " + OIIO::geterror());
        }
        m_impl->mipNanoseconds.fetch_add(elapsedNanoseconds(stageStart), std::memory_order_relaxed);
        std::error_code ec;
        lockWaitStart = std::chrono::steady_clock::now();
        publicationLock = std::make_unique<CrossProcessCacheLock>(root);
        m_impl->lockWaitNanoseconds.fetch_add(elapsedNanoseconds(lockWaitStart),
                                              std::memory_order_relaxed);
        stageStart = std::chrono::steady_clock::now();
        std::filesystem::rename(temp, tx, ec);
        if (ec) {
            if (!std::filesystem::exists(tx)) {
                std::filesystem::remove(temp);
                throw std::runtime_error("cannot publish prepared texture: " + ec.message());
            }
            std::filesystem::remove(temp, ec);
        }
        writePin();
        publicationLock.reset();
        publishNanoseconds += elapsedNanoseconds(stageStart);
    }
    stageStart = std::chrono::steady_clock::now();
    auto preparedInput = OIIO::ImageInput::open(tx.string());
    if (!preparedInput)
        throw std::runtime_error("cannot inspect prepared texture: " + OIIO::geterror());
    const OIIO::ImageSpec preparedSpec = preparedInput->spec();
    preparedInput->close();
    impl->source = absolute.filename().string().rfind("embedded-", 0) == 0
        ? std::filesystem::path{} : absolute;
    impl->prepared = tx;
    impl->width = preparedSpec.width;
    impl->height = preparedSpec.height;
    impl->channels = preparedSpec.nchannels;
    const bool embeddedSource = impl->source.empty();
    impl->version.resolvedIdentifier = embeddedSource ? "embedded:" + key : absolute.generic_string();
    impl->version.interpretationIdentity = embeddedSource ? key : stamp;
    impl->version.fingerprint = rawFingerprint;
    impl->version.generation = generation;
    impl->system = m_impl->system;
    impl->serviceOwner = shared_from_this();
    // A previous cache entry at this content path may have been evicted and
    // recreated while this process kept its TextureSystem alive. Only a file
    // written just now can be such a recreation; an existing one may be
    // sampled by render threads right now, and invalidating it under them
    // clears its level table mid-lookup.
    if (!cacheFilePresent)
        m_impl->system->invalidate(OIIO::ustring(tx.string()), true);
    impl->handle = m_impl->system->get_texture_handle(OIIO::ustring(tx.string()));
    if (impl->handle && !m_impl->system->good(impl->handle)) {
        // A file OIIO once failed to open stays broken until invalidated.
        // Nothing samples a broken file, so retrying it is safe.
        (void)m_impl->system->geterror();
        m_impl->system->invalidate(OIIO::ustring(tx.string()), true);
        impl->handle = m_impl->system->get_texture_handle(OIIO::ustring(tx.string()));
    }
    if (!impl->handle || !m_impl->system->good(impl->handle))
        throw std::runtime_error("cannot open prepared texture: " + m_impl->system->geterror());
    std::error_code accessError;
    std::filesystem::last_write_time(tx, std::filesystem::file_time_type::clock::now(), accessError);
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
                if (auto live = weak.lock()) pinned.emplace(live->preparedPath().filename().string(), true);
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
    OIIO::ImageBuf copy(source);
    if (sourceIsUnassociated)
        copy.specmod().attribute("oiio:UnassociatedAlpha", 1);
    if (!copy.write(materialized.string()))
        throw std::runtime_error("cannot materialize in-memory texture: " + copy.geterror());
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
    if (resource.m_impl->system->texture(resource.m_impl->handle,
            resource.m_impl->system->get_perthread_info(), options, query.s, query.t,
            query.dsdx, query.dtdx, query.dsdy, query.dtdy, channelCount, result))
        return true;
    lookupError() = resource.m_impl->system->geterror();
    return false;
}

std::string const &TextureService::lastError() noexcept { return lookupError(); }

bool TextureService::readPixels(TextureResource const &resource, int xb, int yb,
                                int xe, int ye, int firstChannel, int channelCount,
                                float *result) const
{
    if (!result || xb < 0 || yb < 0 || xe > resource.width() || ye > resource.height() ||
        xe <= xb || ye <= yb || firstChannel < 0 || channelCount <= 0 ||
        firstChannel + channelCount > resource.channels()) return false;
    OIIO::TextureOpt options;
    if (resource.m_impl->system->get_texels(resource.m_impl->handle,
            resource.m_impl->system->get_perthread_info(), options, 0,
            xb, xe, yb, ye, 0, 1, firstChannel, firstChannel + channelCount,
            OIIO::TypeDesc::FLOAT, result))
        return true;
    lookupError() = resource.m_impl->system->geterror();
    return false;
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
