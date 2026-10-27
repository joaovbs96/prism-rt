// OIDN's Metal device behind the Denoiser interface (metal_denoiser.h,
// docs/denoising.md).

#import <Metal/Metal.h>

#include "metal_denoiser.h"

#include "../gpu/gpu_device.h"

#include <OpenImageDenoise/oidn.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace prism::device {

namespace {

// The resolve kernel: a whole-image accumulation (GpuRenderDevice::RenderImage)
// into the packed RGB colour, albedo and normal images OIDN reads. The colour
// sums are two float4 per pixel (sum, sum of squares), then come the light
// AOV sums, four float4 per pixel: the albedo in the first, the normal in the
// w of the first three (shaders/gpu/accessors.hlsl). Each is multiplied by
// the pixel's 1 / samples, as the host does (gpu_device.cpp, ReadLightAovs).
// The Vulkan device's twin is mode 0 of shaders/gpu/vulkan/denoise.hlsl.
constexpr const char* kResolveSource = R"msl(
#include <metal_stdlib>
using namespace metal;

kernel void prism_denoise_resolve(device const float4* sums [[buffer(0)]],
                                  device const float* scales [[buffer(1)]],
                                  device float* color [[buffer(2)]],
                                  device float* albedo [[buffer(3)]],
                                  device float* normal [[buffer(4)]],
                                  constant uint& count [[buffer(5)]],
                                  uint i [[thread_position_in_grid]])
{
    if (i >= count) return;
    const float s = scales[i];
    const float4 c = sums[2 * i];
    device const float4* aov = sums + 2 * count + 4 * i;
    const float4 e0 = aov[0];
    const float4 e1 = aov[1];
    const float4 e2 = aov[2];
    color[3 * i] = c.x * s;
    color[3 * i + 1] = c.y * s;
    color[3 * i + 2] = c.z * s;
    albedo[3 * i] = e0.x * s;
    albedo[3 * i + 1] = e0.y * s;
    albedo[3 * i + 2] = e0.z * s;
    normal[3 * i] = e0.w * s;
    normal[3 * i + 1] = e1.w * s;
    normal[3 * i + 2] = e2.w * s;
}
)msl";

// The denoiser's images, packed RGB float (12 bytes per pixel).
enum ImageSlot { kColor, kAlbedo, kNormal, kOutput, kImageCount };

oidn::Quality OidnQuality(DenoiseQuality quality)
{
    switch (quality) {
    case DenoiseQuality::Fast: return oidn::Quality::Fast;
    case DenoiseQuality::Balanced: return oidn::Quality::Balanced;
    case DenoiseQuality::High: return oidn::Quality::High;
    }
    return oidn::Quality::High;
}

std::string NsErrorText(NSError* error)
{
    return error && error.localizedDescription ? std::string(error.localizedDescription.UTF8String)
                                               : std::string("unknown error");
}

} // namespace

bool MetalGpuDenoiserWanted()
{
    const char* value = std::getenv("PRISM_GPU_DENOISER");
    return !(value && std::string_view(value) == "cpu");
}

struct MetalDenoiser::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> resolve = nil;
    oidn::DeviceRef oidnDevice;
    // Filters are kept between calls: committing one again for images of
    // the same size reuses its memory.
    oidn::FilterRef color = {}, albedoFilter = {}, normalFilter = {};
    // Shared storage (host-visible on Apple GPUs), hazard tracked as
    // oidnNewSharedBufferFromMetal requires; `shared` is OIDN's view of it.
    struct Image {
        id<MTLBuffer> buffer = nil;
        oidn::BufferRef shared;
    };
    Image images[kImageCount];
    id<MTLBuffer> scales = nil; // 1 / samples per pixel (DenoiseAccumulation)
    std::mutex mutex;
    bool shutdown = false;

    void check(const char* what);
    id<MTLBuffer> newBuffer(size_t bytes);
    // Every image holds at least `pixels` RGB pixels.
    void reserve(size_t pixels);
    // Denoises images[kColor] (with the guides that are set) into
    // images[kOutput]; the guides are prefiltered in place when asked.
    void filter(unsigned width, unsigned height, DenoiseSettings const& settings, bool albedo, bool normal);
    void prefilter(oidn::FilterRef& guideFilter, const char* name, ImageSlot slot, unsigned width, unsigned height,
                   DenoiseSettings const& settings);
    void destroy();
};

