// MDL renderer runtime entry points called by code from the MDL SDK's GLSL
// and HLSL backends. Written in the Prism shading dialect; the type names
// (float2 = vec2, Derived_float2, MdlState) resolve in both languages.
// HLSL includes this before the generated code, GLSL after it (the GLSL
// backend declares the prototypes itself).

#ifndef PRISM_MDL_RUNTIME_INC
#define PRISM_MDL_RUNTIME_INC

float mdl_read_argblock_as_float(int offs) { return prism_argblock_float(offs); }
float mdl_read_argblock_as_double(int offs) { return prism_argblock_double(offs); }
int mdl_read_argblock_as_int(int offs) { return prism_argblock_int(offs); }
uint mdl_read_argblock_as_uint(int offs) { return prism_argblock_uint(offs); }
bool mdl_read_argblock_as_bool(int offs) { return prism_argblock_bool(offs); }
float mdl_read_rodata_as_float(int offs) { return prism_rodata_float(offs); }
float mdl_read_rodata_as_double(int offs) { return prism_rodata_double(offs); }
int mdl_read_rodata_as_int(int offs) { return prism_rodata_int(offs); }
uint mdl_read_rodata_as_uint(int offs) { return prism_rodata_uint(offs); }
bool mdl_read_rodata_as_bool(int offs) { return prism_rodata_bool(offs); }

bool tex_texture_isvalid(int tex) { return prism_mdl_texture_valid(tex); }

// 2D textures
int tex_width_2d(int tex, int2 uv_tile, float frame) { return prism_mdl_resolution_2d(tex, uv_tile).x; }
int tex_height_2d(int tex, int2 uv_tile, float frame) { return prism_mdl_resolution_2d(tex, uv_tile).y; }
int tex_first_frame_2d(int tex) { return 0; }
int tex_last_frame_2d(int tex) { return 0; }

float4 tex_lookup_deriv_float4_2d(int tex, Derived_float2 coord, int wrap_u, int wrap_v, float2 crop_u,
                                  float2 crop_v, float frame)
{
    return prism_mdl_lookup_2d(tex, coord.val, coord.dx, coord.dy, wrap_u, wrap_v, crop_u, crop_v);
}
float3 tex_lookup_deriv_float3_2d(int tex, Derived_float2 coord, int wrap_u, int wrap_v, float2 crop_u,
                                  float2 crop_v, float frame)
{
    float4 v = prism_mdl_lookup_2d(tex, coord.val, coord.dx, coord.dy, wrap_u, wrap_v, crop_u, crop_v);
    return float3(v.x, v.y, v.z);
}
float3 tex_lookup_deriv_color_2d(int tex, Derived_float2 coord, int wrap_u, int wrap_v, float2 crop_u,
                                 float2 crop_v, float frame)
{
    float4 v = prism_mdl_lookup_2d(tex, coord.val, coord.dx, coord.dy, wrap_u, wrap_v, crop_u, crop_v);
    return float3(v.x, v.y, v.z);
}
float2 tex_lookup_deriv_float2_2d(int tex, Derived_float2 coord, int wrap_u, int wrap_v, float2 crop_u,
                                  float2 crop_v, float frame)
{
    float4 v = prism_mdl_lookup_2d(tex, coord.val, coord.dx, coord.dy, wrap_u, wrap_v, crop_u, crop_v);
    return float2(v.x, v.y);
}
float tex_lookup_deriv_float_2d(int tex, Derived_float2 coord, int wrap_u, int wrap_v, float2 crop_u,
                                float2 crop_v, float frame)
{
    return prism_mdl_lookup_2d(tex, coord.val, coord.dx, coord.dy, wrap_u, wrap_v, crop_u, crop_v).x;
}

