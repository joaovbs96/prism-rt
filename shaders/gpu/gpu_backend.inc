// GPU implementation of the integrator backend interface (see
// shaders/integrator/path_tracer.inc), shared by the Vulkan (GLSL) and Metal
// (HLSL) devices. Written in the Prism shading dialect. It mirrors the CPU
// classes: SceneManager::prepareHit / prepareFootprint, the legacy Material
// classes, MDLMaterial and the Environment classes.
//
// Requires the per-language resource accessors (gpu_frame, gpu_position, ...),
// gpu_trace, the MDL dispatch functions (prism_mdl_*), the MDL state type
// MdlState and PRISM_IDENTITY_MATRIX.

#ifndef PRISM_GPU_BACKEND_INC
#define PRISM_GPU_BACKEND_INC

PRISM_INLINE float3 gpu_xyz(float4 v) { return float3(v.x, v.y, v.z); }

// Row-vector 3x3 transform stored as columns.
PRISM_INLINE float3 gpu_transform(float3 v, float4 c0, float4 c1, float4 c2)
{
    return float3(dot(v, gpu_xyz(c0)), dot(v, gpu_xyz(c1)), dot(v, gpu_xyz(c2)));
}

PRISM_INLINE bool gpu_filtered() { return (gpu_frame().settings.z & PRISM_FRAME_FILTERED) != 0u; }

PRISM_INLINE float3 gpu_reflect(float3 i, float3 n) { return i - 2.0f * dot(i, n) * n; }

// ---------------------------------------------------------------------------
// Hits
// ---------------------------------------------------------------------------

PRISM_INLINE Hit gpu_empty_hit(IN(PathRay) ray)
{
    Hit h;
    h.hit = false;
    h.o = ray.ray.origin;
    h.d = ray.ray.direction;
    h.p = h.o;
    h.n = splat3(0.0f);
    h.n_g = splat3(0.0f);
    h.n_s = splat3(0.0f);
    h.tangent = splat3(0.0f);
    h.bitangent_sign = 1.0f;
    h.u = 0.0f;
    h.v = 0.0f;
    h.front_face = false;
    h.t = ray.ray.tfar;
    h.instance = 0u;
    h.primitive = 0u;
    h.material = 0u;
    h.ray_footprint = ray.footprint;
    h.texture_footprint.dudx = 0.0f;
    h.texture_footprint.dvdx = 0.0f;
    h.texture_footprint.dudy = 0.0f;
    h.texture_footprint.dvdy = 0.0f;
    h.texture_footprint.valid = false;
    h.texture_footprint.fallback = false;
    h.texture_footprint.filtered = false;
    h.dpdx = splat3(0.0f);
    h.dpdy = splat3(0.0f);
    return h;
}

PRISM_INLINE float3 gpu_plane_differential(IN(Hit) hit, float3 dO, float3 dD, float predicted)
{
    float3 o = hit.o + dO;
    float3 d = hit.d + dD;
    float denominator = dot(d, hit.n_g);
    if (abs(denominator) < 1.0e-6f) return splat3(0.0f);
    float distance = dot(hit.p - o, hit.n_g) / denominator;
    float3 value = o + distance * d - hit.p;
    float len = length(value);
    if (!prism_isfinite(len) || len > predicted * 8.0f) return splat3(0.0f);
    return value;
}

