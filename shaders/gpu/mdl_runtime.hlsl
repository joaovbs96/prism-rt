// MDL renderer runtime for the GPU devices: the functions that code from the
// MDL SDK's HLSL backend calls, mirroring the native texture runtime
// (src/mdl/texture_runtime.cpp). Requires gpu_backend resource accessors.
#pragma once

static const int kTexWrapClamp = 0;
static const int kTexWrapRepeat = 1;
static const int kTexWrapMirroredRepeat = 2;
static const int kTexWrapClip = 3;

// Converts the bits of an IEEE double to float without the double type,
// which Apple GPUs lack. Denormals flush to zero.
float prism_double_bits_to_float(uint lo, uint hi)
{
    uint sign = hi & 0x80000000u;
    int exponent = int((hi >> 20u) & 0x7ffu);
    if (exponent == 0) return asfloat(sign);
    if (exponent == 0x7ff)
        return asfloat(sign | 0x7f800000u | (((hi & 0xfffffu) != 0u || lo != 0u) ? 1u : 0u));
    int e = exponent - 1023 + 127;
    if (e >= 0xff) return asfloat(sign | 0x7f800000u);
    if (e <= 0) return asfloat(sign);
    uint mantissa = ((hi & 0xfffffu) << 3u) | (lo >> 29u);
    return asfloat(sign | (uint(e) << 23u) | mantissa);
}

// Code of one program translated alone (PRISM_MDL_PROGRAM_TABLES, the
// Vulkan callables): its texture indices, read-only data offsets and string
// ids are its own, placed in the scene's buffers by the material's tables
// (GpuMaterial.textures, set by gpu_build_mdl_state). A link unit's code
// uses the scene's indices directly.
#ifdef PRISM_MDL_PROGRAM_TABLES
static uint g_prism_rodata_base = 0u;    // bytes
static uint g_prism_texture_table = 0u;  // words of the read-only data
static uint g_prism_string_table = 0u;
int prism_texture_index(int tex)
{
    return tex <= 0 ? tex : int(gpu_rodata_word(g_prism_texture_table + uint(tex)));
}
int prism_scene_data_global_id(int id)
{
    return id <= 0 ? id : int(gpu_rodata_word(g_prism_string_table + uint(id)));
}
uint prism_rodata_byte_word(int offs) { return gpu_rodata_word((g_prism_rodata_base + uint(offs)) >> 2u); }
#else
int prism_texture_index(int tex) { return tex; }
int prism_scene_data_global_id(int id) { return id; }
uint prism_rodata_byte_word(int offs) { return gpu_rodata_word(uint(offs) >> 2u); }
#endif

uint prism_argblock_byte_word(int offs) { return gpu_argblock_word(uint(offs) >> 2u); }
float prism_argblock_float(int offs) { return asfloat(prism_argblock_byte_word(offs)); }
int prism_argblock_int(int offs) { return int(prism_argblock_byte_word(offs)); }
uint prism_argblock_uint(int offs) { return prism_argblock_byte_word(offs); }
bool prism_argblock_bool(int offs)
{
    return ((prism_argblock_byte_word(offs) >> (8u * (uint(offs) & 3u))) & 0xffu) != 0u;
}
float prism_argblock_double(int offs)
{
    return prism_double_bits_to_float(prism_argblock_byte_word(offs), prism_argblock_byte_word(offs + 4));
}
float prism_rodata_float(int offs) { return asfloat(prism_rodata_byte_word(offs)); }
int prism_rodata_int(int offs) { return int(prism_rodata_byte_word(offs)); }
uint prism_rodata_uint(int offs) { return prism_rodata_byte_word(offs); }
bool prism_rodata_bool(int offs)
{
    return ((prism_rodata_byte_word(offs) >> (8u * (uint(offs) & 3u))) & 0xffu) != 0u;
}
float prism_rodata_double(int offs)
{
    return prism_double_bits_to_float(prism_rodata_byte_word(offs), prism_rodata_byte_word(offs + 4));
}

bool prism_mdl_texture_valid(int tex)
{
    if (tex <= 0 || uint(tex) >= gpu_mdl_texture_count()) return false;
    return gpu_mdl_texture(uint(tex)).info.x != kMdlTextureInvalid;
}

// The image of UDIM tile (u, v), or kImageNone.
uint prism_mdl_tile_image(GpuMdlTexture info, int u, int v)
{
    int du = u - int(info.info.z);
    int dv = v - int(info.info.w);
    if (du < 0 || dv < 0 || du >= int(info.info2.x) || dv >= int(info.info2.y)) return kImageNone;
    uint image = gpu_mdl_tile(info.info.y + uint(dv) * info.info2.x + uint(du));
    return image == kTileMissing ? kImageNone : image;
}

