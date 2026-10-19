#pragma once

// The Vulkan device's material path: a ray tracing pipeline
// (VK_KHR_ray_tracing_pipeline) whose raygen shader runs the shared
// integrator and whose callable shaders are the MDL programs, one each,
// compiled separately. The raygen shader calls a program through CallShader
// with a MaterialCall payload (shaders/gpu/material_call.hlsl) instead of
// inlining every program; with shader execution reordering
// (VK_NV_ray_tracing_invocation_reorder) threads are regrouped by program
// first. Intersection stays in inline ray queries, as in the compute kernel.
//
// With VK_KHR_pipeline_library every shader is its own pipeline library,
// compiled once per device and kept: the raygen shader does not depend on the
// scene and compiles in the background from device creation, a scene compiles
// only the callables no earlier scene had, and its pipeline links them.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace prism::device::gpu {
struct MdlShaderCode;
}

namespace prism::device::vulkan {

// What the physical device offers for the callable path.
struct RayTracingSupport {
    bool pipeline = false;          // VK_KHR_ray_tracing_pipeline
    bool library = false;           // VK_KHR_pipeline_library
    bool reorder = false;           // VK_NV_ray_tracing_invocation_reorder (reorders, not just accepts hints)
    bool executableStatistics = false; // VK_KHR_pipeline_executable_properties
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

// When scenes use the callable path (PRISM_VULKAN_CALLABLES): by default
// (`auto`) above the kernel size threshold or after the compute kernel was
// found too large; 1 always (where ray tracing pipelines exist); 0 never.
enum class CallableMode { Never, Always, Auto };
CallableMode CallableModeSetting();
// Assembled compute kernels above this size use callables in Auto mode
// (PRISM_VULKAN_CALLABLE_THRESHOLD_KIB, default 320). Smaller kernels have
// little material code: the compute kernel compiles about as fast as the
// raygen shader and runs faster (CornellBox, 256 KB: 1.7x); from the lion
// (392 KB, 1 program, 3.3x slower) up, callables are faster or equal.
size_t CallableThresholdBytes();
// Whether to reorder threads by material program (PRISM_VULKAN_REORDER=1, where supported). Off by
// default: measured 2-8% slower on most scenes, 16% faster on the MaterialEggs countertop.
bool ReorderSetting();
// Whether to build pipelines from per-shader libraries (PRISM_VULKAN_PIPELINE_LIBRARIES=0: one
// monolithic pipeline per material set, compiled whole).
bool PipelineLibrariesSetting();
// The define of PRISM_VULKAN_SHADER_SALT (empty when unset): unique shaders for cold-compile measurements.
std::string ShaderSaltDefine();
// Whether to print the driver's per-shader statistics (PRISM_VULKAN_PIPELINE_STATS=1).
bool PipelineStatisticsSetting();

// Shader cache files (SPIR-V, pipeline cache data). They are written whole
// (a flushed temporary file renamed over the old one) with a trailer of their
// size and digest; a file whose trailer does not match (torn by a crash, or
// of an older format) reads as empty and is compiled again. `extra` is a
// value stored with the data.
void WriteCacheFile(std::filesystem::path const& path, void const* data, size_t size, uint64_t extra = 0);
std::vector<char> ReadCacheFile(std::filesystem::path const& path, uint64_t* extra = nullptr);
// Word-aligned bytes that start with the SPIR-V magic number.
bool PlausibleSpirv(std::vector<char> const& bytes);
// Pipeline cache data whose header (VkPipelineCacheHeaderVersionOne) names
// this device and its pipeline cache UUID.
bool PipelineCacheDataMatches(std::vector<char> const& data, VkPhysicalDeviceProperties const& properties);

// The HLSL of the raygen shader: the kernel without MDL code
// (PRISM_CALL_MATERIALS). It depends on no scene, except with reordering
// (the hint bits cover `programs`).
bool RaygenSource(bool reorder, uint32_t programs, std::string* source, std::string* error);
// The HLSL of the callable pipeline: the raygen shader, then one callable per
// program of `code` (that program's dispatch and, with `trim`, only the
// functions of the link unit it reaches), each compiled alone with the
// kMaterialCallProfile library profile.
bool MaterialCallSources(gpu::MdlShaderCode const& code, bool reorder, std::vector<std::string>* sources,
                         std::string* error, bool trim = true);
extern const char* const kMaterialCallProfile; // lib_6_6

// Prints the statistics of every executable of `pipeline` (created with
// VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR).
void PrintPipelineStatistics(VkDevice device, VkPipeline pipeline, std::string const& label);

// One compiled shader of the callable path: SPIR-V (from the cache or DXC)
// and, with libraries, its pipeline library.
struct ShaderBuild {
    std::vector<uint32_t> spirv;
    VkPipeline library = VK_NULL_HANDLE;
    std::string error;
    bool dxcCompiled = false;       // DXC ran (otherwise the SPIR-V cache had it)
    double dxcSeconds = 0.0;
    uint64_t dxcPeakBytes = 0;
    double driverSeconds = 0.0;     // the library's vkCreateRayTracingPipelinesKHR
    bool driverCacheHit = false;    // its pipeline cache file existed
    double readySeconds = 0.0;      // seconds after the request that it was ready
};

// The shaders of the callable path compiled on one device, by source, shared
// by every scene the device renders. Thread safe: concurrent requests for one
// source wait for a single compile.
class MaterialCallShaders {
public:
    MaterialCallShaders(VkDevice device, VkPhysicalDeviceProperties const& properties,
                        RayTracingSupport const& support, VkPipelineLayout layout, std::string pipelineCacheTag);
    ~MaterialCallShaders(); // waits for background compiles
    MaterialCallShaders(MaterialCallShaders const&) = delete;
    MaterialCallShaders& operator=(MaterialCallShaders const&) = delete;

