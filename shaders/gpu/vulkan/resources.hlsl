// Vulkan device resources: descriptor set 0, one binding per slot (see
// kSlot* in shaders/shared/prism_gpu_types.h).

#pragma once

[[vk::binding(kSlotFrame, 0)]] StructuredBuffer<GpuFrame> g_frame;
[[vk::binding(kSlotPositions, 0)]] StructuredBuffer<float> g_positions;
[[vk::binding(kSlotNormals, 0)]] StructuredBuffer<float> g_normals;
[[vk::binding(kSlotTexcoords, 0)]] StructuredBuffer<float> g_texcoords;
[[vk::binding(kSlotTangents, 0)]] StructuredBuffer<float> g_tangents;
[[vk::binding(kSlotIndices, 0)]] StructuredBuffer<uint> g_indices;
[[vk::binding(kSlotPrototypes, 0)]] StructuredBuffer<GpuPrototype> g_prototypes;
[[vk::binding(kSlotInstances, 0)]] StructuredBuffer<GpuInstance> g_instances;
[[vk::binding(kSlotPrimitiveMaterials, 0)]] StructuredBuffer<uint> g_primitiveMaterials;
[[vk::binding(kSlotMaterials, 0)]] StructuredBuffer<GpuMaterial> g_materials;
[[vk::binding(kSlotLegacyTextures, 0)]] StructuredBuffer<GpuLegacyTexture> g_legacyTextures;
[[vk::binding(kSlotSpheres, 0)]] StructuredBuffer<float4> g_spheres;
[[vk::binding(kSlotEnvironment, 0)]] StructuredBuffer<GpuEnvironment> g_environment;
[[vk::binding(kSlotEnvTexelPdf, 0)]] StructuredBuffer<float> g_envTexelPdf;
[[vk::binding(kSlotEnvAliasProbability, 0)]] StructuredBuffer<float> g_envAliasProbability;
[[vk::binding(kSlotEnvAlias, 0)]] StructuredBuffer<uint> g_envAlias;
[[vk::binding(kSlotLights, 0)]] StructuredBuffer<LightRecord> g_lights;
[[vk::binding(kSlotLightAlias, 0)]] StructuredBuffer<float4> g_lightAlias;
[[vk::binding(kSlotPrimvarValues, 0)]] StructuredBuffer<float> g_primvarValues;
[[vk::binding(kSlotPrimvarDirectory, 0)]] StructuredBuffer<GpuPrimvar> g_primvarDirectory;
[[vk::binding(kSlotArgumentBlocks, 0)]] StructuredBuffer<uint> g_argumentBlocks;
[[vk::binding(kSlotRoData, 0)]] StructuredBuffer<uint> g_roData;
[[vk::binding(kSlotMdlTextures, 0)]] StructuredBuffer<GpuMdlTexture> g_mdlTextures;
[[vk::binding(kSlotMdlTiles, 0)]] StructuredBuffer<uint> g_mdlTiles;
[[vk::binding(kSlotBsdfData, 0)]] StructuredBuffer<float> g_bsdfData;
[[vk::binding(kSlotImages, 0)]] StructuredBuffer<GpuImage> g_images;
[[vk::binding(kSlotPixels, 0)]] StructuredBuffer<uint> g_pixels;
[[vk::binding(kSlotRng, 0)]] RWStructuredBuffer<uint> g_rng;
[[vk::binding(kSlotOutput, 0)]] RWStructuredBuffer<float4> g_output;
[[vk::binding(kSlotAccumulation, 0)]] RWStructuredBuffer<float4> g_accumulation;
[[vk::binding(kSlotAccel, 0)]] RaytracingAccelerationStructure g_scene;
[[vk::binding(kSlotImageBase, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(kSlotImageBase + 1, 0)]] SamplerState g_samplers[kSamplerCount];

// Vulkan drivers implement normalize, sqrt and division approximately (the
// spec allows several ULP); NVIDIA's normalize of an interpolated constant
// normal varies by an ULP with the barycentrics, so geometric outputs of a
// cropped render differed from the full frame. One Newton step on the length
// and on the quotient makes it (nearly) correctly rounded, as on the CPU.
template<typename T> T prism_normalize(T v)
{
    const float d = dot(v, v);
    float len = sqrt(d);
    len += mad(-len, len, d) / (2.0f * len);
    T q = v / len;
    q += mad(-q, len, v) / len;
    return q;
}
#define normalize prism_normalize

StructuredBuffer<GpuFrame> frameBuffer() { return g_frame; }
StructuredBuffer<float> positionsBuffer() { return g_positions; }
StructuredBuffer<float> normalsBuffer() { return g_normals; }
StructuredBuffer<float> texcoordsBuffer() { return g_texcoords; }
StructuredBuffer<float> tangentsBuffer() { return g_tangents; }
StructuredBuffer<uint> indicesBuffer() { return g_indices; }
StructuredBuffer<GpuPrototype> prototypesBuffer() { return g_prototypes; }
StructuredBuffer<GpuInstance> instancesBuffer() { return g_instances; }
StructuredBuffer<uint> primitiveMaterialsBuffer() { return g_primitiveMaterials; }
StructuredBuffer<GpuMaterial> materialsBuffer() { return g_materials; }
StructuredBuffer<GpuLegacyTexture> legacyTexturesBuffer() { return g_legacyTextures; }
StructuredBuffer<float4> spheresBuffer() { return g_spheres; }
StructuredBuffer<GpuEnvironment> environmentBuffer() { return g_environment; }
StructuredBuffer<float> envTexelPdfBuffer() { return g_envTexelPdf; }
StructuredBuffer<float> envAliasProbabilityBuffer() { return g_envAliasProbability; }
StructuredBuffer<uint> envAliasBuffer() { return g_envAlias; }
StructuredBuffer<LightRecord> lightsBuffer() { return g_lights; }
StructuredBuffer<float4> lightAliasBuffer() { return g_lightAlias; }
StructuredBuffer<float> primvarValuesBuffer() { return g_primvarValues; }
StructuredBuffer<GpuPrimvar> primvarDirectoryBuffer() { return g_primvarDirectory; }
StructuredBuffer<uint> argumentBlocksBuffer() { return g_argumentBlocks; }
StructuredBuffer<uint> roDataBuffer() { return g_roData; }
StructuredBuffer<GpuMdlTexture> mdlTexturesBuffer() { return g_mdlTextures; }
StructuredBuffer<uint> mdlTilesBuffer() { return g_mdlTiles; }
StructuredBuffer<float> bsdfDataBuffer() { return g_bsdfData; }
StructuredBuffer<GpuImage> imagesBuffer() { return g_images; }
StructuredBuffer<uint> pixelsBuffer() { return g_pixels; }
RWStructuredBuffer<uint> rngBuffer() { return g_rng; }
RWStructuredBuffer<float4> outputBuffer() { return g_output; }
RWStructuredBuffer<float4> accumulationBuffer() { return g_accumulation; }
RaytracingAccelerationStructure sceneStructure() { return g_scene; }
Texture2D<float4> imageTexture(uint image) { return g_textures[NonUniformResourceIndex(image)]; }
SamplerState imageSampler(uint index) { return g_samplers[NonUniformResourceIndex(index)]; }

#ifdef PRISM_CALL_MATERIALS
// Material programs are the callable shaders of the ray tracing pipeline,
// indexed by program (vulkan_material_calls.cpp).
#define prism_call_material(program, call) CallShader(program, call)
#endif

#ifdef PRISM_REORDER_MATERIALS
// Shader execution reordering (SPV_NV_shader_invocation_reorder,
// OpReorderThreadWithHintNV): regroups the raygen shader's threads by the
// low `bits` bits of `hint`.
[[vk::ext_extension("SPV_NV_shader_invocation_reorder")]]
[[vk::ext_capability(5383)]] // ShaderInvocationReorderNV
[[vk::ext_instruction(5280)]] // OpReorderThreadWithHintNV
void prism_reorder_thread(uint hint, uint bits);
#endif
