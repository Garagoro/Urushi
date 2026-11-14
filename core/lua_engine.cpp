#include "lua_engine.h"
#include "logger.h"
#include "ui_schema.h"
#include "d3d11_hook.h"
#include "urushi_ui.h"
#include "bindings/lua_carx.h"
#include "bindings/lua_carfx.h"
#include "bindings/lua_imgui.h"
#include "bindings/lua_replay.h"
#include "bindings/lua_memory.h"
#include "bindings/lua_updater.h"
#include "bindings/lua_optimizer.h"
#include "bindings/lua_urushi.h"
#include "bindings/lua_cloud.h"
#include "bindings/lua_disasm.h"
#include "bindings/lua_pattern.h"
#include "pattern_scanner.h"
#include "lua_sandbox.h"
#include "lua_hardening.h"
#include "ptr_registry.h"
#include "safe_mode.h"
#include <chrono>
#include <algorithm>
#include <unordered_map>

std::recursive_mutex LuaEngine::s_mutex;
lua_State* LuaEngine::s_L = nullptr;
std::wstring LuaEngine::s_scriptsDir;
bool LuaEngine::s_initialized = false;
std::atomic<ULONGLONG> LuaEngine::s_reloadAt{ 0 };
float LuaEngine::s_fixedAccum = 0.0f;

std::vector<int> LuaEngine::s_mainUpdateCallbacks;
std::vector<int> LuaEngine::s_updateCallbacks;
std::vector<int> LuaEngine::s_fixedUpdateCallbacks;
std::vector<int> LuaEngine::s_renderCallbacks;
std::vector<int> LuaEngine::s_keyCallbacks;

static constexpr int kMaxConsecutiveFailures = 25;
static std::unordered_map<int, int> s_cbFailCount;
static std::unordered_map<int, ULONGLONG> s_lastErrLog;

static auto g_lastUpdateTime = std::chrono::steady_clock::now();

bool LuaEngine::Init(const std::wstring& scriptsDir) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_initialized) return true;

    s_scriptsDir = scriptsDir;
    CreateDirectoryW(scriptsDir.c_str(), nullptr);

    s_L = luaL_newstate();
    if (!s_L) {
        Logger::Error("Failed to create Lua state!");
        return false;
    }

    LuaHardening::OpenSafeLibs(s_L);   // no os.execute / io.popen / package / debug / bytecode, restricted io.open
    LuaSandbox::Install(s_L);

    // Register our custom modules
    LuaCarX::Register(s_L);
    LuaCarFX::Register(s_L);
    LuaImGui::Register(s_L);
    LuaReplay::Register(s_L);
    LuaMemory::Register(s_L);
    LuaUpdater::Register(s_L);
    LuaOptimizer::Register(s_L);
    LuaUrushi::Register(s_L);
    LuaCloud::Register(s_L);
    LuaDisasm::Register(s_L);
    LuaPattern::Register(s_L);
    RegisterEventsModule(s_L);
    RegisterLogModule(s_L);

    UiSchema::Init();
    UrushiUI::BeginLuaReload();

    s_initialized = true;
    Logger::Info("Lua engine initialized.");

    // Load main.lua
    std::wstring mainPath = s_scriptsDir + L"\\main.lua";
    std::string mainNarrow = Logger::ToNarrow(mainPath);

    int mainStatus = luaL_loadfilex(s_L, mainNarrow.c_str(), "t");
    if (mainStatus == LUA_OK) {
        mainStatus = LuaSandbox::PCall(s_L, 0, LUA_MULTRET, "main.lua", LuaSandbox::kBudgetLoadMs);
    }
    if (mainStatus != LUA_OK) {
        const char* err = lua_tostring(s_L, -1);
        Logger::Error("Error loading main.lua: " + std::string(err ? err : "unknown"));
        lua_pop(s_L, 1);
    } else {
        // If main.lua or required module returned a declarative panel schema table, auto-register it
        if (lua_gettop(s_L) > 0) {
            if (lua_istable(s_L, -1)) {
                UiSchema::RegisterPanelFromLua(s_L, -1);
            }
            lua_pop(s_L, lua_gettop(s_L));
        }
        Logger::Info("Loaded scripts/main.lua successfully.");
    }
    UrushiUI::EndLuaReload();

    return true;
}

