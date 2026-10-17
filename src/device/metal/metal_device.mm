// Metal render device. The kernel is shaders/gpu/kernel.hlsl with the
// scene's MDL code, compiled DXC (HLSL -> DXIL) -> Metal shader converter
// (DXIL -> metallib) and cached by source hash. Resources are bound through
// Metal shader converter's descriptor heaps; the host mirrors the scene with
// one primitive acceleration structure per prototype and one instance
// acceleration structure.

#define IR_PRIVATE_IMPLEMENTATION
#import <Metal/Metal.h>
#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include "metal_device.h"

#include "../gpu/gpu_device.h"
#include "../gpu/gpu_materials.h"
#include "../gpu/gpu_scene.h"
#include "../gpu/hlsl_compiler.h"
#include "../gpu/hlsl_fp32_rewrite.h"
#include "../gpu/shader_sources.h"
#include "../../scene.h"
#include "../../system_utils.h"
#include "../../texture_service.h"

#include <tbb/parallel_for.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace prism::device {

using prism::device::gpu::GpuSceneData;

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool Verbose()
{
    const char* value = std::getenv("PRISM_GPU_VERBOSE");
    return value && *value && std::string_view(value) != "0";
}

bool ConvertToMetallib(std::vector<char> const& dxil, std::vector<char>* metallib, std::string* error)
{
    IRCompiler* compiler = IRCompilerCreate();
    IRCompilerSetEntryPointName(compiler, "main");
    IRObject* input = IRObjectCreateFromDXIL(reinterpret_cast<const uint8_t*>(dxil.data()), dxil.size(),
                                             IRBytecodeOwnershipNone);
    IRError* irError = nullptr;
    IRObject* output = IRCompilerAllocCompileAndLink(compiler, nullptr, input, &irError);
    bool ok = output != nullptr;
    if (!ok && error) {
        const char* payload = irError ? static_cast<const char*>(IRErrorGetPayload(irError)) : nullptr;
        *error = std::format("Metal shader converter failed ({}): {}",
                             irError ? IRErrorGetCode(irError) : 0u, payload ? payload : "no details");
    }
    if (irError) IRErrorDestroy(irError);
    if (ok) {
        IRMetalLibBinary* binary = IRMetalLibBinaryCreate();
        ok = IRObjectGetMetalLibBinary(output, IRShaderStageCompute, binary);
        if (ok) {
            metallib->resize(IRMetalLibGetBytecodeSize(binary));
            IRMetalLibGetBytecode(binary, reinterpret_cast<uint8_t*>(metallib->data()));
        } else if (error) {
            *error = "Metal shader converter produced no compute library";
        }
        IRMetalLibBinaryDestroy(binary);
        IRObjectDestroy(output);
    }
    IRObjectDestroy(input);
    IRCompilerDestroy(compiler);
    return ok;
}

bool ReadFile(std::filesystem::path const& path, std::vector<char>* bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

void WriteFileAtomically(std::filesystem::path const& path, const void* data, size_t size)
{
    const auto staging = path.string() + std::format(".{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream file(staging, std::ios::binary);
        file.write(static_cast<const char*>(data), std::streamsize(size));
    }
    std::error_code ignored;
    std::filesystem::rename(staging, path, ignored);
}

} // namespace

struct MetalDevice::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
        std::atomic<bool> dirty{true};
    SceneManager const* boundScene = nullptr;

    // Scene resources.
    std::unique_ptr<GpuSceneData> data;
    uint32_t mdlTextureCount = 1;
    id<MTLComputePipelineState> pipeline = nil;
    std::unordered_map<std::string, id<MTLComputePipelineState>> pipelines;
    id<MTLBuffer> buffers[kSlotCount] = {};
    std::vector<id<MTLTexture>> images;
    // Uploaded images by prepared resource, reused across scene rebuilds.
    struct CachedImage {
        std::weak_ptr<const prism::TextureResource> resource;
        id<MTLTexture> texture = nil;
        bool floatPrecision = false;
    };
    std::unordered_map<const prism::TextureResource*, CachedImage> imageCache;
    std::vector<id<MTLAccelerationStructure>> primitiveStructures;
    id<MTLAccelerationStructure> instanceStructure = nil;
    id<MTLBuffer> accelerationHeader = nil;
    id<MTLBuffer> resourceHeap = nil;
    id<MTLBuffer> samplerHeap = nil;
    std::vector<id<MTLSamplerState>> samplers;
    NSMutableArray<id<MTLResource>>* readResources = nil;
    double lastBuildSeconds = 0.0;

    bool ensureScene(SceneManager& scene, std::string* error);
    bool buildPipeline(std::string* error);
    void uploadBuffers();
    void uploadImages();
    bool buildAccelerationStructures(std::string* error);
    void writeHeap();
    id<MTLBuffer> buffer(const void* bytes, size_t size);
    void setBuffer(int slot, id<MTLBuffer> buffer);
    // Resizes a per-render buffer slot to at least `size` bytes.
    id<MTLBuffer> renderBuffer(int slot, size_t size);
    bool dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error);
};

