#include "lua_pattern.h"
#include "../pattern_scanner.h"
#include <windows.h>
#include <psapi.h>
#include <string>
#include <vector>

static HMODULE GetMod(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) {
        HMODULE hMod = GetModuleHandleA("GameAssembly.dll");
        if (!hMod) hMod = GetModuleHandleA(nullptr);
        return hMod;
    }
    const char* name = lua_tostring(L, idx);
    if (!name || name[0] == '\0') {
        HMODULE hMod = GetModuleHandleA("GameAssembly.dll");
        if (!hMod) hMod = GetModuleHandleA(nullptr);
        return hMod;
    }
    return GetModuleHandleA(name);
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
    if (lua_isstring(L, idx)) {
        const char* s = lua_tostring(L, idx);
        if (s) {
            return static_cast<uintptr_t>(std::strtoull(s, nullptr, 0));
        }
    }
    return 0;
}

static const luaL_Reg kPatternFuncs[] = {
    { "find",       LuaPattern::Find },
    { "find_rip",   LuaPattern::FindRip },
    { "find_entry", LuaPattern::FindEntry },
    { "find_all",   LuaPattern::FindAll },
    { "count",      LuaPattern::Count },
    { "get_module", LuaPattern::GetModule },
    { "to_rva",     LuaPattern::ToRva },
    { "to_va",      LuaPattern::ToVa },
    { nullptr, nullptr }
};

void LuaPattern::Register(lua_State* L) {
    luaL_newlib(L, kPatternFuncs);
    lua_setglobal(L, "pattern");
}

int LuaPattern::Find(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    const char* pat = luaL_checkstring(L, 2);
    const char* name = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;

    uintptr_t va = PatternScanner::Find(hMod, pat, name);
    if (va != 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(va));
        char hexBuf[32];
        sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(va));
        lua_pushstring(L, hexBuf);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

int LuaPattern::FindRip(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    const char* pat = luaL_checkstring(L, 2);
    int instrLen = static_cast<int>(luaL_checkinteger(L, 3));
    int ripOffset = static_cast<int>(luaL_checkinteger(L, 4));
    size_t instrOffsetInPattern = static_cast<size_t>(luaL_optinteger(L, 5, 0));
    const char* name = lua_isstring(L, 6) ? lua_tostring(L, 6) : nullptr;

    uintptr_t va = 0;
    if (instrOffsetInPattern > 0) {
        va = PatternScanner::FindRip(hMod, pat, instrLen, ripOffset, instrOffsetInPattern, name);
    } else {
        va = PatternScanner::FindRip(hMod, pat, instrLen, ripOffset, name);
    }

    if (va != 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(va));
        char hexBuf[32];
        sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(va));
        lua_pushstring(L, hexBuf);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

int LuaPattern::FindEntry(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    const char* bodyPat = luaL_checkstring(L, 2);
    size_t maxBacktrack = static_cast<size_t>(luaL_optinteger(L, 3, 256));
    const char* name = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr;

    uintptr_t va = PatternScanner::FindFunctionEntry(hMod, bodyPat, maxBacktrack, name);
    if (va != 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(va));
        char hexBuf[32];
        sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(va));
        lua_pushstring(L, hexBuf);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

int LuaPattern::FindAll(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    const char* pat = luaL_checkstring(L, 2);
    size_t maxMatches = static_cast<size_t>(luaL_optinteger(L, 3, 100));

    std::vector<uintptr_t> matches = PatternScanner::FindAll(hMod, pat, maxMatches);
    lua_createtable(L, static_cast<int>(matches.size()), 0);
    for (size_t i = 0; i < matches.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(matches[i]));
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int LuaPattern::Count(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    const char* pat = luaL_checkstring(L, 2);
    size_t stopAt = static_cast<size_t>(luaL_optinteger(L, 3, 100));

    size_t count = PatternScanner::CountMatches(hMod, pat, stopAt);
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    return 1;
}

int LuaPattern::GetModule(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    if (!hMod) {
        lua_pushnil(L);
        return 1;
    }

    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) {
        lua_pushnil(L);
        return 1;
    }

    char path[MAX_PATH] = {};
    GetModuleFileNameA(hMod, path, MAX_PATH);

    lua_createtable(L, 0, 4);

    uintptr_t base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    lua_pushinteger(L, static_cast<lua_Integer>(base));
    lua_setfield(L, -2, "base");

    char hexBuf[32];
    sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(base));
    lua_pushstring(L, hexBuf);
    lua_setfield(L, -2, "base_hex");

    lua_pushinteger(L, static_cast<lua_Integer>(mi.SizeOfImage));
    lua_setfield(L, -2, "size");

    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");

    return 1;
}

int LuaPattern::ToRva(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    uintptr_t addr = GetAddr(L, 2);
    if (!hMod || !addr) {
        lua_pushnil(L);
        return 1;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
    if (addr < base) {
        lua_pushnil(L);
        return 1;
    }

    uintptr_t rva = addr - base;
    lua_pushinteger(L, static_cast<lua_Integer>(rva));
    char hexBuf[32];
    sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(rva));
    lua_pushstring(L, hexBuf);
    return 2;
}

int LuaPattern::ToVa(lua_State* L) {
    HMODULE hMod = GetMod(L, 1);
    uintptr_t rva = GetAddr(L, 2);
    if (!hMod) {
        lua_pushnil(L);
        return 1;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
    uintptr_t va = base + rva;
    lua_pushinteger(L, static_cast<lua_Integer>(va));
    char hexBuf[32];
    sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(va));
    lua_pushstring(L, hexBuf);
    return 2;
}
