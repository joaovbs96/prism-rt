// Language-independent part of the MDL renderer runtime for GPU devices,
// mirroring the native texture runtime (src/mdl/texture_runtime.cpp).
// Written in the Prism shading dialect. The per-language runtime files wrap
// these functions with the exact signatures the generated code calls.
//
// Requires: gpu_argblock_word, gpu_rodata_word, gpu_mdl_texture, gpu_mdl_tile,
// gpu_mdl_texture_count, gpu_image, gpu_image_sample, gpu_image_texel,
// gpu_bsdf_data.

#ifndef PRISM_MDL_RUNTIME_COMMON_INC
#define PRISM_MDL_RUNTIME_COMMON_INC

#define PRISM_TEX_WRAP_CLAMP 0
#define PRISM_TEX_WRAP_REPEAT 1
#define PRISM_TEX_WRAP_MIRRORED_REPEAT 2
#define PRISM_TEX_WRAP_CLIP 3

// Converts the bits of an IEEE double to float without the double type,
// which Apple GPUs lack. Denormals flush to zero.
PRISM_INLINE float prism_double_bits_to_float(uint lo, uint hi)
{
    uint sign = hi & 0x80000000u;
    int exponent = int((hi >> 20u) & 0x7ffu);
    if (exponent == 0) return prism_asfloat(sign);
    if (exponent == 0x7ff)
        return prism_asfloat(sign | 0x7f800000u | (((hi & 0xfffffu) != 0u || lo != 0u) ? 1u : 0u));
    int e = exponent - 1023 + 127;
    if (e >= 0xff) return prism_asfloat(sign | 0x7f800000u);
    if (e <= 0) return prism_asfloat(sign);
    uint mantissa = ((hi & 0xfffffu) << 3u) | (lo >> 29u);
    return prism_asfloat(sign | (uint(e) << 23u) | mantissa);
}

PRISM_INLINE uint prism_argblock_byte_word(int offs) { return gpu_argblock_word(uint(offs) >> 2u); }
PRISM_INLINE float prism_argblock_float(int offs) { return prism_asfloat(prism_argblock_byte_word(offs)); }
PRISM_INLINE int prism_argblock_int(int offs) { return int(prism_argblock_byte_word(offs)); }
PRISM_INLINE uint prism_argblock_uint(int offs) { return prism_argblock_byte_word(offs); }
PRISM_INLINE bool prism_argblock_bool(int offs)
{
    return ((prism_argblock_byte_word(offs) >> (8u * (uint(offs) & 3u))) & 0xffu) != 0u;
}
PRISM_INLINE float prism_argblock_double(int offs)
{
    return prism_double_bits_to_float(prism_argblock_byte_word(offs), prism_argblock_byte_word(offs + 4));
}
PRISM_INLINE uint prism_rodata_byte_word(int offs) { return gpu_rodata_word(uint(offs) >> 2u); }
PRISM_INLINE float prism_rodata_float(int offs) { return prism_asfloat(prism_rodata_byte_word(offs)); }
PRISM_INLINE int prism_rodata_int(int offs) { return int(prism_rodata_byte_word(offs)); }
PRISM_INLINE uint prism_rodata_uint(int offs) { return prism_rodata_byte_word(offs); }
PRISM_INLINE bool prism_rodata_bool(int offs)
{
    return ((prism_rodata_byte_word(offs) >> (8u * (uint(offs) & 3u))) & 0xffu) != 0u;
}
PRISM_INLINE float prism_rodata_double(int offs)
{
    return prism_double_bits_to_float(prism_rodata_byte_word(offs), prism_rodata_byte_word(offs + 4));
}

PRISM_INLINE bool prism_mdl_texture_valid(int tex)
{
    if (tex <= 0 || uint(tex) >= gpu_mdl_texture_count()) return false;
    return gpu_mdl_texture(uint(tex)).info.x != PRISM_MDL_TEXTURE_INVALID;
}

// The image of UDIM tile (u, v), or PRISM_IMAGE_NONE.
PRISM_INLINE uint prism_mdl_tile_image(GpuMdlTexture info, int u, int v)
{
    int du = u - int(info.info.z);
    int dv = v - int(info.info.w);
    if (du < 0 || dv < 0 || du >= int(info.info2.x) || dv >= int(info.info2.y)) return PRISM_IMAGE_NONE;
    uint image = gpu_mdl_tile(info.info.y + uint(dv) * info.info2.x + uint(du));
    return image == PRISM_TILE_MISSING ? PRISM_IMAGE_NONE : image;
}

