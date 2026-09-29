#include "../src/scene.h"
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool yes, const char* reason) { if (!yes) throw std::runtime_error(reason); }
std::string read(const std::filesystem::path& file) {
    std::ifstream input(file); return {std::istreambuf_iterator<char>(input), {}};
}
void verify(SceneManager& source, const std::filesystem::path& output) {
    source.exportToUSDA(output);
    const auto first = read(output);
    source.exportToUSDA(output);
    require(first == read(output), "USDA export must be byte deterministic");
    const auto stage = UsdStage::Open(output.string());
    require(bool(stage), "Export must be readable by stock USD");
    for (const auto& [id, geometry] : source.geometry) {
        auto prim = stage->GetPrimAtPath(SdfPath("/World/Geometry/Object_"+std::to_string(id)));
        require(bool(prim), "Every source geometry must be exported");
        Material* original = nullptr;
        if (auto sphere = dynamic_cast<Sphere*>(geometry)) {
            UsdGeomSphere authored(prim);
            require(bool(authored), "Sphere must remain a USD sphere prim");
            double radius = 0; authored.GetRadiusAttr().Get(&radius);
            require(radius == sphere->r, "Sphere radius changed");
            VtVec3fArray extent;
            require(authored.GetExtentAttr().HasAuthoredValueOpinion() &&
                    authored.GetExtentAttr().Get(&extent) && extent.size() == 2,
                    "Authored sphere radius requires an authored local extent");
            require(extent[0] == GfVec3f(-sphere->r) && extent[1] == GfVec3f(sphere->r),
                    "Sphere bounds must match its radius, including the large ground");
            original = sphere->material;
        } else if (auto quad = dynamic_cast<Quad*>(geometry)) original = quad->material;
        else if (auto box = dynamic_cast<Box*>(geometry)) original = box->material;
        auto bound = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        require(bool(bound), "Exported geometry must have a material binding");
        UsdShadeShader shader(stage->GetPrimAtPath(bound.GetPath().AppendChild(TfToken("Surface"))));
        TfToken shaderId; shader.GetIdAttr().Get(&shaderId);
        require(shaderId == TfToken("ND_open_pbr_surface_surfaceshader"), "Expected embedded OpenPBR shader");
        Color3 expected;
        bool hasBase = false;
        if (auto diffuse = dynamic_cast<Diffuse*>(original)) {
            expected = diffuse->diffuse_color->sample(0, 0, Float3(0)); hasBase = true;
        } else if (auto metal = dynamic_cast<Metal*>(original)) {
            expected = metal->specular_color->sample(0, 0, Float3(0)); hasBase = true;
        }
        if (hasBase) {
            GfVec3f color; shader.GetInput(TfToken("base_color")).Get(&color);
            require(color == GfVec3f(expected.x(), expected.y(), expected.z()),
                    "Export must preserve source float RGB exactly");
        }
    }
    const auto& description = source.cam.getDescription();
    UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath("/World/Camera")));
    const auto c = camera.GetCamera(UsdTimeCode::Default());
    const auto eye = c.GetTransform().ExtractTranslation();
    require((eye-GfVec3d(description.position.x(),description.position.y(),description.position.z())).GetLength()<1e-5,
            "Camera position changed");
    const double vfov = 2*std::atan(c.GetVerticalAperture()/(2*c.GetFocalLength()))*180/PI;
    require(std::abs(vfov-description.verticalFov)<1e-4, "Camera field of view changed");
    require(std::abs(c.GetHorizontalAperture()/c.GetVerticalAperture()-description.aspect)<1e-5,
            "Camera aspect changed");
    require(c.GetFocusDistance()==description.focusDistance, "Camera focus changed");
    const double radius = c.GetFStop()>0 ? c.GetFocalLength()*.1/(2*c.GetFStop()) : 0;
    require(std::abs(radius-description.focusDistance*std::tan(deg2rad(description.defocusAngle*.5f)))<1e-5,
            "Camera aperture radius changed");
    const auto inverseProjection=c.GetFrustum().ComputeProjectionMatrix().GetInverse();
    for (const auto pixel : {GfVec2i(0,0),GfVec2i(description.width/2,description.height/2),
                            GfVec2i(description.width-1,description.height-1)}) {
        unsigned originalSeed=1234, exportedSeed=originalSeed;
        const auto original=source.cam.get_ray(float(pixel[0]),float(pixel[1]),originalSeed);
        // Replay the original camera's jitter order; this checks projection
        // and aperture rather than compiler-specific argument evaluation order.
        const auto offset=source.cam.sample_square(exportedSeed);
        const double u=(pixel[0]+.5+offset.x())/description.width;
        const double v=1-(pixel[1]+.5+offset.y())/description.height;
        GfVec3d direction=inverseProjection.Transform(GfVec3d(2*u-1,2*v-1,-1)).GetNormalized();
        GfVec3d origin(0);
        if(radius>0) {
            const auto disk=random_in_unit_disk(exportedSeed);
            const auto target=direction*(-c.GetFocusDistance()/direction[2]);
            origin=GfVec3d(disk.x()*radius,disk.y()*radius,0);
            direction=(target-origin).GetNormalized();
        }
        origin=c.GetTransform().Transform(origin);
        direction=c.GetTransform().TransformDir(direction).GetNormalized();
        const GfVec3d originalOrigin(original.org_x,original.org_y,original.org_z);
        const GfVec3d originalDirection=GfVec3d(original.dir_x,original.dir_y,original.dir_z).GetNormalized();
        require((origin-originalOrigin).GetLength()<1e-4 &&
                (direction-originalDirection).GetLength()<1e-5,
                "Exported camera rays changed framing or depth of field");
    }
    if (auto gradient = dynamic_cast<GradientBackground*>(source.environment)) {
        UsdLuxDomeLight dome(stage->GetPrimAtPath(SdfPath("/World/Environment")));
        SdfAssetPath texturePath; dome.GetTextureFileAttr().Get(&texturePath);
        require(!texturePath.GetResolvedPath().empty(), "Gradient sky must resolve relative to USDA");
        ImageTexture<Color3> texture(texturePath.GetResolvedPath());
        EnvironmentMap exported(&texture, Color3(1));
        for (int y=1;y<128;++y) {
            const float latitude = PI*y/128;
            const Float3 direction(std::sin(latitude),std::cos(latitude),0);
            const auto error = exported.radiance(direction)-gradient->radiance(direction);
            require(error.length()<0.002, "Baked gradient radiance deviates from source");
        }
    }
}
}
int main() {
    try {
        RTCDevice device = rtcNewDevice(nullptr);
        require(device != nullptr, "Embree device failed");
        const auto directory = std::filesystem::current_path()/"scene-export-test";
        std::filesystem::create_directories(directory);
        unsigned seed=0;
        InOneWeekendFinal weekend(device,seed); verify(weekend,directory/"weekend.usda");
        CornellBox cornell(device,seed); verify(cornell,directory/"cornell.usda");
        QuadTest quad(device,seed); verify(quad,directory/"quads.usda");
        rtcReleaseDevice(device);
        std::cout << "procedural scene export tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
