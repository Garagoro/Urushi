#pragma once
#include <windows.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// Script sandbox: every call from C++ into Lua goes through LuaSandbox::PCall, which
//  - enforces a wall-clock budget (an endless loop raises "script timeout" instead of freezing the game)
//  - attaches a traceback to errors
//  - records which entry point is executing (written into crash reports)
namespace LuaSandbox {

constexpr DWORD kBudgetCallbackMs = 250;   // per-frame callbacks, UI, key events
constexpr DWORD kBudgetLoadMs     = 60000; // main.lua / script (re)load
constexpr DWORD kBudgetEvalMs     = 5000;  // dev console

// Install the instruction-count hook on a freshly created state.
void Install(lua_State* L);

// Same contract as lua_pcall (function + args already on the stack, no message handler).
// On failure the error message (with traceback) is left on top of the stack.
int PCall(lua_State* L, int nargs, int nresults, const char* label, DWORD budgetMs = kBudgetCallbackMs);

} // namespace LuaSandbox