void LuaEngine::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized) return;

    UiSchema::Shutdown();
    UrushiUI::ClearLuaRefs(s_L);

    // Rollback any live memory code patches to keep game clean
    LuaMemory::RestoreAllPatchesNative();
    PatternScanner::ClearCache();

    if (s_L) {
        luaL_dostring(s_L, "if config and config.save then config.save() end");
        lua_close(s_L);
        s_L = nullptr;
    }

    s_mainUpdateCallbacks.clear();
    s_updateCallbacks.clear();
    s_fixedUpdateCallbacks.clear();
    s_renderCallbacks.clear();
    s_keyCallbacks.clear();
    s_cbFailCount.clear();
    s_lastErrLog.clear();
    PtrRegistry::Clear();

    s_initialized = false;
    Logger::Info("Lua engine shut down.");
}

void LuaEngine::Reload() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    std::wstring dir = s_scriptsDir;
    Logger::Info("Reloading all Lua scripts...");
    Shutdown();
    Init(dir);
}

void LuaEngine::RequestReload() {
    s_reloadAt.store(GetTickCount64() + 1, std::memory_order_release);
}

void LuaEngine::OnScriptModified(const std::string& filename) {
    // Called from the FileWatcher thread. Never touch the Lua state here: closing it while
    // the render thread is inside a Lua call is a crash. Defer to the render thread and
    // debounce so that multi-file saves / half-written files cause a single clean reload.
    Logger::Info("Reload scheduled by FileWatcher: " + filename);
    s_reloadAt.store(GetTickCount64() + 300, std::memory_order_release);
}

void LuaEngine::CallLuaCallbacks(std::vector<int>& callbackRefsIn, int argCount, const char* label) {
    if (!s_L) return;

    // Copy: a callback may register another callback (push_back would invalidate iteration)
    const std::vector<int> callbackRefs = callbackRefsIn;

    for (int ref : callbackRefs) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, ref);
        if (lua_isfunction(s_L, -1)) {
            // Push arguments copy if needed
            for (int i = 0; i < argCount; ++i) {
                lua_pushvalue(s_L, -1 - argCount);
            }
            if (LuaSandbox::PCall(s_L, argCount, 0, label) != LUA_OK) {
                const char* err = lua_tostring(s_L, -1);
                // A failing per-frame callback would otherwise flood the log (and disk) at FPS rate
                const ULONGLONG now = GetTickCount64();
                ULONGLONG& last = s_lastErrLog[ref];
                if (now - last >= 2000) {
                    last = now;
                    Logger::Error(std::string("Lua ") + label + " callback error: " + std::string(err ? err : "unknown"));
                }
                lua_pop(s_L, 1);

                // A callback that fails over and over is switched off instead of burning CPU/log every frame
                if (++s_cbFailCount[ref] >= kMaxConsecutiveFailures) {
                    Logger::Error(std::string("Lua ") + label + " callback disabled after " +
                                  std::to_string(kMaxConsecutiveFailures) + " consecutive failures.");
                    auto it = std::find(callbackRefsIn.begin(), callbackRefsIn.end(), ref);
                    if (it != callbackRefsIn.end()) callbackRefsIn.erase(it);
                    luaL_unref(s_L, LUA_REGISTRYINDEX, ref);
                    s_cbFailCount.erase(ref);
                }
            } else {
                s_cbFailCount.erase(ref);
            }
        } else {
            lua_pop(s_L, 1);
        }
    }
}

void LuaEngine::OnMainUpdate(float dt) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_mainUpdateCallbacks.empty()) return;

    lua_pushnumber(s_L, dt);
    CallLuaCallbacks(s_mainUpdateCallbacks, 1, "on_main_update");
    lua_pop(s_L, 1);
}

void LuaEngine::OnUpdate(float dt) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_updateCallbacks.empty()) return;

    lua_pushnumber(s_L, dt);
    CallLuaCallbacks(s_updateCallbacks, 1, "on_update");
    lua_pop(s_L, 1);
}

void LuaEngine::OnFixedUpdate(float dt) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_fixedUpdateCallbacks.empty()) return;

    lua_pushnumber(s_L, dt);
    CallLuaCallbacks(s_fixedUpdateCallbacks, 1, "on_fixed_update");
    lua_pop(s_L, 1);
}

