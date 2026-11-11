#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaUrushi {
public:
    static void Register(lua_State* L);
};
