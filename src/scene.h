#pragma once

#include <cstdlib>

#include <embree4/rtcore.h>
#include <embree4/rtcore_common.h>

// project libraries
#include "frame_buffer.h"
#include "float3.h"
#include "ray.h"
#include "camera.h"
#include "sampling.h"
#include "material.h"
#include "geometry.h"
#include "mesh/obj_mesh.h"
#include "mesh/gltf_mesh.h"
#include "mesh/usd_mesh.h"
#include "environment.h"

// TODO: for motion blur, we need to add a new copy of the vertexbuffer per timestep
// so we need to have the vertexbuffer and apply some sort of transformation to the buffer
// The easiest way would be to make all meshes be objects, like our mesh loaders,
// and add functions that operate on top of the buffers - e.g. rotating, translating, scaling.
// We need to add these, regardless, so it's better to do the two things at once.
// We can, then, add the transforms at a certain timestep, which will create the additional
// buffers, if the timestep is different than 0.

class SceneManager {
    public:
        SceneManager(RTCDevice &device_) {
            device = &device_;
            scene = rtcNewScene(*device);

            assets_path = std::filesystem::path(std::getenv("PRISM_ASSETS_DIR"));

            MODELS = assets_path / "models";
            TEXTURES = assets_path / "textures";
            ENVIRONMENT = assets_path / "environment";
        }

        Geometry* add_mesh(const std::filesystem::path &file_path, Material* material_override = nullptr) {

            Mesh* mesh;
            if (file_path.extension() == ".obj")
                mesh = new ObjMesh(file_path);
            else if ((file_path.extension() == ".gltf") || (file_path.extension() == ".glb"))
                mesh = new glTFMesh(file_path);
            else if ((file_path.extension() == ".usd") || (file_path.extension() == ".usda") || (file_path.extension() == ".usdz") || (file_path.extension() == ".usdc"))
                mesh = new USDMesh(file_path);

            if (material_override != nullptr) {
                mesh->material_override = material_override;
            }

            int geomID = mesh->setupEmbree(*device, scene);
            geometry[geomID] = mesh;
            return mesh;
        }

        Geometry* add_sphere(const Float3& pos, float r, Material* material) {
            Sphere* sphere = new Sphere(pos, r, material);
            int geomID = sphere->setupEmbree(*device, scene);
            geometry[geomID] = sphere;
            return sphere;
        }

        Geometry* add_quad(const Float3& q, const Float3& u, const Float3& v, Material* material_) {
            Quad* quad = new Quad(q, u, v, material_);
            int geomID = quad->setupEmbree(*device, scene);
            geometry[geomID] = quad;
            return quad;
        }

        Geometry* add_box(const Float3& a, const Float3& b, Material* material_){
            Box* box = new Box(a, b, material_);
            int geomID = box->setupEmbree(*device, scene);
            geometry[geomID] = box;
            return box;
        }

        RayHit cast_ray(RTCRay ray) {
            RTCRayHit e_rayhit;
            e_rayhit.ray = ray;
            e_rayhit.ray.tnear = 0.001f;
            e_rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

            // intersect a single ray with the scene
            rtcIntersect1(scene, &e_rayhit);

            if (e_rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID)
                return RayHit(e_rayhit);
            else {
                Geometry* geom = geometry[e_rayhit.hit.geomID];
                return RayHit(e_rayhit, &geom->geom, geom->type);
            }
        }

        Color3 trace(RTCRay ray, int depth, unsigned int &seed) {
            // reached maximum recursion
            if (depth <= 0)
                return Float3();

            // cast ray and get hit record
            RayHit ray_hit = cast_ray(ray);

            // ray hit
            if (ray_hit.geomid != RTC_INVALID_GEOMETRY_ID){
                // retrieve material by geomid and sample the surface
                Material* material = geometry[ray_hit.geomid]->getMaterial(ray_hit.primid, ray_hit.instid);
                // sample normal map and perturbate normal
                material->perturbNormal(ray_hit);

                // BRDFSample: surface hit color + scattered light ray
                BRDFSample s = material->sample(ray_hit, seed);
                if (!s.scattered)
                    return s.emission;

                RTCRay new_ray = initialize_ray(ray_hit.p, s.wi);

                return s.emission + s.color * trace(new_ray, depth - 1, seed);
            }

            // ray miss: sample environment
            return environment->sample(ray_hit);
        }

