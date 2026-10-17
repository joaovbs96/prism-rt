#include "gpu_scene.h"

#include "../../batched_spheres.h"
#include "../../hydra/environment_cache.h"
#include "../../hydra/fallback_material.h"
#include "../../mdl_material.h"
#include "../../scene.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

namespace prism::device::gpu {

using namespace prism::gpu;

namespace {

float4 F4(Float3 const& v, float w = 0.0f) { return {v.x(), v.y(), v.z(), w}; }
float4 F4(float v) { return {v, v, v, v}; }
float4 F4(Float4 const& v) { return {v.x(), v.y(), v.z(), v.w()}; }

class Builder {
public:
    explicit Builder(GpuSceneData& data) : m_data(data) {}

    uint32_t image(std::shared_ptr<const prism::TextureResource> const& resource)
    {
        if (!resource) return PRISM_IMAGE_NONE;
        auto [found, inserted] = m_images.emplace(resource.get(), uint32_t(m_data.images.size()));
        if (inserted) m_data.images.push_back(resource);
        return found->second;
    }

    template <class T>
    uint32_t texture(Texture<T> const* texture)
    {
        if (!texture) return PRISM_TEXTURE_NONE;
        auto found = m_textures.find(texture);
        if (found != m_textures.end()) return found->second;
        GpuLegacyTexture record;
        record.info.z = sizeof(T) / sizeof(float);
        if (auto const* value = dynamic_cast<ValueTexture<T> const*>(texture)) {
            record.info.x = PRISM_TEXTURE_VALUE;
            record.value = F4(value->value);
        } else if (auto const* image = dynamic_cast<ImageTexture<T> const*>(texture)) {
            record.info.x = PRISM_TEXTURE_IMAGE;
            record.info.y = this->image(image->resource);
        } else if (auto const* mix = dynamic_cast<ImageMixTexture<T> const*>(texture)) {
            record.info.x = PRISM_TEXTURE_IMAGE_MIX;
            record.info.y = this->image(mix->_image ? mix->_image->resource : nullptr);
            record.value = F4(mix->_value ? mix->_value->value : T(1.0f));
        } else {
            // Unknown texture: evaluate it once, as a constant.
            record.info.x = PRISM_TEXTURE_VALUE;
            record.value = F4(texture->sample(0.0f, 0.0f, Float3(0.0f)));
            m_data.warnings.push_back("unsupported texture type evaluated as a constant");
        }
        if (record.info.x != PRISM_TEXTURE_VALUE && record.info.y == PRISM_IMAGE_NONE) {
            record.info.x = PRISM_TEXTURE_VALUE;
            record.value = F4(0.0f);
        }
        const uint32_t index = uint32_t(m_data.legacyTextures.size());
        m_data.legacyTextures.push_back(record);
        m_textures.emplace(texture, index);
        return index;
    }

