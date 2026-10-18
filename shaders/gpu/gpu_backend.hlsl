// The GPU backend of the shared path tracer (shaders/integrator/path_tracer.h),
// for the Metal and Vulkan devices. It mirrors the CPU classes:
// SceneManager::prepareHit / prepareFootprint, the legacy Material classes,
// MDLMaterial and the Environment classes.
//
// Requires the resource accessors (resources.hlsl), gpu_trace (geometry.hlsl)
// and the MDL dispatch functions (prism_mdl_*) of the generated code.

#pragma once

float3 gpu_xyz(float4 v) { return float3(v.x, v.y, v.z); }

// Row-vector 3x3 transform stored as columns.
float3 gpu_transform(float3 v, float4 c0, float4 c1, float4 c2)
{
    return float3(dot(v, gpu_xyz(c0)), dot(v, gpu_xyz(c1)), dot(v, gpu_xyz(c2)));
}

bool gpu_filtered() { return (gpu_frame().settings.z & kFrameFiltered) != 0u; }

float3 gpu_reflect(float3 i, float3 n) { return i - 2.0f * dot(i, n) * n; }

// ---------------------------------------------------------------------------
// Hits
// ---------------------------------------------------------------------------

SurfaceHit gpu_empty_hit(PathRay ray)
{
    SurfaceHit h;
    h.valid = false;
    h.o = ray.ray.origin;
    h.d = ray.ray.direction;
    h.p = h.o;
    h.n = (float3)0.0f;
    h.n_g = (float3)0.0f;
    h.tangent = (float3)0.0f;
    h.bitangent_sign = 1.0f;
    h.u = 0.0f;
    h.v = 0.0f;
    h.front_face = false;
    h.t = ray.ray.tfar;
    h.instance = 0u;
    h.primitive = 0u;
    h.ray_footprint = ray.footprint;
    h.texture_footprint.dudx = 0.0f;
    h.texture_footprint.dvdx = 0.0f;
    h.texture_footprint.dudy = 0.0f;
    h.texture_footprint.dvdy = 0.0f;
    h.texture_footprint.valid = false;
    h.texture_footprint.fallback = false;
    h.texture_footprint.filtered = false;
    h.dpdx = (float3)0.0f;
    h.dpdy = (float3)0.0f;
    return h;
}

float3 gpu_plane_differential(SurfaceHit hit, float3 dO, float3 dD, float predicted)
{
    float3 o = hit.o + dO;
    float3 d = hit.d + dD;
    float denominator = dot(d, hit.n_g);
    if (abs(denominator) < 1.0e-6f) return (float3)0.0f;
    float distance = dot(hit.p - o, hit.n_g) / denominator;
    float3 value = o + distance * d - hit.p;
    float len = length(value);
    if (!isfinite(len) || len > predicted * 8.0f) return (float3)0.0f;
    return value;
}

