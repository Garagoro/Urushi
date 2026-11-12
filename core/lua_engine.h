#pragma once
#include <string>
#include <mutex>
#include <vector>
#include <atomic>
#include <windows.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaEngine {
public:
    static bool Init(const std::wstring& scriptsDir);
    static void Shutdown();

    // Hot-reload & Dev Eval
    static void Reload();
    static void RequestReload(); // safe from inside Lua / any thread: reload happens on the next render frame
    static void OnScriptModified(const std::string& filename);
    static bool Eval(const std::string& code, std::string& outOutput, std::string& outError);

    // Callbacks from Game Loop & Render
    static void OnMainUpdate(float dt);
    static void OnUpdate(float dt);
    static void OnFixedUpdate(float dt);
    static void OnRender();
    static void OnKeyEvent(int vkey, bool isDown);

    static std::wstring GetScriptsDirectory();
    static lua_State* GetState() { return s_L; }
    // Lock that must be held by any code that touches the Lua state outside LuaEngine itself
    static std::recursive_mutex& Mutex() { return s_mutex; }

private:
    static std::atomic<ULONGLONG> s_reloadAt;   // 0 = no reload pending, else tick when to reload
    static float s_fixedAccum;
    static void RegisterEventsModule(lua_State* L);
    static void RegisterLogModule(lua_State* L);
    static void CallLuaCallbacks(std::vector<int>& callbackRefs, int argCount, const char* label);

    static int Lua_OnMainUpdate(lua_State* L);
    static int Lua_OnUpdate(lua_State* L);
    static int Lua_OnFixedUpdate(lua_State* L);
    static int Lua_OnRender(lua_State* L);
    static int Lua_OnKey(lua_State* L);

    static int Lua_LogInfo(lua_State* L);
    static int Lua_LogWarn(lua_State* L);
    static int Lua_LogError(lua_State* L);

    static std::recursive_mutex s_mutex;
    static lua_State* s_L;
    static std::wstring s_scriptsDir;
    static bool s_initialized;

    static std::vector<int> s_mainUpdateCallbacks;
    static std::vector<int> s_updateCallbacks;
    static std::vector<int> s_fixedUpdateCallbacks;
    static std::vector<int> s_renderCallbacks;
    static std::vector<int> s_keyCallbacks;
};
