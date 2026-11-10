#include "hook_manager.h"
#include "logger.h"

std::vector<HookManager::HookEntry> HookManager::s_hooks;
std::atomic<int> HookManager::s_inFlight{ 0 };
std::atomic<bool> HookManager::s_unloading{ false };
bool HookManager::s_initialized = false;

bool HookManager::Init() {
    if (s_initialized) return true;

    s_unloading.store(false, std::memory_order_release);
    if (MH_Initialize() != MH_OK) {
        Logger::Error("Failed to initialize MinHook!");
        return false;
    }
    s_initialized = true;
    Logger::Info("MinHook initialized successfully.");
    return true;
}

void HookManager::BeginUnload() {
    s_unloading.store(true, std::memory_order_release);
    if (s_initialized) {
        DisableAll();
    }
}

bool HookManager::IsUnloading() {
    return s_unloading.load(std::memory_order_acquire);
}

bool HookManager::WaitForInFlight(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (s_inFlight.load(std::memory_order_acquire) > 0) {
        if (GetTickCount64() >= deadline) {
            Logger::Warn("HookManager::WaitForInFlight timed out with active hooks=" +
                         std::to_string(s_inFlight.load(std::memory_order_relaxed)));
            return false;
        }
        Sleep(1);
    }
    return true;
}

void HookManager::Shutdown() {
    if (!s_initialized) return;

    s_unloading.store(true, std::memory_order_release);
    DisableAll();
    MH_Uninitialize();
    s_hooks.clear();
    s_initialized = false;
    Logger::Info("MinHook shut down.");
}

bool HookManager::CreateHook(void* target, void* detour, void** original, const std::string& name) {
    if (!s_initialized && !Init()) return false;

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        Logger::Error("MH_CreateHook failed for " + name + " (status: " + std::to_string(status) + ")");
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        Logger::Error("MH_EnableHook failed for " + name + " (status: " + std::to_string(status) + ")");
        return false;
    }

    s_hooks.push_back({ target, name });
    Logger::Info("Hook installed: " + name);
    return true;
}

bool HookManager::RemoveHook(void* target) {
    if (!s_initialized) return false;

    MH_DisableHook(target);
    MH_RemoveHook(target);

    for (auto it = s_hooks.begin(); it != s_hooks.end(); ++it) {
        if (it->target == target) {
            Logger::Info("Hook removed: " + it->name);
            s_hooks.erase(it);
            break;
        }
    }
    return true;
}

bool HookManager::EnableAll() {
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

bool HookManager::DisableAll() {
    return MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
}
