#include "car_sim_optimizer.h"
#include "unity_bridge.h"
#include "native_carx.h"
#include "hook_manager.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

// Verified RVAs in GameAssembly.dll
constexpr uintptr_t kRvaSwitchingUpdate               = 0xE4FA70;
constexpr uintptr_t kRvaIsFarCar                      = 0xE4F880;
constexpr uintptr_t kRvaProviderGetTransform          = 0xE57BA0;
constexpr uintptr_t kRvaTransformGetPosInjected       = 0x5F72710;
constexpr uintptr_t kRvaIl2CppVirtualInvokeData       = 0x9238B0;
constexpr uintptr_t kRvaCarBehaviourSetEffectsEnabled = 0x1E02570;

constexpr uintptr_t kRvaINetworkCarContainerTypeInfo  = 0x843A128;
constexpr uintptr_t kRvaDeformSleepTypeInfoGlobal     = 0x83E7018;
constexpr uintptr_t kRvaTypeInfoDefinitionTable       = 0x87EAEA0;

// Verified s_TypeInfoDefinitionTable indices
constexpr uint32_t kIdxOptimizeCarGeometry = 24088;
constexpr uint32_t kIdxRemoteCarRenderPolicy = 24090;
constexpr uint32_t kIdxDeformSkeletonSleep = 25730;
constexpr uint32_t kIdxPhysicsRootSleep    = 25775;

// Verified INetworkCarContainer interface slots
constexpr uint32_t kSlotNetCarGetId          = 6;
constexpr uint32_t kSlotNetCarGetPosition    = 7;
constexpr uint32_t kSlotNetCarIsKinematic    = 0xD;

using t_SwitchingUpdate        = void(__fastcall*)(void* self, void* method);
using t_IsFarCar               = uint8_t(__fastcall*)(void* self, void* netCar, void* method);
using t_ProviderGetTransform   = void*(__fastcall*)(void* provider, void* method);
using t_TransformGetPos        = UnityVector3*(__fastcall*)(UnityVector3* outPos, void* tf, void* method);
using t_LookupVirtualInvoke    = void**(__fastcall*)(void* obj, void* ifaceTypeInfo, uint32_t slot);
using t_NetCarGetPosition      = UnityVector3*(__fastcall*)(UnityVector3* outPos, void* netCar, void* method);
using t_NetCarGetInt32         = int32_t(__fastcall*)(void* netCar, void* method);
using t_NetCarGetBool          = uint8_t(__fastcall*)(void* netCar, void* method);
using t_SetCarEffectsEnabled   = void(__fastcall*)(void* carBehaviour, uint8_t enabled, void* method);

using t_CX_CarSetSimMode       = void(*)(void* car, int enable);
using t_CX_CarGetSimMode       = int(*)(void* car);
using t_CX_CarSetSteps         = void(*)(void* car, int steps);
using t_CX_CarGetSteps         = int(*)(void* car);

struct DeformBaseline {
    bool captured = false;
    uint8_t enabled = 1;
    int32_t idleSteps = 30;
    float farDistance = 30.0f;
};

struct RootSleepBaseline {
    bool captured = false;
    uint8_t enabled = 1;
    int32_t idleSteps = 30;
};

struct RenderPolicyBaseline {
    bool captured = false;
    uint8_t enabled = 1;
    float smallRadius = 0.40f;
    float tinyRadius = 0.20f;
    float smallHideDist = 25.0f;
};

struct OptimizeGeomBaseline {
    bool captured = false;
    uint8_t remoteCarPolicyEnabled = 1;
    uint8_t remoteLodHalfRate = 1;
    uint8_t playerBestLod = 0;
};

struct SavedNativeCarState {
    int origSimple = 0;
    int origTraffic = 0;
    int origSteps = 4;
    bool appliedSimple = false;
    bool appliedTraffic = false;
    bool appliedSteps = false;
};

struct SeenFrameCar {
    void* netCar = nullptr;
    int32_t ownerId = 0;
    UnityVector3 pos{};
    float dist = 0.0f;
    bool hasDist = false;
    bool vanillaFar = false;
    bool isKinematic = false;
    bool activeFullSim = false;
    bool forcedSnapshot = false;
};

