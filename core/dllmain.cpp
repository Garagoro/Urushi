#include <windows.h>
#include <shlwapi.h>
#include "logger.h"
#include "hook_manager.h"
#include "native_carx.h"
#include "unity_bridge.h"
#include "d3d11_hook.h"
#include "carfx.h"
#include "replay_engine.h"
#include "file_watcher.h"
#include "lua_engine.h"
#include "updater.h"

#pragma comment(lib, "shlwapi.lib")

#include <atomic>

static HMODULE g_module = nullptr;
static HANDLE g_initThread = nullptr;
static std::atomic<bool> g_initialized{ false };
static std::atomic<bool> g_unloaded{ false };

static std::wstring GetExecutableDirectory() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

extern "C" __declspec(dllexport) void ScriptHook_Unload() {
    if (g_unloaded.exchange(true, std::memory_order_acq_rel)) return;

    if (g_initThread) {
        WaitForSingleObject(g_initThread, 5000);
        CloseHandle(g_initThread);
        g_initThread = nullptr;
    }

    if (!g_initialized.exchange(false, std::memory_order_acq_rel)) {
        Logger::Shutdown();
        return;
    }

    Logger::Info("ScriptHook_Unload invoked. Draining active hooks...");
    if (CarFX::ShouldBypassDepthPrePass() || CarFX::ShouldHideCarInternals()) {
        // Give Unity MainThread frames to restore car_body renderQueue and hidden stealth renderers before detaching hooks
        CarFX::SetEnabled(false);
        Sleep(80);
    }
    D3D11Hook::DetachWndProc();
    HookManager::BeginUnload();
    HookManager::WaitForInFlight(2500);
    Sleep(50);

    FileWatcher::Stop();
    Updater::Shutdown();
    LuaEngine::Shutdown();
    D3D11Hook::Shutdown();
    UnityBridge::Shutdown();
    NativeCarX::Shutdown();
    HookManager::Shutdown();
    Logger::Shutdown();
}

static DWORD WINAPI InitThread(LPVOID) {
    try {
        std::wstring exeDir = GetExecutableDirectory();
        std::wstring logPath = exeDir + L"\\CarX2ScriptHook.log";
        std::wstring replaysDir = exeDir + L"\\replays";
        std::wstring scriptsDir = exeDir + L"\\scripts";

        Logger::Init(logPath);
        Logger::Info("CarX2ScriptHook v1.0.0 initializing...");

        // Wait until NativeCarX.dll is loaded
        int retries = 0;
        while (!GetModuleHandleW(L"NativeCarX.dll") && !g_unloaded.load(std::memory_order_acquire) && retries < 200) {
            Sleep(100);
            retries++;
        }
        if (g_unloaded.load(std::memory_order_acquire)) {
            Logger::Shutdown();
            return 0;
        }

        if (!HookManager::Init()) {
            Logger::Error("Failed to initialize HookManager!");
            return 1;
        }

        if (!NativeCarX::Init()) {
            Logger::Warn("NativeCarX failed initial hook, will continue...");
        }

        if (!UnityBridge::Init()) {
            Logger::Warn("UnityBridge failed initial hook, will retry...");
        }

        if (!D3D11Hook::Init()) {
            Logger::Error("Failed to hook D3D11!");
            return 1;
        }

        ReplayEngine::Init(replaysDir);
        LuaEngine::Init(scriptsDir);

        // Start live hot-reload file watcher
        FileWatcher::Start(scriptsDir, [](const std::string& filename) {
            LuaEngine::OnScriptModified(filename);
        });

        // Start async auto-update check against GitHub repository (Garagoro/Urushi)
        Updater::Init(exeDir, g_module);

        g_initialized = true;
        Logger::Info("CarX2ScriptHook fully initialized and ready. Press F4 to toggle menu.");
    }
    catch (const std::exception& ex) {
        Logger::Error(std::string("Fatal exception in InitThread: ") + ex.what());
    }
    catch (...) {
        Logger::Error("Unknown fatal exception in InitThread!");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        g_module = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        g_initThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        if (lpvReserved == nullptr) { // FreeLibrary called
            ScriptHook_Unload();
        }
        break;
    }
    return TRUE;
}
