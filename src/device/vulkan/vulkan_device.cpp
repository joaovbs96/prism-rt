// Vulkan render device. The kernel is shaders/gpu/kernel.hlsl with the
// scene's MDL code, compiled to SPIR-V with DXC and cached by source hash
// (the same source the Metal device compiles to DXIL). Material sets too
// large for one kernel run as a ray tracing pipeline with one callable
// shader per MDL program instead (vulkan_material_calls.h). Every resource
// slot is a binding of one descriptor set; the scene is mirrored with one
// bottom-level acceleration structure per prototype and one top-level
// structure. The structure follows the Metal device.

#include "vulkan_device.h"
#include "vulkan_material_calls.h"
#if PRISM_HAS_VK_OIDN
#include "vulkan_denoiser.h"
#endif

#include "../gpu/gpu_materials.h"
#include "../gpu/texture_budget.h"
#include "../gpu/texture_streaming.h"
#include "../gpu/hlsl_compiler.h"
#include "../gpu/shader_sources.h"
#include "../../progress.h"
#include "../../scene.h"
#include "../../texture_service.h"
#include "../../texture_gpu_file.h"
#include "../../varying_emitter_signal.h"
#include "../../load_profile.h"
#include "../../mdl_material.h"

#include <OpenImageIO/imageio.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <iostream>
#include <thread>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace prism::device {

using prism::device::gpu::GpuSceneData;

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kMaxImages = 4096;

using gpu_texture::FloatToHalf; // IEEE binary16, round to nearest even (MSVC has no _Float16)

// How an image's texels are stored on the GPU.
enum class Storage { Unorm8, Unorm16, Float16, Float32 };

int StorageBytes(Storage s)
{
    switch (s) {
    case Storage::Unorm8: return 1;
    case Storage::Unorm16: case Storage::Float16: return 2;
    default: return 4;
    }
}

VkFormat StorageFormat(Storage s, int stored)
{
    static const VkFormat formats[4][4] = {
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_UNORM},
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_SFLOAT},
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SFLOAT}};
    return formats[int(s)][stored - 1];
}

// Encodes one texel component as the .tx.exr path always has.
inline void EncodeTexel(Storage s, float v, uint8_t* texel)
{
    switch (s) {
    case Storage::Unorm8: *texel = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); break;
    case Storage::Unorm16: {
        const uint16_t u = uint16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
        std::memcpy(texel, &u, 2);
        break;
    }
    case Storage::Float16: {
        const uint16_t h = FloatToHalf(v);
        std::memcpy(texel, &h, 2);
        break;
    }
    default: std::memcpy(texel, &v, 4);
    }
}

bool SameStorage(gpu_texture::Component c, Storage s)
{
    return (c == gpu_texture::Component::Unorm8 && s == Storage::Unorm8) ||
        (c == gpu_texture::Component::Unorm16 && s == Storage::Unorm16) ||
        (c == gpu_texture::Component::Float16 && s == Storage::Float16) ||
        (c == gpu_texture::Component::Float32 && s == Storage::Float32);
}

OIIO::TypeDesc ComponentType(gpu_texture::Component c)
{
    switch (c) {
    case gpu_texture::Component::Unorm8: return OIIO::TypeDesc::UINT8;
    case gpu_texture::Component::Unorm16: return OIIO::TypeDesc::UINT16;
    case gpu_texture::Component::Float16: return OIIO::TypeDesc::HALF;
    default: return OIIO::TypeDesc::FLOAT;
    }
}

// The storage of an image with a GPU copy. Integer texels keep their
// precision; half and float texels follow the rule the float .tx.exr path
// applies (8-bit when every value is k/255, half below 60000, else float);
// environments keep float.
Storage ChooseStorage(gpu_texture::Header const& h, bool floatPrecision, bool unorm16Filterable)
{
    if (floatPrecision) return Storage::Float32;
    switch (h.component) {
    case gpu_texture::Component::Unorm8: return Storage::Unorm8;
    case gpu_texture::Component::Unorm16: return unorm16Filterable ? Storage::Unorm16 : Storage::Float16;
    default:
        if (h.flags & gpu_texture::kQuantized8) return Storage::Unorm8;
        return h.peak < 60000.0f ? Storage::Float16 : Storage::Float32;
    }
}

// An uninitialized byte buffer (a std::vector would clear it first).
struct Bytes {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
    Bytes() = default;
    explicit Bytes(size_t n) : data(new uint8_t[n]), size(n) {}
    void swap(Bytes& other) noexcept
    {
        std::swap(data, other.data);
        std::swap(size, other.size);
    }
};

struct GpuCopy {
    std::filesystem::path path;
    gpu_texture::Header header{};
    Storage storage = Storage::Float32;
    bool valid = false;
};

// Texel data of one image, converted for upload. Same storage policy as
// the Metal device (see UploadImage there), except that a GPU copy keeps
// 16-bit integer texels as UNORM16 and reduced 8-bit images 8-bit.
struct PreparedImage {
    Bytes staging;
    int width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 1;
    bool fromCopy = false;
};

// PRISM_GPU_TEXTURE_COPIES=0 uploads from the float .tx.exr files instead.
bool GpuCopiesEnabled()
{
    const char* value = std::getenv("PRISM_GPU_TEXTURE_COPIES");
    return !value || std::string_view(value) != "0";
}

// Whole images take their levels from the GPU copy's stored mip chain, the
// single reference every GPU path (streamed or not) samples; mips generated
// with blits depend on the level they start from.
// PRISM_GPU_TEXTURE_BLIT_MIPS=1 generates them with blits as before
// (diagnostics).
bool UsesStoredMips(gpu_texture::Header const& h)
{
    static const bool blits = [] {
        const char* value = std::getenv("PRISM_GPU_TEXTURE_BLIT_MIPS");
        return value && *value == '1';
    }();
    return !blits && h.levelCount == gpu_texture::FullChainLevels(h.width, h.height);
}

// Texture streaming (texture_streaming.h): PRISM_GPU_TEXTURE_STREAMING=0 (or
// the prism:textureStreaming setting) uploads whole textures, reduced to fit
// the budget, as before.
bool StreamingEnabled()
{
    const char* value = std::getenv("PRISM_GPU_TEXTURE_STREAMING");
    return GpuCopiesEnabled() && (!value || std::string_view(value) != "0");
}

// PRISM_GPU_SCENE_BUFFERS: where the read-only scene buffers go (auto:
// device-local when the GPU memory budget allows; device: device-local
// regardless of the budget; staging: device-local through staging copies even
// with ReBAR, for testing; host: system memory). docs/gpu_devices.md,
// "Vulkan memory placement".
enum class SceneBuffers { Auto, Device, Staging, Host };
SceneBuffers SceneBuffersSetting()
{
    const char* value = std::getenv("PRISM_GPU_SCENE_BUFFERS");
    const std::string_view v = value ? value : "";
    if (v == "device") return SceneBuffers::Device;
    if (v == "staging") return SceneBuffers::Staging;
    if (v == "host") return SceneBuffers::Host;
    return SceneBuffers::Auto;
}

uint32_t StreamingSetting(const char* name, uint32_t fallback, uint32_t minimum = 0)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return uint32_t(std::max<long long>(minimum, std::atoll(value)));
}

// The always resident tail: levels at most this many texels on a side.
uint32_t StreamingTailSize() { return StreamingSetting("PRISM_GPU_STREAMING_TAIL", 128, 1); }
// Progressive rendering restarts an accumulation at most once, and only
// before this many samples; later residency changes sharpen it in place.
uint32_t StreamingRestartSamples() { return StreamingSetting("PRISM_GPU_STREAMING_RESTART_SAMPLES", 16); }
// One pixel of each block of this size records its lookups (1 while settling).
uint32_t StreamingFeedbackStride() { return StreamingSetting("PRISM_GPU_STREAMING_FEEDBACK_STRIDE", 4, 1); }
// Levels finer than the estimated level of detail a request adds, in 1/16
// (implementations approximate the LOD; Vulkan allows sqrt(2) in scale).
uint32_t StreamingLodMargin()
{
    const char* value = std::getenv("PRISM_GPU_STREAMING_LOD_MARGIN");
    return uint32_t(std::lround(std::clamp(value && *value ? std::atof(value) : 0.5, 0.0, 15.0) * 16.0));
}
// The fraction of an image's throughput-weighted lookups its request may
// leave out (texture_streaming.h); 0 serves every lookup.
double StreamingThreshold()
{
    const char* value = std::getenv("PRISM_GPU_STREAMING_THRESHOLD");
    return value && *value ? std::max(0.0, std::atof(value)) : 0.001;
}
// Staging per upload batch of the streaming thread.
uint64_t StreamingBatchBytes() { return uint64_t(StreamingSetting("PRISM_GPU_STREAMING_BATCH_MIB", 256, 1)) << 20; }

// Texels of levels [first, first + count) of a GPU copy read from its file
// (`raw`, contiguous), in `storage` with `stored` components: RGB padded to
// RGBA, other precisions converted through float as prepareImageFromCopy
// does. `offsets` receives each level's offset in `out` (out.size bytes).
void ConvertLevels(gpu_texture::Header const& h, Storage storage, uint32_t first, uint32_t count, uint8_t const* raw,
                   uint8_t* out, std::vector<VkDeviceSize>* offsets);

size_t ConvertedBytes(gpu_texture::Header const& h, Storage storage, uint32_t first, uint32_t count)
{
    const int stored = h.channels == 3 ? 4 : int(h.channels);
    size_t bytes = 0;
    for (uint32_t l = first; l < first + count; ++l)
        bytes += size_t(h.levels[l].width) * h.levels[l].height * stored * StorageBytes(storage);
    return bytes;
}

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
    case VK_TIMEOUT: return "timeout";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "out of pool memory";
    case VK_ERROR_FRAGMENTED_POOL: return "fragmented pool";
    case VK_ERROR_UNKNOWN: return "unknown error (VK_ERROR_UNKNOWN)";
    default: {
        thread_local std::string text;
        text = "Vulkan error " + std::to_string(int(result));
        return text.c_str();
    }
    }
}

void ConvertLevels(gpu_texture::Header const& h, Storage storage, uint32_t first, uint32_t count, uint8_t const* raw,
                   uint8_t* out, std::vector<VkDeviceSize>* offsets)
{
    const int channels = int(h.channels), stored = channels == 3 ? 4 : channels;
    const int inBytes = int(gpu_texture::ComponentBytes(h.component)), outBytes = StorageBytes(storage);
    const bool same = SameStorage(h.component, storage);
    const OIIO::TypeDesc type = ComponentType(h.component);
    offsets->clear();
    size_t at = 0;
    for (uint32_t l = first; l < first + count; ++l) {
        const size_t texels = size_t(h.levels[l].width) * h.levels[l].height;
        uint8_t const* in = raw + (h.levels[l].offset - h.levels[first].offset);
        uint8_t* dst = out + at;
        offsets->push_back(VkDeviceSize(at));
        tbb::parallel_for(tbb::blocked_range<size_t>(0, texels, size_t(1) << 14),
            [&](tbb::blocked_range<size_t> const& range) {
                float values[4];
                for (size_t p = range.begin(); p != range.end(); ++p) {
                    uint8_t* texel = dst + p * stored * outBytes;
                    if (same) {
                        std::memcpy(texel, in + p * channels * inBytes, size_t(channels) * inBytes);
                        std::memset(texel + channels * outBytes, 0, size_t(stored - channels) * outBytes);
                        continue;
                    }
                    OIIO::convert_pixel_values(type, in + p * channels * inBytes, OIIO::TypeDesc::FLOAT, values,
                                               channels);
                    for (int c = 0; c < stored; ++c)
                        EncodeTexel(storage, c < channels ? values[c] : 0.0f, texel + c * outBytes);
                }
            });
        at += texels * stored * outBytes;
    }
}

// The top level of a texture as readPixels returns it, read straight from
// its prepared file. Reading whole images through the texture system's
// shared tile cache is several times slower, and much slower again for
// concurrent reads that do not fit in the cache.
bool ReadPreparedImage(prism::TextureResource const& resource, int channels, float* pixels)
{
    auto const& path = resource.preparedPath();
    if (path.empty()) return false;
    auto input = OIIO::ImageInput::open(path.string());
    if (!input) {
        OIIO::geterror(); // clear it
        return false;
    }
    OIIO::ImageSpec const& spec = input->spec();
    if (spec.width != resource.width() || spec.height != resource.height() || spec.nchannels < channels ||
        spec.x != 0 || spec.y != 0 || spec.depth > 1) return false;
    const bool read = input->read_image(0, 0, 0, channels, OIIO::TypeDesc::FLOAT, pixels);
    if (!read) input->geterror();
    return read;
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    bool deviceLocal = false; // a read-only scene slot in device-local memory
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
};

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer storage;
    VkDeviceAddress address = 0;
};

// Pipeline-cache data is only valid for the driver that produced it: name the
// files after the cache UUID and the device and driver identifiers, so a
// driver update or another GPU reads its own file. The driver checks the
// data's header again when it is loaded.
std::string PipelineCacheTag(VkPhysicalDeviceProperties const& properties)
{
    std::string tag;
    for (uint8_t byte : properties.pipelineCacheUUID) tag += std::format("{:02x}", byte);
    return tag + std::format("-{:x}-{:x}-{:x}", properties.vendorID, properties.deviceID,
                             properties.driverVersion);
}


} // namespace

// A short reason for a scene the GPU cannot render, for the host's status line.
static std::string FallbackSummary(std::string const& error)
{
    if (error.find("too large for the driver") != std::string::npos) return "shader too large for the driver";
    if (error.find("too large") != std::string::npos || gpu::HlslTooLarge(error)) return "kernel too large";
    if (error.starts_with("DXC failed") || error.starts_with("cannot run")) return "kernel compile failed";
    std::string first = error.substr(0, error.find_first_of("\r\n"));
    if (first.size() > 60) first = first.substr(0, 57) + "...";
    return first.empty() ? "scene not supported" : first;
}

static bool AssembleVulkanKernel(std::string const& mdlCode, std::string* source, std::string* error,
                                 bool lightAovs = false)
{
    if (!gpu::AssembleShader("gpu/kernel.hlsl", {"gpu/vulkan"}, mdlCode, source, error)) return false;
    *source = vulkan::ShaderSaltDefine() + (lightAovs ? "#define PRISM_LIGHT_AOVS 1\n" : "") + *source;
    return true;
}

// The marker of a compute kernel DXC could not compile within its memory limit.
static std::filesystem::path TooLargeMarker(std::string const& source)
{
    return gpu::ShaderCacheDirectory("vulkan") /
        (gpu::SourceDigest(source + gpu::HlslCompilerSignature(gpu::HlslTarget::Spirv)) + ".too-large");
}

bool CompileVulkanKernel(std::string const& mdlCode, std::vector<uint32_t>* spirv, std::string* error,
                         uint32_t programCount, bool lightAovs, std::atomic<bool> const* cancel)
{
    std::string source;
    if (!AssembleVulkanKernel(mdlCode, &source, error, lightAovs)) return false;
    const auto cacheDirectory = gpu::ShaderCacheDirectory("vulkan");
    const auto cached = cacheDirectory / (gpu::SourceDigest(source) + ".spv");
    std::vector<char> bytes = vulkan::ReadCacheFile(cached);
    if (!vulkan::PlausibleSpirv(bytes)) {
        // A kernel that failed to compile fails again: do not retry it every frame.
        static std::mutex failedMutex;
        static std::unordered_map<std::string, std::string> failed;
        const std::string key = cached.string();
        {
            std::lock_guard lock(failedMutex);
            if (auto found = failed.find(key); found != failed.end()) {
                if (error) *error = found->second;
                return false;
            }
        }
        // A kernel too large for DXC stays too large for the same compiler:
        // a marker in the cache skips the minutes-long attempt next time.
        const auto tooLarge = TooLargeMarker(source);
        const char* retry = std::getenv("PRISM_VULKAN_RETRY_LARGE");
        if (std::ifstream marker{tooLarge}; marker && !(retry && *retry && *retry != '0')) {
            std::string reason;
            std::getline(marker, reason);
            if (error)
                *error = reason + " (remembered from an earlier run; PRISM_VULKAN_RETRY_LARGE=1 tries again)";
            std::lock_guard lock(failedMutex);
            failed[key] = error ? *error : reason;
            return false;
        }
        std::cerr << std::format("PrismRT Vulkan: compiling the GPU kernel{} ({} MDL programs, {} KB of HLSL); this "
                                 "happens once per material set\n", lightAovs ? " with light AOVs" : "",
                                 programCount, source.size() >> 10);
        progress::Detail(Phase::Kernel, std::format("DXC to SPIR-V, {} MDL programs, {} KB HLSL",
                                                    programCount, source.size() >> 10));
        std::string reason;
        if (!gpu::CompileHlsl(source, gpu::HlslTarget::Spirv, cacheDirectory, &bytes, &reason, {}, nullptr, cancel)) {
            // A cancelled compile says nothing about the kernel: not remembered.
            if (gpu::HlslCancelled(reason)) {
                if (error) *error = reason;
                return false;
            }
            if (gpu::HlslTooLarge(reason)) std::ofstream(tooLarge) << reason << '\n';
            if (error) *error = reason;
            std::lock_guard lock(failedMutex);
            failed[key] = reason;
            return false;
        }
        std::error_code ignored;
        std::filesystem::remove(tooLarge, ignored);
        if (!vulkan::PlausibleSpirv(bytes)) {
            if (error) *error = "DXC produced no valid SPIR-V";
            return false;
        }
        vulkan::WriteCacheFile(cached, bytes.data(), bytes.size());
    }
    spirv->resize(bytes.size() / 4);
    std::memcpy(spirv->data(), bytes.data(), bytes.size());
    return true;
}

// The compute kernel without MDL code, in bytes (the part of a kernel's size
// that does not depend on the materials).
static size_t KernelBaseBytes()
{
    static const size_t bytes = [] {
        std::string source;
        return AssembleVulkanKernel(std::string(), &source, nullptr) ? source.size() : size_t(0);
    }();
    return bytes;
}

// Material sets whose estimated compute kernel (KernelBaseBytes plus
// gpu::LinkUnitEstimate) exceeds the callable threshold by this factor use
// callables without generating their link unit: the estimate can exceed the
// link unit's size where per-program code repeats specialized shared code.
// Closer to the threshold, the link unit decides as before.
constexpr double kEstimateMargin = 1.15;

