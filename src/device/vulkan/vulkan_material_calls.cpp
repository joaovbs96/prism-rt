#include "vulkan_material_calls.h"

#include "../gpu/gpu_device.h"
#include "../gpu/gpu_materials.h"
#include "../gpu/hlsl_compiler.h"
#include "../gpu/shader_sources.h"
#include "../../progress.h"

#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace prism::device::vulkan {

const char* const kMaterialCallProfile = "lib_6_6";

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

const char* Setting(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

std::vector<char> ReadFile(std::filesystem::path const& path)
{
    std::vector<char> bytes;
    if (std::ifstream file{path, std::ios::binary})
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return bytes;
}

// The trailer of a cache file.
struct CacheTrailer {
    char magic[8];
    uint64_t size;
    uint64_t digest;
    uint64_t extra;
};
constexpr char kCacheMagic[8] = {'P', 'R', 'I', 'S', 'M', 'C', 'F', '1'};

uint64_t CacheDigest(const char* data, size_t size)
{
    uint64_t a = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) a = (a ^ uint8_t(data[i])) * 1099511628211ull;
    return a ^ (uint64_t(size) * 0x9e3779b97f4a7c15ull);
}

// Writes `bytes` to `path` and makes them durable before returning.
bool WriteDurably(std::filesystem::path const& path, std::vector<char> const& bytes)
{
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
#else
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fflush(file) == 0;
    ok = fsync(fileno(file)) == 0 && ok;
    return std::fclose(file) == 0 && ok;
#endif
}

// The largest growth of the process's committed memory while it lives, for
// the driver's share of pipeline creation (Windows; zero elsewhere).
class CommitSampler {
public:
    CommitSampler()
    {
#ifdef _WIN32
        m_base = Commit();
        m_thread = std::thread([this] {
            while (!m_stop.load()) {
                m_peak = std::max(m_peak.load(), Commit());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
#endif
    }
    ~CommitSampler() { stop(); }
    int64_t stop()
    {
#ifdef _WIN32
        if (m_thread.joinable()) {
            m_stop = true;
            m_thread.join();
            m_peak = std::max(m_peak.load(), Commit());
        }
        return m_peak.load() - m_base;
#else
        return 0;
#endif
    }

private:
#ifdef _WIN32
    static int64_t Commit()
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters)))
            return 0;
        return int64_t(counters.PrivateUsage);
    }
    int64_t m_base = 0;
    std::atomic<int64_t> m_peak{0};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
#endif
};

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// DXC processes run at once (PRISM_DXC_JOBS; each is memory capped by
// PRISM_DXC_MEMORY_MIB). DXC is single threaded and a callable's compile
// takes at most about 450 MiB: by default half the hardware threads, at most
// 12 and one per 2 GiB of physical memory.
unsigned DxcJobs()
{
    if (const char* value = Setting("PRISM_DXC_JOBS")) return unsigned(std::max(1, std::atoi(value)));
    unsigned jobs = std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 12u);
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) jobs = std::clamp(unsigned(status.ullTotalPhys >> 31), 1u, jobs);
#endif
    return jobs;
}

// Shaders compiled at once, DXC and driver (PRISM_VULKAN_COMPILE_THREADS).
// The driver compiles each library on the calling thread.
unsigned CompileThreads()
{
    if (const char* value = Setting("PRISM_VULKAN_COMPILE_THREADS")) return unsigned(std::max(1, std::atoi(value)));
    return std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
}

// Limits the DXC processes running at once.
class DxcSlots {
public:
    explicit DxcSlots(unsigned count) : m_free(count) {}
    void acquire()
    {
        std::unique_lock lock(m_mutex);
        m_ready.wait(lock, [&] { return m_free > 0; });
        --m_free;
    }
    void release()
    {
        {
            std::lock_guard lock(m_mutex);
            ++m_free;
        }
        m_ready.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    unsigned m_free;
};

DxcSlots& Dxc()
{
    static DxcSlots slots(DxcJobs());
    return slots;
}

const gpu::HlslStage kLibraryStage{kMaterialCallProfile, ""};

// The driver compiles the pipeline's shaders on several threads through a
// deferred operation (PRISM_VULKAN_PARALLEL_PIPELINE=0: on one thread).
bool ParallelPipelineSetting()
{
    const char* value = Setting("PRISM_VULKAN_PARALLEL_PIPELINE");
    return !value || std::strcmp(value, "0") != 0;
}

// Ignore stored pipeline caches (PRISM_VULKAN_PIPELINE_CACHE=0), to measure the driver's compile.
bool PipelineCacheSetting()
{
    const char* value = Setting("PRISM_VULKAN_PIPELINE_CACHE");
    return !value || std::strcmp(value, "0") != 0;
}

// A VkPipelineCache with the data of `file` (empty if missing, torn, for
// another device, or rejected by the driver); `extra` is the value stored with it.
VkPipelineCache LoadPipelineCache(VkDevice device, VkPhysicalDeviceProperties const& properties,
                                  std::filesystem::path const& file, size_t* loadedBytes, uint64_t* extra = nullptr)
{
    std::vector<char> data = PipelineCacheSetting() ? ReadCacheFile(file, extra) : std::vector<char>();
    if (!PipelineCacheDataMatches(data, properties)) data.clear();
    if (data.empty() && extra) *extra = 0;
    *loadedBytes = data.size();
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &info, nullptr, &cache) == VK_SUCCESS) return cache;
    *loadedBytes = 0;
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    return vkCreatePipelineCache(device, &info, nullptr, &cache) == VK_SUCCESS ? cache : VK_NULL_HANDLE;
}

