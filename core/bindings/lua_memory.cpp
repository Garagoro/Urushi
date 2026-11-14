#include "lua_memory.h"
#include "../unity_bridge.h"
#include "../ptr_registry.h"
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include "../pattern_scanner.h"

struct SavedPatch {
    std::vector<uint8_t> originalBytes;
    bool active = false;
};

static std::mutex s_patchMutex;
static std::unordered_map<uintptr_t, SavedPatch> s_patchRegistry;
static std::vector<void*> s_nearAllocations;

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

static bool SafeWrite(void* addr, const void* inBuf, size_t size) {
    __try {
        if (!addr || (uintptr_t)addr < 0x10000 || !inBuf) return false;
        memcpy(addr, inBuf, size);
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

int LuaMemory::ReadI32(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    int32_t val = 0;
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

int LuaMemory::ReadBytes(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    size_t sz = static_cast<size_t>(luaL_checkinteger(L, 2));
    if (!addr || sz == 0 || sz > 0x100000) {
        lua_newtable(L);
        return 1;
    }
    std::vector<uint8_t> buf(sz);
    if (!SafeRead(reinterpret_cast<void*>(addr), buf.data(), sz)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(sz), 0);
    for (size_t i = 0; i < sz; ++i) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
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

int LuaMemory::WriteU8(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint8_t val = static_cast<uint8_t>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WriteU16(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint16_t val = static_cast<uint16_t>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WriteU32(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint32_t val = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WriteI32(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    int32_t val = static_cast<int32_t>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WriteU64(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uint64_t val = static_cast<uint64_t>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WritePtr(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    uintptr_t val = GetAddr(L, 2);
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WriteFloat(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    float val = static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, SafeWrite(reinterpret_cast<void*>(addr), &val, sizeof(val)) ? 1 : 0);
    return 1;
}

int LuaMemory::WriteBytes(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "invalid address");
        return 2;
    }

    std::vector<uint8_t> bytes;
    if (lua_isstring(L, 2)) {
        const char* str = lua_tostring(L, 2);
        std::istringstream iss(str);
        std::string token;
        while (iss >> token) {
            if (token == "?" || token == "??") {
                bytes.push_back(0x90);
            } else {
                bytes.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
            }
        }
    } else if (lua_istable(L, 2)) {
        int len = static_cast<int>(lua_rawlen(L, 2));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, 2, i);
            bytes.push_back(static_cast<uint8_t>(lua_tointeger(L, -1)));
            lua_pop(L, 1);
        }
    } else {
        lua_pushboolean(L, false);
        lua_pushstring(L, "expected hex string or byte table");
        return 2;
    }

    if (bytes.empty()) {
        lua_pushboolean(L, true);
        lua_pushinteger(L, 0);
        return 2;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), bytes.size(), PAGE_EXECUTE_READWRITE, &oldProt)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "VirtualProtect failed");
        return 2;
    }

    memcpy(reinterpret_cast<void*>(addr), bytes.data(), bytes.size());
    VirtualProtect(reinterpret_cast<void*>(addr), bytes.size(), oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), bytes.size());

    lua_pushboolean(L, true);
    lua_pushinteger(L, static_cast<lua_Integer>(bytes.size()));
    return 2;
}

void LuaMemory::RestoreAllPatchesNative() {
    bool anyRestored = false;
    std::vector<void*> toFree;
    {
        std::lock_guard<std::mutex> lock(s_patchMutex);
        for (auto& kv : s_patchRegistry) {
            if (!kv.second.active || kv.second.originalBytes.empty()) continue;
            uintptr_t addr = kv.first;
            size_t sz = kv.second.originalBytes.size();
            DWORD oldProt = 0;
            if (VirtualProtect(reinterpret_cast<void*>(addr), sz, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(reinterpret_cast<void*>(addr), kv.second.originalBytes.data(), sz);
                VirtualProtect(reinterpret_cast<void*>(addr), sz, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), sz);
                anyRestored = true;
            }
            kv.second.active = false;
        }
        toFree.swap(s_nearAllocations);
    }
    if (anyRestored && !toFree.empty()) {
        // Give running game threads a brief window after unpatching before freeing RIP-referenced blocks
        Sleep(40);
    }
    for (void* p : toFree) {
        if (p) VirtualFree(p, 0, MEM_RELEASE);
    }
}

int LuaMemory::AllocNear(lua_State* L) {
    const char* modName = luaL_checkstring(L, 1);
    lua_Integer reqSize = luaL_optinteger(L, 2, 4096);
    if (reqSize <= 0 || reqSize > 1048576) {
        lua_pushnil(L);
        return 1;
    }
    HMODULE hMod = GetModuleHandleA(modName);
    if (!hMod) {
        lua_pushnil(L);
        return 1;
    }
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(hMod);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t maxDist = 0x7FFF0000ULL;
    const uintptr_t minAddr = (moduleBase > maxDist) ? (moduleBase - maxDist) : 0x10000;
    const uintptr_t maxAddr = moduleBase + maxDist;

    auto tryAllocFrom = [&](uintptr_t start, bool forward) -> void* {
        uintptr_t cur = forward
            ? ((start + gran - 1) & ~(gran - 1))
            : (start & ~(gran - 1));
        for (int step = 0; step < 32768; ++step) {
            if (forward && cur >= maxAddr) break;
            if (!forward && cur <= minAddr) break;

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State == MEM_FREE && mbi.RegionSize >= static_cast<SIZE_T>(reqSize)) {
                uintptr_t cand = forward
                    ? ((reinterpret_cast<uintptr_t>(mbi.BaseAddress) + gran - 1) & ~(gran - 1))
                    : ((reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - gran) & ~(gran - 1));
                if (cand >= minAddr && cand <= maxAddr) {
                    void* p = VirtualAlloc(reinterpret_cast<LPVOID>(cand), static_cast<SIZE_T>(reqSize),
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (p) return p;
                }
            }
            if (forward) {
                uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= cur) break;
                cur = (next + gran - 1) & ~(gran - 1);
            } else {
                uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                if (regionBase <= gran || regionBase >= cur) break;
                cur = (regionBase - gran) & ~(gran - 1);
            }
        }
        return nullptr;
    };

    void* page = tryAllocFrom(moduleBase, false);
    if (!page) page = tryAllocFrom(moduleBase + 0x700000ULL, true);
    if (!page) {
        lua_pushnil(L);
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(s_patchMutex);
        s_nearAllocations.push_back(page);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(page)));
    return 1;
}

