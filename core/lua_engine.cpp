#include "lua_engine.h"
#include "logger.h"
#include "ui_schema.h"
#include "d3d11_hook.h"
#include "urushi_ui.h"
#include "bindings/lua_carx.h"
#include "bindings/lua_steer.h"
#include "bindings/lua_carfx.h"
#include "bindings/lua_imgui.h"
#include "bindings/lua_replay.h"
#include "bindings/lua_memory.h"
#include "bindings/lua_updater.h"
#include "bindings/lua_optimizer.h"
#include "bindings/lua_urushi.h"
#include <chrono>

std::recursive_mutex LuaEngine::s_mutex;
lua_State* LuaEngine::s_L = nullptr;
std::wstring LuaEngine::s_scriptsDir;
bool LuaEngine::s_initialized = false;

std::vector<int> LuaEngine::s_mainUpdateCallbacks;
std::vector<int> LuaEngine::s_updateCallbacks;
std::vector<int> LuaEngine::s_fixedUpdateCallbacks;
std::vector<int> LuaEngine::s_renderCallbacks;
std::vector<int> LuaEngine::s_keyCallbacks;

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

    luaL_openlibs(s_L);

    // Register our custom modules
    LuaCarX::Register(s_L);
    LuaSteer::Register(s_L);
    LuaCarFX::Register(s_L);
    LuaImGui::Register(s_L);
    LuaReplay::Register(s_L);
    LuaMemory::Register(s_L);
    LuaUpdater::Register(s_L);
    LuaOptimizer::Register(s_L);
    LuaUrushi::Register(s_L);
    RegisterEventsModule(s_L);
    RegisterLogModule(s_L);

    UiSchema::Init();
    UrushiUI::BeginLuaReload();

    s_initialized = true;
    Logger::Info("Lua engine initialized.");

    // Load main.lua
    std::wstring mainPath = s_scriptsDir + L"\\main.lua";
    std::string mainNarrow = Logger::ToNarrow(mainPath);

    if (luaL_dofile(s_L, mainNarrow.c_str()) != LUA_OK) {
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

    if (s_L) {
        lua_close(s_L);
        s_L = nullptr;
    }

    s_mainUpdateCallbacks.clear();
    s_updateCallbacks.clear();
    s_fixedUpdateCallbacks.clear();
    s_renderCallbacks.clear();
    s_keyCallbacks.clear();

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

void LuaEngine::OnScriptModified(const std::string& filename) {
    Logger::Info("Reload triggered by FileWatcher: " + filename);
    Reload();
}

void LuaEngine::CallLuaCallbacks(const std::vector<int>& callbackRefs, int argCount) {
    if (!s_L) return;

    for (int ref : callbackRefs) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, ref);
        if (lua_isfunction(s_L, -1)) {
            // Push arguments copy if needed
            for (int i = 0; i < argCount; ++i) {
                lua_pushvalue(s_L, -1 - argCount);
            }
            if (lua_pcall(s_L, argCount, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(s_L, -1);
                Logger::Error("Lua callback error: " + std::string(err ? err : "unknown"));
                lua_pop(s_L, 1);
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
    CallLuaCallbacks(s_mainUpdateCallbacks, 1);
    lua_pop(s_L, 1);
}

void LuaEngine::OnUpdate(float dt) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_updateCallbacks.empty()) return;

    lua_pushnumber(s_L, dt);
    CallLuaCallbacks(s_updateCallbacks, 1);
    lua_pop(s_L, 1);
}

void LuaEngine::OnFixedUpdate(float dt) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_fixedUpdateCallbacks.empty()) return;

    lua_pushnumber(s_L, dt);
    CallLuaCallbacks(s_fixedUpdateCallbacks, 1);
    lua_pop(s_L, 1);
}

void LuaEngine::OnRender() {
    // Process update tick before render
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_lastUpdateTime).count();
    g_lastUpdateTime = now;

    OnUpdate(dt);
    OnFixedUpdate(0.01666f);

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L) return;

    if (!s_renderCallbacks.empty()) {
        CallLuaCallbacks(s_renderCallbacks, 0);
    }
    UrushiUI::Render(D3D11Hook::IsMenuOpen(), s_L);
}

void LuaEngine::OnKeyEvent(int vkey, bool isDown) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !s_L || s_keyCallbacks.empty()) return;

    lua_pushinteger(s_L, vkey);
    lua_pushboolean(s_L, isDown);
    CallLuaCallbacks(s_keyCallbacks, 2);
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
    } else if (lua_pcall(s_L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char* err = lua_tostring(s_L, -1);
        outError = err ? err : "runtime error";
        lua_pop(s_L, 1);
    } else {
        ok = true;
        int nres = lua_gettop(s_L) - baseTop;
        for (int i = 1; i <= nres; ++i) {
            size_t len = 0;
            const char* s = luaL_tolstring(s_L, baseTop + i, &len);
            if (s && len > 0) {
                if (!outOutput.empty() && outOutput.back() != '\n') outOutput.push_back('\t');
                outOutput.append(s, len);
            }
            lua_pop(s_L, 1);
        }
        lua_pop(s_L, nres);
    }

    // Restore original print
    lua_setglobal(s_L, "print");
    return ok;
}
