#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace LuaAudioIn {

void Register(lua_State* L);

int IsSupported(lua_State* L);
int ListSources(lua_State* L);
int SetSource(lua_State* L);
int GetSource(lua_State* L);
int SetEnabled(lua_State* L);
int IsEnabled(lua_State* L);
int GetStatus(lua_State* L);
int GetLevels(lua_State* L);
int GetSpectrum(lua_State* L);
int SetGain(lua_State* L);
int GetGain(lua_State* L);
int SetSmoothing(lua_State* L);
int GetSmoothing(lua_State* L);

} // namespace LuaAudioIn
