# Windows: opt an executable in to paths longer than 260 characters through
# its application manifest (resources/windows/prism.manifest). Windows honours
# it when LongPathsEnabled is set; without it, a process is limited to
# MAX_PATH whatever the setting says. See docs/deployment.md, "Path length".
function(prism_enable_long_paths target)
    if(MSVC)
        target_sources(${target} PRIVATE "${CMAKE_SOURCE_DIR}/resources/windows/prism.manifest")
    endif()
endfunction()
