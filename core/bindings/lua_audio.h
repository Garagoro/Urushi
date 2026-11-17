#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace LuaAudio {
void Register(lua_State* L);
int List(lua_State* L);
int Play(lua_State* L);
int Stop(lua_State* L);
int StopAll(lua_State* L);
int SetVolume(lua_State* L);
int IsPlaying(lua_State* L);
}
