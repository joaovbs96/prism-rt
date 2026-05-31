#include <iostream>
#include <format>
#include <thread>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iomanip>

// external libraries
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <tbb/mutex.h>
#include <tbb/tick_count.h>
#include <tbb/tbb.h>
#include <embree4/rtcore.h>
#include <xmmintrin.h>
#include <mi/mdl_sdk.h>

#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>

// needed for embree initialization
#if !defined(_MM_SET_DENORMALS_ZERO_MODE)
#define _MM_DENORMALS_ZERO_ON   (0x0040)
#define _MM_DENORMALS_ZERO_OFF  (0x0000)
#define _MM_DENORMALS_ZERO_MASK (0x0040)
#define _MM_SET_DENORMALS_ZERO_MODE(x) (_mm_setcsr((_mm_getcsr() & ~_MM_DENORMALS_ZERO_MASK) | (x)))
#endif

// #include <tbb/global_control.h>
// tbb::global_control gc(tbb::global_control::max_allowed_parallelism, 1);

// project libraries
#include "mdl/context.h"
#include "scene.h"

std::atomic<int> completed_rows{0};
std::atomic<bool> rendering_complete{false};

void print_progress(float progress, double eta_seconds) {
    int barWidth = 50;  // Reduced to make room for ETA

    std::cout << "[";
    int pos = barWidth * progress;
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    
    // Format ETA as HH:MM:SS or MM:SS
    int eta_h = static_cast<int>(eta_seconds) / 3600;
    int eta_m = (static_cast<int>(eta_seconds) % 3600) / 60;
    int eta_s = static_cast<int>(eta_seconds) % 60;
    
    std::cout << "] " << std::fixed << std::setprecision(1) << (progress * 100.0f) << "%";
    
    if (progress >= 1.0f) {
        std::cout << " | Done!";
    } else if (progress > 0.001f) {
        std::cout << std::format(" | ETA: {:02}:{:02}:{:02}", eta_h, eta_m, eta_s);
    }
    
    std::cout << "                    \r";  // Padding to clear previous longer output
    std::cout.flush();

    if (progress >= 1.0f)
        std::cout << std::endl;
}

