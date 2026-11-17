#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace LuaPostFX {
void Register(lua_State* L);
int GetShaderList(lua_State* L);
int Load(lua_State* L);
int Unload(lua_State* L);
int SetEnabled(lua_State* L);
int IsEnabled(lua_State* L);
int SetParam(lua_State* L);
int GetParams(lua_State* L);
}
