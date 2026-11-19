#include "network_safemode.h"
#include "il2cpp_helpers.h"
#include "logger.h"

#include <MinHook.h>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace NetworkSafeMode {

namespace {

using t_VoidThis      = void(__fastcall*)(void* thisPtr, const void* methodInfo);
using t_SetBoolThis   = void(__fastcall*)(void* thisPtr, bool value, const void* methodInfo);
using t_ActivateMs    = void(__fastcall*)(void* thisModel, int32_t durationMs, void* ownerObj, const void* methodInfo);
using t_Deactivate    = void(__fastcall*)(void* thisModel, void* ownerObj, const void* methodInfo);
using t_GetSafeMode   = bool(__fastcall*)(void* thisModel, const void* methodInfo);

t_VoidThis    s_origOnActivate   = nullptr;
t_VoidThis    s_origOnDeactivate = nullptr;
t_SetBoolThis s_origOnSmChanged  = nullptr;
t_SetBoolThis s_origSetIndActive = nullptr;
void*         s_hookOnActivate   = nullptr;
void*         s_hookOnDeactivate = nullptr;
void*         s_hookOnSmChanged  = nullptr;
void*         s_hookSetIndActive = nullptr;

t_ActivateMs  s_fnActivateMs  = nullptr;
t_Deactivate  s_fnDeactivate  = nullptr;
t_GetSafeMode s_fnGetSafeMode = nullptr;

std::mutex   s_mtx;
void*        s_activeController = nullptr;
void*        s_safeModeModel    = nullptr;

std::atomic<bool>     s_initialized{ false };
std::atomic<bool>     s_hooksOk{ false };
std::atomic<bool>     s_inLobby{ false };
std::atomic<bool>     s_safeModeActive{ false };
std::atomic<bool>     s_showGameIndicator{ false };
std::atomic<uint32_t> s_triggersTotal{ 0 };

std::atomic<int>      s_pendingDurationMs{ 0 };
std::atomic<bool>     s_pendingForce{ false };
std::atomic<bool>     s_pendingDeactivate{ false };
ULONGLONG             s_lastTriggerMs = 0;

static bool SafeReadPtr(uintptr_t addr, void** out) {
    if (addr < 0x10000 || !out) return false;
    __try {
        *out = *reinterpret_cast<void* const*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

static bool BytesContain(const uint8_t* p, size_t len, const uint8_t* pat, size_t patLen) {
    if (!p || !pat || patLen == 0 || len < patLen) return false;
    __try {
        for (size_t i = 0; i + patLen <= len; ++i) {
            if (memcmp(p + i, pat, patLen) == 0) return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Finds NetworkCarSafeModeModel::ActivateSafeMode(int32_t durationMs, object owner) among the 3 overloads.
// In x64 IL2CPP codegen, the 2-parameter overload clears r9d (`45 33 C9`) right before tail-jumping to SafeModeActivator.
static void* FindActivateSafeModeMsOverload(uintptr_t klass) {
    if (!klass) return nullptr;
    uintptr_t methodsPtr = 0;
    uint16_t methodCount = 0;
    __try {
        methodsPtr = *reinterpret_cast<const uintptr_t*>(klass + 0x98);
        methodCount = *reinterpret_cast<const uint16_t*>(klass + 0x120);
        if (methodCount == 0 || methodCount > 512) {
            methodCount = *reinterpret_cast<const uint16_t*>(klass + 0x118);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!methodsPtr || methodCount == 0 || methodCount > 512) return nullptr;

    static constexpr uint8_t kXorR9d[3] = { 0x45, 0x33, 0xC9 };
    void* fallback = nullptr;
    for (uint16_t i = 0; i < methodCount; ++i) {
        uintptr_t mi = 0;
        uintptr_t namePtr = 0;
        uintptr_t fnPtr = 0;
        char nameBuf[64] = {};
        __try {
            mi = *reinterpret_cast<const uintptr_t*>(methodsPtr + static_cast<size_t>(i) * sizeof(uintptr_t));
            if (!mi) continue;
            namePtr = *reinterpret_cast<const uintptr_t*>(mi + 0x18);
            fnPtr = *reinterpret_cast<const uintptr_t*>(mi + 0x00);
            if (!namePtr || !fnPtr) continue;
            size_t k = 0;
            const char* s = reinterpret_cast<const char*>(namePtr);
            for (; k + 1 < sizeof(nameBuf) && s[k]; ++k) nameBuf[k] = s[k];
            nameBuf[k] = '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (strcmp(nameBuf, "ActivateSafeMode") != 0) continue;
        if (!fallback) fallback = reinterpret_cast<void*>(fnPtr);
        if (BytesContain(reinterpret_cast<const uint8_t*>(fnPtr), 32, kXorR9d, sizeof(kXorR9d))) {
            return reinterpret_cast<void*>(fnPtr);
        }
    }
    return fallback;
}

static void __fastcall Hook_OnActivate(void* thisPtr, const void* methodInfo) {
    if (s_origOnActivate) {
        s_origOnActivate(thisPtr, methodInfo);
    }
    if (!thisPtr) return;
    void* model = nullptr;
    if (SafeReadPtr(reinterpret_cast<uintptr_t>(thisPtr) + 0x18, &model) && model) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_activeController = thisPtr;
        s_safeModeModel = model;
        s_inLobby.store(true, std::memory_order_release);
        Logger::Info("NetworkSafeMode: attached to active online lobby controller.");
    }
}

static void __fastcall Hook_OnDeactivate(void* thisPtr, const void* methodInfo) {
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (thisPtr == s_activeController) {
            s_activeController = nullptr;
            s_safeModeModel = nullptr;
            s_inLobby.store(false, std::memory_order_release);
            s_safeModeActive.store(false, std::memory_order_release);
            s_pendingDurationMs.store(0, std::memory_order_relaxed);
            s_pendingDeactivate.store(false, std::memory_order_relaxed);
            Logger::Info("NetworkSafeMode: detached (left online lobby).");
        }
    }
    if (s_origOnDeactivate) {
        s_origOnDeactivate(thisPtr, methodInfo);
    }
}

static void __fastcall Hook_OnSafeModeChanged(void* thisPtr, bool isSafeMode, const void* methodInfo) {
    s_safeModeActive.store(isSafeMode, std::memory_order_release);
    if (s_origOnSmChanged) {
        s_origOnSmChanged(thisPtr, isSafeMode, methodInfo);
    }
}

static void __fastcall Hook_SetActiveSafeMode(void* thisPtr, bool active, const void* methodInfo) {
    if (active && !s_showGameIndicator.load(std::memory_order_acquire)) {
        active = false;
    }
    if (s_origSetIndActive) {
        s_origSetIndActive(thisPtr, active, methodInfo);
    }
}

static bool SafeCallGetSafeModeSEH(void* model) {
    if (!model || !s_fnGetSafeMode) return false;
    __try {
        return s_fnGetSafeMode(model, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallActivateMsSEH(void* model, int32_t durationMs, void* owner) {
    if (!model || !owner || !s_fnActivateMs) return false;
    __try {
        s_fnActivateMs(model, durationMs, owner, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallDeactivateSEH(void* model, void* owner) {
    if (!model || !owner || !s_fnDeactivate) return false;
    __try {
        s_fnDeactivate(model, owner, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool Init() {
    if (s_initialized.exchange(true)) return s_hooksOk.load(std::memory_order_acquire);

    const uintptr_t ctrlKlass = Il2CppHelpers::LookupTypeClass("ApplyNetworkSafeModeController", "UI.Controllers.Race");
    const uintptr_t modelKlass = Il2CppHelpers::LookupTypeClass("NetworkCarSafeModeModel", "States.ScreenModels");
    if (!ctrlKlass || !modelKlass) {
        Logger::Warn("NetworkSafeMode: IL2CPP classes not found (ctrl=" + std::to_string(ctrlKlass != 0) +
                     ", model=" + std::to_string(modelKlass != 0) + ")");
        return false;
    }

    void* fnOnAct = Il2CppHelpers::LookupClassMethod(ctrlKlass, "OnActivate");
    void* fnOnDeact = Il2CppHelpers::LookupClassMethod(ctrlKlass, "OnDeactivate");
    void* fnActMs = FindActivateSafeModeMsOverload(modelKlass);
    void* fnDeact = Il2CppHelpers::LookupClassMethod(modelKlass, "DeactivateSafeMode");
    void* fnGetSm = Il2CppHelpers::LookupClassMethod(modelKlass, "get_safeMode");

    if (!fnOnAct || !fnOnDeact || !fnActMs) {
        Logger::Warn("NetworkSafeMode: failed to resolve required methods.");
        return false;
    }

    s_fnActivateMs = reinterpret_cast<t_ActivateMs>(fnActMs);
    s_fnDeactivate = reinterpret_cast<t_Deactivate>(fnDeact);
    s_fnGetSafeMode = reinterpret_cast<t_GetSafeMode>(fnGetSm);

    if (MH_CreateHook(fnOnAct, reinterpret_cast<void*>(&Hook_OnActivate), reinterpret_cast<void**>(&s_origOnActivate)) != MH_OK ||
        MH_EnableHook(fnOnAct) != MH_OK) {
        Logger::Warn("NetworkSafeMode: failed to hook ApplyNetworkSafeModeController::OnActivate");
        return false;
    }
    s_hookOnActivate = fnOnAct;

    if (MH_CreateHook(fnOnDeact, reinterpret_cast<void*>(&Hook_OnDeactivate), reinterpret_cast<void**>(&s_origOnDeactivate)) != MH_OK ||
        MH_EnableHook(fnOnDeact) != MH_OK) {
        Logger::Warn("NetworkSafeMode: failed to hook ApplyNetworkSafeModeController::OnDeactivate");
        MH_DisableHook(s_hookOnActivate);
        MH_RemoveHook(s_hookOnActivate);
        s_hookOnActivate = nullptr;
        return false;
    }
    s_hookOnDeactivate = fnOnDeact;

    if (void* fnOnSmCh = Il2CppHelpers::LookupClassMethod(ctrlKlass, "OnSafeModeChangedHandler")) {
        if (MH_CreateHook(fnOnSmCh, reinterpret_cast<void*>(&Hook_OnSafeModeChanged),
                          reinterpret_cast<void**>(&s_origOnSmChanged)) == MH_OK &&
            MH_EnableHook(fnOnSmCh) == MH_OK) {
            s_hookOnSmChanged = fnOnSmCh;
        }
    }

    const uintptr_t indKlass = Il2CppHelpers::LookupTypeClass("UINetworkSafeModeIndicatorContext", "UI.Contexts.Race");
    if (indKlass) {
        if (void* fnSetInd = Il2CppHelpers::LookupClassMethod(indKlass, "SetActiveSafeMode")) {
            if (MH_CreateHook(fnSetInd, reinterpret_cast<void*>(&Hook_SetActiveSafeMode),
                              reinterpret_cast<void**>(&s_origSetIndActive)) == MH_OK &&
                MH_EnableHook(fnSetInd) == MH_OK) {
                s_hookSetIndActive = fnSetInd;
            }
        }
    }

    s_hooksOk.store(true, std::memory_order_release);
    Logger::Info(std::string("NetworkSafeMode initialized (hooks=OK, syncHook=") +
                 (s_hookOnSmChanged ? "OK" : "OFF") + ", indicatorHook=" +
                 (s_hookSetIndActive ? "OK" : "OFF") + ").");
    return true;
}

void Shutdown() {
    if (!s_initialized.exchange(false)) return;
    if (s_hookOnSmChanged) {
        MH_DisableHook(s_hookOnSmChanged);
        MH_RemoveHook(s_hookOnSmChanged);
        s_hookOnSmChanged = nullptr;
    }
    if (s_hookSetIndActive) {
        MH_DisableHook(s_hookSetIndActive);
        MH_RemoveHook(s_hookSetIndActive);
        s_hookSetIndActive = nullptr;
    }
    if (s_hookOnActivate) {
        MH_DisableHook(s_hookOnActivate);
        MH_RemoveHook(s_hookOnActivate);
        s_hookOnActivate = nullptr;
    }
    if (s_hookOnDeactivate) {
        MH_DisableHook(s_hookOnDeactivate);
        MH_RemoveHook(s_hookOnDeactivate);
        s_hookOnDeactivate = nullptr;
    }
    std::lock_guard<std::mutex> lk(s_mtx);
    s_activeController = nullptr;
    s_safeModeModel = nullptr;
    s_hooksOk.store(false, std::memory_order_release);
    s_inLobby.store(false, std::memory_order_release);
    s_safeModeActive.store(false, std::memory_order_release);
}

void Trigger(int durationMs, bool forceImmediate) {
    if (!s_inLobby.load(std::memory_order_acquire)) return;
    durationMs = std::clamp(durationMs, 500, 10000);
    s_pendingDurationMs.store(durationMs, std::memory_order_relaxed);
    if (forceImmediate) {
        s_pendingForce.store(true, std::memory_order_release);
    }
}

void Deactivate() {
    if (!s_inLobby.load(std::memory_order_acquire)) return;
    s_pendingDeactivate.store(true, std::memory_order_release);
}

void SetShowGameIndicator(bool show) {
    s_showGameIndicator.store(show, std::memory_order_release);
}

bool GetShowGameIndicator() {
    return s_showGameIndicator.load(std::memory_order_acquire);
}

void TickOnMainThread() {
    void* ctrl = nullptr;
    void* model = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        ctrl = s_activeController;
        model = s_safeModeModel;
    }
    if (!ctrl || !model) {
        s_safeModeActive.store(false, std::memory_order_relaxed);
        return;
    }

    if (s_pendingDeactivate.exchange(false, std::memory_order_acq_rel)) {
        SafeCallDeactivateSEH(model, ctrl);
    }

    const int reqMs = s_pendingDurationMs.exchange(0, std::memory_order_acq_rel);
    const bool forceNow = s_pendingForce.exchange(false, std::memory_order_acq_rel);
    if (reqMs > 0) {
        const ULONGLONG nowMs = GetTickCount64();
        // Throttle SmartFox collision-group updates to avoid spamming the server while still extending active danger.
        if (forceNow || (nowMs - s_lastTriggerMs) >= 450) {
            if (SafeCallActivateMsSEH(model, static_cast<int32_t>(reqMs), ctrl)) {
                s_lastTriggerMs = nowMs;
                s_triggersTotal.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    s_safeModeActive.store(SafeCallGetSafeModeSEH(model), std::memory_order_relaxed);
}

Status GetStatus() {
    Status st{};
    st.hooksInstalled = s_hooksOk.load(std::memory_order_acquire);
    st.inLobby = s_inLobby.load(std::memory_order_acquire);
    st.safeModeActive = s_safeModeActive.load(std::memory_order_acquire);
    st.triggersTotal = s_triggersTotal.load(std::memory_order_relaxed);
    return st;
}

} // namespace NetworkSafeMode