void ReportVulkanPrograms(SceneManager& scene, std::ostream& out,
                          prism::VaryingEmitterSignal const* varyingEmitters)
{
    using namespace std::chrono;
    const GpuSceneData data = gpu::BuildGpuScene(scene);
    size_t varying = 0;
    for (auto const* m : data.mdlMaterials) varying += m->variesEmission() ? 1 : 0;
    out << std::format("materials {} varying_emitters {}\n", data.mdlMaterials.size(), varying);
    if (data.mdlMaterials.empty()) return;
    const size_t estimate = KernelBaseBytes() + gpu::EstimatedLinkUnitBytes(gpu::ProgramCodes(data.mdlMaterials));
    const bool perProgram = double(estimate) > double(vulkan::CallableThresholdBytes()) * kEstimateMargin;
    out << std::format("estimate_kb {} layout {} driver_budget_mib {}\n", estimate >> 10,
                       perProgram ? "programs" : "link_unit", vulkan::DriverMemoryBudget() >> 20);
    auto const shared = perProgram ? gpu::CachedProgramShaderCode(data.mdlMaterials)
                                   : gpu::CachedMdlShaderCode(data.mdlMaterials);
    auto const& code = *shared;
    out << std::format("programs {} varying_define {} success {}\n", code.programCount, code.varyingEmitters,
                       code.success);
    // The raygen variants a device prewarms while the materials load (a note
    // of varying emission selects PRISM_VARYING_EMITTERS), and whether one
    // of them is the scene's raygen shader.
    const bool prewarmsEmitters = varyingEmitters && varyingEmitters->Notes() > 0;
    out << std::format("raygen_prewarm {} covers_scene {}\n", prewarmsEmitters ? "emitters" : "plain",
                       prewarmsEmitters || !code.varyingEmitters);
    const auto cache = gpu::ShaderCacheDirectory("report");
    auto compile = [&](std::string const& label, std::string const& source, gpu::HlslStage const& stage) {
        std::vector<char> binary;
        std::string error;
        const auto start = steady_clock::now();
        const bool ok = gpu::CompileHlsl(source, gpu::HlslTarget::Spirv, cache, &binary, &error, stage);
        const std::string limit = ok ? vulkan::DriverSpirvLimitError(binary.size(), label) : std::string();
        out << std::format("{} hlsl_kb {} spirv_kb {} dxc_s {:.1f} driver_est_mib {}{}{}\n", label,
                           source.size() >> 10, binary.size() >> 10,
                           duration<double>(steady_clock::now() - start).count(),
                           vulkan::DriverCompileBytes(binary.size()) >> 20, ok ? "" : " FAILED " + error.substr(0, 200),
                           limit.empty() ? "" : " REFUSED " + limit);
    };
    if (!code.perProgram) {
        std::string source;
        if (AssembleVulkanKernel(code.code, &source, nullptr)) {
            const bool callables = source.size() > vulkan::CallableThresholdBytes();
            out << std::format("link_unit_kernel_kb {} -> {}\n", source.size() >> 10,
                               callables ? "callables" : "megakernel");
            if (!callables) {
                compile("megakernel", source, {});
                return;
            }
        }
    }
    std::vector<std::string> sources;
    std::string error;
    if (!vulkan::MaterialCallSources(code, false, &sources, &error)) {
        out << "callable sources failed: " << error << '\n';
        return;
    }
    const std::vector<uint32_t> slots = vulkan::CallableSlots(sources);
    for (size_t i = 0; i < sources.size(); ++i) {
        // Per-program callables name their program's digest (ProgramCallableSource).
        std::string label = "raygen";
        if (i > 0) {
            const size_t slot = std::find(slots.begin(), slots.end(), uint32_t(i)) - slots.begin();
            const auto part = vulkan::CallablePart(sources[i]);
            label = std::format("callable{}_{}", slot / 2,
                                part == vulkan::MaterialCallPart::Whole      ? "whole"
                                : part == vulkan::MaterialCallPart::Evaluate ? "evaluate"
                                                                             : "sample");
            constexpr std::string_view tag = "// MDL program ";
            if (sources[i].starts_with(tag)) label += " program " + sources[i].substr(tag.size(), 12);
        }
        compile(label, sources[i], gpu::HlslStage{vulkan::kMaterialCallProfile, ""});
    }
}

// Callables compiled while the scene's materials still compile: every program
// translated early (gpu::AddProgramListener) gets its callable compiled in the
// background once the material set is known to use callables. Shared with
// the listener, which may still run while the device is destroyed.
struct CallableSpeculation {
    std::mutex mutex;
    vulkan::MaterialCallShaders* shaders = nullptr; // null once the device is gone
    vulkan::CallableMode mode = vulkan::CallableMode::Auto;
    gpu::LinkUnitEstimate estimate;                 // of the programs since the last scene
    std::vector<std::shared_ptr<const gpu::MdlProgramCode>> pending;
    bool active = false;                            // callables are certain: compile as programs come

    void program(std::shared_ptr<const gpu::MdlProgramCode> const& code)
    {
        std::vector<std::shared_ptr<const gpu::MdlProgramCode>> start;
        vulkan::MaterialCallShaders* target = nullptr;
        {
            std::lock_guard lock(mutex);
            if (!shaders || mode == vulkan::CallableMode::Never) return;
            if (active) {
                start.push_back(code);
            } else {
                pending.push_back(code);
                estimate.add(*code);
                const size_t kernel = KernelBaseBytes() + estimate.bytes();
                if (mode == vulkan::CallableMode::Always ||
                    double(kernel) > double(vulkan::CallableThresholdBytes()) * kEstimateMargin) {
                    active = true;
                    start.swap(pending);
                    if (prism::load_profile::Enabled())
                        prism::load_profile::Mark("vulkan.speculate",
                            std::format("programs={} kernel_estimate_kb={}", start.size(), kernel >> 10));
                }
            }
            target = shaders;
            // Compile while holding the lock, so the device cannot go away
            // meanwhile; prefetch only queues the work.
            for (auto const& program : start)
                for (auto part : vulkan::ProgramParts(program->code.size())) {
                    std::string source, error;
                    if (vulkan::ProgramCallableSource(*program, part, &source, &error))
                        target->prefetch(std::move(source));
                }
        }
    }
    // A material of varying emission is classified (varying_emitter_signal.h):
    // the scene's raygen shader needs PRISM_VARYING_EMITTERS, so that variant
    // compiles now, not once the scene's programs are known.
    // PRISM_VULKAN_PREWARM_EMITTERS=0 leaves it to then.
    void varyingEmitter()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("PRISM_VULKAN_PREWARM_EMITTERS");
            return !value || std::string_view(value) != "0";
        }();
        if (!enabled) return;
        std::lock_guard lock(mutex);
        if (shaders && mode != vulkan::CallableMode::Never) shaders->prewarmRaygen(true);
    }
    // A scene's preparation starts: later programs belong to the next one.
    void reset()
    {
        std::lock_guard lock(mutex);
        estimate = gpu::LinkUnitEstimate();
        pending.clear();
        active = false;
    }
};

struct VulkanDevice::Impl {
    // CancelPreparation: the device is about to go; preparation fails fast.
    std::atomic<bool> cancelled{false};
    // A wait for a submission timed out (waitFence): the GPU may still run
    // it, so nothing is submitted again and nothing it may use is freed (the
    // destructor leaks the device instead of waiting for it).
    std::atomic<bool> hung{false};
    // Waits for `fence`, at most vulkan::FenceTimeoutNanoseconds() (once hung,
    // it only polls). `kernel`: a render kernel's submission, the one
    // PRISM_GPU_SIMULATE_TIMEOUT=1 (test hook) reports as timed out once.
    VkResult waitFence(VkFence fence, bool kernel = false);
    // The error of a failed submission or wait.
    static std::string submissionError(VkResult result);
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
    // A scene that failed to prepare fails again until it changes.
    SceneManager const* failedScene = nullptr;
    std::string failedError;
    std::unique_ptr<GpuSceneData> data;
    uint32_t mdlTextureCount = 1;

    Buffer buffers[kSlotCount];
    // Device-local slots waiting for their staging copy (flushUploads).
    struct PendingUpload {
        int slot = 0;
        Buffer staging;
    };
    std::vector<PendingUpload> pendingUploads;
    std::vector<VkImageView> imageViews;
    struct CachedImage {
        std::weak_ptr<const prism::TextureResource> resource;
        Image image;
        bool floatPrecision = false;
        int shift = 0;   // resolution halvings (texture budget)
        // Streamed: the image holds levels streamedLevel and coarser (-1: whole).
        int streamedLevel = -1;
        Storage storage = Storage::Float32;
    };
    std::unordered_map<const prism::TextureResource*, CachedImage> imageCache;
    size_t reportedReducedImages = 0;
    std::vector<AccelerationStructure> bottomLevel;
    // The bottom-level structure of each prototype (~0u: none).
    std::vector<uint32_t> structureOfPrototype;
    // The geometry buffers and `bottomLevel` hold `data`'s geometry, so an
    // edit that keeps it (gpu::SameGeometry) rebuilds only the top level.
    bool geometryCurrent = false;
    AccelerationStructure topLevel;
    VkSampler samplers[kSamplerCount] = {};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkPipeline> pipelines;
    // Identifies this driver's pipeline-cache data (see PipelineCacheTag).
    std::string pipelineCacheTag;
    VkPhysicalDeviceProperties deviceProperties{};
    // Ray tracing pipelines with material callables, by generated MDL code
    // and reordering; `callables` is the one in use (null: `pipeline` is the
    // compute kernel).
    vulkan::RayTracingSupport rayTracing;
    std::unordered_map<std::string, std::unique_ptr<vulkan::MaterialCallPipeline>> callablePipelines;
    // The compiled shaders (pipeline libraries) of every callable pipeline.
    std::unique_ptr<vulkan::MaterialCallShaders> callableShaders;
    vulkan::MaterialCallPipeline const* callables = nullptr;
    // The generated MDL code `pipeline` was built from.
    std::shared_ptr<const gpu::MdlShaderCode> pipelineCode;
    // The light AOV variant of `pipeline` (prepareLightAovs), built for
    // `aovBase` (the pipelineCode it varies); frames with kFrameLightAovs run it.
    VkPipeline aovPipeline = VK_NULL_HANDLE;
    vulkan::MaterialCallPipeline const* aovCallables = nullptr;
    std::shared_ptr<const gpu::MdlShaderCode> aovBase;
    // Why the scene's materials use the per-program layout (callables), for the log.
    std::string perProgramReason;
    // Query pairs: 0 for dispatch, then kChunkQueryPairs for pass chunks.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    // Pass chunks queued without waiting (submitChunk, waitChunk), in
    // submission order on the one queue: each has its own command buffer,
    // fence and timestamp pair. Chunks write disjoint pixels; the frame and
    // pixel list they read are updated in their command buffers.
    struct Chunk {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint32_t query = 0;
        int readback = -1; // texture feedback readback (-1: none)
        bool done = false, ok = true;
        double seconds = 0.0;
        std::string error;
    };
    static constexpr uint32_t kChunkQueryPairs = 4;
    std::deque<Chunk> chunks;
    std::vector<VkFence> spareFences;
    uint32_t chunkQueries = 0;
    // The pass's pixel list (host-visible), copied into kSlotPixels per chunk.
    Buffer passList;
    // Early MDL translations and the callables they start (CallableSpeculation).
    std::shared_ptr<CallableSpeculation> speculation = std::make_shared<CallableSpeculation>();
    int programListener = 0;
    int emitterListener = 0;

    // Submissions to `queue` and waits for the device to be idle: the early
    // image uploads below submit from their own thread.
    std::mutex queueMutex;
    // Early image uploads: textures are uploaded at full resolution from
    // their GPU copies as they are prepared (TextureService prepared
    // observers), while the scene still loads, until their estimated memory
    // reaches the texture budget. uploadImages takes those its scene uses,
    // with the storage and resolution it chose, and frees the others.
    struct EarlyImage {
        Image image;
        Storage storage = Storage::Float32;
    };
    std::mutex earlyMutex;
    std::condition_variable earlyChanged;
    std::unordered_map<std::string, EarlyImage> earlyImages;      // by GPU copy path
    std::unordered_set<std::string> earlySeen, residentPaths;   // requested; in imageCache
    std::deque<std::shared_ptr<const prism::TextureResource>> earlyQueue;
    std::thread earlyWorker;
    bool earlyStop = false, earlyPaused = false, earlyBusy = false;
    uint64_t earlyBytes = 0, earlyBudget = 0;
    // DeviceOptions::initialTextureBudgetMiB: the budget of the early uploads.
    int initialTextureBudgetMiB = 0;
    // The renderer's texture service (DeviceResources::textures): early
    // uploads observe it, scenes' images are read through it.
    std::shared_ptr<prism::TextureService> textures;
    // The renderer's progress model and logger, bound on the device's own
    // threads, and its note of varying emission (DeviceOptions).
    prism::Progress* progressModel = nullptr;
    prism::Logger const* logger = &prism::logging::Default();
    std::shared_ptr<prism::VaryingEmitterSignal> varyingEmitters;
    std::vector<gpu::ImageFootprint> earlyFootprints; // every image offered since the last scene
    uint32_t earlyUploaded = 0, earlySkipped = 0;
    double earlySeconds = 0.0;
    VkCommandPool earlyPool = VK_NULL_HANDLE;
    VkFence earlyFence = VK_NULL_HANDLE;
    bool unorm16Filterable = false;
    int textureObserver = 0;
    void earlyPrepared(std::shared_ptr<const prism::TextureResource> const& resource);
    void earlyLoop();
    bool earlyUpload(prism::TextureResource const& resource);
    // Stops the early uploads (waits for the one in progress) for uploadImages.
    void pauseEarly();
    void resumeEarly();

    // Texture streaming (texture_streaming.h). Each streamed image holds a
    // contiguous range of its mip levels, [finest, coarsest]: its VkImage is
    // allocated at the finest resident level's size, and sampling with
    // normalized coordinates clamps the level of detail to it by itself.
    // Changing the range creates a new image from the GPU copy's levels on
    // the streaming thread (its own command pool and fence; submissions
    // share the queue under queueMutex), and the render thread swaps the
    // descriptor and the GpuImage record before a dispatch, when no
    // submission of it is pending (dispatches wait for their fence), so no
    // descriptor is updated while in use. Feedback: every dispatch records
    // the finest level each image is asked for in a device buffer, copies it
    // to a free host buffer of a small ring and resets it in the same
    // submission; the streaming thread reads the copies (a dispatch without
    // a free buffer leaves the feedback to accumulate).
    struct StreamSource {
        size_t image = 0; // index in the scene's images
        GpuCopy copy;
        const prism::TextureResource* resource = nullptr;
    };
    struct StreamReady {
        size_t index = 0; // planner index
        Image image;
        uint32_t finest = 0;
    };
    struct Streaming {
        bool active = false; // the bound scene streams (render thread)
        std::mutex mutex;
        std::condition_variable changed;
        gpu::StreamingPlanner planner;
        std::vector<StreamSource> sources;
        std::vector<uint32_t> inFlight; // target being created (kNoRequest: none)
        std::vector<StreamReady> ready;
        std::vector<uint32_t> lastPlan;
        uint32_t lastPending = 0;
        static constexpr int kReadbacks = 3;
        Buffer readback[kReadbacks];
        int readbackState[kReadbacks] = {}; // 0 free, 1 copy submitted, 2 filled
        uint64_t readbackAccumulation[kReadbacks] = {};
        uint32_t feedbackImages = 0;
        uint64_t accumulation = 0;
        uint64_t phase = 0;
        bool replan = false, busy = false, paused = true, stop = false;
        bool hold = false, frozen = false, restartPending = false, upgraded = false, settleChanged = false;
        bool fullFeedback = false, afterPreview = false, resetFeedback = false;
        uint32_t restarts = 0;
        int sample = 0; // of the current pass
        uint64_t roundsSinceFiner = 0;
        std::thread worker;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        // Statistics.
        std::atomic<uint64_t> uploads{0}, uploadedBytes{0}, feedbackRounds{0}, restartsTotal{0}, imageBytes{0};
        bool reportedBelow = false;
    } streaming;
    bool streamScene = false; // the scene being prepared asks for streaming
    int sceneTextureBudgetMiB = 0; // its prism:gpuTextureBudgetMiB (0: automatic)
    bool memoryBudget = false; // VK_EXT_memory_budget
    // Device-local memory this process uses (VK_EXT_memory_budget; 0 without it).
    uint64_t deviceLocalUsage() const;
    void streamLoop();
    // Reads, converts and uploads levels [finest, ...) of each job into new
    // images (one submission on `pool`, waited with `fence`); false on failure.
    struct StreamJob {
        GpuCopy const* copy = nullptr;
        uint32_t finest = 0;
    };
    bool createStreamedImages(std::vector<StreamJob> const& jobs, VkCommandPool pool, VkFence fence,
                              std::vector<Image>* images, uint64_t* bytes, std::string* error);
    void pauseStreaming();
    void resumeStreaming();
    // Swaps in finished images (render thread, between submissions).
    void applyStreaming(bool newAccumulation);
    bool createFeedbackBuffers(uint32_t images, std::string* error);
    void beginWork(uint32_t mode, int sample);
    bool accumulationOutdated();
    bool settleResidency();
    TextureStreamingStats streamingStats();

    ~Impl();
    bool initialize(std::string* reason);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    // `type`: a specific memory type instead of the first matching one.
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer, bool hostVisible = true,
                      bool hostCached = false, uint32_t type = ~0u);
    void destroyBuffer(Buffer& buffer);
    // Where upload() puts the read-only scene slots: host-visible system
    // memory, device-local memory the host maps (ReBAR) and writes directly,
    // or device-local memory filled from staging copies (flushUploads).
    enum class Placement { Host, Mapped, Staged };
    Placement placement = Placement::Host;
    uint32_t localHeap = 0;     // the largest device-local heap
    uint32_t rebarType = ~0u;   // a DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT type on it
    Placement choosePlacement(uint64_t bufferBytes, uint64_t structureBytes);
    // Uploads a slot: host-visible for the slots the host touches per pass,
    // otherwise as `placement` says.
    bool upload(int slot, const void* bytes, size_t size);
    bool flushUploads(std::string* error);
    void destroyStructure(AccelerationStructure& structure);
    VkCommandBuffer beginCommands();
    // `kernel`: a render kernel run (waitFence).
    bool submit(VkCommandBuffer commands, std::string* error, bool kernel = false);
    bool ensureScene(SceneManager& scene, std::string* error);
    // Generates the scene's MDL shader code and uploads its data; `compile`
    // tells whether its pipeline still has to be built (compilePipeline).
    bool prepareMaterials(std::shared_ptr<const gpu::MdlShaderCode>* code, bool* compile, std::string* error);
    // Builds the pipeline of `code`; runs while the scene uploads.
    bool compilePipeline(std::shared_ptr<const gpu::MdlShaderCode> const& shared, std::string* error);
    bool prepareLightAovs(std::string* error);
    bool buildCallablePipeline(std::shared_ptr<const gpu::MdlShaderCode> const& code, std::string const& reason,
                               std::string* error);
    bool uploadImages(std::string* error);
    // Sets the size and mip levels of `out` (format set).
    void finishImage(PreparedImage& out, int width, int height) const;
    // The texels of `copy`, reduced `shift` times, in its storage.
    bool prepareImageFromCopy(GpuCopy const& copy, int shift, PreparedImage& out, uint64_t* readNanoseconds,
                              uint64_t* convertNanoseconds) const;
    // Creates the image of `prepared`, recording its upload and mipmaps.
    bool recordImage(VkCommandBuffer commands, PreparedImage const& prepared, Buffer const& staging, Image* image);
    void destroyImage(Image& image);
    // With `keepBottomLevel`, only the top level is rebuilt.
    bool buildAccelerationStructures(bool keepBottomLevel, std::string* error);
    bool writeDescriptors(std::string* error);
    void* renderBuffer(int slot, size_t bytes);
    bool dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error);
    // Records and submits a kernel run: waiting for it (chunk null), or
    // queued as chunk, which then reads the pass list from entry `offset`.
    bool run(GpuFrame const& frame, uint32_t threads, Chunk* chunk, size_t offset, double* gpuSeconds,
             std::string* error);
    bool beginChunks(size_t entries, std::string* error);
    bool submitChunk(GpuFrame const& frame, uint32_t threads, size_t offset, std::string* error);
    bool waitChunk(double* gpuSeconds, std::string* error);
    // Waits for chunk and releases its command buffer and fence.
    void finishChunk(Chunk& chunk);
    // Finishes every queued chunk (their results stay for waitChunk); false
    // when one failed.
    bool drainChunks(std::string* error);

