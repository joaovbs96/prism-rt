# A GPU submission that does not finish in time (PRISM_GPU_TIMEOUT_SECONDS)
# must stop the CLI with an error, write no image and not hang at exit.
# PRISM_GPU_SIMULATE_TIMEOUT=1 reports the first kernel wait as timed out.
# -DPRISMRT=<exe> -DSCENE=<usda> -DOUTPUT=<png>
file(REMOVE "${OUTPUT}")
set(ENV{PRISM_GPU_SIMULATE_TIMEOUT} 1)
set(ENV{PRISM_GPU_TIMEOUT_SECONDS} 7)
execute_process(
    COMMAND "${PRISMRT}" --scene "${SCENE}" --camera /World/Camera --width 32 --height 32 --spp 4 --adaptive-threshold 0
            --device vulkan --output "${OUTPUT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
if(result EQUAL 0)
    message(FATAL_ERROR "PrismRT exited 0 after a GPU timeout:\n${out}\n${err}")
endif()
if(NOT result MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PrismRT did not exit by itself after a GPU timeout (${result}):\n${err}")
endif()
if(NOT err MATCHES "GPU render failed at sample 1 of 4 \\(32x32[^\n]*the GPU did not finish a submission within 7 s \\(PRISM_GPU_TIMEOUT_SECONDS\\)")
    message(FATAL_ERROR "missing the GPU timeout message:\n${err}")
endif()
if(NOT err MATCHES "the device is left as it is after a GPU timeout")
    message(FATAL_ERROR "the device was not left alone after the timeout:\n${err}")
endif()
if(EXISTS "${OUTPUT}")
    message(FATAL_ERROR "PrismRT wrote ${OUTPUT} after a GPU timeout")
endif()
