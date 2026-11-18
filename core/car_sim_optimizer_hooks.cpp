#include "car_sim_optimizer_internal.h"
#include "il2cpp_helpers.h"
#include "pattern_scanner.h"
#include "game_build.h"
#include "native_carx.h"
#include "hook_manager.h"
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CarSimOptimizerInternal {

namespace {

using Il2CppHelpers::IsCanonPtr;
using Il2CppHelpers::SafeRead;
using Il2CppHelpers::SafeWrite;

// Fallback RVAs inside GameAssembly.dll, per Steam branch (see game_build.h).
constexpr BranchRva kFbSwitchingUpdate           = { 0x0E4FA70, 0x13110F0 };
constexpr BranchRva kFbIsFarCar                  = { 0x0E50760, 0x1311E50 };
constexpr BranchRva kFbProviderGetTransform      = { 0x0E57BA0, 0x1319350 };
constexpr BranchRva kFbTransformGetPosInjected   = { 0x5F72710, 0x6FB9930 };
constexpr BranchRva kFbIl2CppVirtualInvokeData   = { 0x09238B0, 0x0A736D0 };
constexpr BranchRva kFbCarBehaviourSetEffects    = { 0x1E02570, 0x23D2460 };
constexpr BranchRva kFbINetworkCarContainerTI    = { 0x843A128, 0x9797458 };
constexpr BranchRva kFbDeformSleepTypeInfo       = { 0x83E7018, 0x984C918 };

constexpr uint32_t kIdxOptimizeCarGeometry   = 24088;
constexpr uint32_t kIdxRemoteCarRenderPolicy = 24090;
constexpr uint32_t kIdxDeformSkeletonSleep   = 25730;
constexpr uint32_t kIdxPhysicsRootSleep      = 25775;

constexpr uint32_t kSlotNetCarGetId          = 6;
constexpr uint32_t kSlotNetCarGetPosition    = 7;
constexpr uint32_t kSlotNetCarIsKinematic    = 0xD;

using t_SwitchingUpdate           = void(__fastcall*)(void* self, void* method);
using t_IsFarCar                  = uint8_t(__fastcall*)(void* self, void* netCar, void* method);
using t_ProviderGetTransform      = void*(__fastcall*)(void* provider, void* method);
using t_TransformGetPos           = UnityVector3*(__fastcall*)(UnityVector3* outPos, void* tf, void* method);
using t_NetCarGetPosition         = UnityVector3*(__fastcall*)(UnityVector3* outPos, void* netCar, void* method);
using t_NetCarGetInt32            = int32_t(__fastcall*)(void* netCar, void* method);
using t_NetCarGetBool             = uint8_t(__fastcall*)(void* netCar, void* method);
using t_SetCarEffectsEnabled      = void(__fastcall*)(void* carBehaviour, uint8_t enabled, void* method);
using t_SetDamageSystemsState     = void(__fastcall*)(void* self, uint8_t enable, void* method);

using t_CX_CarSetSimMode          = void(*)(void* car, int enable);
using t_CX_CarGetSimMode          = int(*)(void* car);
using t_CX_CarSetSteps            = void(*)(void* car, int steps);
using t_CX_CarGetSteps            = int(*)(void* car);

uintptr_t s_rvaSwitchingUpdate               = 0;
uintptr_t s_rvaIsFarCar                      = 0;
uintptr_t s_rvaProviderGetTransform          = 0;
uintptr_t s_rvaTransformGetPosInjected       = 0;
uintptr_t s_rvaIl2CppVirtualInvokeData       = 0;
uintptr_t s_rvaCarBehaviourSetEffectsEnabled = 0;
uintptr_t s_rvaINetworkCarContainerTypeInfo  = 0;
uintptr_t s_rvaDeformSleepTypeInfoGlobal     = 0;

t_SwitchingUpdate          s_origSwitchingUpdate = nullptr;
t_IsFarCar                 s_origIsFarCar = nullptr;
t_SetDamageSystemsState    s_origCutsceneSetDamageState = nullptr;
t_SetDamageSystemsState    s_origRaceSetDamageState = nullptr;

bool                       s_inSyntheticDamageToggle = false;
bool                       s_gameWantsDamage = true;
int                        s_lastAppliedDamageMaster = -1; // -1 = unknown, 0 = disabled by us, 1 = enabled
bool                       s_lastInSessionForDamage = false;

t_ProviderGetTransform               pfn_ProviderGetTransform = nullptr;
t_TransformGetPos                    pfn_TransformGetPos = nullptr;
Il2CppHelpers::t_LookupVirtualInvoke pfn_LookupVirtualInvoke = nullptr;
t_SetCarEffectsEnabled               pfn_SetCarEffectsEnabled = nullptr;

t_CX_CarSetSimMode pfn_SetSimSimple = nullptr;
t_CX_CarGetSimMode pfn_GetSimSimple = nullptr;
t_CX_CarSetSimMode pfn_SetSimTraffic = nullptr;
t_CX_CarGetSimMode pfn_GetSimTraffic = nullptr;
t_CX_CarSetSteps   pfn_SetIntSteps = nullptr;
t_CX_CarGetSteps   pfn_GetIntSteps = nullptr;

thread_local bool t_inSwitchingUpdate = false;
thread_local bool t_hasPlayerPos = false;
thread_local UnityVector3 t_playerPos{};
thread_local std::vector<SeenFrameCar> t_frameCars;

bool SafeGetPlayerPosFromController(void* switchingCtrl, UnityVector3* outPos) {
    if (!switchingCtrl || !outPos || !pfn_ProviderGetTransform) return false;
    uintptr_t provider = 0;
    if (!SafeRead(reinterpret_cast<uintptr_t>(switchingCtrl) + 0x20, &provider) || !IsCanonPtr(provider, 0x20)) {
        return false;
    }
    __try {
        void* tf = pfn_ProviderGetTransform(reinterpret_cast<void*>(provider), nullptr);
        if (!IsCanonPtr(reinterpret_cast<uintptr_t>(tf), 0x18)) return false;
        UnityVector3 tmp{};
        if (pfn_TransformGetPos) {
            UnityVector3* ret = pfn_TransformGetPos(&tmp, tf, nullptr);
            if (ret && std::isfinite(ret->x) && std::isfinite(ret->y) && std::isfinite(ret->z)) {
                *outPos = *ret;
                return true;
            }
        }
        if (UnityBridge::GetTransformHierarchyPosition(tf, tmp)) {
            *outPos = tmp;
            return std::isfinite(outPos->x) && std::isfinite(outPos->y) && std::isfinite(outPos->z);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeQueryNetCarState(void* netCar, UnityVector3* outPos, int32_t* outId, bool* outKinematic) {
    if (!netCar || !s_gaBase) return false;
    static uintptr_t s_cachedIfaceTypeInfo = 0;
    if (!s_cachedIfaceTypeInfo) {
        s_cachedIfaceTypeInfo = Il2CppHelpers::LookupTypeClass(
            "INetworkCarContainer", "CarX.Common.RealtimeMultiplayer.Core", s_rvaINetworkCarContainerTypeInfo);
    }
    uintptr_t ifaceTypeInfo = s_cachedIfaceTypeInfo;
    if (!ifaceTypeInfo && s_rvaINetworkCarContainerTypeInfo) {
        SafeRead(s_gaBase + s_rvaINetworkCarContainerTypeInfo, &ifaceTypeInfo);
    }
    if (!ifaceTypeInfo || !IsCanonPtr(ifaceTypeInfo, 0x40)) {
        return false;
    }
    void* ifacePtr = reinterpret_cast<void*>(ifaceTypeInfo);

    void* fnPos = nullptr;
    void* miPos = nullptr;
    if (!Il2CppHelpers::LookupInterfaceSlot(netCar, ifacePtr, kSlotNetCarGetPosition, &fnPos, &miPos, pfn_LookupVirtualInvoke)) {
        return false;
    }

    __try {
        UnityVector3 tmp{};
        UnityVector3* ret = reinterpret_cast<t_NetCarGetPosition>(fnPos)(&tmp, netCar, miPos);
        if (!ret || !std::isfinite(ret->x) || !std::isfinite(ret->y) || !std::isfinite(ret->z)) {
            return false;
        }
        *outPos = *ret;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (outId) {
        *outId = 0;
        void* fnId = nullptr;
        void* miId = nullptr;
        if (Il2CppHelpers::LookupInterfaceSlot(netCar, ifacePtr, kSlotNetCarGetId, &fnId, &miId, pfn_LookupVirtualInvoke)) {
            __try {
                *outId = reinterpret_cast<t_NetCarGetInt32>(fnId)(netCar, miId);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    if (outKinematic) {
        *outKinematic = false;
        void* fnKin = nullptr;
        void* miKin = nullptr;
        if (Il2CppHelpers::LookupInterfaceSlot(netCar, ifacePtr, kSlotNetCarIsKinematic, &fnKin, &miKin, pfn_LookupVirtualInvoke)) {
            __try {
                *outKinematic = (reinterpret_cast<t_NetCarGetBool>(fnKin)(netCar, miKin) != 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    return true;
}

void* SafeFindCarBehaviourOnRootGo(void* rootGo) {
    uintptr_t go = reinterpret_cast<uintptr_t>(rootGo);
    if (!IsCanonPtr(go, 0x38)) return nullptr;
    __try {
        uintptr_t compArr = *reinterpret_cast<const uintptr_t*>(go + 0x20);
        uint32_t compCount = *reinterpret_cast<const uint32_t*>(go + 0x30);
        if (compCount == 0 || compCount > 256 || !IsCanonPtr(compArr, static_cast<size_t>(compCount) * 16)) {
            return nullptr;
        }
        for (uint32_t ci = 0; ci < compCount; ++ci) {
            uint32_t cid = *reinterpret_cast<const uint32_t*>(compArr + ci * 16);
            if (cid != 66) continue;
            uintptr_t natComp = *reinterpret_cast<const uintptr_t*>(compArr + ci * 16 + 8);
            if (!IsCanonPtr(natComp, 0x20)) continue;
            uintptr_t gcSlot = *reinterpret_cast<const uintptr_t*>(natComp + 0x18);
            if (!IsCanonPtr(gcSlot & ~7ull, sizeof(uintptr_t))) continue;
            uintptr_t managed = *reinterpret_cast<const uintptr_t*>(gcSlot & ~7ull);
            if ((managed & 7) || !IsCanonPtr(managed, 0x178)) continue;
            uintptr_t klass = *reinterpret_cast<const uintptr_t*>(managed);
            if (!IsCanonPtr(klass, 0x18)) continue;
            uintptr_t knamePtr = *reinterpret_cast<const uintptr_t*>(klass + 0x10);
            if (!IsCanonPtr(knamePtr, 16)) continue;
            const char* kname = reinterpret_cast<const char*>(knamePtr);
            if (kname[0] == 'C' && kname[1] == 'a' && kname[2] == 'r' && strcmp(kname, "CarBehaviour") == 0) {
                return reinterpret_cast<void*>(managed);
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SafeValidateAndCallSetCarEffects(void* carBehaviour, bool enable) {
    if (!pfn_SetCarEffectsEnabled || !carBehaviour) return false;
    uintptr_t cb = reinterpret_cast<uintptr_t>(carBehaviour);
    if (!IsCanonPtr(cb, 0x178)) return false;
    __try {
        uintptr_t klass = *reinterpret_cast<const uintptr_t*>(cb);
        if (!IsCanonPtr(klass, 0x18)) return false;
        uintptr_t knamePtr = *reinterpret_cast<const uintptr_t*>(klass + 0x10);
        if (!IsCanonPtr(knamePtr, 16)) return false;
        if (strcmp(reinterpret_cast<const char*>(knamePtr), "CarBehaviour") != 0) return false;

        uintptr_t particlesPool = *reinterpret_cast<const uintptr_t*>(cb + 0x168);
        uintptr_t skidArray = *reinterpret_cast<const uintptr_t*>(cb + 0x170);
        if (!IsCanonPtr(particlesPool, 0x20) || !IsCanonPtr(skidArray, 0x20)) return false;

        uint32_t skidLen = *reinterpret_cast<const uint32_t*>(skidArray + 0x18);
        if (skidLen > 32 || !IsCanonPtr(skidArray, 0x20 + static_cast<size_t>(skidLen) * 8)) return false;
        for (uint32_t i = 0; i < skidLen; ++i) {
            uintptr_t elem = *reinterpret_cast<const uintptr_t*>(skidArray + 0x20 + static_cast<size_t>(i) * 8);
            if (!IsCanonPtr(elem, 0x18)) return false;
        }

        pfn_SetCarEffectsEnabled(carBehaviour, enable ? 1 : 0, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ShouldBlockDeformationDamage() {
    if (!NativeCarX::IsInSessionFast()) return false;
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_cfg.disableDamage || s_noDamageOverride;
}

// ==================== Hook Detours ====================

bool SafeInvokeCutsceneSetDamageStateRaw(t_SetDamageSystemsState fn, uint8_t enable) {
    if (!fn) return false;
    __try {
        fn(nullptr, enable, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeApplyDamageMasterState(bool enable) {
    if (!s_origCutsceneSetDamageState || !UnityBridge::IsOnMainThread()) return false;
    s_inSyntheticDamageToggle = true;
    const bool ok = SafeInvokeCutsceneSetDamageStateRaw(s_origCutsceneSetDamageState, enable ? 1 : 0);
    s_inSyntheticDamageToggle = false;
    return ok;
}

void HandleSetDamageSystemsState(t_SetDamageSystemsState orig, void* self, uint8_t enable, void* method) {
    HookManager::ScopedHookGuard guard;
    if (HookManager::IsUnloading() || !orig) {
        if (orig) orig(self, enable, method);
        return;
    }
    if (s_inSyntheticDamageToggle) {
        orig(self, enable, method);
        return;
    }
    s_gameWantsDamage = (enable != 0);
    if (ShouldBlockDeformationDamage()) {
        enable = 0;
        s_lastAppliedDamageMaster = 0;
        s_stats.damageBlocks++;
    } else if (NativeCarX::IsInSessionFast()) {
        s_lastAppliedDamageMaster = enable ? 1 : 0;
    }
    orig(self, enable, method);
}

void __fastcall Hooked_CutsceneSetDamageSystemsState(void* self, uint8_t enable, void* method) {
    HandleSetDamageSystemsState(s_origCutsceneSetDamageState, self, enable, method);
}

void __fastcall Hooked_RaceSetDamageSystemsState(void* self, uint8_t enable, void* method) {
    HandleSetDamageSystemsState(s_origRaceSetDamageState, self, enable, method);
}

uint8_t __fastcall Hooked_IsFarCar(void* self, void* netCar, void* method) {
    HookManager::ScopedHookGuard guard;
    if (HookManager::IsUnloading() || !s_origIsFarCar) {
        return s_origIsFarCar ? s_origIsFarCar(self, netCar, method) : 0;
    }

    const uint8_t vanillaFarByte = s_origIsFarCar(self, netCar, method);
    const bool vanillaFar = (vanillaFarByte != 0);

    UnityVector3 netPos{};
    int32_t ownerId = 0;
    bool isKinematic = false;
    bool hasQuery = SafeQueryNetCarState(netCar, &netPos, &ownerId, &isKinematic);

    float dist = 0.0f;
    bool hasDist = false;
    if (hasQuery) {
        if (!t_hasPlayerPos) {
            t_hasPlayerPos = SafeGetPlayerPosFromController(self, &t_playerPos);
        }
        if (t_hasPlayerPos) {
            const float dx = t_playerPos.x - netPos.x;
            const float dy = t_playerPos.y - netPos.y;
            const float dz = t_playerPos.z - netPos.z;
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            hasDist = std::isfinite(dist);
        }
    }

    CarSimOptimizer::Config cfgCopy;
    bool allowFullSim = !vanillaFar;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        cfgCopy = s_cfg;
        if (cfgCopy.enabled) {
            const bool isKnown = (s_knownNetCars.find(netCar) != s_knownNetCars.end());
            const bool wasAllowed = (s_allowedFullSim.find(netCar) != s_allowedFullSim.end());
            const float distLimit = wasAllowed
                ? (std::min)(100.0f, cfgCopy.maxFullSimDistMeters + (std::min)(cfgCopy.hysteresisMeters, 8.0f))
                : cfgCopy.maxFullSimDistMeters;

            if (vanillaFar) {
                allowFullSim = false;
            } else if (cfgCopy.maxFullSimCars <= 1) {
                allowFullSim = false;
            } else if (hasDist && dist > distLimit) {
                allowFullSim = false;
            } else if (s_hasAllowedSet) {
                if (isKnown) {
                    allowFullSim = wasAllowed;
                } else {
                    const size_t remoteCap = static_cast<size_t>((std::max)(0, cfgCopy.maxFullSimCars - 1));
                    allowFullSim = (s_allowedFullSim.size() < remoteCap);
                }
            }
        }
    }

    const bool resultFar = cfgCopy.enabled ? (vanillaFar || !allowFullSim) : vanillaFar;

    if (t_inSwitchingUpdate && netCar) {
        SeenFrameCar sc;
        sc.netCar = netCar;
        sc.ownerId = ownerId;
        sc.pos = netPos;
        sc.dist = dist;
        sc.hasDist = hasDist;
        sc.vanillaFar = vanillaFar;
        sc.isKinematic = isKinematic;
        sc.activeFullSim = !resultFar;
        sc.forcedSnapshot = cfgCopy.enabled && resultFar && !vanillaFar;
        t_frameCars.push_back(sc);
    }

    return resultFar ? 1 : 0;
}

void __fastcall Hooked_SwitchingUpdate(void* self, void* method) {
    HookManager::ScopedHookGuard guard;
    if (HookManager::IsUnloading() || !s_origSwitchingUpdate) {
        if (s_origSwitchingUpdate) s_origSwitchingUpdate(self, method);
        return;
    }

    t_inSwitchingUpdate = true;
    t_hasPlayerPos = SafeGetPlayerPosFromController(self, &t_playerPos);
    t_frameCars.clear();

    s_origSwitchingUpdate(self, method);

    t_inSwitchingUpdate = false;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_stats.switchingUpdateCalls++;

    const auto now = std::chrono::steady_clock::now();
    const int remoteLimit = (std::max)(0, s_cfg.maxFullSimCars - 1);
    const float maxDist = std::clamp(s_cfg.maxFullSimDistMeters, 1.0f, 100.0f);
    const float hyst = std::clamp(s_cfg.hysteresisMeters, 0.0f, 30.0f);
    const float exitDist = (std::min)(100.0f, maxDist + (std::min)(hyst, 8.0f));
    const double cooldownSec = static_cast<double>(std::clamp(s_cfg.switchCooldownSec, 0.0f, 10.0f));

    s_knownNetCars.clear();
    for (const auto& sc : t_frameCars) {
        s_knownNetCars.insert(sc.netCar);
    }

    if (!s_cfg.enabled) {
        s_allowedFullSim.clear();
        s_slotEnteredAt.clear();
        s_lastSwapTime = {};
        s_hasAllowedSet = false;
        s_stats.antiFlickerHolding = false;
    } else {
        struct CandidateCar {
            void* netCar;
            float dist;
        };
        std::vector<CandidateCar> active;
        std::vector<CandidateCar> inactive;
        active.reserve(t_frameCars.size());
        inactive.reserve(t_frameCars.size());

        for (const auto& sc : t_frameCars) {
            if (sc.vanillaFar) continue;
            const float d = sc.hasDist ? sc.dist : 99.0f;
            const bool wasActive = (s_allowedFullSim.find(sc.netCar) != s_allowedFullSim.end());
            if (wasActive) {
                if (!sc.hasDist || d <= exitDist) active.push_back({ sc.netCar, d });
            } else {
                if (!sc.hasDist || d <= maxDist) inactive.push_back({ sc.netCar, d });
            }
        }

        auto byDistAsc = [](const CandidateCar& a, const CandidateCar& b) { return a.dist < b.dist; };
        std::sort(active.begin(), active.end(), byDistAsc);
        std::sort(inactive.begin(), inactive.end(), byDistAsc);

        if (static_cast<int>(active.size()) > remoteLimit) {
            active.resize(static_cast<size_t>(remoteLimit));
        }

        size_t inactiveIdx = 0;
        while (static_cast<int>(active.size()) < remoteLimit && inactiveIdx < inactive.size()) {
            active.push_back(inactive[inactiveIdx]);
            s_slotEnteredAt[inactive[inactiveIdx].netCar] = now;
            ++inactiveIdx;
        }

        bool holding = false;
        if (remoteLimit > 0 && static_cast<int>(active.size()) == remoteLimit && inactiveIdx < inactive.size()) {
            const CandidateCar& worstActive = active.back();
            const CandidateCar& bestInactive = inactive[inactiveIdx];
            if (bestInactive.dist < worstActive.dist) {
                const double sinceSwap = (s_lastSwapTime.time_since_epoch().count() == 0)
                    ? 999.0
                    : std::chrono::duration<double>(now - s_lastSwapTime).count();
                auto itEnt = s_slotEnteredAt.find(worstActive.netCar);
                const double worstHeld = (itEnt == s_slotEnteredAt.end())
                    ? 999.0
                    : std::chrono::duration<double>(now - itEnt->second).count();

                const bool marginOk = (bestInactive.dist + hyst < worstActive.dist);
                const bool emergencyNear = (bestInactive.dist < 8.0f && worstActive.dist > bestInactive.dist + 20.0f &&
                                            sinceSwap >= 0.4 && worstHeld >= 0.4);
                const bool timeOk = emergencyNear || (sinceSwap >= cooldownSec && worstHeld >= cooldownSec);

                if (marginOk && timeOk) {
                    s_slotEnteredAt.erase(worstActive.netCar);
                    s_slotEnteredAt[bestInactive.netCar] = now;
                    active.back() = bestInactive;
                    s_lastSwapTime = now;
                } else {
                    holding = true;
                    s_stats.antiFlickerBlocks++;
                }
            }
        }
        s_stats.antiFlickerHolding = holding;

        s_allowedFullSim.clear();
        for (const auto& c : active) {
            s_allowedFullSim.insert(c.netCar);
            if (s_slotEnteredAt.find(c.netCar) == s_slotEnteredAt.end()) {
                s_slotEnteredAt[c.netCar] = now;
            }
        }
        for (auto it = s_slotEnteredAt.begin(); it != s_slotEnteredAt.end(); ) {
            if (s_allowedFullSim.find(it->first) == s_allowedFullSim.end()) {
                it = s_slotEnteredAt.erase(it);
            } else {
                ++it;
            }
        }
        s_hasAllowedSet = true;
    }

    std::sort(t_frameCars.begin(), t_frameCars.end(), [](const SeenFrameCar& a, const SeenFrameCar& b) {
        if (a.hasDist != b.hasDist) return a.hasDist;
        return a.dist < b.dist;
    });

    s_netCarsSnapshot.clear();
    s_netCarsSnapshot.reserve(t_frameCars.size());
    uint32_t fullCnt = 0;
    uint32_t snapCnt = 0;
    for (const auto& sc : t_frameCars) {
        CarSimOptimizer::CarEntry entry;
        entry.containerPtr = reinterpret_cast<uintptr_t>(sc.netCar);
        entry.ownerId = sc.ownerId;
        entry.x = sc.pos.x;
        entry.y = sc.pos.y;
        entry.z = sc.pos.z;
        entry.distMeters = sc.dist;
        entry.hasDist = sc.hasDist;
        entry.vanillaFar = sc.vanillaFar;
        entry.isKinematic = sc.isKinematic;
        entry.activeFullSim = sc.activeFullSim;
        entry.forcedSnapshot = sc.forcedSnapshot;
        if (sc.activeFullSim) ++fullCnt;
        else ++snapCnt;
        s_netCarsSnapshot.push_back(std::move(entry));
    }

    s_stats.totalNetCars = static_cast<uint32_t>(t_frameCars.size());
    s_stats.fullSimNetCars = fullCnt;
    s_stats.snapshotNetCars = snapCnt;
}

} // namespace

void ResolveNativeExportsIfNeeded() {
    if (s_nativeExportsReady) return;
    HMODULE hNative = GetModuleHandleW(L"NativeCarX.dll");
    if (!hNative) return;
    pfn_SetSimSimple  = reinterpret_cast<t_CX_CarSetSimMode>(GetProcAddress(hNative, "CX_CarSetSimModeSimple"));
    pfn_GetSimSimple  = reinterpret_cast<t_CX_CarGetSimMode>(GetProcAddress(hNative, "CX_CarGetSimModeSimple"));
    pfn_SetSimTraffic = reinterpret_cast<t_CX_CarSetSimMode>(GetProcAddress(hNative, "CX_CarSetSimModeTraffic"));
    pfn_GetSimTraffic = reinterpret_cast<t_CX_CarGetSimMode>(GetProcAddress(hNative, "CX_CarGetSimModeTraffic"));
    pfn_SetIntSteps   = reinterpret_cast<t_CX_CarSetSteps>(GetProcAddress(hNative, "CX_CarSetIntegrationSteps"));
    pfn_GetIntSteps   = reinterpret_cast<t_CX_CarGetSteps>(GetProcAddress(hNative, "CX_CarGetIntegrationSteps"));
    s_nativeExportsReady = (pfn_SetSimSimple && pfn_GetSimSimple && pfn_SetSimTraffic &&
                            pfn_GetSimTraffic && pfn_SetIntSteps && pfn_GetIntSteps);
}

void ResolveStaticsIfNeeded() {
    if (!s_gaBase) return;
    if (!s_sfDeformSleep) {
        s_sfDeformSleep = Il2CppHelpers::LookupTypeStaticFields(
            kIdxDeformSkeletonSleep, "DeformationSkeletonSleepSystem",
            "CarX.DeformationPhysics.Systems", s_rvaDeformSleepTypeInfoGlobal);
    }
    if (s_sfDeformSleep && !s_deformBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfDeformSleep);
        uint8_t en = 1;
        int32_t steps = 30;
        float dist = 30.0f;
        if (SafeRead(p + 0x00, &en) && SafeRead(p + 0x04, &steps) && SafeRead(p + 0x08, &dist) &&
            dist > 0.1f && dist < 5000.0f && steps > 0 && steps < 1000) {
            s_deformBase = { true, en, steps, dist };
        }
    }
    if (!s_sfRootSleep) {
        s_sfRootSleep = Il2CppHelpers::LookupTypeStaticFields(
            kIdxPhysicsRootSleep, "PhysicsRootSleepSystem", "CarX.DeformationPhysics.Systems");
    }
    if (s_sfRootSleep && !s_rootBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRootSleep);
        uint8_t en = 1;
        int32_t steps = 30;
        if (SafeRead(p + 0x00, &en) && SafeRead(p + 0x04, &steps) && steps > 0 && steps < 1000) {
            s_rootBase = { true, en, steps };
        }
    }
    if (!s_sfRenderPolicy) {
        s_sfRenderPolicy = Il2CppHelpers::LookupTypeStaticFields(
            kIdxRemoteCarRenderPolicy, "RemoteCarRenderPolicy", "Framework.Views.Cars.Components");
    }
    if (s_sfRenderPolicy && !s_renderBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRenderPolicy);
        uint8_t en = 1;
        float rSmall = 0.40f, rTiny = 0.20f, hideD = 25.0f;
        if (SafeRead(p + 0x00, &en) && SafeRead(p + 0x04, &rSmall) &&
            SafeRead(p + 0x08, &rTiny) && SafeRead(p + 0x0C, &hideD) &&
            hideD >= 1.0f && hideD < 5000.0f) {
            s_renderBase = { true, en, rSmall, rTiny, hideD };
        }
    }
    if (!s_sfOptimizeGeom) {
        s_sfOptimizeGeom = Il2CppHelpers::LookupTypeStaticFields(
            kIdxOptimizeCarGeometry, "OptimizeCarGeometryComponent", "Framework.Views.Cars.Components");
    }
    if (s_sfOptimizeGeom && !s_geomBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfOptimizeGeom);
        uint8_t pol = 1, half = 1, best = 0;
        if (SafeRead(p + 0x00, &pol) && SafeRead(p + 0x01, &half) && SafeRead(p + 0x02, &best)) {
            s_geomBase = { true, pol, half, best };
        }
    }
}

void SyncDamageMasterSwitch(bool inSession, bool noDamage, bool forceRestore) {
    if (!s_origCutsceneSetDamageState || !UnityBridge::IsOnMainThread()) {
        if (!inSession) {
            s_gameWantsDamage = true;
            s_lastAppliedDamageMaster = -1;
            s_lastInSessionForDamage = false;
        }
        return;
    }

    if (!inSession) {
        if (forceRestore && s_lastAppliedDamageMaster == 0) {
            SafeApplyDamageMasterState(true);
        }
        s_gameWantsDamage = true;
        s_lastAppliedDamageMaster = -1;
        s_lastInSessionForDamage = false;
        return;
    }

    if (!s_lastInSessionForDamage) {
        s_lastInSessionForDamage = true;
        s_lastAppliedDamageMaster = -1;
    }

    if (noDamage) {
        if (s_lastAppliedDamageMaster != 0) {
            if (SafeApplyDamageMasterState(false)) {
                s_lastAppliedDamageMaster = 0;
                Logger::Info("DeformMaster: disabled DeformationPhysicsEnabled singleton");
            }
        }
    } else {
        if (s_lastAppliedDamageMaster == 0) {
            const bool target = forceRestore ? true : s_gameWantsDamage;
            if (SafeApplyDamageMasterState(target)) {
                s_lastAppliedDamageMaster = forceRestore ? -1 : (target ? 1 : 0);
                Logger::Info(std::string("DeformMaster: restored DeformationPhysicsEnabled singleton (enable=") +
                             (target ? "1" : "0") + ")");
            }
        } else if (forceRestore) {
            s_lastAppliedDamageMaster = -1;
        }
    }
}

void SyncStaticSubsystems(const CarSimOptimizer::Config& cfg, bool forceRestore) {
    const bool inSession = NativeCarX::IsInSessionFast();
    const bool noDamage = inSession && !forceRestore && (cfg.disableDamage || s_noDamageOverride);
    SyncDamageMasterSwitch(inSession, noDamage, forceRestore);
    if (s_sfDeformSleep && s_deformBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfDeformSleep);
        if (!forceRestore && cfg.enabled && cfg.tuneDeformationSleep) {
            SafeWrite<uint8_t>(p + 0x00, 1);
            SafeWrite<int32_t>(p + 0x04, std::clamp(cfg.deformIdleSteps, 1, 120));
            SafeWrite<float>(p + 0x08, std::clamp(cfg.deformSleepDistMeters, 1.0f, 200.0f));
        } else {
            SafeWrite<uint8_t>(p + 0x00, s_deformBase.enabled);
            SafeWrite<int32_t>(p + 0x04, s_deformBase.idleSteps);
            SafeWrite<float>(p + 0x08, s_deformBase.farDistance);
        }
    }
    if (s_sfRootSleep && s_rootBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRootSleep);
        if (!forceRestore && cfg.enabled && cfg.tunePhysicsRootSleep) {
            SafeWrite<uint8_t>(p + 0x00, 1);
            SafeWrite<int32_t>(p + 0x04, std::clamp(cfg.physicsRootIdleSteps, 1, 120));
        } else {
            SafeWrite<uint8_t>(p + 0x00, s_rootBase.enabled);
            SafeWrite<int32_t>(p + 0x04, s_rootBase.idleSteps);
        }
    }
    if (s_sfRenderPolicy && s_renderBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRenderPolicy);
        if (!forceRestore && cfg.enabled && cfg.tuneRenderPolicy) {
            SafeWrite<uint8_t>(p + 0x00, 1);
            SafeWrite<float>(p + 0x04, std::clamp(cfg.smallRendererRadius, 0.05f, 3.0f));
            SafeWrite<float>(p + 0x08, std::clamp(cfg.tinyRendererRadius, 0.02f, 2.0f));
            SafeWrite<float>(p + 0x0C, std::clamp(cfg.smallHideDistMeters, 2.0f, 200.0f));
        } else {
            SafeWrite<uint8_t>(p + 0x00, s_renderBase.enabled);
            SafeWrite<float>(p + 0x04, s_renderBase.smallRadius);
            SafeWrite<float>(p + 0x08, s_renderBase.tinyRadius);
            SafeWrite<float>(p + 0x0C, s_renderBase.smallHideDist);
        }
    }
    if (s_sfOptimizeGeom && s_geomBase.captured) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfOptimizeGeom);
        if (!forceRestore && cfg.enabled && cfg.tuneRenderPolicy) {
            SafeWrite<uint8_t>(p + 0x00, 1);
            SafeWrite<uint8_t>(p + 0x01, cfg.remoteLodHalfRate ? 1 : 0);
            SafeWrite<uint8_t>(p + 0x02, cfg.playerBestLod ? 1 : 0);
        } else {
            SafeWrite<uint8_t>(p + 0x00, s_geomBase.remoteCarPolicyEnabled);
            SafeWrite<uint8_t>(p + 0x01, s_geomBase.remoteLodHalfRate);
            SafeWrite<uint8_t>(p + 0x02, s_geomBase.playerBestLod);
        }
    }
}

void UpdateCarEffectsOnMainThread(const CarSimOptimizer::Config& cfg,
                                  const std::vector<LiveCarInfo>& liveCars,
                                  bool forceRestore) {
    std::unordered_set<void*> liveCarBehaviours;
    struct RankedBehaviour {
        void* carBehaviour;
        float dist;
    };
    std::vector<RankedBehaviour> remoteBehaviours;
    remoteBehaviours.reserve(liveCars.size());

    for (const auto& c : liveCars) {
        if (!c.rootGo) continue;
        void* cb = SafeFindCarBehaviourOnRootGo(c.rootGo);
        if (!cb) continue;
        liveCarBehaviours.insert(cb);
        if (!c.isLocalPlayer) {
            remoteBehaviours.push_back({ cb, c.distToPlayer });
        }
    }

    for (auto it = s_effectsDisabledSet.begin(); it != s_effectsDisabledSet.end(); ) {
        if (liveCarBehaviours.find(*it) == liveCarBehaviours.end()) {
            it = s_effectsDisabledSet.erase(it);
        } else {
            ++it;
        }
    }

    if (forceRestore || !cfg.enabled || !cfg.limitCarEffects) {
        for (void* cb : s_effectsDisabledSet) {
            SafeValidateAndCallSetCarEffects(cb, true);
        }
        s_effectsDisabledSet.clear();
        return;
    }

    std::sort(remoteBehaviours.begin(), remoteBehaviours.end(), [](const RankedBehaviour& a, const RankedBehaviour& b) {
        return a.dist < b.dist;
    });

    const int remoteKeep = (std::max)(0, cfg.maxEffectsCars - 1);
    std::unordered_set<void*> shouldDisable;
    for (size_t i = static_cast<size_t>(remoteKeep); i < remoteBehaviours.size(); ++i) {
        shouldDisable.insert(remoteBehaviours[i].carBehaviour);
    }

    for (auto it = s_effectsDisabledSet.begin(); it != s_effectsDisabledSet.end(); ) {
        if (shouldDisable.find(*it) == shouldDisable.end()) {
            SafeValidateAndCallSetCarEffects(*it, true);
            it = s_effectsDisabledSet.erase(it);
        } else {
            ++it;
        }
    }

    for (void* cb : shouldDisable) {
        if (s_effectsDisabledSet.find(cb) == s_effectsDisabledSet.end()) {
            if (SafeValidateAndCallSetCarEffects(cb, false)) {
                s_effectsDisabledSet.insert(cb);
            }
        }
    }
}

void UpdateNativeCarPhysicsOnMainThread(const CarSimOptimizer::Config& cfg,
                                        const std::vector<LiveCarInfo>& liveCars,
                                        bool forceRestore) {
    if (!s_nativeExportsReady) return;

    std::unordered_set<void*> livePtrs;
    struct RankedNative {
        void* carPtr;
        float dist;
    };
    std::vector<RankedNative> remoteNative;
    remoteNative.reserve(liveCars.size());

    for (const auto& c : liveCars) {
        if (!c.carPtr) continue;
        livePtrs.insert(c.carPtr);
        if (!c.isLocalPlayer) {
            remoteNative.push_back({ c.carPtr, c.distToPlayer });
        }
    }

    for (auto it = s_savedNativeCars.begin(); it != s_savedNativeCars.end(); ) {
        if (livePtrs.find(it->first) == livePtrs.end()) {
            it = s_savedNativeCars.erase(it);
        } else {
            ++it;
        }
    }

    auto restoreCar = [](void* carPtr, SavedNativeCarState& st) {
        if (st.appliedSimple) {
            pfn_SetSimSimple(carPtr, st.origSimple);
            st.appliedSimple = false;
        }
        if (st.appliedTraffic) {
            pfn_SetSimTraffic(carPtr, st.origTraffic);
            st.appliedTraffic = false;
        }
        if (st.appliedSteps) {
            pfn_SetIntSteps(carPtr, st.origSteps);
            st.appliedSteps = false;
        }
    };

    if (forceRestore || !cfg.enabled || !cfg.tuneOfflinePhysics) {
        for (auto& kv : s_savedNativeCars) {
            restoreCar(kv.first, kv.second);
        }
        s_savedNativeCars.clear();
        return;
    }

    std::sort(remoteNative.begin(), remoteNative.end(), [](const RankedNative& a, const RankedNative& b) {
        return a.dist < b.dist;
    });

    const int remoteFullLimit = (std::max)(0, cfg.maxFullSimCars - 1);
    std::unordered_set<void*> reduceSet;
    for (size_t i = static_cast<size_t>(remoteFullLimit); i < remoteNative.size(); ++i) {
        reduceSet.insert(remoteNative[i].carPtr);
    }

    for (auto it = s_savedNativeCars.begin(); it != s_savedNativeCars.end(); ) {
        if (reduceSet.find(it->first) == reduceSet.end()) {
            restoreCar(it->first, it->second);
            it = s_savedNativeCars.erase(it);
        } else {
            ++it;
        }
    }

    const int targetSteps = std::clamp(cfg.remoteIntegrationSteps, 1, 16);
    for (void* carPtr : reduceSet) {
        auto it = s_savedNativeCars.find(carPtr);
        if (it == s_savedNativeCars.end()) {
            SavedNativeCarState baseline;
            baseline.origSimple = pfn_GetSimSimple(carPtr);
            baseline.origTraffic = pfn_GetSimTraffic(carPtr);
            baseline.origSteps = (std::max)(1, pfn_GetIntSteps(carPtr));
            it = s_savedNativeCars.emplace(carPtr, baseline).first;
        }
        SavedNativeCarState& st = it->second;

        if (cfg.useSimpleModeBeyondTopN) {
            pfn_SetSimSimple(carPtr, 1);
            st.appliedSimple = true;
        } else if (st.appliedSimple) {
            pfn_SetSimSimple(carPtr, st.origSimple);
            st.appliedSimple = false;
        }

        if (cfg.useTrafficModeBeyondTopN) {
            pfn_SetSimTraffic(carPtr, 1);
            st.appliedTraffic = true;
        } else if (st.appliedTraffic) {
            pfn_SetSimTraffic(carPtr, st.origTraffic);
            st.appliedTraffic = false;
        }

        if (targetSteps != st.origSteps) {
            pfn_SetIntSteps(carPtr, targetSteps);
            st.appliedSteps = true;
        } else if (st.appliedSteps) {
            pfn_SetIntSteps(carPtr, st.origSteps);
            st.appliedSteps = false;
        }
    }
}

bool InstallHooksAndResolvers(HMODULE hGA) {
    s_gaBase = reinterpret_cast<uintptr_t>(hGA);
    auto toRva = [](uintptr_t va) -> uintptr_t { return va >= s_gaBase ? va - s_gaBase : 0; };

    // 1. CarBehaviour::SetCarEffectsEnabled
    s_rvaCarBehaviourSetEffectsEnabled = toRva(PatternScanner::FindFunctionEntryOrFallback(
        hGA, "48 8B 89 68 01 00 00 48 85 C9 74 ? 45 33 C0 E8 ? ? ? ? 48 8B BF 70 01 00 00",
        GameBuild::Pick(kFbCarBehaviourSetEffects), 256, "CarBehaviour_SetEffectsEnabled"));

    // 2. DistanceBasedPhysicsSwitchingSubController::Update & IsFarCar (prefer IL2CPP metadata lookup)
    uintptr_t switchingKlass = Il2CppHelpers::LookupTypeClass(
        "DistanceBasedPhysicsSwitchingSubController", "CarX.Physics.SubControllers");
    if (!switchingKlass) {
        switchingKlass = Il2CppHelpers::LookupTypeClass(
            "DistanceBasedPhysicsSwitchingSubController", "Framework.Views.Cars.Components");
    }
    uintptr_t vaSwitchingUpdate = switchingKlass
        ? reinterpret_cast<uintptr_t>(Il2CppHelpers::LookupClassMethod(switchingKlass, "Update")) : 0;
    s_rvaSwitchingUpdate = vaSwitchingUpdate
        ? toRva(vaSwitchingUpdate)
        : toRva(PatternScanner::FindFunctionEntryOrFallback(
            hGA, "4C 8B 46 28 4D 85 C0 0F 84 ? ? ? ? 33 C9 48 8B 15 ? ? ? ? E8 ? ? ? ? 48 89 44 24 50",
            GameBuild::Pick(kFbSwitchingUpdate), 256, "DistanceBasedSwitching_Update"));
    uintptr_t vaIsFar = switchingKlass
        ? reinterpret_cast<uintptr_t>(Il2CppHelpers::LookupClassMethod(switchingKlass, "IsFarCar")) : 0;
    s_rvaIsFarCar = vaIsFar
        ? toRva(vaIsFar)
        : toRva(PatternScanner::FindOrFallback(
            hGA, "48 89 5C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC B0 01 00 00 4C 8B E9",
            GameBuild::Pick(kFbIsFarCar), "DistanceBasedSwitching_IsFarCar"));

    // 3. CarX provider get_transform
    s_rvaProviderGetTransform = toRva(PatternScanner::FindRipOrFallback(
        hGA, "48 83 C1 18 E8 ? ? ? ? 48 8B 7B 20 48 85 FF 74 ? 33 D2 48 8B CB E8 ? ? ? ?", 5, 1,
        GameBuild::Pick(kFbProviderGetTransform), 0x17, "ProviderGetTransform"));

    // 4. UnityEngine.Transform::get_position_Injected
    s_rvaTransformGetPosInjected = toRva(PatternScanner::FindRipOrFallback(
        hGA, "48 8B D5 E8 ? ? ? ? 48 85 DB 74 ? F2 0F 10 00", 5, 1,
        GameBuild::Pick(kFbTransformGetPosInjected), 3, "Transform_get_position_Injected"));

    // 5. il2cpp interface VirtualInvokeData lookup
    s_rvaIl2CppVirtualInvokeData = toRva(PatternScanner::FindRipOrFallback(
        hGA, "45 0F B7 C3 48 8B CB E8 ? ? ? ? 48 8B 48 08 48 8B D7 E8 ? ? ? ? 4C 8B C0 0F 28 CE", 5, 1,
        GameBuild::Pick(kFbIl2CppVirtualInvokeData), 7, "Il2CppVirtualInvokeData"));

    // 6. INetworkCarContainer & DeformationSkeletonSleepSystem fallback TypeInfos
    s_rvaINetworkCarContainerTypeInfo = toRva(PatternScanner::FindRipOrFallback(
        hGA, "48 8B 15 ? ? ? ? B9 11 00 00 00 4C 8B C3 48 8B 5C 24 30", 7, 3,
        GameBuild::Pick(kFbINetworkCarContainerTI), 0, "INetworkCarContainer_TypeInfo"));
    s_rvaDeformSleepTypeInfoGlobal = toRva(PatternScanner::FindRipOrFallback(
        hGA, "74 ? 48 8B 15 ? ? ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 83 B9 ? ? ? ? 00 75 ? E8 ? ? ? ? 80 3D ? ? ? ? 00", 7, 3,
        GameBuild::Pick(kFbDeformSleepTypeInfo), 0xE, "DeformSkeletonSleep_TypeInfo"));

    pfn_ProviderGetTransform = s_rvaProviderGetTransform ? reinterpret_cast<t_ProviderGetTransform>(s_gaBase + s_rvaProviderGetTransform) : nullptr;
    pfn_TransformGetPos = s_rvaTransformGetPosInjected ? reinterpret_cast<t_TransformGetPos>(s_gaBase + s_rvaTransformGetPosInjected) : nullptr;
    pfn_LookupVirtualInvoke = s_rvaIl2CppVirtualInvokeData ? reinterpret_cast<Il2CppHelpers::t_LookupVirtualInvoke>(s_gaBase + s_rvaIl2CppVirtualInvokeData) : nullptr;
    pfn_SetCarEffectsEnabled = s_rvaCarBehaviourSetEffectsEnabled ? reinterpret_cast<t_SetCarEffectsEnabled>(s_gaBase + s_rvaCarBehaviourSetEffectsEnabled) : nullptr;

    void* targetSwitching = s_rvaSwitchingUpdate ? reinterpret_cast<void*>(s_gaBase + s_rvaSwitchingUpdate) : nullptr;
    void* targetIsFar = s_rvaIsFarCar ? reinterpret_cast<void*>(s_gaBase + s_rvaIsFarCar) : nullptr;

    bool okSwitch = targetSwitching && HookManager::CreateHook(
        targetSwitching, reinterpret_cast<void*>(Hooked_SwitchingUpdate),
        reinterpret_cast<void**>(&s_origSwitchingUpdate),
        "DistanceBasedPhysicsSwitchingSubController::Update");

    bool okFar = targetIsFar && HookManager::CreateHook(
        targetIsFar, reinterpret_cast<void*>(Hooked_IsFarCar),
        reinterpret_cast<void**>(&s_origIsFarCar),
        "DistanceBasedPhysicsSwitchingSubController::IsFarCar");

    // 7. Upstream Deformation Master Switch (DeformationPhysicsEnabled singleton)
    uintptr_t cutsceneDeformKlass = Il2CppHelpers::LookupTypeClass(
        "CutscenesDeformationStateController", "UI.UIView.Runtime.Controllers.Cutscenes");
    void* vaCutsceneSetDamage = cutsceneDeformKlass
        ? Il2CppHelpers::LookupClassMethod(cutsceneDeformKlass, "SetDamageSystemsState") : nullptr;
    bool okCutsceneMaster = vaCutsceneSetDamage && HookManager::CreateHook(
        vaCutsceneSetDamage, reinterpret_cast<void*>(Hooked_CutsceneSetDamageSystemsState),
        reinterpret_cast<void**>(&s_origCutsceneSetDamageState),
        "CutscenesDeformationStateController::SetDamageSystemsState");

    uintptr_t raceDeformKlass = Il2CppHelpers::LookupTypeClass(
        "DeformationSystemsController", "UI.Controllers.Race");
    void* vaRaceSetDamage = raceDeformKlass
        ? Il2CppHelpers::LookupClassMethod(raceDeformKlass, "SetDamageSystemsState") : nullptr;
    bool okRaceMaster = vaRaceSetDamage && HookManager::CreateHook(
        vaRaceSetDamage, reinterpret_cast<void*>(Hooked_RaceSetDamageSystemsState),
        reinterpret_cast<void**>(&s_origRaceSetDamageState),
        "DeformationSystemsController::SetDamageSystemsState");

    ResolveNativeExportsIfNeeded();
    ResolveStaticsIfNeeded();

    return (okSwitch || okFar || okCutsceneMaster || okRaceMaster);
}

} // namespace CarSimOptimizerInternal
