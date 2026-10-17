// HLSL resource access for the GPU backend: SM 6.6 descriptor heaps
// (bound by Metal shader converter at kIRDescriptorHeapBindPoint and
// kIRSamplerHeapBindPoint) and inline ray queries.

#ifndef PRISM_RESOURCES_HLSL
#define PRISM_RESOURCES_HLSL

#define PRISM_BUFFER(T, slot) StructuredBuffer<T> buffer = ResourceDescriptorHeap[slot]
#define PRISM_RW_BUFFER(T, slot) RWStructuredBuffer<T> buffer = ResourceDescriptorHeap[slot]

GpuFrame gpu_frame() { PRISM_BUFFER(GpuFrame, PRISM_SLOT_FRAME); return buffer[0]; }

float3 gpu_position(uint v)
{
    PRISM_BUFFER(float, PRISM_SLOT_POSITIONS);
    return float3(buffer[3u * v], buffer[3u * v + 1u], buffer[3u * v + 2u]);
}
float3 gpu_vertex_normal(uint v)
{
    PRISM_BUFFER(float, PRISM_SLOT_NORMALS);
    return float3(buffer[3u * v], buffer[3u * v + 1u], buffer[3u * v + 2u]);
}
float2 gpu_texcoord(uint v)
{
    PRISM_BUFFER(float, PRISM_SLOT_TEXCOORDS);
    return float2(buffer[2u * v], buffer[2u * v + 1u]);
}
float4 gpu_tangent(uint v)
{
    PRISM_BUFFER(float, PRISM_SLOT_TANGENTS);
    return float4(buffer[4u * v], buffer[4u * v + 1u], buffer[4u * v + 2u], buffer[4u * v + 3u]);
}
uint gpu_index(uint i) { PRISM_BUFFER(uint, PRISM_SLOT_INDICES); return buffer[i]; }
GpuPrototype gpu_prototype(uint i) { PRISM_BUFFER(GpuPrototype, PRISM_SLOT_PROTOTYPES); return buffer[i]; }
GpuInstance gpu_instance(uint i) { PRISM_BUFFER(GpuInstance, PRISM_SLOT_INSTANCES); return buffer[i]; }
uint gpu_primitive_material(uint i) { PRISM_BUFFER(uint, PRISM_SLOT_PRIMITIVE_MATERIALS); return buffer[i]; }
GpuMaterial gpu_material(uint i) { PRISM_BUFFER(GpuMaterial, PRISM_SLOT_MATERIALS); return buffer[i]; }
GpuLegacyTexture gpu_legacy_texture(uint i) { PRISM_BUFFER(GpuLegacyTexture, PRISM_SLOT_LEGACY_TEXTURES); return buffer[i]; }
float4 gpu_sphere(uint i) { PRISM_BUFFER(float4, PRISM_SLOT_SPHERES); return buffer[i]; }
GpuEnvironment gpu_environment() { PRISM_BUFFER(GpuEnvironment, PRISM_SLOT_ENVIRONMENT); return buffer[0]; }
float gpu_env_texel_pdf(uint i) { PRISM_BUFFER(float, PRISM_SLOT_ENV_TEXEL_PDF); return buffer[i]; }
float gpu_env_alias_probability(uint i) { PRISM_BUFFER(float, PRISM_SLOT_ENV_ALIAS_PROBABILITY); return buffer[i]; }
uint gpu_env_alias(uint i) { PRISM_BUFFER(uint, PRISM_SLOT_ENV_ALIAS); return buffer[i]; }
uint gpu_argblock_word(uint i) { PRISM_BUFFER(uint, PRISM_SLOT_ARGUMENT_BLOCKS); return buffer[i]; }
uint gpu_rodata_word(uint i) { PRISM_BUFFER(uint, PRISM_SLOT_RO_DATA); return buffer[i]; }
GpuMdlTexture gpu_mdl_texture(uint i) { PRISM_BUFFER(GpuMdlTexture, PRISM_SLOT_MDL_TEXTURES); return buffer[i]; }
uint gpu_mdl_texture_count() { return gpu_frame().counts.x; }
uint gpu_mdl_tile(uint i) { PRISM_BUFFER(uint, PRISM_SLOT_MDL_TILES); return buffer[i]; }
float gpu_bsdf_data(uint i) { PRISM_BUFFER(float, PRISM_SLOT_BSDF_DATA); return buffer[i]; }
GpuImage gpu_image(uint i) { PRISM_BUFFER(GpuImage, PRISM_SLOT_IMAGES); return buffer[i]; }
GpuTile gpu_tile(uint i) { PRISM_BUFFER(GpuTile, PRISM_SLOT_TILES); return buffer[i]; }

uint gpu_load_rng(uint pixel) { PRISM_RW_BUFFER(uint, PRISM_SLOT_RNG); return buffer[pixel]; }
void gpu_store_rng(uint pixel, uint value) { PRISM_RW_BUFFER(uint, PRISM_SLOT_RNG); buffer[pixel] = value; }
void gpu_store_output(uint index, float4 value) { PRISM_RW_BUFFER(float4, PRISM_SLOT_OUTPUT); buffer[index] = value; }
void gpu_accumulate(uint index, float3 value)
{
    PRISM_RW_BUFFER(float4, PRISM_SLOT_ACCUMULATION);
    buffer[index] = buffer[index] + float4(value, 0.0f);
}

Texture2D<float4> gpu_texture(uint image)
{
    return ResourceDescriptorHeap[NonUniformResourceIndex(PRISM_SLOT_IMAGE_BASE + image)];
}

// Top row first, mip level 0.
float4 gpu_image_texel(uint image, int x, int y) { return gpu_texture(image).Load(int3(x, y, 0)); }

// Mipmapped anisotropic lookup; image coordinates start at the top-left.
float4 gpu_image_sample(uint image, float2 st, float2 dx, float2 dy, uint wrapS, uint wrapT)
{
    SamplerState s = SamplerDescriptorHeap[wrapS * 3u + wrapT];
    return gpu_texture(image).SampleGrad(s, st, dx, dy);
}

#endif // PRISM_RESOURCES_HLSL
