// Types of the MDL SDK's GLSL backend API, included by the generated code
// (backend option glsl_include_for_api_types) for code generated with
// texture derivatives, one texture space, 16 texture results, no auxiliary
// output and no DF handle slots.

#ifndef PRISM_MDL_TYPES_GLSL
#define PRISM_MDL_TYPES_GLSL

struct Derived_float { float val; float dx; float dy; };
struct Derived_float2 { vec2 val; vec2 dx; vec2 dy; };
struct Derived_float3 { vec3 val; vec3 dx; vec3 dy; };
struct Derived_float4 { vec4 val; vec4 dx; vec4 dy; };

struct State {
    vec3 normal;
    vec3 geom_normal;
    Derived_float3 position;
    float animation_time;
    Derived_float3 text_coords[1];
    vec3 tangent_u[1];
    vec3 tangent_v[1];
    vec4 text_results[PRISM_MDL_TEXTURE_RESULTS];
    int ro_data_segment_offset;
    mat4 world_to_object;
    mat4 object_to_world;
    int object_id;
    float meters_per_scene_unit;
    int arg_block_offset;
};

struct Bsdf_sample_data {
    vec3 ior1;
    vec3 ior2;
    vec3 k1;
    vec3 k2;
    vec4 xi;
    float pdf;
    vec3 bsdf_over_pdf;
    int event_type;
    int handle;
    int flags;
};

struct Bsdf_evaluate_data {
    vec3 ior1;
    vec3 ior2;
    vec3 k1;
    vec3 k2;
    vec3 bsdf_diffuse;
    vec3 bsdf_glossy;
    float pdf;
    int flags;
};

struct Bsdf_pdf_data {
    vec3 ior1;
    vec3 ior2;
    vec3 k1;
    vec3 k2;
    float pdf;
    int flags;
};

struct Bsdf_auxiliary_data {
    vec3 ior1;
    vec3 ior2;
    vec3 k1;
    vec3 albedo_diffuse;
    vec3 albedo_glossy;
    vec3 normal;
    vec3 roughness;
    int flags;
};

struct Edf_sample_data {
    vec4 xi;
    vec3 k1;
    float pdf;
    vec3 edf_over_pdf;
    int event_type;
    int handle;
};

struct Edf_evaluate_data {
    vec3 k1;
    float cos;
    vec3 edf;
    float pdf;
};

struct Edf_pdf_data {
    vec3 k1;
    float pdf;
};

struct Edf_auxiliary_data {
    vec3 k1;
};

#endif // PRISM_MDL_TYPES_GLSL
