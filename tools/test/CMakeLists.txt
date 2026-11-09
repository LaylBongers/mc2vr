# Test of the game call shim (src/game/function.cpp, hand-written asm). Win32
# (i386) like the game; run under Wine:
#   cmake --build build/win32 --target mc2vr_call_shim_test && wine build/win32/bin/mc2vr_call_shim_test.exe
# Not built by default and never deployed.
add_executable(mc2vr_call_shim_test EXCLUDE_FROM_ALL
    test_call_shim.cpp
)
target_link_libraries(mc2vr_call_shim_test PRIVATE mc2vr_game)
target_link_options(mc2vr_call_shim_test PRIVATE -static -static-libgcc -static-libstdc++)
