# Launcher: starts the game, waits for the main loop to come alive, then
# injects the carrier DLL. See docs/launcher_plan.md.
add_executable(mc2vr_launcher
    main.c
    boot_wait.c
    inject.c
)

target_include_directories(mc2vr_launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_compile_definitions(mc2vr_launcher PRIVATE UNICODE _UNICODE)

# Fully static so nothing depends on runtime DLLs inside the Wine prefix.
target_link_options(mc2vr_launcher PRIVATE -static -static-libgcc)