id<MTLBuffer> MetalDevice::Impl::buffer(const void* bytes, size_t size)
{
    const size_t length = std::max<size_t>(16, (size + 15) & ~size_t(15));
    id<MTLBuffer> result = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    if (bytes && size) std::memcpy(result.contents, bytes, size);
    return result;
}

void MetalDevice::Impl::setBuffer(int slot, id<MTLBuffer> value) { buffers[slot] = value; }

id<MTLBuffer> MetalDevice::Impl::renderBuffer(int slot, size_t size)
{
    if (!buffers[slot] || buffers[slot].length < size) {
        buffers[slot] = buffer(nullptr, size);
        writeHeap();
    }
    return buffers[slot];
}

template <class T>
static size_t Bytes(std::vector<T> const& v) { return v.size() * sizeof(T); }

void MetalDevice::Impl::uploadBuffers()
{
    GpuSceneData const& d = *data;
    setBuffer(kSlotPositions, buffer(d.positions.data(), Bytes(d.positions)));
    setBuffer(kSlotNormals, buffer(d.normals.data(), Bytes(d.normals)));
    setBuffer(kSlotTexcoords, buffer(d.texcoords.data(), Bytes(d.texcoords)));
    setBuffer(kSlotTangents, buffer(d.tangents.data(), Bytes(d.tangents)));
    setBuffer(kSlotIndices, buffer(d.indices.data(), Bytes(d.indices)));
    setBuffer(kSlotPrototypes, buffer(d.prototypes.data(), Bytes(d.prototypes)));
    setBuffer(kSlotInstances, buffer(d.instances.data(), Bytes(d.instances)));
    setBuffer(kSlotPrimitiveMaterials, buffer(d.primitiveMaterials.data(), Bytes(d.primitiveMaterials)));
    setBuffer(kSlotMaterials, buffer(d.materials.data(), Bytes(d.materials)));
    setBuffer(kSlotLegacyTextures, buffer(d.legacyTextures.data(), Bytes(d.legacyTextures)));
    setBuffer(kSlotSpheres, buffer(d.spheres.data(), Bytes(d.spheres)));
    setBuffer(kSlotEnvironment, buffer(&d.environment, sizeof(d.environment)));
    setBuffer(kSlotEnvTexelPdf, buffer(d.environmentTexelPdf.data(), Bytes(d.environmentTexelPdf)));
    setBuffer(kSlotEnvAliasProbability,
              buffer(d.environmentAliasProbability.data(), Bytes(d.environmentAliasProbability)));
    setBuffer(kSlotEnvAlias, buffer(d.environmentAlias.data(), Bytes(d.environmentAlias)));
    setBuffer(kSlotMdlTextures, buffer(d.mdlTextures.data(), Bytes(d.mdlTextures)));
    setBuffer(kSlotMdlTiles, buffer(d.mdlTiles.data(), Bytes(d.mdlTiles)));
    setBuffer(kSlotBsdfData, buffer(d.bsdfData.data(), Bytes(d.bsdfData)));
    if (!buffers[kSlotFrame]) setBuffer(kSlotFrame, buffer(nullptr, sizeof(GpuFrame)));
    for (int slot : {kSlotTiles, kSlotRng, kSlotOutput, kSlotAccumulation})
        if (!buffers[slot]) setBuffer(slot, buffer(nullptr, 16));
}