    uint32_t material(Material* material)
    {
        auto found = m_materials.find(material);
        if (found != m_materials.end()) return found->second;
        GpuMaterial record;
        record.info.x = PRISM_MATERIAL_INVALID;
        record.textures = {PRISM_TEXTURE_NONE, PRISM_TEXTURE_NONE, PRISM_TEXTURE_NONE, PRISM_TEXTURE_NONE};
        record.textures2 = {PRISM_TEXTURE_NONE, PRISM_TEXTURE_NONE, 0, 0};
        record.params.w = 1.0f;
        if (material) {
            if (material->normal_texture) {
                record.info.w |= PRISM_MATERIAL_NORMAL_TEXTURE;
                record.textures2.x = texture(material->normal_texture);
            }
            if (auto const* mdl = dynamic_cast<MDLMaterial const*>(material)) {
                if (mdl->is_valid()) {
                    record.info.x = PRISM_MATERIAL_MDL;
                    record.info.y = uint32_t(m_data.mdlMaterials.size());
                    m_data.mdlMaterials.push_back(mdl);
                }
            } else if (auto const* fallback = dynamic_cast<prism::hydra::FallbackMaterial const*>(material)) {
                record.info.x = PRISM_MATERIAL_FALLBACK;
                record.textures.x = texture(fallback->diffuse_color);
                record.params.x = fallback->opacity();
            } else if (auto const* dispersive = dynamic_cast<Dispersive const*>(material)) {
                record.info.x = PRISM_MATERIAL_DISPERSIVE;
                record.params = {dispersive->iorD(), dispersive->iorF(), dispersive->iorC(), 1.0f};
            } else if (auto const* glass = dynamic_cast<Glass const*>(material)) {
                record.info.x = PRISM_MATERIAL_GLASS;
                record.params.x = glass->getIor();
            } else if (auto const* diffuse = dynamic_cast<Diffuse const*>(material)) {
                record.info.x = PRISM_MATERIAL_DIFFUSE;
                record.textures.x = texture(diffuse->diffuse_color);
            } else if (auto const* metal = dynamic_cast<Metal const*>(material)) {
                record.info.x = PRISM_MATERIAL_METAL;
                record.textures.x = texture(metal->specular_color);
                record.textures.y = texture(metal->specular_roughness);
            } else if (auto const* emissive = dynamic_cast<Emissive const*>(material)) {
                record.info.x = PRISM_MATERIAL_EMISSIVE;
                record.textures.x = texture(emissive->emissive_color);
                record.params.x = emissive->emissive_multiplier;
            } else if (auto const* gltf = dynamic_cast<DiffuseGltf const*>(material)) {
                record.info.x = PRISM_MATERIAL_DIFFUSE_GLTF;
                record.textures.x = texture(gltf->base_color);
            } else if (auto const* gltf = dynamic_cast<MetalGltf const*>(material)) {
                record.info.x = PRISM_MATERIAL_METAL_GLTF;
                record.textures.x = texture(gltf->base_color);
                record.textures.y = texture(gltf->orm);
            } else if (auto const* gltf = dynamic_cast<SpecularBTDFglTF const*>(material)) {
                record.info.x = PRISM_MATERIAL_SPECULAR_BTDF_GLTF;
                record.textures.x = texture(gltf->base_color);
                record.params.x = gltf->ior;
            } else if (auto const* pbr = dynamic_cast<gltfPBR const*>(material)) {
                record.info.x = PRISM_MATERIAL_GLTF_PBR;
                record.textures.x = texture(pbr->base_color);
                record.textures.y = texture(pbr->orm);
                record.textures.z = texture(pbr->transmission);
                record.textures2.y = texture(pbr->emissive_color);
                record.params.x = pbr->emissive_multiplier;
            } else {
                m_data.warnings.push_back("unsupported material type rendered as invalid");
            }
        }
        const uint32_t index = uint32_t(m_data.materials.size());
        m_data.materials.push_back(record);
        m_materials.emplace(material, index);
        return index;
    }

