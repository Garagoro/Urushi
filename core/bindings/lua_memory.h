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
    static int ReadI32(lua_State* L);
    static int ReadU64(lua_State* L);
    static int ReadPtr(lua_State* L);
    static int ReadFloat(lua_State* L);
    static int ReadBytes(lua_State* L);
    static int ReadString(lua_State* L);
    static int WriteU8(lua_State* L);
    static int WriteU16(lua_State* L);
    static int WriteU32(lua_State* L);
    static int WriteI32(lua_State* L);
    static int WriteU64(lua_State* L);
    static int WritePtr(lua_State* L);
    static int WriteFloat(lua_State* L);
    static int WriteBytes(lua_State* L);
    static int PatchBytes(lua_State* L);
    static int RestorePatch(lua_State* L);
    static int RestoreAllPatches(lua_State* L);
    static int Scan(lua_State* L);
    static int ResolveRip(lua_State* L);
    static int Protect(lua_State* L);
    static int GetModule(lua_State* L);
    static int AllocNear(lua_State* L);
    static int FreeAlloc(lua_State* L);

    // Native Function Invocation (Windows x64 fastcall)
    static int Call(lua_State* L);
    static int CallFloat(lua_State* L);
    static int CallMain(lua_State* L);

    // C++ cleanup hook for hot reload & shutdown
    static void RestoreAllPatchesNative();

    // Unity Scene & GameObject primitives
    static int UnityIsMainThread(lua_State* L);
    static int UnityFind(lua_State* L);
    static int UnityFindPlayerCarModel(lua_State* L);
    static int UnityGetAllObjects(lua_State* L);
    static int UnityGetComponents(lua_State* L);
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
