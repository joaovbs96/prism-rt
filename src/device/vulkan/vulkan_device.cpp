// Vulkan render device. The kernel is shaders/gpu/kernel.hlsl with the
// scene's MDL code, compiled to SPIR-V with DXC and cached by source hash
// (the same source the Metal device compiles to DXIL). Every resource slot is a binding of one descriptor set; the scene is
// mirrored with one bottom-level acceleration structure per prototype and
// one top-level structure. The structure follows the Metal device.

#include "vulkan_device.h"

#include "../gpu/gpu_materials.h"
#include "../gpu/hlsl_compiler.h"
#include "../gpu/shader_sources.h"
#include "../../scene.h"
#include "../../texture_service.h"

#include <tbb/parallel_for.h>
#include <vulkan/vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <unordered_map>

namespace prism::device {

using prism::device::gpu::GpuSceneData;

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kMaxImages = 4096;

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

const char* ResultText(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "success";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
    case VK_ERROR_DEVICE_LOST: return "device lost";
    case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
    default: return "Vulkan error";
    }
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer storage;
    VkDeviceAddress address = 0;
};

} // namespace

bool CompileVulkanKernel(std::string const& mdlCode, std::vector<uint32_t>* spirv, std::string* error)
{
    std::string source;
    if (!gpu::AssembleShader("gpu/kernel.hlsl", {"gpu/vulkan"}, mdlCode, &source, error)) return false;
    const auto cacheDirectory = gpu::ShaderCacheDirectory("vulkan");
    const auto cached = cacheDirectory / (gpu::SourceDigest(source) + ".spv");
    std::vector<char> bytes;
    if (std::ifstream file{cached, std::ios::binary})
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0) {
        if (!gpu::CompileHlsl(source, gpu::HlslTarget::Spirv, cacheDirectory, &bytes, error)) return false;
        std::ofstream(cached, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    }
    spirv->resize(bytes.size() / 4);
    std::memcpy(spirv->data(), bytes.data(), bytes.size());
    return true;
}

struct VulkanDevice::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{};
    float timestampPeriod = 1.0f;
    std::string name;

    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getStructureAddress = nullptr;

    std::atomic<bool> dirty{true};
    SceneManager const* boundScene = nullptr;
    std::unique_ptr<GpuSceneData> data;
    uint32_t mdlTextureCount = 1;

    Buffer buffers[kSlotCount];
    std::vector<VkImageView> imageViews;
    struct CachedImage {
        std::weak_ptr<const prism::TextureResource> resource;
        Image image;
        bool floatPrecision = false;
    };
    std::unordered_map<const prism::TextureResource*, CachedImage> imageCache;
    std::vector<AccelerationStructure> bottomLevel;
    AccelerationStructure topLevel;
    VkSampler samplers[kSamplerCount] = {};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkPipeline> pipelines;
    // The generated MDL code `pipeline` was built from.
    std::shared_ptr<const gpu::MdlShaderCode> pipelineCode;
    VkQueryPool timestamps = VK_NULL_HANDLE;

    ~Impl();
    bool initialize(std::string* reason);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer, bool hostVisible = true);
    void destroyBuffer(Buffer& buffer);
    bool upload(int slot, const void* bytes, size_t size);
    void destroyStructure(AccelerationStructure& structure);
    VkCommandBuffer beginCommands();
    bool submit(VkCommandBuffer commands, std::string* error);
    bool ensureScene(SceneManager& scene, std::string* error);
    bool buildPipeline(std::string* error);
    bool uploadImages(std::string* error);
    bool buildAccelerationStructures(std::string* error);
    bool writeDescriptors(std::string* error);
    void* renderBuffer(int slot, size_t bytes);
    bool dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error);
};

