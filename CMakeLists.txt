cmake_minimum_required(VERSION 3.20)
project(CarX2ScriptHook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook
add_subdirectory(third_party/minhook)

# Lua 5.4 Static Library
file(GLOB LUA_SOURCES "third_party/lua/*.c")
list(REMOVE_ITEM LUA_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/lua.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/onelua.c"
)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC third_party/lua)

# Sub-projects
add_subdirectory(proxy)
add_subdirectory(core)

# Custom target to deploy directly to the game directory
set(GAME_DIR "C:/Program Files (x86)/Steam/steamapps/common/CarX Drift Racing Online 2")

add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:version>" "${GAME_DIR}/version.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:CarX2ScriptHook>" "${GAME_DIR}/CarX2ScriptHook.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${GAME_DIR}/scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${GAME_DIR}/shaders"
    DEPENDS version CarX2ScriptHook
    COMMENT "Deploying CarX2ScriptHook to CarX Drift Racing Online 2..."
)