// Uploads (or reuses) one image: values that are exactly k/255 (8-bit
// data) are stored losslessly in 8 bits, other material images as half
// floats and the environment as floats. Three channels are padded with a
// zero alpha, matching lookups of absent channels.
static id<MTLTexture> UploadImage(id<MTLDevice> device, prism::TextureService& service,
                                  prism::TextureResource const& resource, bool floatPrecision)
{
    const int width = resource.width(), height = resource.height();
    const int channels = std::clamp(resource.channels(), 1, 4);
    std::vector<float> pixels(size_t(width) * height * channels);
    if (!service.readPixels(resource, 0, 0, width, height, 0, channels, pixels.data()))
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
    MTLPixelFormat format;
    int componentBytes;
    if (quantized) {
        format = stored == 1 ? MTLPixelFormatR8Unorm : stored == 2 ? MTLPixelFormatRG8Unorm : MTLPixelFormatRGBA8Unorm;
        componentBytes = 1;
    } else if (!floatPrecision && peak < 60000.0f) {
        format = stored == 1 ? MTLPixelFormatR16Float : stored == 2 ? MTLPixelFormatRG16Float : MTLPixelFormatRGBA16Float;
        componentBytes = 2;
    } else {
        format = stored == 1 ? MTLPixelFormatR32Float : stored == 2 ? MTLPixelFormatRG32Float : MTLPixelFormatRGBA32Float;
        componentBytes = 4;
    }
    const int levels = 1 + int(std::floor(std::log2(double(std::max(width, height)))));
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:NSUInteger(width) height:NSUInteger(height) mipmapped:YES];
    descriptor.mipmapLevelCount = NSUInteger(levels);
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    std::vector<uint8_t> staging(size_t(width) * height * stored * componentBytes);
    for (size_t p = 0; p < size_t(width) * height; ++p) {
        for (int c = 0; c < stored; ++c) {
            const float v = c < channels ? pixels[p * channels + c] : 0.0f;
            uint8_t* out = staging.data() + (p * stored + c) * componentBytes;
            if (componentBytes == 1) *out = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            else if (componentBytes == 2) { _Float16 h = _Float16(v); std::memcpy(out, &h, 2); }
            else std::memcpy(out, &v, 4);
        }
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height)) mipmapLevel:0
                 withBytes:staging.data() bytesPerRow:NSUInteger(width * stored * componentBytes)];
    return texture;
}

void MetalDevice::Impl::uploadImages()
{
    GpuSceneData& d = *data;
    images.assign(d.images.size(), nil);
    std::vector<GpuImage> info(d.images.size());
    std::vector<bool> floatPrecision(d.images.size(), false);
    if (d.environment.info.y != kImageNone && d.environment.info.y < d.images.size())
        floatPrecision[d.environment.info.y] = true;
    std::vector<size_t> missing;
    for (size_t i = 0; i < d.images.size(); ++i) {
        auto const& resource = d.images[i];
        info[i].info = {uint32_t(resource->width()), uint32_t(resource->height()),
                        uint32_t(resource->channels()), uint32_t(i)};
        auto found = imageCache.find(resource.get());
        if (found != imageCache.end() && !found->second.resource.expired() &&
            found->second.floatPrecision == floatPrecision[i])
            images[i] = found->second.texture;
        else
            missing.push_back(i);
    }
    auto service = prism::TextureService::Acquire();
    tbb::parallel_for(size_t(0), missing.size(), [&](size_t m) {
        @autoreleasepool {
            const size_t i = missing[m];
            images[i] = UploadImage(device, *service, *d.images[i], floatPrecision[i]);
        }
    });
    if (!missing.empty()) {
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        for (size_t i : missing)
            if (images[i].mipmapLevelCount > 1) [blit generateMipmapsForTexture:images[i]];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
    }
    // Drop images no longer referenced by any live resource.
    for (auto it = imageCache.begin(); it != imageCache.end();)
        it = it->second.resource.expired() ? imageCache.erase(it) : std::next(it);
    for (size_t i : missing)
        imageCache[d.images[i].get()] = {d.images[i], images[i], floatPrecision[i]};
    setBuffer(kSlotImages, buffer(info.data(), Bytes(info)));
    if (Verbose()) {
        size_t bytes = 0;
        for (id<MTLTexture> t : images) bytes += t.allocatedSize;
        std::cerr << std::format("PrismRT Metal: {} images ({} uploaded), {:.0f} MiB\n", images.size(),
                                 missing.size(), bytes / 1048576.0);
    }
}

