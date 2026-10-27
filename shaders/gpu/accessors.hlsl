// Typed loads and stores of the GPU scene records, on the resource handles of
// the device (metal/resources.hlsl or vulkan/resources.hlsl).

#pragma once

GpuFrame gpu_frame() { return frameBuffer()[0]; }
float3 gpu_position(uint v)
{
    StructuredBuffer<float> b = positionsBuffer();
    return float3(b[3u * v], b[3u * v + 1u], b[3u * v + 2u]);
}
float3 gpu_vertex_normal(uint v)
{
    StructuredBuffer<float> b = normalsBuffer();
    return float3(b[3u * v], b[3u * v + 1u], b[3u * v + 2u]);
}
float2 gpu_texcoord(uint v)
{
    StructuredBuffer<float> b = texcoordsBuffer();
    return float2(b[2u * v], b[2u * v + 1u]);
}
float4 gpu_tangent(uint v)
{
    StructuredBuffer<float> b = tangentsBuffer();
    return float4(b[4u * v], b[4u * v + 1u], b[4u * v + 2u], b[4u * v + 3u]);
}
uint gpu_index(uint i) { return indicesBuffer()[i]; }
GpuPrototype gpu_prototype(uint i) { return prototypesBuffer()[i]; }
GpuInstance gpu_instance(uint i) { return instancesBuffer()[i]; }
uint gpu_primitive_material(uint i) { return primitiveMaterialsBuffer()[i]; }
GpuMaterial gpu_material(uint i) { return materialsBuffer()[i]; }
GpuLegacyTexture gpu_legacy_texture(uint i) { return legacyTexturesBuffer()[i]; }
float4 gpu_sphere(uint i) { return spheresBuffer()[i]; }
GpuEnvironment gpu_environment(uint i) { return environmentBuffer()[i]; }
uint gpu_environment_count() { return max(1u, gpu_frame().lights.z); }
float gpu_env_texel_pdf(uint i) { return envTexelPdfBuffer()[i]; }
float gpu_env_alias_probability(uint i) { return envAliasProbabilityBuffer()[i]; }
uint gpu_env_alias(uint i) { return envAliasBuffer()[i]; }
LightRecord gpu_light(uint i) { return lightsBuffer()[i]; }
float4 gpu_light_alias(uint i) { return lightAliasBuffer()[i]; }
float gpu_primvar_value(uint i) { return primvarValuesBuffer()[i]; }
GpuPrimvar gpu_primvar(uint i) { return primvarDirectoryBuffer()[i]; }
uint gpu_argblock_word(uint i) { return argumentBlocksBuffer()[i]; }
uint gpu_rodata_word(uint i) { return roDataBuffer()[i]; }
GpuMdlTexture gpu_mdl_texture(uint i) { return mdlTexturesBuffer()[i]; }
uint gpu_mdl_texture_count() { return gpu_frame().counts.x; }
uint gpu_mdl_tile(uint i) { return mdlTilesBuffer()[i]; }
float gpu_bsdf_data(uint i) { return bsdfDataBuffer()[i]; }
GpuImage gpu_image(uint i) { return imagesBuffer()[i]; }
uint gpu_pixel(uint i) { return pixelsBuffer()[i]; }

uint gpu_load_rng(uint pixel) { return rngBuffer()[pixel]; }
void gpu_store_rng(uint pixel, uint value)
{
    RWStructuredBuffer<uint> b = rngBuffer();
    b[pixel] = value;
}
// Preview pixel-center hits for geometric outputs, three entries per pixel
// in the accumulation buffer (unused by previews otherwise).
// Three planes of `count` entries, so each store of a SIMD group is
// contiguous: the buffer is host memory, written over the bus on discrete GPUs.
void gpu_store_primary_hit(uint pixel, uint count, SurfaceHit hit)
{
    RWStructuredBuffer<float4> b = accumulationBuffer();
    b[pixel] = float4(hit.p, hit.valid ? 1.0f : 0.0f);
    b[count + pixel] = float4(hit.n, 0.0f);
    b[2u * count + pixel] = float4(hit.u, hit.v, asfloat(hit.instance), asfloat(hit.primitive));
}

void gpu_store_output(uint index, float4 value)
{
    RWStructuredBuffer<float4> b = outputBuffer();
    b[index] = value;
}
// Two entries per pixel: the sum of all samples and of the even-numbered
// ones (the adaptive sampling error estimate).
void gpu_accumulate(uint pixel, float3 value, bool even)
{
    RWStructuredBuffer<float4> b = accumulationBuffer();
    b[2u * pixel] = b[2u * pixel] + float4(value, 0.0f);
    if (even) b[2u * pixel + 1u] = b[2u * pixel + 1u] + float4(value, 0.0f);
}