        void commitScene() {
            // FIXME: commented out, transforms don't seem to be working atm
            // setup scene geometry transformation
            for (auto it = geometry.begin(); it != geometry.end(); it++) {
                // TODO: we will need to turn the transforms into vectors, where each time entry will have its own transform
                it->second->applyTransform();
            }

            // finalize scene setup
            rtcCommitScene(scene);
        }

        RTCDevice* device;
        RTCScene scene;
        std::unordered_map<int, Geometry*> geometry;
        Environment* environment;
        Camera cam;
        int image_width = 1280;
        int image_height = 720;
        std::filesystem::path assets_path;
        std::filesystem::path MODELS;
        std::filesystem::path TEXTURES;
        std::filesystem::path ENVIRONMENT;
};

class Earth : public SceneManager {
    public:
    Earth(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            add_sphere(Float3(0, 0, 0), 2.0f, new Diffuse(new ImageTexture(TEXTURES / "earthmap.jpg")));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(0, 0, 12);
            Float3 target(0.0f);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 20;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

            // environment map - for missed rays
            environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
    }
};

class InOneWeekendFinal : public SceneManager {
    public:
        InOneWeekendFinal(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_sphere(Float3(0, -1000, 0), 1000, new Diffuse(new ColorTexture(Color3(0.5f))));

            for (int x = -11; x < 11; x++) {
                for (int y = -11; y < 11; y++) {
                    float rnd_ = rnd(seed);
                    Float3 center = Float3(x + 0.9 * rnd(seed), 0.2, y + 0.9 * rnd(seed));

                    if ((center - Float3(4.0, 0.2, 0.0)).length() > 0.9f) {
                        if (rnd_ < 0.8f) {
                            add_sphere(center, 0.2f, new Diffuse(new ColorTexture(Color3(rnd(seed), rnd(seed), rnd(seed)))));
                        } else if (rnd_ < 0.95f) {
                            add_sphere(center, 0.2f, new Metal(new ColorTexture(Color3(rnd(seed), rnd(seed), rnd(seed))), new ValueTexture<float>(rnd(seed))));
                        } else {
                            add_sphere(center, 0.2f, new Dispersive(2.417f, 55.0f));
                        }
                    }
                }
            }

            add_sphere(Float3(0, 1, 0), 1.0f, new Glass(1.5f));
            add_sphere(Float3(-4, 1, 0), 1.0f, new Diffuse(new ColorTexture(Color3(0.4, 0.2, 0.1))));
            add_sphere(Float3(4, 1, 0), 1.0f, new Diffuse(new ImageTexture(TEXTURES / "earthmap.jpg")));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(13, 2, 3);
            Float3 target(0.0f);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 20;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.6f, 10.0f);

            // environment map - for missed rays
            environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
        }
};

