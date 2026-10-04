# Carrier: the DLL the launcher injects. Hosts SafetyHook and (later) the
# hot-swappable gameplay-mod module — keep it thin (docs/launcher_plan.md,
# motion-control track).
add_library(mc2vr_carrier SHARED
    dllmain.cpp
    init.cpp
    hooks.cpp
    device.cpp
    view_rewrite.cpp
    eye_replay.cpp
    log.cpp
    # Diagnostic/instrumentation modules (mostly conf-gated, default off).
    debug/probes.cpp
    debug/render_dump.cpp
    debug/stub_trace.cpp
    debug/stream_capture.cpp
    debug/vm_dump.cpp
    sha256.c
)

# MinGW would otherwise name it libmc2vr_carrier.dll; LoadLibraryW in the
# launcher and the deploy script both expect mc2vr_carrier.dll.
set_target_properties(mc2vr_carrier PROPERTIES PREFIX "")

target_include_directories(mc2vr_carrier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_compile_definitions(mc2vr_carrier PRIVATE UNICODE _UNICODE)

target_link_libraries(mc2vr_carrier PRIVATE safetyhook)

# Static runtimes: an injected DLL must not depend on libstdc++-6.dll etc.
# being present in the Wine prefix.
target_link_options(mc2vr_carrier PRIVATE -static -static-libgcc -static-libstdc++)

# The device hooks use __builtin_return_address(0) for caller-site evidence;
# keep frame pointers so it is reliable on i386.
target_compile_options(mc2vr_carrier PRIVATE -fno-omit-frame-pointer)
