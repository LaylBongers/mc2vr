# hlslc: the build-time HLSL compiler driver (hlslc.cpp; run under Wine by
# cmake/hlsl.cmake's mc2vr_hlsl). A Win32 tool like everything here; never deployed.
add_executable(mc2vr_hlslc hlslc.cpp)
target_compile_features(mc2vr_hlslc PRIVATE cxx_std_17)
target_link_options(mc2vr_hlslc PRIVATE -static -static-libgcc -static-libstdc++)