static id<MTLAccelerationStructure> BuildStructure(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                                   MTLAccelerationStructureDescriptor* descriptor)
{
    MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
    id<MTLAccelerationStructure> structure = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    id<MTLBuffer> scratch = [device newBufferWithLength:std::max<NSUInteger>(sizes.buildScratchBufferSize, 16)
                                                options:MTLResourceStorageModePrivate];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> encoder = [commands accelerationStructureCommandEncoder];
    [encoder buildAccelerationStructure:structure descriptor:descriptor scratchBuffer:scratch scratchBufferOffset:0];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    return commands.status == MTLCommandBufferStatusCompleted ? structure : nil;
}

bool MetalDevice::Impl::buildAccelerationStructures(std::string* error)
{
    GpuSceneData const& d = *data;
    primitiveStructures.clear();
    std::vector<uint32_t> structureOfPrototype(d.prototypes.size(), ~0u);
    std::vector<id<MTLBuffer>> boxes;
    std::vector<MTLPrimitiveAccelerationStructureDescriptor*> descriptors;
    for (size_t p = 0; p < d.prototypes.size(); ++p) {
        GpuPrototype const& proto = d.prototypes[p];
        if (proto.info.w == 0) continue;
        MTLAccelerationStructureGeometryDescriptor* geometry = nil;
        if (proto.info.x == kProtoSpheres) {
            std::vector<MTLAxisAlignedBoundingBox> bounds(proto.info.w);
            for (uint32_t s = 0; s < proto.info.w; ++s) {
                float4 const& sphere = d.spheres[proto.info.z + s];
                const float r = std::abs(sphere.w);
                bounds[s].min = {sphere.x - r, sphere.y - r, sphere.z - r};
                bounds[s].max = {sphere.x + r, sphere.y + r, sphere.z + r};
            }
            id<MTLBuffer> boxBuffer = buffer(bounds.data(), Bytes(bounds));
            boxes.push_back(boxBuffer);
            auto* box = [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
            box.boundingBoxBuffer = boxBuffer;
            box.boundingBoxCount = proto.info.w;
            box.boundingBoxStride = sizeof(MTLAxisAlignedBoundingBox);
            box.opaque = NO;
            geometry = box;
        } else {
            auto* triangles = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            triangles.vertexBuffer = buffers[kSlotPositions];
            triangles.vertexBufferOffset = NSUInteger(proto.info.y) * 12;
            triangles.vertexStride = 12;
            triangles.vertexFormat = MTLAttributeFormatFloat3;
            triangles.indexBuffer = buffers[kSlotIndices];
            triangles.indexBufferOffset = NSUInteger(proto.info.z) * 12;
            triangles.indexType = MTLIndexTypeUInt32;
            triangles.triangleCount = proto.info.x == kProtoQuads ? 2 * proto.info.w : proto.info.w;
            triangles.opaque = YES;
            geometry = triangles;
        }
        auto* descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        descriptor.geometryDescriptors = @[geometry];
        structureOfPrototype[p] = uint32_t(descriptors.size());
        descriptors.push_back(descriptor);
    }
    // All primitive structures build in one command buffer, sharing one
    // scratch allocation (scenes can hold thousands of small prototypes).
    std::vector<NSUInteger> scratchOffsets(descriptors.size());
    NSUInteger scratchSize = 0;
    for (size_t i = 0; i < descriptors.size(); ++i) {
        MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:descriptors[i]];
        primitiveStructures.push_back([device newAccelerationStructureWithSize:sizes.accelerationStructureSize]);
        scratchOffsets[i] = scratchSize;
        scratchSize += (sizes.buildScratchBufferSize + 255) & ~NSUInteger(255);
    }
    if (!descriptors.empty()) {
        id<MTLBuffer> scratch = [device newBufferWithLength:std::max<NSUInteger>(scratchSize, 256)
                                                    options:MTLResourceStorageModePrivate];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLAccelerationStructureCommandEncoder> encoder = [commands accelerationStructureCommandEncoder];
        for (size_t i = 0; i < descriptors.size(); ++i)
            [encoder buildAccelerationStructure:primitiveStructures[i] descriptor:descriptors[i]
                                  scratchBuffer:scratch scratchBufferOffset:scratchOffsets[i]];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) {
            if (error) *error = "primitive acceleration structure build failed";
            return false;
        }
    }

    const size_t count = d.instances.size();
    id<MTLBuffer> instanceBuffer = buffer(nullptr, std::max<size_t>(1, count) * sizeof(MTLAccelerationStructureInstanceDescriptor));
    auto* instances = static_cast<MTLAccelerationStructureInstanceDescriptor*>(instanceBuffer.contents);
    for (size_t i = 0; i < count; ++i) {
        auto& instance = instances[i];
        std::memset(&instance, 0, sizeof(instance));
        auto const& m = d.instanceTransforms[i]; // row-major 3x4
        for (int c = 0; c < 4; ++c)
            instance.transformationMatrix.columns[c] = MTLPackedFloat3Make(m[c], m[4 + c], m[8 + c]);
        instance.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
        instance.mask = 0xFF;
        instance.accelerationStructureIndex = structureOfPrototype[d.instances[i].info.x];
    }
    NSMutableArray<id<MTLAccelerationStructure>>* structures = [NSMutableArray array];
    for (id<MTLAccelerationStructure> s : primitiveStructures) [structures addObject:s];
    auto* descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
    descriptor.instancedAccelerationStructures = structures;
    descriptor.instanceCount = count;
    descriptor.instanceDescriptorBuffer = instanceBuffer;
    instanceStructure = BuildStructure(device, queue, descriptor);
    if (!instanceStructure) {
        if (error) *error = "instance acceleration structure build failed";
        return false;
    }
    const size_t headerSize = sizeof(IRRaytracingAccelerationStructureGPUHeader);
    accelerationHeader = buffer(nullptr, headerSize + sizeof(uint32_t) * std::max<size_t>(1, count));
    std::vector<uint32_t> contributions(std::max<size_t>(1, count), 0);
    auto* header = static_cast<uint8_t*>(accelerationHeader.contents);
    IRRaytracingSetAccelerationStructure(header, instanceStructure.gpuResourceID, header + headerSize,
                                         accelerationHeader.gpuAddress + headerSize, contributions.data(),
                                         contributions.size());
    return true;
}

