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
GpuEnvironment gpu_environment() { return environmentBuffer()[0]; }
float gpu_env_texel_pdf(uint i) { return envTexelPdfBuffer()[i]; }
float gpu_env_alias_probability(uint i) { return envAliasProbabilityBuffer()[i]; }
uint gpu_env_alias(uint i) { return envAliasBuffer()[i]; }
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
void gpu_store_primary_hit(uint pixel, SurfaceHit hit)
{
    RWStructuredBuffer<float4> b = accumulationBuffer();
    b[3u * pixel] = float4(hit.p, hit.valid ? 1.0f : 0.0f);
    b[3u * pixel + 1u] = float4(hit.n, 0.0f);
    b[3u * pixel + 2u] = float4(hit.u, hit.v, asfloat(hit.instance), asfloat(hit.primitive));
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

// Top row first, mip level 0.
float4 gpu_image_texel(uint image, int x, int y) { return imageTexture(image).Load(int3(x, y, 0)); }

// Mipmapped anisotropic lookup; image coordinates start at the top-left.
// Samplers are indexed wrapS * 3 + wrapT (0 clamp, 1 repeat, 2 mirror).
float4 gpu_image_sample(uint image, float2 st, float2 dx, float2 dy, uint wrapS, uint wrapT)
{
    return imageTexture(image).SampleGrad(imageSampler(wrapS * 3u + wrapT), st, dx, dy);
}

float hypot(float x, float y) { return sqrt(x * x + y * y); }
