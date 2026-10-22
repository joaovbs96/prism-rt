// Records shared by the GPU host code (C++, after src/device/hlsl_math.h)
// and the GPU kernels (HLSL). Every record is built from 16-byte float4/uint4
// members only, so the C++ layout matches HLSL structured buffers on Metal
// and Vulkan without padding rules. Scalar data lives in separate buffers.

#pragma once


// Prototype (BLAS) types.
static const uint kProtoTriangles = 0u;
static const uint kProtoQuads = 1u;
static const uint kProtoSpheres = 2u;

// Instance flags.
static const uint kInstancePlacement = 1u;   // has a placement transform (normal/tangent matrices)
static const uint kInstanceFlip = 2u;        // orientation flip of the geometric and shading normals
static const uint kInstanceMirrored = 4u;    // reflection: negates the bitangent sign
static const uint kInstanceOpaque = 8u;      // every material always blocks shadow rays
static const uint kInstanceCullBackFaces = 16u; // some primitive has kPrimitiveCullBackFace (non-opaque in the
                                                // acceleration structure, so the trace can skip its back faces)

// Per-primitive material table entries: the material index, and the
// prism:honorDoubleSided cull bit of single-sided, non-transmissive triangles.
static const uint kPrimitiveMaterialMask = 0x7fffffffu;
static const uint kPrimitiveCullBackFace = 0x80000000u;

// Instance masks of the acceleration structure: shadow rays first query the
// opaque instances for any hit, then walk the others for cutouts.
static const uint kMaskOpaque = 1u;
static const uint kMaskNonOpaque = 2u;
static const uint kMaskAll = 0xFFu;

// Material kinds, mirroring the CPU Material classes.
static const uint kMaterialInvalid = 0u;     // magenta emission, no scattering
static const uint kMaterialMdl = 1u;
static const uint kMaterialDiffuse = 2u;
static const uint kMaterialMetal = 3u;
static const uint kMaterialEmissive = 4u;
static const uint kMaterialGlass = 5u;
static const uint kMaterialDispersive = 6u;
static const uint kMaterialDiffuseGltf = 7u;
static const uint kMaterialMetalGltf = 8u;
static const uint kMaterialSpecularBtdfGltf = 9u;
static const uint kMaterialGltfPbr = 10u;
static const uint kMaterialFallback = 11u;   // hydra::FallbackMaterial (Diffuse with an opacity)

// Material flags.
static const uint kMaterialConstantCutout = 1u;
static const uint kMaterialNormalTexture = 2u;
// Fallback materials: the hit prototype's displayColor / displayOpacity
// primvar (kPrimvarDisplay*) replaces the constant colour / opacity.
static const uint kMaterialPrimvarColor = 4u;
static const uint kMaterialPrimvarOpacity = 8u;

// Legacy texture kinds (Texture<T> classes).
static const uint kTextureValue = 0u;
static const uint kTextureImage = 1u;
static const uint kTextureImageMix = 2u;
static const uint kTextureNone = 0xffffffffu;

// MDL texture kinds.
static const uint kMdlTextureInvalid = 0u;
static const uint kMdlTextureImage = 1u;
static const uint kMdlTextureBsdfData = 2u;

// Environment kinds.
static const uint kEnvConstant = 0u;
static const uint kEnvGradient = 1u;
static const uint kEnvLatlong = 2u;
static const uint kEnvCylindrical = 3u;
static const uint kEnvTransformed = 1u;      // flag in GpuEnvironment.info.w

// Frame flags.
static const uint kFrameOrthographic = 1u;
static const uint kFrameEnvironmentMis = 2u;
static const uint kFrameTransparentEscapes = 4u;
static const uint kFrameFiltered = 8u;
static const uint kFrameSeedRng = 16u;
static const uint kFrameLens = 32u;
static const uint kFrameCutouts = 64u;       // some instances are not opaque
static const uint kFramePrimaryHits = 128u;  // preview, pass: also store each pixel's center-ray hit
static const uint kFrameLightLinking = 256u; // instances carry light/shadow-link categories
static const uint kFrameSpatialLights = 512u; // spatial light selection (SampleSettings::spatialLights)
static const uint kFrameTextureFeedback = 1024u; // record requested texture levels (Vulkan texture streaming)
static const uint kFrameLightLobes = 2048u;   // UsdLux diffuse/specular light multipliers (LightTable::lobes)

// Work modes.
static const uint kModePass = 0u;      // one sample per listed pixel
static const uint kModePreview = 1u;   // one sample per preview pixel
static const uint kModePrimary = 3u;   // only the pixel-center hits of the listed pixels
static const uint kModeImage = 2u;     // one sample per listed pixel, accumulated (standalone)

