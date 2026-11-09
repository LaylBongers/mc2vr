# Carrier: the DLL the launcher injects. Hosts SafetyHook and (later) the
# hot-swappable gameplay-mod module — keep it thin (docs/carrier.md,
# motion-control track).
add_library(mc2vr_carrier SHARED
    # Infrastructure: attach, conf, log, host IPC, build lock.
    dllmain.cpp
    init.cpp
    ipc.cpp
    log.cpp
    sha256.c
    # hooks/: every attachment to game code (docs/code_structure.md).
    hooks/adaptation.cpp
    hooks/camera_table.cpp
    hooks/cull_frustum.cpp
    hooks/device.cpp
    hooks/frame_tick.cpp
    hooks/install.cpp
    hooks/occluder_boxes.cpp
    hooks/pacing.cpp
    hooks/reimpl.cpp
    hooks/render_outputs.cpp
    hooks/render_size.cpp
    hooks/site.cpp
    hooks/submit_to_gpu.cpp
    hooks/view_rewrite.cpp
    # reimpl/: reimplemented game systems (docs/reimpl.md).
    reimpl/render/render_cmd.cpp
    # vr/: the VR implementation, plain C++.
    vr/adaptation.cpp
    vr/camera_table.cpp
    vr/cull_frustum.cpp
    vr/device_params.cpp
    vr/eye_replay.cpp
    vr/eye_share.cpp
    vr/hud_timing.cpp
    vr/pacing.cpp
    vr/render_outputs.cpp
    vr/render_size.cpp
    vr/view_rewrite.cpp
    vr/vp_camera.cpp
    # debug/: debugging tools, self-contained (conf-gated, default off).
    debug/beginsubmit_probe.cpp
    debug/build_census.cpp
    debug/census_hooks.cpp
    debug/gpu_census.cpp
    debug/probes.cpp
    debug/ps_census.cpp
    debug/render_dump.cpp
    debug/rt_probe.cpp
    debug/shadow/call_trace.cpp
    debug/shadow/render_cmd.cpp
    debug/stream_capture.cpp
    debug/stub_trace.cpp
    debug/vm_dump.cpp
    debug/wait_census.cpp
    debug/watch.cpp
)

# MinGW would otherwise name it libmc2vr_carrier.dll; LoadLibraryW in the
# launcher and the deploy script both expect mc2vr_carrier.dll.
set_target_properties(mc2vr_carrier PROPERTIES PREFIX "")

target_include_directories(mc2vr_carrier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_compile_definitions(mc2vr_carrier PRIVATE UNICODE _UNICODE)

target_link_libraries(mc2vr_carrier PRIVATE safetyhook mc2vr_game)

# Static runtimes: an injected DLL must not depend on libstdc++-6.dll etc.
# being present in the Wine prefix.
target_link_options(mc2vr_carrier PRIVATE -static -static-libgcc -static-libstdc++)

# The device hooks use __builtin_return_address(0) for caller-site evidence;
# keep frame pointers so it is reliable on i386.
target_compile_options(mc2vr_carrier PRIVATE -fno-omit-frame-pointer)
