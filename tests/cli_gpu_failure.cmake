# A failed GPU render must stop the CLI with an error and write no image
# (it used to save the zeroed, all-black buffer and exit 0).
# -DPRISMRT=<exe> -DSCENE=<usda> -DOUTPUT=<png>
file(REMOVE "${OUTPUT}")
set(ENV{PRISM_GPU_FAIL_DISPATCH_AT} 0)
execute_process(
    COMMAND "${PRISMRT}" --scene "${SCENE}" --camera /World/Camera --width 32 --height 32 --spp 4 --adaptive-threshold 0
            --device vulkan --output "${OUTPUT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 0)
    message(FATAL_ERROR "PrismRT exited 0 after a failed GPU render:\n${out}\n${err}")
endif()
if(NOT err MATCHES "GPU render failed at sample 1 of 4 \\(32x32")
    message(FATAL_ERROR "missing the descriptive GPU failure message:\n${err}")
endif()
if(EXISTS "${OUTPUT}")
    message(FATAL_ERROR "PrismRT wrote ${OUTPUT} after a failed GPU render")
endif()