void MetalDevice::Impl::writeHeap()
{
    const size_t entries = kSlotImageBase + std::max<size_t>(1, images.size());
    if (!resourceHeap || resourceHeap.length < entries * sizeof(IRDescriptorTableEntry))
        resourceHeap = buffer(nullptr, entries * sizeof(IRDescriptorTableEntry));
    auto* heap = static_cast<IRDescriptorTableEntry*>(resourceHeap.contents);
    std::memset(heap, 0, entries * sizeof(IRDescriptorTableEntry));
    for (int slot = 0; slot < kSlotCount; ++slot) {
        if (slot == kSlotAccel || !buffers[slot]) continue;
        IRDescriptorTableSetBuffer(&heap[slot], buffers[slot].gpuAddress, buffers[slot].length);
    }
    if (accelerationHeader) IRDescriptorTableSetAccelerationStructure(&heap[kSlotAccel], accelerationHeader.gpuAddress);
    for (size_t i = 0; i < images.size(); ++i)
        IRDescriptorTableSetTexture(&heap[kSlotImageBase + i], images[i], 0.0f, 0);

    readResources = [NSMutableArray array];
    for (int slot = 0; slot < kSlotCount; ++slot)
        if (buffers[slot]) [readResources addObject:buffers[slot]];
    for (id<MTLTexture> t : images) [readResources addObject:t];
    for (id<MTLAccelerationStructure> s : primitiveStructures) [readResources addObject:s];
    if (instanceStructure) [readResources addObject:instanceStructure];
    if (accelerationHeader) [readResources addObject:accelerationHeader];
}