void MetalDenoiser::Impl::check(const char* what)
{
    const char* message = nullptr;
    if (oidnDevice.getError(message) != oidn::Error::None)
        throw std::runtime_error(std::string(what) + ": " + (message ? message : "unknown error"));
}

id<MTLBuffer> MetalDenoiser::Impl::newBuffer(size_t bytes)
{
    id<MTLBuffer> buffer = [device newBufferWithLength:std::max<size_t>(bytes, 16)
                                               options:MTLResourceStorageModeShared |
                                                       MTLResourceHazardTrackingModeTracked];
    if (!buffer) throw std::runtime_error(std::format("denoise: cannot allocate a Metal buffer of {} bytes", bytes));
    return buffer;
}

void MetalDenoiser::Impl::reserve(size_t pixels)
{
    const size_t bytes = pixels * 3 * sizeof(float);
    for (Image& image : images) {
        if (image.buffer && image.buffer.length >= bytes) continue;
        // The filters may still reference the old buffer: they take the new
        // one when their images are set again (filter()).
        image.shared = oidn::BufferRef();
        image.buffer = newBuffer(bytes);
        image.shared = oidnDevice.newBuffer(image.buffer);
        check("denoise: OIDN cannot share a Metal buffer");
    }
}

void MetalDenoiser::Impl::prefilter(oidn::FilterRef& guideFilter, const char* name, ImageSlot slot,
                                    unsigned width, unsigned height, DenoiseSettings const& settings)
{
    // In place: the buffer holds the denoiser's own copy of the guide.
    if (!guideFilter) {
        guideFilter = oidnDevice.newFilter("RT");
        check("denoise: cannot create OIDN's RT filter");
    }
    guideFilter.setImage(name, images[slot].shared, oidn::Format::Float3, width, height);
    guideFilter.setImage("output", images[slot].shared, oidn::Format::Float3, width, height);
    guideFilter.set("quality", OidnQuality(settings.quality));
    guideFilter.commit();
    check("denoise: cannot set up OIDN's prefilter");
    guideFilter.execute();
    check("denoise: OIDN's prefilter failed");
}

void MetalDenoiser::Impl::filter(unsigned width, unsigned height, DenoiseSettings const& settings, bool albedo,
                                 bool normal)
{
    const bool cleanAux = settings.prefilter && albedo;
    if (cleanAux) {
        prefilter(albedoFilter, "albedo", kAlbedo, width, height, settings);
        if (normal) prefilter(normalFilter, "normal", kNormal, width, height, settings);
    }
    if (!color) {
        color = oidnDevice.newFilter("RT");
        check("denoise: cannot create OIDN's RT filter");
    }
    color.setImage("color", images[kColor].shared, oidn::Format::Float3, width, height);
    if (albedo) color.setImage("albedo", images[kAlbedo].shared, oidn::Format::Float3, width, height);
    else color.unsetImage("albedo");
    if (normal) color.setImage("normal", images[kNormal].shared, oidn::Format::Float3, width, height);
    else color.unsetImage("normal");
    color.setImage("output", images[kOutput].shared, oidn::Format::Float3, width, height);
    color.set("hdr", settings.hdr);
    color.set("cleanAux", cleanAux);
    color.set("quality", OidnQuality(settings.quality));
    color.commit();
    check("denoise: cannot set up OIDN's filter");
    color.execute();
    check("denoise: OIDN failed");
}

