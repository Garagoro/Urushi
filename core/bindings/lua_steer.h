#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaSteer {
public:
    static void Register(lua_State* L);

    static int GetConfig(lua_State* L);
    static int SetConfig(lua_State* L);
    static int SetRecommended(lua_State* L);
    static int SetVanilla(lua_State* L);
    static int GetStats(lua_State* L);
};
