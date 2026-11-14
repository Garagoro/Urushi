#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaPattern {
public:
    static void Register(lua_State* L);

    static int Find(lua_State* L);
    static int FindRip(lua_State* L);
    static int FindEntry(lua_State* L);
    static int FindAll(lua_State* L);
    static int Count(lua_State* L);
    static int GetModule(lua_State* L);
    static int ToRva(lua_State* L);
    static int ToVa(lua_State* L);
};
