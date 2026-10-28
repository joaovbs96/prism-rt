#include <prism/gltf.h>

#include "../mesh/gltf_mesh.h"
#include "../renderer/renderer_context.h"
#include "../scene/scene_internal.h"

#include <map>

namespace prism::gltf {

LoadResult Load(Scene& scene, std::filesystem::path const& path)
{
    if (!std::filesystem::exists(path)) throw Error(ErrorCode::NotFound, "glTF file not found: " + path.string());
    RendererContext& renderer = detail::SceneAccess::context(scene);
    std::shared_ptr<glTFMesh> mesh;
    try {
        mesh = std::make_shared<glTFMesh>(path, !renderer.options().materials.materialxLibraries.empty(),
                                          renderer.materialSettings());
    } catch (Error const&) {
        throw;
    } catch (std::exception const& error) {
        throw Error(ErrorCode::InvalidArgument, error.what());
    }
    if (mesh->vertices.empty() || mesh->indices.empty())
        throw Error(ErrorCode::InvalidArgument, "Scene contains no renderable geometry");

    LoadResult result;
    // Material slots in glTF material order (-1, the default material, first).
    std::map<int, int> slots;
    std::map<std::string, MaterialId> documents;
    InstanceDesc instance;
    for (auto const& [index, source] : mesh->materialSources()) {
        MaterialId id;
        if (source.desc) {
            auto& known = documents[source.desc->path];
            if (!known) known = scene.addMaterial(*source.desc);
            id = known;
            ++result.materials;
        } else {
            id = detail::SceneAccess::addCompiledMaterial(
                scene, std::shared_ptr<Material>(std::shared_ptr<Material>(), ErrorMaterial::Shared()),
                "glTF material " + std::to_string(index));
            ++result.failedMaterials;
        }
        slots[index] = int(instance.materials.size());
        instance.materials.push_back(id);
    }

    // The arrays stay with the reader (shared), which the scene holds until
    // it is replaced.
    auto owned = [&](auto const& vector) { return std::shared_ptr<const void>(mesh, vector.data()); };
    const size_t triangles = mesh->indices.size() / 3;
    MeshDesc desc;
    BufferView points;
    points.data = mesh->vertices.data();
    points.count = mesh->vertices.size();
    points.components = 3;
    points.strideBytes = sizeof(Float3);
    points.owner = owned(mesh->vertices);
    desc.points = Constant(points);
    BufferView indices;
    indices.data = mesh->indices.data();
    indices.count = mesh->indices.size();
    indices.type = DataType::UInt32;
    indices.owner = owned(mesh->indices);
    desc.faceVertexIndices = indices;
    BufferView normals = points;
    normals.data = mesh->normals.data();
    normals.count = mesh->normals.size();
    normals.owner = owned(mesh->normals);
    desc.normals = normals;
    desc.normalsInterpolation = Interpolation::Vertex;
    BufferView uvs;
    uvs.data = mesh->texcoords.data();
    uvs.count = mesh->texcoords.size();
    uvs.components = 2;
    uvs.strideBytes = sizeof(Float2);
    uvs.owner = owned(mesh->texcoords);
    desc.uvs = uvs;
    desc.uvsInterpolation = Interpolation::Vertex;
    // Interleaved primvars (TEXCOORD_1, COLOR_0) as one array each.
    for (auto const& primvar : mesh->primvars) {
        auto values = std::make_shared<std::vector<float>>(mesh->vertices.size() * size_t(primvar.components));
        for (size_t v = 0; v < mesh->vertices.size(); ++v)
            for (int c = 0; c < primvar.components; ++c)
                (*values)[v * size_t(primvar.components) + size_t(c)] =
                    mesh->primvarValues[v * size_t(mesh->primvarStride) + size_t(primvar.offset) + size_t(c)];
        Primvar data;
        data.name = primvar.name;
        data.interpolation = Interpolation::Vertex;
        data.data = BufferView::Of(std::shared_ptr<const std::vector<float>>(values), uint32_t(primvar.components),
                                   DataType::Float32);
        desc.primvars.push_back(std::move(data));
    }
    auto faceSlots = std::make_shared<std::vector<int32_t>>(triangles, 0);
    for (size_t t = 0; t < triangles; ++t) (*faceSlots)[t] = slots.at(mesh->material_mapping.at(int(t)));
    desc.faceMaterialSlots = BufferView::Of(std::shared_ptr<const std::vector<int32_t>>(faceSlots), 1, DataType::Int32);
    if (mesh->single_sided_all) {
        desc.doubleSided = false;
    } else if (!mesh->single_sided.empty()) {
        desc.singleSidedFaces = BufferView::Of(
            std::shared_ptr<const std::vector<uint8_t>>(mesh, &mesh->single_sided), 1, DataType::UInt8);
    }
    result.mesh = scene.addMesh(desc);
    instance.prototype = result.mesh;
    instance.primId = 0;
    result.instance = scene.addInstance(instance);
    result.triangles = triangles;
    return result;
}

} // namespace prism::gltf
