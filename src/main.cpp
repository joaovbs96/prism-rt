#include <iostream>
#include <format>

// external libraries
#include <embree4/rtcore.h>
#include <xmmintrin.h>

// needed for embree initialization
#if !defined(_MM_SET_DENORMALS_ZERO_MODE)
#define _MM_DENORMALS_ZERO_ON   (0x0040)
#define _MM_DENORMALS_ZERO_OFF  (0x0000)
#define _MM_DENORMALS_ZERO_MASK (0x0040)
#define _MM_SET_DENORMALS_ZERO_MODE(x) (_mm_setcsr((_mm_getcsr() & ~_MM_DENORMALS_ZERO_MASK) | (x)))
#endif

// project libraries
#include "frame_buffer.h"
#include "float3.h"
#include "camera.h"
#include "sampling.h"
#include "material.h"

RTCDevice device;

void error_callback(void* user_ptr, enum RTCError error, const char* str)
{
    printf("ERROR %d: %s\n", error, str);
}

class SceneManager {
    public:
        SceneManager(RTCDevice device) {
            scene = rtcNewScene(device);

            // TODO: initialize material structure
        }

        unsigned int add_sphere(const Float3& pos, float r, Material* material) {
            RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
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

        RTCRayHit cast_ray(RTCRay ray) {
            RTCRayHit rayhit;
            rayhit.ray = ray;
            rayhit.ray.tnear = 0.001f;
            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

            // intersect a single ray with the scene
            rtcIntersect1(scene, &rayhit);

            return rayhit;
        }

        Color3 trace(RTCRay ray, int depth) {
            // reached maximum recursion
            if (depth <= 0)
                return Float3();

            // cast ray and get hit record
            RTCRayHit ray_hit = cast_ray(ray);

            // ray hit
            if (ray_hit.hit.geomID != RTC_INVALID_GEOMETRY_ID){
                Float3 hit_n = Float3(ray_hit.hit.Ng_x, ray_hit.hit.Ng_y, ray_hit.hit.Ng_z).normalize();
                Float3 ray_o(ray_hit.ray.org_x, ray_hit.ray.org_y, ray_hit.ray.org_z);
                Float3 ray_d(ray_hit.ray.dir_x, ray_hit.ray.dir_y, ray_hit.ray.dir_z);
                Float3 hit_p = ray_o + ray_hit.ray.tfar * ray_d; // p = o + t * d

                // retrieve material by geomid and sample the surface
                Material* material = materials[ray_hit.hit.geomID];
                Float3 wo = -ray_d;                                         // outgoing ray to the camera - this is the current/next ray
                Float3 wi = material->sample_direction(hit_p, hit_n, wo);   // incoming ray from the light - this is the next ray
                Color3 hit_color = material->evaluate_color(hit_p, hit_n, wo, wi);

                RTCRay new_ray = initialize_ray(hit_p, wi);

                return hit_color * trace(new_ray, depth - 1);
            }

            // ray miss
            Float3 direction(ray_hit.ray.dir_x, ray_hit.ray.dir_y, ray_hit.ray.dir_z);
            Float3 unit_direction = direction.normalize();
            float a = 0.5 * (unit_direction.y() + 1.0f);
            return (1.0-a) * Color3(1.0, 1.0, 1.0) + a * Color3(0.5, 0.7, 1.0);
        }

        RTCScene scene;
        std::unordered_map<int, Material*> materials;
};

RTCDevice initialize_device(int verbose)
{
    RTCDevice device = rtcNewDevice(std::format("verbose={}", verbose).c_str());

    if (!device)
        printf("ERROR %d: cannot create device\n", rtcGetDeviceError(NULL));

    rtcSetDeviceErrorFunction(device, error_callback, NULL);
    return device;
}

int main()
{
    // keeping these give a perf improvement
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    int width = 1280;
    int height = 720;
    int samples = 10;
    int max_depth = 10;

    device = initialize_device(1);

    SceneManager scene_mgr(device);
    scene_mgr.add_sphere(Float3(0, -100.5, -1), 100.0f, new Diffuse(Color3(0.8, 0.8, 0.0)));
    scene_mgr.add_sphere(Float3(0.0, 0.0, -1.2), 0.5f, new Diffuse(Color3(0.1, 0.2, 0.5)));
    scene_mgr.add_sphere(Float3(-1.0, 0.0, -1.0), 0.5f, new Metal(Color3(0.8, 0.8, 0.8), 0.3));
    scene_mgr.add_sphere(Float3(1.0, 0.0, -1.0), 0.5f, new Metal(Color3(0.8, 0.6, 0.2), 1.0));
    rtcCommitScene(scene_mgr.scene);

    RTCRayQueryContext context;
    rtcInitRayQueryContext(&context);

    RTCIntersectArguments intersectArgs;
    rtcInitIntersectArguments(&intersectArgs);
    intersectArgs.context = &context;

    FrameBuffer frame(width, height);

    Camera cam(width, height);
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            Color3 pixel_color(0.0f);
            for (int spp = 0; spp < samples; spp++)
            {
                // create new ray and trace it
                RTCRay ray = cam.get_ray(column, row);
                pixel_color += scene_mgr.trace(ray, max_depth);
            }

            // average samples out
            pixel_color /= samples;

            frame.setPixel(column, row, pixel_color.gamma_correct());
        }
    }

    frame.save("output.png");

    // release scene and device resources
    rtcReleaseScene(scene_mgr.scene);
    rtcReleaseDevice(device);

    return 0;
}