// SceneManager::prepareFootprint
void gpu_prepare_footprint(inout SurfaceHit hit, GpuPrototype proto, GpuInstance inst,
                                        uint localPrimitive)
{
    float predicted = max(1.0e-8f, hit.ray_footprint.width + max(0.0f, hit.t) * hit.ray_footprint.spread);
    if (hit.ray_footprint.exact) {
        hit.dpdx = gpu_plane_differential(hit, hit.ray_footprint.dOdx, hit.ray_footprint.dDdx, predicted);
        hit.dpdy = gpu_plane_differential(hit, hit.ray_footprint.dOdy, hit.ray_footprint.dDdy, predicted);
    }
    if (isZero(hit.dpdx) || isZero(hit.dpdy)) {
        float3 axis = abs(hit.n_g.x) < 0.8f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
        float3 x = normalize(cross(axis, hit.n_g));
        float3 y = normalize(cross(hit.n_g, x));
        hit.dpdx = predicted * x;
        hit.dpdy = predicted * y;
        hit.texture_footprint.fallback = true;
    }

    float3 dpdu = (float3)0.0f;
    float3 dpdv = (float3)0.0f;
    bool spherePole = false;
    if (proto.info.x == kProtoTriangles) {
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
    } else if (proto.info.x == kProtoSpheres) {
        float4 sphere = gpu_sphere(proto.info.z + localPrimitive);
        float radius = sphere.w;
        if (!(radius > 0.0f) || !isfinite(radius)) {
            spherePole = true;
        } else {
            float theta = kPi - 2.0f * kPi * hit.u;
            float phi = kPi * hit.v;
            float sinPhi = sin(phi);
            float cosPhi = cos(phi);
            spherePole = abs(sinPhi) < 1.0e-4f;
            dpdu = 2.0f * kPi * radius * float3(sinPhi * sin(theta), 0.0f, -sinPhi * cos(theta));
            dpdv = kPi * radius * float3(cosPhi * cos(theta), -sinPhi, cosPhi * sin(theta));
        }
    }
    if ((inst.info.y & kInstancePlacement) != 0u) {
        dpdu = gpu_transform(dpdu, inst.tangentColumns[0], inst.tangentColumns[1], inst.tangentColumns[2]);
        dpdv = gpu_transform(dpdv, inst.tangentColumns[0], inst.tangentColumns[1], inst.tangentColumns[2]);
    }
    float a = dot(dpdu, dpdu);
    float b = dot(dpdu, dpdv);
    float c = dot(dpdv, dpdv);
    float determinant = a * c - b * b;
    hit.texture_footprint.filtered = gpu_filtered();
    if (determinant > 1.0e-20f && isfinite(determinant)) {
        float x = dot(hit.dpdx, dpdu);
        float y = dot(hit.dpdx, dpdv);
        hit.texture_footprint.dudx = (c * x - b * y) / determinant;
        hit.texture_footprint.dvdx = (a * y - b * x) / determinant;
        x = dot(hit.dpdy, dpdu);
        y = dot(hit.dpdy, dpdv);
        hit.texture_footprint.dudy = (c * x - b * y) / determinant;
        hit.texture_footprint.dvdy = (a * y - b * x) / determinant;
        hit.texture_footprint.valid =
            isfinite(hit.texture_footprint.dudx) && isfinite(hit.texture_footprint.dvdx) &&
            isfinite(hit.texture_footprint.dudy) && isfinite(hit.texture_footprint.dvdy);
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

// A hit and the material of its primitive.
struct GpuHit {
    SurfaceHit surface;
    uint material;
};

// SceneManager::cast_ray: intersection plus RayHit construction and the
// placement handling of prepareHit.
GpuHit gpu_intersect(PathRay ray, uint mask)
{
    GpuHit result;
    result.material = 0u;
    SurfaceHit hit = gpu_empty_hit(ray);
    GpuRawHit raw = gpu_trace(ray.ray, mask);
    if (!raw.hit) {
        result.surface = hit;
        return result;
    }
    GpuInstance inst = gpu_instance(raw.instance);
    GpuPrototype proto = gpu_prototype(inst.info.x);
    bool placement = (inst.info.y & kInstancePlacement) != 0u;
    bool flip = (inst.info.y & kInstanceFlip) != 0u;
    uint type = proto.info.x;
    uint localPrimitive = type == kProtoQuads ? raw.primitive / 2u : raw.primitive;

    hit.valid = true;
    hit.t = raw.t;
    hit.p = hit.o + hit.t * hit.d;
    hit.instance = inst.info.w;
    hit.primitive = localPrimitive;
    result.material = gpu_primitive_material(inst.info.z + localPrimitive);

    // Geometric normal in the prototype's space (Embree conventions).
    float3 ng = (float3)0.0f;
    uint i0 = 0u;
    uint i1 = 0u;
    uint i2 = 0u;
    float4 sphere = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (type == kProtoSpheres) {
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
    if (type == kProtoTriangles) {
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
    } else if (type == kProtoSpheres) {
        // Texture coordinates must not change when viewed from inside.
        float3 outward = placement ? normalize(localNormal) : (hit.front_face ? hit.n : -hit.n);
        float theta = atan2(outward.z, outward.x);
        float phi = acos(outward.y);
        hit.u = 1.0f - ((theta + kPi) / (2.0f * kPi));
        hit.v = phi / kPi;
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
        if (type == kProtoTriangles)
            hit.n = normalize(gpu_transform(hit.n, inst.normalColumns[0], inst.normalColumns[1],
                                            inst.normalColumns[2]));
        if (!isZero(hit.tangent))
            hit.tangent = normalize(gpu_transform(hit.tangent, inst.tangentColumns[0],
                                                  inst.tangentColumns[1], inst.tangentColumns[2]));
        if (flip) hit.n = -hit.n;
        if ((inst.info.y & kInstanceMirrored) != 0u) hit.bitangent_sign *= -1.0f;
    }
    gpu_prepare_footprint(hit, proto, inst, localPrimitive);
    result.surface = hit;
    return result;
}

// ---------------------------------------------------------------------------
// Images and legacy textures
// ---------------------------------------------------------------------------

// ImageTexture::sample without a footprint: the nearest texel.
float4 gpu_image_nearest(uint image, float u, float v)
{
    GpuImage img = gpu_image(image);
    int i = int(abs(u - trunc(u)) * float(img.info.x - 1u));
    int j = int(abs(v - trunc(v)) * float(img.info.y - 1u));
    return gpu_image_texel(image, i, j);
}

// OIIO bilinear lookup at (s, t) with black outside the image.
float4 gpu_image_bilinear_black(uint image, float s, float t)
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
float4 gpu_legacy_texture_sample(uint ref, float u, float v, bool useFootprint,
                                              TextureFootprint footprint)
{
    if (ref == kTextureNone) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    GpuLegacyTexture tex = gpu_legacy_texture(ref);
    if (tex.info.x == kTextureValue) return tex.value;
    float4 value;
    if (!useFootprint || !footprint.filtered)
        value = gpu_image_nearest(tex.info.y, u, v);
    else
        value = gpu_image_sample(tex.info.y, float2(u, v), float2(footprint.dudx, footprint.dvdx),
                                 float2(footprint.dudy, footprint.dvdy), 1u, 1u);
    if (tex.info.x == kTextureImageMix) value = value * tex.value;
    return value;
}

float3 gpu_texture3(uint ref, SurfaceHit hit, bool useFootprint)
{
    return gpu_xyz(gpu_legacy_texture_sample(ref, hit.u, hit.v, useFootprint, hit.texture_footprint));
}

float gpu_texture1(uint ref, SurfaceHit hit, bool useFootprint)
{
    return gpu_legacy_texture_sample(ref, hit.u, hit.v, useFootprint, hit.texture_footprint).x;
}

// ---------------------------------------------------------------------------
// Legacy materials (material.h, hydra/fallback_material.h)
// ---------------------------------------------------------------------------

float3 gpu_random_unit_vector(inout Rng rng)
{
    float a = rng.next() * (2.0f * kPi);
    float z = (2.0f * rng.next()) - 1.0f;
    float r = sqrt(1.0f - (z * z));
    return float3(r * cos(a), r * sin(a), z);
}

bool gpu_is_near_zero(float3 v)
{
    return abs(v.x) < 1e-8f && abs(v.y) < 1e-8f && abs(v.z) < 1e-8f;
}

float3 gpu_refract(float3 wi, float3 n, float ior)
{
    float cosTheta = min(dot(-wi, n), 1.0f);
    float3 r1 = ior * (wi + cosTheta * n);
    float3 r2 = -sqrt(abs(1.0f - dot(r1, r1))) * n;
    return r1 + r2;
}

float gpu_schlick(float cosine, float ior)
{
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    return r0 + (1.0f - r0) * pow(1.0f - cosine, 5.0f);
}

BsdfSample gpu_bsdf_sample_empty()
{
    BsdfSample s;
    s.wi = (float3)0.0f;
    s.color = (float3)0.0f;
    s.emission = (float3)0.0f;
    s.scattered = false;
    s.pdf = 0.0f;
    s.delta = false;
    s.null_event = false;
    return s;
}

BsdfSample gpu_null_sample(SurfaceHit hit)
{
    BsdfSample s = gpu_bsdf_sample_empty();
    s.wi = hit.d;
    s.color = (float3)1.0f;
    s.scattered = true;
    s.delta = true;
    s.null_event = true;
    return s;
}

BsdfSample gpu_invalid_sample()
{
    BsdfSample s = gpu_bsdf_sample_empty();
    s.emission = float3(1.0f, 0.0f, 1.0f);
    return s;
}

// Diffuse::sample (cosine-like lobe n + random unit vector).
BsdfSample gpu_diffuse_sample(SurfaceHit hit, uint colorTexture, bool footprintColor,
                                           bool withPdf, inout Rng rng)
{
    BsdfSample s = gpu_bsdf_sample_empty();
    float3 wi = hit.n + gpu_random_unit_vector(rng);
    if (gpu_is_near_zero(wi)) wi = hit.n;
    s.wi = wi;
    s.color = gpu_texture3(colorTexture, hit, footprintColor);
    s.scattered = !isZero(wi);
    if (withPdf) s.pdf = max(0.0f, dot(hit.n, normalize(wi))) / kPi;
    return s;
}

// Metal / MetalGltf: perturbed mirror reflection.
BsdfSample gpu_glossy_sample(SurfaceHit hit, float roughness, float3 color, inout Rng rng)
{
    BsdfSample s = gpu_bsdf_sample_empty();
    s.wi = normalize(normalize(gpu_reflect(hit.d, hit.n)) + roughness * gpu_random_unit_vector(rng));
    s.color = color;
    s.scattered = !isZero(s.wi);
    return s;
}

// Glass::sample_direction with the given relative IOR.
float3 gpu_dielectric_direction(SurfaceHit hit, float ior, inout Rng rng)
{
    float relative = hit.front_face ? 1.0f / ior : ior;
    float3 unitDirection = normalize(hit.d);
    float cosTheta = min(dot(-unitDirection, hit.n), 1.0f);
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    if (relative * sinTheta > 1.0f || gpu_schlick(cosTheta, relative) > rng.next())
        return gpu_reflect(unitDirection, hit.n);
    return gpu_refract(unitDirection, hit.n, relative);
}

BsdfSample gpu_legacy_sample(SurfaceHit hit, GpuMaterial material, inout Rng rng)
{
    uint kind = material.info.x;
    if (kind == kMaterialDiffuse) return gpu_diffuse_sample(hit, material.textures.x, true, true, rng);
    if (kind == kMaterialFallback) {
        float opacity = material.params.x;
        if (opacity >= 1.0f) return gpu_diffuse_sample(hit, material.textures.x, true, true, rng);
        if (opacity <= 0.0f || rng.next() >= opacity) return gpu_null_sample(hit);
        return gpu_diffuse_sample(hit, material.textures.x, true, true, rng);
    }
    if (kind == kMaterialMetal) {
        float roughness = gpu_texture1(material.textures.y, hit, true);
        BsdfSample s = gpu_glossy_sample(hit, roughness, (float3)0.0f, rng);
        s.color = gpu_texture3(material.textures.x, hit, true);
        return s;
    }
    if (kind == kMaterialEmissive) {
        BsdfSample s = gpu_bsdf_sample_empty();
        s.emission = gpu_texture3(material.textures.x, hit, true) * material.params.x;
        return s;
    }
    if (kind == kMaterialGlass) {
        BsdfSample s = gpu_bsdf_sample_empty();
        s.wi = gpu_dielectric_direction(hit, material.params.x, rng);
        s.color = (float3)1.0f;
        s.scattered = !isZero(s.wi);
        s.delta = true;
        return s;
    }
    if (kind == kMaterialDispersive) {
        BsdfSample s = gpu_bsdf_sample_empty();
        float choice = rng.next();
        float ior;
        if (choice < 0.333f) { ior = material.params.z; s.color = float3(3.0f, 0.0f, 0.0f); }
        else if (choice < 0.666f) { ior = material.params.x; s.color = float3(0.0f, 3.0f, 0.0f); }
        else { ior = material.params.y; s.color = float3(0.0f, 0.0f, 3.0f); }
        s.wi = gpu_dielectric_direction(hit, ior, rng);
        s.scattered = true;
        s.delta = true;
        return s;
    }
    if (kind == kMaterialDiffuseGltf) return gpu_diffuse_sample(hit, material.textures.x, false, false, rng);
    if (kind == kMaterialMetalGltf) {
        float roughness = gpu_texture3(material.textures.y, hit, true).y;
        BsdfSample s = gpu_glossy_sample(hit, roughness * roughness, (float3)0.0f, rng);
        s.color = gpu_texture3(material.textures.x, hit, false);
        return s;
    }
    if (kind == kMaterialSpecularBtdfGltf) {
        BsdfSample s = gpu_bsdf_sample_empty();
        s.wi = gpu_dielectric_direction(hit, material.params.x, rng);
        s.color = gpu_texture3(material.textures.x, hit, false);
        s.scattered = !isZero(s.wi);
        return s;
    }
    if (kind == kMaterialGltfPbr) {
        // Lobe selection; lobe emission is zero and gltfPBR adds none here.
        float probability = rng.next();
        if (probability <= gpu_texture3(material.textures.y, hit, true).z) {
            float roughness = gpu_texture3(material.textures.y, hit, true).y;
            BsdfSample s = gpu_glossy_sample(hit, roughness * roughness, (float3)0.0f, rng);
            s.color = gpu_texture3(material.textures.x, hit, false);
            return s;
        }
        if (probability <= gpu_texture1(material.textures.z, hit, true)) {
            BsdfSample s = gpu_bsdf_sample_empty();
            s.wi = gpu_dielectric_direction(hit, 1.5f, rng);
            s.color = gpu_texture3(material.textures.x, hit, false);
            s.scattered = !isZero(s.wi);
            return s;
        }
        return gpu_diffuse_sample(hit, material.textures.x, true, true, rng);
    }
    return gpu_invalid_sample();
}

// ---------------------------------------------------------------------------
// MDL materials (mdl_material.h)
// ---------------------------------------------------------------------------

// A prepared shading point: the hit after normal mapping and, for MDL
// materials, the state after the material's init function.
struct GpuShading {
    SurfaceHit surface;
    uint material;
    Shading_state_material state;
};

// MDLMaterial::build_shading_state
void gpu_build_mdl_state(SurfaceHit hit, GpuMaterial material, out Shading_state_material state)
{
    state.text_coords[0].val = float3(hit.u, hit.v, 0.0f);
    bool filtered = hit.texture_footprint.filtered;
    state.text_coords[0].dx = filtered ? float3(hit.texture_footprint.dudx, hit.texture_footprint.dvdx, 0.0f)
                                       : (float3)0.0f;
    state.text_coords[0].dy = filtered ? float3(hit.texture_footprint.dudy, hit.texture_footprint.dvdy, 0.0f)
                                       : (float3)0.0f;
    float3 ng = isZero(hit.n_g) ? hit.n : hit.n_g;
    float3 n = hit.n;
    if (dot(n, ng) < 0.0f) n = -n;
    float3 t = (float3)0.0f;
    bool haveTangent = !isZero(hit.tangent);
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
    for (int i = 0; i < kMdlTextureResults; ++i) state.text_results[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    state.ro_data_segment_offset = 0;
    state.world_to_object = identityMatrix();
    state.object_to_world = identityMatrix();
    state.object_id = 0;
    state.meters_per_scene_unit = 1.0f;
    state.arg_block_offset = int(material.info.z);
}

float gpu_mdl_opacity(GpuMaterial material, inout Shading_state_material state)
{
    if ((material.info.w & kMaterialConstantCutout) != 0u) return material.params.w;
    float opacity = prism_mdl_cutout(material.info.y, state);
    return isfinite(opacity) ? clamp(opacity, 0.0f, 1.0f) : 1.0f;
}

float3 gpu_mdl_emission(GpuMaterial material, inout Shading_state_material state, float3 wo, bool backface)
{
    if (backface && !prism_mdl_thin_walled(material.info.y, state)) return (float3)0.0f;
    float3 intensity = prism_mdl_emission_intensity(material.info.y, backface, state);
    if (intensity.x <= 0.0f && intensity.y <= 0.0f && intensity.z <= 0.0f) return (float3)0.0f;
    Edf_evaluate_data data;
    data.k1 = wo;
    data.cos = 0.0f;
    data.edf = (float3)0.0f;
    data.pdf = 0.0f;
    if (!prism_mdl_edf_evaluate(material.info.y, backface, data, state)) return (float3)0.0f;
    return intensity * data.edf;
}

// MDLMaterial::sampleWithState
BsdfSample gpu_mdl_sample(SurfaceHit hit, GpuMaterial material, inout Shading_state_material state,
                          inout Rng rng, float4 xi)
{
    bool backface = !hit.front_face;
    float3 wo = -normalize(hit.d);
    float opacity = gpu_mdl_opacity(material, state);
    if (opacity <= 0.0f || (opacity < 1.0f && rng.next() >= opacity)) return gpu_null_sample(hit);
    bool thinWalled = prism_mdl_thin_walled(material.info.y, state);
    Bsdf_sample_data data;
    if (!backface || thinWalled) {
        data.ior1 = (float3)1.0f;
        data.ior2 = (float3)-1.0f;
    } else {
        data.ior1 = (float3)-1.0f;
        data.ior2 = (float3)1.0f;
    }
    data.k1 = wo;
    data.k2 = (float3)0.0f;
    data.xi = xi;
    data.pdf = 0.0f;
    data.bsdf_over_pdf = (float3)0.0f;
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
BsdfEvaluation gpu_mdl_evaluate(SurfaceHit hit, GpuMaterial material, inout Shading_state_material state,
                                             float3 wi)
{
    BsdfEvaluation result;
    result.value = (float3)0.0f;
    result.pdf = 0.0f;
    result.valid = false;
    bool backface = !hit.front_face;
    bool thinWalled = prism_mdl_thin_walled(material.info.y, state);
    Bsdf_evaluate_data data;
    data.k1 = -normalize(hit.d);
    data.k2 = normalize(wi);
    if (!backface || thinWalled) {
        data.ior1 = (float3)1.0f;
        data.ior2 = (float3)-1.0f;
    } else {
        data.ior1 = (float3)-1.0f;
        data.ior2 = (float3)1.0f;
    }
    data.bsdf_diffuse = (float3)0.0f;
    data.bsdf_glossy = (float3)0.0f;
    data.pdf = 0.0f;
    data.flags = 0;
    if (!prism_mdl_bsdf_evaluate(material.info.y, backface && thinWalled, data, state)) return result;
    result.value = data.bsdf_diffuse + data.bsdf_glossy;
    result.pdf = data.pdf;
    result.valid = result.pdf >= 0.0f && isfinite(result.pdf);
    return result;
}

// ---------------------------------------------------------------------------
// Environment (environment.h, hydra/environment_cache.h)
// ---------------------------------------------------------------------------

float3 gpu_row_transform(float4 r0, float4 r1, float4 r2, float3 v)
{
    return float3(dot(gpu_xyz(r0), v), dot(gpu_xyz(r1), v), dot(gpu_xyz(r2), v));
}

float3 gpu_env_to_local(GpuEnvironment env, float3 v)
{
    return gpu_row_transform(env.toLocal[0], env.toLocal[1], env.toLocal[2], v);
}

float gpu_texel_solid_angle(uint y, uint width, uint height)
{
    return (4.0f * kPi / float(width)) * sin(kPi * (float(y) + 0.5f) / float(height)) *
        sin(kPi / (2.0f * float(height)));
}

// EnvironmentMap::radiance in the map's local frame.
float3 gpu_latlong_radiance(GpuEnvironment env, float3 direction, float angle)
{
    float3 d = normalize(direction);
    float lon = atan2(d.z, d.x);
    if (lon < 0.0f) lon += 2.0f * kPi;
    float lat = atan2(hypot(d.x, d.z), d.y);
    float3 result;
    if (env.info.y != kImageNone) {
        float s = lon / (2.0f * kPi);
        float t = lat / kPi;
        float2 dx = float2(0.0f, 0.0f);
        float2 dy = float2(0.0f, 0.0f);
        if (angle > 0.0f && isfinite(angle)) {
            float3 tangent = normalize(cross(abs(d.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f)
                                                               : float3(1.0f, 0.0f, 0.0f), d));
            float3 bitangent = cross(d, tangent);
            for (int k = 0; k < 2; ++k) {
                float3 q = normalize(d + angle * (k == 0 ? tangent : bitangent));
                float qlon = atan2(q.z, q.x);
                if (qlon < 0.0f) qlon += 2.0f * kPi;
                float ds = qlon / (2.0f * kPi) - s;
                ds -= round(ds);
                float dt = atan2(hypot(q.x, q.z), q.y) / kPi - t;
                if (k == 0) dx = float2(ds, dt);
                else dy = float2(ds, dt);
            }
            if (hypot(d.x, d.z) < angle) {
                dx = float2(0.5f, 0.0f);
                dy = float2(0.0f, angle / kPi);
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
    if (!isfinite(result.x) || result.x < 0.0f) result.x = 0.0f;
    if (!isfinite(result.y) || result.y < 0.0f) result.y = 0.0f;
    if (!isfinite(result.z) || result.z < 0.0f) result.z = 0.0f;
    return result;
}

float3 gpu_environment_radiance(GpuEnvironment env, float3 world, float angle)
{
    uint kind = env.info.x;
    if (kind == kEnvConstant) return gpu_xyz(env.color0);
    if (kind == kEnvGradient) {
        float3 d = normalize(world);
        float t = 0.5f * (d.y + 1.0f);
        return (1.0f - t) * gpu_xyz(env.color0) + t * gpu_xyz(env.color1);
    }
    if (kind == kEnvCylindrical) {
        float3 d = normalize(world);
        float lon = atan2(d.z, d.x);
        float lat = acos(d.y);
        lon = lon < 0.0f ? lon + (2.0f * kPi) : lon;
        float u = 1.0f - (lon / (2.0f * kPi));
        float v = lat / kPi;
        if (env.info.y == kImageNone) return 2.0f * gpu_xyz(env.color0);
        GpuImage img = gpu_image(env.info.y);
        float i = abs(u - trunc(u)) * float(img.info.x - 1u);
        float j = abs(v - trunc(v)) * float(img.info.y - 1u);
        return 2.0f * gpu_xyz(gpu_image_bilinear_black(env.info.y, i / float(img.info.x),
                                                       j / float(img.info.y)));
    }
    if ((env.info.w & kEnvTransformed) == 0u) return gpu_latlong_radiance(env, world, angle);
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
void gpu_mul_wide(uint a, uint b, out uint lo, out uint hi)
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
EnvironmentLightSample gpu_latlong_sample_local(GpuEnvironment env, float4 u)
{
    EnvironmentLightSample s;
    s.radiance = (float3)0.0f;
    uint width = env.info.z;
    uint height = env.info2.x;
    if (width == 0u) {
        float y = 1.0f - 2.0f * u.x;
        float phi = 2.0f * kPi * u.y;
        float r = sqrt(max(0.0f, 1.0f - y * y));
        s.wi = float3(r * cos(phi), y, r * sin(phi));
        s.pdf = 1.0f / (4.0f * kPi);
        return s;
    }
    uint n = width * height;
    // x = (r1 + r2 / 2^24) * n with r1, r2 24-bit fractions, evaluated in
    // fixed point (the CPU uses double precision here).
    float r1 = u.x;
    float r2 = u.y;
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
    float phi = 2.0f * kPi * (float(px) + u.z) / float(width);
    float theta0 = kPi * float(py) / float(height);
    float theta1 = kPi * float(py + 1u) / float(height);
    float r = u.w;
    // cos(theta) between the rows' bounds, kept accurate near both poles.
    float s0 = sin(0.5f * theta0);
    float s1 = sin(0.5f * theta1);
    float c0 = cos(0.5f * theta0);
    float c1 = cos(0.5f * theta1);
    float oneMinusCos = lerp(2.0f * s0 * s0, 2.0f * s1 * s1, r);
    float onePlusCos = lerp(2.0f * c0 * c0, 2.0f * c1 * c1, r);
    float cosTheta = 0.5f * (onePlusCos - oneMinusCos);
    float sinTheta = sqrt(max(0.0f, oneMinusCos * onePlusCos));
    s.wi = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    s.pdf = gpu_env_texel_pdf(index) / gpu_texel_solid_angle(py, width, height);
    return s;
}

float gpu_latlong_pdf_local(GpuEnvironment env, float3 direction)
{
    uint width = env.info.z;
    uint height = env.info2.x;
    if (width == 0u) return 1.0f / (4.0f * kPi);
    float3 d = normalize(direction);
    float phi = atan2(d.z, d.x);
    if (phi < 0.0f) phi += 2.0f * kPi;
    uint x = min(uint(int(phi * float(width) / (2.0f * kPi))), width - 1u);
    uint y = min(uint(int(atan2(hypot(d.x, d.z), d.y) * float(height) / kPi)), height - 1u);
    return gpu_env_texel_pdf(y * width + x) / gpu_texel_solid_angle(y, width, height);
}

float gpu_env_jacobian(GpuEnvironment env, float3 world)
{
    float len = length(gpu_env_to_local(env, world));
    return len > 0.0f ? abs(env.multiplier.w) / (len * len * len) : 0.0f;
}

// Environment::sampleDirection and its overrides.
EnvironmentLightSample gpu_environment_sample(GpuEnvironment env, float4 u, float angularRadius)
{
    EnvironmentLightSample s;
    if (env.info.x != kEnvLatlong) {
        // Environment::sampleDirection: uniform sphere.
        float y = 1.0f - 2.0f * u.x;
        float phi = 2.0f * kPi * u.y;
        float r = sqrt(max(0.0f, 1.0f - y * y));
        s.wi = float3(r * cos(phi), y, r * sin(phi));
        s.pdf = 1.0f / (4.0f * kPi);
    } else {
        s = gpu_latlong_sample_local(env, u);
        if ((env.info.w & kEnvTransformed) != 0u) {
            s.wi = normalize(gpu_row_transform(env.toWorld[0], env.toWorld[1], env.toWorld[2], s.wi));
            s.pdf *= gpu_env_jacobian(env, s.wi);
        }
    }
    s.radiance = gpu_environment_radiance(env, s.wi, angularRadius);
    return s;
}

float gpu_environment_pdf(GpuEnvironment env, float3 direction)
{
    if (env.info.x != kEnvLatlong) return 1.0f / (4.0f * kPi);
    if ((env.info.w & kEnvTransformed) == 0u) return gpu_latlong_pdf_local(env, direction);
    float3 transformed = gpu_env_to_local(env, direction);
    if (isZero(transformed)) return 0.0f;
    return gpu_latlong_pdf_local(env, normalize(transformed)) * gpu_env_jacobian(env, normalize(direction));
}

// ---------------------------------------------------------------------------
// The backend interface of PathTracer
// ---------------------------------------------------------------------------

struct GpuBackend {
    Rng rng;

    GpuHit intersect(PathRay ray) { return gpu_intersect(ray, kMaskAll); }
    GpuHit intersectNonOpaque(PathRay ray) { return gpu_intersect(ray, kMaskNonOpaque); }
    bool occludedByOpaque(PathRay ray) { return gpu_occluded_by_opaque(ray.ray); }
    bool mayHaveCutouts() { return (gpu_frame().settings.z & kFrameCutouts) != 0u; }

    // Material::perturbNormal + Material::prepare
    GpuShading prepare(GpuHit hit)
    {
        const GpuMaterial material = gpu_material(hit.material);
        GpuShading shading;
        shading.surface = hit.surface;
        shading.material = hit.material;
        shading.surface.n_g = normalize(shading.surface.n);
        if ((material.info.w & kMaterialNormalTexture) != 0u) {
            const float3 sampled = gpu_texture3(material.textures2.x, shading.surface, true);
            if (!isZero(sampled)) {
                const float3 tangentNormal = sampled * 2.0f - 1.0f;
                const float3 ng = shading.surface.n_g;
                const float3 up = abs(ng.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
                const float3 t = cross(up, ng);
                const float3 b = cross(ng, t);
                shading.surface.n = normalize(t * tangentNormal.x + b * tangentNormal.y + ng * tangentNormal.z);
            }
        }
        if (material.info.x == kMaterialMdl) {
            gpu_build_mdl_state(shading.surface, material, shading.state);
            prism_mdl_init(material.info.y, shading.state);
        }
        return shading;
    }

    BsdfSample sample(GpuShading shading, float4 xi)
    {
        const GpuMaterial material = gpu_material(shading.material);
        if (material.info.x == kMaterialMdl) return gpu_mdl_sample(shading.surface, material, shading.state, rng, xi);
        return gpu_legacy_sample(shading.surface, material, rng);
    }

    BsdfEvaluation evaluate(GpuShading shading, float3 wi)
    {
        const GpuMaterial material = gpu_material(shading.material);
        if (material.info.x == kMaterialMdl) return gpu_mdl_evaluate(shading.surface, material, shading.state, wi);
        BsdfEvaluation e = {(float3)0.0f, 0.0f, false};
        if (material.info.x == kMaterialDiffuse || material.info.x == kMaterialFallback) {
            // Diffuse::evaluate
            const float cosine = max(0.0f, dot(shading.surface.n, normalize(wi)));
            e.value = gpu_texture3(material.textures.x, shading.surface, true) * (cosine / kPi);
            e.pdf = cosine / kPi;
            e.valid = cosine > 0.0f;
        }
        return e;
    }

    bool supportsNee(GpuShading shading)
    {
        const uint kind = gpu_material(shading.material).info.x;
        return kind == kMaterialMdl || kind == kMaterialDiffuse || kind == kMaterialFallback;
    }

    bool provenOpaque(GpuHit hit)
    {
        const GpuMaterial material = gpu_material(hit.material);
        if (material.info.x == kMaterialFallback) return material.params.x >= 1.0f;
        if (material.info.x == kMaterialMdl)
            return (material.info.w & kMaterialConstantCutout) != 0u && material.params.w >= 1.0f;
        return false;
    }

    float cutoutOpacity(GpuHit hit)
    {
        const GpuMaterial material = gpu_material(hit.material);
        if (material.info.x == kMaterialFallback) return material.params.x;
        if (material.info.x != kMaterialMdl) return 1.0f;
        if ((material.info.w & kMaterialConstantCutout) != 0u) return material.params.w;
        // Only programs with a varying cutout get here; their init has its own
        // dispatch so the full material set is not inlined a second time.
        Shading_state_material state;
        gpu_build_mdl_state(hit.surface, material, state);
        prism_mdl_init_cutout(material.info.y, state);
        const float opacity = prism_mdl_cutout(material.info.y, state);
        return isfinite(opacity) ? saturate(opacity) : 1.0f;
    }

    float3 environmentRadiance(float3 direction, float angularRadius)
    {
        return gpu_environment_radiance(gpu_environment(), direction, angularRadius);
    }

    EnvironmentLightSample sampleEnvironment(float4 u, float angularRadius)
    {
        return gpu_environment_sample(gpu_environment(), u, angularRadius);
    }

    float environmentPdf(float3 direction) { return gpu_environment_pdf(gpu_environment(), direction); }
    int maxDepth() { return int(gpu_frame().settings.x); }
    int rouletteDepth() { return int(gpu_frame().settings.y); }
    bool environmentMis() { return (gpu_frame().settings.z & kFrameEnvironmentMis) != 0u; }
};