bool MetalDevice::Impl::buildPipeline(std::string* error)
{
    auto const start = Clock::now();
    gpu::MdlShaderCode code = gpu::GenerateMdlShaderCode(data->mdlMaterials);
    if (!code.success)
        std::cerr << "PrismRT Metal: MDL materials unavailable on the GPU (" << code.error << ")\n";
    for (size_t i = 0; i < code.materials.size(); ++i)
        if (code.success && !code.materials[i].valid)
            std::cerr << "PrismRT Metal: MDL material rendered as invalid: " << code.materials[i].error << '\n';
    gpu::ApplyMdlShaderCode(*data, code);
    mdlTextureCount = uint32_t(data->mdlTextures.size());
    setBuffer(kSlotArgumentBlocks, buffer(code.argumentBlocks.data(), code.argumentBlocks.size()));
    setBuffer(kSlotRoData, buffer(code.roData.data(), code.roData.size()));

    std::string mdlCode;
    if (!gpu::RewriteHlslToFp32(code.code, &mdlCode, error)) return false;
    std::string source;
    if (!gpu::AssembleShader("gpu/kernel.hlsl", {"gpu/metal"}, mdlCode, &source, error)) return false;
    const std::string key = gpu::SourceDigest(source);
    if (auto found = pipelines.find(key); found != pipelines.end()) {
        pipeline = found->second;
        return true;
    }
    const auto cacheDirectory = gpu::ShaderCacheDirectory("metal");
    const auto cached = cacheDirectory / (key + ".metallib");
    std::vector<char> metallib;
    if (!ReadFile(cached, &metallib)) {
        std::cerr << "PrismRT Metal: compiling the GPU kernel (" << code.programCount
                  << " MDL programs); this happens once per material set\n";
        auto const dxcStart = Clock::now();
        std::vector<char> dxil;
        if (!gpu::CompileHlsl(source, gpu::HlslTarget::Dxil, cacheDirectory, &dxil, error)) return false;
        const double dxcSeconds = SecondsSince(dxcStart);
        auto const convertStart = Clock::now();
        if (!ConvertToMetallib(dxil, &metallib, error)) return false;
        WriteFileAtomically(cached, metallib.data(), metallib.size());
        std::cerr << std::format("PrismRT Metal: DXC {:.1f} s, shader converter {:.1f} s\n", dxcSeconds,
                                 SecondsSince(convertStart));
    }
    NSError* nsError = nil;
    dispatch_data_t bytes = dispatch_data_create(metallib.data(), metallib.size(), nullptr,
                                                 DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [device newLibraryWithData:bytes error:&nsError];
    if (!library) {
        if (error) *error = std::string("Metal library: ") + nsError.localizedDescription.UTF8String;
        return false;
    }
    auto const pipelineStart = Clock::now();
    id<MTLFunction> function = [library newFunctionWithName:library.functionNames.firstObject];
    pipeline = [device newComputePipelineStateWithFunction:function error:&nsError];
    if (!pipeline) {
        if (error) *error = std::string("Metal pipeline: ") + nsError.localizedDescription.UTF8String;
        return false;
    }
    pipelines[key] = pipeline;
    if (Verbose() || SecondsSince(pipelineStart) > 1.0)
        std::cerr << std::format("PrismRT Metal: pipeline {:.1f} s, total shader setup {:.1f} s\n",
                                 SecondsSince(pipelineStart), SecondsSince(start));
    return true;
}

bool MetalDevice::Impl::ensureScene(SceneManager& scene, std::string* error)
{
    if (boundScene == &scene && !dirty.load() && pipeline) return true;
    auto const start = Clock::now();
    dirty = false;
    boundScene = &scene;
    data = std::make_unique<GpuSceneData>(gpu::BuildGpuScene(scene));
    for (auto const& warning : data->warnings) std::cerr << "PrismRT Metal: " << warning << '\n';
    // The MDL link step patches materials and adds MDL textures and images,
    // so it runs before the upload.
    if (!buildPipeline(error)) { pipeline = nil; boundScene = nullptr; return false; }
    uploadBuffers();
    uploadImages();
    if (!buildAccelerationStructures(error)) { pipeline = nil; boundScene = nullptr; return false; }
    writeHeap();
    lastBuildSeconds = SecondsSince(start);
    if (Verbose())
        std::cerr << std::format("PrismRT Metal: scene ready in {:.2f} s ({} instances, {} triangles, {} spheres)\n",
                                 lastBuildSeconds, data->instances.size(), data->triangleCount(), data->spheres.size());
    return true;
}

bool MetalDevice::Impl::dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error)
{
    if (threads == 0) return true;
    std::memcpy(buffers[kSlotFrame].contents, &frame, sizeof(frame));
    @autoreleasepool {
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:resourceHeap offset:0 atIndex:kIRDescriptorHeapBindPoint];
        [encoder setBuffer:samplerHeap offset:0 atIndex:kIRSamplerHeapBindPoint];
        // Everything is reached through the descriptor heaps, so it must be
        // made resident explicitly.
        for (id<MTLResource> resource in readResources)
            [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) {
            if (error) *error = std::string("Metal command buffer failed: ") +
                (commands.error ? commands.error.localizedDescription.UTF8String : "unknown error");
            return false;
        }
        if (gpuSeconds) *gpuSeconds += commands.GPUEndTime - commands.GPUStartTime;
    }
    return true;
}