// Stores `cache` in `file` if it grew, and destroys it.
void StorePipelineCache(VkDevice device, VkPipelineCache cache, std::filesystem::path const& file,
                        size_t loadedBytes, bool succeeded, uint64_t extra = 0)
{
    if (!cache) return;
    size_t size = 0;
    if (succeeded && vkGetPipelineCacheData(device, cache, &size, nullptr) == VK_SUCCESS && size > 0 &&
        size != loadedBytes) {
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device, cache, &size, data.data()) == VK_SUCCESS)
            WriteCacheFile(file, data.data(), size, extra);
    }
    vkDestroyPipelineCache(device, cache, nullptr);
}

// The interface every library and the linked pipeline share. No shader uses
// ray payloads or hit attributes (the raygen shader traces with ray queries
// and calls the materials with callable data, which the interface does not
// size); the payload size is set to the callable data's (MaterialCall, 272
// bytes) anyway, as a conservative bound.
constexpr uint32_t kMaterialCallBytes = 272;
const VkRayTracingPipelineInterfaceCreateInfoKHR kLibraryInterface{
    VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR, nullptr, kMaterialCallBytes, 0};

std::string SpirvDigest(std::vector<uint32_t> const& spirv)
{
    return gpu::SourceDigest(std::string(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4));
}

bool IdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Calls `f` with every identifier of `text` outside comments and literals.
template<class F> void ForEachIdentifier(std::string_view text, F&& f)
{
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i = text.find('\n', i);
            if (i == std::string_view::npos) return;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i = text.find("*/", i + 2);
            if (i == std::string_view::npos) return;
            i += 2;
        } else if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < text.size() && text[j] != c) j += text[j] == '\\' ? 2 : 1;
            i = j + 1;
        } else if (IdentifierStart(c)) {
            size_t j = i + 1;
            while (j < text.size() && IdentifierChar(text[j])) ++j;
            f(text.substr(i, j - i));
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < text.size() && (IdentifierChar(text[j]) || text[j] == '.')) ++j;
            i = j;
        } else {
            ++i;
        }
    }
}

// The link unit's code split into top-level declarations once, to keep for
// each program only the functions its dispatch reaches: DXC parses and
// legalizes much less, and a callable's source no longer changes with the
// other programs. Declarations other than function definitions are all kept.
class LinkUnitTrimmer {
public:
    explicit LinkUnitTrimmer(std::string const& code) : m_code(code) { m_valid = split(); if (m_valid) index(); }
    bool valid() const { return m_valid; }

    // The code with the functions reachable from `roots`.
    std::string trim(std::string const& roots) const
    {
        std::vector<bool> keep(m_chunks.size(), false);
        std::vector<size_t> pending;
        auto reachChunk = [&](size_t k) {
            if (!keep[k]) {
                keep[k] = true;
                pending.push_back(k);
            }
        };
        ForEachIdentifier(roots, [&](std::string_view identifier) {
            if (auto found = m_definitions.find(identifier); found != m_definitions.end())
                for (size_t k : found->second) reachChunk(k);
        });
        for (size_t k : m_alwaysReached) reachChunk(k);
        while (!pending.empty()) {
            const size_t k = pending.back();
            pending.pop_back();
            for (size_t r : m_references[k]) reachChunk(r);
        }
        std::string trimmed;
        trimmed.reserve(m_code.size());
        for (size_t k = 0; k < m_chunks.size(); ++k)
            if (m_chunks[k].name.empty() || keep[k])
                trimmed.append(m_code, m_chunks[k].begin, m_chunks[k].end - m_chunks[k].begin);
        return trimmed;
    }

private:
    struct Chunk {
        size_t begin, end;
        std::string_view name; // function definitions only
    };

    bool split()
    {
        std::string const& code = m_code;
        size_t start = 0, depth = 0, parenDepth = 0;
        bool sawAssign = false, sawParen = false;
        std::string_view name, lastIdentifier;
        auto reset = [&] {
            sawAssign = sawParen = false;
            name = lastIdentifier = {};
        };
        auto finish = [&](size_t end, bool function) {
            m_chunks.push_back({start, end, function ? name : std::string_view()});
            start = end;
            reset();
        };
        for (size_t i = 0; i < code.size();) {
            const char c = code[i];
            if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
                const size_t end = code.find('\n', i);
                i = end == std::string::npos ? code.size() : end;
                continue;
            }
            if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
                const size_t end = code.find("*/", i + 2);
                if (end == std::string::npos) return false;
                i = end + 2;
                continue;
            }
            if (c == '"' || c == '\'') {
                size_t j = i + 1;
                while (j < code.size() && code[j] != c) j += code[j] == '\\' ? 2 : 1;
                i = j + 1;
                continue;
            }
            if (c == '#' && depth == 0 && parenDepth == 0) {
                // Preprocessor lines stand alone, kept like declarations.
                if (i > start) m_chunks.push_back({start, i, {}});
                const size_t end = code.find('\n', i);
                const size_t next = end == std::string::npos ? code.size() : end + 1;
                m_chunks.push_back({i, next, {}});
                i = start = next;
                reset();
                continue;
            }
            if (IdentifierStart(c)) {
                size_t j = i + 1;
                while (j < code.size() && IdentifierChar(code[j])) ++j;
                if (depth == 0 && parenDepth == 0) lastIdentifier = std::string_view(code).substr(i, j - i);
                i = j;
                continue;
            }
            if (depth == 0) {
                if (c == '(') {
                    if (parenDepth++ == 0 && !sawParen && !sawAssign) {
                        sawParen = true;
                        name = lastIdentifier;
                    }
                } else if (c == ')') {
                    if (parenDepth == 0) return false;
                    --parenDepth;
                } else if (c == '=' && parenDepth == 0) {
                    sawAssign = true;
                } else if (c == ';' && parenDepth == 0) {
                    finish(++i, false);
                    continue;
                }
            }
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (depth == 0) return false;
                if (--depth == 0 && sawParen && !sawAssign) {
                    finish(++i, !name.empty());
                    continue;
                }
            }
            ++i;
        }
        if (depth != 0 || parenDepth != 0) return false;
        if (start < code.size()) m_chunks.push_back({start, code.size(), {}});
        return true;
    }

    // The functions each declaration names, and those other declarations (types, constants) name.
    void index()
    {
        for (size_t k = 0; k < m_chunks.size(); ++k)
            if (!m_chunks[k].name.empty()) m_definitions[m_chunks[k].name].push_back(k);
        m_references.resize(m_chunks.size());
        for (size_t k = 0; k < m_chunks.size(); ++k) {
            auto& references = m_chunks[k].name.empty() ? m_alwaysReached : m_references[k];
            ForEachIdentifier(std::string_view(m_code).substr(m_chunks[k].begin, m_chunks[k].end - m_chunks[k].begin),
                              [&](std::string_view identifier) {
                                  if (auto found = m_definitions.find(identifier); found != m_definitions.end())
                                      references.insert(references.end(), found->second.begin(), found->second.end());
                              });
        }
    }

    std::string const& m_code;
    bool m_valid = false;
    std::vector<Chunk> m_chunks;
    std::unordered_map<std::string_view, std::vector<size_t>> m_definitions;
    std::vector<std::vector<size_t>> m_references;
    std::vector<size_t> m_alwaysReached;
};

