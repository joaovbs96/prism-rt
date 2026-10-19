#pragma once

// The Vulkan device's path for material sets too large for one kernel: a ray
// tracing pipeline (VK_KHR_ray_tracing_pipeline) whose raygen shader runs the
// shared integrator and whose callable shaders are the MDL programs, one
// each, compiled separately. The raygen shader calls a program through
// CallShader with a MaterialCall payload (shaders/gpu/material_call.hlsl)
// instead of inlining every program; with shader execution reordering
// (VK_NV_ray_tracing_invocation_reorder) threads are regrouped by program
// first. Intersection stays in inline ray queries, as in the compute kernel.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace prism::device::gpu {
struct MdlShaderCode;
}

namespace prism::device::vulkan {

// What the physical device offers for the callable path.
struct RayTracingSupport {
    bool pipeline = false;          // VK_KHR_ray_tracing_pipeline
    bool reorder = false;           // VK_NV_ray_tracing_invocation_reorder (reorders, not just accepts hints)
    bool executableStatistics = false; // VK_KHR_pipeline_executable_properties
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

// When scenes use the callable path (PRISM_VULKAN_CALLABLES: 0 never, 1
// always, otherwise above the kernel size threshold or after the compute
// kernel was found too large).
enum class CallableMode { Never, Always, Auto };
CallableMode CallableModeSetting();
// Assembled compute kernels above this size use callables in Auto mode
// (PRISM_VULKAN_CALLABLE_THRESHOLD_KIB, default 1024).
size_t CallableThresholdBytes();
// Whether to reorder threads by material program (PRISM_VULKAN_REORDER=1, where supported). Off by
// default: measured 2-8% slower on most scenes, 16% faster on the MaterialEggs countertop.
bool ReorderSetting();
// Whether to print the driver's per-shader statistics (PRISM_VULKAN_PIPELINE_STATS=1).
bool PipelineStatisticsSetting();

// The HLSL of the callable pipeline: the raygen shader (the kernel without
// MDL code, PRISM_CALL_MATERIALS), then one callable per program of `code`
// (the link unit with only that program's dispatch), each compiled alone with
// the kMaterialCallProfile library profile.
bool MaterialCallSources(gpu::MdlShaderCode const& code, bool reorder, std::vector<std::string>* sources,
                         std::string* error);
extern const char* const kMaterialCallProfile; // lib_6_6

// Prints the statistics of every executable of `pipeline` (created with
// VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR).
void PrintPipelineStatistics(VkDevice device, VkPipeline pipeline, std::string const& label);

// Costs of building one pipeline, for PRISM_GPU_VERBOSE.
struct CallableBuildReport {
    uint32_t programs = 0;
    uint32_t compiled = 0;          // callables DXC compiled (others came from the cache)
    double dxcWallSeconds = 0.0;    // all DXC work, compiles run in parallel
    double dxcMaxSeconds = 0.0;     // slowest single compile
    uint64_t dxcPeakBytes = 0;      // largest single DXC process
    double raygenSeconds = 0.0;
    uint64_t raygenPeakBytes = 0;
    double pipelineSeconds = 0.0;   // vkCreateRayTracingPipelinesKHR
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

    // Compiles the raygen shader and one callable per program of `code`
    // (cached by source), and creates the pipeline and its shader binding table.
    bool build(gpu::MdlShaderCode const& code, VkPipelineLayout layout, bool reorder, std::string* error);
    // Records the launch of `threads` raygen invocations (descriptor set bound by the caller).
    void trace(VkCommandBuffer commands, uint32_t threads) const;
    VkPipeline pipeline() const { return m_pipeline; }
    CallableBuildReport const& report() const { return m_report; }

private:
    bool compileShaders(gpu::MdlShaderCode const& code, bool reorder,
                        std::vector<std::vector<uint32_t>>* modules, std::string* error);
    bool createBindingTable(uint32_t groups, std::string* error);
    VkResult createPipeline(VkRayTracingPipelineCreateInfoKHR const& info, VkPipelineCache cache);

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
