#include "gpu_scene.h"

#include "../light_table.h"

#include "../../batched_spheres.h"
#include "../../hydra/environment_cache.h"
#include "../../hydra/fallback_material.h"
#include "../../mdl_material.h"
#include "../../scene.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <map>
#include <unordered_map>

namespace prism::device::gpu {


namespace {

float4 F4(Float3 const& v, float w = 0.0f) { return {v.x(), v.y(), v.z(), w}; }
float4 F4(float v) { return {v, v, v, v}; }
float4 F4(Float4 const& v) { return {v.x(), v.y(), v.z(), v.w()}; }

class Builder {
public:
    explicit Builder(GpuSceneData& data) : m_data(data) {}

    uint32_t image(std::shared_ptr<const prism::TextureResource> const& resource)
    {
        if (!resource) return kImageNone;
        auto [found, inserted] = m_images.emplace(resource.get(), uint32_t(m_data.images.size()));
        if (inserted) m_data.images.push_back(resource);
        return found->second;
    }

    template <class T>
    uint32_t texture(Texture<T> const* texture)
    {
        if (!texture) return kTextureNone;
        auto found = m_textures.find(texture);
        if (found != m_textures.end()) return found->second;
        GpuLegacyTexture record;
        record.info.z = sizeof(T) / sizeof(float);
        if (auto const* value = dynamic_cast<ValueTexture<T> const*>(texture)) {
            record.info.x = kTextureValue;
            record.value = F4(value->value);
        } else if (auto const* image = dynamic_cast<ImageTexture<T> const*>(texture)) {
            record.info.x = kTextureImage;
            record.info.y = this->image(image->resource);
        } else if (auto const* mix = dynamic_cast<ImageMixTexture<T> const*>(texture)) {
            record.info.x = kTextureImageMix;
            record.info.y = this->image(mix->_image ? mix->_image->resource : nullptr);
            record.value = F4(mix->_value ? mix->_value->value : T(1.0f));
        } else {
            // Unknown texture: evaluate it once, as a constant.
            record.info.x = kTextureValue;
            record.value = F4(texture->sample(0.0f, 0.0f, Float3(0.0f)));
            m_data.warnings.push_back("unsupported texture type evaluated as a constant");
        }
        if (record.info.x != kTextureValue && record.info.y == kImageNone) {
            record.info.x = kTextureValue;
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
        record.info.x = kMaterialInvalid;
        record.textures = {kTextureNone, kTextureNone, kTextureNone, kTextureNone};
        record.textures2 = {kTextureNone, kTextureNone, 0, 0};
        record.params.w = 1.0f;
        if (material) {
            if (material->normal_texture) {
                record.info.w |= kMaterialNormalTexture;
                record.textures2.x = texture(material->normal_texture);
            }
            if (auto const* mdl = dynamic_cast<MDLMaterial const*>(material)) {
                if (mdl->is_valid()) {
                    record.info.x = kMaterialMdl;
                    record.info.y = uint32_t(m_data.mdlMaterials.size());
                    m_data.mdlMaterials.push_back(mdl);
                }
            } else if (auto const* fallback = dynamic_cast<prism::hydra::FallbackMaterial const*>(material)) {
                record.info.x = kMaterialFallback;
                record.textures.x = texture(fallback->diffuse_color);
                record.params.x = fallback->opacity();
            } else if (auto const* dispersive = dynamic_cast<Dispersive const*>(material)) {
                record.info.x = kMaterialDispersive;
                record.params = {dispersive->iorD(), dispersive->iorF(), dispersive->iorC(), 1.0f};
            } else if (auto const* glass = dynamic_cast<Glass const*>(material)) {
                record.info.x = kMaterialGlass;
                record.params.x = glass->getIor();
            } else if (auto const* diffuse = dynamic_cast<Diffuse const*>(material)) {
                record.info.x = kMaterialDiffuse;
                record.textures.x = texture(diffuse->diffuse_color);
            } else if (auto const* metal = dynamic_cast<Metal const*>(material)) {
                record.info.x = kMaterialMetal;
                record.textures.x = texture(metal->specular_color);
                record.textures.y = texture(metal->specular_roughness);
            } else if (auto const* emissive = dynamic_cast<Emissive const*>(material)) {
                record.info.x = kMaterialEmissive;
                record.textures.x = texture(emissive->emissive_color);
                record.params.x = emissive->emissive_multiplier;
            } else if (auto const* gltf = dynamic_cast<DiffuseGltf const*>(material)) {
                record.info.x = kMaterialDiffuseGltf;
                record.textures.x = texture(gltf->base_color);
            } else if (auto const* gltf = dynamic_cast<MetalGltf const*>(material)) {
                record.info.x = kMaterialMetalGltf;
                record.textures.x = texture(gltf->base_color);
                record.textures.y = texture(gltf->orm);
            } else if (auto const* gltf = dynamic_cast<SpecularBTDFglTF const*>(material)) {
                record.info.x = kMaterialSpecularBtdfGltf;
                record.textures.x = texture(gltf->base_color);
                record.params.x = gltf->ior;
            } else if (auto const* pbr = dynamic_cast<gltfPBR const*>(material)) {
                record.info.x = kMaterialGltfPbr;
                record.textures.x = texture(pbr->base_color);
                record.textures.y = texture(pbr->orm);
                record.textures.z = texture(pbr->transmission);
                // gltfPBR::sample returns its lobes' samples, which carry no
                // emission; the emission texture is never sampled.
            } else {
                m_data.warnings.push_back("unsupported material type rendered as invalid");
            }
        }
        const uint32_t index = uint32_t(m_data.materials.size());
        m_data.materials.push_back(record);
        m_materials.emplace(material, index);
        return index;
    }

    std::optional<uint32_t> materialIndex(Material const* material) const
    {
        auto found = m_materials.find(const_cast<Material*>(material));
        if (found == m_materials.end()) return std::nullopt;
        return found->second;
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
            record.info.x = kProtoTriangles;
            record.info.z = uint32_t(m_data.indices.size() / 3);
            const size_t vertexCount = mesh ? mesh->vertices.size() : 0;
            // The Embree vertex buffer holds any baked transform.
            float const* embree = geometry->geom ? static_cast<float const*>(
                rtcGetGeometryBufferData(geometry->geom, RTC_BUFFER_TYPE_VERTEX, 0)) : nullptr;
            // Sized once and filled in place: scenes rebuild this on every edit.
            const size_t base = m_data.positions.size() / 3;
            m_data.positions.resize(3 * (base + vertexCount));
            m_data.normals.resize(3 * (base + vertexCount), 0.0f);
            m_data.texcoords.resize(2 * (base + vertexCount), 0.0f);
            m_data.tangents.resize(4 * (base + vertexCount), 0.0f);
            float* positions = m_data.positions.data() + 3 * base;
            float* normals = m_data.normals.data() + 3 * base;
            float* texcoords = m_data.texcoords.data() + 2 * base;
            if (embree) {
                std::memcpy(positions, embree, vertexCount * 3 * sizeof(float));
            } else {
                for (size_t v = 0; v < vertexCount; ++v) {
                    positions[3 * v] = mesh->vertices[v].x();
                    positions[3 * v + 1] = mesh->vertices[v].y();
                    positions[3 * v + 2] = mesh->vertices[v].z();
                }
            }
            for (size_t v = 0; v < std::min(vertexCount, mesh ? mesh->normals.size() : 0); ++v) {
                normals[3 * v] = mesh->normals[v].x();
                normals[3 * v + 1] = mesh->normals[v].y();
                normals[3 * v + 2] = mesh->normals[v].z();
            }
            for (size_t v = 0; v < std::min(vertexCount, mesh ? mesh->texcoords.size() : 0); ++v) {
                texcoords[2 * v] = mesh->texcoords[v].u();
                texcoords[2 * v + 1] = mesh->texcoords[v].v();
            }
            if (mesh)
                std::memcpy(m_data.tangents.data() + 4 * base, mesh->tangents.data(),
                            std::min(4 * vertexCount, mesh->tangents.size()) * sizeof(float));
            const size_t triangles = mesh ? mesh->indices.size() / 3 : 0;
            m_data.indices.insert(m_data.indices.end(), mesh->indices.begin(),
                                  mesh->indices.begin() + triangles * 3);
            record.info.w = uint32_t(triangles);
            record.info2.x = mesh && mesh->texcoords.size() == vertexCount ? 1u : 0u;
            if (mesh && mesh->primvarStride > 0 &&
                mesh->primvarValues.size() == vertexCount * size_t(mesh->primvarStride)) {
                record.info2.y = uint32_t(m_data.primvars.size());
                record.info2.z = uint32_t(mesh->primvars.size());
                record.info2.w = uint32_t(m_data.primvarValues.size());
                m_data.primvarValues.insert(m_data.primvarValues.end(), mesh->primvarValues.begin(),
                                            mesh->primvarValues.end());
                for (auto const& primvar : mesh->primvars) {
                    GpuPrimvar entry;
                    entry.info = {0u, uint32_t(primvar.components), uint32_t(primvar.offset),
                                  uint32_t(mesh->primvarStride)};
                    m_data.primvars.push_back(entry);
                    m_data.primvarNames.push_back(primvar.name);
                }
            }
        } else if (geometry->type == RTC_GEOMETRY_TYPE_QUAD) {
            // Embree splits quad (v0, v1, v2, v3) into (v0, v1, v3) and
            // (v2, v3, v1); the kernel restores the quad parameterization.
            record.info.x = kProtoQuads;
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
            record.info.x = kProtoSpheres;
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
            record.info.x = kProtoTriangles;
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

// The GPU form of a map's importance tables, built once per map. The cache
// is keyed by the tables' storage, checked against a sample of the content
// so storage reused by another map is not mistaken for it.
std::shared_ptr<const GpuSceneData::EnvironmentTables> SharedEnvironmentTables(EnvironmentMap const& map)
{
    struct Entry {
        const float* storage = nullptr;
        size_t size = 0;
        std::vector<float> sample;
        std::shared_ptr<const GpuSceneData::EnvironmentTables> tables;
    };
    const size_t size = map.texelProbability.size();
    std::vector<float> sample;
    for (size_t i = 0; i < size; i += std::max<size_t>(1, size / 1024)) sample.push_back(map.texelProbability[i]);
    static std::mutex mutex;
    static std::vector<Entry> cache;
    std::lock_guard lock(mutex);
    for (auto const& entry : cache)
        if (entry.storage == map.texelProbability.data() && entry.size == size && entry.sample == sample)
            return entry.tables;
    auto tables = std::make_shared<GpuSceneData::EnvironmentTables>();
    tables->texelPdf = map.texelProbability;
    tables->aliasProbability = map.probability;
    tables->alias.assign(map.alias.begin(), map.alias.end());
    constexpr size_t maxEntries = 2;
    if (cache.size() >= maxEntries) cache.erase(cache.begin());
    cache.push_back({map.texelProbability.data(), size, std::move(sample), tables});
    return tables;
}

// One dome's record; its lat-long map (if any) is returned through *map.
GpuEnvironment BuildEnvironmentRecord(Environment const* environment, Builder& builder, GpuSceneData& data,
                                      EnvironmentMap const** map)
{
    GpuEnvironment record{};
    record.info.x = kEnvConstant;
    record.info.y = kImageNone;
    SetMatrixRows(record.toLocal, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    SetMatrixRows(record.toWorld, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    record.multiplier.w = 1.0f;
    record.selection.x = 1.0f;
    *map = nullptr;
    if (!environment) return record;

    auto setLatLong = [&](EnvironmentMap const& latlong) {
        record.info.x = kEnvLatlong;
        record.multiplier = F4(latlong.multiplier, record.multiplier.w);
        if (auto const* image = dynamic_cast<ImageTexture<Color3> const*>(latlong.texture);
            image && image->width > 0 && image->height > 0)
            record.info.y = builder.image(image->resource);
        else if (latlong.texture)
            record.color0 = F4(latlong.texture->sampleInterp(0.0f, 0.0f, Float3(0.0f)));
        if (!latlong.probability.empty()) {
            record.info.z = uint32_t(latlong.width);
            record.info2.x = uint32_t(latlong.height);
            *map = &latlong;
        }
    };
    auto setTransform = [&](std::array<float, 9> const& toLocal,
                            std::array<float, 9> const& toWorld, float det) {
        record.info.w |= kEnvTransformed;
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
    } else if (auto const* latlong = dynamic_cast<EnvironmentMap const*>(environment)) {
        setLatLong(*latlong);
    } else if (auto const* cylindrical = dynamic_cast<CylindricalEnvironmentMap const*>(environment)) {
        record.info.x = kEnvCylindrical;
        if (auto const* image = dynamic_cast<ImageTexture<Color3> const*>(cylindrical->texture))
            record.info.y = builder.image(image->resource);
        else if (cylindrical->texture)
            record.color0 = F4(cylindrical->texture->sampleInterp(0.0f, 0.0f, Float3(0.0f)));
    } else if (auto const* gradient = dynamic_cast<GradientBackground const*>(environment)) {
        record.info.x = kEnvGradient;
        record.color0 = F4(gradient->t0->sample(0, 0, Float3(0.0f)));
        record.color1 = F4(gradient->t1->sample(0, 0, Float3(0.0f)));
    } else if (auto const* constant = dynamic_cast<ConstantColorBackground const*>(environment)) {
        record.color0 = F4(constant->texture->sample(0, 0, Float3(0.0f)));
    } else {
        // Unknown environments are directional constants at best; sample one.
        record.color0 = F4(environment->radiance(Float3(0.0f, 1.0f, 0.0f)));
        data.warnings.push_back("unsupported environment type rendered as a constant");
    }
    return record;
}

void BuildEnvironment(Environment const* environment, Builder& builder, GpuSceneData& data)
{
    data.environments.clear();
    data.environmentTables.reset();
    std::vector<Environment const*> parts{environment};
    std::vector<float> weights{1.0f};
    if (auto const* composite = dynamic_cast<CompositeEnvironment const*>(environment);
        composite && !composite->parts.empty()) {
        parts = composite->parts;
        weights = composite->weights;
    }
    std::vector<EnvironmentMap const*> maps;
    for (size_t i = 0; i < parts.size(); ++i) {
        EnvironmentMap const* map = nullptr;
        GpuEnvironment record = BuildEnvironmentRecord(parts[i], builder, data, &map);
        record.selection.x = weights[i];
        data.environments.push_back(record);
        maps.push_back(map);
    }
    size_t mapCount = 0;
    for (auto const* map : maps) mapCount += map ? 1 : 0;
    if (mapCount == 1) {
        for (auto const* map : maps)
            if (map) data.environmentTables = SharedEnvironmentTables(*map);
    } else if (mapCount > 1) {
        auto tables = std::make_shared<GpuSceneData::EnvironmentTables>();
        for (size_t i = 0; i < maps.size(); ++i) {
            if (!maps[i]) continue;
            data.environments[i].info3.x = uint32_t(tables->texelPdf.size());
            tables->texelPdf.insert(tables->texelPdf.end(), maps[i]->texelProbability.begin(),
                                    maps[i]->texelProbability.end());
            tables->aliasProbability.insert(tables->aliasProbability.end(), maps[i]->probability.begin(),
                                            maps[i]->probability.end());
            tables->alias.insert(tables->alias.end(), maps[i]->alias.begin(), maps[i]->alias.end());
        }
        data.environmentTables = std::move(tables);
    }
}

} // namespace

bool SameGeometry(GpuSceneData const& a, GpuSceneData const& b)
{
    auto same = [](auto const& x, auto const& y) {
        return x.size() == y.size() &&
            (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return same(a.positions, b.positions) && same(a.normals, b.normals) && same(a.texcoords, b.texcoords) &&
        same(a.tangents, b.tangents) && same(a.indices, b.indices) && same(a.prototypes, b.prototypes) &&
        same(a.spheres, b.spheres);
}

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
            instance.info.y |= kInstancePlacement;
            const bool flip = p.flipOrientation ||
                (p.mirrored && geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE);
            if (flip) instance.info.y |= kInstanceFlip;
            if (p.mirrored) instance.info.y |= kInstanceMirrored;
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

    const LightTable lights = BuildLightTable(scene);
    data.lights = lights.records;
    data.lightAlias = lights.alias;
    data.analyticLights = lights.analyticCount;
    for (auto& record : data.lights) {
        if ((record.info.y & kLightTextured) == 0u) continue;
        auto const* image = dynamic_cast<ImageTexture<Color3> const*>(lights.textures[record.info.z].get());
        record.info.z = image ? builder.image(image->resource) : kImageNone;
    }
    for (auto const& [material, emitter] : lights.emitters)
        if (auto index = builder.materialIndex(material))
            data.materials[*index].emitter = {emitter.pdfArea, emitter.twoSided ? 1.0f : 0.0f, 0.0f, 0.0f};
    data.mdlTextures.resize(1);
    return data;
}

void ApplyMdlShaderCode(GpuSceneData& data, MdlShaderCode const& code)
{
    for (auto& material : data.materials) {
        if (material.info.x != kMaterialMdl) continue;
        auto const& record = code.materials.at(material.info.y);
        if (!code.success || !record.valid) {
            material.info.x = kMaterialInvalid;
            continue;
        }
        material.info.y = record.program;
        material.info.z = record.argBlockOffset;
        if (record.constantCutout) {
            material.info.w |= kMaterialConstantCutout;
            material.params.w = *record.constantCutout;
        }
    }
    // Instances whose every material always blocks shadow rays.
    auto blocks = [](GpuMaterial const& m) {
        if (m.info.x == kMaterialMdl) return (m.info.w & kMaterialConstantCutout) != 0u && m.params.w >= 1.0f;
        if (m.info.x == kMaterialFallback) return m.params.x >= 1.0f;
        return true; // legacy materials and invalid MDL materials are opaque
    };
    data.hasCutouts = false;
    for (auto& instance : data.instances) {
        const uint32_t primitives = data.prototypes[instance.info.x].info.w;
        bool opaque = true;
        for (uint32_t p = 0; p < primitives && opaque; ++p)
            opaque = blocks(data.materials[data.primitiveMaterials[instance.info.z + p]]);
        if (opaque) instance.info.y |= kInstanceOpaque;
        else data.hasCutouts = true;
    }

    // Named primvars answer the scene data lookups of their name.
    std::unordered_map<std::string, uint32_t> stringIds;
    for (size_t i = 1; i < code.stringConstants.size(); ++i)
        if (!code.stringConstants[i].empty()) stringIds.emplace(code.stringConstants[i], uint32_t(i));
    for (size_t i = 0; i < data.primvars.size(); ++i) {
        auto found = stringIds.find(data.primvarNames[i]);
        data.primvars[i].info.x = found != stringIds.end() ? found->second : 0u;
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
            record.info = {kMdlTextureImage, uint32_t(data.mdlTiles.size()),
                           uint32_t(uMin), uint32_t(vMin)};
            record.info2 = {uCount, vCount, texture.tiled ? 1u : 0u, 0u};
            const size_t first = data.mdlTiles.size();
            data.mdlTiles.resize(first + size_t(uCount) * vCount, kTileMissing);
            for (auto const& [tile, resource] : texture.tiles)
                data.mdlTiles[first + size_t(tile.second - vMin) * uCount + (tile.first - uMin)] =
                    image(resource);
        } else if (texture.kind == MdlTexture::Kind::BsdfData && texture.values) {
            record.info = {kMdlTextureBsdfData, uint32_t(data.bsdfData.size()),
                           uint32_t(texture.width), uint32_t(texture.height)};
            record.info2 = {uint32_t(texture.depth), uint32_t(texture.components), 0u, 0u};
            data.bsdfData.insert(data.bsdfData.end(), texture.values,
                texture.values + texture.width * texture.height * texture.depth * texture.components);
        }
        data.mdlTextures.push_back(record);
    }
}

} // namespace prism::device::gpu
