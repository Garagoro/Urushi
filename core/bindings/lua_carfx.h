#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaCarFX {
public:
    static void Register(lua_State* L);

    static int Load(lua_State* L);
    static int Unload(lua_State* L);
    static int Reload(lua_State* L);
    static int SetEnabled(lua_State* L);
    static int IsEnabled(lua_State* L);
    static int SetMode(lua_State* L);
    static int SetStealth(lua_State* L);
    static int SetParam(lua_State* L);
    static int GetParam(lua_State* L);
    static int SetTex(lua_State* L);
    static int ClearTex(lua_State* L);
    static int SetGameInput(lua_State* L);
    static int GetGameInput(lua_State* L);
    static int ListShaders(lua_State* L);
    static int GetStatus(lua_State* L);
    static int GetSpatial(lua_State* L);
    static int GetParts(lua_State* L);
    static int SetParts(lua_State* L);
};
