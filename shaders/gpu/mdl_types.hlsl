// Types of the MDL SDK's HLSL backend API (MDL_TARGET_CODE_TYPES) for code
// generated with texture derivatives, one texture space, no auxiliary
// output and no DF handle slots.

#pragma once

// Texture results cached by generated init functions (backend option
// num_texture_results; see gpu_materials.h).
static const int kMdlTextureResults = 16;

struct Derived_float { float val; float dx; float dy; };
struct Derived_float2 { float2 val; float2 dx; float2 dy; };
struct Derived_float3 { float3 val; float3 dx; float3 dy; };
struct Derived_float4 { float4 val; float4 dx; float4 dy; };

struct Shading_state_material {
    float3 normal;
    float3 geom_normal;
    Derived_float3 position;
    float animation_time;
    Derived_float3 text_coords[1];
    float3 tangent_u[1];
    float3 tangent_v[1];
    float4 text_results[kMdlTextureResults];
    uint ro_data_segment_offset;
    float4x4 world_to_object;
    float4x4 object_to_world;
    uint object_id;
    float meters_per_scene_unit;
    uint arg_block_offset;
};


struct Bsdf_sample_data {
    float3 ior1;
    float3 ior2;
    float3 k1;
    float3 k2;
    float4 xi;
    float pdf;
    float3 bsdf_over_pdf;
    int event_type;
    int handle;
    int flags;
};

struct Bsdf_evaluate_data {
    float3 ior1;
    float3 ior2;
    float3 k1;
    float3 k2;
    float3 bsdf_diffuse;
    float3 bsdf_glossy;
    float pdf;
    int flags;
};

struct Bsdf_pdf_data {
    float3 ior1;
    float3 ior2;
    float3 k1;
    float3 k2;
    float pdf;
    int flags;
};

struct Bsdf_auxiliary_data {
    float3 ior1;
    float3 ior2;
    float3 k1;
    float3 albedo_diffuse;
    float3 albedo_glossy;
    float3 normal;
    float3 roughness;
    int flags;
};

struct Edf_sample_data {
    float4 xi;
    float3 k1;
    float pdf;
    float3 edf_over_pdf;
    int event_type;
    int handle;
};

struct Edf_evaluate_data {
    float3 k1;
    float cos;
    float3 edf;
    float pdf;
};

struct Edf_pdf_data {
    float3 k1;
    float pdf;
};

struct Edf_auxiliary_data {
    float3 k1;
};

float4x4 identityMatrix()
{
    return float4x4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

