cmake_minimum_required(VERSION 3.25)

project(mc2vr LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# compile_commands.json for clangd (.clangd points it at build/win32), with
# include paths inline rather than in the MinGW toolchain's response files.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES OFF)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES OFF)

# Single output dir: launch.sh deploys from build/win32/bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Everything is 32-bit (the game is i386); a 64-bit carrier silently fails
# to inject. The toolchain file selects the compilers — refuse a native build
# so the mistake is loud instead of silent.
if(NOT CMAKE_C_COMPILER_TARGET AND NOT CMAKE_TOOLCHAIN_FILE AND NOT WIN32)
    message(FATAL_ERROR
        "Use the MinGW cross toolchain: "
        "cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "mc2vr is 32-bit only: use cmake/i686-w64-mingw32.cmake")
endif()

include(cmake/hlsl.cmake)

add_subdirectory(vendor/safetyhook)
add_subdirectory(tools/hlslc)
add_subdirectory(src/launcher)
add_subdirectory(src/game)
add_subdirectory(src/carrier)
add_subdirectory(tools/test)