class ABeautifulGame : public SceneManager {
    public:
        ABeautifulGame(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh(MODELS / "ABeautifulGame" / "ABeautifulGame.gltf");

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(0.8, 0.15, 0.7);
            Float3 target(0.0f, 0.05, 0.0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 12;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

            // environment map - for missed rays
            ImageTexture<Float3>* texture = new ImageTexture<Float3>(ENVIRONMENT / "IndoorEnvironmentHDRI003_4K-HDR.exr");
            texture->adjustExposure(0.5f);
            environment = new EnvironmentMap(texture);
        }
};

class NormalTangentTest : public SceneManager {
    public:
        NormalTangentTest(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh(MODELS / "NormalTangentTest.glb");

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(0.8, 0.15, 0.7);
            Float3 target(0.0f, 0.05, 0.0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 12;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

            // environment map - for missed rays
            environment = new EnvironmentMap(new ImageTexture(ENVIRONMENT / "brown_photostudio_03_4k.exr"));
        }
};

class Duck : public SceneManager {
    public:
        Duck(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh(MODELS / "Duck" / "duck.glb");

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(11, 2, 7.5);
            Float3 target(0.0f, 0.85, 0.0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 8;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);

            // environment map - for missed rays
            environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
        }
};

class QuadTest : public SceneManager {
    public:
        QuadTest(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_quad(Float3(-3,-2, 5), Float3(0, 0,-4), Float3(0, 4, 0), new Diffuse(new ColorTexture(Color3(0.2, 1.0, 0.2))));
            add_quad(Float3(-2,-2, 0), Float3(4, 0, 0), Float3(0, 4, 0), new Diffuse(new ColorTexture(Color3(0.2, 1.0, 0.2))));
            add_quad(Float3( 3,-2, 1), Float3(0, 0, 4), Float3(0, 4, 0), new Diffuse(new ColorTexture(Color3(0.2, 1.0, 0.2))));
            add_quad(Float3(-2, 3, 1), Float3(4, 0, 0), Float3(0, 0, 4), new Diffuse(new ColorTexture(Color3(0.2, 1.0, 0.2))));
            add_quad(Float3(-2,-3, 5), Float3(4, 0, 0), Float3(0, 0,-4), new Diffuse(new ColorTexture(Color3(0.2, 1.0, 0.2))));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            image_width = 1280;
            image_height = 1280;
            Float3 position(0.0, 0.0, 9.0);
            Float3 target(0.0f);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 80;
            cam = Camera(position, target, up, image_width, image_height, float(image_width) / image_height, vfov, 0.0f, 10.0f);

            // environment map - for missed rays
            environment = new GradientBackground(new ColorTexture(Color3(1.0, 1.0, 1.0)), new ColorTexture(Color3(0.5, 0.7, 1.0)));
        }
};

class CornellBox : public SceneManager {
    public:
        CornellBox(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            Material* red = new Diffuse(new ColorTexture(Color3(.65, .05, .05)));
            Material* white = new Diffuse(new ColorTexture(Color3(.73, .73, .73)));
            Material* green = new Diffuse(new ColorTexture(Color3(.12, .45, .15)));
            Material* light = new Emissive(new ColorTexture(Color3(1.0f)), 15.f);

            add_quad(Float3(555,0,0),       Float3(0,555,0),    Float3(0,0,555),    green);
            add_quad(Float3(0,0,0),         Float3(0,555,0),    Float3(0,0,555),    red);
            add_quad(Float3(343, 554, 332), Float3(-130,0,0),   Float3(0,0,-105),   light);
            add_quad(Float3(0,0,0),         Float3(555,0,0),    Float3(0,0,555),    white);
            add_quad(Float3(555,555,555),   Float3(-555,0,0),   Float3(0,0,-555),   white);
            add_quad(Float3(0,0,555),       Float3(555,0,0),    Float3(0,555,0),    white);

            Geometry* box1 = add_box(Float3(0.f),     Float3(165, 330, 165),  white);
            box1->transform->translate(265,0,295);
            box1->transform->rotateY(15);

            Geometry* box2 = add_box(Float3(0.0f),    Float3(165,165,165),  white);
            box2->transform->rotateY(-18);
            box2->transform->translate(130, 0, 65);

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            image_width = 1280;
            image_height = 1280;
            Float3 position(278, 278, -800);
            Float3 target(278, 278, 0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 40;
            cam = Camera(position, target, up, image_width, image_height, float(image_width) / image_height, vfov, 0.0f, 10.0f);

            // environment map - for missed rays
            environment = new ConstantColorBackground(new ColorTexture(Color3(0.0f)));
        }
};

class USDCube : public SceneManager {
    public:
        USDCube(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            Material* white = new Diffuse(new ColorTexture(Color3(.73, .73, .73)));
            add_mesh(MODELS / "cube.usdc", white);

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(13, 2, 3);
            Float3 target(0.0f);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 20;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.6f, 10.0f);

            // environment map - for missed rays
            environment = new ConstantColorBackground(new ColorTexture(Color3(1.0f)));
        }
};

class PixarKitchen : public SceneManager {
    public:
        PixarKitchen(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh(MODELS / "Kitchen_set" / "Kitchen_set.usd");

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(75, -385, 120);
            Float3 target(75, 0.0f, 120);
            Float3 up(0.0f, 0.0f, 1.0f);
            float vfov = 60;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.f, 10.0f);

            // environment map - for missed rays
            environment = new ConstantColorBackground(new ColorTexture(Color3(1.0f)));
        }
};
