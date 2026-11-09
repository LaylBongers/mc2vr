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
    hooks/hud.cpp
    hooks/install.cpp
    hooks/occluder_boxes.cpp
    hooks/entry.cpp
    hooks/reimpl.cpp
    hooks/render_outputs.cpp
    hooks/render_size.cpp
    hooks/site.cpp
    # reimpl/: reimplemented game systems (docs/reimpl.md).
    reimpl/render/begin_submit.cpp
    reimpl/render/end_sub_view.cpp
    reimpl/render/render_cmd.cpp
    reimpl/render/submit_to_gpu.cpp
    # vr/: the VR implementation, plain C++.
    vr/adaptation.cpp
    vr/camera_table.cpp
    vr/cull_frustum.cpp
    vr/device_params.cpp
    vr/eye_replay.cpp
    vr/eye_share.cpp
    vr/hud.cpp
    vr/hud_timing.cpp
    vr/msaa.cpp
    vr/pacing.cpp
    vr/render_outputs.cpp
    vr/render_size.cpp
    vr/shader_constants.cpp
    vr/view_rewrite.cpp
    vr/world_markers.cpp
    vr/vp_camera.cpp
    # debug/: debugging tools, self-contained (conf-gated, default off).
    debug/build_census.cpp
    debug/camera_census.cpp
    debug/census_hooks.cpp
    debug/gpu_census.cpp
    debug/gui_census.cpp
    debug/ps_census.cpp
    debug/render_dump.cpp
    debug/stream_capture.cpp
    debug/vm_dump.cpp
    debug/wait_census.cpp
    debug/watch.cpp
)

# Reimplemented engine code does float math like the original: scalar SSE
# (mulss/addss), not the i686 default x87, whose extended precision would
# change the rounding of e.g. a*b+c.
file(GLOB MC2VR_REIMPL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/reimpl/*/*.cpp)
set_source_files_properties(${MC2VR_REIMPL_SOURCES} PROPERTIES COMPILE_OPTIONS "-msse2;-mfpmath=sse")

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