std::recursive_mutex s_mutex;
CarSimOptimizer::Config s_cfg{};
CarSimOptimizer::Stats s_stats{};
std::vector<CarSimOptimizer::CarEntry> s_netCarsSnapshot;

uintptr_t s_gaBase = 0;
t_SwitchingUpdate s_origSwitchingUpdate = nullptr;
t_IsFarCar s_origIsFarCar = nullptr;
t_ProviderGetTransform pfn_ProviderGetTransform = nullptr;
t_TransformGetPos pfn_TransformGetPos = nullptr;
t_LookupVirtualInvoke pfn_LookupVirtualInvoke = nullptr;
t_SetCarEffectsEnabled pfn_SetCarEffectsEnabled = nullptr;

// NativeCarX dynamic exports
t_CX_CarSetSimMode pfn_SetSimSimple = nullptr;
t_CX_CarGetSimMode pfn_GetSimSimple = nullptr;
t_CX_CarSetSimMode pfn_SetSimTraffic = nullptr;
t_CX_CarGetSimMode pfn_GetSimTraffic = nullptr;
t_CX_CarSetSteps pfn_SetIntSteps = nullptr;
t_CX_CarGetSteps pfn_GetIntSteps = nullptr;
bool s_nativeExportsReady = false;

// Resolved IL2CPP static_fields pointers
void* s_sfDeformSleep = nullptr;
void* s_sfRootSleep = nullptr;
void* s_sfRenderPolicy = nullptr;
void* s_sfOptimizeGeom = nullptr;

DeformBaseline s_deformBase{};
RootSleepBaseline s_rootBase{};
RenderPolicyBaseline s_renderBase{};
OptimizeGeomBaseline s_geomBase{};

// Ranking / hysteresis / anti-flicker state (owned by switching thread + mutex)
std::unordered_set<void*> s_allowedFullSim;
std::unordered_set<void*> s_knownNetCars;
std::unordered_map<void*, std::chrono::steady_clock::time_point> s_slotEnteredAt;
std::chrono::steady_clock::time_point s_lastSwapTime{};
bool s_hasAllowedSet = false;

// Main-thread tracked overrides
std::unordered_set<void*> s_effectsDisabledSet; // CarBehaviour*
std::unordered_map<void*, SavedNativeCarState> s_savedNativeCars; // CX_Car*

// Frame-local collection inside DistanceBasedPhysicsSwitchingSubController::Update
thread_local bool t_inSwitchingUpdate = false;
thread_local bool t_hasPlayerPos = false;
thread_local UnityVector3 t_playerPos{};
thread_local std::vector<SeenFrameCar> t_frameCars;

// ==================== SEH Leaf Helpers ====================

static inline bool IsCanonPtr(uintptr_t p, size_t sz) {
    return p >= 0x10000ULL && p <= 0x00007FFFFFFEFFFFULL && sz > 0 && (p + sz) > p;
}

