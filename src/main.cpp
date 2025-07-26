#include <iostream>
#include <format>
#include <thread>

// external libraries
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <tbb/mutex.h>
#include <tbb/tick_count.h>
#include <tbb/tbb.h>
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
#include "load_gltf.h"

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

void print_progress(float progress) {
    int barWidth = 70;

    std::cout << "[";
    int pos = barWidth * progress;
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << float(progress * 100.0) << "%                        \r";
    std::cout.flush();

    if (progress == 1.0)
        std::cout << std::endl;
}

int main()
{
    // keeping these give a perf improvement
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    int width = 1280;
    int height = 720;
    int samples = 8;
    int max_depth = 10;

    RTCDevice device = initialize_device(1);

    unsigned int main_seed = 0;
    SceneManager* scene_mgr = new Duck(device, main_seed);
    rtcCommitScene(scene_mgr->scene);

    RTCRayQueryContext context;
    rtcInitRayQueryContext(&context);

    RTCIntersectArguments intersectArgs;
    rtcInitIntersectArguments(&intersectArgs);
    intersectArgs.context = &context;

    FrameBuffer frame(width, height);

    // Debugging, if we want to print something during rendering.
    // tbb::global_control gc(tbb::global_control::max_allowed_parallelism, 1);

    auto start_time = tbb::tick_count::now();
    tbb::parallel_for(0, height, [&](int row)
    {
        tbb::parallel_for(0, width, [&](int column)
        {
            unsigned int seed = tea<64>(row * width, column);

            // render for N samples
            Color3 pixel_color(0.0f);
            for (int spp = 0; spp < samples; spp++)
            {
                // create new ray and trace it
                RTCRay ray = scene_mgr->cam.get_ray(column, row, seed);
                pixel_color += scene_mgr->trace(ray, max_depth, seed);
            }

            // average samples out and put it in the frame buffer
            pixel_color /= samples;
            frame.setPixel(column, row, pixel_color.gamma_correct());
        });

    });
    auto end_time = tbb::tick_count::now();
    std::cout << "Rendering took " << (end_time - start_time).seconds() << " seconds." << std::endl;

    frame.save("output.png");

    // TODO: collect and release all geometry nodes from scene

    // release scene and device resources
    rtcReleaseScene(scene_mgr->scene);
    rtcReleaseDevice(device);

    return 0;
}