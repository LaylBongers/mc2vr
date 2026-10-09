# Self-test stand-ins; not built by default (nothing depends on them and they
# must NOT be deployed). Build with:
#   cmake --build build/win32 --target mc2vr_selftest_sleeper mc2vr_selftest_ipc_probe
add_executable(mc2vr_selftest_sleeper EXCLUDE_FROM_ALL sleeper.c)

# IPC round-trip probe: a stand-in carrier that drives mc2vr_host --mock
# over the shared-memory contract (win32, validating the cross-bitness path).
add_executable(mc2vr_selftest_ipc_probe EXCLUDE_FROM_ALL ipc_probe.c)
target_include_directories(mc2vr_selftest_ipc_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
)