float4 tex_lookup_float4_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v,
                            float frame)
{
    return prism_mdl_lookup_2d(tex, coord, float2(0.0f, 0.0f), float2(0.0f, 0.0f), wrap_u, wrap_v, crop_u, crop_v);
}
float3 tex_lookup_float3_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v,
                            float frame)
{
    float4 v = tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame);
    return float3(v.x, v.y, v.z);
}
float3 tex_lookup_color_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v,
                           float frame)
{
    float4 v = tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame);
    return float3(v.x, v.y, v.z);
}
float2 tex_lookup_float2_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v,
                            float frame)
{
    float4 v = tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame);
    return float2(v.x, v.y);
}
float tex_lookup_float_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v,
                          float frame)
{
    return tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame).x;
}

float4 tex_texel_float4_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    return prism_mdl_texel_2d(tex, coord, uv_tile);
}
float3 tex_texel_float3_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    float4 v = prism_mdl_texel_2d(tex, coord, uv_tile);
    return float3(v.x, v.y, v.z);
}
float3 tex_texel_color_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    float4 v = prism_mdl_texel_2d(tex, coord, uv_tile);
    return float3(v.x, v.y, v.z);
}
float2 tex_texel_float2_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    float4 v = prism_mdl_texel_2d(tex, coord, uv_tile);
    return float2(v.x, v.y);
}
float tex_texel_float_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    return prism_mdl_texel_2d(tex, coord, uv_tile).x;
}

// 3D textures (BSDF data tables)
int tex_width_3d(int tex, float frame) { return prism_mdl_resolution_3d(tex).x; }
int tex_height_3d(int tex, float frame) { return prism_mdl_resolution_3d(tex).y; }
int tex_depth_3d(int tex, float frame) { return prism_mdl_resolution_3d(tex).z; }
int tex_first_frame_3d(int tex) { return 0; }
int tex_last_frame_3d(int tex) { return 0; }

float4 tex_lookup_float4_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u,
                            float2 crop_v, float2 crop_w, float frame)
{
    return prism_mdl_lookup_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w);
}
float3 tex_lookup_float3_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u,
                            float2 crop_v, float2 crop_w, float frame)
{
    float4 v = prism_mdl_lookup_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w);
    return float3(v.x, v.y, v.z);
}
float3 tex_lookup_color_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u,
                           float2 crop_v, float2 crop_w, float frame)
{
    float4 v = prism_mdl_lookup_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w);
    return float3(v.x, v.y, v.z);
}
float2 tex_lookup_float2_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u,
                            float2 crop_v, float2 crop_w, float frame)
{
    float4 v = prism_mdl_lookup_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w);
    return float2(v.x, v.y);
}
float tex_lookup_float_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u,
                          float2 crop_v, float2 crop_w, float frame)
{
    return prism_mdl_lookup_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w).x;
}

float4 tex_texel_float4_3d(int tex, int3 coord, float frame) { return prism_mdl_texel_3d(tex, coord); }
float3 tex_texel_float3_3d(int tex, int3 coord, float frame)
{
    float4 v = prism_mdl_texel_3d(tex, coord);
    return float3(v.x, v.y, v.z);
}
float3 tex_texel_color_3d(int tex, int3 coord, float frame)
{
    float4 v = prism_mdl_texel_3d(tex, coord);
    return float3(v.x, v.y, v.z);
}
float2 tex_texel_float2_3d(int tex, int3 coord, float frame)
{
    float4 v = prism_mdl_texel_3d(tex, coord);
    return float2(v.x, v.y);
}
float tex_texel_float_3d(int tex, int3 coord, float frame) { return prism_mdl_texel_3d(tex, coord).x; }