// MDL expands scalar images to RGB and adds opaque alpha (ExpandChannels).
float4 prism_expand_channels(float4 v, uint channels)
{
    if (channels == 1u) return float4(v.x, v.x, v.x, 1.0f);
    if (channels == 2u) return float4(v.x, v.x, v.x, v.y);
    if (channels == 3u) return float4(v.x, v.y, v.z, 1.0f);
    return v;
}

// Remap: wrapping in the MDL coordinate domain. False for clipped lookups.
bool prism_mdl_remap(inout float x, inout float dx, inout float dy, int mode)
{
    if (mode == kTexWrapClip && (x < 0.0f || x > 1.0f)) return false;
    if (mode == kTexWrapClamp || mode == kTexWrapClip) {
        if (x <= 0.0f || x >= 1.0f) { dx = 0.0f; dy = 0.0f; }
        x = clamp(x, 0.0f, 1.0f);
    } else if (mode == kTexWrapMirroredRepeat) {
        float cell = floor(x);
        x -= cell;
        if (abs(cell) - 2.0f * floor(abs(cell) * 0.5f) != 0.0f) { x = 1.0f - x; dx = -dx; dy = -dy; }
    } else {
        x -= floor(x);
    }
    return true;
}

// Sampler wrap of the remapped coordinate (Wrap in the native runtime).
uint prism_mdl_sampler_wrap(int mode)
{
    if (mode == kTexWrapRepeat) return 1u;
    if (mode == kTexWrapMirroredRepeat) return 2u;
    return 0u;
}

// One integer crop interval of an image axis (croppedResource).
void prism_crop_axis(float c0, float c1, int size, bool topDown,
                                  out float offset, out float scale)
{
    float s0 = clamp(c0, 0.0f, 1.0f);
    float s1 = clamp(c1, s0, 1.0f);
    int first = int(float(size - 1) * s0);
    int count = min(size - first, max(1, int(float(size) * (s1 - s0))));
    if (topDown) first = size - first - count;
    offset = float(first) / float(size);
    scale = float(count) / float(size);
}

float4 prism_mdl_lookup_2d(int tex, float2 coord, float2 dx, float2 dy, int wrapU,
                                        int wrapV, float2 cropU, float2 cropV)
{
    tex = prism_texture_index(tex);
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != kMdlTextureImage) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float s = coord.x;
    float t = coord.y;
    if (!isfinite(s) || !isfinite(t)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    bool tiled = info.info2.z != 0u;
    if (tiled && (s < -2147483648.0f || s >= 2147483648.0f || t < -2147483648.0f || t >= 2147483648.0f))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    int tileU = tiled ? int(floor(s)) : 0;
    int tileV = tiled ? int(floor(t)) : 0;
    uint image = prism_mdl_tile_image(info, tileU, tileV);
    if (image == kImageNone) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (tiled) { s -= float(tileU); t -= float(tileV); }
    float dsdx = dx.x;
    float dsdy = dy.x;
    float dtdx = dx.y;
    float dtdy = dy.y;
    if (!prism_mdl_remap(s, dsdx, dsdy, wrapU) || !prism_mdl_remap(t, dtdx, dtdy, wrapV))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuImage img = gpu_image(image);
    // MDL canvas coordinates start at the lower-left, images at the top.
    float qs = s;
    float qt = 1.0f - t;
    dtdx = -dtdx;
    dtdy = -dtdy;
    if (cropU.x != 0.0f || cropU.y != 1.0f || cropV.x != 0.0f || cropV.y != 1.0f) {
        float offsetS;
        float scaleS;
        float offsetT;
        float scaleT;
        prism_crop_axis(cropU.x, cropU.y, int(img.info.x), false, offsetS, scaleS);
        prism_crop_axis(cropV.x, cropV.y, int(img.info.y), true, offsetT, scaleT);
        qs = offsetS + qs * scaleS;
        qt = offsetT + qt * scaleT;
        dsdx *= scaleS; dsdy *= scaleS;
        dtdx *= scaleT; dtdy *= scaleT;
    }
    float4 value = gpu_image_sample(image, float2(qs, qt), float2(dsdx, dtdx), float2(dsdy, dtdy),
                                    prism_mdl_sampler_wrap(wrapU), prism_mdl_sampler_wrap(wrapV));
    return prism_expand_channels(value, img.info.z);
}

