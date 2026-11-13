set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

add_library(urushi SHARED
    dllmain.cpp
    logger.h logger.cpp
    settings.h settings.cpp
    cloud_scripts.h cloud_scripts.cpp
    crash_handler.h crash_handler.cpp
    safe_mode.h safe_mode.cpp
    lua_sandbox.h lua_sandbox.cpp
    lua_hardening.h lua_hardening.cpp
    ptr_registry.h
    input_manager.h input_manager.cpp
    hook_manager.h hook_manager.cpp
    pattern_scanner.h pattern_scanner.cpp
    game_build.h game_build.cpp
    native_carx.h native_carx.cpp
    steer_assist.h steer_assist.cpp
    unity_bridge.h unity_bridge.cpp
    ecs_collision.h ecs_collision.cpp
    car_sim_optimizer.h car_sim_optimizer.cpp
    d3d11_hook.h d3d11_hook.cpp
    carfx_shader_decl.h carfx_shader_decl.cpp
    carfx.h carfx.cpp
    replay_engine.h replay_engine.cpp
    file_watcher.h file_watcher.cpp
    lua_engine.h lua_engine.cpp
    ui_schema.h ui_schema.cpp
    urushi_ui.h urushi_ui.cpp
    dev_pipe.h dev_pipe.cpp
    updater.h updater.cpp
    bindings/lua_carx.h bindings/lua_carx.cpp
    bindings/lua_steer.h bindings/lua_steer.cpp
    bindings/lua_carfx.h bindings/lua_carfx.cpp
    bindings/lua_imgui.h bindings/lua_imgui.cpp
    bindings/lua_replay.h bindings/lua_replay.cpp
    bindings/lua_memory.h bindings/lua_memory.cpp
    bindings/lua_updater.h bindings/lua_updater.cpp
    bindings/lua_optimizer.h bindings/lua_optimizer.cpp
    bindings/lua_urushi.h bindings/lua_urushi.cpp
    bindings/lua_cloud.h bindings/lua_cloud.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(urushi PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/minhook/include"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/lua"
)

target_link_libraries(urushi PRIVATE
    minhook
    lua
    d3d11
    dxgi
    shlwapi
    windowscodecs
    ole32
    winhttp
    bcrypt
)

set_target_properties(urushi PROPERTIES
    OUTPUT_NAME "urushi"
    PREFIX ""
    SUFFIX ".dll"
)

# Linker map next to urushi.dll (does not change the generated code): lets tools/resolve_crash.py turn the
# "urushi_*.dll+0xOFFSET" frames of a crash report into function names. Keep the .map of every DLL you ship.
if(MSVC)
    target_link_options(urushi PRIVATE /MAP)
endif()
