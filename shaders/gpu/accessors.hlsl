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
GpuTile gpu_tile(uint i) { return tilesBuffer()[i]; }

uint gpu_load_rng(uint pixel) { return rngBuffer()[pixel]; }
void gpu_store_rng(uint pixel, uint value)
{
    RWStructuredBuffer<uint> b = rngBuffer();
    b[pixel] = value;
}
void gpu_store_output(uint index, float4 value)
{
    RWStructuredBuffer<float4> b = outputBuffer();
    b[index] = value;
}
void gpu_accumulate(uint index, float3 value)
{
    RWStructuredBuffer<float4> b = accumulationBuffer();
    b[index] = b[index] + float4(value, 0.0f);
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