#if PRISM_LIGHT_AOVS
// Light AOVs (kFrameLightAovs): four entries per slot (albedo, diffuse,
// specular, emission; the denoising normal's x, y, z in the w of the first
// three) after `base` entries, in the output buffer (one sample) or added up
// in the accumulation buffer (image mode, after its 2 * pixels colour sums).
void gpu_store_aovs(uint base, uint slot, LightAovSample a)
{
    RWStructuredBuffer<float4> b = outputBuffer();
    b[base + 4u * slot] = float4(a.albedo, a.normal.x);
    b[base + 4u * slot + 1u] = float4(a.diffuse, a.normal.y);
    b[base + 4u * slot + 2u] = float4(a.specular, a.normal.z);
    b[base + 4u * slot + 3u] = float4(a.emission, 0.0f);
}
void gpu_accumulate_aovs(uint base, uint pixel, LightAovSample a)
{
    RWStructuredBuffer<float4> b = accumulationBuffer();
    const uint i = base + 4u * pixel;
    b[i] = b[i] + float4(a.albedo, a.normal.x);
    b[i + 1u] = b[i + 1u] + float4(a.diffuse, a.normal.y);
    b[i + 2u] = b[i + 2u] + float4(a.specular, a.normal.z);
    b[i + 3u] = b[i + 3u] + float4(a.emission, 0.0f);
}
#endif

#ifdef PRISM_TEXTURE_STREAMING
// Texture feedback of this thread's lookups (kFrameTextureFeedback; a
// subset of pixels): each lookup adds the path's weight to the histogram
// bin of the level it asks for, so the host can leave out levels only paths
// of negligible contribution ask for. The weight is the path throughput's
// largest component, clamped to 1, in 1/256 (gpu_backend: pathWeight). The
// callables get both from their caller (kMaterialCallFeedback).
static uint g_prism_feedback = 0u;
static uint g_prism_feedback_weight = 256u;
static const uint kFeedbackLevels = 16u; // histogram bins per image

void gpu_texture_request(uint image, uint level)
{
    if (g_prism_feedback == 0u || g_prism_feedback_weight == 0u) return;
    RWStructuredBuffer<uint> b = textureFeedbackBuffer();
    InterlockedAdd(b[image * kFeedbackLevels + min(level, kFeedbackLevels - 1u)], g_prism_feedback_weight);
}

// The finest level a 16x anisotropic trilinear lookup with these gradients
// (in normalized coordinates) reads: Vulkan's LOD rule, lambda =
// log2(major / N) with N = min(ceil(major / minor), 16) and the axes in
// level-0 texels, half a level finer for implementations' approximations.
void gpu_texture_feedback(uint image, float2 dx, float2 dy)
{
    const GpuImage img = gpu_image(image);
    const float2 size = float2(float(img.info.x), float(img.info.y));
    const float a = length(dx * size);
    const float b = length(dy * size);
    const float major = max(a, b);
    const float minor = min(a, b);
    const float n = minor > 0.0f ? min(ceil(major / minor), 16.0f) : 16.0f;
    const float lambda = log2(major / n) - float(gpu_frame().streaming.y) / 16.0f; // margin, 1/16 levels
    const uint level = lambda >= 15.0f ? 15u : (lambda >= 1.0f ? uint(lambda) : 0u);
    gpu_texture_request(image, level);
}
#endif

// Top row first, in level-0 texels (read from the finest resident level).
float4 gpu_image_texel(uint image, int x, int y)
{
#ifdef PRISM_TEXTURE_STREAMING
    const uint level = gpu_image(image).info.w;
    gpu_texture_request(image, 0u);
    return imageTexture(image).Load(int3(x >> level, y >> level, 0));
#else
    return imageTexture(image).Load(int3(x, y, 0));
#endif
}

// Mipmapped anisotropic lookup; image coordinates start at the top-left.
// Samplers are indexed wrapS * 3 + wrapT (0 clamp, 1 repeat, 2 mirror).
// A streamed image holds only its resident levels, so the lookup's level
// of detail is clamped to them without a clamp operand.
float4 gpu_image_sample(uint image, float2 st, float2 dx, float2 dy, uint wrapS, uint wrapT)
{
#ifdef PRISM_TEXTURE_STREAMING
    if (g_prism_feedback != 0u) gpu_texture_feedback(image, dx, dy);
#endif
    return imageTexture(image).SampleGrad(imageSampler(wrapS * 3u + wrapT), st, dx, dy);
}

float hypot(float x, float y) { return sqrt(x * x + y * y); }
