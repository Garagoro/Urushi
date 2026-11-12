#include "lua_sandbox.h"
#include "crash_handler.h"

#include <atomic>

namespace LuaSandbox {

namespace {

std::atomic<ULONGLONG> g_deadline{ 0 };
std::atomic<DWORD> g_budgetMs{ 0 };
std::atomic<bool> g_tripped{ false };   // a timeout fired: hook runs on EVERY instruction until the outermost call unwinds

void CountHook(lua_State* L, lua_Debug*) {
    const ULONGLONG d = g_deadline.load(std::memory_order_relaxed);
    if (d != 0 && GetTickCount64() > d) {
        // Scripts can swallow the error with pcall (`while true do pcall(spin) end`). Once the budget is blown
        // fire on every instruction so the error escapes the pcall and reaches our outermost PCall.
        if (!g_tripped.exchange(true)) lua_sethook(L, CountHook, LUA_MASKCOUNT, 1);
        luaL_error(L, "script timeout: exceeded %d ms budget (endless loop?)",
                   static_cast<int>(g_budgetMs.load(std::memory_order_relaxed)));
    }
}

int MessageHandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

} // namespace

void Install(lua_State* L) {
    // Fires every 2000 VM instructions: negligible overhead, ~microsecond granularity.
    lua_sethook(L, CountHook, LUA_MASKCOUNT, 2000);
}

int LoadFileText(lua_State* L, const char* path, int envIdx) {
    if (envIdx != 0) envIdx = lua_absindex(L, envIdx);
    const int st = luaL_loadfilex(L, path, "t");
    if (st != LUA_OK) return st;
    if (envIdx != 0 && lua_istable(L, envIdx)) {
        lua_pushvalue(L, envIdx);
        if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);   // first upvalue of a main chunk is _ENV
    }
    return LUA_OK;
}

int PCall(lua_State* L, int nargs, int nresults, const char* label, DWORD budgetMs) {
    // Nested calls keep the outermost (earliest) deadline.
    const ULONGLONG prevDeadline = g_deadline.load(std::memory_order_relaxed);
    const DWORD prevBudget = g_budgetMs.load(std::memory_order_relaxed);
    const ULONGLONG mine = GetTickCount64() + budgetMs;
    if (prevDeadline == 0 || mine < prevDeadline) {
        g_deadline.store(mine, std::memory_order_relaxed);
        g_budgetMs.store(budgetMs, std::memory_order_relaxed);
    }

    const char* prevCtx = CrashHandler::GetLuaContext();
    CrashHandler::SetLuaContext(label);

    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, MessageHandler);
    lua_insert(L, base);
    const int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);

    CrashHandler::SetLuaContext(prevCtx);
    if (g_tripped.load() && (prevDeadline == 0 || GetTickCount64() <= prevDeadline)) {
        g_tripped.store(false);
        lua_sethook(L, CountHook, LUA_MASKCOUNT, 2000); // back to the cheap cadence
    }
    g_deadline.store(prevDeadline, std::memory_order_relaxed);
    g_budgetMs.store(prevBudget, std::memory_order_relaxed);
    return status;
}

} // namespace LuaSandbox
