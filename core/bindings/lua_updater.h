#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaUpdater {
public:
    static void Register(lua_State* L);

    static int GetStatus(lua_State* L);
    static int Check(lua_State* L);
    static int Install(lua_State* L);
    static int SetDismissed(lua_State* L);
    static int RestartGame(lua_State* L);
};