    // Whether shaders become pipeline libraries (supported and not disabled).
    bool libraries() const { return m_libraries; }
    // Starts compiling the scene-independent raygen shader on a background thread.
    void prewarmRaygen();
    // The shader of `source` (SPIR-V and, with libraries, its library); compiles it once.
    std::shared_ptr<const ShaderBuild> shader(std::string const& source, bool raygen);

private:
    std::shared_ptr<ShaderBuild> compile(std::string const& source, bool raygen);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties{};
    RayTracingSupport m_support;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::string m_cacheTag;
    bool m_libraries = false;
    PFN_vkCreateRayTracingPipelinesKHR m_createPipelines = nullptr;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<const ShaderBuild>>> m_shaders;
    std::future<void> m_prewarm;
};

// Costs of building one pipeline, for PRISM_GPU_VERBOSE.
struct CallableBuildReport {
    uint32_t programs = 0;
    uint32_t compiled = 0;          // callables DXC compiled (others came from the cache)
    uint32_t librariesCompiled = 0; // callable libraries the driver compiled for this pipeline
    uint32_t librariesCached = 0;   // ... of them from a pipeline cache file
    bool libraries = false;         // linked from pipeline libraries
    double shadersSeconds = 0.0;    // all shader work (DXC and, with libraries, driver), in parallel
    double dxcMaxSeconds = 0.0;     // slowest single compile
    uint64_t dxcPeakBytes = 0;      // largest single DXC process
    double driverMaxSeconds = 0.0;  // slowest library compile
    double driverSumSeconds = 0.0;  // all library compiles
    bool raygenCompiled = false;    // DXC compiled the raygen shader for this pipeline
    double raygenSeconds = 0.0;     // its DXC
    double raygenDriverSeconds = 0.0; // its library
    double raygenWaitSeconds = 0.0; // waited for it after the callables were ready
    uint64_t raygenPeakBytes = 0;
    double pipelineSeconds = 0.0;   // vkCreateRayTracingPipelinesKHR: whole pipeline, or the link
    uint32_t pipelineThreads = 1;   // threads that joined its deferred compile
    int64_t pipelineCommitBytes = 0; // process commit growth at its peak during pipeline creation (Windows)
    bool pipelineCacheHit = false;  // the pipeline cache file existed
};

class MaterialCallPipeline {
public:
    MaterialCallPipeline(VkDevice device, VkPhysicalDevice physical, RayTracingSupport const& support,
                         std::string pipelineCacheTag);
    ~MaterialCallPipeline();
    MaterialCallPipeline(MaterialCallPipeline const&) = delete;
    MaterialCallPipeline& operator=(MaterialCallPipeline const&) = delete;

    // Compiles (or takes from `shaders`) the raygen shader and one callable
    // per program of `code`, and creates the pipeline, linked from libraries
    // where `shaders` makes them, and its shader binding table.
    bool build(gpu::MdlShaderCode const& code, VkPipelineLayout layout, bool reorder, MaterialCallShaders& shaders,
               std::string* error);
    // Records the launch of `threads` raygen invocations (descriptor set bound by the caller).
    void trace(VkCommandBuffer commands, uint32_t threads) const;
    VkPipeline pipeline() const { return m_pipeline; }
    CallableBuildReport const& report() const { return m_report; }

private:
    bool createBindingTable(uint32_t groups, std::string* error);
    VkResult createPipeline(VkRayTracingPipelineCreateInfoKHR const& info, VkPipelineCache cache);
    bool createMonolithic(std::vector<std::shared_ptr<const ShaderBuild>> const& shaders, VkPipelineLayout layout,
                          std::string* error);
    bool link(std::vector<std::shared_ptr<const ShaderBuild>> const& shaders, VkPipelineLayout layout,
              std::string* error);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    RayTracingSupport m_support;
    std::string m_cacheTag;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkBuffer m_table = VK_NULL_HANDLE;
    VkDeviceMemory m_tableMemory = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR m_raygen{};
    VkStridedDeviceAddressRegionKHR m_callables{};
    CallableBuildReport m_report;
    PFN_vkCreateRayTracingPipelinesKHR m_createPipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR m_groupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR m_traceRays = nullptr;
    PFN_vkCreateDeferredOperationKHR m_createDeferred = nullptr;
    PFN_vkDestroyDeferredOperationKHR m_destroyDeferred = nullptr;
    PFN_vkGetDeferredOperationMaxConcurrencyKHR m_deferredConcurrency = nullptr;
    PFN_vkGetDeferredOperationResultKHR m_deferredResult = nullptr;
    PFN_vkDeferredOperationJoinKHR m_joinDeferred = nullptr;
};

} // namespace prism::device::vulkan
