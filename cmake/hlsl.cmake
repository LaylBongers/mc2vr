# Build-time HLSL compilation (docs/build.md "HLSL shader compiler").
#
#   mc2vr_hlsl(<target> <source.hlsl> ENTRY <fn> PROFILE <ps_3_0|vs_3_0> SYMBOL <name>
#              [DEFINES <name>[=<value>]...] [INCLUDES <dir>...])
#
# Compiles <source.hlsl> with Microsoft's d3dcompiler_43 under Wine (tools/hlslc)
# into ${CMAKE_BINARY_DIR}/generated/shaders/<name>.h:
#   mc2vr::shaders::<name>, a uint32_t array of the bytecode.
# <target> gets the header as a source and generated/ as an include directory
# (#include "shaders/<name>.h"). #include dependencies are tracked (depfile).
#
# The compiler lives in the project's own Wine prefix, MC2VR_HLSL_PREFIX
# (default <repo>/.wine; README, setup step 3).

find_program(MC2VR_WINE wine)
set(MC2VR_HLSL_PREFIX "${PROJECT_SOURCE_DIR}/.wine" CACHE PATH
    "Wine prefix holding Microsoft's d3dcompiler_43 (README, setup step 3)")
set(MC2VR_HLSL_DLL "${MC2VR_HLSL_PREFIX}/drive_c/windows/syswow64/d3dcompiler_43.dll")

function(mc2vr_hlsl target source)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "ENTRY;PROFILE;SYMBOL" "DEFINES;INCLUDES")
    if(NOT arg_ENTRY OR NOT arg_PROFILE OR NOT arg_SYMBOL)
        message(FATAL_ERROR "mc2vr_hlsl(${source}): ENTRY, PROFILE and SYMBOL are required")
    endif()
    if(NOT MC2VR_WINE)
        message(FATAL_ERROR "mc2vr_hlsl: wine not found; it runs the HLSL compiler (docs/build.md)")
    endif()
    if(NOT EXISTS "${MC2VR_HLSL_DLL}")
        message(FATAL_ERROR
            "mc2vr_hlsl: ${MC2VR_HLSL_DLL} is missing. Install the HLSL compiler once "
            "(README, setup step 3):\n  WINEPREFIX=\"${MC2VR_HLSL_PREFIX}\" winetricks -q d3dcompiler_43")
    endif()

    get_filename_component(src "${source}" ABSOLUTE)
    set(out_dir "${CMAKE_BINARY_DIR}/generated/shaders")
    set(out "${out_dir}/${arg_SYMBOL}.h")
    set(args --dll "Z:${MC2VR_HLSL_DLL}" --entry ${arg_ENTRY} --profile ${arg_PROFILE}
             --symbol ${arg_SYMBOL} --out "Z:${out}" --depfile "Z:${out}.d")
    foreach(d IN LISTS arg_DEFINES)
        list(APPEND args -D ${d})
    endforeach()
    foreach(i IN LISTS arg_INCLUDES)
        get_filename_component(i "${i}" ABSOLUTE)
        list(APPEND args -I "Z:${i}")
    endforeach()

    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND ${CMAKE_COMMAND} -E env "WINEPREFIX=${MC2VR_HLSL_PREFIX}" WINEDEBUG=-all
                "${MC2VR_WINE}" "$<TARGET_FILE:mc2vr_hlslc>" ${args} "Z:${src}"
        DEPENDS mc2vr_hlslc "${src}" "${MC2VR_HLSL_DLL}"
        DEPFILE "${out}.d"
        COMMENT "HLSL ${arg_SYMBOL} (${arg_PROFILE})"
        VERBATIM)
    target_sources(${target} PRIVATE "${out}")
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
endfunction()
