# Carrier: the DLL the launcher injects. Hosts SafetyHook, the OpenXR session
# and (later) the hot-swappable gameplay-mod module — keep it thin (docs/carrier.md,
# motion-control track).
add_library(mc2vr_carrier SHARED
    # Infrastructure: attach, conf, log, build lock.
    crash_log.cpp
    dllmain.cpp
    init.cpp
    log.cpp
    sha256.c
    # hooks/: every attachment to game code (docs/code_structure.md).
    hooks/adaptation.cpp
    hooks/camera_table.cpp
    hooks/cull_frustum.cpp
    hooks/device.cpp
    hooks/first_person.cpp
    hooks/frame_tick.cpp
    hooks/hud.cpp
    hooks/install.cpp
    hooks/occlusion_queries.cpp
    hooks/shader_reimpl.cpp
    hooks/entry.cpp
    hooks/reimpl.cpp
    hooks/render_outputs.cpp
    hooks/render_size.cpp
    hooks/site.cpp
    # reimpl/: reimplemented game systems (docs/reimpl.md).
    reimpl/render/begin_submit.cpp
    reimpl/render/bloom_emit_passes.cpp
    reimpl/render/build_camera_constants.cpp
    reimpl/render/clouds_emit.cpp
    reimpl/render/end_sub_view.cpp
    reimpl/render/fit_shadow_cascades.cpp
    reimpl/render/render_cmd.cpp
    reimpl/render/setup_viewports.cpp
    reimpl/render/submit_to_gpu.cpp
    # vr/: the VR implementation, plain C++.
    vr/adaptation.cpp
    vr/camera_table.cpp
    vr/cull_frustum.cpp
    vr/first_person.cpp
    vr/device_params.cpp
    vr/eye_replay.cpp
    vr/eye_share.cpp
    vr/hud.cpp
    vr/hud_timing.cpp
    vr/msaa.cpp
    vr/occlusion_queries.cpp
    vr/pacing.cpp
    vr/render_outputs.cpp
    vr/render_size.cpp
    vr/shader_constants.cpp
    vr/view_rewrite.cpp
    vr/world_markers.cpp
    vr/vp_camera.cpp
    vr/xr_present.cpp
    vr/xr_runtime.cpp
    vr/xr_session.cpp
    # debug/: debugging tools, self-contained (conf-gated, default off).
    debug/build_census.cpp
    debug/camera_census.cpp
    debug/census_hooks.cpp
    debug/cull_probe.cpp
    debug/gameplay_census.cpp
    debug/gpu_census.cpp
    debug/gui_census.cpp
    debug/occlusion_off.cpp
    debug/ps_census.cpp
    debug/render_dump.cpp
    debug/vm_dump.cpp
    debug/wait_census.cpp
    debug/watch.cpp
)

# Reimplemented shaders (reimpl/shaders/, docs/shaders.md): HLSL compiled at
# build time into generated/shaders/<name>.h (cmake/hlsl.cmake).
mc2vr_hlsl(mc2vr_carrier reimpl/shaders/PgBillboardTreeFP.hlsl ENTRY main PROFILE ps_3_0 SYMBOL PgBillboardTreeFP)
mc2vr_hlsl(mc2vr_carrier reimpl/shaders/PgUnderwaterFP.hlsl ENTRY main PROFILE ps_3_0 SYMBOL PgUnderwaterFP)
mc2vr_hlsl(mc2vr_carrier reimpl/shaders/PgWaterFPDWE.hlsl ENTRY main PROFILE ps_3_0 SYMBOL PgWaterFPDWE)
mc2vr_hlsl(mc2vr_carrier reimpl/shaders/PgWaterFP.hlsl ENTRY main PROFILE ps_3_0 SYMBOL PgWaterFP)
mc2vr_hlsl(mc2vr_carrier reimpl/shaders/PgWaterFP.hlsl ENTRY main PROFILE ps_3_0 SYMBOL PgWaterFPbl DEFINES MC2VR_WATER_BL=1)
mc2vr_hlsl(mc2vr_carrier reimpl/shaders/PgWaterFP.hlsl ENTRY main PROFILE ps_3_0 SYMBOL PgWaterFP_LI DEFINES MC2VR_WATER_LI=1)

# Each family in its 7 light variants (reimpl/shaders/lights.hlsli), named as
# the originals' .sho files: <suffix>:<point lights 0|1>:<spot lights 0|1>:<_li 0|1>.
set(MC2VR_LIGHT_VARIANTS
    ":0:0:0" "_pl:1:0:0" "_pl_li:1:0:1" "_pl_sl:1:1:0" "_pl_sl_li:1:1:1" "_sl:0:1:0" "_sl_li:0:1:1")
foreach(family IN ITEMS PgFastFP PgScrubLightFP PgDecal2FP)
    foreach(variant IN LISTS MC2VR_LIGHT_VARIANTS)
        string(REPLACE ":" ";" v "${variant}")
        list(GET v 0 suffix)
        list(GET v 1 point)
        list(GET v 2 spot)
        list(GET v 3 li)
        mc2vr_hlsl(mc2vr_carrier reimpl/shaders/${family}.hlsl ENTRY main PROFILE ps_3_0
                   SYMBOL ${family}${suffix}
                   DEFINES MC2VR_POINT_LIGHTS=${point} MC2VR_SPOT_LIGHTS=${spot} MC2VR_LIGHTS_LI=${li})
    endforeach()
endforeach()

# Reimplemented engine code does float math like the original: scalar SSE
# (mulss/addss), not the i686 default x87, whose extended precision would
# change the rounding of e.g. a*b+c.
file(GLOB MC2VR_REIMPL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/reimpl/*/*.cpp)
set_source_files_properties(${MC2VR_REIMPL_SOURCES} PROPERTIES COMPILE_OPTIONS "-msse2;-mfpmath=sse")

# MinGW would otherwise name it libmc2vr_carrier.dll; LoadLibraryW in the
# launcher and the deploy script both expect mc2vr_carrier.dll.
set_target_properties(mc2vr_carrier PROPERTIES PREFIX "")

# OpenXR and Vulkan: headers only. The carrier negotiates with the OpenXR
# runtime DLL itself and takes Vulkan from the library DXVK loaded
# (docs/xr_session.md).
target_include_directories(mc2vr_carrier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(mc2vr_carrier SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/openxr-sdk/include
    ${CMAKE_SOURCE_DIR}/vendor/vulkan-headers/include
)

target_compile_definitions(mc2vr_carrier PRIVATE UNICODE _UNICODE)

target_link_libraries(mc2vr_carrier PRIVATE safetyhook mc2vr_game)

# Static runtimes: an injected DLL must not depend on libstdc++-6.dll etc.
# being present in the Wine prefix.
target_link_options(mc2vr_carrier PRIVATE -static -static-libgcc -static-libstdc++)

# The device hooks use __builtin_return_address(0) for caller-site evidence;
# keep frame pointers so it is reliable on i386.
target_compile_options(mc2vr_carrier PRIVATE -fno-omit-frame-pointer)