// SceneManager::prepareFootprint
PRISM_INLINE void gpu_prepare_footprint(INOUT(Hit) hit, IN(GpuPrototype) proto, IN(GpuInstance) inst,
                                        uint localPrimitive)
{
    float predicted = max(1.0e-8f, hit.ray_footprint.width + max(0.0f, hit.t) * hit.ray_footprint.spread);
    if (hit.ray_footprint.exact) {
        hit.dpdx = gpu_plane_differential(hit, hit.ray_footprint.dOdx, hit.ray_footprint.dDdx, predicted);
        hit.dpdy = gpu_plane_differential(hit, hit.ray_footprint.dOdy, hit.ray_footprint.dDdy, predicted);
    }
    if (prism_is_zero(hit.dpdx) || prism_is_zero(hit.dpdy)) {
        float3 axis = abs(hit.n_g.x) < 0.8f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
        float3 x = normalize(cross(axis, hit.n_g));
        float3 y = normalize(cross(hit.n_g, x));
        hit.dpdx = predicted * x;
        hit.dpdy = predicted * y;
        hit.texture_footprint.fallback = true;
    }

    float3 dpdu = splat3(0.0f);
    float3 dpdv = splat3(0.0f);
    bool spherePole = false;
    if (proto.info.x == PRISM_PROTO_TRIANGLES) {
        if (proto.info2.x != 0u) {
            uint base = 3u * (proto.info.z + localPrimitive);
            uint i0 = proto.info.y + gpu_index(base);
            uint i1 = proto.info.y + gpu_index(base + 1u);
            uint i2 = proto.info.y + gpu_index(base + 2u);
            float3 e1 = gpu_position(i1) - gpu_position(i0);
            float3 e2 = gpu_position(i2) - gpu_position(i0);
            float2 t0 = gpu_texcoord(i0);
            float2 t1 = gpu_texcoord(i1);
            float2 t2 = gpu_texcoord(i2);
            float du1 = t1.x - t0.x;
            float dv1 = t1.y - t0.y;
            float du2 = t2.x - t0.x;
            float dv2 = t2.y - t0.y;
            float determinant = du1 * dv2 - du2 * dv1;
            if (abs(determinant) > 1.0e-12f) {
                float inverse = 1.0f / determinant;
                dpdu = inverse * (dv2 * e1 - dv1 * e2);
                dpdv = inverse * (-du2 * e1 + du1 * e2);
            }
        }
    } else if (proto.info.x == PRISM_PROTO_SPHERES) {
        float4 sphere = gpu_sphere(proto.info.z + localPrimitive);
        float radius = sphere.w;
        if (!(radius > 0.0f) || !prism_isfinite(radius)) {
            spherePole = true;
        } else {
            float theta = PRISM_PI - 2.0f * PRISM_PI * hit.u;
            float phi = PRISM_PI * hit.v;
            float sinPhi = sin(phi);
            float cosPhi = cos(phi);
            spherePole = abs(sinPhi) < 1.0e-4f;
            dpdu = 2.0f * PRISM_PI * radius * float3(sinPhi * sin(theta), 0.0f, -sinPhi * cos(theta));
            dpdv = PRISM_PI * radius * float3(cosPhi * cos(theta), -sinPhi, cosPhi * sin(theta));
        }
    }
    if ((inst.info.y & PRISM_INSTANCE_PLACEMENT) != 0u) {
        dpdu = gpu_transform(dpdu, inst.tangentColumns[0], inst.tangentColumns[1], inst.tangentColumns[2]);
        dpdv = gpu_transform(dpdv, inst.tangentColumns[0], inst.tangentColumns[1], inst.tangentColumns[2]);
    }
    float a = dot(dpdu, dpdu);
    float b = dot(dpdu, dpdv);
    float c = dot(dpdv, dpdv);
    float determinant = a * c - b * b;
    hit.texture_footprint.filtered = gpu_filtered();
    if (determinant > 1.0e-20f && prism_isfinite(determinant)) {
        float x = dot(hit.dpdx, dpdu);
        float y = dot(hit.dpdx, dpdv);
        hit.texture_footprint.dudx = (c * x - b * y) / determinant;
        hit.texture_footprint.dvdx = (a * y - b * x) / determinant;
        x = dot(hit.dpdy, dpdu);
        y = dot(hit.dpdy, dpdv);
        hit.texture_footprint.dudy = (c * x - b * y) / determinant;
        hit.texture_footprint.dvdy = (a * y - b * x) / determinant;
        hit.texture_footprint.valid =
            prism_isfinite(hit.texture_footprint.dudx) && prism_isfinite(hit.texture_footprint.dvdx) &&
            prism_isfinite(hit.texture_footprint.dudy) && prism_isfinite(hit.texture_footprint.dvdy);
    }
    if (spherePole) {
        float meridianScale = sqrt(max(c, 1.0e-12f));
        float dv = min(1.0f, predicted / meridianScale);
        hit.texture_footprint.dudx = 0.5f;
        hit.texture_footprint.dvdx = dv;
        hit.texture_footprint.dudy = 0.5f;
        hit.texture_footprint.dvdy = dv;
        hit.texture_footprint.valid = true;
        hit.texture_footprint.fallback = true;
    } else if (!hit.texture_footprint.valid) {
        float scale = sqrt(max(max(a, c), 1.0e-12f));
        float rho = min(1.0f, predicted / scale);
        hit.texture_footprint.dudx = rho;
        hit.texture_footprint.dvdx = 0.0f;
        hit.texture_footprint.dudy = 0.0f;
        hit.texture_footprint.dvdy = rho;
        hit.texture_footprint.valid = true;
        hit.texture_footprint.fallback = true;
    }
}

// SceneManager::cast_ray: intersection plus RayHit construction and the
// placement handling of prepareHit.
Hit backend_intersect(IN(PathRay) ray)
{
    Hit hit = gpu_empty_hit(ray);
    GpuRawHit raw = gpu_trace(ray.ray);
    if (!raw.hit) return hit;
    GpuInstance inst = gpu_instance(raw.instance);
    GpuPrototype proto = gpu_prototype(inst.info.x);
    bool placement = (inst.info.y & PRISM_INSTANCE_PLACEMENT) != 0u;
    bool flip = (inst.info.y & PRISM_INSTANCE_FLIP) != 0u;
    uint type = proto.info.x;
    uint localPrimitive = type == PRISM_PROTO_QUADS ? raw.primitive / 2u : raw.primitive;

    hit.hit = true;
    hit.t = raw.t;
    hit.p = hit.o + hit.t * hit.d;
    hit.instance = inst.info.w;
    hit.primitive = localPrimitive;
    hit.material = gpu_primitive_material(inst.info.z + localPrimitive);

    // Geometric normal in the prototype's space (Embree conventions).
    float3 ng = splat3(0.0f);
    uint i0 = 0u;
    uint i1 = 0u;
    uint i2 = 0u;
    float4 sphere = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (type == PRISM_PROTO_SPHERES) {
        sphere = gpu_sphere(proto.info.z + localPrimitive);
        ng = raw.objectOrigin + raw.t * raw.objectDirection - float3(sphere.x, sphere.y, sphere.z);
    } else {
        uint base = 3u * (proto.info.z + raw.primitive);
        i0 = proto.info.y + gpu_index(base);
        i1 = proto.info.y + gpu_index(base + 1u);
        i2 = proto.info.y + gpu_index(base + 2u);
        float3 p0 = gpu_position(i0);
        ng = cross(gpu_position(i1) - p0, gpu_position(i2) - p0);
    }
    float3 localNormal = ng;
    if (placement) {
        ng = gpu_transform(ng, inst.normalColumns[0], inst.normalColumns[1], inst.normalColumns[2]);
        if (flip) ng = -ng;
    }

    // RayHit
    hit.n = normalize(ng);
    hit.front_face = dot(hit.d, hit.n) < 0.0f;
    hit.n = hit.front_face ? hit.n : -hit.n;
    hit.n_g = hit.n;
    hit.u = raw.barycentrics.x;
    hit.v = raw.barycentrics.y;
    if (type == PRISM_PROTO_TRIANGLES) {
        float u = raw.barycentrics.x;
        float v = raw.barycentrics.y;
        float w = 1.0f - u - v;
        float3 n = w * gpu_vertex_normal(i0) + u * gpu_vertex_normal(i1) + v * gpu_vertex_normal(i2);
        float2 uv = gpu_texcoord(i0) * w + gpu_texcoord(i1) * u + gpu_texcoord(i2) * v;
        float4 tangent = gpu_tangent(i0) * w + gpu_tangent(i1) * u + gpu_tangent(i2) * v;
        hit.n = normalize(n);
        hit.n = hit.front_face ? hit.n : -hit.n;
        hit.u = uv.x;
        hit.v = uv.y;
        hit.tangent = gpu_xyz(tangent);
        hit.bitangent_sign = tangent.w < 0.0f ? -1.0f : 1.0f;
    } else if (type == PRISM_PROTO_SPHERES) {
        // Texture coordinates must not change when viewed from inside.
        float3 outward = placement ? normalize(localNormal) : (hit.front_face ? hit.n : -hit.n);
        float theta = prism_atan2(outward.z, outward.x);
        float phi = acos(outward.y);
        hit.u = 1.0f - ((theta + PRISM_PI) / (2.0f * PRISM_PI));
        hit.v = phi / PRISM_PI;
        hit.tangent = float3(outward.z, 0.0f, -outward.x);
        hit.bitangent_sign = -1.0f;
    } else {
        // Quads: Embree's (u, v) over the quad, from the triangle pair.
        if ((raw.primitive & 1u) != 0u) {
            hit.u = 1.0f - raw.barycentrics.x;
            hit.v = 1.0f - raw.barycentrics.y;
        }
    }

    if (placement) {
        if (type == PRISM_PROTO_TRIANGLES)
            hit.n = normalize(gpu_transform(hit.n, inst.normalColumns[0], inst.normalColumns[1],
                                            inst.normalColumns[2]));
        if (!prism_is_zero(hit.tangent))
            hit.tangent = normalize(gpu_transform(hit.tangent, inst.tangentColumns[0],
                                                  inst.tangentColumns[1], inst.tangentColumns[2]));
        if (flip) hit.n = -hit.n;
        if ((inst.info.y & PRISM_INSTANCE_MIRRORED) != 0u) hit.bitangent_sign *= -1.0f;
    }
    gpu_prepare_footprint(hit, proto, inst, localPrimitive);
    return hit;
}

