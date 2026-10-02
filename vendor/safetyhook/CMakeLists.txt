# SafetyHook v0.7.0 — amalgamated build with Zydis bundled (BSL-1.0, see LICENSE).
# Vendored so builds are offline/reproducible; release:
#   https://github.com/cursey/safetyhook/releases/tag/v0.7.0
# (safetyhook-amalgamated-zydis.zip, sha256
#  505d4c07ec1c5b94a17f3906ca86afbe1264e738d8becaa244866694c6200c2c)
add_library(safetyhook STATIC
    safetyhook.cpp
    Zydis.c
)

set_source_files_properties(Zydis.c PROPERTIES C_STANDARD 11)

# compat/ holds the Windows.h case shim for MinGW on case-sensitive filesystems.
target_include_directories(safetyhook
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
)

target_compile_features(safetyhook PUBLIC cxx_std_23)