VulkanDevice::Impl::~Impl()
{
    if (!device) {
        if (instance) vkDestroyInstance(instance, nullptr);
        return;
    }
    vkDeviceWaitIdle(device);
    for (auto& [key, p] : pipelines) vkDestroyPipeline(device, p, nullptr);
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (VkSampler s : samplers) if (s) vkDestroySampler(device, s, nullptr);
    for (auto& structure : bottomLevel) destroyStructure(structure);
    destroyStructure(topLevel);
    for (auto& [key, cached] : imageCache) {
        vkDestroyImageView(device, cached.image.view, nullptr);
        vkDestroyImage(device, cached.image.image, nullptr);
        vkFreeMemory(device, cached.image.memory, nullptr);
    }
    for (auto& buffer : buffers) destroyBuffer(buffer);
    if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}

bool VulkanDevice::Impl::initialize(std::string* reason)
{
    auto fail = [&](std::string text) {
        if (reason) *reason = std::move(text);
        return false;
    };
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "PrismRT";
    application.apiVersion = VK_API_VERSION_1_3;
    // Portability drivers (MoltenVK, KosmicKrisp) enumerate only on request.
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data());
    std::vector<const char*> extensions;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    for (auto const& e : available)
        if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = uint32_t(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance); r != VK_SUCCESS)
        return fail(std::string("no Vulkan instance (") + ResultText(r) + ")");

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    const char* required[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                              VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    std::string rejected;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> deviceExtensions(count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, deviceExtensions.data());
        bool complete = properties.apiVersion >= VK_API_VERSION_1_3;
        for (const char* r : required) {
            bool found = false;
            for (auto const& e : deviceExtensions) found = found || std::strcmp(e.extensionName, r) == 0;
            complete = complete && found;
        }
        if (!complete) {
            if (!rejected.empty()) rejected += ", ";
            rejected += properties.deviceName;
            continue;
        }
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> familyProperties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, familyProperties.data());
        for (uint32_t f = 0; f < families; ++f) {
            if (familyProperties[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                physical = candidate;
                queueFamily = f;
                name = properties.deviceName;
                timestampPeriod = properties.limits.timestampPeriod;
                break;
            }
        }
        // Prefer discrete GPUs.
        if (physical == candidate && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (!physical)
        return fail(devices.empty() ? "no Vulkan device"
                                    : "no Vulkan device supports ray queries (" + rejected + ")");

    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rayQuery.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    acceleration.accelerationStructure = VK_TRUE;
    acceleration.pNext = &rayQuery;
    VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12.bufferDeviceAddress = VK_TRUE;
    vulkan12.descriptorIndexing = VK_TRUE;
    vulkan12.runtimeDescriptorArray = VK_TRUE;
    vulkan12.descriptorBindingPartiallyBound = VK_TRUE;
    vulkan12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vulkan12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vulkan12.pNext = &acceleration;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.features.samplerAnisotropy = VK_TRUE;
    features.pNext = &vulkan12;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(std::size(required));
    deviceInfo.ppEnabledExtensionNames = required;
    if (VkResult r = vkCreateDevice(physical, &deviceInfo, nullptr, &device); r != VK_SUCCESS)
        return fail(std::string("cannot create the Vulkan device (") + ResultText(r) + ")");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    accelerationProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &accelerationProperties;
    vkGetPhysicalDeviceProperties2(physical, &properties2);

    createAccelerationStructure = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    destroyAccelerationStructure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    getBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    getStructureAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    if (!createAccelerationStructure || !destroyAccelerationStructure || !getBuildSizes || !cmdBuild ||
        !getStructureAddress)
        return fail("acceleration structure functions unavailable");

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device, &fenceInfo, nullptr, &fence);
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps);

    // Samplers: index = wrapS * 3 + wrapT, wraps clamp / repeat / mirror.
    const VkSamplerAddressMode modes[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                           VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for (int s = 0; s < 3; ++s) for (int t = 0; t < 3; ++t) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = modes[s];
        info.addressModeV = modes[t];
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = 16.0f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(device, &info, nullptr, &samplers[s * 3 + t]);
    }

    // One set: a storage buffer per slot, the acceleration structure, and
    // the image and sampler arrays.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = slot;
        binding.descriptorType = slot == kSlotAccel ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(binding);
    }
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = kSlotImageBase + 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerBinding.descriptorCount = kSamplerCount;
    samplerBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    samplerBinding.pImmutableSamplers = samplers;
    bindings.push_back(samplerBinding);
    VkDescriptorSetLayoutBinding imageBinding{};
    imageBinding.binding = kSlotImageBase;
    imageBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    imageBinding.descriptorCount = kMaxImages;
    imageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings.push_back(imageBinding); // the variable-count binding must be last
    std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    flags.back() = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagInfo.bindingCount = uint32_t(flags.size());
    flagInfo.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &flagInfo;
    layoutInfo.bindingCount = uint32_t(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS)
        return fail("cannot create the descriptor set layout");
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlotCount},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxImages},
    };
    VkDescriptorPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCreate.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCreate.maxSets = 1;
    poolCreate.poolSizeCount = uint32_t(std::size(sizes));
    poolCreate.pPoolSizes = sizes;
    vkCreateDescriptorPool(device, &poolCreate, nullptr, &descriptorPool);
    return true;
}