#if PRISM_HAS_VK_OIDN
    // The denoiser (vk_oidn): its features are enabled with the device when
    // the GPU has them (denoiseUnavailable empty); it is created at its
    // first use, which logs once which denoiser the device uses.
    std::vector<const char*> enabledExtensions;
    std::string denoiseUnavailable;
    std::mutex denoiserMutex;
    bool denoiserTried = false;
    std::shared_ptr<vulkan::VulkanDenoiser> denoiser;
    std::shared_ptr<vulkan::VulkanDenoiser> gpuDenoiser();
#endif
};

#if PRISM_HAS_VK_OIDN
namespace {
// PRISM_GPU_DENOISER=cpu: the GPU devices denoise with OIDN's CPU device on
// host copies (and the Vulkan device enables no feature for vk_oidn).
bool GpuDenoiserWanted()
{
    const char* value = std::getenv("PRISM_GPU_DENOISER");
    return !(value && std::string_view(value) == "cpu");
}
} // namespace

std::shared_ptr<vulkan::VulkanDenoiser> VulkanDevice::Impl::gpuDenoiser()
{
    std::lock_guard lock(denoiserMutex);
    if (!denoiserTried) {
        denoiserTried = true;
        if (denoiseUnavailable.empty()) {
            vulkan::DenoiseDevice host;
            host.instance = instance;
            host.physical = physical;
            host.device = device;
            host.queue = queue;
            host.queueFamily = queueFamily;
            host.queueMutex = &queueMutex;
            host.extensions = enabledExtensions;
            const auto start = Clock::now();
            denoiser = vulkan::VulkanDenoiser::Create(host, &denoiseUnavailable);
            if (denoiser && gpu::GpuVerbose())
                std::cerr << std::format("PrismRT Vulkan: vk_oidn set up in {:.1f} ms\n",
                                         std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        if (denoiser)
            std::cerr << "PrismRT Vulkan: denoising on the GPU with " << denoiser->Name() << '\n';
        else
            std::cerr << "PrismRT Vulkan: denoising with OIDN's CPU device on host copies (" << denoiseUnavailable
                      << ")\n";
    }
    return denoiser;
}
#endif

VulkanDevice::Impl::~Impl()
{
    prism::load_profile::Scope profile("vulkan.destroy");
    if (!device) {
        if (instance) vkDestroyInstance(instance, nullptr);
        return;
    }
    // Its steps, for PRISM_LOAD_PROFILE.
    std::optional<prism::load_profile::Scope> step;
    auto phase = [&](const char* name) {
        step.reset();
        if (prism::load_profile::Enabled()) step.emplace(name);
    };
    phase("vulkan.destroy.workers");
    if (programListener) gpu::RemoveProgramListener(programListener);
    if (emitterListener && varyingEmitters) varyingEmitters->Unsubscribe(emitterListener);
    {
        std::lock_guard lock(speculation->mutex);
        speculation->shaders = nullptr;
    }
    if (textureObserver) textures->removePreparedObserver(textureObserver);
    {
        std::lock_guard lock(earlyMutex);
        earlyStop = true;
        earlyQueue.clear();
    }
    earlyChanged.notify_all();
    if (earlyWorker.joinable()) earlyWorker.join();
    {
        std::lock_guard lock(streaming.mutex);
        streaming.stop = true;
    }
    streaming.changed.notify_all();
    if (streaming.worker.joinable()) streaming.worker.join();
#if PRISM_HAS_VK_OIDN
    phase("vulkan.destroy.denoiser");
    // A frontend may still hold the denoiser (the delegate's denoise jobs):
    // its Vulkan objects go now, later calls use the CPU device.
    if (denoiser) denoiser->Shutdown();
#endif
    if (hung.load()) {
        // The GPU may still run a submission: waiting could take forever and
        // what it uses must not be freed. The device and its objects are
        // leaked (the driver frees them with the process).
        if (callableShaders) {
            callableShaders->cancel();
            (void)callableShaders.release();
        }
        for (auto& [key, callablePipeline] : callablePipelines) (void)callablePipeline.release();
        std::cerr << "PrismRT Vulkan: the device is left as it is after a GPU timeout\n";
        return;
    }
    phase("vulkan.destroy.idle");
    vkDeviceWaitIdle(device);
    phase("vulkan.destroy.images");
    for (auto& ready : streaming.ready) destroyImage(ready.image);
    streaming.ready.clear();
    for (auto& readback : streaming.readback) destroyBuffer(readback);
    if (streaming.fence) vkDestroyFence(device, streaming.fence, nullptr);
    if (streaming.pool) vkDestroyCommandPool(device, streaming.pool, nullptr);
    for (auto& [path, early] : earlyImages) destroyImage(early.image);
    earlyImages.clear();
    if (earlyFence) vkDestroyFence(device, earlyFence, nullptr);
    if (earlyPool) vkDestroyCommandPool(device, earlyPool, nullptr);
    phase("vulkan.destroy.callables");
    callables = nullptr;
    callablePipelines.clear();
    callableShaders.reset();
    phase("vulkan.destroy.resources");
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
    for (auto& pending : pendingUploads) destroyBuffer(pending.staging);
    for (Chunk& chunk : chunks) finishChunk(chunk);
    for (VkFence spare : spareFences) vkDestroyFence(device, spare, nullptr);
    destroyBuffer(passList);
    if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    phase("vulkan.destroy.device");
    vkDestroyDevice(device, nullptr);
    phase("vulkan.destroy.instance");
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

    // Optional: ray tracing pipelines for the material callables, shader
    // execution reordering, and driver statistics of compiled shaders.
    std::vector<const char*> enabled(std::begin(required), std::end(required));
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> deviceExtensions(count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, deviceExtensions.data());
        auto has = [&](const char* name) {
            for (auto const& e : deviceExtensions)
                if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR pipelineFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV reorderFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV};
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR statisticsFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
        pipelineFeatures.pNext = &reorderFeatures;
        reorderFeatures.pNext = &statisticsFeatures;
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &pipelineFeatures;
        vkGetPhysicalDeviceFeatures2(physical, &query);
        rayTracing.pipeline = has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) && pipelineFeatures.rayTracingPipeline;
        rayTracing.library = rayTracing.pipeline && has(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        rayTracing.reorder = rayTracing.pipeline && has(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) &&
            reorderFeatures.rayTracingInvocationReorder;
        rayTracing.executableStatistics = has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) &&
            statisticsFeatures.pipelineExecutableInfo;
        if (rayTracing.pipeline) enabled.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (rayTracing.library) enabled.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        if (rayTracing.reorder) enabled.push_back(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
        if (rayTracing.executableStatistics) enabled.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        // This process's device memory use, for the texture statistics.
        memoryBudget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if (memoryBudget) enabled.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR pipelineFeature{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    pipelineFeature.rayTracingPipeline = VK_TRUE;
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV reorderFeature{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV};
    reorderFeature.rayTracingInvocationReorder = VK_TRUE;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR statisticsFeature{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    statisticsFeature.pipelineExecutableInfo = VK_TRUE;
    void* optionalFeatures = nullptr;
    if (rayTracing.executableStatistics) {
        statisticsFeature.pNext = optionalFeatures;
        optionalFeatures = &statisticsFeature;
    }
    if (rayTracing.reorder) {
        reorderFeature.pNext = optionalFeatures;
        optionalFeatures = &reorderFeature;
    }
    if (rayTracing.pipeline) {
        pipelineFeature.pNext = optionalFeatures;
        optionalFeatures = &pipelineFeature;
    }
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rayQuery.rayQuery = VK_TRUE;
    rayQuery.pNext = optionalFeatures;
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
#if PRISM_HAS_VK_OIDN
    // The denoiser's features (vk_oidn), when the GPU has them all; chained
    // in front: features -> 1.1 -> 1.3 [-> cooperative matrices] -> 1.2 -> ...
    VkPhysicalDeviceVulkan11Features vulkan11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopMat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    bool cooperativeMatrix = false;
    if (!GpuDenoiserWanted()) {
        denoiseUnavailable = "PRISM_GPU_DENOISER=cpu";
    } else if (vulkan::EnableDenoiseFeatures(physical, features, vulkan11, vulkan12, vulkan13, coopMat,
                                             &cooperativeMatrix, enabled, &denoiseUnavailable)) {
        coopMat.pNext = &vulkan12;
        vulkan13.pNext = cooperativeMatrix ? static_cast<void*>(&coopMat) : static_cast<void*>(&vulkan12);
        vulkan11.pNext = &vulkan13;
        features.pNext = &vulkan11;
    }
    enabledExtensions = enabled;
#endif
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(enabled.size());
    deviceInfo.ppEnabledExtensionNames = enabled.data();
    if (VkResult r = vkCreateDevice(physical, &deviceInfo, nullptr, &device); r != VK_SUCCESS)
        return fail(std::string("cannot create the Vulkan device (") + ResultText(r) + ")");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    for (uint32_t h = 0; h < memoryProperties.memoryHeapCount; ++h)
        if ((memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            memoryProperties.memoryHeaps[h].size > memoryProperties.memoryHeaps[localHeap].size)
            localHeap = h;
    // ReBAR: a host-visible type covering the device-local heap (not the
    // 256 MiB BAR window, which drivers expose as a heap of its own).
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount && rebarType == ~0u; ++i) {
        const VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((memoryProperties.memoryTypes[i].propertyFlags & needed) == needed &&
            memoryProperties.memoryTypes[i].heapIndex == localHeap &&
            (memoryProperties.memoryHeaps[localHeap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            rebarType = i;
    }
    accelerationProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &accelerationProperties;
    if (rayTracing.pipeline) accelerationProperties.pNext = &rayTracing.properties;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    accelerationProperties.pNext = nullptr;
    rayTracing.properties.pNext = nullptr;
    pipelineCacheTag = PipelineCacheTag(properties2.properties);
    deviceProperties = properties2.properties;

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
    queryInfo.queryCount = 2 * (1 + kChunkQueryPairs);
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
    // the image and sampler arrays, for the compute kernel and the ray
    // tracing pipeline's shaders.
    const VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT |
        (rayTracing.pipeline ? VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR : 0);
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = slot;
        binding.descriptorType = slot == kSlotAccel ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = stages;
        bindings.push_back(binding);
    }
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = kSlotImageBase + 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerBinding.descriptorCount = kSamplerCount;
    samplerBinding.stageFlags = stages;
    samplerBinding.pImmutableSamplers = samplers;
    bindings.push_back(samplerBinding);
    VkDescriptorSetLayoutBinding imageBinding{};
    imageBinding.binding = kSlotImageBase;
    imageBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    imageBinding.descriptorCount = kMaxImages;
    imageBinding.stageFlags = stages;
    bindings.push_back(imageBinding);
    // The image array is partially bound: only the scene's images are written.
    // (A variable count would have to be on the highest binding, the samplers'.)
    std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    flags.back() = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
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
    if (rayTracing.pipeline) {
        callableShaders = std::make_unique<vulkan::MaterialCallShaders>(device, deviceProperties, rayTracing,
                                                                         pipelineLayout, pipelineCacheTag);
        callableShaders->setReporting(progressModel, logger);
        // The raygen shader depends on no scene: compile it while the scene loads.
        if (vulkan::CallableModeSetting() != vulkan::CallableMode::Never) callableShaders->prewarmRaygen();
    }
    // MDL classes are translated as they compile; with callables, their
    // shaders compile meanwhile too.
    {
        std::lock_guard lock(speculation->mutex);
        speculation->shaders = callableShaders && callableShaders->libraries() ? callableShaders.get() : nullptr;
        speculation->mode = rayTracing.pipeline ? vulkan::CallableModeSetting() : vulkan::CallableMode::Never;
    }
    // Materials of varying emission seen before this device (or from now on).
    if (varyingEmitters) {
        emitterListener = varyingEmitters->Subscribe([speculation = speculation] { speculation->varyingEmitter(); });
        if (varyingEmitters->Notes() > 0) speculation->varyingEmitter();
    }
    programListener = gpu::AddProgramListener(
        [speculation = speculation](std::shared_ptr<const gpu::MdlProgramCode> const& code) {
            speculation->program(code);
        });

    // Early image uploads (PRISM_VULKAN_EARLY_IMAGES=0 disables them).
    auto filterable = [&](VkFormat format) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT;
        return (properties.optimalTilingFeatures & needed) == needed;
    };
    unorm16Filterable = filterable(VK_FORMAT_R16_UNORM) && filterable(VK_FORMAT_R16G16_UNORM) &&
        filterable(VK_FORMAT_R16G16B16A16_UNORM);
    // Texture streaming: its own command pool and fence, and a worker that
    // waits for the first streaming scene.
    vkCreateCommandPool(device, &poolInfo, nullptr, &streaming.pool);
    vkCreateFence(device, &fenceInfo, nullptr, &streaming.fence);
    if (streaming.pool && streaming.fence) streaming.worker = std::thread([this] { streamLoop(); });
    // Streamed scenes upload only small mip tails at scene setup, so early
    // full-resolution uploads would only cost memory: they run when
    // streaming is disabled.
    const char* early = std::getenv("PRISM_VULKAN_EARLY_IMAGES");
    if (GpuCopiesEnabled() && !StreamingEnabled() && !(early && std::string_view(early) == "0")) {
        uint64_t deviceLocal = 0;
        for (uint32_t h = 0; h < memoryProperties.memoryHeapCount; ++h)
            if (memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                deviceLocal = std::max<uint64_t>(deviceLocal, memoryProperties.memoryHeaps[h].size);
        earlyBudget = gpu::TextureBudget(deviceLocal, initialTextureBudgetMiB);
        vkCreateCommandPool(device, &poolInfo, nullptr, &earlyPool);
        vkCreateFence(device, &fenceInfo, nullptr, &earlyFence);
        if (earlyPool && earlyFence)
            textureObserver = textures->addPreparedObserver(
                [this](std::shared_ptr<const prism::TextureResource> const& resource) { earlyPrepared(resource); });
    }
    return true;
}

void VulkanDevice::Impl::earlyPrepared(std::shared_ptr<const prism::TextureResource> const& resource)
{
    std::string path = resource->gpuCopyPath().string();
    {
        std::lock_guard lock(earlyMutex);
        if (earlyStop || earlyPaused || residentPaths.contains(path) || !earlySeen.insert(path).second) return;
        earlyQueue.push_back(resource);
        if (!earlyWorker.joinable()) earlyWorker = std::thread([this] { earlyLoop(); });
    }
    earlyChanged.notify_all();
}

void VulkanDevice::Impl::earlyLoop()
{
    prism::progress::Bind progressBinding(progressModel);
    prism::logging::Bind logBinding(logger);
    for (;;) {
        std::shared_ptr<const prism::TextureResource> resource;
        {
            std::unique_lock lock(earlyMutex);
            earlyChanged.wait(lock, [&] { return earlyStop || (!earlyPaused && !earlyQueue.empty()); });
            if (earlyStop) return;
            resource = std::move(earlyQueue.front());
            earlyQueue.pop_front();
            earlyBusy = true;
        }
        const auto start = Clock::now();
        bool uploaded = false;
        {
            progress::Scope phase(Phase::Upload);
            try {
                uploaded = earlyUpload(*resource);
            } catch (std::exception const& e) {
                if (gpu::GpuVerbose()) std::cerr << "PrismRT Vulkan: early image upload failed: " << e.what() << '\n';
            }
        }
        {
            std::lock_guard lock(earlyMutex);
            earlyBusy = false;
            earlySeconds += SecondsSince(start);
            if (uploaded) ++earlyUploaded;
            else ++earlySkipped;
        }
        earlyChanged.notify_all();
    }
}

bool VulkanDevice::Impl::earlyUpload(prism::TextureResource const& resource)
{
    auto const& service = textures;
    GpuCopy copy;
    std::string reason;
    copy.path = service->gpuImage(resource, &reason);
    gpu_texture::Header& h = copy.header;
    if (copy.path.empty() || !gpu_texture::Read(copy.path, &h, nullptr, &reason) ||
        int(h.width) != resource.width() || int(h.height) != resource.height() ||
        int(h.channels) != std::clamp(resource.channels(), 1, 4))
        return false;
    // Scene images other than environments: those keep float storage and are
    // uploaded with the scene.
    copy.storage = ChooseStorage(h, false, unorm16Filterable);
    const int channels = int(h.channels);
    const uint64_t estimate = gpu::ImageBytes({int(h.width), int(h.height), channels == 3 ? 4 : channels,
                                               StorageBytes(copy.storage), false}, 0);
    {
        // An image the budget already reduces among the images seen so far
        // stays reduced once the scene is complete (its share only shrinks):
        // the scene uploads it.
        std::lock_guard lock(earlyMutex);
        earlyFootprints.push_back({int(h.width), int(h.height), channels == 3 ? 4 : channels,
                                   StorageBytes(copy.storage), false});
        if (gpu::FitImages(earlyFootprints, earlyBudget).back() > 0) return false;
        if (earlyBytes + estimate > earlyBudget) return false;
        earlyBytes += estimate;
    }
    if (UsesStoredMips(h)) {
        // The copy's own mip chain, as every GPU path uses it.
        std::vector<Image> images;
        std::string reason;
        const bool ok = createStreamedImages({{&copy, 0}}, earlyPool, earlyFence, &images, nullptr, &reason);
        std::lock_guard lock(earlyMutex);
        if (!ok) {
            earlyBytes -= estimate;
            return false;
        }
        earlyImages[copy.path.string()] = {images[0], copy.storage};
        return true;
    }
    PreparedImage prepared;
    uint64_t readTime = 0, convertTime = 0;
    Buffer staging;
    Image image;
    bool ok = prepareImageFromCopy(copy, 0, prepared, &readTime, &convertTime) &&
        createBuffer(prepared.staging.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging);
    if (ok) {
        std::memcpy(staging.mapped, prepared.staging.data.get(), prepared.staging.size);
        Bytes().swap(prepared.staging);
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = earlyPool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        ok = vkAllocateCommandBuffers(device, &info, &commands) == VK_SUCCESS;
        if (ok) {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(commands, &begin);
            const bool recorded = recordImage(commands, prepared, staging, &image);
            vkEndCommandBuffer(commands);
            VkResult result = VK_ERROR_UNKNOWN;
            if (recorded && !hung.load()) {
                VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &commands;
                vkResetFences(device, 1, &earlyFence);
                {
                    std::lock_guard lock(queueMutex);
                    result = vkQueueSubmit(queue, 1, &submitInfo, earlyFence);
                }
                if (result == VK_SUCCESS) result = waitFence(earlyFence);
            }
            // A copy the GPU may still run keeps its command buffer and staging.
            if (result == VK_TIMEOUT) staging = {};
            else vkFreeCommandBuffers(device, earlyPool, 1, &commands);
            ok = recorded && result == VK_SUCCESS;
        }
    }
    destroyBuffer(staging);
    std::lock_guard lock(earlyMutex);
    if (!ok) {
        if (!hung.load()) destroyImage(image);
        earlyBytes -= estimate;
        return false;
    }
    if (prism::load_profile::Enabled())
        prism::load_profile::Mark("vulkan.image_early",
            std::format("name={} size={}x{} format={} read_ms={:.1f} convert_ms={:.1f}",
                        resource.sourcePath().filename().string(), prepared.width, prepared.height,
                        int(prepared.format), readTime / 1e6, convertTime / 1e6));
    earlyImages[copy.path.string()] = {image, copy.storage};
    return true;
}

void VulkanDevice::Impl::pauseEarly()
{
    std::unique_lock lock(earlyMutex);
    earlyPaused = true;
    earlySkipped += uint32_t(earlyQueue.size());
    earlyQueue.clear();
    earlyChanged.wait(lock, [&] { return !earlyBusy; });
}

void VulkanDevice::Impl::resumeEarly()
{
    {
        std::lock_guard lock(earlyMutex);
        earlyPaused = false;
        earlySeen.clear();
        earlyFootprints.clear();
        earlyUploaded = earlySkipped = 0;
        earlySeconds = 0.0;
    }
    earlyChanged.notify_all();
}

uint64_t VulkanDevice::Impl::deviceLocalUsage() const
{
    if (!memoryBudget) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physical, &properties);
    uint64_t usage = 0;
    for (uint32_t h = 0; h < properties.memoryProperties.memoryHeapCount; ++h)
        if (properties.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += budget.heapUsage[h];
    return usage;
}

bool VulkanDevice::Impl::createStreamedImages(std::vector<StreamJob> const& jobs, VkCommandPool pool, VkFence fence,
                                              std::vector<Image>* images, uint64_t* bytes, std::string* error)
{
    images->assign(jobs.size(), Image{});
    std::vector<Buffer> staging(jobs.size());
    std::vector<std::vector<VkDeviceSize>> offsets(jobs.size());
    std::vector<std::string> errors(jobs.size());
    std::atomic<bool> ok{true};
    std::atomic<uint64_t> staged{0};
    // Each job's levels straight into a host-cached staging buffer (the
    // digest check reads them back), converted only when the storage differs.
    tbb::parallel_for(size_t(0), jobs.size(), [&](size_t j) {
        GpuCopy const& copy = *jobs[j].copy;
        gpu_texture::Header const& h = copy.header;
        const uint32_t first = jobs[j].finest, count = h.levelCount - first;
        const bool direct = SameStorage(h.component, copy.storage) && (h.channels != 3);
        const size_t size = ConvertedBytes(h, copy.storage, first, count);
        if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging[j], true, true)) {
            errors[j] = "cannot allocate a texture staging buffer";
            ok = false;
            return;
        }
        auto* mapped = static_cast<uint8_t*>(staging[j].mapped);
        if (direct) {
            if (!gpu_texture::ReadLevels(copy.path, h, first, count, mapped, &errors[j])) {
                ok = false;
                return;
            }
            for (uint32_t l = first; l < first + count; ++l)
                offsets[j].push_back(VkDeviceSize(h.levels[l].offset - h.levels[first].offset));
        } else {
            Bytes raw(size_t(gpu_texture::LevelRangeBytes(h, first, count)));
            if (!gpu_texture::ReadLevels(copy.path, h, first, count, raw.data.get(), &errors[j])) {
                ok = false;
                return;
            }
            ConvertLevels(h, copy.storage, first, count, raw.data.get(), mapped, &offsets[j]);
        }
        staged += size;
    });
    const VkPipelineStageFlags shaderStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        (rayTracing.pipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR : 0);
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (ok) {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(device, &info, &commands) == VK_SUCCESS;
        if (!ok) errors.push_back("cannot allocate a command buffer");
    }
    if (ok) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        for (size_t j = 0; j < jobs.size() && ok; ++j) {
            GpuCopy const& copy = *jobs[j].copy;
            gpu_texture::Header const& h = copy.header;
            const uint32_t first = jobs[j].finest, levels = h.levelCount - first;
            const int stored = h.channels == 3 ? 4 : int(h.channels);
            Image& image = (*images)[j];
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = StorageFormat(copy.storage, stored);
            imageInfo.extent = {h.levels[first].width, h.levels[first].height, 1};
            imageInfo.mipLevels = levels;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (vkCreateImage(device, &imageInfo, nullptr, &image.image) != VK_SUCCESS) {
                errors.push_back("cannot create a streamed image");
                ok = false;
                break;
            }
            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(device, image.image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (allocation.memoryTypeIndex == ~0u ||
                vkAllocateMemory(device, &allocation, nullptr, &image.memory) != VK_SUCCESS) {
                errors.push_back("out of memory for streamed images");
                ok = false;
                break;
            }
            vkBindImageMemory(device, image.image, image.memory, 0);
            image.bytes = requirements.size;
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            std::vector<VkBufferImageCopy> regions(levels);
            for (uint32_t l = 0; l < levels; ++l) {
                regions[l] = {};
                regions[l].bufferOffset = offsets[j][l];
                regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                regions[l].imageExtent = {h.levels[first + l].width, h.levels[first + l].height, 1};
            }
            vkCmdCopyBufferToImage(commands, staging[j].buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   levels, regions.data());
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, shaderStages, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = imageInfo.format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            if (vkCreateImageView(device, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
                errors.push_back("cannot create a streamed image view");
                ok = false;
            }
        }
        vkEndCommandBuffer(commands);
        if (ok && hung.load()) {
            errors.push_back(vulkan::FenceTimeoutError());
            ok = false;
        }
        VkResult result = VK_SUCCESS;
        if (ok) {
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commands;
            vkResetFences(device, 1, &fence);
            {
                std::lock_guard lock(queueMutex);
                result = vkQueueSubmit(queue, 1, &submitInfo, fence);
            }
            if (result == VK_SUCCESS) result = waitFence(fence);
            if (result != VK_SUCCESS) {
                errors.push_back(submissionError(result));
                ok = false;
            }
        }
        // What a copy the GPU may still run uses stays.
        if (result == VK_TIMEOUT) {
            if (error) *error = submissionError(result);
            return false;
        }
        vkFreeCommandBuffers(device, pool, 1, &commands);
    }
    for (auto& buffer : staging) destroyBuffer(buffer);
    if (!ok) {
        for (auto& image : *images) destroyImage(image);
        if (error)
            for (auto const& e : errors)
                if (!e.empty()) { *error = e; break; }
        return false;
    }
    if (bytes) *bytes = staged.load();
    return true;
}

void VulkanDevice::Impl::streamLoop()
{
    prism::progress::Bind progressBinding(progressModel);
    prism::logging::Bind logBinding(logger);
    Streaming& s = streaming;
    std::vector<uint32_t> histograms;
    std::vector<uint8_t> failed;
    for (;;) {
        std::vector<StreamJob> jobs;
        std::vector<size_t> indices;
        {
            std::unique_lock lock(s.mutex);
            auto filled = [&] {
                for (int k = 0; k < Streaming::kReadbacks; ++k)
                    if (s.readbackState[k] == 2) return true;
                return false;
            };
            s.changed.wait(lock, [&] { return s.stop || (!s.paused && (filled() || s.replan)); });
            if (s.stop) return;
            const size_t n = s.sources.size();
            if (failed.size() != n) failed.assign(n, 0);
            // Feedback of the current accumulation; older rounds asked for another view.
            for (int k = 0; k < Streaming::kReadbacks; ++k) {
                if (s.readbackState[k] != 2) continue;
                if (s.readbackAccumulation[k] == s.accumulation && n) {
                    auto const* data = static_cast<uint32_t const*>(s.readback[k].mapped);
                    histograms.resize(n * gpu::kFeedbackLevels);
                    for (size_t p = 0; p < n; ++p)
                        std::memcpy(histograms.data() + p * gpu::kFeedbackLevels,
                                    data + size_t(s.sources[p].image) * gpu::kFeedbackLevels,
                                    gpu::kFeedbackLevels * sizeof(uint32_t));
                    const bool finer = s.planner.addFeedback(histograms.data(), n);
                    ++s.feedbackRounds;
                    if (finer) {
                        s.roundsSinceFiner = 0;
                        s.replan = true;
                    } else {
                        ++s.roundsSinceFiner;
                    }
                }
                s.readbackState[k] = 0;
            }
            if (!s.replan || s.frozen) continue;
            s.replan = false;
            s.lastPlan = s.planner.plan(s.fullFeedback); // settling: from the requests alone (deterministic)
            // Evictions first (they free memory), then promotions, smallest first.
            std::vector<std::pair<uint64_t, size_t>> promotions, demotions;
            uint32_t pending = 0;
            for (size_t p = 0; p < n; ++p) {
                const uint32_t target = s.lastPlan[p], resident = s.planner.resident(p);
                if (target == resident || failed[p]) continue;
                if (target < resident) ++pending;
                if (s.inFlight[p] != gpu::kNoRequest) continue;
                const uint64_t bytes = gpu::StreamedBytes(s.planner.image(p), target);
                (target < resident ? promotions : demotions).push_back({bytes, p});
            }
            s.lastPending = pending;
            std::sort(promotions.begin(), promotions.end());
            demotions.insert(demotions.end(), promotions.begin(), promotions.end());
            uint64_t batch = 0;
            for (auto const& [bytes, p] : demotions) {
                if (!jobs.empty() && batch + bytes > StreamingBatchBytes()) break;
                batch += bytes;
                s.inFlight[p] = s.lastPlan[p];
                jobs.push_back({&s.sources[p].copy, s.lastPlan[p]});
                indices.push_back(p);
            }
            if (pending == 0) {
                // Report what the budget keeps below the requested resolution, once per count.
                const auto stats = s.planner.stats(s.lastPlan);
                if (stats.belowRequested && !s.reportedBelow) {
                    std::cerr << std::format("PrismRT Vulkan: {} of {} streamed textures below the requested "
                                             "resolution to fit GPU memory (prism:gpuTextureBudgetMiB)\n",
                                             stats.belowRequested, stats.requested);
                    s.reportedBelow = true;
                }
            }
            if (jobs.empty()) continue;
            s.busy = true;
        }
        std::vector<Image> images;
        uint64_t bytes = 0;
        std::string error;
        const auto start = Clock::now();
        const bool ok = createStreamedImages(jobs, s.pool, s.fence, &images, &bytes, &error);
        {
            std::lock_guard lock(s.mutex);
            s.busy = false;
            if (ok && !s.paused) {
                for (size_t j = 0; j < jobs.size(); ++j) s.ready.push_back({indices[j], images[j], jobs[j].finest});
                s.uploads += jobs.size();
                s.uploadedBytes += bytes;
                if (gpu::GpuVerbose())
                    std::cerr << std::format("PrismRT Vulkan: streamed {} image{} ({:.1f} MiB) in {:.1f} ms\n",
                                             jobs.size(), jobs.size() == 1 ? "" : "s", double(bytes) / 1048576.0,
                                             SecondsSince(start) * 1e3);
            } else {
                for (auto& image : images) destroyImage(image);
                for (size_t p : indices) {
                    s.inFlight[p] = gpu::kNoRequest;
                    if (!ok && p < failed.size()) failed[p] = 1;
                }
                if (!ok) std::cerr << "PrismRT Vulkan: texture streaming failed: " << error << '\n';
            }
            s.replan = true;
        }
        s.changed.notify_all();
    }
}

void VulkanDevice::Impl::pauseStreaming()
{
    std::unique_lock lock(streaming.mutex);
    streaming.paused = true;
    streaming.active = false;
    streaming.changed.wait(lock, [&] { return !streaming.busy; });
    for (auto& ready : streaming.ready) destroyImage(ready.image);
    streaming.ready.clear();
    for (int k = 0; k < Streaming::kReadbacks; ++k) streaming.readbackState[k] = 0;
}

void VulkanDevice::Impl::resumeStreaming()
{
    {
        std::lock_guard lock(streaming.mutex);
        streaming.paused = false;
        streaming.replan = true;
    }
    streaming.changed.notify_all();
}

void VulkanDevice::Impl::applyStreaming(bool newAccumulation)
{
    Streaming& s = streaming;
    if (!s.active || !descriptorSet) return;
    std::vector<StreamReady> ready;
    {
        std::lock_guard lock(s.mutex);
        if (s.ready.empty()) return;
        // A settled (offline) accumulation keeps its residency.
        if (!newAccumulation && s.frozen) return;
        ready.swap(s.ready);
    }
    auto* records = static_cast<GpuImage*>(buffers[kSlotImages].mapped);
    std::vector<VkDescriptorImageInfo> infos(ready.size());
    std::vector<VkWriteDescriptorSet> writes(ready.size());
    std::vector<Image> retired;
    for (size_t r = 0; r < ready.size(); ++r) {
        StreamReady& item = ready[r];
        StreamSource const& source = s.sources[item.index];
        auto cached = imageCache.find(source.resource);
        if (cached != imageCache.end()) {
            retired.push_back(cached->second.image);
            cached->second.image = item.image;
            cached->second.streamedLevel = int(item.finest);
        }
        imageViews[source.image] = item.image.view;
        records[source.image].info.w = item.finest;
        infos[r] = {VK_NULL_HANDLE, item.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& write = writes[r];
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = kSlotImageBase;
        write.dstArrayElement = uint32_t(source.image);
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &infos[r];
    }
    // No submission of the render thread is pending (dispatches wait for
    // theirs), and streaming submissions never use these images: the
    // descriptors change, then the replaced images go.
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    for (Image& image : retired) destroyImage(image);
    uint64_t imageBytes = 0;
    for (auto const& [key, cached] : imageCache) imageBytes += cached.image.bytes;
    s.imageBytes = imageBytes;
    {
        std::lock_guard lock(s.mutex);
        for (StreamReady const& item : ready) {
            const uint32_t old = s.planner.resident(item.index);
            const uint32_t request = s.planner.request(item.index);
            s.planner.setResident(item.index, item.finest);
            s.inFlight[item.index] = gpu::kNoRequest;
            if (item.finest < old && request != gpu::kNoRequest && request < old) s.upgraded = true;
        }
        s.settleChanged = true;
        s.replan = true;
    }
    s.changed.notify_all();
}

bool VulkanDevice::Impl::createFeedbackBuffers(uint32_t images, std::string* error)
{
    Streaming& s = streaming;
    destroyBuffer(buffers[kSlotTextureFeedback]);
    for (auto& readback : s.readback) destroyBuffer(readback);
    const VkDeviceSize bytes = VkDeviceSize(images) * gpu::kFeedbackLevels * 4;
    if (!createBuffer(std::max<VkDeviceSize>(16, bytes),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      &buffers[kSlotTextureFeedback], false)) {
        if (error) *error = "cannot allocate the texture feedback buffer";
        return false;
    }
    s.feedbackImages = images;
    if (!images) return true;
    for (auto& readback : s.readback)
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback, true, true)) {
            if (error) *error = "cannot allocate texture feedback readbacks";
            return false;
        }
    VkCommandBuffer commands = beginCommands();
    vkCmdFillBuffer(commands, buffers[kSlotTextureFeedback].buffer, 0, bytes, 0u);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             (rayTracing.pipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR : 0),
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    return submit(commands, error);
}

void VulkanDevice::Impl::beginWork(uint32_t mode, int sample)
{
    Streaming& s = streaming;
    if (!s.active) return;
    const bool preview = mode == kModePreview;
    const bool begins = preview || ((mode == kModePass || mode == kModeImage) && sample == 0);
    if (mode == kModePass || mode == kModeImage) {
        std::lock_guard lock(s.mutex);
        s.sample = sample;
    }
    if (begins) {
        std::lock_guard lock(s.mutex);
        if (s.restartPending) {
            s.restartPending = false; // the same view again: its requests stay
        } else if (s.hold) {
            s.hold = false;           // settled: this accumulation keeps residency
            s.frozen = true;
            s.restarts = 0;
        } else if (!(s.afterPreview && !preview)) {
            // A new view (a pass right after a preview shows the preview's).
            ++s.accumulation;
            s.planner.beginAccumulation();
            s.restarts = 0;
            s.frozen = false;
            s.roundsSinceFiner = 0;
            s.reportedBelow = false;
            s.replan = true;
            s.resetFeedback = true;
        }
        s.afterPreview = preview;
        s.settleChanged = false;
    }
    if (begins) s.changed.notify_all();
    applyStreaming(begins);
    if (begins) {
        // Levels in place before the first sample are not upgrades of it.
        std::lock_guard lock(s.mutex);
        s.upgraded = false;
    }
}

bool VulkanDevice::Impl::accumulationOutdated()
{
    Streaming& s = streaming;
    if (!s.active) return false;
    applyStreaming(false);
    std::lock_guard lock(s.mutex);
    // One restart, early in the accumulation, once the residency the first
    // feedback asked for is in place; later changes sharpen it in place (the
    // samples before them washing out), never restart it.
    if (s.frozen || !s.upgraded || s.restarts >= 1 || s.sample >= int(StreamingRestartSamples())) return false;
    if (s.busy || !s.ready.empty() || s.replan || s.lastPending != 0 || s.roundsSinceFiner < 2) return false;
    ++s.restarts;
    ++s.restartsTotal;
    s.restartPending = true;
    s.upgraded = false;
    return true;
}

bool VulkanDevice::Impl::settleResidency()
{
    Streaming& s = streaming;
    if (!s.active) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(StreamingSetting("PRISM_GPU_STREAMING_SETTLE_SECONDS", 300));
    for (;;) {
        applyStreaming(true);
        std::unique_lock lock(s.mutex);
        bool feedback = false;
        for (int k = 0; k < Streaming::kReadbacks; ++k) feedback = feedback || s.readbackState[k] != 0;
        if (!s.busy && s.ready.empty() && !s.replan && !feedback && s.lastPending == 0) break;
        if (Clock::now() > deadline) {
            std::cerr << "PrismRT Vulkan: texture streaming did not settle in time "
                         "(PRISM_GPU_STREAMING_SETTLE_SECONDS); rendering with the levels resident now\n";
            break;
        }
        s.changed.wait_for(lock, std::chrono::milliseconds(5));
    }
    bool changed;
    {
        std::lock_guard lock(s.mutex);
        changed = s.settleChanged;
        s.settleChanged = false;
    }
    if (gpu::GpuVerbose()) {
        const TextureStreamingStats stats = streamingStats();
        std::cerr << std::format("PrismRT Vulkan: texture residency {}: {:.1f} MiB of {:.0f} MiB budget, {} of {} "
                                 "streamed textures requested, {} below requested; image memory {:.1f} MiB, "
                                 "device-local memory {:.1f} MiB\n",
                                 changed ? "changed" : "settled", double(stats.residentBytes) / 1048576.0,
                                 double(stats.budgetBytes) / 1048576.0, stats.requested, stats.images,
                                 stats.belowRequested, double(stats.imageBytes) / 1048576.0,
                                 double(stats.deviceBytes) / 1048576.0);
    }
    return changed;
}

TextureStreamingStats VulkanDevice::Impl::streamingStats()
{
    TextureStreamingStats stats;
    stats.deviceBytes = deviceLocalUsage();
    std::lock_guard lock(streaming.mutex);
    stats.active = streaming.active;
    stats.imageBytes = streaming.imageBytes.load();
    stats.uploads = streaming.uploads.load();
    stats.uploadedBytes = streaming.uploadedBytes.load();
    stats.feedbackRounds = streaming.feedbackRounds.load();
    stats.restarts = streaming.restartsTotal.load();
    if (!streaming.active) return stats;
    const auto planned = streaming.planner.stats(streaming.lastPlan);
    stats.budgetBytes = planned.budget;
    stats.residentBytes = planned.residentBytes;
    stats.images = planned.images;
    stats.requested = planned.requested;
    stats.pending = planned.pending;
    stats.belowRequested = planned.belowRequested;
    return stats;
}

uint32_t VulkanDevice::Impl::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return ~0u;
}

bool VulkanDevice::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer, bool hostVisible,
                                      bool hostCached, uint32_t type)
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
    const VkMemoryPropertyFlags hostFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    allocation.memoryTypeIndex = type != ~0u && (requirements.memoryTypeBits & (1u << type)) ? type : ~0u;
    if (type != ~0u && allocation.memoryTypeIndex == ~0u) {
        vkDestroyBuffer(device, buffer->buffer, nullptr);
        *buffer = {};
        return false;
    }
    // The host reads results back from these buffers every pass. Uncached
    // (write-combined) memory makes those reads several times slower than
    // the dispatch itself, so prefer a cached type when one exists.
    if (hostVisible && hostCached && type == ~0u)
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
            hostFlags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (allocation.memoryTypeIndex == ~0u)
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
            hostVisible ? hostFlags : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

// Slots the host writes or reads after their upload: the per-dispatch frame
// and pixel list, the RNG, output and accumulation, and the image records
// (texture streaming rewrites their levels). They stay host-visible; every
// other slot is read only by the kernels and the AS builds, and lives in
// device-local memory (docs/gpu_devices.md, "Vulkan memory placement").
static bool HostVisibleSlot(int slot)
{
    return slot == kSlotFrame || slot == kSlotPixels || slot == kSlotRng || slot == kSlotOutput ||
           slot == kSlotAccumulation || slot == kSlotImages;
}

VulkanDevice::Impl::Placement VulkanDevice::Impl::choosePlacement(uint64_t bufferBytes, uint64_t structureBytes)
{
    const SceneBuffers setting = SceneBuffersSetting();
    if (setting == SceneBuffers::Host) return Placement::Host;
    const Placement local =
        rebarType != ~0u && setting != SceneBuffers::Staging ? Placement::Mapped : Placement::Staged;
    if (setting != SceneBuffers::Auto) return local;
    // One budget with the textures: the buffers go to device-local memory
    // only when the whole texture budget still fits after them (the placement
    // never changes an image; a smaller texture budget would).
    if (!memoryBudget) return Placement::Host;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physical, &properties);
    // The usage includes this device's current scene buffers (the upload
    // replaces them) and its resident images (part of the texture budget).
    uint64_t own = 0, resident = 0;
    for (Buffer const& buffer : buffers)
        if (buffer.deviceLocal) own += buffer.size;
    for (auto const& [key, cached] : imageCache) resident += cached.image.bytes;
    {
        std::lock_guard lock(earlyMutex);
        resident += earlyBytes;
    }
    const uint64_t limit = budget.heapBudget[localHeap] + own;
    const uint64_t available = limit - std::min(limit, budget.heapUsage[localHeap]);
    const uint64_t textures = gpu::TextureBudget(memoryProperties.memoryHeaps[localHeap].size, sceneTextureBudgetMiB);
    const bool fits = gpu::SceneBuffersFit(available, textures, resident, bufferBytes, structureBytes);
    if (gpu::GpuVerbose())
        std::cerr << std::format("PrismRT Vulkan: scene buffers {:.1f} MiB (+{:.1f} MiB for structures), "
                                 "{:.1f} MiB available, texture budget {:.1f} MiB ({:.1f} resident): {}\n",
                                 double(bufferBytes) / 1048576.0, double(structureBytes) / 1048576.0,
                                 double(available) / 1048576.0, double(textures) / 1048576.0,
                                 double(resident) / 1048576.0, fits ? "device-local" : "host memory");
    return fits ? local : Placement::Host;
}

// Writes `size` bytes (none when `bytes` is null) and zeroes the rest of a
// mapped buffer of `capacity` bytes.
static void Fill(void* mapped, const void* bytes, size_t size, size_t capacity)
{
    const size_t copied = bytes ? size : 0;
    if (copied) std::memcpy(mapped, bytes, copied);
    std::memset(static_cast<uint8_t*>(mapped) + copied, 0, capacity - copied);
}

bool VulkanDevice::Impl::upload(int slot, const void* bytes, size_t size)
{
    destroyBuffer(buffers[slot]);
    // A slot uploaded again before the copies ran drops its earlier copy.
    for (auto it = pendingUploads.begin(); it != pendingUploads.end();)
        if (it->slot == slot) {
            destroyBuffer(it->staging);
            it = pendingUploads.erase(it);
        } else {
            ++it;
        }
    // Pass chunks update the frame and copy the pixel list in their command
    // buffers (run).
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        (slot == kSlotFrame || slot == kSlotPixels ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0);
    const Placement where = HostVisibleSlot(slot) ? Placement::Host : placement;
    if (where == Placement::Mapped && createBuffer(size, usage, &buffers[slot], true, false, rebarType)) {
        // ReBAR: written like host memory. Host writes to host-coherent
        // memory made before a queue submission are visible to its commands.
        Fill(buffers[slot].mapped, bytes, size, size_t(buffers[slot].size));
        buffers[slot].deviceLocal = true;
        return true;
    }
    if (where == Placement::Staged) {
        // Filled from a staging buffer of the whole (padded) size by
        // flushUploads.
        PendingUpload pending{slot};
        if (createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffers[slot], false)) {
            if (createBuffer(buffers[slot].size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &pending.staging, true)) {
                Fill(pending.staging.mapped, bytes, size, size_t(pending.staging.size));
                buffers[slot].deviceLocal = true;
                pendingUploads.push_back(pending);
                return true;
            }
            destroyBuffer(buffers[slot]);
        }
    }
    // Host-visible (also when no device-local memory is left).
    const bool readBack = slot == kSlotRng || slot == kSlotOutput || slot == kSlotAccumulation;
    if (!createBuffer(size, usage, &buffers[slot], true, readBack)) return false;
    if (bytes && size) std::memcpy(buffers[slot].mapped, bytes, size);
    return true;
}