// ---------------------------------------------------------------------------
// Images and legacy textures
// ---------------------------------------------------------------------------

// ImageTexture::sample without a footprint: the nearest texel.
PRISM_INLINE float4 gpu_image_nearest(uint image, float u, float v)
{
    GpuImage img = gpu_image(image);
    int i = int(abs(u - trunc(u)) * float(img.info.x - 1u));
    int j = int(abs(v - trunc(v)) * float(img.info.y - 1u));
    return gpu_image_texel(image, i, j);
}

// OIIO bilinear lookup at (s, t) with black outside the image.
PRISM_INLINE float4 gpu_image_bilinear_black(uint image, float s, float t)
{
    GpuImage img = gpu_image(image);
    float x = s * float(img.info.x) - 0.5f;
    float y = t * float(img.info.y) - 0.5f;
    float fx = floor(x);
    float fy = floor(y);
    float ax = x - fx;
    float ay = y - fy;
    int x0 = int(fx);
    int y0 = int(fy);
    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int k = 0; k < 4; ++k) {
        int xi = x0 + (k & 1);
        int yi = y0 + (k >> 1);
        if (xi < 0 || yi < 0 || xi >= int(img.info.x) || yi >= int(img.info.y)) continue;
        float w = ((k & 1) != 0 ? ax : 1.0f - ax) * ((k >> 1) != 0 ? ay : 1.0f - ay);
        result += w * gpu_image_texel(image, xi, yi);
    }
    return result;
}

// Texture<T>::sample(u, v, p) (useFootprint false) or
// Texture<T>::sample(u, v, p, footprint).
PRISM_INLINE float4 gpu_legacy_texture_sample(uint ref, float u, float v, bool useFootprint,
                                              IN(TextureFootprint) footprint)
{
    if (ref == PRISM_TEXTURE_NONE) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuLegacyTexture tex = gpu_legacy_texture(ref);
    if (tex.info.x == PRISM_TEXTURE_VALUE) return tex.value;
    float4 value;
    if (!useFootprint || !footprint.filtered)
        value = gpu_image_nearest(tex.info.y, u, v);
    else
        value = gpu_image_sample(tex.info.y, float2(u, v), float2(footprint.dudx, footprint.dvdx),
                                 float2(footprint.dudy, footprint.dvdy), 1u, 1u);
    if (tex.info.x == PRISM_TEXTURE_IMAGE_MIX) value = value * tex.value;
    return value;
}

PRISM_INLINE float3 gpu_texture3(uint ref, IN(Hit) hit, bool useFootprint)
{
    return gpu_xyz(gpu_legacy_texture_sample(ref, hit.u, hit.v, useFootprint, hit.texture_footprint));
}

PRISM_INLINE float gpu_texture1(uint ref, IN(Hit) hit, bool useFootprint)
{
    return gpu_legacy_texture_sample(ref, hit.u, hit.v, useFootprint, hit.texture_footprint).x;
}

// ---------------------------------------------------------------------------
// Legacy materials (material.h, hydra/fallback_material.h)
// ---------------------------------------------------------------------------

PRISM_INLINE float3 gpu_random_unit_vector(INOUT(uint) seed)
{
    float a = prism_rnd(seed) * (2.0f * PRISM_PI);
    float z = (2.0f * prism_rnd(seed)) - 1.0f;
    float r = sqrt(1.0f - (z * z));
    return float3(r * cos(a), r * sin(a), z);
}

PRISM_INLINE bool gpu_is_near_zero(float3 v)
{
    return abs(v.x) < 1e-8f && abs(v.y) < 1e-8f && abs(v.z) < 1e-8f;
}

PRISM_INLINE float3 gpu_refract(float3 wi, float3 n, float ior)
{
    float cosTheta = min(dot(-wi, n), 1.0f);
    float3 r1 = ior * (wi + cosTheta * n);
    float3 r2 = -sqrt(abs(1.0f - dot(r1, r1))) * n;
    return r1 + r2;
}

PRISM_INLINE float gpu_schlick(float cosine, float ior)
{
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    return r0 + (1.0f - r0) * pow(1.0f - cosine, 5.0f);
}

