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
#include "ray.h"
#include "camera.h"
#include "sampling.h"
#include "material.h"
#include "scene.h"

void error_callback(void* user_ptr, enum RTCError error, const char* str)
{
    printf("ERROR %d: %s\n", error, str);
}

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
    int max_depth = 50;

    RTCDevice device = initialize_device(1);

    InOneWeekendFinal scene_mgr = InOneWeekendFinal(device);
    rtcCommitScene(scene_mgr.scene);

    RTCRayQueryContext context;
    rtcInitRayQueryContext(&context);

    RTCIntersectArguments intersectArgs;
    rtcInitIntersectArguments(&intersectArgs);
    intersectArgs.context = &context;

    FrameBuffer frame(width, height);

    // Camera cam(camera_pos, camera_tgt, camera_vup, aperture, aspect_ratio, fov, focus_dist);
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            Color3 pixel_color(0.0f);
            for (int spp = 0; spp < samples; spp++)
            {
                // create new ray and trace it
                RTCRay ray = scene_mgr.cam.get_ray(column, row);
                pixel_color += scene_mgr.trace(ray, max_depth);
            }

            // average samples out
            pixel_color /= samples;

            frame.setPixel(column, row, pixel_color.gamma_correct());
        }
    }

    frame.save("output_2.png");

    // release scene and device resources
    rtcReleaseScene(scene_mgr.scene);
    rtcReleaseDevice(device);

    return 0;
}