#include "scene.h"

#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdShade/shader.h>
#include <map>

namespace {
GfVec3f vec(const Float3& v) { return GfVec3f(v.x(), v.y(), v.z()); }
GfVec3d vecd(const Float3& v) { return GfVec3d(v.x(), v.y(), v.z()); }

Color3 constant(Texture<Color3>* texture) {
    auto value = dynamic_cast<ValueTexture<Color3>*>(texture);
    if (!value) throw std::runtime_error("Procedural export requires constant material colors");
    return value->value;
}

UsdShadeMaterial exportMaterial(const UsdStageRefPtr& stage, const SdfPath& path,
                               Material* source) {
    auto material = UsdShadeMaterial::Define(stage, path);
    auto shader = UsdShadeShader::Define(stage, path.AppendChild(TfToken("Surface")));
    shader.CreateIdAttr(VtValue(TfToken("ND_open_pbr_surface_surfaceshader")));
    auto scalar = [&](const char* name, float value) {
        shader.CreateInput(TfToken(name), SdfValueTypeNames->Float).Set(value);
    };
    auto color = [&](const char* name, Color3 value) {
        shader.CreateInput(TfToken(name), SdfValueTypeNames->Color3f).Set(vec(value));
    };
    // Explicit values avoid accidental energy from OpenPBR's default lobes.
    scalar("base_weight", 1); scalar("specular_weight", 0);
    scalar("base_metalness", 0); scalar("transmission_weight", 0);
    scalar("emission_luminance", 0); scalar("coat_weight", 0);
    if (auto diffuse = dynamic_cast<Diffuse*>(source)) {
        color("base_color", constant(diffuse->diffuse_color));
        scalar("base_diffuse_roughness", 0);
    } else if (auto metal = dynamic_cast<Metal*>(source)) {
        color("base_color", constant(metal->specular_color));
        scalar("base_metalness", 1); scalar("specular_weight", 1);
        auto roughness = dynamic_cast<ValueTexture<float>*>(metal->specular_roughness);
        if (!roughness) throw std::runtime_error("Procedural export requires constant roughness");
        scalar("specular_roughness", roughness->value);
    } else if (auto glass = dynamic_cast<Glass*>(source)) {
        color("base_color", Color3(1)); color("transmission_color", Color3(1));
        scalar("transmission_weight", 1); scalar("specular_weight", 1);
        scalar("specular_roughness", 0); scalar("specular_ior", glass->getIor());
    } else if (auto emission = dynamic_cast<Emissive*>(source)) {
        scalar("base_weight", 0);
        color("emission_color", constant(emission->emissive_color));
        scalar("emission_luminance", emission->emissive_multiplier);
    } else throw std::runtime_error("Unsupported procedural material in USDA export");
    auto output = shader.CreateOutput(TfToken("out"), SdfValueTypeNames->Token);
    material.CreateSurfaceOutput(TfToken("mtlx")).ConnectToSource(output);
    return material;
}

void exportSky(const UsdStageRefPtr& stage, const std::filesystem::path& path,
               Environment* environment) {
    if (!environment) return;
    auto dome = UsdLuxDomeLight::Define(stage, SdfPath("/World/Environment"));
    dome.CreateIntensityAttr(VtValue(1.0f));
    if (auto solid = dynamic_cast<ConstantColorBackground*>(environment)) {
        dome.CreateColorAttr(VtValue(vec(constant(solid->texture))));
        return;
    }
    auto gradient = dynamic_cast<GradientBackground*>(environment);
    if (!gradient) throw std::runtime_error("Unsupported procedural environment in USDA export");
    const Color3 bottom = constant(gradient->t0), top = constant(gradient->t1);
    constexpr int width = 32, height = 2048;
    std::vector<float> pixels(width * height * 3);
    for (int y = 0; y < height; ++y) {
        const float t = 0.5f * (1.0f + std::cos(PI * (y + 0.5f) / height));
        const Color3 c = bottom * (1.0f - t) + top * t;
        for (int x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel)
                pixels[(y * width + x) * 3 + channel] = c[channel];
    }
    const auto skyPath = path.parent_path() / (path.stem().string() + "_sky.exr");
    OIIO::ImageBuf image(OIIO::ImageSpec(width, height, 3, OIIO::TypeDesc::FLOAT));
    image.set_pixels(OIIO::ROI(), OIIO::TypeDesc::FLOAT, pixels.data());
    if (!image.write(skyPath.string())) throw std::runtime_error(image.geterror());
    dome.CreateColorAttr(VtValue(GfVec3f(1)));
    dome.CreateTextureFileAttr(VtValue(SdfAssetPath(skyPath.filename().generic_string())));
    dome.CreateTextureFormatAttr(VtValue(TfToken("latlong")));
}
}