// Whether callables hold only their program's functions (PRISM_VULKAN_TRIM_CALLABLES=0: the whole link unit).
bool TrimCallablesSetting()
{
    const char* value = Setting("PRISM_VULKAN_TRIM_CALLABLES");
    return !value || std::strcmp(value, "0") != 0;
}

// The program a callable source names (ProgramCallableSource), for logs.
std::string ProgramTag(std::string const& source)
{
    constexpr std::string_view tag = "// MDL program ";
    if (source.compare(0, tag.size(), tag) != 0) return {};
    return ", program " + source.substr(tag.size(), source.find('\n') - tag.size());
}

void ReplaceAll(std::string& text, std::string const& from, std::string const& to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

} // namespace

void WriteCacheFile(std::filesystem::path const& path, void const* data, size_t size, uint64_t extra)
{
    static std::atomic<uint64_t> serial{0};
    std::vector<char> bytes(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    CacheTrailer trailer{};
    std::memcpy(trailer.magic, kCacheMagic, sizeof(kCacheMagic));
    trailer.size = size;
    trailer.digest = CacheDigest(bytes.data(), size);
    trailer.extra = extra;
    bytes.insert(bytes.end(), reinterpret_cast<const char*>(&trailer),
                 reinterpret_cast<const char*>(&trailer) + sizeof(trailer));
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = std::filesystem::path(path.string() + std::format(".{}.{}.tmp",
        std::hash<std::thread::id>{}(std::this_thread::get_id()), ++serial));
    if (!WriteDurably(temporary, bytes)) {
        std::filesystem::remove(temporary, error);
        return;
    }
    // Concurrent readers see the previous complete file or this one.
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

std::vector<char> ReadCacheFile(std::filesystem::path const& path, uint64_t* extra)
{
    std::vector<char> bytes = ReadFile(path);
    CacheTrailer trailer{};
    if (bytes.size() < sizeof(trailer)) return {};
    std::memcpy(&trailer, bytes.data() + bytes.size() - sizeof(trailer), sizeof(trailer));
    const size_t size = bytes.size() - sizeof(trailer);
    if (std::memcmp(trailer.magic, kCacheMagic, sizeof(kCacheMagic)) != 0 || trailer.size != size ||
        trailer.digest != CacheDigest(bytes.data(), size))
        return {};
    bytes.resize(size);
    if (extra) *extra = trailer.extra;
    return bytes;
}

bool PlausibleSpirv(std::vector<char> const& bytes)
{
    uint32_t magic = 0;
    if (bytes.size() < 20 || bytes.size() % 4 != 0) return false;
    std::memcpy(&magic, bytes.data(), 4);
    return magic == 0x07230203u;
}

bool PipelineCacheDataMatches(std::vector<char> const& data, VkPhysicalDeviceProperties const& properties)
{
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header)) return false;
    std::memcpy(&header, data.data(), sizeof(header));
    return header.headerSize >= sizeof(header) && header.headerSize <= data.size() &&
        header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        header.vendorID == properties.vendorID && header.deviceID == properties.deviceID &&
        std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

bool RaygenSource(bool reorder, uint32_t programs, std::string* source, std::string* error)
{
    std::string defines = "#define PRISM_CALL_MATERIALS 1\n" + ShaderSaltDefine();
    if (reorder) {
        uint32_t bits = 1;
        while ((1u << bits) < programs && bits < 16) ++bits;
        defines += std::format("#define PRISM_REORDER_MATERIALS 1\n#define PRISM_REORDER_HINT_BITS {}u\n", bits);
    }
    if (!gpu::AssembleShader("gpu/kernel.hlsl", {"gpu/vulkan"}, std::string(), source, error)) return false;
    *source = defines + *source;
    return true;
}

bool ProgramCallableSource(gpu::MdlProgramCode const& program, std::string* source, std::string* error)
{
    if (!gpu::AssembleShader("gpu/vulkan/material_callable.hlsl", {"gpu/vulkan"}, program.code, source, error))
        return false;
    *source = std::format("// MDL program {}\n", program.digest.substr(0, 12)) + ShaderSaltDefine() +
        "#define PRISM_MDL_PROGRAM_TABLES 1\n" + *source;
    return true;
}

bool MaterialCallSources(gpu::MdlShaderCode const& code, bool reorder, std::vector<std::string>* sources,
                         std::string* error, bool trim)
{
    if (code.perProgram) {
        const uint32_t programs = uint32_t(code.programs.size());
        sources->assign(programs + 1, std::string());
        if (!RaygenSource(reorder, programs, &(*sources)[0], error)) return false;
        for (uint32_t p = 0; p < programs; ++p)
            if (!ProgramCallableSource(*code.programs[p], &(*sources)[p + 1], error)) return false;
        return true;
    }
    const uint32_t programs = uint32_t(code.programDispatch.size());
    sources->assign(programs + 1, std::string());
    if (!RaygenSource(reorder, programs, &(*sources)[0], error)) return false;
    const std::string salt = ShaderSaltDefine();
    std::optional<LinkUnitTrimmer> trimmer;
    if (trim && TrimCallablesSetting()) trimmer.emplace(code.targetCode);
    for (uint32_t p = 0; p < programs; ++p) {
        std::string mdl;
        if (trimmer && trimmer->valid()) {
            // Only this program's functions remain, and only it reaches this
            // callable: without its index in the source (function names and
            // dispatch cases), the program compiles to the same callable in
            // any material set.
            std::string dispatch = code.programDispatch[p];
            ReplaceAll(dispatch, "switch (program)", "switch (0u)");
            ReplaceAll(dispatch, std::format("case {}u:", p), "case 0u:");
            mdl = trimmer->trim(code.programDispatch[p] + code.sceneData) + dispatch + code.sceneData;
            ReplaceAll(mdl, std::format("prism_p{}_", p), "prism_p_");
            ReplaceAll(mdl, std::format("Prism material dispatch: program {} ", p), "Prism material dispatch ");
        } else {
            mdl = code.targetCode + code.programDispatch[p] + code.sceneData;
        }
        if (!gpu::AssembleShader("gpu/vulkan/material_callable.hlsl", {"gpu/vulkan"}, mdl, &(*sources)[p + 1],
                                 error))
            return false;
        (*sources)[p + 1] = salt + (*sources)[p + 1];
    }
    return true;
}

// PRISM_VULKAN_SHADER_SALT=<n> changes every module's code, so DXC and the
// driver (which keeps its own cache) compile anew: cold measurements.
std::string ShaderSaltDefine()
{
    const char* value = Setting("PRISM_VULKAN_SHADER_SALT");
    return value ? std::format("#define PRISM_SHADER_SALT {}u\n", std::strtoul(value, nullptr, 10) | 0x80000000u)
                 : std::string();
}

CallableMode CallableModeSetting()
{
    const char* value = Setting("PRISM_VULKAN_CALLABLES");
    if (!value || std::strcmp(value, "auto") == 0) return CallableMode::Auto;
    if (std::strcmp(value, "0") == 0) return CallableMode::Never;
    return CallableMode::Always;
}

size_t CallableThresholdBytes()
{
    if (const char* value = Setting("PRISM_VULKAN_CALLABLE_THRESHOLD_KIB"))
        return size_t(std::max(0ll, std::atoll(value))) << 10;
    return size_t(320) << 10;
}

bool ReorderSetting()
{
    const char* value = Setting("PRISM_VULKAN_REORDER");
    return value && std::strcmp(value, "0") != 0;
}

bool PipelineLibrariesSetting()
{
    const char* value = Setting("PRISM_VULKAN_PIPELINE_LIBRARIES");
    return !value || std::strcmp(value, "0") != 0;
}

bool PipelineStatisticsSetting()
{
    const char* value = Setting("PRISM_VULKAN_PIPELINE_STATS");
    return value && std::strcmp(value, "0") != 0;
}

void PrintPipelineStatistics(VkDevice device, VkPipeline pipeline, std::string const& label)
{
    auto properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
    auto statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
    if (!properties || !statistics) return;
    VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    info.pipeline = pipeline;
    uint32_t count = 0;
    const VkResult listed = properties(device, &info, &count, nullptr);
    if (listed != VK_SUCCESS || count == 0) {
        std::cerr << std::format("PrismRT Vulkan: {}: the driver reports no executables (VkResult {})\n", label,
                                 int(listed));
        return;
    }
    std::vector<VkPipelineExecutablePropertiesKHR> executables(
        count, VkPipelineExecutablePropertiesKHR{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    properties(device, &info, &count, executables.data());
    for (uint32_t e = 0; e < count; ++e) {
        VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        executable.pipeline = pipeline;
        executable.executableIndex = e;
        uint32_t statCount = 0;
        statistics(device, &executable, &statCount, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> stats(
            statCount, VkPipelineExecutableStatisticKHR{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        statistics(device, &executable, &statCount, stats.data());
        std::string line = std::format("PrismRT Vulkan: {} executable {} '{}' (subgroup {}):", label, e,
                                       executables[e].name, executables[e].subgroupSize);
        for (auto const& s : stats) {
            line += std::format(" {}=", s.name);
            switch (s.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: line += s.value.b32 ? "true" : "false"; break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: line += std::to_string(s.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: line += std::to_string(s.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: line += std::format("{:.3f}", s.value.f64); break;
            default: line += "?"; break;
            }
        }
        std::cerr << line << '\n';
    }
}

MaterialCallShaders::MaterialCallShaders(VkDevice device, VkPhysicalDeviceProperties const& properties,
                                         RayTracingSupport const& support, VkPipelineLayout layout,
                                         std::string pipelineCacheTag)
    : m_device(device), m_properties(properties), m_support(support), m_layout(layout),
      m_cacheTag(std::move(pipelineCacheTag))
{
    m_createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    // Statistics are captured on whole pipelines only.
    m_libraries = support.pipeline && support.library && m_createPipelines && PipelineLibrariesSetting() &&
        !(support.executableStatistics && PipelineStatisticsSetting());
}

MaterialCallShaders::~MaterialCallShaders()
{
    {
        // Queued compiles are dropped; running ones finish.
        std::lock_guard lock(m_queueMutex);
        m_stopping = true;
        m_queue.clear();
    }
    m_queueReady.notify_all();
    for (auto& worker : m_workers) worker.join();
    if (m_prewarm.valid()) m_prewarm.wait();
    std::lock_guard lock(m_mutex);
    for (auto& [key, future] : m_shaders) {
        auto const& built = future.get();
        if (built && built->library) vkDestroyPipeline(m_device, built->library, nullptr);
    }
}

void MaterialCallShaders::prewarmRaygen()
{
    if (m_prewarm.valid() || !m_libraries) return;
    m_prewarm = std::async(std::launch::async, [this] {
#ifdef _WIN32
        // The raygen shader is usually the last shader a first scene waits
        // for: ahead of the scene loading that runs meanwhile (DXC inherits it).
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        std::string source, error;
        if (RaygenSource(false, 0, &source, &error)) (void)shader(source, true);
    });
}

void MaterialCallShaders::prefetch(std::string source)
{
    if (!m_libraries) return;
    {
        const std::string key = gpu::SourceDigest(source + gpu::HlslCompilerSignature(gpu::HlslTarget::Spirv,
                                                                                      kLibraryStage));
        std::lock_guard lock(m_mutex);
        if (m_shaders.contains(key)) return;
    }
    {
        std::lock_guard lock(m_queueMutex);
        if (m_stopping) return;
        m_queue.push_back(std::move(source));
        if (m_workers.size() < CompileThreads() && m_workers.size() < m_queue.size() + m_busy)
            m_workers.emplace_back([this] { prefetchWorker(); });
    }
    ++m_prefetched;
    m_queueReady.notify_one();
}

void MaterialCallShaders::prefetchWorker()
{
    for (;;) {
        std::string source;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueReady.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) return;
            source = std::move(m_queue.front());
            m_queue.pop_front();
            ++m_busy;
        }
        {
            // A compile for a scene that is still loading: reported with the
            // kernel phase, which the material phase outranks meanwhile.
            progress::Scope kernel(Phase::Kernel);
            progress::Detail(Phase::Kernel, std::format("material callables ahead of the scene: {} of {}",
                                                        m_prefetchedDone.load() + 1, m_prefetched.load()));
            (void)shader(source, false);
        }
        ++m_prefetchedDone;
        std::lock_guard lock(m_queueMutex);
        --m_busy;
    }
}

std::shared_ptr<const ShaderBuild> MaterialCallShaders::shader(std::string const& source, bool raygen)
{
    const std::string key = gpu::SourceDigest(source + gpu::HlslCompilerSignature(gpu::HlslTarget::Spirv,
                                                                                  kLibraryStage));
    const auto start = Clock::now();
    // Without libraries the SPIR-V cache is enough: nothing is kept.
    if (!m_libraries) return compile(source, raygen);
    std::promise<std::shared_ptr<const ShaderBuild>> promise;
    std::shared_future<std::shared_ptr<const ShaderBuild>> earlier;
    {
        std::lock_guard lock(m_mutex);
        if (auto found = m_shaders.find(key); found != m_shaders.end()) earlier = found->second;
        else m_shaders.emplace(key, promise.get_future().share());
    }
    if (earlier.valid()) {
        // Compiled (or compiling) for an earlier request: no work for this one.
        auto const& built = earlier.get();
        auto reused = std::make_shared<ShaderBuild>();
        reused->library = built->library;
        reused->stackSize = built->stackSize;
        reused->error = built->error;
        reused->readySeconds = SecondsSince(start);
        reused->reused = true;
        if (!built->error.empty()) {
            // A failed compile is tried again by the next request.
            std::lock_guard lock(m_mutex);
            m_shaders.erase(key);
        }
        return reused;
    }
    auto built = compile(source, raygen);
    built->readySeconds = SecondsSince(start);
    promise.set_value(built);
    return built;
}

std::shared_ptr<ShaderBuild> MaterialCallShaders::compile(std::string const& source, bool raygen)
{
    auto built = std::make_shared<ShaderBuild>();
    const auto directory = gpu::ShaderCacheDirectory("vulkan");
    const auto cached = directory /
        (gpu::SourceDigest(source + gpu::HlslCompilerSignature(gpu::HlslTarget::Spirv, kLibraryStage)) + ".spv");
    std::vector<char> bytes = ReadCacheFile(cached);
    if (!PlausibleSpirv(bytes)) {
        gpu::HlslCompileStats stats;
        std::string reason;
        Dxc().acquire();
        const bool ok = gpu::CompileHlsl(source, gpu::HlslTarget::Spirv, directory, &bytes, &reason, kLibraryStage,
                                         &stats);
        Dxc().release();
        built->dxcCompiled = true;
        built->dxcSeconds = stats.seconds;
        built->dxcPeakBytes = stats.peakBytes;
        if (gpu::GpuVerbose())
            std::cerr << std::format("PrismRT Vulkan: DXC {} {:.1f} s, peak {} MiB, {} KB SPIR-V{}\n",
                                     raygen ? "raygen" : "callable", stats.seconds, stats.peakBytes >> 20,
                                     bytes.size() >> 10, ProgramTag(source));
        if (!ok || !PlausibleSpirv(bytes)) {
            built->error = (raygen ? std::string("raygen shader: ") : std::string("material callable: ")) + reason;
            return built;
        }
        WriteCacheFile(cached, bytes.data(), bytes.size());
    }
    built->spirv.resize(bytes.size() / 4);
    std::memcpy(built->spirv.data(), bytes.data(), bytes.size());
    if (!m_libraries) return built;

    // The library: one stage, one general group.
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = bytes.size();
    moduleInfo.pCode = built->spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        built->error = "cannot create a ray tracing shader module";
        return built;
    }
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = raygen ? VK_SHADER_STAGE_RAYGEN_BIT_KHR : VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    stage.module = module;
    stage.pName = raygen ? "raygen" : "material";
    VkRayTracingShaderGroupCreateInfoKHR group{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = VK_SHADER_UNUSED_KHR;
    group.anyHitShader = VK_SHADER_UNUSED_KHR;
    group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    info.stageCount = 1;
    info.pStages = &stage;
    info.groupCount = 1;
    info.pGroups = &group;
    info.maxPipelineRayRecursionDepth = 1;
    info.pLibraryInterface = &kLibraryInterface;
    info.layout = m_layout;
    const auto cacheFile = directory / (SpirvDigest(built->spirv) + "-lib-" + m_cacheTag + ".vkpc");
    size_t loaded = 0;
    uint64_t recordedStack = 0;
    VkPipelineCache cache = LoadPipelineCache(m_device, m_properties, cacheFile, &loaded, &recordedStack);
    built->driverCacheHit = loaded > 0;
    const auto start = Clock::now();
    const VkResult result = m_createPipelines(m_device, VK_NULL_HANDLE, cache, 1, &info, nullptr, &built->library);
    built->driverSeconds = SecondsSince(start);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (result == VK_SUCCESS) {
        static auto groupStackSize = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupStackSizeKHR"));
        const uint64_t reported = groupStackSize
            ? groupStackSize(m_device, built->library, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR) : 0;
        built->stackSize = std::max(reported, recordedStack);
        if (gpu::GpuVerbose() && reported < recordedStack)
            std::cerr << std::format("PrismRT Vulkan: {} library reports a {} B stack, {} B when it was compiled\n",
                                     raygen ? "raygen" : "callable", reported, recordedStack);
    }
    // The stack size is stored with the cache data, also when only it changed.
    StorePipelineCache(m_device, cache, cacheFile, built->stackSize != recordedStack ? 0 : loaded,
                       result == VK_SUCCESS, built->stackSize);
    if (gpu::GpuVerbose())
        std::cerr << std::format("PrismRT Vulkan: library {} {:.2f} s{}, stack {} B{}\n", raygen ? "raygen" : "callable",
                                 built->driverSeconds, built->driverCacheHit ? " (pipeline cache)" : "",
                                 built->stackSize, ProgramTag(source));
    if (result != VK_SUCCESS) {
        built->library = VK_NULL_HANDLE;
        built->error = std::format("cannot create a {} pipeline library (VkResult {})",
                                   raygen ? "raygen" : "callable", int(result));
    }
    return built;
}

MaterialCallPipeline::MaterialCallPipeline(VkDevice device, VkPhysicalDevice physical,
                                           RayTracingSupport const& support, std::string pipelineCacheTag)
    : m_device(device), m_physical(physical), m_support(support), m_cacheTag(std::move(pipelineCacheTag))
{
    m_createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    m_groupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
    m_traceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR"));
    m_setStackSize = reinterpret_cast<PFN_vkCmdSetRayTracingPipelineStackSizeKHR>(
        vkGetDeviceProcAddr(device, "vkCmdSetRayTracingPipelineStackSizeKHR"));
    m_groupStackSize = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupStackSizeKHR"));
    m_createDeferred = reinterpret_cast<PFN_vkCreateDeferredOperationKHR>(
        vkGetDeviceProcAddr(device, "vkCreateDeferredOperationKHR"));
    m_destroyDeferred = reinterpret_cast<PFN_vkDestroyDeferredOperationKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyDeferredOperationKHR"));
    m_deferredConcurrency = reinterpret_cast<PFN_vkGetDeferredOperationMaxConcurrencyKHR>(
        vkGetDeviceProcAddr(device, "vkGetDeferredOperationMaxConcurrencyKHR"));
    m_deferredResult = reinterpret_cast<PFN_vkGetDeferredOperationResultKHR>(
        vkGetDeviceProcAddr(device, "vkGetDeferredOperationResultKHR"));
    m_joinDeferred = reinterpret_cast<PFN_vkDeferredOperationJoinKHR>(
        vkGetDeviceProcAddr(device, "vkDeferredOperationJoinKHR"));
}

VkResult MaterialCallPipeline::createPipeline(VkRayTracingPipelineCreateInfoKHR const& info, VkPipelineCache cache)
{
    VkDeferredOperationKHR deferred = VK_NULL_HANDLE;
    const bool parallel = ParallelPipelineSetting() && m_createDeferred && m_destroyDeferred &&
        m_deferredConcurrency && m_deferredResult && m_joinDeferred &&
        m_createDeferred(m_device, nullptr, &deferred) == VK_SUCCESS;
    VkResult result = m_createPipelines(m_device, parallel ? deferred : VK_NULL_HANDLE, cache, 1, &info, nullptr,
                                        &m_pipeline);
    if (!parallel) return result;
    if (result == VK_OPERATION_DEFERRED_KHR) {
        // Every joining thread compiles shaders until the driver has none left.
        const uint32_t workers = std::clamp(m_deferredConcurrency(m_device, deferred), 1u,
                                            std::max(1u, std::thread::hardware_concurrency()));
        m_report.pipelineThreads = workers;
        tbb::parallel_for(0u, workers, [&](uint32_t) {
            while (m_joinDeferred(m_device, deferred) == VK_THREAD_IDLE_KHR) std::this_thread::yield();
        });
        result = m_deferredResult(m_device, deferred);
    } else if (result == VK_OPERATION_NOT_DEFERRED_KHR) {
        result = VK_SUCCESS;
    }
    m_destroyDeferred(m_device, deferred, nullptr);
    return result;
}

MaterialCallPipeline::~MaterialCallPipeline()
{
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_table) vkDestroyBuffer(m_device, m_table, nullptr);
    if (m_tableMemory) vkFreeMemory(m_device, m_tableMemory, nullptr);
}

bool MaterialCallPipeline::build(gpu::MdlShaderCode const& code, VkPipelineLayout layout, bool reorder,
                                 MaterialCallShaders& shaderSet, std::string* error)
{
    if (!m_createPipelines || !m_groupHandles || !m_traceRays) {
        if (error) *error = "ray tracing pipeline functions unavailable";
        return false;
    }
    std::vector<std::string> sources;
    if (!MaterialCallSources(code, reorder, &sources, error)) return false;
    const uint32_t programs = uint32_t(sources.size() - 1);
    m_report.programs = programs;
    m_report.libraries = shaderSet.libraries();
    progress::Detail(Phase::Kernel, std::format("compiling {} material callables", programs));
    if (gpu::GpuVerbose())
        std::cerr << std::format("PrismRT Vulkan: preparing {} material callables ({} DXC jobs, {} compile threads, "
                                 "{})\n", programs, DxcJobs(), CompileThreads(),
                                 shaderSet.libraries() ? "pipeline libraries" : "one pipeline");
    // Callables first, the raygen shader (usually compiling since device creation) last.
    std::vector<std::shared_ptr<const ShaderBuild>> shaders(sources.size());
    std::atomic<size_t> done{0};
    const auto start = Clock::now();
    tbb::task_arena arena{int(CompileThreads())};
    arena.execute([&] {
        tbb::parallel_for(size_t(1), sources.size(), size_t(1), [&](size_t i) {
            shaders[i] = shaderSet.shader(sources[i], false);
            progress::Detail(Phase::Kernel, std::format("material callable {} of {}", ++done, programs));
        });
    });
    // A trimmed callable that does not compile (a reference the trimming
    // missed) compiles again with the whole link unit.
    std::vector<std::string> full;
    for (size_t i = 1; i < sources.size() && !code.perProgram; ++i) {
        if (shaders[i]->error.empty()) continue;
        if (full.empty() && !MaterialCallSources(code, reorder, &full, error, false)) return false;
        if (full[i] == sources[i]) continue;
        std::cerr << std::format("PrismRT Vulkan: material callable {} compiles with the whole link unit ({})\n",
                                 i - 1, shaders[i]->error.substr(0, 300));
        shaders[i] = shaderSet.shader(full[i], false);
    }
    const auto callablesReady = Clock::now();
    shaders[0] = shaderSet.shader(sources[0], true);
    m_report.raygenWaitSeconds = SecondsSince(callablesReady);
    m_report.shadersSeconds = SecondsSince(start);
    for (size_t i = 0; i < shaders.size(); ++i) {
        ShaderBuild const& s = *shaders[i];
        if (!s.error.empty()) {
            if (error) *error = s.error;
            return false;
        }
        if (i == 0) {
            m_report.raygenCompiled = s.dxcCompiled;
            m_report.raygenSeconds = s.dxcSeconds;
            m_report.raygenPeakBytes = s.dxcPeakBytes;
            m_report.raygenDriverSeconds = s.driverSeconds;
            continue;
        }
        if (s.dxcCompiled) ++m_report.compiled;
        if (s.reused) {
            ++m_report.reused;
            m_report.reusedWaitSeconds = std::max(m_report.reusedWaitSeconds, s.readySeconds);
        }
        m_report.dxcMaxSeconds = std::max(m_report.dxcMaxSeconds, s.dxcSeconds);
        m_report.dxcPeakBytes = std::max(m_report.dxcPeakBytes, s.dxcPeakBytes);
        if (s.driverSeconds > 0.0) {
            ++m_report.librariesCompiled;
            if (s.driverCacheHit) ++m_report.librariesCached;
        }
        m_report.driverMaxSeconds = std::max(m_report.driverMaxSeconds, s.driverSeconds);
        m_report.driverSumSeconds += s.driverSeconds;
    }
    progress::Detail(Phase::Kernel, "creating the Vulkan ray tracing pipeline");
    const bool created = shaderSet.libraries() ? link(shaders, layout, error) : createMonolithic(shaders, layout, error);
    if (created && gpu::GpuVerbose()) {
        auto stackSize = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupStackSizeKHR"));
        if (stackSize) {
            VkDeviceSize callableMax = 0;
            for (uint32_t g = 1; g < shaders.size(); ++g)
                callableMax = std::max(callableMax,
                                       stackSize(m_device, m_pipeline, g, VK_SHADER_GROUP_SHADER_GENERAL_KHR));
            std::cerr << std::format("PrismRT Vulkan: stack raygen {} B, largest callable {} B\n",
                                     stackSize(m_device, m_pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR),
                                     callableMax);
        }
    }
    return created && createBindingTable(uint32_t(shaders.size()), error);
}

bool MaterialCallPipeline::link(std::vector<std::shared_ptr<const ShaderBuild>> const& shaders,
                                VkPipelineLayout layout, std::string* error)
{
    // Groups of the linked pipeline follow the libraries: raygen, then the programs in order.
    std::vector<VkPipeline> libraries;
    for (auto const& s : shaders) libraries.push_back(s->library);
    VkPipelineLibraryCreateInfoKHR libraryInfo{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    libraryInfo.libraryCount = uint32_t(libraries.size());
    libraryInfo.pLibraries = libraries.data();
    // The stack size is set at each launch from the libraries' sizes (the
    // default formula of the spec for a raygen shader calling callables, with
    // no hit shaders: raygen + 2 x the largest callable), the largest each
    // library ever reported, rather than left to the linked pipeline's default.
    const bool dynamicStack = m_setStackSize && m_groupStackSize;
    const VkDynamicState stackState = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 1;
    dynamicState.pDynamicStates = &stackState;
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.maxPipelineRayRecursionDepth = 1;
    info.pLibraryInfo = &libraryInfo;
    info.pLibraryInterface = &kLibraryInterface;
    info.pDynamicState = dynamicStack ? &dynamicState : nullptr;
    info.layout = layout;
    const auto start = Clock::now();
    const VkResult result = m_createPipelines(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr,
                                              &m_pipeline);
    m_report.pipelineSeconds = SecondsSince(start);
    if (result != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        if (error) *error = std::format("cannot link the ray tracing pipeline (VkResult {})", int(result));
        return false;
    }
    if (dynamicStack) {
        uint64_t raygen = std::max(shaders[0]->stackSize,
                                   m_groupStackSize(m_device, m_pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR));
        uint64_t callable = 0;
        for (uint32_t g = 1; g < shaders.size(); ++g)
            callable = std::max({callable, shaders[g]->stackSize,
                                 m_groupStackSize(m_device, m_pipeline, g, VK_SHADER_GROUP_SHADER_GENERAL_KHR)});
        m_stackSize = uint32_t(std::min<uint64_t>(raygen + 2 * callable, UINT32_MAX));
        m_dynamicStack = true;
    }
    return true;
}

bool MaterialCallPipeline::createMonolithic(std::vector<std::shared_ptr<const ShaderBuild>> const& shaders,
                                            VkPipelineLayout layout, std::string* error)
{
    std::vector<VkShaderModule> shaderModules(shaders.size(), VK_NULL_HANDLE);
    std::vector<VkPipelineShaderStageCreateInfo> stages(shaders.size());
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups(shaders.size());
    std::string digests;
    bool created = true;
    for (size_t i = 0; i < shaders.size(); ++i) {
        auto const& spirv = shaders[i]->spirv;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = spirv.size() * 4;
        moduleInfo.pCode = spirv.data();
        created = created && vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shaderModules[i]) == VK_SUCCESS;
        digests += SpirvDigest(spirv);
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_RAYGEN_BIT_KHR : VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        stages[i].module = shaderModules[i];
        stages[i].pName = i == 0 ? "raygen" : "material";
        groups[i] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        groups[i].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[i].generalShader = uint32_t(i);
        groups[i].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    auto destroyModules = [&] {
        for (VkShaderModule m : shaderModules)
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    if (!created) {
        destroyModules();
        if (error) *error = "cannot create the ray tracing shader modules";
        return false;
    }
    const bool statistics = m_support.executableStatistics && PipelineStatisticsSetting();
    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.flags = statistics ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0;
    info.stageCount = uint32_t(stages.size());
    info.pStages = stages.data();
    info.groupCount = uint32_t(groups.size());
    info.pGroups = groups.data();
    // Intersection uses ray queries; the raygen shader never calls TraceRay.
    info.maxPipelineRayRecursionDepth = 1;
    info.layout = layout;

    // The driver translates every shader here; keep its result for later runs.
    const auto cacheFile = gpu::ShaderCacheDirectory("vulkan") /
        (gpu::SourceDigest(digests + (statistics ? "|stats" : "")) + "-" + m_cacheTag + ".vkpc");
    size_t loaded = 0;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(m_physical, &properties);
    VkPipelineCache cache = LoadPipelineCache(m_device, properties, cacheFile, &loaded);
    m_report.pipelineCacheHit = loaded > 0;
    const auto start = Clock::now();
    CommitSampler commit;
    const VkResult result = createPipeline(info, cache);
    m_report.pipelineCommitBytes = commit.stop();
    m_report.pipelineSeconds = SecondsSince(start);
    destroyModules();
    StorePipelineCache(m_device, cache, cacheFile, loaded, result == VK_SUCCESS);
    if (result != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        if (error) *error = std::format("cannot create the ray tracing pipeline (VkResult {})", int(result));
        return false;
    }
    if (statistics) PrintPipelineStatistics(m_device, m_pipeline, "callable pipeline");
    return true;
}

bool MaterialCallPipeline::createBindingTable(uint32_t groups, std::string* error)
{
    auto const& p = m_support.properties;
    const uint32_t handleSize = p.shaderGroupHandleSize;
    const uint32_t stride = AlignUp(handleSize, p.shaderGroupHandleAlignment);
    const uint32_t callableOffset = AlignUp(stride, p.shaderGroupBaseAlignment);
    const uint32_t callables = groups - 1;
    const VkDeviceSize bytes = VkDeviceSize(callableOffset) + VkDeviceSize(std::max(callables, 1u)) * stride +
        p.shaderGroupBaseAlignment; // room to align the base address
    std::vector<uint8_t> handles(size_t(groups) * handleSize);
    if (m_groupHandles(m_device, m_pipeline, 0, groups, handles.size(), handles.data()) != VK_SUCCESS) {
        if (error) *error = "cannot read the shader group handles";
        return false;
    }
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_table) != VK_SUCCESS) {
        if (error) *error = "cannot create the shader binding table";
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(m_device, m_table, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(m_physical, &memory);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == ~0u; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
            type = i;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &flags;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == ~0u || vkAllocateMemory(m_device, &allocation, nullptr, &m_tableMemory) != VK_SUCCESS) {
        if (error) *error = "cannot allocate the shader binding table";
        return false;
    }
    vkBindBufferMemory(m_device, m_table, m_tableMemory, 0);
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = m_table;
    const VkDeviceAddress address = vkGetBufferDeviceAddress(m_device, &addressInfo);
    const VkDeviceAddress base = (address + p.shaderGroupBaseAlignment - 1) /
        p.shaderGroupBaseAlignment * p.shaderGroupBaseAlignment;
    void* mapped = nullptr;
    vkMapMemory(m_device, m_tableMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
    auto* table = static_cast<uint8_t*>(mapped) + (base - address);
    std::memcpy(table, handles.data(), handleSize);
    for (uint32_t c = 0; c < callables; ++c)
        std::memcpy(table + callableOffset + c * stride, handles.data() + size_t(c + 1) * handleSize, handleSize);
    vkUnmapMemory(m_device, m_tableMemory);
    m_raygen = {base, stride, stride};
    m_callables = callables ? VkStridedDeviceAddressRegionKHR{base + callableOffset, stride,
                                                              VkDeviceSize(callables) * stride}
                            : VkStridedDeviceAddressRegionKHR{};
    return true;
}

void MaterialCallPipeline::trace(VkCommandBuffer commands, uint32_t threads) const
{
    const VkStridedDeviceAddressRegionKHR none{};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipeline);
    if (m_dynamicStack) m_setStackSize(commands, m_stackSize);
    m_traceRays(commands, &m_raygen, &none, &none, &m_callables, threads, 1, 1);
}

} // namespace prism::device::vulkan
