#include <iostream>
#include <format>

// external libraries
#include <embree4/rtcore.h>

// project libraries
#include "frame_buffer.h"
#include "float3.h"
#include "camera.h"

RTCDevice device;

void error_callback(void* user_ptr, enum RTCError error, const char* str)
{
    printf("ERROR %d: %s\n", error, str);
}

// class Scene {
//     public:
//         Scene() {}



//         RTCScene scene;
// };

RTCDevice initialize_device(int verbose)
{
    RTCDevice device = rtcNewDevice(std::format("verbose={}", verbose).c_str());

    if (!device)
        printf("ERROR %d: cannot create device\n", rtcGetDeviceError(NULL));

    rtcSetDeviceErrorFunction(device, error_callback, NULL);
    return device;
}

RTCScene initialize_scene(RTCDevice device) {
    RTCScene scene = rtcNewScene(device);

    // create and commit new geometry
    RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);

    // creates buffers for vertex and index of triangle
    // NOTE: when defining new geometry for a given scene, this is basically
    // what will be defining each geometry itself
    float* vertices = (float*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), 3);
    unsigned* indices = (unsigned*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned), 1);

    if (vertices && indices)
    {
        vertices[0] = 0.f; vertices[1] = 0.f; vertices[2] = 0.f;
        vertices[3] = 1.f; vertices[4] = 0.f; vertices[5] = 0.f;
        vertices[6] = 0.f; vertices[7] = 1.f; vertices[8] = 0.f;

        indices[0] = 0; indices[1] = 1; indices[2] = 2;
    }

    // we need to commit the geometry after finishing setting it up
    rtcCommitGeometry(geom);

    // By attaching, this scene takes ownership of the geometry
    rtcAttachGeometry(scene, geom);

    // After attaching, we no longer need to hold the
    // geom handle and can release it
    rtcReleaseGeometry(geom);

    // After we are done setting up the scene, we commit as well
    rtcCommitScene(scene);

    return scene;
}


RTCRayHit cast_ray(RTCScene scene, RTCRay ray) {
    RTCRayHit rayhit;
    rayhit.ray = ray;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

    // intersect a single ray with the scene
    rtcIntersect1(scene, &rayhit);

    return rayhit;
}

Color3 ray_color(RTCRayHit ray_hit) {
    if (ray_hit.hit.geomID != RTC_INVALID_GEOMETRY_ID){
        return 0.5 * (Float3(ray_hit.hit.Ng_x, ray_hit.hit.Ng_y, ray_hit.hit.Ng_z) + Color3(1));
    }
    Float3 direction(ray_hit.ray.dir_x, ray_hit.ray.dir_y, ray_hit.ray.dir_z);
    Float3 unit_direction = direction.normalize();
    float a = 0.5 * (unit_direction.y() + 1.0f);
    return (1.0-a) * Color3(1.0, 1.0, 1.0) + a * Color3(0.5, 0.7, 1.0);
}

unsigned int add_sphere(RTCScene scene, const Float3& pos, float r) {
    RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
    float* vb = (float*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4*sizeof(float), 1);
    vb[0] = pos.x(); vb[1] = pos.y(); vb[2] = pos.z(); vb[3] = r;

    // TODO: somewhere, we will need to keep a structure
    // with Materials, and a mapping of geometry IDs to Materials
    // We may want to consider a thin wrapper around scene that handles such
    // things automatically

    // commit and release geom handle
    rtcCommitGeometry(geom);
    unsigned int geomID = rtcAttachGeometry(scene, geom);
    rtcReleaseGeometry(geom);
    return geomID;
}

int main()
{
    int width = 1280;
    int height = 720;
    int samples = 100;

    device = initialize_device(1);

    RTCScene scene = initialize_scene(device);
    add_sphere(scene, Float3(0, 0, -1), 0.5f);
    add_sphere(scene, Float3(0, -100.5, -1), 100.0f);
    rtcCommitScene(scene);

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
                RTCRay ray = cam.get_ray(column, row);
                RTCRayHit ray_hit = cast_ray(scene, ray);
                pixel_color += ray_color(ray_hit);
            }

            // average samples out
            pixel_color /= samples;

            frame.setPixel(column, row, pixel_color);
        }
    }

    frame.save("output.jpg");

    // release scene and device resources
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);

    return 0;
}