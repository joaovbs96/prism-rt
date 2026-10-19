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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
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
#endif

namespace prism::device::vulkan {

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

// Writes a cache file whole: concurrent readers see the previous complete
// file or this one.
void WriteFileAtomically(std::filesystem::path const& path, void const* data, size_t size)
{
    static std::atomic<uint64_t> serial{0};
    std::error_code error;
    const auto temporary = std::filesystem::path(path.string() + std::format(".{}.{}.tmp",
        std::hash<std::thread::id>{}(std::this_thread::get_id()), ++serial));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(static_cast<const char*>(data), std::streamsize(size))) {
            out.close();
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

std::vector<char> ReadFile(std::filesystem::path const& path)
{
    std::vector<char> bytes;
    if (std::ifstream file{path, std::ios::binary})
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return bytes;
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

// DXC processes run at once for the callables (PRISM_DXC_JOBS; each is
// memory capped by PRISM_DXC_MEMORY_MIB).
unsigned DxcJobs()
{
    if (const char* value = Setting("PRISM_DXC_JOBS")) return unsigned(std::max(1, std::atoi(value)));
    return std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 4u);
}

const gpu::HlslStage kLibraryStage{"lib_6_6", ""};

} // namespace

CallableMode CallableModeSetting()
{
    const char* value = Setting("PRISM_VULKAN_CALLABLES");
    if (!value) return CallableMode::Auto;
    if (std::strcmp(value, "0") == 0) return CallableMode::Never;
    if (std::strcmp(value, "1") == 0) return CallableMode::Always;
    return CallableMode::Auto;
}

size_t CallableThresholdBytes()
{
    if (const char* value = Setting("PRISM_VULKAN_CALLABLE_THRESHOLD_KIB"))
        return size_t(std::max(0ll, std::atoll(value))) << 10;
    return size_t(1024) << 10;
}

bool ReorderSetting()
{
    const char* value = Setting("PRISM_VULKAN_REORDER");
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
    if (properties(device, &info, &count, nullptr) != VK_SUCCESS) return;
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

MaterialCallPipeline::MaterialCallPipeline(VkDevice device, VkPhysicalDevice physical,
                                           RayTracingSupport const& support, std::string pipelineCacheTag)
    : m_device(device), m_physical(physical), m_support(support), m_cacheTag(std::move(pipelineCacheTag))
{
    m_createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    m_groupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
    m_traceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR"));
}

MaterialCallPipeline::~MaterialCallPipeline()
{
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_table) vkDestroyBuffer(m_device, m_table, nullptr);
    if (m_tableMemory) vkFreeMemory(m_device, m_tableMemory, nullptr);
}

bool MaterialCallPipeline::compileShaders(gpu::MdlShaderCode const& code, bool reorder,
                                          std::vector<std::vector<uint32_t>>* modules, std::string* error)
{
    const uint32_t programs = uint32_t(code.programDispatch.size());
    // Module 0: the raygen shader (the kernel without MDL code); then one
    // callable per program: the link unit with only that program's dispatch.
    std::vector<std::string> sources(programs + 1);
    std::string defines = "#define PRISM_CALL_MATERIALS 1\n";
    if (reorder) {
        uint32_t bits = 1;
        while ((1u << bits) < programs && bits < 16) ++bits;
        defines += std::format("#define PRISM_REORDER_MATERIALS 1\n#define PRISM_REORDER_HINT_BITS {}u\n", bits);
    }
    if (!gpu::AssembleShader("gpu/kernel.hlsl", {"gpu/vulkan"}, std::string(), &sources[0], error)) return false;
    sources[0] = defines + sources[0];
    for (uint32_t p = 0; p < programs; ++p) {
        const std::string mdl = code.targetCode + code.programDispatch[p] + code.sceneData;
        if (!gpu::AssembleShader("gpu/vulkan/material_callable.hlsl", {"gpu/vulkan"}, mdl, &sources[p + 1], error))
            return false;
    }
    const auto directory = gpu::ShaderCacheDirectory("vulkan");
    const std::string signature = gpu::HlslCompilerSignature(gpu::HlslTarget::Spirv, kLibraryStage);
    std::vector<std::filesystem::path> cached(sources.size());
    std::vector<size_t> missing;
    modules->assign(sources.size(), {});
    for (size_t i = 0; i < sources.size(); ++i) {
        cached[i] = directory / (gpu::SourceDigest(sources[i] + signature) + ".spv");
        const std::vector<char> bytes = ReadFile(cached[i]);
        if (bytes.empty() || bytes.size() % 4 != 0) {
            missing.push_back(i);
            continue;
        }
        (*modules)[i].resize(bytes.size() / 4);
        std::memcpy((*modules)[i].data(), bytes.data(), bytes.size());
    }
    m_report.programs = programs;
    m_report.compiled = uint32_t(std::count_if(missing.begin(), missing.end(), [](size_t i) { return i > 0; }));
    if (missing.empty()) return true;
    std::cerr << std::format("PrismRT Vulkan: compiling {} of {} material callables and {} raygen shader "
                             "({} DXC jobs); this happens once per material set\n",
                             m_report.compiled, programs, missing.front() == 0 ? "the" : "no", DxcJobs());
    progress::Detail(Phase::Kernel, std::format("DXC to SPIR-V, {} callables", missing.size()));
    std::mutex mutex;
    std::string firstError;
    std::atomic<size_t> done{0};
    const auto start = Clock::now();
    tbb::task_arena arena{int(DxcJobs())};
    arena.execute([&] {
        tbb::parallel_for(size_t(0), missing.size(), size_t(1), [&](size_t m) {
            const size_t i = missing[m];
            std::vector<char> bytes;
            std::string reason;
            gpu::HlslCompileStats stats;
            const bool ok = gpu::CompileHlsl(sources[i], gpu::HlslTarget::Spirv, directory, &bytes, &reason,
                                             kLibraryStage, &stats);
            std::lock_guard lock(mutex);
            if (i == 0) {
                m_report.raygenSeconds = stats.seconds;
                m_report.raygenPeakBytes = stats.peakBytes;
            } else {
                m_report.dxcMaxSeconds = std::max(m_report.dxcMaxSeconds, stats.seconds);
                m_report.dxcPeakBytes = std::max(m_report.dxcPeakBytes, stats.peakBytes);
            }
            if (gpu::GpuVerbose())
                std::cerr << std::format("PrismRT Vulkan: DXC {} {:.1f} s, peak {} MiB, {} KB SPIR-V\n",
                                         i == 0 ? std::string("raygen") : std::format("callable {}", i - 1),
                                         stats.seconds, stats.peakBytes >> 20, bytes.size() >> 10);
            progress::Detail(Phase::Kernel, std::format("DXC to SPIR-V, callable {} of {}", ++done, missing.size()));
            if (!ok) {
                if (firstError.empty())
                    firstError = (i == 0 ? std::string("raygen shader: ") : std::format("material callable {}: ", i - 1))
                        + reason;
                return;
            }
            WriteFileAtomically(cached[i], bytes.data(), bytes.size());
            (*modules)[i].resize(bytes.size() / 4);
            std::memcpy((*modules)[i].data(), bytes.data(), bytes.size());
        });
    });
    m_report.dxcWallSeconds = SecondsSince(start);
    if (!firstError.empty()) {
        if (error) *error = firstError;
        return false;
    }
    return true;
}

bool MaterialCallPipeline::build(gpu::MdlShaderCode const& code, VkPipelineLayout layout, bool reorder,
                                 std::string* error)
{
    if (!m_createPipelines || !m_groupHandles || !m_traceRays) {
        if (error) *error = "ray tracing pipeline functions unavailable";
        return false;
    }
    std::vector<std::vector<uint32_t>> modules;
    if (!compileShaders(code, reorder, &modules, error)) return false;

    progress::Detail(Phase::Kernel, "creating the Vulkan ray tracing pipeline");
    std::vector<VkShaderModule> shaderModules(modules.size(), VK_NULL_HANDLE);
    std::vector<VkPipelineShaderStageCreateInfo> stages(modules.size());
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups(modules.size());
    std::string digests;
    bool created = true;
    for (size_t i = 0; i < modules.size(); ++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = modules[i].size() * 4;
        moduleInfo.pCode = modules[i].data();
        created = created && vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shaderModules[i]) == VK_SUCCESS;
        digests += gpu::SourceDigest(std::string(reinterpret_cast<const char*>(modules[i].data()), moduleInfo.codeSize));
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

    // The driver translates every callable here; keep its result for later runs.
    const auto cacheFile = gpu::ShaderCacheDirectory("vulkan") /
        (gpu::SourceDigest(digests + (statistics ? "|stats" : "")) + "-" + m_cacheTag + ".vkpc");
    const std::vector<char> cacheData = ReadFile(cacheFile);
    m_report.pipelineCacheHit = !cacheData.empty();
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = cacheData.size();
    cacheInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &cache) != VK_SUCCESS) {
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;
        if (vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE;
    }
    const auto start = Clock::now();
    CommitSampler commit;
    const VkResult result = m_createPipelines(m_device, VK_NULL_HANDLE, cache, 1, &info, nullptr, &m_pipeline);
    m_report.pipelineCommitBytes = commit.stop();
    m_report.pipelineSeconds = SecondsSince(start);
    destroyModules();
    if (cache) {
        size_t size = 0;
        if (result == VK_SUCCESS && vkGetPipelineCacheData(m_device, cache, &size, nullptr) == VK_SUCCESS &&
            size > 0 && size != cacheData.size()) {
            std::vector<char> data(size);
            if (vkGetPipelineCacheData(m_device, cache, &size, data.data()) == VK_SUCCESS)
                WriteFileAtomically(cacheFile, data.data(), size);
        }
        vkDestroyPipelineCache(m_device, cache, nullptr);
    }
    if (result != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        if (error) *error = std::format("cannot create the ray tracing pipeline (VkResult {})", int(result));
        return false;
    }
    if (statistics) PrintPipelineStatistics(m_device, m_pipeline, "callable pipeline");
    return createBindingTable(uint32_t(groups.size()), error);
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
    m_traceRays(commands, &m_raygen, &none, &none, &m_callables, threads, 1, 1);
}

} // namespace prism::device::vulkan
