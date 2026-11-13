# Tests of the game call shim (src/game/function.cpp) and the entry for
# reimplemented originals (src/carrier/hooks/entry.cpp), both hand-written asm. Win32
# (i386) like the game; run under Wine:
#   cmake --build build/win32 --target mc2vr_call_shim_test && wine build/win32/bin/mc2vr_call_shim_test.exe
#   cmake --build build/win32 --target mc2vr_game_entry_test && wine build/win32/bin/mc2vr_game_entry_test.exe
#   cmake --build build/win32 --target mc2vr_crash_log_test && wine build/win32/bin/mc2vr_crash_log_test.exe
#   cmake --build build/win32 --target mc2vr_hlsl_test && wine build/win32/bin/mc2vr_hlsl_test.exe
# Not built by default and never deployed.
add_executable(mc2vr_call_shim_test EXCLUDE_FROM_ALL
    test_call_shim.cpp
)
target_link_libraries(mc2vr_call_shim_test PRIVATE mc2vr_game)
target_link_options(mc2vr_call_shim_test PRIVATE -static -static-libgcc -static-libstdc++)

add_executable(mc2vr_game_entry_test EXCLUDE_FROM_ALL
    test_game_entry.cpp
    ${PROJECT_SOURCE_DIR}/src/carrier/hooks/entry.cpp
)
target_include_directories(mc2vr_game_entry_test PRIVATE ${PROJECT_SOURCE_DIR}/src/carrier)
target_link_libraries(mc2vr_game_entry_test PRIVATE mc2vr_game)
target_link_options(mc2vr_game_entry_test PRIVATE -static -static-libgcc -static-libstdc++)
# SSE like the carrier: the test clobbers XMM5..XMM7 to check the entry restores them.
target_compile_options(mc2vr_game_entry_test PRIVATE -msse2 -mfpmath=sse)

add_executable(mc2vr_crash_log_test EXCLUDE_FROM_ALL
    test_crash_log.cpp
    ${PROJECT_SOURCE_DIR}/src/carrier/crash_log.cpp
    ${PROJECT_SOURCE_DIR}/src/carrier/log.cpp
)
target_include_directories(mc2vr_crash_log_test PRIVATE ${PROJECT_SOURCE_DIR}/src/carrier)
target_link_options(mc2vr_crash_log_test PRIVATE -static -static-libgcc -static-libstdc++)

# The build-time HLSL pipeline (cmake/hlsl.cmake): a shader compiled with the
# project's d3dcompiler_43 keeps the game's bool-register static branches.
#   cmake --build build/win32 --target mc2vr_hlsl_test && wine build/win32/bin/mc2vr_hlsl_test.exe
add_executable(mc2vr_hlsl_test EXCLUDE_FROM_ALL test_hlsl.cpp)
mc2vr_hlsl(mc2vr_hlsl_test hlsl/static_bool.hlsl ENTRY main PROFILE ps_3_0 SYMBOL test_static_bool_ps)
target_link_options(mc2vr_hlsl_test PRIVATE -static -static-libgcc -static-libstdc++)