int LuaMemory::FreeAlloc(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushboolean(L, false);
        return 1;
    }
    void* target = reinterpret_cast<void*>(addr);
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(s_patchMutex);
        for (auto it = s_nearAllocations.begin(); it != s_nearAllocations.end(); ++it) {
            if (*it == target) {
                s_nearAllocations.erase(it);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, VirtualFree(target, 0, MEM_RELEASE) != 0);
    return 1;
}

int LuaMemory::PatchBytes(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "invalid address");
        return 2;
    }

    std::vector<uint8_t> bytes;
    if (lua_isstring(L, 2)) {
        const char* str = lua_tostring(L, 2);
        std::istringstream iss(str);
        std::string token;
        while (iss >> token) {
            if (token == "?" || token == "??") {
                bytes.push_back(0x90);
            } else {
                bytes.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
            }
        }
    } else if (lua_istable(L, 2)) {
        int len = static_cast<int>(lua_rawlen(L, 2));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, 2, i);
            bytes.push_back(static_cast<uint8_t>(lua_tointeger(L, -1)));
            lua_pop(L, 1);
        }
    } else {
        lua_pushboolean(L, false);
        lua_pushstring(L, "expected hex string or byte table");
        return 2;
    }

    if (bytes.empty()) {
        lua_pushboolean(L, true);
        lua_pushinteger(L, 0);
        return 2;
    }

    size_t sz = bytes.size();
    {
        std::lock_guard<std::mutex> lock(s_patchMutex);
        auto it = s_patchRegistry.find(addr);
        if (it == s_patchRegistry.end()) {
            std::vector<uint8_t> orig(sz);
            if (SafeRead(reinterpret_cast<void*>(addr), orig.data(), sz)) {
                s_patchRegistry[addr] = { orig, false };
            }
        }
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sz, PAGE_EXECUTE_READWRITE, &oldProt)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "VirtualProtect failed");
        return 2;
    }

    memcpy(reinterpret_cast<void*>(addr), bytes.data(), sz);
    VirtualProtect(reinterpret_cast<void*>(addr), sz, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), sz);

    {
        std::lock_guard<std::mutex> lock(s_patchMutex);
        s_patchRegistry[addr].active = true;
    }

    lua_pushboolean(L, true);
    lua_pushinteger(L, static_cast<lua_Integer>(sz));
    return 2;
}

