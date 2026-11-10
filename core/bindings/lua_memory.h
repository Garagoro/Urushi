#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaMemory {
public:
    static void Register(lua_State* L);

    // Memory primitives
    static int ReadU8(lua_State* L);
    static int ReadU16(lua_State* L);
    static int ReadU32(lua_State* L);
    static int ReadU64(lua_State* L);
    static int ReadPtr(lua_State* L);
    static int ReadFloat(lua_State* L);
    static int ReadString(lua_State* L);
    static int GetModule(lua_State* L);

    // Unity Scene & GameObject primitives
    static int UnityIsMainThread(lua_State* L);
    static int UnityFind(lua_State* L);
    static int UnityFindPlayerCarModel(lua_State* L);
    static int UnityGetAllObjects(lua_State* L);
    static int UnityGetName(lua_State* L);
    static int UnityGetTransform(lua_State* L);
    static int UnityClone(lua_State* L);
    static int UnityDestroy(lua_State* L);
    static int UnitySetActive(lua_State* L);
    static int UnitySetLayer(lua_State* L);
    static int UnitySetPosition(lua_State* L);
    static int UnitySetRotation(lua_State* L);
    static int UnityGetPosition(lua_State* L);
    static int UnityGetRotation(lua_State* L);
};