uint32_t VulkanDevice::Impl::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return ~0u;
}

bool VulkanDevice::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer, bool hostVisible)
{
    *buffer = {};
    buffer->size = std::max<VkDeviceSize>(16, (size + 15) & ~VkDeviceSize(15));
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = buffer->size;
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(device, &info, nullptr, &buffer->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer->buffer, &requirements);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &flags;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, hostVisible
        ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == ~0u ||
        vkAllocateMemory(device, &allocation, nullptr, &buffer->memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer->buffer, nullptr);
        *buffer = {};
        return false;
    }
    vkBindBufferMemory(device, buffer->buffer, buffer->memory, 0);
    if (hostVisible) vkMapMemory(device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped);
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer->buffer;
    buffer->address = vkGetBufferDeviceAddress(device, &addressInfo);
    return true;
}

void VulkanDevice::Impl::destroyBuffer(Buffer& buffer)
{
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

bool VulkanDevice::Impl::upload(int slot, const void* bytes, size_t size)
{
    destroyBuffer(buffers[slot]);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (!createBuffer(size, usage, &buffers[slot])) return false;
    if (bytes && size) std::memcpy(buffers[slot].mapped, bytes, size);
    return true;
}

void VulkanDevice::Impl::destroyStructure(AccelerationStructure& structure)
{
    if (structure.handle) destroyAccelerationStructure(device, structure.handle, nullptr);
    destroyBuffer(structure.storage);
    structure = {};
}

VkCommandBuffer VulkanDevice::Impl::beginCommands()
{
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = commandPool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &info, &commands);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    return commands;
}

bool VulkanDevice::Impl::submit(VkCommandBuffer commands, std::string* error)
{
    vkEndCommandBuffer(commands);
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &commands;
    vkResetFences(device, 1, &fence);
    VkResult result = vkQueueSubmit(queue, 1, &info, fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(device, commandPool, 1, &commands);
    if (result != VK_SUCCESS) {
        if (error) *error = std::string("Vulkan submission failed: ") + ResultText(result);
        return false;
    }
    return true;
}

bool VulkanDevice::Impl::buildPipeline(std::string* error)
{
    auto const start = Clock::now();
    const auto shared = gpu::CachedMdlShaderCode(data->mdlMaterials);
    gpu::MdlShaderCode const& code = *shared;
    if (!code.success)
        std::cerr << "PrismRT Vulkan: MDL materials unavailable on the GPU (" << code.error << ")\n";
    gpu::ApplyMdlShaderCode(*data, code);
    mdlTextureCount = uint32_t(data->mdlTextures.size());
    upload(kSlotArgumentBlocks, code.argumentBlocks.data(), code.argumentBlocks.size());
    upload(kSlotRoData, code.roData.data(), code.roData.size());
    // The same generated code (an edit that kept the materials) keeps its pipeline.
    if (shared == pipelineCode && pipeline != VK_NULL_HANDLE) return true;
    pipelineCode = nullptr;
    std::vector<uint32_t> spirv;
    if (!CompileVulkanKernel(code.code, &spirv, error)) return false;
    const std::string key = gpu::SourceDigest(std::string(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4));
    if (auto found = pipelines.find(key); found != pipelines.end()) {
        pipeline = found->second;
        pipelineCode = shared;
        return true;
    }
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirv.size() * 4;
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        if (error) *error = "cannot create the shader module";
        return false;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = pipelineLayout;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) {
        if (error) *error = std::string("cannot create the compute pipeline: ") + ResultText(result);
        pipeline = VK_NULL_HANDLE;
        return false;
    }
    pipelines[key] = pipeline;
    pipelineCode = shared;
    if (gpu::GpuVerbose())
        std::cerr << std::format("PrismRT Vulkan: shader setup {:.1f} s\n", SecondsSince(start));
    return true;
}