// MDL expands scalar images to RGB and adds opaque alpha (ExpandChannels).
PRISM_INLINE float4 prism_expand_channels(float4 v, uint channels)
{
    if (channels == 1u) return float4(v.x, v.x, v.x, 1.0f);
    if (channels == 2u) return float4(v.x, v.x, v.x, v.y);
    if (channels == 3u) return float4(v.x, v.y, v.z, 1.0f);
    return v;
}

// Remap: wrapping in the MDL coordinate domain. False for clipped lookups.
PRISM_INLINE bool prism_mdl_remap(INOUT(float) x, INOUT(float) dx, INOUT(float) dy, int mode)
{
    if (mode == PRISM_TEX_WRAP_CLIP && (x < 0.0f || x > 1.0f)) return false;
    if (mode == PRISM_TEX_WRAP_CLAMP || mode == PRISM_TEX_WRAP_CLIP) {
        if (x <= 0.0f || x >= 1.0f) { dx = 0.0f; dy = 0.0f; }
        x = clamp(x, 0.0f, 1.0f);
    } else if (mode == PRISM_TEX_WRAP_MIRRORED_REPEAT) {
        float cell = floor(x);
        x -= cell;
        if (abs(cell) - 2.0f * floor(abs(cell) * 0.5f) != 0.0f) { x = 1.0f - x; dx = -dx; dy = -dy; }
    } else {
        x -= floor(x);
    }
    return true;
}

// Sampler wrap of the remapped coordinate (Wrap in the native runtime).
PRISM_INLINE uint prism_mdl_sampler_wrap(int mode)
{
    if (mode == PRISM_TEX_WRAP_REPEAT) return 1u;
    if (mode == PRISM_TEX_WRAP_MIRRORED_REPEAT) return 2u;
    return 0u;
}

// One integer crop interval of an image axis (croppedResource).
PRISM_INLINE void prism_crop_axis(float c0, float c1, int size, bool topDown,
                                  OUT(float) offset, OUT(float) scale)
{
    float s0 = clamp(c0, 0.0f, 1.0f);
    float s1 = clamp(c1, s0, 1.0f);
    int first = int(float(size - 1) * s0);
    int count = min(size - first, max(1, int(float(size) * (s1 - s0))));
    if (topDown) first = size - first - count;
    offset = float(first) / float(size);
    scale = float(count) / float(size);
}

PRISM_INLINE float4 prism_mdl_lookup_2d(int tex, float2 coord, float2 dx, float2 dy, int wrapU,
                                        int wrapV, float2 cropU, float2 cropV)
{
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != PRISM_MDL_TEXTURE_IMAGE) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float s = coord.x;
    float t = coord.y;
    if (!prism_isfinite(s) || !prism_isfinite(t)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    bool tiled = info.info2.z != 0u;
    if (tiled && (s < -2147483648.0f || s >= 2147483648.0f || t < -2147483648.0f || t >= 2147483648.0f))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    int tileU = tiled ? int(floor(s)) : 0;
    int tileV = tiled ? int(floor(t)) : 0;
    uint image = prism_mdl_tile_image(info, tileU, tileV);
    if (image == PRISM_IMAGE_NONE) return float4(0.0f, 0.0f, 0.0f, 0.0f);
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

PRISM_INLINE int2 prism_mdl_resolution_2d(int tex, int2 uvTile)
{
    if (!prism_mdl_texture_valid(tex)) return int2(0, 0);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != PRISM_MDL_TEXTURE_IMAGE) return int2(0, 0);
    uint image = prism_mdl_tile_image(info, uvTile.x, uvTile.y);
    if (image == PRISM_IMAGE_NONE) return int2(0, 0);
    GpuImage img = gpu_image(image);
    return int2(int(img.info.x), int(img.info.y));
}

PRISM_INLINE float4 prism_mdl_texel_2d(int tex, int2 coord, int2 uvTile)
{
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != PRISM_MDL_TEXTURE_IMAGE) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint image = prism_mdl_tile_image(info, uvTile.x, uvTile.y);
    if (image == PRISM_IMAGE_NONE) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuImage img = gpu_image(image);
    if (coord.x < 0 || coord.y < 0 || coord.x >= int(img.info.x) || coord.y >= int(img.info.y))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return prism_expand_channels(gpu_image_texel(image, coord.x, int(img.info.y) - 1 - coord.y),
                                 img.info.z);
}

