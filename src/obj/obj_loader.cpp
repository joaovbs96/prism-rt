#include <prism/obj.h>

#include "../mesh/obj_mesh.h"
#include "../renderer/renderer_context.h"
#include "../scene/scene_internal.h"

#include <map>

namespace prism::obj {

LoadResult Load(Scene& scene, std::filesystem::path const& path)
{
    if (!std::filesystem::exists(path)) throw Error(ErrorCode::NotFound, "OBJ file not found: " + path.string());
    RendererContext& renderer = detail::SceneAccess::context(scene);
    std::shared_ptr<ObjMesh> mesh;
    try {
        mesh = std::make_shared<ObjMesh>(path, !renderer.options().materials.materialxLibraries.empty(),
                                         renderer.materialSettings());
    } catch (Error const&) {
        throw;
    } catch (std::exception const& error) {
        throw Error(ErrorCode::InvalidArgument, error.what());
    }
    if (mesh->vertices.empty() || mesh->indices.empty())
        throw Error(ErrorCode::InvalidArgument, "Scene contains no renderable geometry");

    LoadResult result;
    // Material slots in MTL material order (-1, faces without one, first).
    std::map<int, int> slots;
    std::map<std::string, MaterialId> documents;
    InstanceDesc instance;
    for (auto const& [index, source] : mesh->materialSources()) {
        MaterialId id;
        if (source.desc && source.desc->kind == MaterialDesc::Kind::MaterialXFile) {
            auto& known = documents[source.desc->path];
            if (!known) known = scene.addMaterial(*source.desc);
            id = known;
            ++result.materials;
        } else if (source.desc) {
            id = scene.addMaterial(*source.desc);
        } else {
            id = detail::SceneAccess::addCompiledMaterial(
                scene, std::shared_ptr<Material>(std::shared_ptr<Material>(), ErrorMaterial::Shared()),
                "OBJ material " + std::to_string(index));
            ++result.failedMaterials;
        }
        slots[index] = int(instance.materials.size());
        instance.materials.push_back(id);
    }

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
    auto faceSlots = std::make_shared<std::vector<int32_t>>(triangles, 0);
    for (size_t t = 0; t < triangles; ++t) (*faceSlots)[t] = slots.at(mesh->material_mapping.at(int(t)));
    desc.faceMaterialSlots = BufferView::Of(std::shared_ptr<const std::vector<int32_t>>(faceSlots), 1, DataType::Int32);
    result.mesh = scene.addMesh(desc);
    instance.prototype = result.mesh;
    instance.primId = 0;
    result.instance = scene.addInstance(instance);
    result.triangles = triangles;
    return result;
}

} // namespace prism::obj