// Cube maps and PTEX are not supported (as in the native runtime).
int tex_width_cube(int tex) { return 0; }
int tex_height_cube(int tex) { return 0; }
float4 tex_lookup_float4_cube(int tex, float3 coord) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
float3 tex_lookup_float3_cube(int tex, float3 coord) { return float3(0.0f, 0.0f, 0.0f); }
float3 tex_lookup_color_cube(int tex, float3 coord) { return float3(0.0f, 0.0f, 0.0f); }
float2 tex_lookup_float2_cube(int tex, float3 coord) { return float2(0.0f, 0.0f); }
float tex_lookup_float_cube(int tex, float3 coord) { return 0.0f; }
float4 tex_texel_float4_cube(int tex, int3 coord) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
float3 tex_texel_float3_cube(int tex, int3 coord) { return float3(0.0f, 0.0f, 0.0f); }
float3 tex_texel_color_cube(int tex, int3 coord) { return float3(0.0f, 0.0f, 0.0f); }
float2 tex_texel_float2_cube(int tex, int3 coord) { return float2(0.0f, 0.0f); }
float tex_texel_float_cube(int tex, int3 coord) { return 0.0f; }
float4 tex_lookup_float4_ptex(int tex, int channel) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }
float3 tex_lookup_float3_ptex(int tex, int channel) { return float3(0.0f, 0.0f, 0.0f); }
float3 tex_lookup_color_ptex(int tex, int channel) { return float3(0.0f, 0.0f, 0.0f); }
float2 tex_lookup_float2_ptex(int tex, int channel) { return float2(0.0f, 0.0f); }
float tex_lookup_float_ptex(int tex, int channel) { return 0.0f; }

// Light profiles and measured BSDFs are rejected at compile time.
bool df_light_profile_isvalid(int lp_idx) { return false; }
float df_light_profile_power(int lp_idx) { return 0.0f; }
float df_light_profile_maximum(int lp_idx) { return 0.0f; }
float df_light_profile_evaluate(int lp_idx, float2 theta_phi) { return 0.0f; }
float3 df_light_profile_sample(int lp_idx, float3 xi) { return float3(-1.0f, -1.0f, 0.0f); }
float df_light_profile_pdf(int lp_idx, float2 theta_phi) { return 0.0f; }
bool df_bsdf_measurement_isvalid(int bm_idx) { return false; }
int3 df_bsdf_measurement_resolution(int bm_idx, int part) { return int3(0, 0, 0); }
float3 df_bsdf_measurement_evaluate(int bm_idx, float2 theta_phi_in, float2 theta_phi_out, int part)
{
    return float3(0.0f, 0.0f, 0.0f);
}
float3 df_bsdf_measurement_sample(int bm_idx, float2 theta_phi_out, float3 xi, int part)
{
    return float3(-1.0f, -1.0f, 0.0f);
}
float df_bsdf_measurement_pdf(int bm_idx, float2 theta_phi_in, float2 theta_phi_out, int part) { return 0.0f; }
float4 df_bsdf_measurement_albedos(int bm_idx, float2 theta_phi) { return float4(0.0f, 0.0f, 0.0f, 0.0f); }

// No scene data: lookups return their defaults, as in the native runtime.
bool scene_data_isvalid(INOUT(MdlState) state, int scene_data_id) { return false; }
float4 scene_data_lookup_float4(INOUT(MdlState) state, int id, float4 value, bool uniform_lookup) { return value; }
float3 scene_data_lookup_float3(INOUT(MdlState) state, int id, float3 value, bool uniform_lookup) { return value; }
float3 scene_data_lookup_color(INOUT(MdlState) state, int id, float3 value, bool uniform_lookup) { return value; }
float2 scene_data_lookup_float2(INOUT(MdlState) state, int id, float2 value, bool uniform_lookup) { return value; }
float scene_data_lookup_float(INOUT(MdlState) state, int id, float value, bool uniform_lookup) { return value; }
int4 scene_data_lookup_int4(INOUT(MdlState) state, int id, int4 value, bool uniform_lookup) { return value; }
int3 scene_data_lookup_int3(INOUT(MdlState) state, int id, int3 value, bool uniform_lookup) { return value; }
int2 scene_data_lookup_int2(INOUT(MdlState) state, int id, int2 value, bool uniform_lookup) { return value; }
int scene_data_lookup_int(INOUT(MdlState) state, int id, int value, bool uniform_lookup) { return value; }

#endif // PRISM_MDL_RUNTIME_INC