    // One prototype per distinct geometry.
    uint32_t prototype(Geometry* geometry)
    {
        auto found = m_prototypes.find(geometry);
        if (found != m_prototypes.end()) return found->second;
        GpuPrototype record;
        const uint32_t firstVertex = uint32_t(m_data.positions.size() / 3);
        record.info.y = firstVertex;
        auto pushVertex = [&](float const* p) {
            m_data.positions.insert(m_data.positions.end(), p, p + 3);
        };
        if (geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE) {
            auto* mesh = dynamic_cast<Mesh*>(geometry);
            record.info.x = PRISM_PROTO_TRIANGLES;
            record.info.z = uint32_t(m_data.indices.size() / 3);
            const size_t vertexCount = mesh ? mesh->vertices.size() : 0;
            // The Embree vertex buffer holds any baked transform.
            float const* embree = geometry->geom ? static_cast<float const*>(
                rtcGetGeometryBufferData(geometry->geom, RTC_BUFFER_TYPE_VERTEX, 0)) : nullptr;
            for (size_t v = 0; v < vertexCount; ++v) {
                float p[3] = {mesh->vertices[v].x(), mesh->vertices[v].y(), mesh->vertices[v].z()};
                if (embree) std::memcpy(p, embree + 3 * v, sizeof(p));
                pushVertex(p);
                Float3 n = v < mesh->normals.size() ? mesh->normals[v] : Float3(0.0f);
                m_data.normals.insert(m_data.normals.end(), {n.x(), n.y(), n.z()});
                Float2 uv = v < mesh->texcoords.size() ? mesh->texcoords[v] : Float2(0.0f, 0.0f);
                m_data.texcoords.insert(m_data.texcoords.end(), {uv.u(), uv.v()});
                for (int c = 0; c < 4; ++c)
                    m_data.tangents.push_back(4 * v + c < mesh->tangents.size()
                        ? mesh->tangents[4 * v + c] : 0.0f);
            }
            const size_t triangles = mesh ? mesh->indices.size() / 3 : 0;
            m_data.indices.insert(m_data.indices.end(), mesh->indices.begin(),
                                  mesh->indices.begin() + triangles * 3);
            record.info.w = uint32_t(triangles);
            record.info2.x = mesh && mesh->texcoords.size() == vertexCount ? 1u : 0u;
        } else if (geometry->type == RTC_GEOMETRY_TYPE_QUAD) {
            // Embree splits quad (v0, v1, v2, v3) into (v0, v1, v3) and
            // (v2, v3, v1); the kernel restores the quad parameterization.
            record.info.x = PRISM_PROTO_QUADS;
            record.info.z = uint32_t(m_data.indices.size() / 3);
            const int vertexCount = geometry->vertex_count;
            float const* vertices = static_cast<float const*>(
                rtcGetGeometryBufferData(geometry->geom, RTC_BUFFER_TYPE_VERTEX, 0));
            uint32_t const* quads = static_cast<uint32_t const*>(
                rtcGetGeometryBufferData(geometry->geom, RTC_BUFFER_TYPE_INDEX, 0));
            const uint32_t quadCount = dynamic_cast<Box*>(geometry) ? 6u : 1u;
            for (int v = 0; v < vertexCount; ++v) {
                pushVertex(vertices + 3 * v);
                m_data.normals.insert(m_data.normals.end(), {0.0f, 0.0f, 0.0f});
                m_data.texcoords.insert(m_data.texcoords.end(), {0.0f, 0.0f});
                m_data.tangents.insert(m_data.tangents.end(), {0.0f, 0.0f, 0.0f, 1.0f});
            }
            for (uint32_t q = 0; q < quadCount; ++q) {
                uint32_t const* i = quads + 4 * q;
                m_data.indices.insert(m_data.indices.end(), {i[0], i[1], i[3], i[2], i[3], i[1]});
            }
            record.info.w = quadCount;
        } else if (geometry->type == RTC_GEOMETRY_TYPE_SPHERE_POINT) {
            record.info.x = PRISM_PROTO_SPHERES;
            record.info.z = uint32_t(m_data.spheres.size());
            uint32_t count = 0;
            if (auto* batch = dynamic_cast<BatchedSpheres*>(geometry)) {
                for (auto const& entry : batch->entries)
                    m_data.spheres.push_back(F4(entry.center, entry.radius));
                count = uint32_t(batch->entries.size());
            } else if (auto* sphere = dynamic_cast<Sphere*>(geometry)) {
                m_data.spheres.push_back(F4(sphere->pos, sphere->r));
                count = 1;
            }
            record.info.w = count;
        } else {
            m_data.warnings.push_back("unsupported geometry type skipped");
            record.info.x = PRISM_PROTO_TRIANGLES;
            record.info.w = 0;
        }
        const uint32_t index = uint32_t(m_data.prototypes.size());
        m_data.prototypes.push_back(record);
        m_prototypes.emplace(geometry, index);
        return index;
    }

