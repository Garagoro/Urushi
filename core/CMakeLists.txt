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

add_library(CarX2ScriptHook SHARED
    dllmain.cpp
    logger.h logger.cpp
    hook_manager.h hook_manager.cpp
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
    ${IMGUI_SOURCES}
)

target_include_directories(CarX2ScriptHook PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/third_party/minhook/include"
    "${CMAKE_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_SOURCE_DIR}/third_party/lua"
)

target_link_libraries(CarX2ScriptHook PRIVATE
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

set_target_properties(CarX2ScriptHook PROPERTIES
    OUTPUT_NAME "CarX2ScriptHook"
    PREFIX ""
    SUFFIX ".dll"
)