// Progress monitoring thread
void progress_monitor(int total_rows) {
    auto start_time = std::chrono::steady_clock::now();
    
    while (!rendering_complete.load(std::memory_order_relaxed)) {
        int completed = completed_rows.load(std::memory_order_relaxed);
        float progress = static_cast<float>(completed) / total_rows;
        
        // Calculate ETA
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time).count();
        
        double eta_seconds = 0.0;
        if (progress > 0.001f) {  // Avoid division by very small numbers
            double total_estimated = elapsed / progress;
            eta_seconds = total_estimated - elapsed;
        }
        
        print_progress(progress, eta_seconds);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    print_progress(1.0f, 0.0);  // Final update
}

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
    // ============================================================
    // Phase 2: MDL SDK Initialization
    // ============================================================
    std::cout << "=== Phase 2: MDL SDK Initialization ===\n\n";
    
    // The Context constructor now:
    //   1. Loads libmdl_sdk.dll
    //   2. Loads plugins (nv_openimageio by default) BEFORE neuray->start()
    //   3. Starts the SDK
    //   4. Acquires all core interfaces
    mdl::Context mdl_context;
    
    if (!mdl_context.is_valid()) {
        std::cerr << "[ERROR] Failed to initialize MDL SDK\n";
        std::cerr << "Error: " << mdl_context.get_error() << "\n";
        return 1;
    }
    
    std::cout << "[OK] MDL SDK initialized\n";
    std::cout << "Version: " << mdl_context.get_neuray()->get_version() << "\n\n";
    
    // Verify core interfaces
    std::cout << "=== Verifying Core Interfaces ===\n";
    
    if (mdl_context.get_database())
        std::cout << "[OK] IDatabase interface acquired\n";
    if (mdl_context.get_config())
        std::cout << "[OK] IMdl_configuration interface acquired\n";
    if (mdl_context.get_impexp())
        std::cout << "[OK] IMdl_impexp_api interface acquired\n";
    if (mdl_context.get_factory())
        std::cout << "[OK] IMdl_factory interface acquired\n";
    
    // Test database/transaction system
    std::cout << "\n=== Testing Database System ===\n";
    
    mi::base::Handle<mi::neuraylib::IScope> scope(
        mdl_context.get_database()->get_global_scope());
    
    if (!scope) {
        std::cerr << "[ERROR] Failed to get global scope\n";
        return 1;
    }
    std::cout << "[OK] Global scope acquired\n";
    
    mi::base::Handle<mi::neuraylib::ITransaction> transaction(
        scope->create_transaction());
    
    if (!transaction) {
        std::cerr << "[ERROR] Failed to create transaction\n";
        return 1;
    }
    std::cout << "[OK] Transaction created\n";
    
    transaction->commit();
    std::cout << "[OK] Transaction committed\n";
    
    // Test backend API
    std::cout << "\n=== Testing Compilation Backend ===\n";
    
    mi::base::Handle<mi::neuraylib::IMdl_backend_api> backend_api(
        mdl_context.get_neuray()->get_api_component<mi::neuraylib::IMdl_backend_api>());
    
    if (!backend_api) {
        std::cerr << "[ERROR] Failed to get backend API\n";
        return 1;
    }
    std::cout << "[OK] Backend API acquired\n";
    
    mi::base::Handle<mi::neuraylib::IMdl_backend> cpu_backend(
        backend_api->get_backend(mi::neuraylib::IMdl_backend_api::MB_NATIVE));
    
    if (!cpu_backend) {
        std::cerr << "[ERROR] Failed to get native backend\n";
        return 1;
    }
    std::cout << "[OK] Native backend (CPU) available\n";
    
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "PHASE 2 COMPLETE - SDK Ready\n";
    std::cout << std::string(60, '=') << "\n";
    
    // ============================================================
    // Phase 9: Render with MaterialX Material
    // ============================================================
    std::cout << "\n=== Phase 9: Rendering with MaterialX Material ===\n\n";

    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    int samples = 256;
    int max_depth = 10;

    RTCDevice device = initialize_device(0);
    unsigned int main_seed = 0;

    // Create the scene — MDLMaterial compiles the .mtlx internally
    SceneManager* scene_mgr = new MaterialXTest(device, main_seed, mdl_context);

    scene_mgr->commitScene();
    std::cout << "Finished committing scene." << std::endl;

    FrameBuffer frame(scene_mgr->image_width, scene_mgr->image_height);
    std::cout << "Rendering " << scene_mgr->image_width << "x" << scene_mgr->image_height
              << " @ " << samples << " spp, max depth " << max_depth << std::endl;

    completed_rows = 0;
    rendering_complete = false;

    std::thread progress_thread(progress_monitor, scene_mgr->image_height);

    auto start_time = tbb::tick_count::now();
    tbb::parallel_for(0, scene_mgr->image_height, [&](int row)
    {
        tbb::parallel_for(0, scene_mgr->image_width, [&](int column)
        {
            unsigned int seed = tea<64>(row * scene_mgr->image_width, column);

            Color3 pixel_color(0.0f);
            for (int spp = 0; spp < samples; spp++)
            {
                RTCRay ray = scene_mgr->cam.get_ray(column, row, seed);
                pixel_color += scene_mgr->trace(ray, max_depth, seed);
            }

            pixel_color /= samples;
            frame.setPixel(column, row, pixel_color);
        });

        completed_rows.fetch_add(1, std::memory_order_relaxed);
    });

    rendering_complete = true;
    progress_thread.join();

    auto end_time = tbb::tick_count::now();
    std::cout << "Rendering took " << (end_time - start_time).seconds() << " seconds." << std::endl;

    frame.applyGammaCorrection();

    std::filesystem::path output_path = std::filesystem::current_path() / "output.png";
    std::cout << "Saving Image to File: " << output_path << std::endl;
    frame.save(output_path);

    // Cleanup
    rtcReleaseScene(scene_mgr->scene);
    rtcReleaseDevice(device);
    delete scene_mgr;

    return 0;
}