bool VulkanDevice::Impl::flushUploads(std::string* error)
{
    if (pendingUploads.empty()) return true;
    VkCommandBuffer commands = beginCommands();
    for (PendingUpload const& pending : pendingUploads) {
        const VkBufferCopy region{0, 0, pending.staging.size};
        vkCmdCopyBuffer(commands, pending.staging.buffer, buffers[pending.slot].buffer, 1, &region);
    }
    // The kernels and the AS builds of later submissions read the copies.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
    // submit waits for the copies, so the staging buffers can go.
    const bool ok = submit(commands, error);
    for (PendingUpload& pending : pendingUploads) destroyBuffer(pending.staging);
    pendingUploads.clear();
    return ok;
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

VkResult VulkanDevice::Impl::waitFence(VkFence fence, bool kernel)
{
    const bool wasHung = hung.load();
    VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, wasHung ? 0 : vulkan::FenceTimeoutNanoseconds());
    // Test hook: the first kernel wait reports a timeout (after the real
    // wait, so no work is left running when the process ends).
    static std::atomic<bool> simulate = [] {
        const char* value = std::getenv("PRISM_GPU_SIMULATE_TIMEOUT");
        return value && std::string_view(value) == "1";
    }();
    if (kernel && result == VK_SUCCESS && simulate.exchange(false)) result = VK_TIMEOUT;
    if (result == VK_TIMEOUT && !hung.exchange(true))
        std::cerr << "PrismRT Vulkan: " << vulkan::FenceTimeoutError() << '\n';
    return result;
}

