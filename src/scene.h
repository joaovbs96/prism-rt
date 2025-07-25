#pragma once

#include <embree4/rtcore.h>
#include <embree4/rtcore_common.h>

// project libraries
#include "frame_buffer.h"
#include "float3.h"
#include "ray.h"
#include "camera.h"
#include "sampling.h"
#include "material.h"
#include "mesh_load.h"

class SceneManager {
    public:
        SceneManager(RTCDevice &device_) {
            device = &device_;
            scene = rtcNewScene(*device);
        }

        unsigned int add_mesh(const std::filesystem::path &file_path, Material* material) {

            Mesh* mesh;
            if (file_path.extension() == ".obj")
                mesh = new ObjMesh(file_path);
            else if ((file_path.extension() == ".gltf") || (file_path.extension() == ".glb"))
                mesh = new glTFMesh(file_path);

            int geomID = mesh->setupEmbree(*device, scene);

            // collect assigned material - we will gatter it from the geomid
            materials[geomID] = material;

            return geomID;
        }

        unsigned int add_sphere(const Float3& pos, float r, Material* material) {
            RTCGeometry geom = rtcNewGeometry(*device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
            float* vb = (float*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4*sizeof(float), 1);
            vb[0] = pos.x(); vb[1] = pos.y(); vb[2] = pos.z(); vb[3] = r;

            // commit and release geom handle
            rtcCommitGeometry(geom);
            unsigned int geomID = rtcAttachGeometry(scene, geom);
            rtcReleaseGeometry(geom);

            // collect assigned material - we will gatter it from the geomid
            materials[geomID] = material;

            return geomID;
        }

        RayHit cast_ray(RTCRay ray) {
            RTCRayHit e_rayhit;
            e_rayhit.ray = ray;
            e_rayhit.ray.tnear = 0.001f;
            e_rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

            // intersect a single ray with the scene
            rtcIntersect1(scene, &e_rayhit);

            return RayHit(e_rayhit);
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
                Material* material = materials[ray_hit.geomid];
                
                // BRDFSample: surface hit color + scattered light ray
                BRDFSample s = material->sample(ray_hit, seed);

                RTCRay new_ray = initialize_ray(ray_hit.p, s.wi);

                return s.color * trace(new_ray, depth - 1, seed);
            }

            // ray miss
            float a = 0.5 * (ray_hit.d.y() + 1.0f);
            return (1.0-a) * Color3(1.0, 1.0, 1.0) + a * Color3(0.5, 0.7, 1.0);
        }

        RTCDevice* device;
        RTCScene scene;
        std::unordered_map<int, Material*> materials;
        Camera cam;
};

class InOneWeekendFinal : public SceneManager {
    public:
        InOneWeekendFinal(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_sphere(Float3(0, -1000, 0), 1000, new Diffuse(Color3(0.5f)));

            for (int x = -11; x < 11; x++) {
                for (int y = -11; y < 11; y++) {
                    float rnd_ = rnd(seed);
                    Float3 center = Float3(x + 0.9 * rnd(seed), 0.2, y + 0.9 * rnd(seed));

                    if ((center - Float3(4.0, 0.2, 0.0)).length() > 0.9f) {
                        if (rnd_ < 0.8f) {
                            add_sphere(center, 0.2f, new Diffuse(Color3(rnd(seed), rnd(seed), rnd(seed))));
                        } else if (rnd_ < 0.95f) {
                            add_sphere(center, 0.2f, new Metal(Color3(rnd(seed), rnd(seed), rnd(seed)), rnd(seed)));
                        } else {
                            add_sphere(center, 0.2f, new Glass(1.5f));
                        }
                    }
                }
            }

            add_sphere(Float3(0, 1, 0), 1.0f, new Glass(1.5f));
            add_sphere(Float3(-4, 1, 0), 1.0f, new Diffuse(Color3(0.4, 0.2, 0.1)));
            add_sphere(Float3(4, 1, 0), 1.0f, new Metal(Color3(0.7, 0.6, 0.5), 0.0f));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(13, 2, 3);
            Float3 target(0.0f);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 20;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.6f, 10.0f);
        }
};

class ABeautifulGame : public SceneManager {
    public:
        ABeautifulGame(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh("C:/Users/joaov/Desktop/ABeautifulGame.glb", new Diffuse(Color3(0.5, 0.5, 0.5)));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(0.8, 0.15, 0.7);
            Float3 target(0.0f, 0.05, 0.0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 12;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);
        }
};

class Duck : public SceneManager {
    public:
        Duck(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh("C:/Users/joaov/Desktop/duck_reset.glb", new Diffuse(Color3(0.5, 0.5, 0.5)));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(11, 2, 7.5);
            Float3 target(0.0f, 0.85, 0.0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 8;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);
        }
};

class Duck2 : public SceneManager {
    public:
        Duck2(RTCDevice device_, unsigned int &seed) : SceneManager(device_) {
            std::cout << "Started scene initialization." << std::endl;
            add_mesh("C:/Users/joaov/Desktop/duck.obj", new Diffuse(Color3(0.5, 0.5, 0.5)));

            std::cout << "Finished geometry creation." << std::endl;

            // camera initialization
            Float3 position(11, 2, 7.5);
            Float3 target(0.0f, 0.85, 0.0);
            Float3 up(0.0f, 1.0f, 0.0f);
            float vfov = 8;
            cam = Camera(position, target, up, 1280, 720, 16.0f / 9.0f, vfov, 0.0f, 10.0f);
        }
};