int2 prism_mdl_resolution_2d(int tex, int2 uvTile)
{
    tex = prism_texture_index(tex);
    if (!prism_mdl_texture_valid(tex)) return int2(0, 0);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != kMdlTextureImage) return int2(0, 0);
    uint image = prism_mdl_tile_image(info, uvTile.x, uvTile.y);
    if (image == kImageNone) return int2(0, 0);
    GpuImage img = gpu_image(image);
    return int2(int(img.info.x), int(img.info.y));
}

float4 prism_mdl_texel_2d(int tex, int2 coord, int2 uvTile)
{
    tex = prism_texture_index(tex);
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != kMdlTextureImage) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint image = prism_mdl_tile_image(info, uvTile.x, uvTile.y);
    if (image == kImageNone) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuImage img = gpu_image(image);
    if (coord.x < 0 || coord.y < 0 || coord.x >= int(img.info.x) || coord.y >= int(img.info.y))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return prism_expand_channels(gpu_image_texel(image, coord.x, int(img.info.y) - 1 - coord.y),
                                 img.info.z);
}

// BSDF data tables: manual trilinear filtering, as in the native runtime
// (CropAxis, RemapTexel, LoadDfTexel). MDL's layered BSDFs call this at every
// layer and it is inlined everywhere, so it is written without loops or
// short-circuit conditions: the three axes as vectors, the wrap modes and
// crop checks as selects, one branch for the texture, one for the
// coordinates and one for the second and third components of RGB tables
// (most tables are scalar: 8 reads). Remapped texels always lie inside the
// cropped table, so the reads need no bounds checks. The arithmetic is that
// of the per-axis, per-texel version, in the same order.
float4 prism_df_texel(GpuMdlTexture info, int x, int y, int z)
{
    uint width = info.info.z;
    uint height = info.info.w;
    uint depth = info.info2.x;
    uint components = info.info2.y;
    if (x < 0 || y < 0 || z < 0 || uint(x) >= width || uint(y) >= height || uint(z) >= depth)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint index = info.info.y + ((uint(z) * height + uint(y)) * width + uint(x)) * components;
    if (components == 1u) {
        float v = gpu_bsdf_data(index);
        return float4(v, v, v, 1.0f);
    }
    return float4(gpu_bsdf_data(index), gpu_bsdf_data(index + 1u), gpu_bsdf_data(index + 2u), 1.0f);
}

// Texel indices of the axes for wrap modes `mode` in a cropped table of
// `count` texels starting at `offset`.
int3 prism_df_wrap(int3 value, int3 count, int3 mode, int3 offset)
{
    int3 clamped = clamp(value, int3(0, 0, 0), count - 1);
    int3 cell = value / count;
    int3 local = value - cell * count;
    bool3 negative = local < 0;
    local = select(negative, local + count, local);
    cell = select(negative, cell - 1, cell);
    local = select(and(mode == kTexWrapMirroredRepeat, (cell & 1) != 0), count - 1 - local, local);
    return select(or(mode == kTexWrapClamp, mode == kTexWrapClip), clamped, local) + offset;
}

// One component of the table, filtered: lerps over x of the four (y, z)
// texel pairs (indices of the x0 and x1 texels), then over y and z, in the
// order of the native runtime.
float prism_df_trilinear(uint4 x0, uint4 x1, float3 f)
{
    float4 lower = float4(gpu_bsdf_data(x0.x), gpu_bsdf_data(x0.y), gpu_bsdf_data(x0.z), gpu_bsdf_data(x0.w));
    float4 upper = float4(gpu_bsdf_data(x1.x), gpu_bsdf_data(x1.y), gpu_bsdf_data(x1.z), gpu_bsdf_data(x1.w));
    float4 x = lower * (1.0f - f.x) + upper * f.x;
    float2 y = x.xz * (1.0f - f.y) + x.yw * f.y;
    return y.x * (1.0f - f.z) + y.y * f.z;
}