std::unique_ptr<MetalDevice> MetalDevice::Create(std::string* reason)
{
    if (const char* disabled = std::getenv("PRISM_DISABLE_METAL"); disabled && *disabled && *disabled != '0') {
        if (reason) *reason = "disabled by PRISM_DISABLE_METAL";
        return nullptr;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        if (reason) *reason = "no Metal GPU";
        return nullptr;
    }
    if (!device.supportsRaytracing) {
        if (reason) *reason = std::string(device.name.UTF8String) + " has no ray tracing support";
        return nullptr;
    }
    std::string source;
    if (!gpu::LoadShaderSource("gpu/kernel.hlsl", &source)) {
        if (reason) *reason = "the Metal kernel sources are missing";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->queue = [device newCommandQueue];
    // Samplers: index = wrapS * 3 + wrapT, wraps clamp / repeat / mirror.
    const MTLSamplerAddressMode modes[3] = {MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
                                            MTLSamplerAddressModeMirrorRepeat};
    impl->samplerHeap = impl->buffer(nullptr, kSamplerCount * sizeof(IRDescriptorTableEntry));
    auto* samplerTable = static_cast<IRDescriptorTableEntry*>(impl->samplerHeap.contents);
    for (int s = 0; s < 3; ++s) for (int t = 0; t < 3; ++t) {
        MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
        descriptor.minFilter = MTLSamplerMinMagFilterLinear;
        descriptor.magFilter = MTLSamplerMinMagFilterLinear;
        descriptor.mipFilter = MTLSamplerMipFilterLinear;
        descriptor.maxAnisotropy = 16;
        descriptor.sAddressMode = modes[s];
        descriptor.tAddressMode = modes[t];
        descriptor.supportArgumentBuffers = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
        impl->samplers.push_back(sampler);
        IRDescriptorTableSetSampler(&samplerTable[s * 3 + t], sampler, 0.0f);
    }
    return std::unique_ptr<MetalDevice>(new MetalDevice(std::move(impl)));
}

MetalDevice::MetalDevice(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
MetalDevice::~MetalDevice() = default;

DeviceInfo MetalDevice::Info() const
{
    DeviceInfo info;
    info.kind = DeviceKind::Metal;
    info.name = std::string(m_impl->device.name.UTF8String) + " (Metal)";
    // Apple9 GPUs (M3, A17 and newer) have ray-tracing hardware; earlier
    // ones run the same Metal ray-tracing API in software.
    info.hardwareRayTracing = [m_impl->device supportsFamily:MTLGPUFamilyApple9];
    return info;
}

void MetalDevice::SceneChanged(SceneManager&) { m_impl->dirty = true; }

bool MetalDevice::PrepareScene(SceneManager& scene, std::string* error)
{
    return m_impl->ensureScene(scene, error);
}

GpuSceneData const& MetalDevice::Data() const { return *m_impl->data; }
uint32_t MetalDevice::MdlTextureCount() const { return m_impl->mdlTextureCount; }
void* MetalDevice::RenderBuffer(int slot, size_t bytes) { return m_impl->renderBuffer(slot, bytes).contents; }

bool MetalDevice::Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds, std::string* error)
{
    return m_impl->dispatch(frame, threads, gpuSeconds, error);
}

} // namespace prism::device
