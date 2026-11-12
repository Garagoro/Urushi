#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// Lua module `cloud`: script library (see cloud_scripts.h)
class LuaCloud {
public:
    static void Register(lua_State* L);
};