float4 prism_mdl_lookup_3d(int tex, float3 coord, int wrapU, int wrapV, int wrapW,
                                        float2 cropU, float2 cropV, float2 cropW)
{
    tex = prism_texture_index(tex);
    if (or(tex <= 0, uint(tex) >= gpu_mdl_texture_count())) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    uint3 size = uint3(info.info.z, info.info.w, info.info2.x);
    // Crop: the texel range [offset, offset + count) of each axis.
    float3 c0 = float3(cropU.x, cropV.x, cropW.x);
    float3 c1 = float3(cropU.y, cropV.y, cropW.y);
    bool3 valid = and(and(isfinite(c0), isfinite(c1)), and(and(c0 >= 0.0f, c1 <= 1.0f), and(c1 >= c0, size != 0u)));
    uint3 offset = min(uint3(float3(size - 1u) * c0), size - 1u);
    uint3 count = max(uint3(float3(size) * (c1 - c0)), 1u);
    count = min(count, size - offset);
    // Remap: the two texels around the coordinate and the weight between them.
    int3 mode = int3(wrapU, wrapV, wrapW);
    valid = and(valid, and(isfinite(coord), !and(mode == kTexWrapClip, or(coord < 0.0f, coord >= 1.0f))));
    float3 t = coord * float3(count) - 0.5f;
    float3 base = floor(t);
    float3 f = t - base;
    int3 n = int3(max(count, 1u));
    int3 lower = prism_df_wrap(int3(base), n, mode, int3(offset));
    int3 upper = prism_df_wrap(int3(base) + 1, n, mode, int3(offset));
    if (or(info.info.x != kMdlTextureBsdfData, !all(valid))) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint width = info.info.z;
    uint height = info.info.w;
    uint components = info.info2.y;
    uint3 lo = uint3(lower);
    uint3 hi = uint3(upper);
    // Rows (y, z) = (lo, lo), (hi, lo), (lo, hi), (hi, hi), first component.
    uint4 rows = info.info.y + (uint4(lo.z, lo.z, hi.z, hi.z) * height + uint4(lo.y, hi.y, lo.y, hi.y)) * width * components;
    uint4 x0 = rows + lo.x * components;
    uint4 x1 = rows + hi.x * components;
    float r = prism_df_trilinear(x0, x1, f);
    float3 value = float3(r, r, r);
    [branch] if (components != 1u) {
        value.y = prism_df_trilinear(x0 + 1u, x1 + 1u, f);
        value.z = prism_df_trilinear(x0 + 2u, x1 + 2u, f);
    }
    // Alpha: the same lerps of opaque texels.
    float w = 1.0f * (1.0f - f.x) + 1.0f * f.x;
    w = w * (1.0f - f.y) + w * f.y;
    w = w * (1.0f - f.z) + w * f.z;
    return float4(value, w);
}

int3 prism_mdl_resolution_3d(int tex)
{
    tex = prism_texture_index(tex);
    if (!prism_mdl_texture_valid(tex)) return int3(0, 0, 0);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != kMdlTextureBsdfData) return int3(0, 0, 0);
    return int3(int(info.info.z), int(info.info.w), int(info.info2.x));
}

float4 prism_mdl_texel_3d(int tex, int3 coord)
{
    tex = prism_texture_index(tex);
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != kMdlTextureBsdfData) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return prism_df_texel(info, coord.x, coord.y, coord.z);
}



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

bool tex_texture_isvalid(int tex) { return prism_mdl_texture_valid(prism_texture_index(tex)); }

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

bool prism_scene_data_is_texcoord(int id);
bool prism_scene_data_is_camera_position(int id);

