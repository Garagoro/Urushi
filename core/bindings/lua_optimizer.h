#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaOptimizer {
public:
    static void Register(lua_State* L);

    static int GetConfig(lua_State* L);
    static int SetConfig(lua_State* L);
    static int ResetDefaults(lua_State* L);
    static int GetStats(lua_State* L);
    static int GetCars(lua_State* L);
};