// BSDF data tables: manual trilinear filtering, as in the native runtime.
PRISM_INLINE bool prism_crop_texels(float c0, float c1, uint size, OUT(uint) offset, OUT(uint) count)
{
    offset = 0u;
    count = 0u;
    if (!prism_isfinite(c0) || !prism_isfinite(c1) || c0 < 0.0f || c1 > 1.0f || c1 < c0 || size == 0u)
        return false;
    offset = uint(float(size - 1u) * c0);
    count = max(uint(float(size) * (c1 - c0)), 1u);
    if (offset >= size) offset = size - 1u;
    count = min(count, size - offset);
    return true;
}

PRISM_INLINE bool prism_remap_texel(float coordinate, uint size, int mode, uint offset,
                                    OUT(int) lower, OUT(int) upper, OUT(float) fraction)
{
    lower = 0;
    upper = 0;
    fraction = 0.0f;
    if (!prism_isfinite(coordinate) || size == 0u) return false;
    if (mode == PRISM_TEX_WRAP_CLIP && (coordinate < 0.0f || coordinate >= 1.0f)) return false;
    float tex = coordinate * float(size) - 0.5f;
    float base = floor(tex);
    fraction = tex - base;
    int n = int(size);
    for (int k = 0; k < 2; ++k) {
        int value = int(base) + k;
        int mapped;
        if (mode == PRISM_TEX_WRAP_CLAMP || mode == PRISM_TEX_WRAP_CLIP) {
            mapped = clamp(value, 0, n - 1);
        } else {
            int cell = value / n;
            int local = value - cell * n;
            if (local < 0) { local += n; cell -= 1; }
            if (mode == PRISM_TEX_WRAP_MIRRORED_REPEAT && (abs(cell) % 2) == 1) local = n - 1 - local;
            mapped = local;
        }
        if (k == 0) lower = mapped + int(offset);
        else upper = mapped + int(offset);
    }
    return true;
}

PRISM_INLINE float4 prism_df_texel(GpuMdlTexture info, int x, int y, int z)
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

PRISM_INLINE float4 prism_lerp4(float4 a, float4 b, float t) { return a * (1.0f - t) + b * t; }

PRISM_INLINE float4 prism_mdl_lookup_3d(int tex, float3 coord, int wrapU, int wrapV, int wrapW,
                                        float2 cropU, float2 cropV, float2 cropW)
{
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != PRISM_MDL_TEXTURE_BSDF_DATA) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint ox;
    uint oy;
    uint oz;
    uint nx;
    uint ny;
    uint nz;
    if (!prism_crop_texels(cropU.x, cropU.y, info.info.z, ox, nx) ||
        !prism_crop_texels(cropV.x, cropV.y, info.info.w, oy, ny) ||
        !prism_crop_texels(cropW.x, cropW.y, info.info2.x, oz, nz))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    int x0;
    int x1;
    int y0;
    int y1;
    int z0;
    int z1;
    float fx;
    float fy;
    float fz;
    if (!prism_remap_texel(coord.x, nx, wrapU, ox, x0, x1, fx) ||
        !prism_remap_texel(coord.y, ny, wrapV, oy, y0, y1, fy) ||
        !prism_remap_texel(coord.z, nz, wrapW, oz, z0, z1, fz))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 a = prism_lerp4(prism_lerp4(prism_df_texel(info, x0, y0, z0), prism_df_texel(info, x1, y0, z0), fx),
                           prism_lerp4(prism_df_texel(info, x0, y1, z0), prism_df_texel(info, x1, y1, z0), fx), fy);
    float4 b = prism_lerp4(prism_lerp4(prism_df_texel(info, x0, y0, z1), prism_df_texel(info, x1, y0, z1), fx),
                           prism_lerp4(prism_df_texel(info, x0, y1, z1), prism_df_texel(info, x1, y1, z1), fx), fy);
    return prism_lerp4(a, b, fz);
}

PRISM_INLINE int3 prism_mdl_resolution_3d(int tex)
{
    if (!prism_mdl_texture_valid(tex)) return int3(0, 0, 0);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != PRISM_MDL_TEXTURE_BSDF_DATA) return int3(0, 0, 0);
    return int3(int(info.info.z), int(info.info.w), int(info.info2.x));
}

PRISM_INLINE float4 prism_mdl_texel_3d(int tex, int3 coord)
{
    if (!prism_mdl_texture_valid(tex)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuMdlTexture info = gpu_mdl_texture(uint(tex));
    if (info.info.x != PRISM_MDL_TEXTURE_BSDF_DATA) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return prism_df_texel(info, coord.x, coord.y, coord.z);
}

#endif // PRISM_MDL_RUNTIME_COMMON_INC