void MetalDenoiser::Impl::destroy()
{
    color = oidn::FilterRef();
    albedoFilter = oidn::FilterRef();
    normalFilter = oidn::FilterRef();
    for (Image& image : images) {
        image.shared = oidn::BufferRef();
        image.buffer = nil;
    }
    scales = nil;
    oidnDevice = oidn::DeviceRef();
    resolve = nil;
    queue = nil;
    device = nil;
}

std::shared_ptr<MetalDenoiser> MetalDenoiser::Create(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                                     std::string* error)
{
    @autoreleasepool {
        auto fail = [&](std::string reason) -> std::shared_ptr<MetalDenoiser> {
            if (error) *error = std::move(reason);
            return nullptr;
        };
        if (!device || !queue) return fail("no Metal device");
        // False both for an OIDN built without its Metal device and for a
        // GPU OIDN does not support (it asks for Metal 3 on an Apple GPU).
        if (!oidn::isMetalDeviceSupported(device)) {
            const char* message = nullptr;
            oidn::getError(message);
            return fail(std::format("OIDN {} has no Metal device for {}{}{}", OIDN_VERSION_STRING,
                                    device.name.UTF8String, message ? ": " : "", message ? message : ""));
        }
        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->queue = queue;
        // OIDN records into command buffers of the render device's queue, so
        // its work is ordered with the renders.
        impl->oidnDevice = oidn::newMetalDevice(queue);
        if (!impl->oidnDevice) {
            const char* message = nullptr;
            oidn::getError(message);
            return fail(std::string("cannot create OIDN's Metal device: ") + (message ? message : "unknown error"));
        }
        impl->oidnDevice.commit();
        const char* message = nullptr;
        if (impl->oidnDevice.getError(message) != oidn::Error::None)
            return fail(std::string("cannot create OIDN's Metal device: ") + (message ? message : "unknown error"));

        NSError* nsError = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kResolveSource]
                                                      options:options
                                                        error:&nsError];
        if (!library) return fail("the denoiser's resolve kernel: " + NsErrorText(nsError));
        id<MTLFunction> function = [library newFunctionWithName:@"prism_denoise_resolve"];
        if (!function) return fail("the denoiser's resolve kernel has no entry point");
        impl->resolve = [device newComputePipelineStateWithFunction:function error:&nsError];
        if (!impl->resolve) return fail("the denoiser's resolve pipeline: " + NsErrorText(nsError));
        return std::shared_ptr<MetalDenoiser>(new MetalDenoiser(std::move(impl)));
    }
}

MetalDenoiser::MetalDenoiser(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

MetalDenoiser::~MetalDenoiser() { Shutdown(); }

void MetalDenoiser::Shutdown()
{
    std::lock_guard lock(m_impl->mutex);
    @autoreleasepool {
        m_impl->destroy();
    }
    m_impl->shutdown = true;
}

std::string MetalDenoiser::Name() const
{
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->shutdown) return "OIDN CPU (the Metal device was released)";
    return "OIDN " OIDN_VERSION_STRING " Metal";
}

