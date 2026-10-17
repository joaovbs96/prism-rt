// Records shared by the GPU host code (C++) and the GPU kernels (GLSL, HLSL).
// Every record is built from 16-byte float4/uint4 members only, so C++,
// std430 (GLSL) and structured-buffer (HLSL) layouts agree without padding
// rules. Scalar data lives in separate float/uint buffers.

#ifndef PRISM_GPU_TYPES_H
#define PRISM_GPU_TYPES_H

#if defined(__cplusplus)
#include <cstdint>
namespace prism::gpu {
struct float4 { float x = 0, y = 0, z = 0, w = 0; };
struct uint4 { uint32_t x = 0, y = 0, z = 0, w = 0; };
#define PRISM_GPU_STRUCT struct
#elif defined(PRISM_GLSL)
#define float4 vec4
#define uint4 uvec4
#define PRISM_GPU_STRUCT struct
#else
#define PRISM_GPU_STRUCT struct
#endif

// Prototype (BLAS) types.
#define PRISM_PROTO_TRIANGLES 0u
#define PRISM_PROTO_QUADS 1u
#define PRISM_PROTO_SPHERES 2u

// Instance flags.
#define PRISM_INSTANCE_PLACEMENT 1u   // has a placement transform (normal/tangent matrices)
#define PRISM_INSTANCE_FLIP 2u        // orientation flip of the geometric and shading normals
#define PRISM_INSTANCE_MIRRORED 4u    // reflection: negates the bitangent sign

// Material kinds, mirroring the CPU Material classes.
#define PRISM_MATERIAL_INVALID 0u     // magenta emission, no scattering
#define PRISM_MATERIAL_MDL 1u
#define PRISM_MATERIAL_DIFFUSE 2u
#define PRISM_MATERIAL_METAL 3u
#define PRISM_MATERIAL_EMISSIVE 4u
#define PRISM_MATERIAL_GLASS 5u
#define PRISM_MATERIAL_DISPERSIVE 6u
#define PRISM_MATERIAL_DIFFUSE_GLTF 7u
#define PRISM_MATERIAL_METAL_GLTF 8u
#define PRISM_MATERIAL_SPECULAR_BTDF_GLTF 9u
#define PRISM_MATERIAL_GLTF_PBR 10u
#define PRISM_MATERIAL_FALLBACK 11u   // hydra::FallbackMaterial (Diffuse with constant opacity)

// Material flags.
#define PRISM_MATERIAL_CONSTANT_CUTOUT 1u
#define PRISM_MATERIAL_NORMAL_TEXTURE 2u

// Legacy texture kinds (Texture<T> classes).
#define PRISM_TEXTURE_VALUE 0u
#define PRISM_TEXTURE_IMAGE 1u
#define PRISM_TEXTURE_IMAGE_MIX 2u
#define PRISM_TEXTURE_NONE 0xffffffffu

// MDL texture kinds.
#define PRISM_MDL_TEXTURE_INVALID 0u
#define PRISM_MDL_TEXTURE_IMAGE 1u
#define PRISM_MDL_TEXTURE_BSDF_DATA 2u

// Environment kinds.
#define PRISM_ENV_CONSTANT 0u
#define PRISM_ENV_GRADIENT 1u
#define PRISM_ENV_LATLONG 2u
#define PRISM_ENV_CYLINDRICAL 3u
#define PRISM_ENV_TRANSFORMED 1u      // flag in GpuEnvironment.info.w

// Frame flags.
#define PRISM_FRAME_ORTHOGRAPHIC 1u
#define PRISM_FRAME_ENVIRONMENT_MIS 2u
#define PRISM_FRAME_TRANSPARENT_ESCAPES 4u
#define PRISM_FRAME_FILTERED 8u
#define PRISM_FRAME_SEED_RNG 16u
#define PRISM_FRAME_LENS 32u

// Work modes.
#define PRISM_MODE_PASS 0u      // one sample per pixel of the listed tiles
#define PRISM_MODE_PREVIEW 1u   // one sample per preview pixel
#define PRISM_MODE_IMAGE 2u     // one sample per pixel, accumulated (standalone)

// Resource slots: descriptor-heap indices (HLSL) and set-0 bindings (GLSL).
#define PRISM_SLOT_FRAME 0
#define PRISM_SLOT_ACCEL 1
#define PRISM_SLOT_POSITIONS 2
#define PRISM_SLOT_NORMALS 3
#define PRISM_SLOT_TEXCOORDS 4
#define PRISM_SLOT_TANGENTS 5
#define PRISM_SLOT_INDICES 6
#define PRISM_SLOT_PROTOTYPES 7
#define PRISM_SLOT_INSTANCES 8
#define PRISM_SLOT_PRIMITIVE_MATERIALS 9
#define PRISM_SLOT_MATERIALS 10
#define PRISM_SLOT_LEGACY_TEXTURES 11
#define PRISM_SLOT_SPHERES 12
#define PRISM_SLOT_ENVIRONMENT 13
#define PRISM_SLOT_ENV_TEXEL_PDF 14
#define PRISM_SLOT_ENV_ALIAS_PROBABILITY 15
#define PRISM_SLOT_ENV_ALIAS 16
#define PRISM_SLOT_ARGUMENT_BLOCKS 17
#define PRISM_SLOT_RO_DATA 18
#define PRISM_SLOT_MDL_TEXTURES 19
#define PRISM_SLOT_MDL_TILES 20
#define PRISM_SLOT_BSDF_DATA 21
#define PRISM_SLOT_IMAGES 22
#define PRISM_SLOT_TILES 23
#define PRISM_SLOT_RNG 24
#define PRISM_SLOT_OUTPUT 25
#define PRISM_SLOT_ACCUMULATION 26
#define PRISM_SLOT_COUNT 27
// Images follow the fixed slots (HLSL) or form one array binding (GLSL).
#define PRISM_SLOT_IMAGE_BASE 32

// Sampler slots: anisotropic, mipmapped; index = wrapS * 3 + wrapT with
// wrap 0: clamp, 1: repeat, 2: mirrored repeat.
#define PRISM_SAMPLER_COUNT 9

#define PRISM_IMAGE_NONE 0xffffffffu
#define PRISM_TILE_MISSING 0xffffffffu

PRISM_GPU_STRUCT GpuPrototype {
    uint4 info;   // x: type, y: first vertex, z: first triangle (or sphere), w: primitive count
    uint4 info2;  // x: has per-vertex texture coordinates
};

PRISM_GPU_STRUCT GpuInstance {
    // Columns of the row-vector 3x3 placement matrices: v' = (dot(v,c0), dot(v,c1), dot(v,c2)).
    float4 normalColumns[3];
    float4 tangentColumns[3];
    uint4 info;   // x: prototype, y: flags, z: first primitive material, w: placement id
};

PRISM_GPU_STRUCT GpuMaterial {
    uint4 info;      // x: kind, y: MDL program, z: MDL argument block offset, w: flags
    uint4 textures;  // legacy texture records (kind dependent)
    uint4 textures2; // x: normal texture, y: gltfPBR emission
    float4 params;   // kind dependent scalars; params.w: constant cutout opacity
};

PRISM_GPU_STRUCT GpuLegacyTexture {
    uint4 info;      // x: kind, y: image, z: channels of the template type, w: unused
    float4 value;    // constant value (VALUE) or multiplier (IMAGE_MIX)
};

PRISM_GPU_STRUCT GpuImage {
    uint4 info;      // x: width, y: height, z: source channels, w: descriptor index
};

PRISM_GPU_STRUCT GpuMdlTexture {
    uint4 info;      // x: kind, y: first tile slot / data offset, z: u min / width, w: v min / height
    uint4 info2;     // x: u count / depth, y: v count / components
};

PRISM_GPU_STRUCT GpuEnvironment {
    uint4 info;        // x: kind, y: image (or PRISM_IMAGE_NONE), z: distribution width (0: uniform), w: flags
    uint4 info2;       // x: distribution height
    float4 color0;     // constant / gradient start / value texture
    float4 color1;     // gradient end
    float4 multiplier; // xyz: lat-long multiplier, w: transform determinant
    float4 toLocal[3]; // rows of the world-to-environment matrix
    float4 toWorld[3]; // rows of its inverse
};

PRISM_GPU_STRUCT GpuFrame {
    float4 inverseProjection[4]; // rows, row-vector convention (p' = p * M)
    float4 cameraToWorld[4];     // rows, row-vector convention
    float4 camera;               // x: near clip, y: far clip, z: aperture radius, w: focus distance
    float4 shading;              // x: sample clamp, y: exposure scale
    uint4 size;                  // x: width, y: height, z: preview width, w: preview height
    uint4 settings;              // x: max depth, y: roulette depth, z: flags, w: seed
    uint4 work;                  // x: thread count, y: tile count, z: mode, w: first thread
};

PRISM_GPU_STRUCT GpuTile {
    uint4 rect;   // x, y, width, height
    uint4 info;   // x: first thread of the tile
};

#if defined(__cplusplus)
} // namespace prism::gpu
#endif

#endif // PRISM_GPU_TYPES_H