    // Per-primitive materials of one placement, shared between equal tables.
    uint32_t primitiveMaterials(std::vector<uint32_t> const& table)
    {
        auto [found, inserted] = m_materialTables.emplace(table, uint32_t(m_data.primitiveMaterials.size()));
        if (inserted)
            m_data.primitiveMaterials.insert(m_data.primitiveMaterials.end(), table.begin(), table.end());
        return found->second;
    }

private:
    GpuSceneData& m_data;
    std::unordered_map<const void*, uint32_t> m_images;
    std::unordered_map<const void*, uint32_t> m_textures;
    std::unordered_map<Material*, uint32_t> m_materials;
    std::unordered_map<Geometry*, uint32_t> m_prototypes;
    std::map<std::vector<uint32_t>, uint32_t> m_materialTables;
};

void SetMatrixRows(float4 rows[3], std::array<float, 9> const& m)
{
    for (int r = 0; r < 3; ++r) rows[r] = {m[r * 3], m[r * 3 + 1], m[r * 3 + 2], 0.0f};
}

void BuildEnvironment(Environment const* environment, Builder& builder, GpuSceneData& data)
{
    GpuEnvironment& record = data.environment;
    record = {};
    record.info.x = PRISM_ENV_CONSTANT;
    record.info.y = PRISM_IMAGE_NONE;
    SetMatrixRows(record.toLocal, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    SetMatrixRows(record.toWorld, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    record.multiplier.w = 1.0f;
    if (!environment) return;

    auto setLatLong = [&](EnvironmentMap const& map) {
        record.info.x = PRISM_ENV_LATLONG;
        record.multiplier = F4(map.multiplier, record.multiplier.w);
        if (auto const* image = dynamic_cast<ImageTexture<Color3> const*>(map.texture);
            image && image->width > 0 && image->height > 0)
            record.info.y = builder.image(image->resource);
        else if (map.texture)
            record.color0 = F4(map.texture->sampleInterp(0.0f, 0.0f, Float3(0.0f)));
        if (!map.probability.empty()) {
            record.info.z = uint32_t(map.width);
            record.info2.x = uint32_t(map.height);
            data.environmentTexelPdf = map.texelProbability;
            data.environmentAliasProbability = map.probability;
            data.environmentAlias.assign(map.alias.begin(), map.alias.end());
        }
    };
    auto setTransform = [&](std::array<float, 9> const& toLocal,
                            std::array<float, 9> const& toWorld, float det) {
        record.info.w |= PRISM_ENV_TRANSFORMED;
        SetMatrixRows(record.toLocal, toLocal);
        SetMatrixRows(record.toWorld, toWorld);
        record.multiplier.w = det;
    };

    if (auto const* cached = dynamic_cast<prism::hydra::CachedTransformedEnvironment const*>(environment)) {
        setTransform(cached->worldToEnvironment(), cached->environmentToWorld(),
                     cached->transformDeterminant());
        setLatLong(cached->localMap());
    } else if (auto const* transformed = dynamic_cast<TransformedEnvironmentMap const*>(environment)) {
        setTransform(transformed->worldToEnvironment(), transformed->environmentToWorld(),
                     transformed->transformDeterminant());
        setLatLong(*transformed);
    } else if (auto const* map = dynamic_cast<EnvironmentMap const*>(environment)) {
        setLatLong(*map);
    } else if (auto const* cylindrical = dynamic_cast<CylindricalEnvironmentMap const*>(environment)) {
        record.info.x = PRISM_ENV_CYLINDRICAL;
        if (auto const* image = dynamic_cast<ImageTexture<Color3> const*>(cylindrical->texture))
            record.info.y = builder.image(image->resource);
        else if (cylindrical->texture)
            record.color0 = F4(cylindrical->texture->sampleInterp(0.0f, 0.0f, Float3(0.0f)));
    } else if (auto const* gradient = dynamic_cast<GradientBackground const*>(environment)) {
        record.info.x = PRISM_ENV_GRADIENT;
        record.color0 = F4(gradient->t0->sample(0, 0, Float3(0.0f)));
        record.color1 = F4(gradient->t1->sample(0, 0, Float3(0.0f)));
    } else if (auto const* constant = dynamic_cast<ConstantColorBackground const*>(environment)) {
        record.color0 = F4(constant->texture->sample(0, 0, Float3(0.0f)));
    } else {
        // Unknown environments are directional constants at best; sample one.
        record.color0 = F4(environment->radiance(Float3(0.0f, 1.0f, 0.0f)));
        data.warnings.push_back("unsupported environment type rendered as a constant");
    }
}

} // namespace

GpuSceneData BuildGpuScene(SceneManager& scene)
{
    GpuSceneData data;
    Builder builder(data);
    data.maxDepth = scene.max_depth;
    data.rouletteDepth = scene.rr_min_depth;
    data.environmentMis = scene.environmentSampling;
    data.filteredTextures = scene.textureFiltering == prism::TextureFiltering::Filtered;

    // Deterministic order: by placement id.
    std::vector<int> placements;
    placements.reserve(scene.geometry.size());
    for (auto const& [id, geometry] : scene.geometry)
        if (!scene.disabled_placements.contains(id)) placements.push_back(id);
    std::sort(placements.begin(), placements.end());

    std::vector<uint32_t> table;
    for (int id : placements) {
        Geometry* geometry = scene.geometry.at(id);
        if (!geometry) continue;
        const uint32_t prototype = builder.prototype(geometry);
        GpuPrototype const& proto = data.prototypes[prototype];
        if (proto.info.w == 0) continue;

        GpuInstance instance;
        instance.info.x = prototype;
        instance.info.w = uint32_t(id);
        std::array<float, 12> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const bool instanced = scene.intersection_geometry.contains(id);
        if (instanced) {
            if (RTCGeometry embree = rtcGetGeometry(scene.scene, unsigned(id)))
                rtcGetGeometryTransform(embree, 0.0f, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, transform.data());
        }
        auto placement = scene.placement_transforms.find(id);
        if (placement != scene.placement_transforms.end()) {
            auto const& p = placement->second;
            instance.info.y |= PRISM_INSTANCE_PLACEMENT;
            const bool flip = p.flipOrientation ||
                (p.mirrored && geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE);
            if (flip) instance.info.y |= PRISM_INSTANCE_FLIP;
            if (p.mirrored) instance.info.y |= PRISM_INSTANCE_MIRRORED;
            for (int c = 0; c < 3; ++c) {
                instance.normalColumns[c] = {p.normal[c], p.normal[3 + c], p.normal[6 + c], 0.0f};
                instance.tangentColumns[c] = {p.tangent[c], p.tangent[3 + c], p.tangent[6 + c], 0.0f};
            }
        }

        // SceneManager::getMaterial for every primitive of the placement.
        const uint32_t primitives = proto.info.w;
        table.assign(primitives, 0);
        auto binding = scene.placement_materials.find(id);
        const unsigned instanceId = instanced ? unsigned(id) : RTC_INVALID_GEOMETRY_ID;
        for (uint32_t prim = 0; prim < primitives; ++prim) {
            Material* material = nullptr;
            bool resolved = false;
            if (binding != scene.placement_materials.end()) {
                auto triangle = binding->second.triangles.find(int(prim));
                if (triangle != binding->second.triangles.end()) {
                    material = triangle->second;
                    resolved = true;
                } else if (binding->second.fallback) {
                    material = binding->second.fallback;
                    resolved = true;
                }
            }
            if (!resolved) {
                try { material = geometry->getMaterial(prim, instanceId); }
                catch (std::exception const&) { material = nullptr; }
            }
            table[prim] = builder.material(material);
        }
        instance.info.z = builder.primitiveMaterials(table);
        data.instances.push_back(instance);
        data.instanceTransforms.push_back(transform);
    }

    BuildEnvironment(scene.environment, builder, data);
    data.mdlTextures.resize(1);
    return data;
}

void ApplyMdlShaderCode(GpuSceneData& data, MdlShaderCode const& code)
{
    for (auto& material : data.materials) {
        if (material.info.x != PRISM_MATERIAL_MDL) continue;
        auto const& record = code.materials.at(material.info.y);
        if (!code.success || !record.valid) {
            material.info.x = PRISM_MATERIAL_INVALID;
            continue;
        }
        material.info.y = record.program;
        material.info.z = record.argBlockOffset;
        if (record.constantCutout) {
            material.info.w |= PRISM_MATERIAL_CONSTANT_CUTOUT;
            material.params.w = *record.constantCutout;
        }
    }
    data.mdlTextures.assign(1, GpuMdlTexture{});
    data.mdlTiles.clear();
    data.bsdfData.clear();
    std::unordered_map<const void*, uint32_t> images;
    for (uint32_t i = 0; i < data.images.size(); ++i) images.emplace(data.images[i].get(), i);
    auto image = [&](std::shared_ptr<const prism::TextureResource> const& resource) {
        auto [found, inserted] = images.emplace(resource.get(), uint32_t(data.images.size()));
        if (inserted) data.images.push_back(resource);
        return found->second;
    };
    for (size_t t = 1; t < code.textures.size(); ++t) {
        auto const& texture = code.textures[t];
        GpuMdlTexture record;
        if (texture.kind == MdlTexture::Kind::Image && !texture.tiles.empty()) {
            int uMin = texture.tiles.front().first.first, uMax = uMin;
            int vMin = texture.tiles.front().first.second, vMax = vMin;
            for (auto const& [tile, resource] : texture.tiles) {
                uMin = std::min(uMin, tile.first); uMax = std::max(uMax, tile.first);
                vMin = std::min(vMin, tile.second); vMax = std::max(vMax, tile.second);
            }
            const uint32_t uCount = uint32_t(uMax - uMin + 1), vCount = uint32_t(vMax - vMin + 1);
            record.info = {PRISM_MDL_TEXTURE_IMAGE, uint32_t(data.mdlTiles.size()),
                           uint32_t(uMin), uint32_t(vMin)};
            record.info2 = {uCount, vCount, texture.tiled ? 1u : 0u, 0u};
            const size_t first = data.mdlTiles.size();
            data.mdlTiles.resize(first + size_t(uCount) * vCount, PRISM_TILE_MISSING);
            for (auto const& [tile, resource] : texture.tiles)
                data.mdlTiles[first + size_t(tile.second - vMin) * uCount + (tile.first - uMin)] =
                    image(resource);
        } else if (texture.kind == MdlTexture::Kind::BsdfData && texture.values) {
            record.info = {PRISM_MDL_TEXTURE_BSDF_DATA, uint32_t(data.bsdfData.size()),
                           uint32_t(texture.width), uint32_t(texture.height)};
            record.info2 = {uint32_t(texture.depth), uint32_t(texture.components), 0u, 0u};
            data.bsdfData.insert(data.bsdfData.end(), texture.values,
                texture.values + texture.width * texture.height * texture.depth * texture.components);
        }
        data.mdlTextures.push_back(record);
    }
}

} // namespace prism::device::gpu