std::string VulkanDevice::Impl::submissionError(VkResult result)
{
    if (result == VK_TIMEOUT) return vulkan::FenceTimeoutError();
    return std::string("Vulkan submission failed: ") + ResultText(result);
}

bool VulkanDevice::Impl::submit(VkCommandBuffer commands, std::string* error, bool kernel)
{
    vkEndCommandBuffer(commands);
    if (hung.load()) {
        vkFreeCommandBuffers(device, commandPool, 1, &commands);
        if (error) *error = vulkan::FenceTimeoutError();
        return false;
    }
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &commands;
    vkResetFences(device, 1, &fence);
    VkResult result;
    {
        std::lock_guard lock(queueMutex);
        result = vkQueueSubmit(queue, 1, &info, fence);
    }
    if (result == VK_SUCCESS) result = waitFence(fence, kernel);
    // A command buffer the GPU may still run stays allocated.
    if (result != VK_TIMEOUT) vkFreeCommandBuffers(device, commandPool, 1, &commands);
    if (result != VK_SUCCESS) {
        if (error) *error = submissionError(result);
        return false;
    }
    return true;
}

bool VulkanDevice::Impl::prepareMaterials(std::shared_ptr<const gpu::MdlShaderCode>* result, bool* compile,
                                          std::string* error)
{
    auto const start = Clock::now();
    progress::Scope kernelPhase(Phase::Kernel);
    progress::Detail(Phase::Kernel, "generating MDL shader code");
    speculation->reset();
    // Material sets that certainly use callables take the per-program layout,
    // whose callables compiled ahead (CallableSpeculation); the others the
    // link unit, which decides between the compute kernel and callables
    // exactly (compilePipeline).
    perProgramReason.clear();
    const vulkan::CallableMode callableMode = rayTracing.pipeline ? vulkan::CallableModeSetting()
                                                                  : vulkan::CallableMode::Never;
    size_t estimate = 0;
    const char* layoutSetting = std::getenv("PRISM_VULKAN_PROGRAM_LAYOUT");
    if (layoutSetting && std::string_view(layoutSetting) == "0") {
        // Diagnostics: the link unit for every material set, as before.
    } else if (callableMode == vulkan::CallableMode::Always) {
        perProgramReason = "PRISM_VULKAN_CALLABLES=1";
    } else if (callableMode == vulkan::CallableMode::Auto && !data->mdlMaterials.empty()) {
        estimate = KernelBaseBytes() + gpu::EstimatedLinkUnitBytes(gpu::ProgramCodes(data->mdlMaterials));
        if (double(estimate) > double(vulkan::CallableThresholdBytes()) * kEstimateMargin)
            perProgramReason = std::format("{} KB kernel estimate above the {} KB threshold", estimate >> 10,
                                           vulkan::CallableThresholdBytes() >> 10);
    }
    const auto shared = perProgramReason.empty() ? gpu::CachedMdlShaderCode(data->mdlMaterials)
                                                 : gpu::CachedProgramShaderCode(data->mdlMaterials);
    if (const char* check = std::getenv("PRISM_VULKAN_CHECK_ESTIMATE"); check && *check == '1' && estimate) {
        // Diagnostics: the estimate against the link unit's kernel.
        auto linkUnit = gpu::CachedMdlShaderCode(data->mdlMaterials);
        std::string source;
        if (AssembleVulkanKernel(linkUnit->code, &source, nullptr))
            std::cerr << std::format("PrismRT Vulkan: kernel estimate {} KB, link unit kernel {} KB ({} programs)\n",
                                     estimate >> 10, source.size() >> 10, linkUnit->programCount);
    }
    gpu::MdlShaderCode const& code = *shared;
    if (!code.success)
        std::cerr << "PrismRT Vulkan: MDL materials unavailable on the GPU (" << code.error << ")\n";
    gpu::ApplyMdlShaderCode(*data, code);
    mdlTextureCount = uint32_t(data->mdlTextures.size());
    upload(kSlotArgumentBlocks, code.argumentBlocks.data(), code.argumentBlocks.size());
    upload(kSlotRoData, code.roData.data(), code.roData.size());
    prism::load_profile::Mark("vulkan.mdl_code",
        std::format("mdl_code_s={:.3f} translate_s={:.3f} programs={} layout={} kernel_estimate_kb={}",
                    SecondsSince(start), code.translateSeconds, code.programCount,
                    code.perProgram ? "programs" : "link_unit", estimate >> 10));
    *result = shared;
    // The same generated code (an edit that kept the materials) keeps its pipeline.
    *compile = !(shared == pipelineCode && pipeline != VK_NULL_HANDLE);
    if (*compile) {
        pipeline = VK_NULL_HANDLE;
        pipelineCode = nullptr;
        callables = nullptr;
    }
    return true;
}

bool VulkanDevice::Impl::compilePipeline(std::shared_ptr<const gpu::MdlShaderCode> const& shared,
                                         std::string* error)
{
    auto const start = Clock::now();
    progress::Scope kernelPhase(Phase::Kernel);
    gpu::MdlShaderCode const& code = *shared;
    auto const generated = start;
    if (cancelled.load()) {
        if (error) *error = "preparation cancelled";
        return false;
    }
    if (code.perProgram) return buildCallablePipeline(shared, perProgramReason, error);
    // All but the smallest material sets use the callable pipeline: above the
    // size threshold (vulkan_material_calls.h), when DXC was found unable to
    // compile the kernel, or when it fails now for lack of memory.
    const vulkan::CallableMode callableMode = rayTracing.pipeline ? vulkan::CallableModeSetting()
                                                                  : vulkan::CallableMode::Never;
    if (callableMode == vulkan::CallableMode::Always)
        return buildCallablePipeline(shared, "PRISM_VULKAN_CALLABLES=1", error);
    if (callableMode == vulkan::CallableMode::Auto) {
        std::string source;
        if (AssembleVulkanKernel(code.code, &source, nullptr, code.lightAovs)) {
            const char* retry = std::getenv("PRISM_VULKAN_RETRY_LARGE");
            const bool retryLarge = retry && *retry && *retry != '0';
            if (source.size() > vulkan::CallableThresholdBytes())
                return buildCallablePipeline(shared, std::format("{} KB kernel above the {} KB threshold",
                    source.size() >> 10, vulkan::CallableThresholdBytes() >> 10), error);
            if (!retryLarge && std::filesystem::exists(TooLargeMarker(source)))
                return buildCallablePipeline(shared, "the kernel was too large for DXC in an earlier run", error);
        }
    }
    // The compute kernel: the raygen prewarms are not needed and would only
    // compete with its compile (and hold up the device's destruction).
    if (callableShaders) callableShaders->abandonPrewarms();
    std::vector<uint32_t> spirv;
    progress::Detail(Phase::Kernel, "SPIR-V kernel");
    if (!CompileVulkanKernel(code.code, &spirv, error, code.programCount, code.lightAovs, &cancelled)) {
        if (callableMode == vulkan::CallableMode::Auto && error && gpu::HlslTooLarge(*error))
            return buildCallablePipeline(shared, "the kernel is too large for DXC", error);
        return false;
    }
    if (std::string limit = vulkan::DriverSpirvLimitError(spirv.size() * 4, "compute kernel"); !limit.empty()) {
        if (callableMode == vulkan::CallableMode::Auto) return buildCallablePipeline(shared, limit, error);
        if (error) *error = limit;
        return false;
    }
    auto const compiled = Clock::now();
    const std::string key = gpu::SourceDigest(std::string(reinterpret_cast<const char*>(spirv.data()), spirv.size() * 4));
    if (auto found = pipelines.find(key); found != pipelines.end()) {
        pipeline = found->second;
        pipelineCode = shared;
        return true;
    }
    // The driver's compile below cannot be interrupted: not started once cancelled.
    if (cancelled.load()) {
        if (error) *error = "preparation cancelled";
        return false;
    }
    progress::Detail(Phase::Kernel, "creating the Vulkan pipeline");
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirv.size() * 4;
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        if (error) *error = "cannot create the shader module";
        return false;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const bool statistics = rayTracing.executableStatistics && vulkan::PipelineStatisticsSetting();
    if (statistics) info.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = pipelineLayout;
    // The driver compiles the SPIR-V to GPU code here, which takes seconds
    // for a large kernel: keep its result next to the SPIR-V for later runs.
    const auto pipelineCacheFile = gpu::ShaderCacheDirectory("vulkan") / (key + "-" + pipelineCacheTag + ".vkpc");
    std::vector<char> cacheData = vulkan::ReadCacheFile(pipelineCacheFile);
    if (!vulkan::PipelineCacheDataMatches(cacheData, deviceProperties)) cacheData.clear();
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = cacheData.size();
    cacheInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache) != VK_SUCCESS) {
        // Data the driver rejects outright: start empty.
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;
        if (vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache) != VK_SUCCESS)
            pipelineCache = VK_NULL_HANDLE;
    }
    VkResult result;
    {
        std::optional<vulkan::CompileMemorySlot> slot;
        if (cacheData.empty()) slot.emplace(vulkan::DriverCompileBytes(spirv.size() * 4));
        result = vkCreateComputePipelines(device, pipelineCache, 1, &info, nullptr, &pipeline);
    }
    vkDestroyShaderModule(device, module, nullptr);
    if (pipelineCache) {
        size_t size = 0;
        if (result == VK_SUCCESS && vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) == VK_SUCCESS &&
            size > 0 && size != cacheData.size()) {
            std::vector<char> data(size);
            if (vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) == VK_SUCCESS)
                vulkan::WriteCacheFile(pipelineCacheFile, data.data(), size);
        }
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
    }
    if (result != VK_SUCCESS) {
        if (error) *error = std::string("cannot create the compute pipeline: ") + ResultText(result);
        pipeline = VK_NULL_HANDLE;
        return false;
    }
    pipelines[key] = pipeline;
    pipelineCode = shared;
    if (statistics) vulkan::PrintPipelineStatistics(device, pipeline, "compute kernel");
    prism::load_profile::Mark("vulkan.pipeline",
        std::format("callables=0 spirv_s={:.3f} pipeline_s={:.3f} programs={}",
                    std::chrono::duration<double>(compiled - generated).count(), SecondsSince(compiled),
                    code.programCount));
    if (gpu::GpuVerbose())
        std::cerr << std::format("PrismRT Vulkan: shader setup {:.1f} s (SPIR-V {:.1f} s, pipeline {:.1f} s)\n",
                                 SecondsSince(start), std::chrono::duration<double>(compiled - generated).count(),
                                 SecondsSince(compiled));
    return true;
}