template <typename T>
static bool SafeRead(uintptr_t addr, T* out) {
    if (!out || !IsCanonPtr(addr, sizeof(T))) return false;
    __try {
        *out = *reinterpret_cast<const T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
static bool SafeWrite(uintptr_t addr, const T& val) {
    if (!IsCanonPtr(addr, sizeof(T))) return false;
    __try {
        *reinterpret_cast<T*>(addr) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadCString(uintptr_t addr, char* buf, size_t maxLen) {
    if (!buf || maxLen < 2 || !IsCanonPtr(addr, 1)) return false;
    __try {
        const char* p = reinterpret_cast<const char*>(addr);
        size_t i = 0;
        for (; i + 1 < maxLen; ++i) {
            char c = p[i];
            buf[i] = c;
            if (c == '\0') return true;
        }
        buf[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = '\0';
        return false;
    }
}

static void* ResolveStaticFieldsFromTypeInfo(uintptr_t klassPtr, const char* expectedName, const char* expectedNs) {
    if (!IsCanonPtr(klassPtr, 0xC0)) return nullptr;
    uintptr_t namePtr = 0;
    uintptr_t nsPtr = 0;
    uintptr_t staticFields = 0;
    if (!SafeRead(klassPtr + 0x10, &namePtr) || !SafeRead(klassPtr + 0x18, &nsPtr)) return nullptr;
    char nameBuf[96] = {};
    char nsBuf[96] = {};
    if (!SafeReadCString(namePtr, nameBuf, sizeof(nameBuf))) return nullptr;
    if (!SafeReadCString(nsPtr, nsBuf, sizeof(nsBuf))) return nullptr;
    if (strcmp(nameBuf, expectedName) != 0 || strcmp(nsBuf, expectedNs) != 0) return nullptr;
    if (!SafeRead(klassPtr + 0xB8, &staticFields)) return nullptr;
    return IsCanonPtr(staticFields, 0x20) ? reinterpret_cast<void*>(staticFields) : nullptr;
}

static void* LookupTypeStaticFields(uint32_t typeIdx, const char* expectedName, const char* expectedNs, uintptr_t fallbackGlobalRva = 0) {
    if (!s_gaBase) return nullptr;
    uintptr_t tablePtr = 0;
    if (SafeRead(s_gaBase + kRvaTypeInfoDefinitionTable, &tablePtr) && IsCanonPtr(tablePtr, sizeof(uintptr_t))) {
        uintptr_t klassPtr = 0;
        if (SafeRead(tablePtr + static_cast<uintptr_t>(typeIdx) * sizeof(uintptr_t), &klassPtr)) {
            if (void* sf = ResolveStaticFieldsFromTypeInfo(klassPtr, expectedName, expectedNs)) {
                return sf;
            }
        }
    }
    if (fallbackGlobalRva != 0) {
        uintptr_t klassPtr = 0;
        if (SafeRead(s_gaBase + fallbackGlobalRva, &klassPtr)) {
            return ResolveStaticFieldsFromTypeInfo(klassPtr, expectedName, expectedNs);
        }
    }
    return nullptr;
}

static bool LookupInterfaceSlotSEH(void* obj, void* ifaceTypeInfo, uint32_t slot, void** outFn, void** outMethodInfo) {
    if (!obj || !ifaceTypeInfo || !outFn || !outMethodInfo) return false;
    *outFn = nullptr;
    *outMethodInfo = nullptr;
    __try {
        uintptr_t objAddr = reinterpret_cast<uintptr_t>(obj);
        if (!IsCanonPtr(objAddr, sizeof(uintptr_t))) return false;
        uintptr_t klass = *reinterpret_cast<const uintptr_t*>(objAddr);
        if (!IsCanonPtr(klass, 0x140)) return false;

        uint16_t ifaceCount = *reinterpret_cast<const uint16_t*>(klass + 0x12E);
        uintptr_t ifaceOffsets = *reinterpret_cast<const uintptr_t*>(klass + 0xB0);
        if (ifaceCount > 0 && ifaceCount < 256 && IsCanonPtr(ifaceOffsets, static_cast<size_t>(ifaceCount) * 16)) {
            for (uint16_t i = 0; i < ifaceCount; ++i) {
                uintptr_t entryIface = *reinterpret_cast<const uintptr_t*>(ifaceOffsets + static_cast<size_t>(i) * 16);
                if (entryIface == reinterpret_cast<uintptr_t>(ifaceTypeInfo)) {
                    uint32_t startSlot = *reinterpret_cast<const uint32_t*>(ifaceOffsets + static_cast<size_t>(i) * 16 + 8);
                    uintptr_t vtableSlot = klass + 0x138 + static_cast<size_t>(startSlot + slot) * 16;
                    if (!IsCanonPtr(vtableSlot, 16)) break;
                    void* fn = *reinterpret_cast<void* const*>(vtableSlot);
                    void* mi = *reinterpret_cast<void* const*>(vtableSlot + 8);
                    if (IsCanonPtr(reinterpret_cast<uintptr_t>(fn), 8)) {
                        *outFn = fn;
                        *outMethodInfo = mi;
                        return true;
                    }
                    break;
                }
            }
        }
        if (pfn_LookupVirtualInvoke) {
            void** slotPair = pfn_LookupVirtualInvoke(obj, ifaceTypeInfo, slot);
            if (IsCanonPtr(reinterpret_cast<uintptr_t>(slotPair), 16)) {
                void* fn = slotPair[0];
                void* mi = slotPair[1];
                if (IsCanonPtr(reinterpret_cast<uintptr_t>(fn), 8)) {
                    *outFn = fn;
                    *outMethodInfo = mi;
                    return true;
                }
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outFn = nullptr;
        *outMethodInfo = nullptr;
        return false;
    }
}

static bool SafeGetPlayerPosFromController(void* switchingCtrl, UnityVector3* outPos) {
    if (!switchingCtrl || !outPos || !pfn_ProviderGetTransform || !pfn_TransformGetPos) return false;
    uintptr_t provider = 0;
    if (!SafeRead(reinterpret_cast<uintptr_t>(switchingCtrl) + 0x20, &provider) || !IsCanonPtr(provider, 0x20)) {
        return false;
    }
    __try {
        void* tf = pfn_ProviderGetTransform(reinterpret_cast<void*>(provider), nullptr);
        if (!IsCanonPtr(reinterpret_cast<uintptr_t>(tf), 0x18)) return false;
        UnityVector3 tmp{};
        UnityVector3* ret = pfn_TransformGetPos(&tmp, tf, nullptr);
        if (!ret) return false;
        *outPos = *ret;
        return std::isfinite(outPos->x) && std::isfinite(outPos->y) && std::isfinite(outPos->z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeQueryNetCarState(void* netCar, UnityVector3* outPos, int32_t* outId, bool* outKinematic) {
    if (!netCar || !s_gaBase) return false;
    uintptr_t ifaceTypeInfo = 0;
    if (!SafeRead(s_gaBase + kRvaINetworkCarContainerTypeInfo, &ifaceTypeInfo) || !IsCanonPtr(ifaceTypeInfo, 0x40)) {
        return false;
    }
    void* ifacePtr = reinterpret_cast<void*>(ifaceTypeInfo);

    void* fnPos = nullptr;
    void* miPos = nullptr;
    if (!LookupInterfaceSlotSEH(netCar, ifacePtr, kSlotNetCarGetPosition, &fnPos, &miPos)) {
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
        if (LookupInterfaceSlotSEH(netCar, ifacePtr, kSlotNetCarGetId, &fnId, &miId)) {
            __try {
                *outId = reinterpret_cast<t_NetCarGetInt32>(fnId)(netCar, miId);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    if (outKinematic) {
        *outKinematic = false;
        void* fnKin = nullptr;
        void* miKin = nullptr;
        if (LookupInterfaceSlotSEH(netCar, ifacePtr, kSlotNetCarIsKinematic, &fnKin, &miKin)) {
            __try {
                *outKinematic = (reinterpret_cast<t_NetCarGetBool>(fnKin)(netCar, miKin) != 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    return true;
}

static void* SafeFindCarBehaviourOnRootGo(void* rootGo) {
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
            if (cid != 66) continue; // MonoBehaviour
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

static bool SafeValidateAndCallSetCarEffects(void* carBehaviour, bool enable) {
    if (!pfn_SetCarEffectsEnabled || !carBehaviour) return false;
    uintptr_t cb = reinterpret_cast<uintptr_t>(carBehaviour);
    if (!IsCanonPtr(cb, 0x178)) return false;
    __try {
        uintptr_t klass = *reinterpret_cast<const uintptr_t*>(cb);
        if (!IsCanonPtr(klass, 0x18)) return false;
        uintptr_t knamePtr = *reinterpret_cast<const uintptr_t*>(klass + 0x10);
        if (!IsCanonPtr(knamePtr, 16)) return false;
        if (strcmp(reinterpret_cast<const char*>(knamePtr), "CarBehaviour") != 0) return false;

        // CarBehaviour::SetCarEffectsEnabled (0x1E02570) dereferences +0x168 and +0x170 items without null checks
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

// ==================== Static Config Sync & Restore ====================

static void ResolveNativeExportsIfNeeded() {
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

static void ResolveStaticsIfNeeded() {
    if (!s_gaBase) return;
    if (!s_sfDeformSleep) {
        s_sfDeformSleep = LookupTypeStaticFields(
            kIdxDeformSkeletonSleep, "DeformationSkeletonSleepSystem",
            "CarX.DeformationPhysics.Systems", kRvaDeformSleepTypeInfoGlobal);
        if (s_sfDeformSleep && !s_deformBase.captured) {
            uintptr_t p = reinterpret_cast<uintptr_t>(s_sfDeformSleep);
            uint8_t en = 1;
            int32_t steps = 30;
            float dist = 30.0f;
            if (SafeRead(p + 0x00, &en) && SafeRead(p + 0x04, &steps) && SafeRead(p + 0x08, &dist) &&
                dist > 0.1f && dist < 5000.0f && steps > 0 && steps < 1000) {
                s_deformBase = { true, en, steps, dist };
            } else {
                s_sfDeformSleep = nullptr;
            }
        }
    }
    if (!s_sfRootSleep) {
        s_sfRootSleep = LookupTypeStaticFields(
            kIdxPhysicsRootSleep, "PhysicsRootSleepSystem", "CarX.DeformationPhysics.Systems");
        if (s_sfRootSleep && !s_rootBase.captured) {
            uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRootSleep);
            uint8_t en = 1;
            int32_t steps = 30;
            if (SafeRead(p + 0x00, &en) && SafeRead(p + 0x04, &steps) && steps > 0 && steps < 1000) {
                s_rootBase = { true, en, steps };
            } else {
                s_sfRootSleep = nullptr;
            }
        }
    }
    if (!s_sfRenderPolicy) {
        s_sfRenderPolicy = LookupTypeStaticFields(
            kIdxRemoteCarRenderPolicy, "RemoteCarRenderPolicy", "Framework.Views.Cars.Components");
        if (s_sfRenderPolicy && !s_renderBase.captured) {
            uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRenderPolicy);
            uint8_t en = 1;
            float rSmall = 0.40f, rTiny = 0.20f, hideD = 25.0f;
            if (SafeRead(p + 0x00, &en) && SafeRead(p + 0x04, &rSmall) &&
                SafeRead(p + 0x08, &rTiny) && SafeRead(p + 0x0C, &hideD) &&
                hideD >= 1.0f && hideD < 5000.0f) {
                s_renderBase = { true, en, rSmall, rTiny, hideD };
            } else {
                s_sfRenderPolicy = nullptr;
            }
        }
    }
    if (!s_sfOptimizeGeom) {
        s_sfOptimizeGeom = LookupTypeStaticFields(
            kIdxOptimizeCarGeometry, "OptimizeCarGeometryComponent", "Framework.Views.Cars.Components");
        if (s_sfOptimizeGeom && !s_geomBase.captured) {
            uintptr_t p = reinterpret_cast<uintptr_t>(s_sfOptimizeGeom);
            uint8_t pol = 1, half = 1, best = 0;
            if (SafeRead(p + 0x00, &pol) && SafeRead(p + 0x01, &half) && SafeRead(p + 0x02, &best)) {
                s_geomBase = { true, pol, half, best };
            } else {
                s_sfOptimizeGeom = nullptr;
            }
        }
    }
}

static void SyncStaticSubsystems(const CarSimOptimizer::Config& cfg, bool forceRestore) {
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

// ==================== Main-Thread Effects & Offline Physics ====================

static void UpdateCarEffectsOnMainThread(const CarSimOptimizer::Config& cfg,
                                         const std::vector<LiveCarInfo>& liveCars,
                                         bool forceRestore) {
    // Map currently live CarBehaviour* by car pointer and track valid set to prevent stale dereference
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

    // Prune dead CarBehaviour entries before touching anything
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

    // Restore cars that moved back inside the budget
    for (auto it = s_effectsDisabledSet.begin(); it != s_effectsDisabledSet.end(); ) {
        if (shouldDisable.find(*it) == shouldDisable.end()) {
            SafeValidateAndCallSetCarEffects(*it, true);
            it = s_effectsDisabledSet.erase(it);
        } else {
            ++it;
        }
    }

    // Disable effects on excess far cars
    for (void* cb : shouldDisable) {
        if (s_effectsDisabledSet.find(cb) == s_effectsDisabledSet.end()) {
            if (SafeValidateAndCallSetCarEffects(cb, false)) {
                s_effectsDisabledSet.insert(cb);
            }
        }
    }
}

static void UpdateNativeCarPhysicsOnMainThread(const CarSimOptimizer::Config& cfg,
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

    // Prune destroyed cars cleanly
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

    // Restore cars back inside top-N
    for (auto it = s_savedNativeCars.begin(); it != s_savedNativeCars.end(); ) {
        if (reduceSet.find(it->first) == reduceSet.end()) {
            restoreCar(it->first, it->second);
            it = s_savedNativeCars.erase(it);
        } else {
            ++it;
        }
    }

    // Apply requested NativeCarX reductions beyond top-N
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

// ==================== Hook Detours ====================

static uint8_t __fastcall Hooked_IsFarCar(void* self, void* netCar, void* method) {
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

static void __fastcall Hooked_SwitchingUpdate(void* self, void* method) {
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

    // Recompute allowed top-(N-1) set with spatial hysteresis + temporal anti-flicker cooldown
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_stats.switchingUpdateCalls++;

    const auto now = std::chrono::steady_clock::now();
    const int remoteLimit = (std::max)(0, s_cfg.maxFullSimCars - 1);
    const float maxDist = std::clamp(s_cfg.maxFullSimDistMeters, 1.0f, 100.0f);
    const float hyst = std::clamp(s_cfg.hysteresisMeters, 0.0f, 30.0f);
    const float exitDist = (std::min)(100.0f, maxDist + (std::min)(hyst, 8.0f));
    const double cooldownSec = static_cast<double>(std::clamp(s_cfg.switchCooldownSec, 0.0f, 10.0f));

    // Update known net car set for IsFarCar first
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
                if (!sc.hasDist || d <= exitDist) {
                    active.push_back({ sc.netCar, d });
                }
            } else {
                if (!sc.hasDist || d <= maxDist) {
                    inactive.push_back({ sc.netCar, d });
                }
            }
        }

        auto byDistAsc = [](const CandidateCar& a, const CandidateCar& b) {
            return a.dist < b.dist;
        };
        std::sort(active.begin(), active.end(), byDistAsc);
        std::sort(inactive.begin(), inactive.end(), byDistAsc);

        if (static_cast<int>(active.size()) > remoteLimit) {
            active.resize(static_cast<size_t>(remoteLimit));
        }

        // Fill free slots immediately without waiting for cooldown
        size_t inactiveIdx = 0;
        while (static_cast<int>(active.size()) < remoteLimit && inactiveIdx < inactive.size()) {
            active.push_back(inactive[inactiveIdx]);
            s_slotEnteredAt[inactive[inactiveIdx].netCar] = now;
            ++inactiveIdx;
        }

        // Anti-flicker protection on occupied slots: require BOTH distance advantage > hysteresis AND hold cooldown
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

    // Publish snapshot sorted by physical distance
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

// ==================== Public API ====================

bool CarSimOptimizer::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_stats.initialized) return true;

    HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!hGA) {
        Logger::Warn("CarSimOptimizer: GameAssembly.dll not loaded yet, will retry.");
        return false;
    }
    s_gaBase = reinterpret_cast<uintptr_t>(hGA);

    pfn_ProviderGetTransform = reinterpret_cast<t_ProviderGetTransform>(s_gaBase + kRvaProviderGetTransform);
    pfn_TransformGetPos = reinterpret_cast<t_TransformGetPos>(s_gaBase + kRvaTransformGetPosInjected);
    pfn_LookupVirtualInvoke = reinterpret_cast<t_LookupVirtualInvoke>(s_gaBase + kRvaIl2CppVirtualInvokeData);
    pfn_SetCarEffectsEnabled = reinterpret_cast<t_SetCarEffectsEnabled>(s_gaBase + kRvaCarBehaviourSetEffectsEnabled);

    void* targetSwitching = reinterpret_cast<void*>(s_gaBase + kRvaSwitchingUpdate);
    void* targetIsFar = reinterpret_cast<void*>(s_gaBase + kRvaIsFarCar);

    bool okSwitch = HookManager::CreateHook(
        targetSwitching,
        reinterpret_cast<void*>(Hooked_SwitchingUpdate),
        reinterpret_cast<void**>(&s_origSwitchingUpdate),
        "DistanceBasedPhysicsSwitchingSubController::Update");

    bool okFar = HookManager::CreateHook(
        targetIsFar,
        reinterpret_cast<void*>(Hooked_IsFarCar),
        reinterpret_cast<void**>(&s_origIsFarCar),
        "DistanceBasedPhysicsSwitchingSubController::IsFarCar");

    ResolveNativeExportsIfNeeded();
    ResolveStaticsIfNeeded();

    s_stats.hooksInstalled = okSwitch && okFar;
    s_stats.initialized = true;

    Logger::Info(std::string("CarSimOptimizer initialized (hooks=") +
                 (s_stats.hooksInstalled ? "OK" : "PARTIAL") + ")");
    return s_stats.hooksInstalled;
}

void CarSimOptimizer::Shutdown() {
    auto live = UnityBridge::GetLiveCarList();
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_stats.initialized && !s_gaBase) return;

    s_cfg.enabled = false;
    SyncStaticSubsystems(s_cfg, true);
    UpdateNativeCarPhysicsOnMainThread(s_cfg, live, true);
    // Clear tracked CarBehaviour pointers; active Unity scenes are restored on MainThread prior to hook detach
    s_effectsDisabledSet.clear();

    s_allowedFullSim.clear();
    s_knownNetCars.clear();
    s_slotEnteredAt.clear();
    s_lastSwapTime = {};
    s_netCarsSnapshot.clear();
    s_hasAllowedSet = false;
    s_stats.antiFlickerHolding = false;
    s_stats.initialized = false;
    s_stats.hooksInstalled = false;
    Logger::Info("CarSimOptimizer baselines restored cleanly.");
}

void CarSimOptimizer::UpdateOnMainThread(float /*dt*/, const std::vector<LiveCarInfo>& liveCars) {
    if (HookManager::IsUnloading()) return;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_stats.initialized) {
        if (!Init()) return;
    }

    ResolveNativeExportsIfNeeded();
    ResolveStaticsIfNeeded();
    SyncStaticSubsystems(s_cfg, false);

    if (liveCars.empty()) {
        s_allowedFullSim.clear();
        s_knownNetCars.clear();
        s_slotEnteredAt.clear();
        s_lastSwapTime = {};
        s_netCarsSnapshot.clear();
        s_hasAllowedSet = false;
        s_stats.antiFlickerHolding = false;
        s_stats.totalNetCars = 0;
        s_stats.fullSimNetCars = 0;
        s_stats.snapshotNetCars = 0;
    }

    UpdateCarEffectsOnMainThread(s_cfg, liveCars, false);
    UpdateNativeCarPhysicsOnMainThread(s_cfg, liveCars, false);

    // Annotate net car snapshot entries with matched Unity/driver names
    for (auto& netEntry : s_netCarsSnapshot) {
        const LiveCarInfo* best = nullptr;
        float bestD2 = 6.25f; // 2.5m match radius
        for (const auto& lc : liveCars) {
            if (lc.isLocalPlayer) continue;
            const float dx = lc.x - netEntry.x;
            const float dy = lc.y - netEntry.y;
            const float dz = lc.z - netEntry.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) {
                bestD2 = d2;
                best = &lc;
            }
        }
        if (best && !best->unityName.empty()) {
            netEntry.name = best->unityName;
        } else if (netEntry.name.empty()) {
            netEntry.name = "NetCar #" + std::to_string(netEntry.ownerId);
        }
    }

    // Update live counters from static_fields
    if (s_sfDeformSleep) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfDeformSleep);
        SafeRead(p + 0x14, &s_stats.deformSleepingCars);
        SafeRead(p + 0x18, &s_stats.deformTransitions);
    }
    if (s_sfRootSleep) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRootSleep);
        SafeRead(p + 0x08, &s_stats.rootSleepingRoots);
        SafeRead(p + 0x0C, &s_stats.rootTransitions);
    }

    s_stats.nativeExportsReady = s_nativeExportsReady;
    s_stats.deformSleepResolved = (s_sfDeformSleep != nullptr);
    s_stats.rootSleepResolved = (s_sfRootSleep != nullptr);
    s_stats.renderPolicyResolved = (s_sfRenderPolicy != nullptr);
    s_stats.optimizeGeomResolved = (s_sfOptimizeGeom != nullptr);
    s_stats.effectsDisabledCars = static_cast<int32_t>(s_effectsDisabledSet.size());
    s_stats.offlineReducedCars = static_cast<int32_t>(s_savedNativeCars.size());

    if (s_deformBase.captured) {
        s_stats.vanillaDeformDist = s_deformBase.farDistance;
        s_stats.vanillaDeformIdleSteps = s_deformBase.idleSteps;
    }
    if (s_rootBase.captured) {
        s_stats.vanillaRootIdleSteps = s_rootBase.idleSteps;
    }
    if (s_renderBase.captured) {
        s_stats.vanillaSmallHideDist = s_renderBase.smallHideDist;
        s_stats.vanillaSmallRadius = s_renderBase.smallRadius;
        s_stats.vanillaTinyRadius = s_renderBase.tinyRadius;
    }
}

CarSimOptimizer::Config CarSimOptimizer::GetConfig() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_cfg;
}

void CarSimOptimizer::SetConfig(const Config& cfg) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    Config clamped = cfg;
    clamped.maxFullSimCars = std::clamp(clamped.maxFullSimCars, 1, 32);
    clamped.maxFullSimDistMeters = std::clamp(clamped.maxFullSimDistMeters, 5.0f, 100.0f);
    clamped.hysteresisMeters = std::clamp(clamped.hysteresisMeters, 0.0f, 30.0f);
    clamped.switchCooldownSec = std::clamp(clamped.switchCooldownSec, 0.0f, 10.0f);
    clamped.deformSleepDistMeters = std::clamp(clamped.deformSleepDistMeters, 3.0f, 100.0f);
    clamped.deformIdleSteps = std::clamp(clamped.deformIdleSteps, 2, 60);
    clamped.physicsRootIdleSteps = std::clamp(clamped.physicsRootIdleSteps, 2, 60);
    clamped.smallHideDistMeters = std::clamp(clamped.smallHideDistMeters, 4.0f, 100.0f);
    clamped.smallRendererRadius = std::clamp(clamped.smallRendererRadius, 0.10f, 1.50f);
    clamped.tinyRendererRadius = std::clamp(clamped.tinyRendererRadius, 0.05f, 1.00f);
    clamped.maxEffectsCars = std::clamp(clamped.maxEffectsCars, 1, 32);
    clamped.remoteIntegrationSteps = std::clamp(clamped.remoteIntegrationSteps, 1, 16);

    const bool coreSelectionChanged = (clamped.enabled != s_cfg.enabled) ||
                                      (clamped.maxFullSimCars != s_cfg.maxFullSimCars) ||
                                      (std::fabs(clamped.maxFullSimDistMeters - s_cfg.maxFullSimDistMeters) > 0.01f);
    if (coreSelectionChanged) {
        s_lastSwapTime = {};
        s_slotEnteredAt.clear();
    }
    s_cfg = clamped;
}

void CarSimOptimizer::ResetDefaults() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_lastSwapTime = {};
    s_slotEnteredAt.clear();
    s_stats.antiFlickerHolding = false;
    s_cfg = Config{};
}

CarSimOptimizer::Stats CarSimOptimizer::GetStats() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_stats;
}

std::vector<CarSimOptimizer::CarEntry> CarSimOptimizer::GetNetCarsSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_netCarsSnapshot;
}