void LuaEngine::OnRender() {
    // Apply a pending hot-reload here: render thread, outside any Lua call
    const ULONGLONG reloadAt = s_reloadAt.load(std::memory_order_acquire);
    if (reloadAt != 0 && GetTickCount64() >= reloadAt) {
        s_reloadAt.store(0, std::memory_order_release);
        Reload();
    }

    // Process update tick before render
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_lastUpdateTime).count();
    g_lastUpdateTime = now;
    if (dt > 0.25f) dt = 0.25f; // hitch / alt-tab guard

    OnUpdate(dt);

    // Fixed step at 60 Hz independent of render FPS
    constexpr float kFixedStep = 1.0f / 60.0f;
    s_fixedAccum += dt;
    int steps = 0;
    while (s_fixedAccum >= kFixedStep && steps < 4) {
        OnFixedUpdate(kFixedStep);
        s_fixedAccum -= kFixedStep;
        ++steps;
    }
    if (steps == 4) s_fixedAccum = 0.0f;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L) return;

    if (!s_renderCallbacks.empty()) {
        CallLuaCallbacks(s_renderCallbacks, 0, "on_render");
    }
    UrushiUI::Render(D3D11Hook::IsMenuOpen(), s_L);
}

void LuaEngine::OnKeyEvent(int vkey, bool isDown) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_keyCallbacks.empty()) return;

    lua_pushinteger(s_L, vkey);
    lua_pushboolean(s_L, isDown);
    CallLuaCallbacks(s_keyCallbacks, 2, "on_key");
    lua_pop(s_L, 2);
}

int LuaEngine::Lua_OnMainUpdate(lua_State* L) {
    if (lua_isfunction(L, 1)) {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_mainUpdateCallbacks.push_back(ref);
    }
    return 0;
}

int LuaEngine::Lua_OnUpdate(lua_State* L) {
    if (lua_isfunction(L, 1)) {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_updateCallbacks.push_back(ref);
    }
    return 0;
}

int LuaEngine::Lua_OnFixedUpdate(lua_State* L) {
    if (lua_isfunction(L, 1)) {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_fixedUpdateCallbacks.push_back(ref);
    }
    return 0;
}

int LuaEngine::Lua_OnRender(lua_State* L) {
    if (lua_isfunction(L, 1)) {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_renderCallbacks.push_back(ref);
    }
    return 0;
}

int LuaEngine::Lua_OnKey(lua_State* L) {
    if (lua_isfunction(L, 1)) {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_keyCallbacks.push_back(ref);
    }
    return 0;
}

void LuaEngine::RegisterEventsModule(lua_State* L) {
    static const luaL_Reg event_funcs[] = {
        { "on_main_update", Lua_OnMainUpdate },
        { "on_update", Lua_OnUpdate },
        { "on_fixed_update", Lua_OnFixedUpdate },
        { "on_render", Lua_OnRender },
        { "on_draw", Lua_OnRender },
        { "on_key", Lua_OnKey },
        { nullptr, nullptr }
    };
    luaL_newlib(L, event_funcs);
    lua_setglobal(L, "events");
}

int LuaEngine::Lua_LogInfo(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    Logger::Info(str);
    return 0;
}

int LuaEngine::Lua_LogWarn(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    Logger::Warn(str);
    return 0;
}

int LuaEngine::Lua_LogError(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    Logger::Error(str);
    return 0;
}

void LuaEngine::RegisterLogModule(lua_State* L) {
    static const luaL_Reg log_funcs[] = {
        { "info", Lua_LogInfo },
        { "warn", Lua_LogWarn },
        { "error", Lua_LogError },
        { nullptr, nullptr }
    };
    luaL_newlib(L, log_funcs);
    lua_setglobal(L, "log");
}

std::wstring LuaEngine::GetScriptsDirectory() {
    return s_scriptsDir;
}