bool VulkanDevice::Impl::buildCallablePipeline(std::shared_ptr<const gpu::MdlShaderCode> const& shared,
                                               std::string const& reason, std::string* error)
{
    gpu::MdlShaderCode const& code = *shared;
    auto const start = Clock::now();
    const bool reorder = rayTracing.reorder && vulkan::ReorderSetting();
    std::string identity = code.code;
    if (code.perProgram)
        for (auto const& program : code.programs) identity += "|" + program->digest;
    const std::string key = gpu::SourceDigest(identity) + (code.perProgram ? "|programs" : "") +
        (reorder ? "|reorder" : "") + (code.varyingEmitters ? "|emitters" : "") + (code.lightAovs ? "|aovs" : "");
    std::cerr << std::format("PrismRT Vulkan: {} MDL programs as callable shaders ({}){}{}\n", code.programCount,
                             reason, reorder ? ", reordered by program" : "",
                             code.lightAovs ? ", with light AOVs" : "");
    auto found = callablePipelines.find(key);
    if (found == callablePipelines.end()) {
        auto built = std::make_unique<vulkan::MaterialCallPipeline>(device, physical, rayTracing, pipelineCacheTag);
        if (!built->build(code, pipelineLayout, reorder, *callableShaders, error)) {
            if (error) *error = "callable material pipeline: " + *error;
            return false;
        }
        found = callablePipelines.emplace(key, std::move(built)).first;
        auto const& r = found->second->report();
        prism::load_profile::Mark("vulkan.pipeline",
            std::format("callables=1 libraries={} programs={} dxc_compiled={} shaders_s={:.3f} dxc_max_s={:.3f} "
                        "driver_max_s={:.3f} driver_sum_s={:.3f} libraries_compiled={} libraries_cached={} "
                        "raygen_dxc={} raygen_dxc_s={:.3f} raygen_driver_s={:.3f} raygen_wait_s={:.3f} "
                        "pipeline_s={:.3f} total_s={:.3f} layout={} reused={} reused_wait_s={:.3f}",
                        r.libraries ? 1 : 0, r.programs, r.compiled, r.shadersSeconds, r.dxcMaxSeconds,
                        r.driverMaxSeconds, r.driverSumSeconds, r.librariesCompiled, r.librariesCached,
                        r.raygenCompiled ? 1 : 0, r.raygenSeconds, r.raygenDriverSeconds, r.raygenWaitSeconds,
                        r.pipelineSeconds, SecondsSince(start), code.perProgram ? "programs" : "link_unit",
                        r.reused, r.reusedWaitSeconds));
        if (gpu::GpuVerbose())
            std::cerr << std::format(
                "PrismRT Vulkan: callable pipeline {:.2f} s: {} programs, {} DXC compiles (slowest {:.1f} s, peak {} "
                "MiB), shaders {:.2f} s; raygen {}, waited {:.2f} s; {} {:.3f} s{}\n",
                SecondsSince(start), r.programs, r.compiled, r.dxcMaxSeconds, r.dxcPeakBytes >> 20, r.shadersSeconds,
                r.raygenCompiled ? std::format("DXC {:.1f} s", r.raygenSeconds) : std::string("ready"),
                r.raygenWaitSeconds,
                r.libraries ? std::format("{} libraries compiled ({} from pipeline caches, slowest {:.2f} s, sum "
                                          "{:.2f} s), link", r.librariesCompiled, r.librariesCached,
                                          r.driverMaxSeconds, r.driverSumSeconds)
                            : std::string("driver pipeline"),
                r.pipelineSeconds,
                r.libraries ? std::string()
                            : std::format(" ({}, {} threads, +{} MiB commit)",
                                          r.pipelineCacheHit ? "pipeline cache hit" : "no pipeline cache",
                                          r.pipelineThreads, r.pipelineCommitBytes >> 20));
    }
    callables = found->second.get();
    pipeline = callables->pipeline();
    pipelineCode = shared;
    return true;
}

// Creates the image of `prepared`, recording its upload and mipmaps.
void VulkanDevice::Impl::finishImage(PreparedImage& out, int width, int height) const
{
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(physical, out.format, &formatProperties);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const bool mipmapped = (formatProperties.optimalTilingFeatures & blit) == blit;
    out.levels = mipmapped ? 1u + uint32_t(std::floor(std::log2(double(std::max(width, height))))) : 1u;
    out.width = width;
    out.height = height;
}

// From the GPU copy: read, then pad RGB to RGBA, reduce or convert as the
// storage needs (usually nothing: the texels are uploaded as stored).
bool VulkanDevice::Impl::prepareImageFromCopy(GpuCopy const& copy, int shift, PreparedImage& out,
                                              uint64_t* readNanoseconds, uint64_t* convertNanoseconds) const
{
    auto nanosecondsSince = [](Clock::time_point start) {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    };
    gpu_texture::Header const& h = copy.header;
    const int channels = int(h.channels), stored = channels == 3 ? 4 : channels;
    const auto readStart = Clock::now();
    // Level 0 only: the image's mips are generated with blits (recordImage).
    Bytes payload(size_t(h.levels[0].bytes));
    std::string reason;
    if (!gpu_texture::ReadLevels(copy.path, h, 0, 1, payload.data.get(), &reason)) {
        std::cerr << std::format("PrismRT Vulkan: cannot read {}: {}; decoding its prepared file\n",
                                 copy.path.filename().string(), reason);
        return false;
    }
    *readNanoseconds = nanosecondsSince(readStart);
    const auto convertStart = Clock::now();
    int width = int(h.width), height = int(h.height);
    const Storage storage = copy.storage;
    const int outBytes = StorageBytes(storage);
    const size_t texels = size_t(width) * height;
    const bool asStored = shift == 0 && SameStorage(h.component, storage);
    if (asStored && stored == channels) {
        out.staging = std::move(payload);
    } else if (asStored) {
        // Pad RGB to RGBA; the fourth component reads as zero.
        const int cb = outBytes;
        out.staging = Bytes(texels * stored * cb);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, texels, size_t(1) << 14),
            [&](tbb::blocked_range<size_t> const& range) {
                for (size_t p = range.begin(); p != range.end(); ++p) {
                    uint8_t* dst = out.staging.data.get() + p * stored * cb;
                    std::memcpy(dst, payload.data.get() + p * channels * cb, size_t(channels) * cb);
                    std::memset(dst + channels * cb, 0, size_t(stored - channels) * cb);
                }
            });
    } else {
        // Through float: OIIO's conversion, exactly as the .tx.exr holds it.
        std::vector<float> pixels(texels * channels);
        const OIIO::TypeDesc type = ComponentType(h.component);
        constexpr size_t chunk = size_t(1) << 18;
        tbb::parallel_for(size_t(0), (pixels.size() + chunk - 1) / chunk, [&](size_t c) {
            const size_t begin = c * chunk, count = std::min(chunk, pixels.size() - begin);
            OIIO::convert_pixel_values(type, payload.data.get() + begin * type.size(), OIIO::TypeDesc::FLOAT,
                                       pixels.data() + begin, int(count));
        });
        Bytes().swap(payload);
        gpu::Downsample(pixels, width, height, channels, shift);
        const size_t reducedTexels = size_t(width) * height;
        out.staging = Bytes(reducedTexels * stored * outBytes);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, reducedTexels, size_t(1) << 14),
            [&](tbb::blocked_range<size_t> const& range) {
                for (size_t p = range.begin(); p != range.end(); ++p)
                    for (int c = 0; c < stored; ++c)
                        EncodeTexel(storage, c < channels ? pixels[p * channels + c] : 0.0f,
                                    out.staging.data.get() + (p * stored + c) * outBytes);
            });
    }
    out.format = StorageFormat(storage, stored);
    out.fromCopy = true;
    finishImage(out, width, height);
    *convertNanoseconds = nanosecondsSince(convertStart);
    return true;
}

bool VulkanDevice::Impl::recordImage(VkCommandBuffer commands, PreparedImage const& prepared, Buffer const& staging,
                                     Image* image)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = prepared.format;
    imageInfo.extent = {uint32_t(prepared.width), uint32_t(prepared.height), 1};
    imageInfo.mipLevels = prepared.levels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &image->image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image->image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocation, nullptr, &image->memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image->image, image->memory, 0);
    image->bytes = requirements.size;
    const uint32_t levels = prepared.levels;
    auto barrier = [&](uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage,
                       VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
        vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {uint32_t(prepared.width), uint32_t(prepared.height), 1};
    vkCmdCopyBufferToImage(commands, staging.buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int w = prepared.width, h = prepared.height;
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
        vkCmdBlitImage(commands, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        barrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    barrier(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = prepared.format;
    // Absent channels read as zero (Metal semantics for R/RG formats).
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    return vkCreateImageView(device, &viewInfo, nullptr, &image->view) == VK_SUCCESS;
}

void VulkanDevice::Impl::destroyImage(Image& image)
{
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = {};
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
    auto const& service = textures;
    // Images beyond the memory budget are uploaded at lower resolutions.
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint64_t deviceLocal = 0;
    for (uint32_t h = 0; h < memory.memoryHeapCount; ++h)
        if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            deviceLocal = std::max<uint64_t>(deviceLocal, memory.memoryHeaps[h].size);
    std::vector<bool> isEnvironment(d.images.size(), false);
    for (auto const& environment : d.environments)
        if (environment.info.y < d.images.size()) isEnvironment[environment.info.y] = true;
    // UNORM16 storage needs sampling, linear filtering and blits (for mips).
    auto filterable = [&](VkFormat format) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT;
        return (properties.optimalTilingFeatures & needed) == needed;
    };
    const bool unorm16Filterable = filterable(VK_FORMAT_R16_UNORM) && filterable(VK_FORMAT_R16G16_UNORM) &&
        filterable(VK_FORMAT_R16G16B16A16_UNORM);
    // Each image's GPU-ready copy (texture_gpu_file.h), when it has one: the
    // texels at the source's precision, uploaded without decoding the float
    // .tx.exr. Images without one take the .tx.exr path below.
    std::vector<GpuCopy> copies(d.images.size());
    const auto uploadStart = Clock::now();
    const auto copiesStart = Clock::now();
    if (GpuCopiesEnabled()) {
        tbb::parallel_for(size_t(0), d.images.size(), [&](size_t i) {
            auto const& resource = *d.images[i];
            std::string reason;
            GpuCopy& copy = copies[i];
            copy.path = service->gpuImage(resource, &reason);
            gpu_texture::Header& h = copy.header;
            copy.valid = !copy.path.empty() && gpu_texture::Read(copy.path, &h, nullptr, &reason) &&
                int(h.width) == resource.width() && int(h.height) == resource.height() &&
                int(h.channels) == std::clamp(resource.channels(), 1, 4);
            if (copy.valid)
                copy.storage = ChooseStorage(h, isEnvironment[i], unorm16Filterable);
            else if (gpu::GpuVerbose())
                std::cerr << std::format("PrismRT Vulkan: no GPU copy of {} ({}); decoding its prepared file\n",
                                         resource.sourcePath().filename().string(), reason);
        });
    }
    const double copiesSeconds = SecondsSince(copiesStart);
    // Streamed images (texture_streaming.h): GPU copies with their whole mip
    // chain, other than environments. The others are uploaded whole, as
    // without streaming, and their memory comes off the streaming budget.
    std::vector<bool> streamed(d.images.size(), false);
    if (streamScene)
        for (size_t i = 0; i < d.images.size(); ++i) {
            gpu_texture::Header const& h = copies[i].header;
            streamed[i] = copies[i].valid && !isEnvironment[i] &&
                h.levelCount == gpu_texture::FullChainLevels(h.width, h.height);
        }
    std::vector<gpu::ImageFootprint> footprints(d.images.size());
    std::vector<gpu::ImageFootprint> pinnedFootprints;
    std::vector<size_t> pinned;
    for (size_t i = 0; i < d.images.size(); ++i) {
        const int channels = std::clamp(d.images[i]->channels(), 1, 4);
        // Without a GPU copy the storage is unknown until the texels are
        // read; estimate the 8-bit case as before.
        const int componentBytes = copies[i].valid ? StorageBytes(copies[i].storage) : isEnvironment[i] ? 4 : 1;
        footprints[i] = {d.images[i]->width(), d.images[i]->height(), channels == 3 ? 4 : channels,
                         componentBytes, bool(isEnvironment[i])};
        if (!streamed[i]) {
            pinned.push_back(i);
            pinnedFootprints.push_back(footprints[i]);
        }
    }
    const uint64_t textureBudget = gpu::TextureBudget(deviceLocal, sceneTextureBudgetMiB);
    std::vector<int> shift(d.images.size(), 0);
    uint64_t pinnedBytes = 0;
    {
        const std::vector<int> pinnedShift = gpu::FitImages(pinnedFootprints, textureBudget);
        for (size_t k = 0; k < pinned.size(); ++k) {
            shift[pinned[k]] = pinnedShift[k];
            pinnedBytes += gpu::ImageBytes(pinnedFootprints[k], pinnedShift[k]);
        }
    }
    size_t reduced = 0;
    for (int s : shift) reduced += s > 0 ? 1 : 0;
    if (reduced && reduced != reportedReducedImages) {
        std::cerr << "PrismRT Vulkan: " << reduced << " of " << d.images.size()
                  << " images uploaded at reduced resolution to fit GPU memory "
                     "(prism:gpuTextureBudgetMiB)\n";
    }
    reportedReducedImages = reduced;
    // The streamed images' entries, and their tails (or what an earlier
    // scene left resident) for those not in the cache.
    Streaming& s = streaming;
    std::vector<gpu::StreamedImage> streamImages;
    std::vector<uint32_t> streamResident;
    s.sources.clear();
    std::vector<size_t> streamMissing; // stream source indices
    const uint32_t tailSize = StreamingTailSize();
    // Images uploaded while the scene loaded: those this scene uses with the
    // storage and full resolution it chose are taken, the others freed (the
    // uploads stop meanwhile and continue for later scenes).
    pauseEarly();
    uint32_t claimed = 0, earlyUploadedNow = 0, earlySkippedNow = 0;
    double earlySecondsNow = 0.0;
    struct EarlyResume {
        Impl& impl;
        ~EarlyResume() { impl.resumeEarly(); }
    } earlyResume{*this};
    struct EarlyEnd {
        Impl& impl;
        std::vector<GpuCopy> const& copies;
        ~EarlyEnd()
        {
            std::lock_guard lock(impl.earlyMutex);
            for (auto& [path, early] : impl.earlyImages) impl.destroyImage(early.image);
            impl.earlyImages.clear();
            impl.earlyBytes = 0;
            impl.residentPaths.clear();
            for (auto const& copy : copies)
                if (copy.valid) impl.residentPaths.insert(copy.path.string());
        }
    } earlyEnd{*this, copies};
    {
        std::lock_guard lock(earlyMutex);
        earlyUploadedNow = earlyUploaded;
        earlySkippedNow = earlySkipped;
        earlySecondsNow = earlySeconds;
    }
    std::vector<size_t> missing;
    for (size_t i = 0; i < d.images.size(); ++i) {
        auto const& resource = d.images[i];
        info[i].info = {uint32_t(std::max(1, resource->width() >> shift[i])),
                        uint32_t(std::max(1, resource->height() >> shift[i])),
                        uint32_t(resource->channels()), 0u};
        auto found = imageCache.find(resource.get());
        if (streamed[i]) {
            gpu_texture::Header const& h = copies[i].header;
            const int stored = h.channels == 3 ? 4 : int(h.channels);
            gpu::StreamedImage image{h.width, h.height, h.levelCount, uint32_t(stored * StorageBytes(copies[i].storage)),
                                     gpu::TailLevel(h.width, h.height, h.levelCount, tailSize)};
            uint32_t resident = image.tailLevel;
            if (found != imageCache.end() && !found->second.resource.expired() && found->second.streamedLevel >= 0 &&
                found->second.storage == copies[i].storage) {
                resident = std::min(uint32_t(found->second.streamedLevel), image.tailLevel);
                if (resident != uint32_t(found->second.streamedLevel)) {
                    destroyImage(found->second.image); // coarser than its tail: create the tail again
                    imageCache.erase(found);
                    streamMissing.push_back(s.sources.size());
                } else {
                    imageViews[i] = found->second.image.view;
                }
            } else {
                if (found != imageCache.end()) {
                    destroyImage(found->second.image);
                    imageCache.erase(found);
                }
                streamMissing.push_back(s.sources.size());
            }
            info[i].info = {h.width, h.height, uint32_t(resource->channels()), resident};
            s.sources.push_back({i, copies[i], resource.get()});
            streamImages.push_back(image);
            streamResident.push_back(resident);
            continue;
        }
        if (found != imageCache.end() && !found->second.resource.expired() && found->second.streamedLevel < 0 &&
            found->second.floatPrecision == isEnvironment[i] && found->second.shift == shift[i]) {
            imageViews[i] = found->second.image.view;
            continue;
        }
        if (found != imageCache.end()) {
            vkDestroyImageView(device, found->second.image.view, nullptr);
            vkDestroyImage(device, found->second.image.image, nullptr);
            vkFreeMemory(device, found->second.image.memory, nullptr);
            imageCache.erase(found);
        }
        if (shift[i] == 0 && copies[i].valid && !isEnvironment[i]) {
            std::lock_guard lock(earlyMutex);
            auto early = earlyImages.find(copies[i].path.string());
            if (early != earlyImages.end() && early->second.storage == copies[i].storage) {
                imageCache[resource.get()] = {resource, early->second.image, false, 0};
                imageViews[i] = early->second.image.view;
                earlyImages.erase(early);
                ++claimed;
                continue;
            }
        }
        missing.push_back(i);
    }
    // Whole images with a stored mip chain upload its levels from `shift`
    // on (the reduced resolution is then a stored level too).
    std::vector<size_t> storedMissing;
    {
        std::vector<size_t> rest;
        for (size_t i : missing)
            (copies[i].valid && UsesStoredMips(copies[i].header) ? storedMissing : rest).push_back(i);
        missing.swap(rest);
    }
    // The calling thread's model, also for the image workers.
    prism::Progress* const uploadProgress = prism::Progress::Current();
    progress::SetTotal(Phase::Upload, missing.size() + storedMissing.size());
    progress::Detail(Phase::Upload, missing.empty() ? "images (cached)" : "images");

    using Prepared = PreparedImage;
    std::atomic<uint64_t> readNanoseconds{0}, convertNanoseconds{0}, stagedBytes{0}, copiedImages{0};
    auto nanosecondsSince = [](Clock::time_point start) {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    };
    auto finish = [&](Prepared& out, int width, int height) { finishImage(out, width, height); };
    auto profileImage = [&](prism::TextureResource const& resource, Prepared const& out, int channels, int s,
                            uint64_t readTime, uint64_t convertTime) {
        readNanoseconds += readTime;
        convertNanoseconds += convertTime;
        stagedBytes += out.staging.size;
        if (prism::load_profile::Enabled())
            prism::load_profile::Mark("vulkan.image",
                std::format("name={} size={}x{} channels={} shift={} format={} bytes={} read_ms={:.1f} "
                            "convert_ms={:.1f} source={}",
                            resource.sourcePath().filename().string(), out.width, out.height, channels, s,
                            int(out.format), out.staging.size, readTime / 1e6, convertTime / 1e6,
                            out.fromCopy ? "gpu" : "tx"));
    };
    // From the GPU copy: read, then pad RGB to RGBA, reduce or convert as the
    // storage needs (usually nothing: the texels are uploaded as stored).
    auto prepareFromCopy = [&](size_t i, Prepared& out) {
        uint64_t readTime = 0, convertTime = 0;
        if (!prepareImageFromCopy(copies[i], shift[i], out, &readTime, &convertTime)) return false;
        ++copiedImages;
        profileImage(*d.images[i], out, int(copies[i].header.channels), shift[i], readTime, convertTime);
        return true;
    };
    auto prepare = [&](size_t i, Prepared& out) {
        if (copies[i].valid && prepareFromCopy(i, out)) {
            if (uploadProgress) uploadProgress->Add(Phase::Upload);
            return;
        }
        auto const& resource = d.images[i];
        const bool floatPrecision = isEnvironment[i];
        int width = resource->width(), height = resource->height();
        const int channels = std::clamp(resource->channels(), 1, 4);
        const auto readStart = Clock::now();
        std::vector<float> pixels(size_t(width) * height * channels);
        if (!ReadPreparedImage(*resource, channels, pixels.data()) &&
            !service->readPixels(*resource, 0, 0, width, height, 0, channels, pixels.data()))
            std::fill(pixels.begin(), pixels.end(), 0.0f);
        const uint64_t readTime = nanosecondsSince(readStart);
        const auto convertStart = Clock::now();
        gpu::Downsample(pixels, width, height, channels, shift[i]);
        struct Scan {
            bool quantized;
            float peak;
        };
        const Scan scan = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, pixels.size(), size_t(1) << 16), Scan{!floatPrecision, 0.0f},
            [&](tbb::blocked_range<size_t> const& range, Scan scan) {
                for (size_t p = range.begin(); p != range.end(); ++p) {
                    const float v = pixels[p];
                    if (scan.quantized) {
                        const float k = v * 255.0f;
                        if (!(v >= 0.0f && v <= 1.0f) || std::abs(k - std::round(k)) > 1e-3f) scan.quantized = false;
                    }
                    if (std::isfinite(v)) scan.peak = std::max(scan.peak, std::abs(v));
                }
                return scan;
            },
            [](Scan a, Scan b) { return Scan{a.quantized && b.quantized, std::max(a.peak, b.peak)}; });
        const Storage storage = scan.quantized ? Storage::Unorm8
            : (!floatPrecision && scan.peak < 60000.0f) ? Storage::Float16 : Storage::Float32;
        const int stored = channels == 3 ? 4 : channels;
        const int componentBytes = StorageBytes(storage);
        out.format = StorageFormat(storage, stored);
        finish(out, width, height);
        out.staging = Bytes(size_t(width) * height * stored * componentBytes);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, size_t(width) * height, size_t(1) << 14),
            [&](tbb::blocked_range<size_t> const& range) {
                for (size_t p = range.begin(); p != range.end(); ++p)
                    for (int c = 0; c < stored; ++c)
                        EncodeTexel(storage, c < channels ? pixels[p * channels + c] : 0.0f,
                                    out.staging.data.get() + (p * stored + c) * componentBytes);
            });
        profileImage(*resource, out, channels, shift[i], readTime, nanosecondsSince(convertStart));
        if (uploadProgress) uploadProgress->Add(Phase::Upload);
    };

    auto record = [&](VkCommandBuffer commands, Prepared const& prepared, Buffer const& staging, Image* image) {
        return recordImage(commands, prepared, staging, image);
    };
    auto destroyImage = [&](Image& image) { this->destroyImage(image); };

    // Images are prepared in parallel, in batches that bound the host
    // memory, and each batch is uploaded with one submission.
    constexpr uint64_t batchBytes = uint64_t(2) << 30;
    uint64_t prepareNanoseconds = 0, submitNanoseconds = 0;
    for (size_t first = 0; first < missing.size();) {
        size_t last = first;
        uint64_t bytes = 0;
        while (last < missing.size()) {
            auto const& resource = d.images[missing[last]];
            // The float texels and the converted copy (at most as large).
            const uint64_t imageBytes = uint64_t(resource->width()) * uint64_t(resource->height()) *
                uint64_t(std::clamp(resource->channels(), 1, 4)) * 8;
            if (last > first && bytes + imageBytes > batchBytes) break;
            bytes += imageBytes;
            ++last;
        }
        std::vector<Prepared> prepared(last - first);
        const auto batchStart = Clock::now();
        tbb::parallel_for(first, last, [&](size_t m) { prepare(missing[m], prepared[m - first]); });
        prepareNanoseconds += nanosecondsSince(batchStart);
        const auto submitStart = Clock::now();
        std::vector<Buffer> staging(prepared.size());
        std::vector<Image> images(prepared.size());
        VkCommandBuffer commands = beginCommands();
        bool recorded = true;
        for (size_t k = 0; k < prepared.size() && recorded; ++k) {
            if (!createBuffer(prepared[k].staging.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging[k])) {
                if (error) *error = "cannot allocate an image staging buffer";
                recorded = false;
                break;
            }
            std::memcpy(staging[k].mapped, prepared[k].staging.data.get(), prepared[k].staging.size);
            Bytes().swap(prepared[k].staging);
            if (!record(commands, prepared[k], staging[k], &images[k])) {
                if (error) *error = "out of memory for images";
                recorded = false;
            }
        }
        const bool submitted = submit(commands, recorded ? error : nullptr) && recorded;
        submitNanoseconds += nanosecondsSince(submitStart);
        for (auto& buffer : staging) destroyBuffer(buffer);
        if (!submitted) {
            for (auto& image : images) destroyImage(image);
            return false;
        }
        for (size_t k = 0; k < prepared.size(); ++k) {
            const size_t i = missing[first + k];
            imageCache[d.images[i].get()] = {d.images[i], images[k], bool(isEnvironment[i]), shift[i]};
            imageViews[i] = images[k].view;
        }
        first = last;
    }
    for (size_t first = 0; first < storedMissing.size();) {
        std::vector<StreamJob> jobs;
        uint64_t bytes = 0;
        size_t last = first;
        while (last < storedMissing.size() && (last == first || bytes < StreamingBatchBytes())) {
            const size_t i = storedMissing[last++];
            jobs.push_back({&copies[i], uint32_t(shift[i])});
            bytes += gpu_texture::LevelRangeBytes(copies[i].header, uint32_t(shift[i]),
                                                  copies[i].header.levelCount - uint32_t(shift[i]));
        }
        std::vector<Image> images;
        uint64_t staged = 0;
        if (!createStreamedImages(jobs, commandPool, fence, &images, &staged, error)) return false;
        stagedBytes += staged;
        copiedImages += images.size();
        for (size_t k = 0; k < images.size(); ++k) {
            const size_t i = storedMissing[first + k];
            CachedImage cached{d.images[i], images[k], bool(isEnvironment[i]), shift[i]};
            cached.storage = copies[i].storage;
            imageCache[d.images[i].get()] = cached;
            imageViews[i] = images[k].view;
            if (uploadProgress) uploadProgress->Add(Phase::Upload);
        }
        first = last;
    }
    missing.insert(missing.end(), storedMissing.begin(), storedMissing.end()); // counted in the summary
    // Streamed images: their tails, in batches.
    const auto streamStart = Clock::now();
    uint64_t tailBytes = 0;
    for (size_t first = 0; first < streamMissing.size();) {
        std::vector<StreamJob> jobs;
        uint64_t bytes = 0;
        size_t last = first;
        while (last < streamMissing.size() && (last == first || bytes < StreamingBatchBytes())) {
            const size_t p = streamMissing[last++];
            jobs.push_back({&s.sources[p].copy, streamResident[p]});
            bytes += gpu::StreamedBytes(streamImages[p], streamResident[p]);
        }
        std::vector<Image> images;
        uint64_t staged = 0;
        if (!createStreamedImages(jobs, commandPool, fence, &images, &staged, error)) return false;
        tailBytes += staged;
        for (size_t k = 0; k < images.size(); ++k) {
            StreamSource const& source = s.sources[streamMissing[first + k]];
            CachedImage cached{d.images[source.image], images[k], false, 0};
            cached.streamedLevel = int(streamResident[streamMissing[first + k]]);
            cached.storage = source.copy.storage;
            imageCache[source.resource] = cached;
            imageViews[source.image] = images[k].view;
        }
        first = last;
    }
    {
        std::lock_guard lock(s.mutex);
        const uint64_t streamBudget = textureBudget > pinnedBytes ? textureBudget - pinnedBytes : 0;
        s.planner.reset(streamImages, streamBudget, streamResident, StreamingThreshold());
        s.inFlight.assign(s.sources.size(), gpu::kNoRequest);
        s.lastPlan.clear();
        s.lastPending = 0;
        ++s.accumulation;
        s.restarts = 0;
        s.frozen = s.hold = s.restartPending = s.upgraded = s.afterPreview = false;
        s.reportedBelow = false;
    }
    if (!s.sources.empty() && (gpu::GpuVerbose() || prism::load_profile::Enabled())) {
        uint64_t resident = 0;
        for (size_t p = 0; p < streamImages.size(); ++p) resident += gpu::StreamedBytes(streamImages[p], streamResident[p]);
        const std::string summary = std::format(
            "streaming {} of {} images: {} tails created ({:.1f} MiB read, {:.2f} s), {:.1f} MiB resident, "
            "budget {:.0f} MiB ({:.0f} MiB of whole images)",
            s.sources.size(), d.images.size(), streamMissing.size(), double(tailBytes) / 1048576.0,
            SecondsSince(streamStart), double(resident) / 1048576.0,
            double(textureBudget > pinnedBytes ? textureBudget - pinnedBytes : 0) / 1048576.0,
            double(pinnedBytes) / 1048576.0);
        if (gpu::GpuVerbose()) std::cerr << "PrismRT Vulkan: " << summary << '\n';
        prism::load_profile::Mark("vulkan.streaming", summary);
    }
    for (auto it = imageCache.begin(); it != imageCache.end();) {
        if (!it->second.resource.expired()) { ++it; continue; }
        vkDestroyImageView(device, it->second.image.view, nullptr);
        vkDestroyImage(device, it->second.image.image, nullptr);
        vkFreeMemory(device, it->second.image.memory, nullptr);
        it = imageCache.erase(it);
    }
    VkDeviceSize bytes = 0;
    for (auto const& [key, cached] : imageCache) bytes += cached.image.bytes;
    s.imageBytes = bytes;
    if (gpu::GpuVerbose() || prism::load_profile::Enabled()) {
        const std::string summary = std::format(
            "{} images ({} uploaded, {} reduced), {:.0f} MiB of image memory, {:.0f} MiB staged; "
            "{:.2f} s total: prepare {:.2f} s (read {:.2f} s, convert {:.2f} s summed over threads), "
            "copy and submit {:.2f} s; {} from GPU copies (found in {:.2f} s); {} uploaded early ({} taken, {} left to the scene, {:.2f} s)", d.images.size(), missing.size(),
            reduced, double(bytes) / 1048576.0, double(stagedBytes.load()) / 1048576.0, SecondsSince(uploadStart),
            prepareNanoseconds / 1e9, readNanoseconds.load() / 1e9, convertNanoseconds.load() / 1e9,
            submitNanoseconds / 1e9, copiedImages.load(), copiesSeconds, earlyUploadedNow, claimed, earlySkippedNow,
            earlySecondsNow);
        if (gpu::GpuVerbose()) std::cerr << "PrismRT Vulkan: " << summary << '\n';
        prism::load_profile::Mark("vulkan.images", summary);
    }
    return upload(kSlotImages, info.data(), info.size() * sizeof(GpuImage));
}