void MetalDenoiser::Denoise(DenoiseRequest const& request)
{
    std::unique_lock lock(m_impl->mutex);
    if (m_impl->shutdown) {
        lock.unlock();
        auto cpu = SharedCpuDenoiser();
        if (!cpu) throw std::runtime_error("denoise: the Metal device was released and there is no CPU denoiser");
        cpu->Denoise(request);
        return;
    }
    Impl& impl = *m_impl;
    const size_t width = request.width, height = request.height;
    if (!width || !height || !request.color.pixels || !request.output || request.color.stride < 3 ||
        request.outputStride < 3 || (request.albedo.pixels && request.albedo.stride < 3) ||
        (request.normal.pixels && request.normal.stride < 3))
        throw std::runtime_error("denoise: empty or malformed images");
    // OIDN takes a normal only together with an albedo.
    const bool albedo = request.albedo.pixels != nullptr;
    const bool normal = albedo && request.normal.pixels != nullptr;
    const size_t count = width * height;
    @autoreleasepool {
        impl.reserve(count);
        const HostImage sources[3] = {request.color, albedo ? request.albedo : HostImage{},
                                      normal ? request.normal : HostImage{}};
        for (int k = 0; k < 3; ++k) {
            if (!sources[k].pixels) continue;
            auto* out = static_cast<float*>(impl.images[k].buffer.contents);
            for (size_t p = 0; p < count; ++p) {
                const float* in = sources[k].pixels + p * sources[k].stride;
                out[3 * p] = in[0];
                out[3 * p + 1] = in[1];
                out[3 * p + 2] = in[2];
            }
        }
        impl.filter(request.width, request.height, request.settings, albedo, normal);
        // request.output may alias request.color, which was copied above.
        auto const* denoised = static_cast<const float*>(impl.images[kOutput].buffer.contents);
        for (size_t p = 0; p < count; ++p)
            for (int c = 0; c < 3; ++c) request.output[p * request.outputStride + c] = denoised[3 * p + c];
    }
}

void MetalDenoiser::DenoiseAccumulation(id<MTLBuffer> accumulation, std::vector<int> const& samples,
                                        unsigned width, unsigned height, DenoiseSettings const& settings,
                                        std::vector<float>* output)
{
    std::lock_guard lock(m_impl->mutex);
    Impl& impl = *m_impl;
    if (impl.shutdown) throw std::runtime_error("denoise: the Metal device was released");
    const size_t count = size_t(width) * height;
    // Colour sums (32 bytes per pixel), then the light AOV sums (64).
    if (!count || samples.size() != count || !accumulation || accumulation.length < count * 96)
        throw std::runtime_error("denoise: the render has no accumulation to denoise");
    const auto start = std::chrono::steady_clock::now();
    @autoreleasepool {
        impl.reserve(count);
        if (!impl.scales || impl.scales.length < count * sizeof(float)) impl.scales = impl.newBuffer(count * sizeof(float));
        auto* scales = static_cast<float*>(impl.scales.contents);
        // The scale the host applies to the same sums (gpu_device.cpp).
        for (size_t p = 0; p < count; ++p) scales[p] = samples[p] > 0 ? 1.0f / float(samples[p]) : 0.0f;
        id<MTLCommandBuffer> commands = [impl.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:impl.resolve];
        [encoder setBuffer:accumulation offset:0 atIndex:0];
        [encoder setBuffer:impl.scales offset:0 atIndex:1];
        [encoder setBuffer:impl.images[kColor].buffer offset:0 atIndex:2];
        [encoder setBuffer:impl.images[kAlbedo].buffer offset:0 atIndex:3];
        [encoder setBuffer:impl.images[kNormal].buffer offset:0 atIndex:4];
        const uint32_t pixels = uint32_t(count);
        [encoder setBytes:&pixels length:sizeof(pixels) atIndex:5];
        const NSUInteger group = std::min<NSUInteger>(64, impl.resolve.maxTotalThreadsPerThreadgroup);
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        // OIDN's command buffers go to the same queue after this one; the
        // wait makes the order explicit and surfaces a failed resolve.
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted)
            throw std::runtime_error("denoise: the resolve kernel failed: " + NsErrorText(commands.error));
        const auto resolved = std::chrono::steady_clock::now();
        impl.filter(width, height, settings, true, true);
        output->resize(3 * count);
        std::memcpy(output->data(), impl.images[kOutput].buffer.contents, 3 * count * sizeof(float));
        if (gpu::GpuVerbose())
            std::cerr << std::format("PrismRT Metal: OIDN denoise {}x{}: resolve {:.1f} ms, filters {:.1f} ms\n",
                                     width, height,
                                     std::chrono::duration<double, std::milli>(resolved - start).count(),
                                     std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - resolved).count());
    }
}

} // namespace prism::device
