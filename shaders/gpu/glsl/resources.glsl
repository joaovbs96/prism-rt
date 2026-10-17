// GLSL resource access for the GPU backend: one descriptor set with a
// storage buffer per slot, the top-level acceleration structure, and arrays
// of sampled images and samplers.

#ifndef PRISM_RESOURCES_GLSL
#define PRISM_RESOURCES_GLSL

#define PRISM_SSBO(name, T, slot) \
    layout(std430, set = 0, binding = slot) readonly buffer name##Block { T name[]; }
#define PRISM_RW_SSBO(name, T, slot) \
    layout(std430, set = 0, binding = slot) buffer name##Block { T name[]; }

PRISM_SSBO(g_frame, GpuFrame, PRISM_SLOT_FRAME);
PRISM_SSBO(g_positions, float, PRISM_SLOT_POSITIONS);
PRISM_SSBO(g_normals, float, PRISM_SLOT_NORMALS);
PRISM_SSBO(g_texcoords, float, PRISM_SLOT_TEXCOORDS);
PRISM_SSBO(g_tangents, float, PRISM_SLOT_TANGENTS);
PRISM_SSBO(g_indices, uint, PRISM_SLOT_INDICES);
PRISM_SSBO(g_prototypes, GpuPrototype, PRISM_SLOT_PROTOTYPES);
PRISM_SSBO(g_instances, GpuInstance, PRISM_SLOT_INSTANCES);
PRISM_SSBO(g_primitiveMaterials, uint, PRISM_SLOT_PRIMITIVE_MATERIALS);
PRISM_SSBO(g_materials, GpuMaterial, PRISM_SLOT_MATERIALS);
PRISM_SSBO(g_legacyTextures, GpuLegacyTexture, PRISM_SLOT_LEGACY_TEXTURES);
PRISM_SSBO(g_spheres, float4, PRISM_SLOT_SPHERES);
PRISM_SSBO(g_environment, GpuEnvironment, PRISM_SLOT_ENVIRONMENT);
PRISM_SSBO(g_envTexelPdf, float, PRISM_SLOT_ENV_TEXEL_PDF);
PRISM_SSBO(g_envAliasProbability, float, PRISM_SLOT_ENV_ALIAS_PROBABILITY);
PRISM_SSBO(g_envAlias, uint, PRISM_SLOT_ENV_ALIAS);
PRISM_SSBO(g_argumentBlocks, uint, PRISM_SLOT_ARGUMENT_BLOCKS);
PRISM_SSBO(g_roData, uint, PRISM_SLOT_RO_DATA);
PRISM_SSBO(g_mdlTextures, GpuMdlTexture, PRISM_SLOT_MDL_TEXTURES);
PRISM_SSBO(g_mdlTiles, uint, PRISM_SLOT_MDL_TILES);
PRISM_SSBO(g_bsdfData, float, PRISM_SLOT_BSDF_DATA);
PRISM_SSBO(g_images, GpuImage, PRISM_SLOT_IMAGES);
PRISM_SSBO(g_tiles, GpuTile, PRISM_SLOT_TILES);
PRISM_RW_SSBO(g_rng, uint, PRISM_SLOT_RNG);
PRISM_RW_SSBO(g_output, float4, PRISM_SLOT_OUTPUT);
PRISM_RW_SSBO(g_accumulation, float4, PRISM_SLOT_ACCUMULATION);
layout(set = 0, binding = PRISM_SLOT_ACCEL) uniform accelerationStructureEXT g_scene;
layout(set = 0, binding = PRISM_SLOT_IMAGE_BASE) uniform texture2D g_textures[];
layout(set = 0, binding = PRISM_SLOT_IMAGE_BASE + 1) uniform sampler g_samplers[PRISM_SAMPLER_COUNT];

GpuFrame gpu_frame() { return g_frame[0]; }
float3 gpu_position(uint v) { return float3(g_positions[3u * v], g_positions[3u * v + 1u], g_positions[3u * v + 2u]); }
float3 gpu_vertex_normal(uint v) { return float3(g_normals[3u * v], g_normals[3u * v + 1u], g_normals[3u * v + 2u]); }
float2 gpu_texcoord(uint v) { return float2(g_texcoords[2u * v], g_texcoords[2u * v + 1u]); }
float4 gpu_tangent(uint v)
{
    return float4(g_tangents[4u * v], g_tangents[4u * v + 1u], g_tangents[4u * v + 2u], g_tangents[4u * v + 3u]);
}
uint gpu_index(uint i) { return g_indices[i]; }
GpuPrototype gpu_prototype(uint i) { return g_prototypes[i]; }
GpuInstance gpu_instance(uint i) { return g_instances[i]; }
uint gpu_primitive_material(uint i) { return g_primitiveMaterials[i]; }
GpuMaterial gpu_material(uint i) { return g_materials[i]; }
GpuLegacyTexture gpu_legacy_texture(uint i) { return g_legacyTextures[i]; }
float4 gpu_sphere(uint i) { return g_spheres[i]; }
GpuEnvironment gpu_environment() { return g_environment[0]; }
float gpu_env_texel_pdf(uint i) { return g_envTexelPdf[i]; }
float gpu_env_alias_probability(uint i) { return g_envAliasProbability[i]; }
uint gpu_env_alias(uint i) { return g_envAlias[i]; }
uint gpu_argblock_word(uint i) { return g_argumentBlocks[i]; }
uint gpu_rodata_word(uint i) { return g_roData[i]; }
GpuMdlTexture gpu_mdl_texture(uint i) { return g_mdlTextures[i]; }
uint gpu_mdl_texture_count() { return g_frame[0].counts.x; }
uint gpu_mdl_tile(uint i) { return g_mdlTiles[i]; }
float gpu_bsdf_data(uint i) { return g_bsdfData[i]; }
GpuImage gpu_image(uint i) { return g_images[i]; }
GpuTile gpu_tile(uint i) { return g_tiles[i]; }

uint gpu_load_rng(uint pixel) { return g_rng[pixel]; }
void gpu_store_rng(uint pixel, uint value) { g_rng[pixel] = value; }
void gpu_store_output(uint index, float4 value) { g_output[index] = value; }
void gpu_accumulate(uint index, float3 value) { g_accumulation[index] += float4(value, 0.0f); }

// Top row first, mip level 0.
float4 gpu_image_texel(uint image, int x, int y)
{
    return texelFetch(sampler2D(g_textures[nonuniformEXT(image)], g_samplers[0]), ivec2(x, y), 0);
}

// Mipmapped anisotropic lookup; image coordinates start at the top-left.
float4 gpu_image_sample(uint image, float2 st, float2 dx, float2 dy, uint wrapS, uint wrapT)
{
    return textureGrad(sampler2D(g_textures[nonuniformEXT(image)], g_samplers[nonuniformEXT(wrapS * 3u + wrapT)]),
                       st, dx, dy);
}

#endif // PRISM_RESOURCES_GLSL