static void DumpLuaValue(lua_State* L, int idx, std::string& out, int depth = 0) {
    if (depth > 3) {
        out += "{...}";
        return;
    }
    int t = lua_type(L, idx);
    switch (t) {
    case LUA_TNIL:
        out += "nil";
        break;
    case LUA_TBOOLEAN:
        out += (lua_toboolean(L, idx) ? "true" : "false");
        break;
    case LUA_TNUMBER: {
        if (lua_isinteger(L, idx)) {
            out += std::to_string(lua_tointeger(L, idx));
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.4g", lua_tonumber(L, idx));
            out += buf;
        }
        break;
    }
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        if (depth == 0) {
            out.append(s, len);
        } else {
            out += "\"";
            out.append(s, len);
            out += "\"";
        }
        break;
    }
    case LUA_TLIGHTUSERDATA: {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)reinterpret_cast<uintptr_t>(lua_touserdata(L, idx)));
        out += buf;
        break;
    }
    case LUA_TTABLE: {
        if (luaL_getmetafield(L, idx, "__tostring")) {
            lua_pushvalue(L, idx < 0 ? idx - 1 : idx);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                size_t len = 0;
                const char* s = lua_tolstring(L, -1, &len);
                if (s) out.append(s, len);
                lua_pop(L, 1);
                break;
            }
            lua_pop(L, 1);
        }
        out += "{ ";
        lua_pushnil(L);
        int normIdx = (idx > 0) ? idx : (lua_gettop(L) + idx);
        bool first = true;
        int count = 0;
        while (lua_next(L, normIdx) != 0) {
            if (!first) out += ", ";
            first = false;
            if (++count > 40) {
                out += "...";
                lua_pop(L, 2);
                break;
            }
            if (lua_type(L, -2) == LUA_TSTRING) {
                out += lua_tostring(L, -2);
                out += " = ";
            } else if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                out += "[";
                out += std::to_string(lua_tointeger(L, -2));
                out += "] = ";
            }
            DumpLuaValue(L, -1, out, depth + 1);
            lua_pop(L, 1);
        }
        out += " }";
        break;
    }
    default: {
        size_t len = 0;
        const char* s = luaL_tolstring(L, idx, &len);
        if (s) out.append(s, len);
        lua_pop(L, 1);
        break;
    }
    }
}

bool LuaEngine::Eval(const std::string& code, std::string& outOutput, std::string& outError) {
    outOutput.clear();
    outError.clear();
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L) {
        outError = "Lua engine not initialized";
        return false;
    }

    struct PrintCtx {
        std::string* buf;
    } ctx{ &outOutput };

    auto capturePrint = [](lua_State* L) -> int {
        auto* pc = static_cast<PrintCtx*>(lua_touserdata(L, lua_upvalueindex(1)));
        int n = lua_gettop(L);
        for (int i = 1; i <= n; ++i) {
            size_t len = 0;
            const char* s = luaL_tolstring(L, i, &len);
            if (i > 1 && pc && pc->buf) pc->buf->push_back('\t');
            if (s && pc && pc->buf) pc->buf->append(s, len);
            lua_pop(L, 1);
        }
        if (pc && pc->buf) pc->buf->push_back('\n');
        return 0;
    };

    lua_getglobal(s_L, "print"); // save old print at stack -1
    lua_pushlightuserdata(s_L, &ctx);
    lua_pushcclosure(s_L, capturePrint, 1);
    lua_setglobal(s_L, "print");

    int baseTop = lua_gettop(s_L);
    std::string expr = "return " + code;
    int loadRes = luaL_loadbuffer(s_L, expr.data(), expr.size(), "=eval");
    if (loadRes != LUA_OK) {
        lua_pop(s_L, 1);
        loadRes = luaL_loadbuffer(s_L, code.data(), code.size(), "=eval");
    }

    bool ok = false;
    if (loadRes != LUA_OK) {
        const char* err = lua_tostring(s_L, -1);
        outError = err ? err : "syntax error";
        lua_pop(s_L, 1);
    } else if (LuaSandbox::PCall(s_L, 0, LUA_MULTRET, "eval", LuaSandbox::kBudgetEvalMs) != LUA_OK) {
        const char* err = lua_tostring(s_L, -1);
        outError = err ? err : "runtime error";
        lua_pop(s_L, 1);
    } else {
        ok = true;
        int nres = lua_gettop(s_L) - baseTop;
        for (int i = 1; i <= nres; ++i) {
            if (!outOutput.empty() && outOutput.back() != '\n') outOutput.push_back('\t');
            DumpLuaValue(s_L, baseTop + i, outOutput, 0);
        }
        lua_pop(s_L, nres);
    }

    // Restore original print
    lua_setglobal(s_L, "print");
    return ok;
}
