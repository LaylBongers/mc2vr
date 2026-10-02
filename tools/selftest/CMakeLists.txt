# Self-test stand-in exe; not built by default (nothing depends on it and
# it must NOT be deployed). Build with:
#   cmake --build build/win32 --target mc2vr_selftest_sleeper
add_executable(mc2vr_selftest_sleeper EXCLUDE_FROM_ALL sleeper.c)