// Resource slots: descriptor-heap indices (HLSL) and set-0 bindings (GLSL).
static const uint kSlotFrame = 0;
static const uint kSlotAccel = 1;
static const uint kSlotPositions = 2;
static const uint kSlotNormals = 3;
static const uint kSlotTexcoords = 4;
static const uint kSlotTangents = 5;
static const uint kSlotIndices = 6;
static const uint kSlotPrototypes = 7;
static const uint kSlotInstances = 8;
static const uint kSlotPrimitiveMaterials = 9;
static const uint kSlotMaterials = 10;
static const uint kSlotLegacyTextures = 11;
static const uint kSlotSpheres = 12;
static const uint kSlotEnvironment = 13;
static const uint kSlotEnvTexelPdf = 14;
static const uint kSlotEnvAliasProbability = 15;
static const uint kSlotEnvAlias = 16;
static const uint kSlotArgumentBlocks = 17;
static const uint kSlotRoData = 18;
static const uint kSlotMdlTextures = 19;
static const uint kSlotMdlTiles = 20;
static const uint kSlotBsdfData = 21;
static const uint kSlotImages = 22;
static const uint kSlotPixels = 23;   // pixel indices of the pass or image threads
static const uint kSlotRng = 24;
static const uint kSlotOutput = 25;
static const uint kSlotAccumulation = 26;
static const uint kSlotLights = 27;        // LightRecord (shaders/integrator/lights.h)
static const uint kSlotLightAlias = 28;    // float4 alias table over the lights and the dome
static const uint kSlotPrimvarValues = 29;     // float, named primvars per vertex
static const uint kSlotPrimvarDirectory = 30;  // uint4 per named primvar of a prototype
static const uint kSlotTextureFeedback = 31;   // uint per image and level (16): weighted requests
static const uint kSlotCount = 32;
// Images follow the fixed slots (HLSL) or form one array binding (GLSL).
static const uint kSlotImageBase = 32;

// Sampler slots: anisotropic, mipmapped; index = wrapS * 3 + wrapT with
// wrap 0: clamp, 1: repeat, 2: mirrored repeat.
static const uint kSamplerCount = 9;

static const uint kImageNone = 0xffffffffu;
static const uint kTileMissing = 0xffffffffu;

struct GpuPrototype {
    uint4 info;   // x: type, y: first vertex, z: first triangle (or sphere), w: primitive count
    uint4 info2;  // x: has per-vertex texture coordinates, y: first primvar directory entry,
                  // z: primvar count, w: first primvar value
};

// One named primvar of a prototype: x: scene data id (string constant of the
// MDL link unit, 0: unused), y: components (kPrimvarComponents) and role
// (kPrimvarDisplay*), z: offset in a vertex's values, w: values per vertex.
static const uint kPrimvarComponents = 0xffu;
static const uint kPrimvarDisplayColor = 0x100u;   // USD displayColor (fallback materials)
static const uint kPrimvarDisplayOpacity = 0x200u; // USD displayOpacity
static const uint kPrimvarRoles = 0xff00u;

struct GpuPrimvar {
    uint4 info;
};

struct GpuInstance {
    // Columns of the row-vector 3x3 placement matrices: v' = (dot(v,c0), dot(v,c1), dot(v,c2)).
    float4 normalColumns[3];
    float4 tangentColumns[3];
    uint4 info;   // x: prototype, y: flags, z: first primitive material, w: placement id
    uint4 links;  // x: light-link categories, y: shadow-link categories (~0u without linking),
                  // z: first per-instance primvar directory entry, w: their count
};

struct GpuMaterial {
    uint4 info;      // x: kind, y: MDL program, z: MDL argument block offset, w: flags
    uint4 textures;  // legacy texture records (kind dependent); MDL programs translated alone
                     // (callables): x read-only data offset (bytes), y texture table, z string
                     // table (32-bit word indices into the read-only data)
    uint4 textures2; // x: normal texture, y: gltfPBR emission
    float4 params;   // kind dependent scalars; params.w: constant cutout opacity
    float4 emitter;  // x: selection density per unit area of its emissive triangles (0: none),
                     // y: 1 if both faces emit
};

struct GpuLegacyTexture {
    uint4 info;      // x: kind, y: image, z: channels of the template type, w: unused
    float4 value;    // constant value (VALUE) or multiplier (IMAGE_MIX)
};

struct GpuImage {
    uint4 info;      // x: width, y: height, z: source channels, w: finest resident mip level
                     // (Vulkan texture streaming: the image holds levels w and coarser; else 0)
};

struct GpuMdlTexture {
    uint4 info;      // x: kind, y: first tile slot / data offset, z: u min / width, w: v min / height
    uint4 info2;     // x: u count / depth, y: v count / components
};

struct GpuEnvironment {
    uint4 info;        // x: kind, y: image (or kImageNone), z: distribution width (0: uniform), w: flags
    uint4 info2;       // x: distribution height
    float4 color0;     // constant / gradient start / value texture
    float4 color1;     // gradient end
    float4 multiplier; // xyz: lat-long multiplier, w: transform determinant
    float4 toLocal[3]; // rows of the world-to-environment matrix
    float4 toWorld[3]; // rows of its inverse
    float4 selection;  // x: probability of sampling this dome among the domes, y: UsdLux diffuse
                       // multiplier, z: specular multiplier
    uint4 info3;       // x: offset of its importance tables
};

struct GpuFrame {
    float4 inverseProjection[4]; // rows, row-vector convention (p' = p * M)
    float4 cameraToWorld[4];     // rows, row-vector convention
    float4 camera;               // x: near clip, y: far clip, z: aperture radius, w: focus distance
    float4 shading;              // x: sample clamp, y: exposure scale, z: minimum NEE probability,
                                 // w: depth from which NEE is probabilistic
    uint4 size;                  // x: width, y: height, z: preview width, w: preview height
    uint4 settings;              // x: max depth, y: roulette depth, z: flags, w: seed
    uint4 work;                  // x: thread count, y: image mode: pixels traced in parallel
                                 // (0: one sample per listed pixel), z: mode
    uint4 counts;                // x: MDL texture count, y: debug view, z: sample number,
                                 // w: light contribution mask (0: all)
    uint4 lights;                // x: light records, y: analytic lights, z: domes
    uint4 streaming;             // texture feedback (kFrameTextureFeedback): x: image count, y: LOD margin
                                 // (1/16 levels), z: phase,
                                 // w: pixel stride (one pixel of each stride x stride block records)
    float4 color;                // xyz: luminance weights of the working space (light selection)
};



