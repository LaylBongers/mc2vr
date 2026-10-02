cmake_minimum_required(VERSION 3.25)

project(mc2vr LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Single output dir: launch.sh deploys from build/win32/bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Everything is 32-bit (the game is i386); a 64-bit carrier silently fails
# to inject. The toolchain file selects the i686 compilers — refuse a
# native build so the mistake is loud instead of silent.
if(NOT CMAKE_C_COMPILER_TARGET AND NOT CMAKE_TOOLCHAIN_FILE AND NOT WIN32)
    message(FATAL_ERROR
        "Use the MinGW cross toolchain: "
        "cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake")
endif()

add_subdirectory(vendor/safetyhook)
add_subdirectory(src/launcher)
add_subdirectory(src/carrier)
add_subdirectory(tools/selftest)
