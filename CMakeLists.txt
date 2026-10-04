cmake_minimum_required(VERSION 3.25)

project(mc2vr LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Single output dir: launch.sh deploys from build/win32/bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The game side is 32-bit (the game is i386); a 64-bit carrier silently fails
# to inject. The toolchain file selects the compilers — refuse a native build
# so the mistake is loud instead of silent.
if(NOT CMAKE_C_COMPILER_TARGET AND NOT CMAKE_TOOLCHAIN_FILE AND NOT WIN32)
    message(FATAL_ERROR
        "Use the MinGW cross toolchain: "
        "cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake "
        "(game side) or cmake -B build/win64 "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake (OpenXR host)")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Win64 build: only the OpenXR host (loader built from the vendored SDK).
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    # The loader's -Werror=undef trips on MinGW's WINAPI_PARTITION_SYSTEM.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DWINAPI_PARTITION_SYSTEM=0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWINAPI_PARTITION_SYSTEM=0")
    add_subdirectory(vendor/openxr-sdk EXCLUDE_FROM_ALL)
    add_subdirectory(src/host)
    add_subdirectory(tools/probe)
else()
    add_subdirectory(vendor/safetyhook)
    add_subdirectory(src/launcher)
    add_subdirectory(src/carrier)
    add_subdirectory(tools/selftest)
    add_subdirectory(tools/probe)
endif()