bool VulkanDevice::Impl::uploadImages(std::string* error)
{
    GpuSceneData const& d = *data;
    imageViews.assign(d.images.size(), VK_NULL_HANDLE);
    if (d.images.size() > kMaxImages) {
        if (error) *error = "too many images for the Vulkan device";
        return false;
    }
    std::vector<GpuImage> info(d.images.size());
    auto service = prism::TextureService::Acquire();
    for (size_t i = 0; i < d.images.size(); ++i) {
        auto const& resource = d.images[i];
        bool floatPrecision = false;
        for (auto const& environment : d.environments) floatPrecision |= environment.info.y == i;
        info[i].info = {uint32_t(resource->width()), uint32_t(resource->height()),
                        uint32_t(resource->channels()), uint32_t(i)};
        auto found = imageCache.find(resource.get());
        if (found != imageCache.end() && !found->second.resource.expired() &&
            found->second.floatPrecision == floatPrecision) {
            imageViews[i] = found->second.image.view;
            continue;
        }
        // Same storage policy as the Metal device (see UploadImage there).
        const int width = resource->width(), height = resource->height();
        const int channels = std::clamp(resource->channels(), 1, 4);
        std::vector<float> pixels(size_t(width) * height * channels);
        if (!service->readPixels(*resource, 0, 0, width, height, 0, channels, pixels.data()))
            std::fill(pixels.begin(), pixels.end(), 0.0f);
        bool quantized = !floatPrecision;
        float peak = 0.0f;
        for (float v : pixels) {
            if (quantized) {
                const float k = v * 255.0f;
                if (!(v >= 0.0f && v <= 1.0f) || std::abs(k - std::round(k)) > 1e-3f) quantized = false;
            }
            if (std::isfinite(v)) peak = std::max(peak, std::abs(v));
        }
        const int stored = channels == 3 ? 4 : channels;
        const int componentBytes = quantized ? 1 : (!floatPrecision && peak < 60000.0f) ? 2 : 4;
        static const VkFormat formats[3][4] = {
            {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM},
            {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_SFLOAT},
            {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SFLOAT}};
        const VkFormat format = formats[componentBytes == 1 ? 0 : componentBytes == 2 ? 1 : 2][stored - 1];
        VkFormatProperties formatProperties;
        vkGetPhysicalDeviceFormatProperties(physical, format, &formatProperties);
        const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        const bool mipmapped = (formatProperties.optimalTilingFeatures & blit) == blit;
        const uint32_t levels = mipmapped ? 1u + uint32_t(std::floor(std::log2(double(std::max(width, height))))) : 1u;

        std::vector<uint8_t> staging(size_t(width) * height * stored * componentBytes);
        tbb::parallel_for(size_t(0), size_t(width) * height, [&](size_t p) {
            for (int c = 0; c < stored; ++c) {
                const float v = c < channels ? pixels[p * channels + c] : 0.0f;
                uint8_t* out = staging.data() + (p * stored + c) * componentBytes;
                if (componentBytes == 1) *out = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                else if (componentBytes == 2) { _Float16 h = _Float16(v); std::memcpy(out, &h, 2); }
                else std::memcpy(out, &v, 4);
            }
        });
        Buffer stagingBuffer;
        if (!createBuffer(staging.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stagingBuffer)) {
            if (error) *error = "cannot allocate an image staging buffer";
            return false;
        }
        std::memcpy(stagingBuffer.mapped, staging.data(), staging.size());

        Image image;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {uint32_t(width), uint32_t(height), 1};
        imageInfo.mipLevels = levels;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCreateImage(device, &imageInfo, nullptr, &image.image);
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image.image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device, &allocation, nullptr, &image.memory) != VK_SUCCESS) {
            destroyBuffer(stagingBuffer);
            if (error) *error = "out of memory for images";
            return false;
        }
        vkBindImageMemory(device, image.image, image.memory, 0);

        VkCommandBuffer commands = beginCommands();
        auto barrier = [&](uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage,
                           VkPipelineStageFlags dstStage) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = src;
            b.dstAccessMask = dst;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
            vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {uint32_t(width), uint32_t(height), 1};
        vkCmdCopyBufferToImage(commands, stagingBuffer.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        int w = width, h = height;
        for (uint32_t level = 1; level < levels; ++level) {
            barrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            region.srcOffsets[1] = {w, h, 1};
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.dstOffsets[1] = {w, h, 1};
            vkCmdBlitImage(commands, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
            barrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        barrier(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        const bool submitted = submit(commands, error);
        destroyBuffer(stagingBuffer);
        if (!submitted) return false;

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        // Absent channels read as zero (Metal semantics for R/RG formats).
        viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        vkCreateImageView(device, &viewInfo, nullptr, &image.view);
        imageCache[resource.get()] = {resource, image, floatPrecision};
        imageViews[i] = image.view;
    }
    for (auto it = imageCache.begin(); it != imageCache.end();) {
        if (!it->second.resource.expired()) { ++it; continue; }
        vkDestroyImageView(device, it->second.image.view, nullptr);
        vkDestroyImage(device, it->second.image.image, nullptr);
        vkFreeMemory(device, it->second.image.memory, nullptr);
        it = imageCache.erase(it);
    }
    return upload(kSlotImages, info.data(), info.size() * sizeof(GpuImage));
}

bool VulkanDevice::Impl::buildAccelerationStructures(std::string* error)
{
    GpuSceneData const& d = *data;
    for (auto& structure : bottomLevel) destroyStructure(structure);
    bottomLevel.clear();
    destroyStructure(topLevel);
    const VkDeviceSize scratchAlignment =
        std::max<VkDeviceSize>(accelerationProperties.minAccelerationStructureScratchOffsetAlignment, 256);
    auto alignUp = [](VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; };

    struct Build {
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        VkDeviceSize scratchOffset = 0;
    };
    std::vector<Build> builds;
    std::vector<uint32_t> structureOfPrototype(d.prototypes.size(), ~0u);
    std::vector<VkAabbPositionsKHR> boxes;
    std::vector<size_t> boxOffsets(d.prototypes.size(), 0);
    for (size_t p = 0; p < d.prototypes.size(); ++p) {
        GpuPrototype const& proto = d.prototypes[p];
        if (proto.info.x != kProtoSpheres || proto.info.w == 0) continue;
        boxOffsets[p] = boxes.size();
        for (uint32_t s = 0; s < proto.info.w; ++s) {
            float4 const& sphere = d.spheres[proto.info.z + s];
            const float r = std::abs(sphere.w);
            boxes.push_back({sphere.x - r, sphere.y - r, sphere.z - r, sphere.x + r, sphere.y + r, sphere.z + r});
        }
    }
    Buffer boxBuffer;
    createBuffer(boxes.size() * sizeof(VkAabbPositionsKHR),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &boxBuffer);
    if (!boxes.empty()) std::memcpy(boxBuffer.mapped, boxes.data(), boxes.size() * sizeof(VkAabbPositionsKHR));

    VkDeviceSize scratchSize = 0;
    builds.reserve(d.prototypes.size());
    for (size_t p = 0; p < d.prototypes.size(); ++p) {
        GpuPrototype const& proto = d.prototypes[p];
        if (proto.info.w == 0) continue;
        Build build;
        uint32_t primitives = proto.info.w;
        if (proto.info.x == kProtoSpheres) {
            build.geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            auto& aabbs = build.geometry.geometry.aabbs;
            aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            aabbs.data.deviceAddress = boxBuffer.address + boxOffsets[p] * sizeof(VkAabbPositionsKHR);
            aabbs.stride = sizeof(VkAabbPositionsKHR);
            build.geometry.flags = 0; // procedural hits are resolved by the ray query
        } else {
            if (proto.info.x == kProtoQuads) primitives *= 2;
            uint32_t maxVertex = 0;
            for (size_t i = size_t(proto.info.z) * 3; i < size_t(proto.info.z + primitives) * 3; ++i)
                maxVertex = std::max(maxVertex, d.indices[i]);
            build.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto& triangles = build.geometry.geometry.triangles;
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = buffers[kSlotPositions].address + VkDeviceSize(proto.info.y) * 12;
            triangles.vertexStride = 12;
            triangles.maxVertex = maxVertex;
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = buffers[kSlotIndices].address + VkDeviceSize(proto.info.z) * 12;
            build.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        }
        build.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.info.geometryCount = 1;
        build.range.primitiveCount = primitives;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        build.info.pGeometries = &build.geometry;
        getBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build.info, &primitives, &sizes);
        AccelerationStructure structure;
        createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                     &structure.storage, false);
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = structure.storage.buffer;
        create.size = sizes.accelerationStructureSize;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        createAccelerationStructure(device, &create, nullptr, &structure.handle);
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = structure.handle;
        structure.address = getStructureAddress(device, &addressInfo);
        build.info.dstAccelerationStructure = structure.handle;
        build.scratchOffset = scratchSize;
        scratchSize += alignUp(sizes.buildScratchSize, scratchAlignment);
        structureOfPrototype[p] = uint32_t(bottomLevel.size());
        bottomLevel.push_back(structure);
        builds.push_back(build);
    }

    // Top level: one instance per placement, in instance-table order.
    std::vector<VkAccelerationStructureInstanceKHR> instances(d.instances.size());
    for (size_t i = 0; i < d.instances.size(); ++i) {
        auto& instance = instances[i];
        std::memcpy(instance.transform.matrix, d.instanceTransforms[i].data(), sizeof(instance.transform.matrix));
        instance.instanceCustomIndex = uint32_t(i);
        instance.mask = (d.instances[i].info.y & kInstanceOpaque) != 0u ? kMaskOpaque : kMaskNonOpaque;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = bottomLevel[structureOfPrototype[d.instances[i].info.x]].address;
    }
    Buffer instanceBuffer;
    createBuffer(instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &instanceBuffer);
    if (!instances.empty())
        std::memcpy(instanceBuffer.mapped, instances.data(), instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    Build top;
    top.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    top.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    top.geometry.geometry.instances.data.deviceAddress = instanceBuffer.address;
    top.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    top.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    top.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    top.info.geometryCount = 1;
    top.info.pGeometries = &top.geometry;
    uint32_t instanceCount = uint32_t(instances.size());
    top.range.primitiveCount = instanceCount;
    VkAccelerationStructureBuildSizesInfoKHR topSizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &top.info, &instanceCount, &topSizes);
    createBuffer(topSizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                 &topLevel.storage, false);
    VkAccelerationStructureCreateInfoKHR topCreate{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    topCreate.buffer = topLevel.storage.buffer;
    topCreate.size = topSizes.accelerationStructureSize;
    topCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    createAccelerationStructure(device, &topCreate, nullptr, &topLevel.handle);
    top.info.dstAccelerationStructure = topLevel.handle;
    const VkDeviceSize topScratch = alignUp(scratchSize, scratchAlignment);
    scratchSize = topScratch + topSizes.buildScratchSize;

    Buffer scratch;
    // Offsets are aligned relative to an aligned base inside the allocation.
    if (!createBuffer(scratchSize + scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratch, false)) {
        if (error) *error = "cannot allocate acceleration structure scratch memory";
        return false;
    }
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
    for (Build& build : builds) {
        build.info.pGeometries = &build.geometry;
        build.info.scratchData.deviceAddress = alignUp(scratch.address, scratchAlignment) + build.scratchOffset;
        infos.push_back(build.info);
        ranges.push_back(&build.range);
    }
    VkCommandBuffer commands = beginCommands();
    if (!infos.empty()) cmdBuild(commands, uint32_t(infos.size()), infos.data(), ranges.data());
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    top.info.scratchData.deviceAddress = alignUp(scratch.address, scratchAlignment) + topScratch;
    const VkAccelerationStructureBuildRangeInfoKHR* topRange = &top.range;
    cmdBuild(commands, 1, &top.info, &topRange);
    const bool ok = submit(commands, error);
    destroyBuffer(scratch);
    destroyBuffer(instanceBuffer);
    destroyBuffer(boxBuffer);
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = topLevel.handle;
    topLevel.address = getStructureAddress(device, &addressInfo);
    return ok;
}

bool VulkanDevice::Impl::writeDescriptors(std::string* error)
{
    if (descriptorSet) vkFreeDescriptorSets(device, descriptorPool, 1, &descriptorSet);
    descriptorSet = VK_NULL_HANDLE;
    const uint32_t imageCount = uint32_t(std::max<size_t>(1, imageViews.size()));
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variable.descriptorSetCount = 1;
    variable.pDescriptorCounts = &imageCount;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.pNext = &variable;
    allocate.descriptorPool = descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(device, &allocate, &descriptorSet) != VK_SUCCESS) {
        if (error) *error = "cannot allocate the descriptor set";
        return false;
    }
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos(kSlotCount);
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
        if (slot == kSlotAccel || !buffers[slot].buffer) continue;
        bufferInfos[slot] = {buffers[slot].buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfos[slot];
        writes.push_back(write);
    }
    VkWriteDescriptorSetAccelerationStructureKHR structureWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    structureWrite.accelerationStructureCount = 1;
    structureWrite.pAccelerationStructures = &topLevel.handle;
    VkWriteDescriptorSet accel{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    accel.pNext = &structureWrite;
    accel.dstSet = descriptorSet;
    accel.dstBinding = kSlotAccel;
    accel.descriptorCount = 1;
    accel.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes.push_back(accel);
    std::vector<VkDescriptorImageInfo> imageInfos;
    for (VkImageView view : imageViews)
        imageInfos.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (!imageInfos.empty()) {
        VkWriteDescriptorSet images{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        images.dstSet = descriptorSet;
        images.dstBinding = kSlotImageBase;
        images.descriptorCount = uint32_t(imageInfos.size());
        images.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        images.pImageInfo = imageInfos.data();
        writes.push_back(images);
    }
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool VulkanDevice::Impl::ensureScene(SceneManager& scene, std::string* error)
{
    if (boundScene == &scene && !dirty.load() && pipeline) return true;
    vkDeviceWaitIdle(device);
    dirty = false;
    boundScene = &scene;
    data = std::make_unique<GpuSceneData>(gpu::BuildGpuScene(scene));
    for (auto const& warning : data->warnings) std::cerr << "PrismRT Vulkan: " << warning << '\n';
    auto fail = [&] { pipeline = VK_NULL_HANDLE; boundScene = nullptr; return false; };
    if (!buildPipeline(error)) return fail();
    GpuSceneData const& d = *data;
    auto bytes = [](auto const& v) { return v.size() * sizeof(v[0]); };
    upload(kSlotPositions, d.positions.data(), bytes(d.positions));
    upload(kSlotNormals, d.normals.data(), bytes(d.normals));
    upload(kSlotTexcoords, d.texcoords.data(), bytes(d.texcoords));
    upload(kSlotTangents, d.tangents.data(), bytes(d.tangents));
    upload(kSlotIndices, d.indices.data(), bytes(d.indices));
    upload(kSlotPrototypes, d.prototypes.data(), bytes(d.prototypes));
    upload(kSlotInstances, d.instances.data(), bytes(d.instances));
    upload(kSlotPrimitiveMaterials, d.primitiveMaterials.data(), bytes(d.primitiveMaterials));
    upload(kSlotMaterials, d.materials.data(), bytes(d.materials));
    upload(kSlotLegacyTextures, d.legacyTextures.data(), bytes(d.legacyTextures));
    upload(kSlotSpheres, d.spheres.data(), bytes(d.spheres));
    upload(kSlotEnvironment, d.environments.data(), bytes(d.environments));
    upload(kSlotLights, d.lights.data(), bytes(d.lights));
    upload(kSlotLightAlias, d.lightAlias.data(), bytes(d.lightAlias));
    static const GpuSceneData::EnvironmentTables noTables;
    auto const& tables = d.environmentTables ? *d.environmentTables : noTables;
    upload(kSlotEnvTexelPdf, tables.texelPdf.data(), bytes(tables.texelPdf));
    upload(kSlotEnvAliasProbability, tables.aliasProbability.data(), bytes(tables.aliasProbability));
    upload(kSlotEnvAlias, tables.alias.data(), bytes(tables.alias));
    upload(kSlotMdlTextures, d.mdlTextures.data(), bytes(d.mdlTextures));
    upload(kSlotMdlTiles, d.mdlTiles.data(), bytes(d.mdlTiles));
    upload(kSlotBsdfData, d.bsdfData.data(), bytes(d.bsdfData));
    if (!buffers[kSlotFrame].buffer) upload(kSlotFrame, nullptr, sizeof(GpuFrame));
    for (int slot : {kSlotPixels, kSlotRng, kSlotOutput, kSlotAccumulation})
        if (!buffers[slot].buffer) upload(slot, nullptr, 16);
    if (!uploadImages(error) || !buildAccelerationStructures(error) || !writeDescriptors(error)) return fail();
    return true;
}

void* VulkanDevice::Impl::renderBuffer(int slot, size_t bytes)
{
    if (buffers[slot].size < bytes) {
        vkDeviceWaitIdle(device);
        upload(slot, nullptr, bytes);
        if (descriptorSet) {
            VkDescriptorBufferInfo info{buffers[slot].buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = descriptorSet;
            write.dstBinding = uint32_t(slot);
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }
    return buffers[slot].mapped;
}

bool VulkanDevice::Impl::dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error)
{
    if (threads == 0) return true;
    std::memcpy(buffers[kSlotFrame].mapped, &frame, sizeof(frame));
    VkCommandBuffer commands = beginCommands();
    vkCmdResetQueryPool(commands, timestamps, 0, 2);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDispatch(commands, (threads + 63) / 64, 1, 1);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
    if (!submit(commands, error)) return false;
    if (gpuSeconds) {
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            *gpuSeconds += double(ticks[1] - ticks[0]) * timestampPeriod * 1e-9;
    }
    return true;
}

std::unique_ptr<VulkanDevice> VulkanDevice::Create(std::string* reason)
{
    if (const char* disabled = std::getenv("PRISM_DISABLE_VULKAN"); disabled && *disabled && *disabled != '0') {
        if (reason) *reason = "disabled by PRISM_DISABLE_VULKAN";
        return nullptr;
    }
    std::string source;
    if (!gpu::LoadShaderSource("gpu/kernel.hlsl", &source)) {
        if (reason) *reason = "the Vulkan kernel sources are missing";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    if (!impl->initialize(reason)) return nullptr;
    return std::unique_ptr<VulkanDevice>(new VulkanDevice(std::move(impl)));
}

VulkanDevice::VulkanDevice(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
VulkanDevice::~VulkanDevice() = default;

DeviceInfo VulkanDevice::Info() const
{
    DeviceInfo info;
    info.kind = DeviceKind::Vulkan;
    info.name = m_impl->name + " (Vulkan)";
    info.hardwareRayTracing = true;
    return info;
}

void VulkanDevice::SceneChanged(SceneManager&) { m_impl->dirty = true; }
bool VulkanDevice::PrepareScene(SceneManager& scene, std::string* error) { return m_impl->ensureScene(scene, error); }
GpuSceneData const& VulkanDevice::Data() const { return *m_impl->data; }
uint32_t VulkanDevice::MdlTextureCount() const { return m_impl->mdlTextureCount; }
void* VulkanDevice::RenderBuffer(int slot, size_t bytes) { return m_impl->renderBuffer(slot, bytes); }
bool VulkanDevice::Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error)
{
    return m_impl->dispatch(frame, threads, gpuSeconds, error);
}

} // namespace prism::device