int LuaMemory::RestorePatch(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    if (!addr) {
        lua_pushboolean(L, false);
        return 1;
    }

    std::vector<uint8_t> orig;
    {
        std::lock_guard<std::mutex> lock(s_patchMutex);
        auto it = s_patchRegistry.find(addr);
        if (it != s_patchRegistry.end() && it->second.active) {
            orig = it->second.originalBytes;
            it->second.active = false;
        }
    }

    if (orig.empty()) {
        lua_pushboolean(L, false);
        return 1;
    }

    size_t sz = orig.size();
    DWORD oldProt = 0;
    if (VirtualProtect(reinterpret_cast<void*>(addr), sz, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(reinterpret_cast<void*>(addr), orig.data(), sz);
        VirtualProtect(reinterpret_cast<void*>(addr), sz, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), sz);
        lua_pushboolean(L, true);
        return 1;
    }

    lua_pushboolean(L, false);
    return 1;
}

int LuaMemory::RestoreAllPatches(lua_State* L) {
    RestoreAllPatchesNative();
    lua_pushboolean(L, true);
    return 1;
}

int LuaMemory::Scan(lua_State* L) {
    HMODULE hMod = nullptr;
    const char* pat = nullptr;
    if (lua_isstring(L, 2)) {
        const char* modName = lua_tostring(L, 1);
        if (modName && modName[0] != '\0') hMod = GetModuleHandleA(modName);
        pat = luaL_checkstring(L, 2);
    } else {
        pat = luaL_checkstring(L, 1);
    }
    if (!hMod) {
        hMod = GetModuleHandleA("GameAssembly.dll");
        if (!hMod) hMod = GetModuleHandleA(nullptr);
    }

    uintptr_t va = PatternScanner::FindCached(hMod, pat);
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

int LuaMemory::ResolveRip(lua_State* L) {
    uintptr_t instrAddr = GetAddr(L, 1);
    int ripOffset = static_cast<int>(luaL_checkinteger(L, 2));
    int instrLen = static_cast<int>(luaL_checkinteger(L, 3));

    uintptr_t target = PatternScanner::ResolveRip(instrAddr, instrLen, ripOffset);
    if (target != 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(target));
        char hexBuf[32];
        sprintf_s(hexBuf, "0x%llX", static_cast<unsigned long long>(target));
        lua_pushstring(L, hexBuf);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

int LuaMemory::Protect(lua_State* L) {
    uintptr_t addr = GetAddr(L, 1);
    size_t size = static_cast<size_t>(luaL_checkinteger(L, 2));
    DWORD newProt = static_cast<DWORD>(luaL_checkinteger(L, 3));

    DWORD oldProt = 0;
    if (VirtualProtect(reinterpret_cast<void*>(addr), size, newProt, &oldProt)) {
        lua_pushinteger(L, oldProt);
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

typedef uintptr_t(__fastcall* t_NativeFn8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef float(__fastcall* t_NativeFn8Float)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static uintptr_t GetNativeArgVal(lua_State* L, int idx) {
    if (idx > lua_gettop(L) || lua_isnil(L, idx)) return 0;
    if (lua_islightuserdata(L, idx)) {
        return reinterpret_cast<uintptr_t>(lua_touserdata(L, idx));
    }
    if (lua_isboolean(L, idx)) {
        return lua_toboolean(L, idx) ? 1 : 0;
    }
    if (lua_isinteger(L, idx)) {
        return static_cast<uintptr_t>(lua_tointeger(L, idx));
    }
    if (lua_isnumber(L, idx)) {
        return static_cast<uintptr_t>(lua_tonumber(L, idx));
    }
    if (lua_isstring(L, idx)) {
        return reinterpret_cast<uintptr_t>(lua_tostring(L, idx));
    }
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "managed_addr");
        if (lua_isinteger(L, -1)) {
            uintptr_t a = static_cast<uintptr_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            return a;
        }
        lua_pop(L, 1);
        lua_getfield(L, idx, "addr");
        if (lua_isinteger(L, -1)) {
            uintptr_t a = static_cast<uintptr_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            return a;
        }
        lua_pop(L, 1);
        lua_getfield(L, idx, "ptr");
        if (lua_islightuserdata(L, -1)) {
            uintptr_t a = reinterpret_cast<uintptr_t>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            return a;
        }
        lua_pop(L, 1);
    }
    return 0;
}

int LuaMemory::Call(lua_State* L) {
    uintptr_t fnAddr = GetAddr(L, 1);
    if (!fnAddr || fnAddr < 0x10000) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid function address");
        return 2;
    }
    uintptr_t a1 = GetNativeArgVal(L, 2);
    uintptr_t a2 = GetNativeArgVal(L, 3);
    uintptr_t a3 = GetNativeArgVal(L, 4);
    uintptr_t a4 = GetNativeArgVal(L, 5);
    uintptr_t a5 = GetNativeArgVal(L, 6);
    uintptr_t a6 = GetNativeArgVal(L, 7);
    uintptr_t a7 = GetNativeArgVal(L, 8);
    uintptr_t a8 = GetNativeArgVal(L, 9);

    __try {
        auto pfn = reinterpret_cast<t_NativeFn8>(fnAddr);
        uintptr_t res = pfn(a1, a2, a3, a4, a5, a6, a7, a8);
        lua_pushinteger(L, static_cast<lua_Integer>(res));
        lua_pushboolean(L, true);
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        lua_pushstring(L, "Access Violation in native call");
        return 2;
    }
}

int LuaMemory::CallFloat(lua_State* L) {
    uintptr_t fnAddr = GetAddr(L, 1);
    if (!fnAddr || fnAddr < 0x10000) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid function address");
        return 2;
    }
    uintptr_t a1 = GetNativeArgVal(L, 2);
    uintptr_t a2 = GetNativeArgVal(L, 3);
    uintptr_t a3 = GetNativeArgVal(L, 4);
    uintptr_t a4 = GetNativeArgVal(L, 5);
    uintptr_t a5 = GetNativeArgVal(L, 6);
    uintptr_t a6 = GetNativeArgVal(L, 7);
    uintptr_t a7 = GetNativeArgVal(L, 8);
    uintptr_t a8 = GetNativeArgVal(L, 9);

    __try {
        auto pfn = reinterpret_cast<t_NativeFn8Float>(fnAddr);
        float res = pfn(a1, a2, a3, a4, a5, a6, a7, a8);
        lua_pushnumber(L, static_cast<lua_Number>(res));
        lua_pushboolean(L, true);
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        lua_pushstring(L, "Access Violation in native call");
        return 2;
    }
}

int LuaMemory::CallMain(lua_State* L) {
    return Call(L);
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

struct CompRawInfo {
    uint32_t cid = 0;
    uintptr_t natComp = 0;
    uintptr_t managed = 0;
    uintptr_t klass = 0;
    char name[128] = { 0 };
    char ns[128] = { 0 };
};

static bool SafeExtractComponentInfoSEH(uintptr_t entryAddr, CompRawInfo& out) {
    __try {
        if (!entryAddr || entryAddr < 0x10000) return false;
        out.cid = *reinterpret_cast<uint32_t*>(entryAddr);
        out.natComp = *reinterpret_cast<uintptr_t*>(entryAddr + 8);
        if (out.natComp < 0x10000) return true;

        uintptr_t gcSlot = *reinterpret_cast<uintptr_t*>(out.natComp + 0x18);
        if (gcSlot < 0x10000) return true;

        out.managed = *reinterpret_cast<uintptr_t*>(gcSlot & ~7ull);
        if (out.managed < 0x10000 || (out.managed % 8 != 0)) return true;

        out.klass = *reinterpret_cast<uintptr_t*>(out.managed);
        if (out.klass < 0x10000) return true;

        uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(out.klass + 0x10);
        uintptr_t nsPtr = *reinterpret_cast<uintptr_t*>(out.klass + 0x18);
        if (namePtr >= 0x10000) {
            strncpy_s(out.name, reinterpret_cast<const char*>(namePtr), sizeof(out.name) - 1);
        }
        if (nsPtr >= 0x10000) {
            strncpy_s(out.ns, reinterpret_cast<const char*>(nsPtr), sizeof(out.ns) - 1);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int LuaMemory::UnityGetComponents(lua_State* L) {
    uintptr_t go = GetAddr(L, 1);
    lua_newtable(L);
    if (!go || go < 0x10000) return 1;

    uintptr_t compArr = 0;
    uint32_t compCount = 0;
    if (!SafeRead(reinterpret_cast<void*>(go + 0x20), &compArr, sizeof(compArr)) ||
        !SafeRead(reinterpret_cast<void*>(go + 0x30), &compCount, sizeof(compCount))) {
        return 1;
    }

    if (compCount == 0 || compCount > 256 || !compArr || compArr < 0x10000) return 1;

    int outIdx = 1;
    for (uint32_t ci = 0; ci < compCount; ++ci) {
        uintptr_t entryAddr = compArr + static_cast<size_t>(ci) * 16;
        CompRawInfo info{};
        if (!SafeExtractComponentInfoSEH(entryAddr, info)) continue;

        std::string compName = info.name;
        std::string compNs = info.ns;

        if (compName.empty()) {
            if (info.cid == 121) compName = "Transform";
            else if (info.cid == 66) compName = "MonoBehaviour";
            else compName = "Component_" + std::to_string(info.cid);
            if (compNs.empty() && info.cid == 121) compNs = "UnityEngine";
        }

        lua_newtable(L);
        lua_pushinteger(L, info.cid);
        lua_setfield(L, -2, "cid");
        lua_pushstring(L, compName.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, compNs.c_str());
        lua_setfield(L, -2, "namespace");
        if (info.managed >= 0x10000) {
            lua_pushlightuserdata(L, reinterpret_cast<void*>(info.managed));
            lua_setfield(L, -2, "ptr");
            lua_pushinteger(L, static_cast<lua_Integer>(info.managed));
            lua_setfield(L, -2, "managed_addr");
        }
        if (info.klass >= 0x10000) {
            lua_pushinteger(L, static_cast<lua_Integer>(info.klass));
            lua_setfield(L, -2, "class_addr");
        }
        if (info.natComp >= 0x10000) {
            lua_pushlightuserdata(L, reinterpret_cast<void*>(info.natComp));
            lua_setfield(L, -2, "native_ptr");
        }

        lua_rawseti(L, -2, outIdx++);
    }

    return 1;
}

static const luaL_Reg mem_funcs[] = {
    { "read_u8",             LuaMemory::ReadU8 },
    { "read_u16",            LuaMemory::ReadU16 },
    { "read_u32",            LuaMemory::ReadU32 },
    { "read_i32",            LuaMemory::ReadI32 },
    { "read_u64",            LuaMemory::ReadU64 },
    { "read_ptr",            LuaMemory::ReadPtr },
    { "read_float",          LuaMemory::ReadFloat },
    { "read_bytes",          LuaMemory::ReadBytes },
    { "read_string",         LuaMemory::ReadString },
    { "write_u8",            LuaMemory::WriteU8 },
    { "write_u16",           LuaMemory::WriteU16 },
    { "write_u32",           LuaMemory::WriteU32 },
    { "write_i32",           LuaMemory::WriteI32 },
    { "write_u64",           LuaMemory::WriteU64 },
    { "write_ptr",           LuaMemory::WritePtr },
    { "write_float",         LuaMemory::WriteFloat },
    { "write_bytes",         LuaMemory::WriteBytes },
    { "patch_bytes",         LuaMemory::PatchBytes },
    { "restore_patch",       LuaMemory::RestorePatch },
    { "restore_all_patches", LuaMemory::RestoreAllPatches },
    { "scan",                LuaMemory::Scan },
    { "resolve_rip",         LuaMemory::ResolveRip },
    { "protect",             LuaMemory::Protect },
    { "get_module",          LuaMemory::GetModule },
    { "alloc_near",          LuaMemory::AllocNear },
    { "free_alloc",          LuaMemory::FreeAlloc },
    { "call",                LuaMemory::Call },
    { "call_float",          LuaMemory::CallFloat },
    { "call_main",           LuaMemory::CallMain },
    { nullptr, nullptr }
};

static const luaL_Reg unity_funcs[] = {
    { "is_main_thread",         LuaMemory::UnityIsMainThread },
    { "find",                   LuaMemory::UnityFind },
    { "find_player_car_model",  LuaMemory::UnityFindPlayerCarModel },
    { "get_all_objects",        LuaMemory::UnityGetAllObjects },
    { "get_components",         LuaMemory::UnityGetComponents },
    { "get_name",               LuaMemory::UnityGetName },
    { "get_transform",          LuaMemory::UnityGetTransform },
    { "clone",                  LuaMemory::UnityClone },
    { "destroy",                LuaMemory::UnityDestroy },
    { "set_active",             LuaMemory::UnitySetActive },
    { "set_layer",              LuaMemory::UnitySetLayer },
    { "set_position",           LuaMemory::UnitySetPosition },
    { "set_rotation",           LuaMemory::UnitySetRotation },
    { "get_position",           LuaMemory::UnityGetPosition },
    { "get_rotation",           LuaMemory::UnityGetRotation },
    { nullptr, nullptr }
};

void LuaMemory::Register(lua_State* L) {
    luaL_newlib(L, mem_funcs);
    lua_setglobal(L, "memory");

    luaL_newlib(L, unity_funcs);
    lua_setglobal(L, "unity");
}
