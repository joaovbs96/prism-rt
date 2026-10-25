# CLI-vs-Hydra parity harness (docs/hydra.md, "CLI and Hydra parity").
#
# prism_add_parity_fixture(NAME <name> SCENE <usd>
#     [CAMERA /World/Camera] [WIDTH 64] [HEIGHT 64] [SPP 16]
#     [PRODUCTS <file>...]
#     [TOLERANCE 0.01] [MEAN_TOLERANCE <tolerance>]
#     [ARGS <options of both frontends>...]
#     [CLI_ARGS <PrismRT options>...] [HYDRA_ARGS <prism_hydra_render options>...]
#     [ENV <NAME=value>...]   (environment of both renders)
#     [LAYERS <file>:<layer>...]
#     [EXACT_LAYERS <file>:<layer>...] [RELABEL_LAYERS <file>:<layer>...]
#     [CLI_DEVICE cpu|vulkan|metal])
#
# Renders SCENE on the CPU device with the standalone CLI (PrismRT) and with
# the Hydra delegate (prism_hydra_render), then compares the two with
# prism_compare_images: normalized RMSE within TOLERANCE and relative mean
# luminance within MEAN_TOLERANCE (default: TOLERANCE). Both render a fixed
# number of samples (no adaptive sampling: _prism_fixed_sampling). Without PRODUCTS both
# render one image from CAMERA; with PRODUCTS both write the RenderProducts of
# the stage's render settings and each listed file is compared. LAYERS
# compares those layers too, within the tolerances (light AOVs); EXACT_LAYERS
# compares those layers (geometric AOVs) exactly; RELABEL_LAYERS compares id
# layers exactly up to a renumbering (prim ids: each loader numbers its prims).
# CLI_DEVICE renders the CLI's side on another device (a GPU: name the
# fixture *_vulkan or *_metal so its tests get the gpu label and lock). SCENE
# is relative to the source directory unless absolute.
#
# Tests: prism_parity_<name>_cli, prism_parity_<name>_hydra (the CTest
# fixture prism_parity_<name>), and prism_parity_<name>_match (one image) or
# prism_parity_<name>_<file stem>_match, prism_parity_<name>_<file
# stem>_<layer>_{layer,exact,relabel} for the layers; outputs go to
# ${CMAKE_BINARY_DIR}/parity/<name>/{cli,hydra}, which is returned in
# prism_parity_<name>_dir.
function(prism_add_parity_fixture)
    cmake_parse_arguments(P "" "NAME;SCENE;CAMERA;WIDTH;HEIGHT;SPP;TOLERANCE;MEAN_TOLERANCE;CLI_DEVICE"
        "ARGS;CLI_ARGS;HYDRA_ARGS;ENV;PRODUCTS;LAYERS;EXACT_LAYERS;RELABEL_LAYERS" ${ARGN})
    if(NOT P_NAME OR NOT P_SCENE)
        message(FATAL_ERROR "prism_add_parity_fixture needs NAME and SCENE")
    endif()
    foreach(_default IN ITEMS "CAMERA;/World/Camera" "WIDTH;64" "HEIGHT;64" "SPP;16" "TOLERANCE;0.01" "CLI_DEVICE;cpu")
        list(GET _default 0 _key)
        list(GET _default 1 _value)
        if(NOT DEFINED P_${_key})
            set(P_${_key} "${_value}")
        endif()
    endforeach()
    if(NOT DEFINED P_MEAN_TOLERANCE)
        set(P_MEAN_TOLERANCE "${P_TOLERANCE}")
    endif()
    set(_scene "${P_SCENE}")
    if(NOT IS_ABSOLUTE "${_scene}")
        set(_scene "${CMAKE_CURRENT_SOURCE_DIR}/${_scene}")
    endif()
    set(_dir "${CMAKE_BINARY_DIR}/parity/${P_NAME}")
    set(_prefix prism_parity_${P_NAME})
    if(P_PRODUCTS)
        add_test(NAME ${_prefix}_cli
            COMMAND PrismRT --scene "${_scene}" --device ${P_CLI_DEVICE} --output-dir "${_dir}/cli" ${_prism_fixed_sampling} ${P_ARGS} ${P_CLI_ARGS})
        add_test(NAME ${_prefix}_hydra
            COMMAND prism_hydra_render "${_scene}" --render-settings default "${_dir}/hydra" ${_prism_fixed_sampling} ${P_ARGS} ${P_HYDRA_ARGS})
        set(_outputs ${P_PRODUCTS})
    else()
        add_test(NAME ${_prefix}_cli
            COMMAND PrismRT --scene "${_scene}" --camera ${P_CAMERA} --width ${P_WIDTH} --height ${P_HEIGHT}
                --spp ${P_SPP} --device ${P_CLI_DEVICE} ${_prism_fixed_sampling} ${P_ARGS} ${P_CLI_ARGS} --output "${_dir}/cli/image.exr")
        add_test(NAME ${_prefix}_hydra
            COMMAND prism_hydra_render "${_scene}" ${P_CAMERA} "${_dir}/hydra/image.exr"
                ${P_WIDTH} ${P_HEIGHT} ${P_SPP} ${_prism_fixed_sampling} ${P_ARGS} ${P_HYDRA_ARGS})
        set(_outputs image.exr)
    endif()
    # The CPU device in both frontends unless CLI_DEVICE says otherwise.
    set_tests_properties(${_prefix}_hydra PROPERTIES
        ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu;${P_ENV}"
        FIXTURES_SETUP ${_prefix})
    set_tests_properties(${_prefix}_cli PROPERTIES
        ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=${P_CLI_DEVICE};${P_ENV}"
        FIXTURES_SETUP ${_prefix})
    list(LENGTH _outputs _count)
    foreach(_output IN LISTS _outputs)
        if(_count EQUAL 1)
            set(_test ${_prefix}_match)
        else()
            get_filename_component(_stem "${_output}" NAME_WE)
            get_filename_component(_extension "${_output}" LAST_EXT)
            string(REPLACE "." "" _extension "${_extension}")
            set(_test ${_prefix}_${_stem}_${_extension}_match)
        endif()
        add_test(NAME ${_test}
            COMMAND prism_compare_images "${_dir}/cli/${_output}" "${_dir}/hydra/${_output}"
                --tolerance ${P_TOLERANCE} --mean-tolerance ${P_MEAN_TOLERANCE})
        set_tests_properties(${_test} PROPERTIES FIXTURES_REQUIRED ${_prefix})
    endforeach()
    foreach(_compared IN LISTS P_LAYERS)
        string(REPLACE ":" ";" _parts "${_compared}")
        list(GET _parts 0 _output)
        list(GET _parts 1 _layer)
        get_filename_component(_stem "${_output}" NAME_WE)
        set(_test ${_prefix}_${_stem}_${_layer}_layer)
        add_test(NAME ${_test}
            COMMAND prism_compare_images "${_dir}/cli/${_output}" "${_dir}/hydra/${_output}"
                --layer ${_layer} --tolerance ${P_TOLERANCE} --mean-tolerance ${P_MEAN_TOLERANCE})
        set_tests_properties(${_test} PROPERTIES FIXTURES_REQUIRED ${_prefix})
    endforeach()
    foreach(_exact IN LISTS P_EXACT_LAYERS)
        string(REPLACE ":" ";" _parts "${_exact}")
        list(GET _parts 0 _output)
        list(GET _parts 1 _layer)
        get_filename_component(_stem "${_output}" NAME_WE)
        set(_test ${_prefix}_${_stem}_${_layer}_exact)
        add_test(NAME ${_test}
            COMMAND prism_compare_images "${_dir}/cli/${_output}" "${_dir}/hydra/${_output}"
                --exact --layer ${_layer})
        set_tests_properties(${_test} PROPERTIES FIXTURES_REQUIRED ${_prefix})
    endforeach()
    foreach(_relabel IN LISTS P_RELABEL_LAYERS)
        string(REPLACE ":" ";" _parts "${_relabel}")
        list(GET _parts 0 _output)
        list(GET _parts 1 _layer)
        get_filename_component(_stem "${_output}" NAME_WE)
        set(_test ${_prefix}_${_stem}_${_layer}_relabel)
        add_test(NAME ${_test}
            COMMAND prism_compare_images "${_dir}/cli/${_output}" "${_dir}/hydra/${_output}"
                --layer ${_layer} --relabel)
        set_tests_properties(${_test} PROPERTIES FIXTURES_REQUIRED ${_prefix})
    endforeach()
    set(prism_parity_${P_NAME}_dir "${_dir}" PARENT_SCOPE)
endfunction()
