#pragma once
#include <windows.h>
#include <atomic>
#include <vector>
#include <string>
#include <MinHook.h>

class HookManager {
public:
    struct ScopedHookGuard {
        ScopedHookGuard() noexcept { s_inFlight.fetch_add(1, std::memory_order_acq_rel); }
        ~ScopedHookGuard() noexcept { s_inFlight.fetch_sub(1, std::memory_order_acq_rel); }
        ScopedHookGuard(const ScopedHookGuard&) = delete;
        ScopedHookGuard& operator=(const ScopedHookGuard&) = delete;
    };

    static bool Init();
    static void BeginUnload();
    static bool IsUnloading();
    static bool WaitForInFlight(DWORD timeoutMs);
    static void Shutdown();

    static bool CreateHook(void* target, void* detour, void** original, const std::string& name = "");
    static bool RemoveHook(void* target);
    static bool EnableAll();
    static bool DisableAll();

private:
    struct HookEntry {
        void* target;
        std::string name;
    };
    static std::vector<HookEntry> s_hooks;
    static std::atomic<int> s_inFlight;
    static std::atomic<bool> s_unloading;
    static bool s_initialized;
};