PRISM_INLINE BsdfSample gpu_bsdf_sample_empty()
{
    BsdfSample s;
    s.wi = splat3(0.0f);
    s.color = splat3(0.0f);
    s.emission = splat3(0.0f);
    s.scattered = false;
    s.pdf = 0.0f;
    s.delta = false;
    s.null_event = false;
    return s;
}

PRISM_INLINE BsdfSample gpu_null_sample(IN(Hit) hit)
{
    BsdfSample s = gpu_bsdf_sample_empty();
    s.wi = hit.d;
    s.color = splat3(1.0f);
    s.scattered = true;
    s.delta = true;
    s.null_event = true;
    return s;
}

PRISM_INLINE BsdfSample gpu_invalid_sample()
{
    BsdfSample s = gpu_bsdf_sample_empty();
    s.emission = float3(1.0f, 0.0f, 1.0f);
    return s;
}

// Diffuse::sample (cosine-like lobe n + random unit vector).
PRISM_INLINE BsdfSample gpu_diffuse_sample(IN(Hit) hit, uint colorTexture, bool footprintColor,
                                           bool withPdf, INOUT(uint) seed)
{
    BsdfSample s = gpu_bsdf_sample_empty();
    float3 wi = hit.n + gpu_random_unit_vector(seed);
    if (gpu_is_near_zero(wi)) wi = hit.n;
    s.wi = wi;
    s.color = gpu_texture3(colorTexture, hit, footprintColor);
    s.scattered = !prism_is_zero(wi);
    if (withPdf) s.pdf = max(0.0f, dot(hit.n, normalize(wi))) / PRISM_PI;
    return s;
}

// Metal / MetalGltf: perturbed mirror reflection.
PRISM_INLINE BsdfSample gpu_glossy_sample(IN(Hit) hit, float roughness, float3 color, INOUT(uint) seed)
{
    BsdfSample s = gpu_bsdf_sample_empty();
    s.wi = normalize(normalize(gpu_reflect(hit.d, hit.n)) + roughness * gpu_random_unit_vector(seed));
    s.color = color;
    s.scattered = !prism_is_zero(s.wi);
    return s;
}

// Glass::sample_direction with the given relative IOR.
PRISM_INLINE float3 gpu_dielectric_direction(IN(Hit) hit, float ior, INOUT(uint) seed)
{
    float relative = hit.front_face ? 1.0f / ior : ior;
    float3 unitDirection = normalize(hit.d);
    float cosTheta = min(dot(-unitDirection, hit.n), 1.0f);
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    if (relative * sinTheta > 1.0f || gpu_schlick(cosTheta, relative) > prism_rnd(seed))
        return gpu_reflect(unitDirection, hit.n);
    return gpu_refract(unitDirection, hit.n, relative);
}

PRISM_INLINE BsdfSample gpu_legacy_sample(IN(Hit) hit, IN(GpuMaterial) material, INOUT(uint) seed)
{
    uint kind = material.info.x;
    if (kind == PRISM_MATERIAL_DIFFUSE) return gpu_diffuse_sample(hit, material.textures.x, true, true, seed);
    if (kind == PRISM_MATERIAL_FALLBACK) {
        float opacity = material.params.x;
        if (opacity >= 1.0f) return gpu_diffuse_sample(hit, material.textures.x, true, true, seed);
        if (opacity <= 0.0f || prism_rnd(seed) >= opacity) return gpu_null_sample(hit);
        return gpu_diffuse_sample(hit, material.textures.x, true, true, seed);
    }
    if (kind == PRISM_MATERIAL_METAL) {
        float roughness = gpu_texture1(material.textures.y, hit, true);
        BsdfSample s = gpu_glossy_sample(hit, roughness, splat3(0.0f), seed);
        s.color = gpu_texture3(material.textures.x, hit, true);
        return s;
    }
    if (kind == PRISM_MATERIAL_EMISSIVE) {
        BsdfSample s = gpu_bsdf_sample_empty();
        s.emission = gpu_texture3(material.textures.x, hit, true) * material.params.x;
        return s;
    }
    if (kind == PRISM_MATERIAL_GLASS) {
        BsdfSample s = gpu_bsdf_sample_empty();
        s.wi = gpu_dielectric_direction(hit, material.params.x, seed);
        s.color = splat3(1.0f);
        s.scattered = !prism_is_zero(s.wi);
        s.delta = true;
        return s;
    }
    if (kind == PRISM_MATERIAL_DISPERSIVE) {
        BsdfSample s = gpu_bsdf_sample_empty();
        float choice = prism_rnd(seed);
        float ior;
        if (choice < 0.333f) { ior = material.params.z; s.color = float3(3.0f, 0.0f, 0.0f); }
        else if (choice < 0.666f) { ior = material.params.x; s.color = float3(0.0f, 3.0f, 0.0f); }
        else { ior = material.params.y; s.color = float3(0.0f, 0.0f, 3.0f); }
        s.wi = gpu_dielectric_direction(hit, ior, seed);
        s.scattered = true;
        s.delta = true;
        return s;
    }
    if (kind == PRISM_MATERIAL_DIFFUSE_GLTF) return gpu_diffuse_sample(hit, material.textures.x, false, false, seed);
    if (kind == PRISM_MATERIAL_METAL_GLTF) {
        float roughness = gpu_texture3(material.textures.y, hit, true).y;
        BsdfSample s = gpu_glossy_sample(hit, roughness * roughness, splat3(0.0f), seed);
        s.color = gpu_texture3(material.textures.x, hit, false);
        return s;
    }
    if (kind == PRISM_MATERIAL_SPECULAR_BTDF_GLTF) {
        BsdfSample s = gpu_bsdf_sample_empty();
        s.wi = gpu_dielectric_direction(hit, material.params.x, seed);
        s.color = gpu_texture3(material.textures.x, hit, false);
        s.scattered = !prism_is_zero(s.wi);
        return s;
    }
    if (kind == PRISM_MATERIAL_GLTF_PBR) {
        // Lobe selection; lobe emission is zero and gltfPBR adds none here.
        float probability = prism_rnd(seed);
        if (probability <= gpu_texture3(material.textures.y, hit, true).z) {
            float roughness = gpu_texture3(material.textures.y, hit, true).y;
            BsdfSample s = gpu_glossy_sample(hit, roughness * roughness, splat3(0.0f), seed);
            s.color = gpu_texture3(material.textures.x, hit, false);
            return s;
        }
        if (probability <= gpu_texture1(material.textures.z, hit, true)) {
            BsdfSample s = gpu_bsdf_sample_empty();
            s.wi = gpu_dielectric_direction(hit, 1.5f, seed);
            s.color = gpu_texture3(material.textures.x, hit, false);
            s.scattered = !prism_is_zero(s.wi);
            return s;
        }
        return gpu_diffuse_sample(hit, material.textures.x, true, true, seed);
    }
    return gpu_invalid_sample();
}