// A named primvar of the hit triangle (Mesh::interpolatePrimvar).
bool prism_primvar(Shading_state_material state, int id, out float4 value)
{
    value = (float4)0.0f;
    if (state.primvar_prototype == 0xffffffffu || id <= 0) return false;
    const GpuPrototype prototype = gpu_prototype(state.primvar_prototype);
    for (uint i = 0u; i < prototype.info2.z; ++i) {
        const GpuPrimvar primvar = gpu_primvar(prototype.info2.y + i);
        if (primvar.info.x != uint(prism_scene_data_global_id(id))) continue;
        const float w[3] = {1.0f - state.primvar_bary.x - state.primvar_bary.y, state.primvar_bary.x,
                            state.primvar_bary.y};
        float result[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (uint k = 0u; k < 3u; ++k) {
            const uint base = prototype.info2.w + (state.primvar_vertex[k] - prototype.info.y) * primvar.info.w +
                primvar.info.z;
            for (uint c = 0u; c < 4u; ++c)
                if (c < (primvar.info.y & kPrimvarComponents)) result[c] += w[k] * gpu_primvar_value(base + c);
        }
        value = float4(result[0], result[1], result[2], result[3]);
        return true;
    }
    return false;
}

// Scene data: the texture coordinates under their usual primvar names
// (prism_scene_data_is_texcoord, generated per link unit); other lookups
// return their defaults, as in the native runtime.
bool scene_data_isvalid(inout Shading_state_material state, int scene_data_id)
{
    float4 probe;
    return prism_scene_data_is_texcoord(scene_data_id) || prism_scene_data_is_camera_position(scene_data_id) ||
        prism_primvar(state, scene_data_id, probe);
}
float4 scene_data_lookup_float4(inout Shading_state_material state, int id, float4 value, bool uniform_lookup)
{
    float4 primvar;
    return prism_primvar(state, id, primvar) ? primvar : value;
}
// MaterialX viewdirection: the view point of a path tracer is the origin of the ray that hit.
float3 scene_data_lookup_float3(inout Shading_state_material state, int id, float3 value, bool uniform_lookup)
{
    if (prism_scene_data_is_camera_position(id)) return state.view_origin;
    float4 primvar;
    return prism_primvar(state, id, primvar) ? primvar.xyz : value;
}
float3 scene_data_lookup_color(inout Shading_state_material state, int id, float3 value, bool uniform_lookup)
{
    float4 primvar;
    return prism_primvar(state, id, primvar) ? primvar.xyz : value;
}
float2 scene_data_lookup_float2(inout Shading_state_material state, int id, float2 value, bool uniform_lookup)
{
    if (prism_scene_data_is_texcoord(id))
        return float2(state.text_coords[0].val.x, state.text_coords[0].val.y);
    float4 primvar;
    return prism_primvar(state, id, primvar) ? primvar.xy : value;
}
Derived_float2 scene_data_lookup_deriv_float2(inout Shading_state_material state, int id, Derived_float2 value,
                                              bool uniform_lookup)
{
    float4 primvar;
    if (!prism_scene_data_is_texcoord(id)) {
        if (!prism_primvar(state, id, primvar)) return value;
        Derived_float2 named;
        named.val = primvar.xy;
        named.dx = (float2)0.0f;
        named.dy = (float2)0.0f;
        return named;
    }
    Derived_float2 result;
    result.val = float2(state.text_coords[0].val.x, state.text_coords[0].val.y);
    result.dx = float2(state.text_coords[0].dx.x, state.text_coords[0].dx.y);
    result.dy = float2(state.text_coords[0].dy.x, state.text_coords[0].dy.y);
    return result;
}
Derived_float scene_data_lookup_deriv_float(inout Shading_state_material state, int id, Derived_float value,
                                            bool uniform_lookup)
{
    float4 primvar;
    if (!prism_primvar(state, id, primvar)) return value;
    Derived_float result;
    result.val = primvar.x;
    result.dx = 0.0f;
    result.dy = 0.0f;
    return result;
}
Derived_float3 scene_data_lookup_deriv_float3(inout Shading_state_material state, int id, Derived_float3 value,
                                              bool uniform_lookup)
{
    float4 primvar;
    const bool camera = prism_scene_data_is_camera_position(id);
    if (!camera && !prism_primvar(state, id, primvar)) return value;
    Derived_float3 result;
    result.val = camera ? state.view_origin : primvar.xyz;
    result.dx = (float3)0.0f;
    result.dy = (float3)0.0f;
    return result;
}
Derived_float4 scene_data_lookup_deriv_float4(inout Shading_state_material state, int id, Derived_float4 value,
                                              bool uniform_lookup)
{
    float4 primvar;
    if (!prism_primvar(state, id, primvar)) return value;
    Derived_float4 result;
    result.val = primvar;
    result.dx = (float4)0.0f;
    result.dy = (float4)0.0f;
    return result;
}
Derived_float3 scene_data_lookup_deriv_color(inout Shading_state_material state, int id, Derived_float3 value,
                                             bool uniform_lookup)
{
    float4 primvar;
    if (!prism_primvar(state, id, primvar)) return value;
    Derived_float3 result;
    result.val = primvar.xyz;
    result.dx = (float3)0.0f;
    result.dy = (float3)0.0f;
    return result;
}
float scene_data_lookup_float(inout Shading_state_material state, int id, float value, bool uniform_lookup)
{
    float4 primvar;
    return prism_primvar(state, id, primvar) ? primvar.x : value;
}
int4 scene_data_lookup_int4(inout Shading_state_material state, int id, int4 value, bool uniform_lookup) { return value; }
int3 scene_data_lookup_int3(inout Shading_state_material state, int id, int3 value, bool uniform_lookup) { return value; }
int2 scene_data_lookup_int2(inout Shading_state_material state, int id, int2 value, bool uniform_lookup) { return value; }
int scene_data_lookup_int(inout Shading_state_material state, int id, int value, bool uniform_lookup) { return value; }