void SceneManager::exportToUSDA(const std::filesystem::path& path) const {
    if (path.extension() != ".usda") throw std::runtime_error("Export output must end in .usda");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto stage = UsdStage::CreateInMemory();
    auto world = UsdGeomXform::Define(stage, SdfPath("/World"));
    stage->SetDefaultPrim(world.GetPrim());
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
    UsdGeomXform::Define(stage, SdfPath("/World/Geometry"));
    stage->DefinePrim(SdfPath("/World/Materials"), TfToken("Scope"));
    // Embree IDs follow construction order; never serialize unordered-map order.
    const std::map<int, Geometry*> ordered(geometry.begin(), geometry.end());
    std::map<Material*, UsdShadeMaterial> materials;
    for (const auto& [id, object] : ordered) {
        const SdfPath primPath("/World/Geometry/Object_" + std::to_string(id));
        UsdPrim prim;
        Material* source = nullptr;
        GfMatrix4d transform(1);
        const float* m = object->transform->getMatrix();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) transform[r][c] = m[c * 4 + r];
        if (auto sphere = dynamic_cast<Sphere*>(object)) {
            auto usdSphere = UsdGeomSphere::Define(stage, primPath);
            usdSphere.CreateRadiusAttr(VtValue(double(sphere->r)));
            VtVec3fArray extent;
            UsdGeomSphere::ComputeExtent(double(sphere->r), &extent);
            usdSphere.CreateExtentAttr(VtValue(extent));
            GfMatrix4d translation(1); translation.SetTranslate(vecd(sphere->pos));
            transform = translation * transform;
            prim = usdSphere.GetPrim(); source = sphere->material;
        } else {
            VtVec3fArray points;
            VtIntArray counts, indices;
            if (auto quad = dynamic_cast<Quad*>(object)) {
                points = {vec(quad->c1), vec(quad->c2), vec(quad->c4), vec(quad->c3)};
                counts = {4}; indices = {0, 1, 2, 3}; source = quad->material;
            } else if (auto box = dynamic_cast<Box*>(object)) {
                for (auto const& p : box->vertices) points.push_back(vec(p));
                counts = {4, 4, 4, 4, 4, 4};
                indices = {4,5,6,7, 1,0,3,2, 5,1,2,6, 0,4,7,3, 3,7,6,2, 0,1,5,4};
                source = box->material;
            } else throw std::runtime_error("Unsupported procedural geometry in USDA export");
            auto mesh = UsdGeomMesh::Define(stage, primPath);
            mesh.CreatePointsAttr(VtValue(points));
            mesh.CreateFaceVertexCountsAttr(VtValue(counts));
            mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
            mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
            VtVec3fArray normals;
            size_t first = 0;
            for (int count : counts) {
                auto n = GfCross(points[indices[first+1]] - points[indices[first]],
                                 points[indices[first+2]] - points[indices[first]]).GetNormalized();
                for (int corner = 0; corner < count; ++corner) normals.push_back(n);
                first += count;
            }
            mesh.CreateNormalsAttr(VtValue(normals));
            mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
            prim = mesh.GetPrim();
        }
        UsdGeomXformable(prim).AddTransformOp().Set(transform);
        auto found = materials.find(source);
        if (found == materials.end()) {
            auto matPath = SdfPath("/World/Materials/Material_" + std::to_string(materials.size()));
            found = materials.emplace(source, exportMaterial(stage, matPath, source)).first;
        }
        UsdShadeMaterialBindingAPI::Apply(prim).Bind(found->second);
    }
    const auto& c = cam.getDescription();
    auto camera = UsdGeomCamera::Define(stage, SdfPath("/World/Camera"));
    GfMatrix4d view(1); view.SetLookAt(vecd(c.position), vecd(c.target), vecd(c.up));
    camera.AddTransformOp().Set(view.GetInverse());
    constexpr float focal = 50;
    const float aperture = 2 * focal * std::tan(deg2rad(c.verticalFov * 0.5f));
    camera.CreateFocalLengthAttr(VtValue(focal));
    camera.CreateVerticalApertureAttr(VtValue(aperture));
    camera.CreateHorizontalApertureAttr(VtValue(aperture * c.aspect));
    camera.CreateFocusDistanceAttr(VtValue(c.focusDistance));
    const float radius = c.focusDistance * std::tan(deg2rad(c.defocusAngle * 0.5f));
    camera.CreateFStopAttr(VtValue(radius > 0 ? focal * 0.1f / (2 * radius) : 0.0f));
    camera.CreateClippingRangeAttr(VtValue(GfVec2f(0.001f, 1e30f)));
    auto settings = UsdRenderSettings::Define(stage, SdfPath("/Render/Settings"));
    settings.CreateResolutionAttr(VtValue(GfVec2i(c.width, c.height)));
    settings.CreateCameraRel().SetTargets({camera.GetPath()});
    stage->SetMetadata(TfToken("renderSettingsPrimPath"), VtValue(settings.GetPath().GetString()));
    exportSky(stage, path, environment);
    if (!stage->GetRootLayer()->Export(path.string()))
        throw std::runtime_error("Cannot export USDA: " + path.string());
}