bool VulkanDevice::Impl::buildAccelerationStructures(bool keepBottomLevel, std::string* error)
{
    GpuSceneData const& d = *data;
    if (!keepBottomLevel) {
        for (auto& structure : bottomLevel) destroyStructure(structure);
        bottomLevel.clear();
    }
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
    // Kept bottom levels keep their prototype map (the prototypes are part
    // of the geometry SameGeometry compares).
    if (!keepBottomLevel) structureOfPrototype.assign(d.prototypes.size(), ~0u);
    std::vector<VkAabbPositionsKHR> boxes;
    std::vector<size_t> boxOffsets(d.prototypes.size(), 0);
    for (size_t p = 0; !keepBottomLevel && p < d.prototypes.size(); ++p) {
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
    for (size_t p = 0; !keepBottomLevel && p < d.prototypes.size(); ++p) {
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
        // prism:honorDoubleSided: the trace skips culled back faces itself
        // (gpu_culled_triangle), so their triangles must reach it.
        if ((d.instances[i].info.y & kInstanceCullBackFaces) != 0u)
            instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
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
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
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
    // After a GPU timeout the scene renders on the CPU (RendersOnCpu).
    if (hung.load()) {
        if (error) *error = vulkan::FenceTimeoutError();
        return false;
    }
    if (boundScene == &scene && !dirty.load() && pipeline) return true;
    if (failedScene == &scene && !dirty.load()) {
        if (error) *error = failedError;
        return false;
    }
    if (cancelled.load()) {
        if (error) *error = "preparation cancelled";
        return false;
    }
    failedScene = nullptr;
    // Streaming stops while the scene's images change (and resumes for a
    // scene that streams).
    pauseStreaming();
    streamScene = StreamingEnabled() && scene.textureStreaming;
    sceneTextureBudgetMiB = scene.gpuTextureBudgetMiB;
    {
        std::lock_guard lock(queueMutex);
        vkDeviceWaitIdle(device);
    }
    auto const sceneStart = Clock::now();
    dirty = false;
    boundScene = &scene;
    progress::Scope uploadPhase(Phase::Upload);
    progress::Detail(Phase::Upload, "converting the scene");
    auto previous = std::move(data);
    data = std::make_unique<GpuSceneData>(gpu::BuildGpuScene(scene));
    // Edits that keep the geometry (placements, materials, lights) keep the
    // geometry buffers and the bottom-level structures, as on Metal.
    const bool sameGeometry = previous && geometryCurrent && gpu::SameGeometry(*previous, *data);
    previous.reset();
    geometryCurrent = false;
    auto const sceneBuilt = Clock::now();
    for (auto const& warning : data->warnings) std::cerr << "PrismRT Vulkan: " << warning << '\n';
    // The pipeline compiles while the scene uploads: it needs only the
    // generated code, and the uploads do not touch the pipeline.
    std::string compileError;
    std::future<bool> compiled;
    auto fail = [&] {
        if (compiled.valid()) compiled.wait();
        pipeline = VK_NULL_HANDLE;
        boundScene = nullptr;
        failedScene = &scene;
        failedError = error && !error->empty() ? *error : std::string("the scene could not be prepared");
        return false;
    };
    std::shared_ptr<const gpu::MdlShaderCode> code;
    bool compile = false;
    // Between steps, a cancel (teardown) ends the preparation; the scene is
    // left unprepared (it would be prepared again, but the device goes).
    auto stop = [&] {
        if (!cancelled.load()) return false;
        if (error) *error = "preparation cancelled";
        return true;
    };
    if (!prepareMaterials(&code, &compile, error)) return fail();
    if (stop()) return fail();
    auto const piped = Clock::now();
    if (compile)
        compiled = std::async(std::launch::async, [this, code, &compileError,
                                                   model = prism::Progress::Current(),
                                                   log = &prism::logging::Current()] {
            prism::progress::Bind progressBinding(model);
            prism::logging::Bind logBinding(log);
            return compilePipeline(code, &compileError);
        });
    struct Join {
        std::future<bool>& f;
        ~Join() { if (f.valid()) f.wait(); }
    } join{compiled};
    GpuSceneData const& d = *data;
    progress::Detail(Phase::Upload, "buffers");
    struct SlotData {
        int slot;
        const void* data;
        size_t bytes;
    };
    auto of = [](int slot, auto const& v) { return SlotData{slot, v.data(), v.size() * sizeof(v[0])}; };
    static const GpuSceneData::EnvironmentTables noTables;
    auto const& tables = d.environmentTables ? *d.environmentTables : noTables;
    const SlotData slots[] = {
        of(kSlotPositions, d.positions), of(kSlotNormals, d.normals), of(kSlotTexcoords, d.texcoords),
        of(kSlotTangents, d.tangents), of(kSlotIndices, d.indices), of(kSlotPrototypes, d.prototypes),
        of(kSlotInstances, d.instances), of(kSlotPrimitiveMaterials, d.primitiveMaterials),
        of(kSlotMaterials, d.materials), of(kSlotLegacyTextures, d.legacyTextures), of(kSlotSpheres, d.spheres),
        of(kSlotEnvironment, d.environments), of(kSlotLights, d.lights), of(kSlotLightAlias, d.lightAlias),
        of(kSlotPrimvarValues, d.primvarValues), of(kSlotPrimvarDirectory, d.primvars),
        of(kSlotEnvTexelPdf, tables.texelPdf), of(kSlotEnvAliasProbability, tables.aliasProbability),
        of(kSlotEnvAlias, tables.alias), of(kSlotMdlTextures, d.mdlTextures), of(kSlotMdlTiles, d.mdlTiles),
        of(kSlotBsdfData, d.bsdfData)};
    // The acceleration structures built from the positions and indices are
    // estimated at their size.
    uint64_t sceneBytes = 0;
    for (SlotData const& s : slots) sceneBytes += s.bytes;
    placement = choosePlacement(sceneBytes, slots[0].bytes + slots[4].bytes); // positions, indices
    auto geometrySlot = [](int slot) {
        return slot == kSlotPositions || slot == kSlotNormals || slot == kSlotTexcoords || slot == kSlotTangents ||
               slot == kSlotIndices || slot == kSlotPrototypes || slot == kSlotSpheres;
    };
    for (SlotData const& s : slots)
        if (!sameGeometry || !geometrySlot(s.slot)) upload(s.slot, s.data, s.bytes);
    if (!buffers[kSlotFrame].buffer) upload(kSlotFrame, nullptr, sizeof(GpuFrame));
    for (int slot : {kSlotPixels, kSlotRng, kSlotOutput, kSlotAccumulation})
        if (!buffers[slot].buffer) upload(slot, nullptr, 16);
    if (!flushUploads(error) || stop()) return fail();
    if (gpu::GpuVerbose()) {
        uint64_t local = 0;
        int count = 0;
        for (Buffer const& buffer : buffers)
            if (buffer.deviceLocal) {
                local += buffer.size;
                ++count;
            }
        std::cerr << std::format("PrismRT Vulkan: {} scene buffers ({:.1f} MiB) in device-local memory ({})\n",
                                 count, double(local) / 1048576.0,
                                 placement == Placement::Mapped ? "ReBAR, written directly"
                                 : placement == Placement::Staged ? "staging copies" : "none: host memory");
    }
    auto const uploaded = Clock::now();
    if (!uploadImages(error) || stop()) return fail();
    if (!createFeedbackBuffers(streaming.sources.empty() ? 0u : uint32_t(d.images.size()), error)) return fail();
    auto const imaged = Clock::now();
    progress::SetTotal(Phase::Upload, 0);
    progress::Detail(Phase::Upload, "acceleration structures");
    if (stop() || !buildAccelerationStructures(sameGeometry, error)) return fail();
    geometryCurrent = true;
    auto const structured = Clock::now();
    progress::Detail(Phase::Upload, "waiting for the shaders");
    if (compiled.valid() && !compiled.get()) {
        if (error) *error = compileError;
        return fail();
    }
    if (!pipeline) {
        if (error && error->empty()) *error = "no pipeline";
        return fail();
    }
    auto const shadersReady = Clock::now();
    if (!writeDescriptors(error)) return fail();
    {
        std::lock_guard lock(streaming.mutex);
        streaming.active = !streaming.sources.empty();
    }
    if (streaming.active) resumeStreaming();
    prism::load_profile::Mark("vulkan.scene",
        std::format("total_s={:.3f} gpu_scene_s={:.3f} mdl_code_s={:.3f} buffers_s={:.3f} images_s={:.3f} "
                    "acceleration_s={:.3f} shader_wait_s={:.3f} images={} geometry_kept={}",
                    SecondsSince(sceneStart), std::chrono::duration<double>(sceneBuilt - sceneStart).count(),
                    std::chrono::duration<double>(piped - sceneBuilt).count(),
                    std::chrono::duration<double>(uploaded - piped).count(),
                    std::chrono::duration<double>(imaged - uploaded).count(),
                    std::chrono::duration<double>(structured - imaged).count(),
                    std::chrono::duration<double>(shadersReady - structured).count(), d.images.size(),
                    sameGeometry ? 1 : 0));
    if (gpu::GpuVerbose())
        std::cerr << std::format("PrismRT Vulkan: scene {:.3f} s (buffers {:.3f}, images {:.3f}, "
                                 "acceleration structures {:.3f}; {} prototypes, {} instances{})\n",
                                 SecondsSince(sceneStart), std::chrono::duration<double>(uploaded - sceneStart).count(),
                                 std::chrono::duration<double>(imaged - uploaded).count(), SecondsSince(imaged),
                                 d.prototypes.size(), d.instances.size(),
                                 sameGeometry ? "; geometry kept" : "");
    return true;
}

bool VulkanDevice::Impl::prepareLightAovs(std::string* error)
{
    if (!pipelineCode || !pipeline) {
        if (error) *error = "no pipeline";
        return false;
    }
    if (aovBase == pipelineCode && aovPipeline) return true;
    // compilePipeline builds the variant as the scene's pipeline (cached by
    // its SPIR-V or callable key): keep it aside and restore the scene's.
    const VkPipeline base = pipeline;
    vulkan::MaterialCallPipeline const* const baseCallables = callables;
    const auto baseCode = pipelineCode;
    auto variant = std::make_shared<gpu::MdlShaderCode>(*pipelineCode);
    variant->lightAovs = true;
    callables = nullptr;
    const bool built = compilePipeline(variant, error);
    if (built) {
        aovPipeline = pipeline;
        aovCallables = callables;
        aovBase = baseCode;
    }
    pipeline = base;
    callables = baseCallables;
    pipelineCode = baseCode;
    return built;
}

void* VulkanDevice::Impl::renderBuffer(int slot, size_t bytes)
{
    if (buffers[slot].size < bytes) {
        // Queued chunks may use the buffer (their results stay for waitChunk).
        (void)drainChunks(nullptr);
        if (hung.load()) {
            // Hung work may still use the old buffer: it stays (nothing runs
            // on the device again, the new one only keeps callers going).
            buffers[slot] = {};
        } else {
            std::lock_guard lock(queueMutex);
            vkDeviceWaitIdle(device);
        }
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

bool VulkanDevice::Impl::dispatch(GpuFrame const& work, uint32_t threads, double* gpuSeconds, std::string* error)
{
    if (threads == 0) return true;
    // The frame is written from the host: no queued chunk may still read it.
    if (!drainChunks(error)) return false;
    return run(work, threads, nullptr, 0, gpuSeconds, error);
}

bool VulkanDevice::Impl::beginChunks(size_t entries, std::string* error)
{
    // Leftovers of a pass that ended early (an exception in a callback).
    while (!chunks.empty()) {
        finishChunk(chunks.front());
        chunks.pop_front();
    }
    const VkDeviceSize bytes = std::max<VkDeviceSize>(16, VkDeviceSize(entries) * 4);
    if (passList.size < bytes) {
        destroyBuffer(passList);
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &passList, true)) {
            if (error) *error = "cannot allocate the pass pixel list";
            return false;
        }
    }
    // Every chunk's list fits.
    renderBuffer(kSlotPixels, size_t(bytes));
    return true;
}

bool VulkanDevice::Impl::submitChunk(GpuFrame const& work, uint32_t threads, size_t offset, std::string* error)
{
    Chunk chunk;
    if (!run(work, threads, &chunk, offset, nullptr, error)) return false;
    chunks.push_back(std::move(chunk));
    return true;
}

void VulkanDevice::Impl::finishChunk(Chunk& chunk)
{
    if (chunk.done) return;
    chunk.done = true;
    const VkResult result = waitFence(chunk.fence, true);
    if (result != VK_SUCCESS) {
        chunk.ok = false;
        chunk.error = submissionError(result);
    } else {
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps, chunk.query, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            chunk.seconds = double(ticks[1] - ticks[0]) * timestampPeriod * 1e-9;
    }
    // The command buffer and fence of a submission the GPU may still run stay.
    if (result != VK_TIMEOUT) {
        vkFreeCommandBuffers(device, commandPool, 1, &chunk.commands);
        spareFences.push_back(chunk.fence);
    }
    chunk.commands = VK_NULL_HANDLE;
    chunk.fence = VK_NULL_HANDLE;
    if (chunk.readback >= 0) {
        Streaming& s = streaming;
        {
            std::lock_guard lock(s.mutex);
            s.readbackState[chunk.readback] = chunk.ok ? 2 : 0;
        }
        s.changed.notify_all();
    }
}

bool VulkanDevice::Impl::drainChunks(std::string* error)
{
    bool ok = true;
    for (Chunk& chunk : chunks) {
        finishChunk(chunk);
        if (!chunk.ok && ok) {
            ok = false;
            if (error) *error = chunk.error;
        }
    }
    return ok;
}

bool VulkanDevice::Impl::waitChunk(double* gpuSeconds, std::string* error)
{
    if (chunks.empty()) return true;
    Chunk chunk = std::move(chunks.front());
    chunks.pop_front();
    finishChunk(chunk);
    if (!chunk.ok) {
        if (error) *error = chunk.error;
        return false;
    }
    if (gpuSeconds) *gpuSeconds += chunk.seconds;
    return true;
}

bool VulkanDevice::Impl::run(GpuFrame const& work, uint32_t threads, Chunk* chunk, size_t offset,
                             double* gpuSeconds, std::string* error)
{
    if (hung.load()) {
        if (error) *error = vulkan::FenceTimeoutError();
        return false;
    }
    GpuFrame frame = work;
    // Texture streaming: finished images swap in now, and the dispatch
    // records texture feedback into a free readback (none free: it stays in
    // the device buffer for the next copy).
    Streaming& s = streaming;
    int readback = -1;
    if (s.active && descriptorSet) {
        bool swaps;
        {
            std::lock_guard lock(s.mutex);
            swaps = !s.ready.empty() && !s.frozen;
        }
        // The descriptors change only with no queued chunk using them.
        if (swaps && chunk && !drainChunks(error)) return false;
    }
    if (s.active) applyStreaming(false);
    // Feedback is recorded while residency can still change in this
    // accumulation (PRISM_GPU_STREAMING_FEEDBACK=1: always, for measuring
    // its cost; 0: only while settling).
    static const int feedbackMode = [] {
        const char* value = std::getenv("PRISM_GPU_STREAMING_FEEDBACK");
        return value && *value ? std::atoi(value) : -1;
    }();
    bool feedback = false;
    if (s.active) {
        std::lock_guard lock(s.mutex);
        feedback = s.fullFeedback || feedbackMode == 1 ||
            (feedbackMode != 0 && !s.frozen);
    }
    if (feedback) {
        std::lock_guard lock(s.mutex);
        frame.settings.z |= kFrameTextureFeedback;
        frame.streaming = {s.feedbackImages, StreamingLodMargin(), uint32_t(s.phase++),
                           s.fullFeedback ? 1u : StreamingFeedbackStride()};
        for (int k = 0; k < Streaming::kReadbacks && readback < 0; ++k)
            if (s.readbackState[k] == 0) {
                readback = k;
                s.readbackState[k] = 1;
                s.readbackAccumulation[k] = s.accumulation;
            }
    }
    const VkPipelineStageFlags shaderStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        (rayTracing.pipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR : 0);
    VkCommandBuffer commands = beginCommands();
    if (!chunk) {
        std::memcpy(buffers[kSlotFrame].mapped, &frame, sizeof(frame));
    } else {
        // After the kernels queued before (which read the frame and the
        // list, and write the same buffers at other pixels), this chunk's
        // frame and pixels, then its kernel.
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, shaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT | shaderStages, 0, 1, &before, 0,
                             nullptr, 0, nullptr);
        static_assert(sizeof(GpuFrame) % 4 == 0 && sizeof(GpuFrame) <= 65536);
        vkCmdUpdateBuffer(commands, buffers[kSlotFrame].buffer, 0, sizeof(frame), &frame);
        const VkBufferCopy region{VkDeviceSize(offset) * 4, 0, VkDeviceSize(threads) * 4};
        vkCmdCopyBuffer(commands, passList.buffer, buffers[kSlotPixels].buffer, 1, &region);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, shaderStages, 0, 1, &after, 0, nullptr, 0,
                             nullptr);
    }
    bool resetFeedback = false;
    if (feedback) {
        std::lock_guard lock(s.mutex);
        resetFeedback = std::exchange(s.resetFeedback, false);
    }
    if (resetFeedback) {
        // A new view: what earlier dispatches recorded without a readback goes.
        const VkDeviceSize bytes = VkDeviceSize(s.feedbackImages) * gpu::kFeedbackLevels * 4;
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, shaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0,
                             nullptr);
        vkCmdFillBuffer(commands, buffers[kSlotTextureFeedback].buffer, 0, bytes, 0u);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, shaderStages, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
    }
    const uint32_t query = chunk ? 2 * (1 + chunkQueries++ % kChunkQueryPairs) : 0;
    vkCmdResetQueryPool(commands, timestamps, query, 2);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, query);
    // The light AOV variant (prepareLightAovs) for frames that ask for it.
    const bool aovs = (frame.settings.z & kFrameLightAovs) != 0u && aovPipeline && aovBase == pipelineCode;
    vulkan::MaterialCallPipeline const* const calls = aovs ? aovCallables : callables;
    if (calls) {
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout, 0, 1,
                                &descriptorSet, 0, nullptr);
        calls->trace(commands, threads);
    } else {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, aovs ? aovPipeline : pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0,
                                nullptr);
        vkCmdDispatch(commands, (threads + 63) / 64, 1, 1);
    }
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, query + 1);
    if (readback >= 0) {
        Buffer const& feedback = buffers[kSlotTextureFeedback];
        const VkDeviceSize bytes = VkDeviceSize(s.feedbackImages) * gpu::kFeedbackLevels * 4;
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, shaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
        VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(commands, feedback.buffer, s.readback[readback].buffer, 1, &region);
        VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        copied.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copied.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &copied, 0,
                             nullptr, 0, nullptr);
        vkCmdFillBuffer(commands, feedback.buffer, 0, bytes, 0u);
        VkMemoryBarrier reset{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        reset.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        reset.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, shaderStages | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &reset, 0, nullptr, 0, nullptr);
    }
    if (chunk) {
        // Queued: finishChunk waits for it and hands over the readback.
        vkEndCommandBuffer(commands);
        VkFence chunkFence = VK_NULL_HANDLE;
        if (!spareFences.empty()) {
            chunkFence = spareFences.back();
            spareFences.pop_back();
            vkResetFences(device, 1, &chunkFence);
        } else {
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(device, &fenceInfo, nullptr, &chunkFence);
        }
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &commands;
        VkResult result;
        {
            std::lock_guard lock(queueMutex);
            result = vkQueueSubmit(queue, 1, &info, chunkFence);
        }
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device, commandPool, 1, &commands);
            spareFences.push_back(chunkFence);
            if (readback >= 0) {
                {
                    std::lock_guard lock(s.mutex);
                    s.readbackState[readback] = 0;
                }
                s.changed.notify_all();
            }
            if (error) *error = submissionError(result);
            return false;
        }
        chunk->commands = commands;
        chunk->fence = chunkFence;
        chunk->query = query;
        chunk->readback = readback;
        return true;
    }
    const bool submitted = submit(commands, error, true);
    if (readback >= 0) {
        {
            std::lock_guard lock(s.mutex);
            s.readbackState[readback] = submitted ? 2 : 0;
        }
        s.changed.notify_all();
    }
    if (!submitted) return false;
    if (gpuSeconds) {
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            *gpuSeconds += double(ticks[1] - ticks[0]) * timestampPeriod * 1e-9;
    }
    return true;
}

