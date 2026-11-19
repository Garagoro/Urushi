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
    static int Compile(lua_State* L);
    static int Unload(lua_State* L);
    static int Reload(lua_State* L);
    static int SetEnabled(lua_State* L);
    static int IsEnabled(lua_State* L);
    static int SetTargetEnabled(lua_State* L);
    static int IsTargetEnabled(lua_State* L);
    static int SetMode(lua_State* L);
    static int SetStealth(lua_State* L);
    static int SetParam(lua_State* L);
    static int GetParam(lua_State* L);
    static int SetTargetParam(lua_State* L);
    static int GetTargetParam(lua_State* L);
    static int SetTex(lua_State* L);
    static int ClearTex(lua_State* L);
    static int GetTexInfo(lua_State* L);
    static int SetTexPlayback(lua_State* L);
    static int ListTextures(lua_State* L);
    static int SetGameInput(lua_State* L);
    static int GetGameInput(lua_State* L);
    static int ListShaders(lua_State* L);
    static int GetStatus(lua_State* L);
    static int GetSpatial(lua_State* L);
    static int GetParts(lua_State* L);
    static int SetParts(lua_State* L);
    static int GetBodyExtras(lua_State* L);
    static int SetBodyExtras(lua_State* L);
};
