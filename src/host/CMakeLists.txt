# OpenXR/D3D11 host (x86_64). See docs/s4_handover.md.
add_executable(mc2vr_host
    main.cpp
    log.cpp
    ipc.cpp
    d3d.cpp
    eyes.cpp
    mock.cpp
    xr_session.cpp
)

target_link_libraries(mc2vr_host PRIVATE openxr_loader d3d11 dxgi)
target_include_directories(mc2vr_host PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/openxr-sdk/include
    ${CMAKE_SOURCE_DIR}/src/common
)

# Static runtimes: nothing but system DLLs needed inside the Wine prefix.
target_link_options(mc2vr_host PRIVATE -static -static-libgcc -static-libstdc++)
