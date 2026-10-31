#pragma once

// The Vulkan device's denoiser (docs/denoising.md): nvpro-samples' vk_oidn,
// Open Image Denoise's RT filter network (the OIDN 2.5.1 weights) as Vulkan
// compute, recorded into the device's queue on buffers the denoiser holds.
// Tensor cores (VK_KHR_cooperative_matrix) on NVIDIA GPUs, vk_oidn's generic
// convolutions elsewhere (VK_OIDN_COOPMAT=0 forces them).
//
// Like OIDN's CPU device (denoiser.cpp) it prefilters the guides when asked:
// OIDN's albedo and normal networks, packed for vk_oidn by
// tools/ci/fetch_vk_oidn.ps1 and embedded in the build, run first and the
// colour filter then takes them as clean (cleanAux).

#include "../denoiser.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace prism::device::vulkan {

// The device features vk_oidn needs. Before vkCreateDevice: when the
// physical device has them all, sets them (only to VK_TRUE) in the creation
// structures and appends the extension of the optional cooperative matrices
// (*cooperativeMatrix tells whether `coopMat` must be chained); otherwise
// changes nothing and returns false with what is missing.
bool EnableDenoiseFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2& features,
                           VkPhysicalDeviceVulkan11Features& vulkan11, VkPhysicalDeviceVulkan12Features& vulkan12,
                           VkPhysicalDeviceVulkan13Features& vulkan13,
                           VkPhysicalDeviceCooperativeMatrixFeaturesKHR& coopMat, bool* cooperativeMatrix,
                           std::vector<const char*>& extensions, std::string* missing);

// What the denoiser uses of the render device. Submissions to `queue` hold
// *queueMutex.
struct DenoiseDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    std::mutex* queueMutex = nullptr;
    std::vector<const char*> extensions; // enabled on the device
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
};

class VulkanDenoiser final : public Denoiser {
public:
    // Null with the reason when vk_oidn cannot run on the device.
    static std::shared_ptr<VulkanDenoiser> Create(DenoiseDevice const& device, std::string* error);
    ~VulkanDenoiser() override;

    // "vk_oidn 0.2.0 Vulkan (cooperative matrices)" or "(generic convolutions)".
    std::string Name() const override;
    // Host images: uploaded, denoised, read back.
    void Denoise(DenoiseRequest const& request) override;
    // A whole-image render's accumulation, where it lives (`accumulation`,
    // a device address: GpuRenderDevice::RenderImage's sums, the light AOV
    // sums after the colour's, four entries per pixel or, with `guidesOnly`,
    // two: kFrameGuidesOnly), averaged over each pixel's `samples` by a
    // resolve pass; *output receives width*height RGB triples.
    void DenoiseAccumulation(VkDeviceAddress accumulation, std::vector<int> const& samples, unsigned width,
                             unsigned height, DenoiseSettings const& settings, bool guidesOnly,
                             std::vector<float>* output);
    // The render device is going away: frees every Vulkan object. Later
    // calls of Denoise use OIDN's CPU device.
    void Shutdown();

    struct Impl;

private:
    explicit VulkanDenoiser(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::device::vulkan
