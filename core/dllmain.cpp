#include <windows.h>
#include <shlwapi.h>
#include "logger.h"
#include "hook_manager.h"
#include "native_carx.h"
#include "unity_bridge.h"
#include "car_sim_optimizer.h"
#include "d3d11_hook.h"
#include "carfx.h"
#include "replay_engine.h"
#include "file_watcher.h"
#include "lua_engine.h"
#include "dev_pipe.h"
#include "updater.h"
#include "settings.h"
#include "crash_handler.h"
#include "safe_mode.h"
#include "cloud_scripts.h"

#pragma comment(lib, "shlwapi.lib")

#include <atomic>

static HMODULE g_module = nullptr;
static std::wstring g_exeDir;
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
    const bool featuresUp = SafeMode::FeaturesStarted();
    const bool needCarFxDrain = featuresUp && (CarFX::ShouldBypassDepthPrePass() || CarFX::ShouldHideCarInternals());
    const bool needOptDrain = featuresUp && CarSimOptimizer::GetConfig().enabled;
    if (needCarFxDrain) {
        CarFX::SetEnabled(false);
    }
    if (needOptDrain) {
        CarSimOptimizer::ResetDefaults();
    }
    if (needCarFxDrain || needOptDrain) {
        // Give Unity MainThread frames to restore materials, car effects, and kinematic states before detaching hooks
        Sleep(80);
    }
    D3D11Hook::DetachWndProc();
    HookManager::BeginUnload();
    HookManager::WaitForInFlight(2500);
    Sleep(50);

    if (featuresUp) {
        FileWatcher::Stop();
        DevPipe::Shutdown();
        Updater::Shutdown();
        CloudScripts::Shutdown();
        LuaEngine::Shutdown();
        CarSimOptimizer::Shutdown();
    }
    D3D11Hook::Shutdown();
    if (featuresUp) {
        UnityBridge::Shutdown();
        NativeCarX::Shutdown();
    }
    HookManager::Shutdown();
    CrashHandler::Shutdown();
    Logger::Shutdown();
}

// "Phase B": everything that touches the game or runs scripts. Skipped entirely in Safe mode and
// started later from the Safe mode window (worker thread) if the user leaves Safe mode.
static void StartFeatures() {
    try {
        const std::wstring replaysDir = g_exeDir + L"\\replays";
        const std::wstring scriptsDir = g_exeDir + L"\\scripts";

        if (!NativeCarX::Init()) {
            Logger::Warn("NativeCarX failed initial hook, will continue...");
        }

        if (!UnityBridge::Init()) {
            Logger::Warn("UnityBridge failed initial hook, will retry...");
        }

        if (!CarSimOptimizer::Init()) {
            Logger::Warn("CarSimOptimizer failed initial hook, will retry...");
        }

        CloudScripts::Init(g_exeDir);   // before Lua: main.lua loads installed cloud scripts
        ReplayEngine::Init(replaysDir);
        LuaEngine::Init(scriptsDir);

        // Start live hot-reload file watcher
        FileWatcher::Start(scriptsDir, [](const std::string& filename) {
            LuaEngine::OnScriptModified(filename);
        });

        // Dev pipe (screenshots, Lua eval, reload) is a remote-control channel for local tools: OFF unless
        // urushi_settings.ini has [dev] pipe=1 (needed by tools/urushi_dev_mcp.py)
        if (Settings::GetBool("dev", "pipe", false)) {
            DevPipe::Init(g_exeDir);
        } else {
            Logger::Info("DevPipe disabled ([dev] pipe=0).");
        }

        // Start async auto-update check against GitHub repository (Garagoro/Urushi)
        Updater::Init(g_exeDir, g_module);
    }
    catch (const std::exception& ex) {
        Logger::Error(std::string("Exception while starting features: ") + ex.what());
    }
    catch (...) {
        Logger::Error("Unknown exception while starting features!");
    }
}

static DWORD WINAPI InitThread(LPVOID) {
    try {
        g_exeDir = GetExecutableDirectory();
        std::wstring logPath = g_exeDir + L"\\urushi.log";

        Settings::Init(g_exeDir);
        Logger::Init(logPath);
        Logger::Info("URUSHI v1.0.0 initializing...");
        CrashHandler::Init(g_exeDir, g_module);

        // Wait until NativeCarX.dll is loaded
        int retries = 0;
        while (!GetModuleHandleW(L"NativeCarX.dll") && !g_unloaded.load(std::memory_order_acquire) && retries < 200) {
            Sleep(100);
            retries++;
        }
        if (g_unloaded.load(std::memory_order_acquire)) {
            CrashHandler::Shutdown();
            Logger::Shutdown();
            return 0;
        }

        if (!HookManager::Init()) {
            Logger::Error("Failed to initialize HookManager!");
            return 1;
        }

        // The overlay (D3D11 + ImGui) is always installed: it hosts the Safe mode UI and the menu.
        if (!D3D11Hook::Init()) {
            Logger::Error("Failed to hook D3D11!");
            return 1;
        }

        SafeMode::SetStartFeaturesCallback(&StartFeatures);
        if (SafeMode::Decide()) {
            StartFeatures();
            SafeMode::MarkFull();
        } else {
            Logger::Warn("URUSHI is running WITHOUT features (Safe mode / waiting for the user's choice).");
        }

        g_initialized = true;
        Logger::Info("URUSHI fully initialized and ready. Press F4 to toggle menu.");
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
