#include "lua_memory.h"
#include "../unity_bridge.h"
#include "../ptr_registry.h"
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>

static bool SafeRead(const void* addr, void* outBuf, size_t size) {
    __try {
        if (!addr || (uintptr_t)addr < 0x10000 || !outBuf) return false;
        memcpy(outBuf, addr, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uintptr_t GetAddr(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return reinterpret_cast<uintptr_t>(lua_touserdata(L, idx));
    }
    if (lua_isinteger(L, idx)) {
        return static_cast<uintptr_t>(lua_tointeger(L, idx));
    }
    if (lua_isnumber(L, idx)) {
        return static_cast<uintptr_t>(lua_tonumber(L, idx));
    }
    return 0;
}

int LuaMemory::ReadU8(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint8_t val = 0;
    if (SafeRead(reinterpret_cast<void*>(addr), &val, sizeof(val))) {
        lua_pushinteger(L, val);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::ReadU16(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint16_t val = 0;
    if (SafeRead(reinterpret_cast<void*>(addr), &val, sizeof(val))) {
        lua_pushinteger(L, val);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::ReadU32(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint32_t val = 0;
    if (SafeRead(reinterpret_cast<void*>(addr), &val, sizeof(val))) {
        lua_pushinteger(L, val);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::ReadU64(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint64_t val = 0;
    if (SafeRead(reinterpret_cast<void*>(addr), &val, sizeof(val))) {
        lua_pushinteger(L, static_cast<lua_Integer>(val));
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::ReadPtr(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    void* val = nullptr;
    if (SafeRead(reinterpret_cast<void*>(addr), &val, sizeof(val))) {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(val)));
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::ReadFloat(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    float val = 0.0f;
    if (SafeRead(reinterpret_cast<void*>(addr), &val, sizeof(val))) {
        lua_pushnumber(L, val);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::ReadString(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    int maxLen = static_cast<int>(luaL_optinteger(L, 2, 256));
    if (maxLen <= 0) maxLen = 1;
    if (maxLen > 4096) maxLen = 4096;

    std::vector<char> buf(maxLen + 1, 0);
    if (SafeRead(reinterpret_cast<void*>(addr), buf.data(), maxLen)) {
        buf[maxLen] = '\0';
        lua_pushstring(L, buf.data());
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::GetModule(lua_State* L) {
    const char* modName = luaL_checkstring(L, 1);
    std::wstring wname(modName, modName + strlen(modName));
    HMODULE hMod = GetModuleHandleW(wname.c_str());
    if (!hMod) {
        lua_pushnil(L);
        return 1;
    }

    MODULEINFO mi = { 0 };
    GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi));

    lua_newtable(L);
    lua_pushlightuserdata(L, mi.lpBaseOfDll);
    lua_setfield(L, -2, "base_ptr");
    lua_pushinteger(L, reinterpret_cast<lua_Integer>(mi.lpBaseOfDll));
    lua_setfield(L, -2, "base");
    lua_pushinteger(L, mi.SizeOfImage);
    lua_setfield(L, -2, "size");
    return 1;
}

// ================= Unity Scene Bindings =================

int LuaMemory::UnityIsMainThread(lua_State* L) {
    lua_pushboolean(L, UnityBridge::IsOnMainThread());
    return 1;
}

int LuaMemory::UnityFind(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* go = UnityBridge::FindGameObjectByName(name);
    if (go) {
        PtrRegistry::Push(L, go);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::UnityFindPlayerCarModel(lua_State* L) {
    void* rootTf = nullptr;
    void* modelGo = UnityBridge::FindPlayerCarModel(&rootTf);
    if (!modelGo) {
        lua_pushnil(L);
        return 1;
    }
    PtrRegistry::Push(L, modelGo);
    if (rootTf) {
        PtrRegistry::Push(L, rootTf);
        return 2;
    }
    return 1;
}

int LuaMemory::UnityGetName(lua_State* L) {
    void* go = PtrRegistry::Arg(L, 1);
    if (!go) {
        lua_pushnil(L);
        return 1;
    }
    std::string name = UnityBridge::GetGameObjectName(go);
    if (!name.empty()) {
        lua_pushstring(L, name.c_str());
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::UnityGetTransform(lua_State* L) {
    void* go = PtrRegistry::Arg(L, 1);
    void* tf = UnityBridge::GetTransformOfGameObject(go);
    if (tf) {
        PtrRegistry::Push(L, tf);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::UnityGetAllObjects(lua_State* L) {
    int maxObjects = static_cast<int>(luaL_optinteger(L, 1, 20000));
    if (maxObjects <= 0) maxObjects = 20000;
    auto objects = UnityBridge::GetAllGameObjects(maxObjects);

    lua_newtable(L);
    int outIdx = 1;
    for (const auto& obj : objects) {
        lua_newtable(L);

        PtrRegistry::Push(L, obj.go);
        lua_setfield(L, -2, "ptr");

        lua_pushstring(L, obj.name.empty() ? "Unnamed" : obj.name.c_str());
        lua_setfield(L, -2, "name");

        if (obj.transform) {
            PtrRegistry::Push(L, obj.transform);
            lua_setfield(L, -2, "transform");
        }
        if (obj.parentTransform) {
            PtrRegistry::Push(L, obj.parentTransform);
            lua_setfield(L, -2, "parent_transform");
        }
        if (obj.hasPos) {
            lua_pushnumber(L, obj.pos.x);
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, obj.pos.y);
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, obj.pos.z);
            lua_setfield(L, -2, "z");
        }

        lua_rawseti(L, -2, outIdx++);
    }
    return 1;
}

int LuaMemory::UnityClone(lua_State* L) {
    void* go = PtrRegistry::Arg(L, 1);
    void* cloned = UnityBridge::CloneGameObjectDirect(go);
    if (cloned) {
        PtrRegistry::Push(L, cloned);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::UnityDestroy(lua_State* L) {
    void* obj = PtrRegistry::Arg(L, 1);
    float delay = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    lua_pushboolean(L, UnityBridge::DestroyObjectDirect(obj, delay));
    return 1;
}

int LuaMemory::UnitySetActive(lua_State* L) {
    void* go = PtrRegistry::Arg(L, 1);
    bool active = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, UnityBridge::SetActiveDirect(go, active));
    return 1;
}

int LuaMemory::UnitySetLayer(lua_State* L) {
    void* go = PtrRegistry::Arg(L, 1);
    int layer = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, UnityBridge::SetLayerDirect(go, layer));
    return 1;
}

int LuaMemory::UnitySetPosition(lua_State* L) {
    void* tf = PtrRegistry::Arg(L, 1);
    UnityVector3 pos{
        static_cast<float>(lua_tonumber(L, 2)),
        static_cast<float>(lua_tonumber(L, 3)),
        static_cast<float>(lua_tonumber(L, 4))
    };
    lua_pushboolean(L, UnityBridge::SetTransformPositionDirect(tf, pos));
    return 1;
}

int LuaMemory::UnitySetRotation(lua_State* L) {
    void* tf = PtrRegistry::Arg(L, 1);
    UnityVector4 rot{
        static_cast<float>(lua_tonumber(L, 2)),
        static_cast<float>(lua_tonumber(L, 3)),
        static_cast<float>(lua_tonumber(L, 4)),
        static_cast<float>(lua_tonumber(L, 5))
    };
    lua_pushboolean(L, UnityBridge::SetTransformRotationDirect(tf, rot));
    return 1;
}

int LuaMemory::UnityGetPosition(lua_State* L) {
    void* tf = PtrRegistry::Arg(L, 1);
    if (!tf) {
        lua_pushnil(L);
        return 1;
    }
    UnityVector3 pos{};
    UnityVector4 rot{};
    if (UnityBridge::IsOnMainThread() && UnityBridge::GetTransformPosRotDirect(tf, pos, rot)) {
        lua_pushnumber(L, pos.x);
        lua_pushnumber(L, pos.y);
        lua_pushnumber(L, pos.z);
        return 3;
    }
    if (UnityBridge::GetTransformHierarchyPosition(tf, pos)) {
        lua_pushnumber(L, pos.x);
        lua_pushnumber(L, pos.y);
        lua_pushnumber(L, pos.z);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::UnityGetRotation(lua_State* L) {
    void* tf = PtrRegistry::Arg(L, 1);
    if (!tf || !UnityBridge::IsOnMainThread()) {
        lua_pushnil(L);
        return 1;
    }
    UnityVector3 pos{};
    UnityVector4 rot{};
    if (UnityBridge::GetTransformPosRotDirect(tf, pos, rot)) {
        lua_pushnumber(L, rot.x);
        lua_pushnumber(L, rot.y);
        lua_pushnumber(L, rot.z);
        lua_pushnumber(L, rot.w);
        return 4;
    }
    lua_pushnil(L);
    return 1;
}

static const luaL_Reg mem_funcs[] = {
    { "read_u8", LuaMemory::ReadU8 },
    { "read_u16", LuaMemory::ReadU16 },
    { "read_u32", LuaMemory::ReadU32 },
    { "read_u64", LuaMemory::ReadU64 },
    { "read_ptr", LuaMemory::ReadPtr },
    { "read_float", LuaMemory::ReadFloat },
    { "read_string", LuaMemory::ReadString },
    { "get_module", LuaMemory::GetModule },
    { nullptr, nullptr }
};

static const luaL_Reg unity_funcs[] = {
    { "is_main_thread", LuaMemory::UnityIsMainThread },
    { "find", LuaMemory::UnityFind },
    { "find_player_car_model", LuaMemory::UnityFindPlayerCarModel },
    { "get_all_objects", LuaMemory::UnityGetAllObjects },
    { "get_name", LuaMemory::UnityGetName },
    { "get_transform", LuaMemory::UnityGetTransform },
    { "clone", LuaMemory::UnityClone },
    { "destroy", LuaMemory::UnityDestroy },
    { "set_active", LuaMemory::UnitySetActive },
    { "set_layer", LuaMemory::UnitySetLayer },
    { "set_position", LuaMemory::UnitySetPosition },
    { "set_rotation", LuaMemory::UnitySetRotation },
    { "get_position", LuaMemory::UnityGetPosition },
    { "get_rotation", LuaMemory::UnityGetRotation },
    { nullptr, nullptr }
};

void LuaMemory::Register(lua_State* L) {
    luaL_newlib(L, mem_funcs);
    lua_setglobal(L, "memory");

    luaL_newlib(L, unity_funcs);
    lua_setglobal(L, "unity");
}