std::unique_ptr<VulkanDevice> VulkanDevice::Create(DeviceResources const& options, std::string* reason)
{
    // DeviceOptions::disableVulkan (PRISM_DISABLE_VULKAN) is checked by
    // CreateDevice (device.cpp).
    std::string source;
    if (!gpu::LoadShaderSource("gpu/kernel.hlsl", &source)) {
        if (reason) *reason = "the Vulkan kernel sources are missing";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->initialTextureBudgetMiB = options.initialTextureBudgetMiB;
    impl->textures = options.textures ? options.textures : prism::TextureService::Acquire();
    impl->progressModel = options.progress;
    if (options.logger) impl->logger = options.logger;
    impl->varyingEmitters = options.varyingEmitters;
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

void VulkanDevice::SceneChanged(SceneManager& scene)
{
    m_impl->dirty = true;
    m_cpu.SceneChanged(scene);
}

bool VulkanDevice::RendersOnCpu(SceneManager& scene)
{
    std::lock_guard lock(m_renderMutex);
    std::string error;
    if (m_impl->ensureScene(scene, &error)) {
        if (!m_cpuReason.empty()) progress::Device("vulkan: " + m_impl->name);
        m_cpuReason.clear();
        return false;
    }
    // A cancelled preparation (the device goes away) is not a GPU failure.
    if (m_impl->cancelled.load()) return true;
    if (error != m_cpuReason) {
        progress::Device("cpu (Vulkan fallback: " + FallbackSummary(error) + ")");
        std::cerr << LogPrefix() << error << '\n'
                  << LogPrefix() << "cannot render the scene on the GPU; rendering it with the CPU device\n";
        m_cpuReason = error;
    }
    return true;
}

std::vector<float> VulkanDevice::RenderImage(SceneManager& scene, ImageRequest const& request)
{
    if (RendersOnCpu(scene)) return m_cpu.RenderImage(scene, request);
    return GpuRenderDevice::RenderImage(scene, request);
}

void VulkanDevice::RenderPass(SceneManager& scene, PassRequest const& request)
{
    const bool traces = request.traceColor || (request.tracePrimary && request.resolvePrimary);
    if (traces && RendersOnCpu(scene)) return m_cpu.RenderPass(scene, request);
    GpuRenderDevice::RenderPass(scene, request);
}

bool VulkanDevice::RenderPreview(SceneManager& scene, PreviewRequest const& request)
{
    if (RendersOnCpu(scene)) return m_cpu.RenderPreview(scene, request);
    return GpuRenderDevice::RenderPreview(scene, request);
}

void VulkanDevice::Prepare(SceneManager& scene)
{
    if (RendersOnCpu(scene) && !m_impl->cancelled.load()) m_cpu.Prepare(scene);
}

void VulkanDevice::CancelPreparation()
{
    m_impl->cancelled.store(true);
    // Thread safe; the device and its shader set outlive this call (the
    // caller holds the device).
    if (m_impl->callableShaders) m_impl->callableShaders->cancel();
    m_cpu.CancelPreparation();
}

bool VulkanDevice::AccumulationOutdated(SceneManager& scene)
{
    std::lock_guard lock(m_renderMutex);
    if (m_impl->boundScene != &scene || m_impl->dirty.load()) return false;
    return m_impl->accumulationOutdated();
}

bool VulkanDevice::SettleTextures(SceneManager& scene, PassRequest const& request)
{
    if (RendersOnCpu(scene)) return false;
    return GpuRenderDevice::SettleTextures(scene, request);
}

TextureStreamingStats VulkanDevice::TextureStreaming() const { return m_impl->streamingStats(); }
void VulkanDevice::BeginWork(uint32_t mode, int sample) { m_impl->beginWork(mode, sample); }
bool VulkanDevice::StreamsTextures() const { return m_impl->streaming.active; }
bool VulkanDevice::SettleResidency() { return m_impl->settleResidency(); }
void VulkanDevice::FullFeedback(bool full)
{
    std::lock_guard lock(m_impl->streaming.mutex);
    m_impl->streaming.fullFeedback = full;
}
void VulkanDevice::HoldResidency()
{
    std::lock_guard lock(m_impl->streaming.mutex);
    m_impl->streaming.hold = m_impl->streaming.active;
}

bool VulkanDevice::PrepareScene(SceneManager& scene, std::string* error) { return m_impl->ensureScene(scene, error); }
bool VulkanDevice::PrepareLightAovs(std::string* error) { return m_impl->prepareLightAovs(error); }
bool VulkanDevice::GuidesOnlyAovs() const
{
    // PRISM_GPU_GUIDES_ONLY=0 (diagnostics): always the four-entry layout.
    static const bool enabled = [] {
        const char* value = std::getenv("PRISM_GPU_GUIDES_ONLY");
        return !(value && std::string_view(value) == "0");
    }();
    return enabled;
}
GpuSceneData const& VulkanDevice::Data() const { return *m_impl->data; }
uint32_t VulkanDevice::MdlTextureCount() const { return m_impl->mdlTextureCount; }
void* VulkanDevice::RenderBuffer(int slot, size_t bytes) { return m_impl->renderBuffer(slot, bytes); }
bool VulkanDevice::Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error)
{
    return m_impl->dispatch(frame, threads, gpuSeconds, error);
}
int VulkanDevice::ChunksInFlight() const
{
    // Two: one traced while the host reads the other back. Each more one
    // delays a stop (an abandoned pass waits for its submitted chunks).
    // PRISM_GPU_PASS_CHUNKS_IN_FLIGHT=1 (diagnostics): one at a time, as before.
    static const int chunks = [] {
        const char* value = std::getenv("PRISM_GPU_PASS_CHUNKS_IN_FLIGHT");
        return value && *value ? std::clamp(std::atoi(value), 1, int(Impl::kChunkQueryPairs)) : 2;
    }();
    return chunks;
}
bool VulkanDevice::BeginChunks(size_t entries, std::string* error) { return m_impl->beginChunks(entries, error); }
bool VulkanDevice::SubmitChunk(GpuFrame const& frame, uint32_t const* list, uint32_t count, size_t offset,
                               std::string* error)
{
    if (count == 0) return true;
    // Host-coherent: written before the submission that copies it.
    std::memcpy(static_cast<uint32_t*>(m_impl->passList.mapped) + offset, list, size_t(count) * 4);
    return m_impl->submitChunk(frame, count, offset, error);
}
bool VulkanDevice::WaitChunk(double* gpuSeconds, std::string* error) { return m_impl->waitChunk(gpuSeconds, error); }

std::shared_ptr<Denoiser> VulkanDevice::CreateDenoiser(std::string* error)
{
#if PRISM_HAS_VK_OIDN
    if (auto denoiser = m_impl->gpuDenoiser()) return denoiser;
#endif
    return GpuRenderDevice::CreateDenoiser(error);
}

bool VulkanDevice::DenoiseAccumulation(ImageRequest const& request, std::vector<int> const& samples)
{
#if PRISM_HAS_VK_OIDN
    // Under m_renderMutex (RenderImage): the accumulation stays in place.
    auto denoiser = m_impl->gpuDenoiser();
    if (!denoiser) return false;
    denoiser->DenoiseAccumulation(m_impl->buffers[kSlotAccumulation].address, samples, request.width,
                                  request.height, request.denoise, m_accumulationGuidesOnly, request.denoised);
    if (request.denoiser) *request.denoiser = denoiser->Name();
    return true;
#else
    (void)request;
    (void)samples;
    return false;
#endif
}

} // namespace prism::device