// ---------------------------------------------------------------------------
// MDL materials (mdl_material.h)
// ---------------------------------------------------------------------------

struct MaterialContext {
    MdlState state;
};

// MDLMaterial::build_shading_state
PRISM_INLINE void gpu_build_mdl_state(IN(Hit) hit, IN(GpuMaterial) material, OUT(MdlState) state)
{
    state.text_coords[0].val = float3(hit.u, hit.v, 0.0f);
    bool filtered = hit.texture_footprint.filtered;
    state.text_coords[0].dx = filtered ? float3(hit.texture_footprint.dudx, hit.texture_footprint.dvdx, 0.0f)
                                       : splat3(0.0f);
    state.text_coords[0].dy = filtered ? float3(hit.texture_footprint.dudy, hit.texture_footprint.dvdy, 0.0f)
                                       : splat3(0.0f);
    float3 ng = prism_is_zero(hit.n_g) ? hit.n : hit.n_g;
    float3 n = hit.n;
    if (dot(n, ng) < 0.0f) n = -n;
    float3 t = splat3(0.0f);
    bool haveTangent = !prism_is_zero(hit.tangent);
    if (haveTangent) {
        float3 projected = hit.tangent - dot(n, hit.tangent) * n;
        if (dot(projected, projected) < 1e-12f) haveTangent = false;
        else t = normalize(projected);
    }
    if (!haveTangent) {
        if (abs(n.x) > abs(n.z)) t = normalize(float3(-n.y, n.x, 0.0f));
        else t = normalize(float3(0.0f, -n.z, n.y));
    }
    float3 b = cross(n, t);
    if (hit.bitangent_sign < 0.0f) b = -b;
    state.tangent_u[0] = t;
    state.tangent_v[0] = b;
    state.position.val = hit.p;
    state.position.dx = hit.dpdx;
    state.position.dy = hit.dpdy;
    state.normal = n;
    state.geom_normal = ng;
    state.animation_time = 0.0f;
    for (int i = 0; i < PRISM_MDL_TEXTURE_RESULTS; ++i) state.text_results[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    state.ro_data_segment_offset = 0;
    state.world_to_object = PRISM_IDENTITY_MATRIX;
    state.object_to_world = PRISM_IDENTITY_MATRIX;
    state.object_id = 0;
    state.meters_per_scene_unit = 1.0f;
    state.arg_block_offset = int(material.info.z);
}

PRISM_INLINE float gpu_mdl_opacity(IN(GpuMaterial) material, INOUT(MdlState) state)
{
    if ((material.info.w & PRISM_MATERIAL_CONSTANT_CUTOUT) != 0u) return material.params.w;
    float opacity = prism_mdl_cutout(material.info.y, state);
    return prism_isfinite(opacity) ? clamp(opacity, 0.0f, 1.0f) : 1.0f;
}

PRISM_INLINE float3 gpu_mdl_emission(IN(GpuMaterial) material, INOUT(MdlState) state, float3 wo, bool backface)
{
    if (backface && !prism_mdl_thin_walled(material.info.y, state)) return splat3(0.0f);
    float3 intensity = prism_mdl_emission_intensity(material.info.y, backface, state);
    if (intensity.x <= 0.0f && intensity.y <= 0.0f && intensity.z <= 0.0f) return splat3(0.0f);
    Edf_evaluate_data data;
    data.k1 = wo;
    data.cos = 0.0f;
    data.edf = splat3(0.0f);
    data.pdf = 0.0f;
    if (!prism_mdl_edf_evaluate(material.info.y, backface, data, state)) return splat3(0.0f);
    return intensity * data.edf;
}

// MDLMaterial::sampleWithState
PRISM_INLINE BsdfSample gpu_mdl_sample(IN(Hit) hit, IN(GpuMaterial) material, INOUT(MdlState) state,
                                       INOUT(uint) seed)
{
    bool backface = !hit.front_face;
    float3 wo = -normalize(hit.d);
    float opacity = gpu_mdl_opacity(material, state);
    if (opacity <= 0.0f || (opacity < 1.0f && prism_rnd(seed) >= opacity)) return gpu_null_sample(hit);
    bool thinWalled = prism_mdl_thin_walled(material.info.y, state);
    Bsdf_sample_data data;
    if (!backface || thinWalled) {
        data.ior1 = splat3(1.0f);
        data.ior2 = splat3(-1.0f);
    } else {
        data.ior1 = splat3(-1.0f);
        data.ior2 = splat3(1.0f);
    }
    data.k1 = wo;
    data.k2 = splat3(0.0f);
    data.xi.x = prism_rnd(seed);
    data.xi.y = prism_rnd(seed);
    data.xi.z = prism_rnd(seed);
    data.xi.w = prism_rnd(seed);
    data.pdf = 0.0f;
    data.bsdf_over_pdf = splat3(0.0f);
    data.event_type = 0;
    data.handle = 0;
    data.flags = 0;
    // A missing DF is a zero BSDF: the data stays an absorption event.
    prism_mdl_bsdf_sample(material.info.y, backface && thinWalled, data, state);
    BsdfSample s = gpu_bsdf_sample_empty();
    if (data.event_type != 0) {
        s.scattered = true;
        s.wi = data.k2;
        s.color = data.bsdf_over_pdf;
        s.pdf = data.pdf;
        s.delta = (data.event_type & 4) != 0;
    }
    s.emission = gpu_mdl_emission(material, state, wo, backface);
    return s;
}

// MDLMaterial::evaluateWithState
PRISM_INLINE BsdfEvaluation gpu_mdl_evaluate(IN(Hit) hit, IN(GpuMaterial) material, INOUT(MdlState) state,
                                             float3 wi)
{
    BsdfEvaluation result;
    result.value = splat3(0.0f);
    result.pdf = 0.0f;
    result.valid = false;
    bool backface = !hit.front_face;
    bool thinWalled = prism_mdl_thin_walled(material.info.y, state);
    Bsdf_evaluate_data data;
    data.k1 = -normalize(hit.d);
    data.k2 = normalize(wi);
    if (!backface || thinWalled) {
        data.ior1 = splat3(1.0f);
        data.ior2 = splat3(-1.0f);
    } else {
        data.ior1 = splat3(-1.0f);
        data.ior2 = splat3(1.0f);
    }
    data.bsdf_diffuse = splat3(0.0f);
    data.bsdf_glossy = splat3(0.0f);
    data.pdf = 0.0f;
    data.flags = 0;
    if (!prism_mdl_bsdf_evaluate(material.info.y, backface && thinWalled, data, state)) return result;
    result.value = data.bsdf_diffuse + data.bsdf_glossy;
    result.pdf = data.pdf;
    result.valid = result.pdf >= 0.0f && prism_isfinite(result.pdf);
    return result;
}

// ---------------------------------------------------------------------------
// Material backend interface
// ---------------------------------------------------------------------------

void backend_prepare_material(INOUT(Hit) hit, OUT(MaterialContext) context)
{
    GpuMaterial material = gpu_material(hit.material);
    // Material::perturbNormal
    hit.n_g = normalize(hit.n);
    hit.n_s = splat3(0.0f);
    if ((material.info.w & PRISM_MATERIAL_NORMAL_TEXTURE) != 0u) {
        hit.n_s = gpu_texture3(material.textures2.x, hit, true);
        if (!prism_is_zero(hit.n_s)) {
            float3 tangentNormal = hit.n_s * 2.0f - splat3(1.0f);
            float3 up = abs(hit.n_g.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            float3 t = cross(up, hit.n_g);
            float3 b = cross(hit.n_g, t);
            hit.n = normalize(t * tangentNormal.x + b * tangentNormal.y + hit.n_g * tangentNormal.z);
        }
    }
    if (material.info.x == PRISM_MATERIAL_MDL) {
        gpu_build_mdl_state(hit, material, context.state);
        prism_mdl_init(material.info.y, context.state);
    }
}

BsdfSample backend_sample_material(IN(Hit) hit, INOUT(MaterialContext) context, INOUT(uint) seed)
{
    GpuMaterial material = gpu_material(hit.material);
    if (material.info.x == PRISM_MATERIAL_MDL) return gpu_mdl_sample(hit, material, context.state, seed);
    return gpu_legacy_sample(hit, material, seed);
}

BsdfEvaluation backend_evaluate_material(IN(Hit) hit, float3 wi, INOUT(MaterialContext) context)
{
    GpuMaterial material = gpu_material(hit.material);
    if (material.info.x == PRISM_MATERIAL_MDL) return gpu_mdl_evaluate(hit, material, context.state, wi);
    BsdfEvaluation e;
    e.value = splat3(0.0f);
    e.pdf = 0.0f;
    e.valid = false;
    if (material.info.x == PRISM_MATERIAL_DIFFUSE || material.info.x == PRISM_MATERIAL_FALLBACK) {
        // Diffuse::evaluate
        float cosine = max(0.0f, dot(hit.n, normalize(wi)));
        e.value = gpu_texture3(material.textures.x, hit, true) * (cosine / PRISM_PI);
        e.pdf = cosine / PRISM_PI;
        e.valid = cosine > 0.0f;
    }
    return e;
}

bool backend_supports_nee(IN(Hit) hit, INOUT(MaterialContext) context)
{
    uint kind = gpu_material(hit.material).info.x;
    return kind == PRISM_MATERIAL_MDL || kind == PRISM_MATERIAL_DIFFUSE || kind == PRISM_MATERIAL_FALLBACK;
}

bool backend_proven_opaque(IN(Hit) hit)
{
    GpuMaterial material = gpu_material(hit.material);
    if (material.info.x == PRISM_MATERIAL_FALLBACK) return material.params.x >= 1.0f;
    if (material.info.x == PRISM_MATERIAL_MDL)
        return (material.info.w & PRISM_MATERIAL_CONSTANT_CUTOUT) != 0u && material.params.w >= 1.0f;
    return false;
}

float backend_cutout_opacity(IN(Hit) hit)
{
    GpuMaterial material = gpu_material(hit.material);
    if (material.info.x == PRISM_MATERIAL_FALLBACK) return material.params.x;
    if (material.info.x != PRISM_MATERIAL_MDL) return 1.0f;
    if ((material.info.w & PRISM_MATERIAL_CONSTANT_CUTOUT) != 0u) return material.params.w;
    // Only programs with a varying cutout get here; their init has its own
    // dispatch so the full material set is not inlined a second time.
    MdlState state;
    gpu_build_mdl_state(hit, material, state);
    prism_mdl_init_cutout(material.info.y, state);
    float opacity = prism_mdl_cutout(material.info.y, state);
    return prism_isfinite(opacity) ? clamp(opacity, 0.0f, 1.0f) : 1.0f;
}

// ---------------------------------------------------------------------------
// Environment (environment.h, hydra/environment_cache.h)
// ---------------------------------------------------------------------------

PRISM_INLINE float3 gpu_row_transform(IN(float4) r0, IN(float4) r1, IN(float4) r2, float3 v)
{
    return float3(dot(gpu_xyz(r0), v), dot(gpu_xyz(r1), v), dot(gpu_xyz(r2), v));
}

PRISM_INLINE float3 gpu_env_to_local(IN(GpuEnvironment) env, float3 v)
{
    return gpu_row_transform(env.toLocal[0], env.toLocal[1], env.toLocal[2], v);
}

PRISM_INLINE float gpu_texel_solid_angle(uint y, uint width, uint height)
{
    return (4.0f * PRISM_PI / float(width)) * sin(PRISM_PI * (float(y) + 0.5f) / float(height)) *
        sin(PRISM_PI / (2.0f * float(height)));
}

// EnvironmentMap::radiance in the map's local frame.
PRISM_INLINE float3 gpu_latlong_radiance(IN(GpuEnvironment) env, float3 direction, float angle)
{
    float3 d = normalize(direction);
    float lon = prism_atan2(d.z, d.x);
    if (lon < 0.0f) lon += 2.0f * PRISM_PI;
    float lat = prism_atan2(prism_hypot(d.x, d.z), d.y);
    float3 result;
    if (env.info.y != PRISM_IMAGE_NONE) {
        float s = lon / (2.0f * PRISM_PI);
        float t = lat / PRISM_PI;
        float2 dx = float2(0.0f, 0.0f);
        float2 dy = float2(0.0f, 0.0f);
        if (angle > 0.0f && prism_isfinite(angle)) {
            float3 tangent = normalize(cross(abs(d.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f)
                                                               : float3(1.0f, 0.0f, 0.0f), d));
            float3 bitangent = cross(d, tangent);
            for (int k = 0; k < 2; ++k) {
                float3 q = normalize(d + angle * (k == 0 ? tangent : bitangent));
                float qlon = prism_atan2(q.z, q.x);
                if (qlon < 0.0f) qlon += 2.0f * PRISM_PI;
                float ds = qlon / (2.0f * PRISM_PI) - s;
                ds -= prism_round(ds);
                float dt = prism_atan2(prism_hypot(q.x, q.z), q.y) / PRISM_PI - t;
                if (k == 0) dx = float2(ds, dt);
                else dy = float2(ds, dt);
            }
            if (prism_hypot(d.x, d.z) < angle) {
                dx = float2(0.5f, 0.0f);
                dy = float2(0.0f, angle / PRISM_PI);
            }
        }
        if (!gpu_filtered()) {
            dx = float2(0.0f, 0.0f);
            dy = float2(0.0f, 0.0f);
        }
        result = gpu_xyz(env.multiplier) * gpu_xyz(gpu_image_sample(env.info.y, float2(s, t), dx, dy, 1u, 0u));
    } else {
        result = gpu_xyz(env.multiplier) * gpu_xyz(env.color0);
    }
    if (!prism_isfinite(result.x) || result.x < 0.0f) result.x = 0.0f;
    if (!prism_isfinite(result.y) || result.y < 0.0f) result.y = 0.0f;
    if (!prism_isfinite(result.z) || result.z < 0.0f) result.z = 0.0f;
    return result;
}

PRISM_INLINE float3 gpu_environment_radiance(IN(GpuEnvironment) env, float3 world, float angle)
{
    uint kind = env.info.x;
    if (kind == PRISM_ENV_CONSTANT) return gpu_xyz(env.color0);
    if (kind == PRISM_ENV_GRADIENT) {
        float3 d = normalize(world);
        float t = 0.5f * (d.y + 1.0f);
        return (1.0f - t) * gpu_xyz(env.color0) + t * gpu_xyz(env.color1);
    }
    if (kind == PRISM_ENV_CYLINDRICAL) {
        float3 d = normalize(world);
        float lon = prism_atan2(d.z, d.x);
        float lat = acos(d.y);
        lon = lon < 0.0f ? lon + (2.0f * PRISM_PI) : lon;
        float u = 1.0f - (lon / (2.0f * PRISM_PI));
        float v = lat / PRISM_PI;
        if (env.info.y == PRISM_IMAGE_NONE) return 2.0f * gpu_xyz(env.color0);
        GpuImage img = gpu_image(env.info.y);
        float i = abs(u - trunc(u)) * float(img.info.x - 1u);
        float j = abs(v - trunc(v)) * float(img.info.y - 1u);
        return 2.0f * gpu_xyz(gpu_image_bilinear_black(env.info.y, i / float(img.info.x),
                                                       j / float(img.info.y)));
    }
    if ((env.info.w & PRISM_ENV_TRANSFORMED) == 0u) return gpu_latlong_radiance(env, world, angle);
    float3 local = normalize(gpu_env_to_local(env, world));
    float transformedAngle = angle;
    if (angle > 0.0f) {
        float3 d = normalize(world);
        float3 t = normalize(cross(abs(d.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f), d));
        float3 b = cross(d, t);
        transformedAngle = max(length(normalize(gpu_env_to_local(env, d + angle * t)) - local),
                               length(normalize(gpu_env_to_local(env, d + angle * b)) - local));
    }
    return gpu_latlong_radiance(env, local, transformedAngle);
}

// 32x32 -> 64-bit product as (low, high) words.
PRISM_INLINE void gpu_mul_wide(uint a, uint b, OUT(uint) lo, OUT(uint) hi)
{
    uint a0 = a & 0xffffu;
    uint a1 = a >> 16u;
    uint b0 = b & 0xffffu;
    uint b1 = b >> 16u;
    uint p00 = a0 * b0;
    uint p01 = a0 * b1;
    uint p10 = a1 * b0;
    uint p11 = a1 * b1;
    uint middle = (p00 >> 16u) + (p01 & 0xffffu) + (p10 & 0xffffu);
    lo = (p00 & 0xffffu) | (middle << 16u);
    hi = p11 + (p01 >> 16u) + (p10 >> 16u) + (middle >> 16u);
}

// EnvironmentMap::sampleLocalDirection
PRISM_INLINE EnvironmentSample gpu_latlong_sample_local(IN(GpuEnvironment) env, INOUT(uint) seed)
{
    EnvironmentSample s;
    s.radiance = splat3(0.0f);
    uint width = env.info.z;
    uint height = env.info2.x;
    if (width == 0u) {
        float y = 1.0f - 2.0f * prism_rnd(seed);
        float phi = 2.0f * PRISM_PI * prism_rnd(seed);
        float r = sqrt(max(0.0f, 1.0f - y * y));
        s.wi = float3(r * cos(phi), y, r * sin(phi));
        s.pdf = 1.0f / (4.0f * PRISM_PI);
        return s;
    }
    uint n = width * height;
    // x = (r1 + r2 / 2^24) * n with r1, r2 24-bit fractions, evaluated in
    // fixed point (the CPU uses double precision here).
    float r1 = prism_rnd(seed);
    float r2 = prism_rnd(seed);
    uint k1 = uint(r1 * 16777216.0f);
    uint k2 = uint(r2 * 16777216.0f);
    uint lo;
    uint hi;
    gpu_mul_wide(k1, n, lo, hi);
    uint integerPart = (hi << 8u) | (lo >> 24u);
    float fraction = float(lo & 0xffffffu) / 16777216.0f;
    gpu_mul_wide(k2, n, lo, hi);
    fraction += (float(hi) * 4294967296.0f + float(lo)) / 281474976710656.0f;
    float carry = floor(fraction);
    uint column = min(integerPart + uint(carry), n - 1u);
    fraction -= carry;
    uint index = fraction < gpu_env_alias_probability(column) ? column : gpu_env_alias(column);
    uint px = index % width;
    uint py = index / width;
    float phi = 2.0f * PRISM_PI * (float(px) + prism_rnd(seed)) / float(width);
    float theta0 = PRISM_PI * float(py) / float(height);
    float theta1 = PRISM_PI * float(py + 1u) / float(height);
    float r = prism_rnd(seed);
    // cos(theta) between the rows' bounds, kept accurate near both poles.
    float s0 = sin(0.5f * theta0);
    float s1 = sin(0.5f * theta1);
    float c0 = cos(0.5f * theta0);
    float c1 = cos(0.5f * theta1);
    float oneMinusCos = prism_lerp(2.0f * s0 * s0, 2.0f * s1 * s1, r);
    float onePlusCos = prism_lerp(2.0f * c0 * c0, 2.0f * c1 * c1, r);
    float cosTheta = 0.5f * (onePlusCos - oneMinusCos);
    float sinTheta = sqrt(max(0.0f, oneMinusCos * onePlusCos));
    s.wi = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    s.pdf = gpu_env_texel_pdf(index) / gpu_texel_solid_angle(py, width, height);
    return s;
}

PRISM_INLINE float gpu_latlong_pdf_local(IN(GpuEnvironment) env, float3 direction)
{
    uint width = env.info.z;
    uint height = env.info2.x;
    if (width == 0u) return 1.0f / (4.0f * PRISM_PI);
    float3 d = normalize(direction);
    float phi = prism_atan2(d.z, d.x);
    if (phi < 0.0f) phi += 2.0f * PRISM_PI;
    uint x = min(uint(int(phi * float(width) / (2.0f * PRISM_PI))), width - 1u);
    uint y = min(uint(int(prism_atan2(prism_hypot(d.x, d.z), d.y) * float(height) / PRISM_PI)), height - 1u);
    return gpu_env_texel_pdf(y * width + x) / gpu_texel_solid_angle(y, width, height);
}

PRISM_INLINE float gpu_env_jacobian(IN(GpuEnvironment) env, float3 world)
{
    float len = length(gpu_env_to_local(env, world));
    return len > 0.0f ? abs(env.multiplier.w) / (len * len * len) : 0.0f;
}

float3 backend_environment_radiance(float3 direction, float angularRadius)
{
    return gpu_environment_radiance(gpu_environment(), direction, angularRadius);
}

EnvironmentSample backend_environment_sample(INOUT(uint) seed, float angularRadius)
{
    GpuEnvironment env = gpu_environment();
    EnvironmentSample s;
    if (env.info.x != PRISM_ENV_LATLONG) {
        // Environment::sampleDirection: uniform sphere.
        float y = 1.0f - 2.0f * prism_rnd(seed);
        float phi = 2.0f * PRISM_PI * prism_rnd(seed);
        float r = sqrt(max(0.0f, 1.0f - y * y));
        s.wi = float3(r * cos(phi), y, r * sin(phi));
        s.pdf = 1.0f / (4.0f * PRISM_PI);
    } else {
        s = gpu_latlong_sample_local(env, seed);
        if ((env.info.w & PRISM_ENV_TRANSFORMED) != 0u) {
            s.wi = normalize(gpu_row_transform(env.toWorld[0], env.toWorld[1], env.toWorld[2], s.wi));
            s.pdf *= gpu_env_jacobian(env, s.wi);
        }
    }
    s.radiance = gpu_environment_radiance(env, s.wi, angularRadius);
    return s;
}

float backend_environment_pdf(float3 direction)
{
    GpuEnvironment env = gpu_environment();
    if (env.info.x != PRISM_ENV_LATLONG) return 1.0f / (4.0f * PRISM_PI);
    if ((env.info.w & PRISM_ENV_TRANSFORMED) == 0u) return gpu_latlong_pdf_local(env, direction);
    float3 transformed = gpu_env_to_local(env, direction);
    if (prism_is_zero(transformed)) return 0.0f;
    return gpu_latlong_pdf_local(env, normalize(transformed)) * gpu_env_jacobian(env, normalize(direction));
}

int backend_max_depth() { return int(gpu_frame().settings.x); }
int backend_roulette_depth() { return int(gpu_frame().settings.y); }
bool backend_environment_mis() { return (gpu_frame().settings.z & PRISM_FRAME_ENVIRONMENT_MIS) != 0u; }

#endif // PRISM_GPU_BACKEND_INC
