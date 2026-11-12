#pragma once
#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// Hardened Lua standard library. Replaces luaL_openlibs().
//
// Threat model: Lua scripts may come from anywhere (not only the reviewed repository). A script must NOT be able to
//  - run programs or shell commands, load native code, or load pre-compiled bytecode,
//  - download anything (there is no network API in Lua at all),
//  - create files that can be executed / loaded as code, or touch files outside the mod's data folder,
//  - tamper with the sandbox itself (debug library, instruction-count hook, registry).
// Native code is out of scope by design: whoever edits the C++ core is a fork, not a script.
//
// What remains in Lua:
//  base (without dofile/loadfile/collectgarbage, `load` is text-only), coroutine, table, string (without dump), math, utf8,
//  os = { time, clock, date, difftime }, debug = { traceback },
//  io = { open, type } where open() only reaches <gameDir>\urushi_data\ (read/write) and <gameDir>\scripts\ (read),
//        only for data extensions (.json .txt .cfg .csv), with path/name validation and a size budget.
namespace LuaHardening {

void OpenSafeLibs(lua_State* L);

// ---- exposed for tests ----
// Returns true if `path` may be opened with `mode` ("r", "w", "a", "r+", ... with optional b). exeDir without trailing slash.
bool IsIoAllowed(const wchar_t* exeDir, const char* pathUtf8, const char* mode, std::wstring* outFull = nullptr);

} // namespace LuaHardening
