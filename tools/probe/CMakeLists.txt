# S4-2 shared-handle probe: two tiny processes answering whether DXVK D3D9
# shared textures (legacy pSharedHandle) open in DXVK D3D11 across processes —
# the key S4 risk (stereo_design.md §S4 risk 1). The producer builds in the
# 32-bit tree, the consumer in the 64-bit tree; drive both (same Proton
# prefix, concurrently) with tools/probe/run_shared_handle.sh.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(mc2vr_probe_shared_consumer EXCLUDE_FROM_ALL d3d11_consumer.cpp)
    target_link_libraries(mc2vr_probe_shared_consumer d3d11)
    target_link_options(mc2vr_probe_shared_consumer PRIVATE -static -static-libgcc -static-libstdc++)
else()
    add_executable(mc2vr_probe_shared_producer EXCLUDE_FROM_ALL d3d9_producer.cpp)
    target_link_libraries(mc2vr_probe_shared_producer d3d9)
    target_link_options(mc2vr_probe_shared_producer PRIVATE -static -static-libgcc -static-libstdc++)
endif()
