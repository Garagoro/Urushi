#include "unity_bridge.h"
#include "pattern_scanner.h"
#include "game_build.h"
#include "carfx.h"
#include "ecs_collision.h"
#include "car_sim_optimizer.h"
#include "native_carx.h"
#include "replay_engine.h"
#include "hook_manager.h"
#include "lua_engine.h"
#include "logger.h"
#include "settings.h"
#include <psapi.h>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

uintptr_t UnityBridge::s_unityBase = 0;
uintptr_t UnityBridge::s_unitySize = 0;
std::recursive_mutex UnityBridge::s_mutex;
void* UnityBridge::s_playerCSharpCar = nullptr;
void* UnityBridge::s_playerNativeObj = nullptr;
std::vector<UnityBridge::GhostSlot> UnityBridge::s_ghostSlots;
std::vector<UnityBridge::SceneCarModelCache> UnityBridge::s_sceneCarCache;
std::vector<LiveCarInfo> UnityBridge::s_liveCars;
std::chrono::steady_clock::time_point UnityBridge::s_lastSceneCarScan{};
bool UnityBridge::s_initialized = false;

std::atomic<DWORD> UnityBridge::s_mainThreadId{ 0 };
std::atomic<bool> UnityBridge::s_pendingSpawn{ false };
std::atomic<bool> UnityBridge::s_pendingDestroy{ false };
std::atomic<bool> UnityBridge::s_pendingTransform{ false };
std::atomic<bool> UnityBridge::s_ghostMirrorMode{ false };
float UnityBridge::s_ghostMirrorOffset = 3.8f;
std::atomic<bool> UnityBridge::s_pendingReplayApply{ false };
float UnityBridge::s_pendingReplayOffset = 0.0f;
static ReplayFrame s_pendingReplayFrameData{};
static float s_lastMainDt = 0.016f;
std::atomic<bool> UnityBridge::s_pendingMultiReplayApply{ false };
float UnityBridge::s_pendingMultiReplayTime = 0.0f;
float UnityBridge::s_pendingMultiReplayOffset = 0.0f;
std::atomic<bool> UnityBridge::s_ghostCollisionEnabled{ false };
std::atomic<bool> UnityBridge::s_ghostMirrorPlayerShader{ true };
std::atomic<bool> UnityBridge::s_pendingCollisionSync{ false };
std::atomic<bool> UnityBridge::s_pendingTeleport{ false };
std::atomic<bool> UnityBridge::s_pendingRepairCar{ false };
TeleportPose UnityBridge::s_pendingTeleportPose{};
int UnityBridge::s_pendingTeleportVelMode = 0;
UnityVector3 UnityBridge::s_pendingPos{};
UnityVector4 UnityBridge::s_pendingRot{};

UnityBridge::t_ExecutePlayerLoop UnityBridge::s_originalExecutePlayerLoop = nullptr;

UnityBridge::t_Find UnityBridge::pfn_Find = nullptr;
UnityBridge::t_Internal_CloneSingle UnityBridge::pfn_CloneSingle = nullptr;
UnityBridge::t_Destroy UnityBridge::pfn_Destroy = nullptr;
UnityBridge::t_SetActive UnityBridge::pfn_SetActive = nullptr;
UnityBridge::t_SetLayer UnityBridge::pfn_SetLayer = nullptr;
UnityBridge::t_GameObject_get_transform UnityBridge::pfn_GameObject_get_transform = nullptr;
UnityBridge::t_Transform_set_position UnityBridge::pfn_Transform_set_position = nullptr;
UnityBridge::t_Transform_set_rotation UnityBridge::pfn_Transform_set_rotation = nullptr;
UnityBridge::t_Transform_get_position UnityBridge::pfn_Transform_get_position = nullptr;
UnityBridge::t_Transform_get_rotation UnityBridge::pfn_Transform_get_rotation = nullptr;
UnityBridge::t_GetMainCameraNative UnityBridge::pfn_GetMainCameraNative = nullptr;
UnityBridge::t_InternalRaycast UnityBridge::pfn_InternalRaycast = nullptr;

// ================= Leaf SEH Helpers (No C++ Unwinding Objects) =================

static bool SafeReadBytes(const void* addr, void* outBuf, size_t size) {
    __try {
        if (!addr || (uintptr_t)addr < 0x10000 || !outBuf) return false;
        memcpy(outBuf, addr, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadU16(const void* addr, uint16_t* outVal) {
    return SafeReadBytes(addr, outVal, sizeof(uint16_t));
}

// ================= CarFX Player Livery Extractor (Validated + Leaf SEH) =================

typedef void* (*t_Tex_GetNativePtr)(void* nativeTex);

// Fast user-mode canonical range check for use inside __try / __except leaf SEH blocks (zero kernel VirtualQuery stalls)
static inline bool IsReadableMem(uintptr_t addr, size_t size) {
    return addr >= 0x10000ULL && addr <= 0x00007FFFFFFEFFFFULL && size > 0 && (addr + size) > addr;
}

static bool IsReadableMemVQ(uintptr_t addr, size_t size) {
    if (!IsReadableMem(addr, size)) return false;
    uintptr_t cur = addr;
    const uintptr_t end = addr + size;
    const DWORD kReadMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if ((mbi.Protect & kReadMask) == 0) return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur) return false;
        cur = regionEnd;
    }
    return true;
}

static bool ReadBuiltinNativeTexGlobal(uintptr_t fnAddr, void** outNativeTex) {
    if (!fnAddr || !outNativeTex) return false;
    *outNativeTex = nullptr;
    // Unity 6000.3 get_*Texture_Injected: 53 48 83 EC 20 48 8B 1D <disp32>
    if (!IsReadableMemVQ(fnAddr, 13)) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(fnAddr);
    if (p[6] != 0x48 || p[7] != 0x8B || p[8] != 0x1D) return false;
    int32_t disp = 0;
    memcpy(&disp, p + 9, sizeof(disp));
    uintptr_t globalSlot = static_cast<uintptr_t>(static_cast<intptr_t>(fnAddr + 13) + disp);
    if (!IsReadableMemVQ(globalSlot, sizeof(void*))) return false;
    void* nat = *reinterpret_cast<void* const*>(globalSlot);
    if (reinterpret_cast<uintptr_t>(nat) < 0x10000) return false;
    *outNativeTex = nat;
    return true;
}

static bool SafeCallGetNativeTexPtr(uintptr_t fnAddr, void* nativeTex, void** outD3DPtr) {
    if (!fnAddr || !nativeTex || !outD3DPtr) return false;
    *outD3DPtr = nullptr;
    const uintptr_t natAddr = reinterpret_cast<uintptr_t>(nativeTex);
    // UP+0x4824f0 reads [rcx+0x110] and then derefs [rbx+0x10] with no null check!
    if (!IsReadableMemVQ(natAddr, 0x118)) return false;
    uintptr_t gfxTex = *reinterpret_cast<const uintptr_t*>(natAddr + 0x110);
    if (!IsReadableMemVQ(gfxTex, 0x18)) return false;

    __try {
        *outD3DPtr = reinterpret_cast<t_Tex_GetNativePtr>(fnAddr)(nativeTex);
        return *outD3DPtr != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outD3DPtr = nullptr;
        return false;
    }
}

struct UnityManagedSpanWrapper {
    const wchar_t* begin;
    int32_t length;
};

typedef void* (*t_UP_MatGetTexture)(void* natMat, int32_t propId);
typedef void  (*t_UP_MatSetTexture)(void* natMat, int32_t propId, void* natTex);
typedef void  (*t_UP_GfxCopyTextureFull)(void* srcNatTex, void* dstNatTex);
typedef void* (*t_UP_InstanceIDToObject)(const int32_t* instanceId);
typedef int32_t (*t_UP_MatGetRenderQueue)(void* natMat);
typedef void (*t_UP_MatSetRenderQueue)(void* natMat, int32_t queue);
typedef void (*t_UP_MatSetShaderPassEnabled)(void* natMat, const UnityManagedSpanWrapper* passName, bool enabled);
typedef void* (*t_UP_GetOrAssignInstantiatedMat)(void* natRenderer, int32_t slotIdx);
typedef void (*t_UP_RendererSetMat0)(void* natRenderer, void* natMat);
typedef void (*t_UP_RendererSetMatAt)(void* natRenderer, const int32_t* matId, uint32_t slot);

static t_UP_InstanceIDToObject GetInstanceIDToObjectFn(uintptr_t unityBase) {
    static t_UP_InstanceIDToObject s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "40 56 48 83 EC 20 83 39 00 48 8B F1 75 08 33 C0", 0x107480, "InstanceIDToObject");
        s_cached = reinterpret_cast<t_UP_InstanceIDToObject>(va);
    }
    return s_cached;
}

static t_UP_MatGetRenderQueue GetMatGetRenderQueueFn(uintptr_t unityBase) {
    static t_UP_MatGetRenderQueue s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 83 EC 28 83 79 2C 00 7C ? 8B 41 2C 48 83 C4 28 C3", 0x95EB0, "MatGetRenderQueue");
        s_cached = reinterpret_cast<t_UP_MatGetRenderQueue>(va);
    }
    return s_cached;
}

static t_UP_MatSetRenderQueue GetMatSetRenderQueueFn(uintptr_t unityBase) {
    static t_UP_MatSetRenderQueue s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "40 57 48 83 EC 20 48 8B F9 39 51 2C 74 ? 80 3D ? ? ? ? 00 89 51 2C", 0x95EF0, "MatSetRenderQueue");
        s_cached = reinterpret_cast<t_UP_MatSetRenderQueue>(va);
    }
    return s_cached;
}

static t_UP_MatSetShaderPassEnabled GetMatSetShaderPassEnabledFn(uintptr_t unityBase) {
    static t_UP_MatSetShaderPassEnabled s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "40 53 57 48 83 EC 68 48 8B F9 C6 44 24 30 00", 0x98530, "MatSetShaderPassEnabled");
        s_cached = reinterpret_cast<t_UP_MatSetShaderPassEnabled>(va);
    }
    return s_cached;
}

static t_UP_GetOrAssignInstantiatedMat GetOrAssignInstantiatedMatFn(uintptr_t unityBase) {
    static t_UP_GetOrAssignInstantiatedMat s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 10 48 89 6C 24 20 44 88 44 24 18 56 41 56 41 57", 0x47AA00, "GetOrAssignInstantiatedMat");
        s_cached = reinterpret_cast<t_UP_GetOrAssignInstantiatedMat>(va);
    }
    return s_cached;
}

static t_UP_RendererSetMat0 GetRendererSetMat0Fn(uintptr_t unityBase) {
    static t_UP_RendererSetMat0 s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B F9 48 8B F2", 0x8CE50, "RendererSetMat0");
        s_cached = reinterpret_cast<t_UP_RendererSetMat0>(va);
    }
    return s_cached;
}

static uintptr_t GetNativeTexPtrFn(uintptr_t unityBase) {
    static uintptr_t s_cached = 0;
    if (!s_cached && unityBase) {
        s_cached = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 08 57 48 83 EC 20 48 8B 99 10 01 00 00", 0x4824F0, "GetNativeTexturePtr");
    }
    return s_cached;
}

static t_UP_MatGetTexture GetMatGetTextureFn(uintptr_t unityBase) {
    static t_UP_MatGetTexture s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "89 54 24 10 55 56 48 8D 6C 24 B1 48 81 EC D8 00 00 00 48 8B 41 48", 0x603830, "MatGetTexture");
        s_cached = reinterpret_cast<t_UP_MatGetTexture>(va);
    }
    return s_cached;
}

static t_UP_MatSetTexture GetMatSetTextureFn(uintptr_t unityBase) {
    static t_UP_MatSetTexture s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 8B C4 89 50 10 55 53 56 48 8D 68 A1 48 81 EC F0 00 00 00", 0x602EB0, "MatSetTexture");
        s_cached = reinterpret_cast<t_UP_MatSetTexture>(va);
    }
    return s_cached;
}

static t_UP_GfxCopyTextureFull GetGfxCopyTextureFullFn(uintptr_t unityBase) {
    static t_UP_GfxCopyTextureFull s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 55 48 8D 6C 24 A9 48 81 EC D0 00 00 00", 0x7E620, "GfxCopyTextureFull");
        s_cached = reinterpret_cast<t_UP_GfxCopyTextureFull>(va);
    }
    return s_cached;
}

static void* GetGameObjectManagerPtr(uintptr_t unityBase) {
    static void* s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindRipOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "B8 FF FF FF FF 48 8B 0D ? ? ? ? 8B D0", 7, 3, 0x2232770, 5, "GameObjectManager");
        s_cached = reinterpret_cast<void*>(va);
    }
    return s_cached;
}

static void* GetGCHandleTargetPtr(uintptr_t unityBase) {
    static void* s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindRipOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "C3 48 8B 05 ? ? ? ? 48 8B 49 18 FF D0", 7, 3, 0x22339F0, 1, "GCHandleTarget");
        s_cached = reinterpret_cast<void*>(va);
    }
    return s_cached;
}

static void GetBuiltinNativeTextures(uintptr_t unityBase, void** outWhite, void** outBlack, void** outNormal) {
    static void* s_natWhite = nullptr;
    static void* s_natBlack = nullptr;
    static void* s_natNormal = nullptr;
    static bool s_resolved = false;
    if (!s_resolved && unityBase) {
        s_resolved = true;
        ReadBuiltinNativeTexGlobal(unityBase + 0xA8CF0, &s_natWhite);
        ReadBuiltinNativeTexGlobal(unityBase + 0xA8D30, &s_natBlack);
        ReadBuiltinNativeTexGlobal(unityBase + 0xA8E30, &s_natNormal);
    }
    if (outWhite) *outWhite = s_natWhite;
    if (outBlack) *outBlack = s_natBlack;
    if (outNormal) *outNormal = s_natNormal;
}

static bool IsDefaultNativeTex(void* natTex, void* natWhite, void* natBlack, void* natNormal) {
    return !natTex || natTex == natWhite || natTex == natBlack || natTex == natNormal;
}

static void* SafeGetMatTextureSEH(uintptr_t unityBase, void* natMat, int32_t propId) {
    if (!unityBase || !natMat) return nullptr;
    if (!IsReadableMem(reinterpret_cast<uintptr_t>(natMat), 0x58)) return nullptr;
    const auto pfnGetTex = GetMatGetTextureFn(unityBase);
    if (!pfnGetTex) return nullptr;
    __try {
        return pfnGetTex(natMat, propId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool SafeSetMatTextureSEH(uintptr_t unityBase, void* natMat, int32_t propId, void* natTex) {
    if (!unityBase || !natMat) return false;
    if (!IsReadableMem(reinterpret_cast<uintptr_t>(natMat), 0x58)) return false;
    if (natTex && !IsReadableMem(reinterpret_cast<uintptr_t>(natTex), 0x44)) return false;
    const auto pfnSetTex = GetMatSetTextureFn(unityBase);
    if (!pfnSetTex) return false;
    __try {
        pfnSetTex(natMat, propId, natTex);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCopyTextureFullSEH(uintptr_t unityBase, void* srcNatTex, void* dstNatTex) {
    if (!unityBase || !srcNatTex || !dstNatTex || srcNatTex == dstNatTex) return false;
    const uintptr_t sAddr = reinterpret_cast<uintptr_t>(srcNatTex);
    const uintptr_t dAddr = reinterpret_cast<uintptr_t>(dstNatTex);
    if (!IsReadableMemVQ(sAddr, 0x118) || !IsReadableMemVQ(dAddr, 0x118)) return false;
    const uintptr_t sGfx = *reinterpret_cast<const uintptr_t*>(sAddr + 0x110);
    const uintptr_t dGfx = *reinterpret_cast<const uintptr_t*>(dAddr + 0x110);
    if (!IsReadableMemVQ(sGfx, 0x18) || !IsReadableMemVQ(dGfx, 0x18)) return false;
    const auto pfnCopy = GetGfxCopyTextureFullFn(unityBase);
    if (!pfnCopy) return false;
    __try {
        pfnCopy(srcNatTex, dstNatTex);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct CarBehaviourInfo {
    uintptr_t carBehaviour = 0;
    void* nativeCarPtr = nullptr;
    void* raceCarPtr = nullptr;
    bool isPlayer = false;
};

struct Float4x4 {
    float m[16];
};

static uintptr_t SafeFindCarBehaviourOnRootGoSEH(void* rootGo) {
    if (!rootGo) return 0;
    __try {
        uintptr_t go = reinterpret_cast<uintptr_t>(rootGo);
        if (!IsReadableMem(go, 0x38)) return 0;
        uintptr_t compArr = *reinterpret_cast<uintptr_t*>(go + 0x20);
        uint32_t compCount = *reinterpret_cast<uint32_t*>(go + 0x30);
        if (compCount == 0 || compCount > 256 || !IsReadableMem(compArr, static_cast<size_t>(compCount) * 16)) return 0;

        for (uint32_t ci = 0; ci < compCount; ++ci) {
            uint32_t cid = *reinterpret_cast<uint32_t*>(compArr + ci * 16);
            if (cid != 66) continue; // MonoBehaviour
            uintptr_t natComp = *reinterpret_cast<uintptr_t*>(compArr + ci * 16 + 8);
            if (!IsReadableMem(natComp, 0x20)) continue;
            uintptr_t gcSlot = *reinterpret_cast<uintptr_t*>(natComp + 0x18);
            if (!IsReadableMem(gcSlot & ~7ull, sizeof(uintptr_t))) continue;
            uintptr_t managed = *reinterpret_cast<uintptr_t*>(gcSlot & ~7ull);
            if ((managed & 7) || !IsReadableMem(managed, 0x118)) continue;
            uintptr_t klass = *reinterpret_cast<uintptr_t*>(managed);
            if (!IsReadableMem(klass, 0x18)) continue;
            const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
            if (!IsReadableMem(reinterpret_cast<uintptr_t>(kname), 16)) continue;
            if (kname[0] == 'C' && kname[1] == 'a' && kname[2] == 'r' &&
                strcmp(kname, "CarBehaviour") == 0) {
                return managed;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static bool SafeInspectCarBehaviourOnRootGoSEH(void* rootGo, CarBehaviourInfo* outInfo) {
    if (!rootGo || !outInfo) return false;
    *outInfo = CarBehaviourInfo{};
    uintptr_t cb = SafeFindCarBehaviourOnRootGoSEH(rootGo);
    if (cb < 0x10000) return false;

    __try {
        outInfo->carBehaviour = cb;
        // Verified offsets in CarBehaviour [idx=23722]:
        //   +0x50: <carX>k__BackingField (CARXCar : Car) -> +0xC8: Car.native (NativeCarX handle)
        //   +0x58: Framework.Models.Cars.RaceCars.RaceCar* raceCarModel
        //   +0x110: uint8_t m_isPlayer
        outInfo->isPlayer = (*reinterpret_cast<const uint8_t*>(cb + 0x110) != 0);

        uintptr_t carXObj = *reinterpret_cast<const uintptr_t*>(cb + 0x50);
        if (IsReadableMem(carXObj, 0xD0)) {
            void* nativePtr = *reinterpret_cast<void* const*>(carXObj + 0xC8);
            if (reinterpret_cast<uintptr_t>(nativePtr) >= 0x10000) {
                outInfo->nativeCarPtr = nativePtr;
            }
        }

        uintptr_t raceCar = *reinterpret_cast<const uintptr_t*>(cb + 0x58);
        if (IsReadableMem(raceCar, 0x160)) {
            outInfo->raceCarPtr = reinterpret_cast<void*>(raceCar);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outInfo = CarBehaviourInfo{};
        return false;
    }
}

static void* SafeFindDeformationControllerSEH(void* carBehaviour) {
    if (!carBehaviour) return nullptr;
    uintptr_t cb = reinterpret_cast<uintptr_t>(carBehaviour);
    if (!IsReadableMem(cb, 0x150)) return nullptr;
    __try {
        uintptr_t dict = *reinterpret_cast<const uintptr_t*>(cb + 0x140);
        if (!IsReadableMem(dict, 0x30)) return nullptr;
        uint32_t count = *reinterpret_cast<const uint32_t*>(dict + 0x20);
        uintptr_t entries = *reinterpret_cast<const uintptr_t*>(dict + 0x18);
        if (!entries || count == 0 || count > 64) return nullptr;
        if (!IsReadableMem(entries, 0x20 + count * 24)) return nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t val = *reinterpret_cast<const uintptr_t*>(entries + 0x20 + i * 24 + 16);
            if (!val || !IsReadableMem(val, 0x18)) continue;
            uintptr_t klass = *reinterpret_cast<const uintptr_t*>(val);
            if (!klass || !IsReadableMem(klass, 0x20)) continue;
            uintptr_t namePtr = *reinterpret_cast<const uintptr_t*>(klass + 0x10);
            if (!IsReadableMem(namePtr, 32)) continue;
            const char* kname = reinterpret_cast<const char*>(namePtr);
            if (strcmp(kname, "CarDeformationFeatureController") == 0) {
                return reinterpret_cast<void*>(val);
            }
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool SafeCallRaceCarTeleportSEH(uintptr_t gaBase, void* raceCarPtr,
                                       const Float4x4* mat,
                                       const UnityVector3* linVel,
                                       const UnityVector3* angVel) {
    if (!gaBase || !raceCarPtr || !mat || !linVel || !angVel) return false;
    __try {
        uintptr_t rc = reinterpret_cast<uintptr_t>(raceCarPtr);
        if (!IsReadableMem(rc, 0x160)) return false;
        uintptr_t klass = *reinterpret_cast<uintptr_t*>(rc);
        if (!IsReadableMem(klass, 0x18)) return false;
        const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(kname), 16) ||
            strcmp(kname, "RaceCar") != 0) {
            return false;
        }
        uintptr_t localToWorldObj = *reinterpret_cast<uintptr_t*>(rc + 0x140);
        uintptr_t velObj = *reinterpret_cast<uintptr_t*>(rc + 0x148);
        uintptr_t angVelObj = *reinterpret_cast<uintptr_t*>(rc + 0x150);
        if (!IsReadableMem(localToWorldObj, 0x68) ||
            !IsReadableMem(velObj, 0x20) ||
            !IsReadableMem(angVelObj, 0x20)) {
            return false;
        }

        // Dynamically resolved RVAs in GameAssembly.dll (RaceCar):
        //   ForceVelocityChange(Vector3 velocity, Vector3 angularVelocity)
        //   ForceTransformChange(float4x4 localToWorld)
        using t_ForceVelocityChange = void(__fastcall*)(void*, const UnityVector3*, const UnityVector3*, void*);
        using t_ForceTransformChangeMat = void(__fastcall*)(void*, const Float4x4*, void*);

        static uintptr_t s_rvaVel = 0;
        static uintptr_t s_rvaTf = 0;
        if (!s_rvaVel && gaBase) {
            uintptr_t vaVel = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(gaBase),
                "48 83 EC 38 4C 8B 89 48 01 00 00 4D 85 C9 ? ? F2 0F 10 02",
                GameBuild::Pick({ 0x1DCE130, 0x239D160 }), "ForceVelocityChange");
            s_rvaVel = vaVel ? (vaVel - gaBase) : 0;

            uintptr_t vaTf = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(gaBase),
                "48 89 5C 24 08 57 48 83 EC 60 48 8B F9 48 8B DA 48 8B 89 40 01 00 00",
                GameBuild::Pick({ 0x1DCE0B0, 0x239D0E0 }), "ForceTransformChangeMat");
            s_rvaTf = vaTf ? (vaTf - gaBase) : 0;
        }
        if (!s_rvaVel || !s_rvaTf) return false; // unknown build and signatures missed

        auto pfnVel = reinterpret_cast<t_ForceVelocityChange>(gaBase + s_rvaVel);
        auto pfnTf  = reinterpret_cast<t_ForceTransformChangeMat>(gaBase + s_rvaTf);

        if (pfnVel) pfnVel(raceCarPtr, linVel, angVel, nullptr);
        if (pfnTf) pfnTf(raceCarPtr, mat, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeExtractMasterMatTexPairSEH(void* rootGo, uint32_t targetTypeId,
                                           void** outNatAlbedo, void** outNatPacked,
                                           uint32_t* outAlbTexId, uint32_t* outPackTexId,
                                           uintptr_t* outPartsMap) {
    if (!rootGo || !outNatAlbedo || !outNatPacked || !outAlbTexId || !outPackTexId) return false;
    *outNatAlbedo = nullptr;
    *outNatPacked = nullptr;
    *outAlbTexId = 0;
    *outPackTexId = 0;
    if (outPartsMap) *outPartsMap = 0;

    uintptr_t carBehaviour = SafeFindCarBehaviourOnRootGoSEH(rootGo);
    if (carBehaviour < 0x10000) return false;

    __try {

        // Verified chain: CarBehaviour +0x60 -> CarModel +0x38 -> PartsMap +0x18 -> MasterMaterials +0x20 -> m_textures
        uintptr_t carModel = *reinterpret_cast<uintptr_t*>(carBehaviour + 0x60);
        if (!IsReadableMem(carModel, 0x40)) return false;
        uintptr_t partsMap = *reinterpret_cast<uintptr_t*>(carModel + 0x38);
        if (!IsReadableMem(partsMap, 0x48)) return false;
        if (outPartsMap) *outPartsMap = partsMap;
        uintptr_t masterMats = *reinterpret_cast<uintptr_t*>(partsMap + 0x18);
        if (!IsReadableMem(masterMats, 0x28)) return false;
        uintptr_t mTexturesDict = *reinterpret_cast<uintptr_t*>(masterMats + 0x20);
        if (!IsReadableMem(mTexturesDict, 0x28)) return false;

        uintptr_t outerEntries = *reinterpret_cast<uintptr_t*>(mTexturesDict + 0x18);
        uint32_t outerCount = *reinterpret_cast<uint32_t*>(mTexturesDict + 0x20);
        if (outerCount == 0 || outerCount > 64 || !IsReadableMem(outerEntries, 0x20)) return false;
        uint32_t outerLen = *reinterpret_cast<uint32_t*>(outerEntries + 0x18);
        if (outerCount > outerLen) outerCount = outerLen;
        if (outerCount == 0 || !IsReadableMem(outerEntries, 0x20 + static_cast<size_t>(outerCount) * 24)) return false;

        uintptr_t subDict = 0;
        uint32_t foundTids[16] = {};
        uint32_t numFoundTids = 0;
        for (uint32_t i = 0; i < outerCount; ++i) {
            uintptr_t e = outerEntries + 0x20 + static_cast<size_t>(i) * 24;
            int32_t hc = *reinterpret_cast<int32_t*>(e + 0);
            if (hc < 0) continue;
            uint32_t tid = *reinterpret_cast<uint32_t*>(e + 8);
            if (numFoundTids < 16) foundTids[numFoundTids++] = tid;
            if (tid == targetTypeId) {
                subDict = *reinterpret_cast<uintptr_t*>(e + 16);
            }
        }
        if (!IsReadableMem(subDict, 0x28)) return false;

        uintptr_t innerEntries = *reinterpret_cast<uintptr_t*>(subDict + 0x18);
        uint32_t innerCount = *reinterpret_cast<uint32_t*>(subDict + 0x20);
        if (innerCount == 0 || innerCount > 64 || !IsReadableMem(innerEntries, 0x20)) return false;
        uint32_t innerLen = *reinterpret_cast<uint32_t*>(innerEntries + 0x18);
        if (innerCount > innerLen) innerCount = innerLen;
        if (innerCount == 0 || !IsReadableMem(innerEntries, 0x20 + static_cast<size_t>(innerCount) * 24)) return false;

        uintptr_t manAlb = 0, manPack = 0;
        for (uint32_t j = 0; j < innerCount; ++j) {
            uintptr_t e = innerEntries + 0x20 + static_cast<size_t>(j) * 24;
            int32_t hc = *reinterpret_cast<int32_t*>(e + 0);
            if (hc < 0) continue;
            int32_t propId = *reinterpret_cast<int32_t*>(e + 8);
            uintptr_t texMan = *reinterpret_cast<uintptr_t*>(e + 16);
            if (propId == 380) manAlb = texMan;        // 0x17C = u_sTexture
            else if (propId == 1370) manPack = texMan; // 0x55A = u_sPackedMap
        }

        if (IsReadableMem(manAlb, 0x18)) {
            uintptr_t nat = *reinterpret_cast<uintptr_t*>(manAlb + 0x10);
            if (IsReadableMem(nat, 0x44)) {
                *outNatAlbedo = reinterpret_cast<void*>(nat);
                *outAlbTexId = *reinterpret_cast<uint32_t*>(nat + 0x40);
            }
        }
        if (IsReadableMem(manPack, 0x18)) {
            uintptr_t nat = *reinterpret_cast<uintptr_t*>(manPack + 0x10);
            if (IsReadableMem(nat, 0x44)) {
                *outNatPacked = reinterpret_cast<void*>(nat);
                *outPackTexId = *reinterpret_cast<uint32_t*>(nat + 0x40);
            }
        }
        return (*outNatAlbedo != nullptr) || (*outNatPacked != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outNatAlbedo = nullptr;
        *outNatPacked = nullptr;
        return false;
    }
}

static bool SafeCheckNativeRootGo(void* rootTf, void** outRootGo) {
    if (!rootTf || !outRootGo) return false;
    *outRootGo = nullptr;
    uintptr_t tf = reinterpret_cast<uintptr_t>(rootTf);
    if (!IsReadableMem(tf + 0x20, sizeof(void*))) return false;
    __try {
        *outRootGo = *reinterpret_cast<void**>(tf + 0x20);
        return *outRootGo != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outRootGo = nullptr;
        return false;
    }
}

struct CarRendererRef {
    int32_t instanceId = 0;
    int32_t matIds[16] = {};
    uint32_t matCount = 0;
};

struct SavedStealthMatSwap {
    int32_t rendererInstId = 0;
    int32_t slotIdx = 0;
    int32_t origAssetMatId = 0;
};

enum CarMatCategory : int {
    kMatSkip = 0,
    kMatBody = 1,
    kMatStealthPart = 2
};

static void* SafeIdToObjectSEH(uintptr_t unityBase, int32_t id) {
    if (!unityBase || id == 0) return nullptr;
    const auto pfn = GetInstanceIDToObjectFn(unityBase);
    if (!pfn) return nullptr;
    __try {
        return pfn(&id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Which internals group a material belongs to (0 = not an internal: body, glass, shadow proxy).
static uint32_t InternalsPartOfMaterial(const char* name) {
    char l[64] = {};
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(l); ++n) l[n] = static_cast<char>(tolower(static_cast<unsigned char>(name[n])));
    auto has = [&](const char* s) { return strstr(l, s) != nullptr; };
    if (has("car_body") || has("car_glass") || has("shadowcaster")) return 0;
    if (has("rim") || has("yolo") || has("defaultdiffuse") || has("wheel_motion")) return CarFX::kPartWheels;
    if (has("brakerotor") || has("caliper")) return CarFX::kPartBrakes;
    if (has("suspension")) return CarFX::kPartSuspension;
    if (has("engine") || has("j2z") || has("rs20") || has("aircleaner") || has("chrome") || has("plastic") ||
        has("logos") || has("flclst") || has("turbo") || has("exhaust") || has("radiator") || has("intake") || has("bell")) {
        return CarFX::kPartEngine;
    }
    return CarFX::kPartInterior;
}

static bool SafeReadMatNameSEH(uintptr_t unityBase, int32_t matId, char* outName, size_t maxLen) {
    if (!unityBase || matId == 0 || !outName || maxLen == 0) return false;
    outName[0] = '\0';
    const auto pfnIdToObj = GetInstanceIDToObjectFn(unityBase);
    if (!pfnIdToObj) return false;
    __try {
        void* natMat = pfnIdToObj(&matId);
        const uintptr_t mAddr = reinterpret_cast<uintptr_t>(natMat);
        if (!IsReadableMem(mAddr, 0x38)) return false;
        const char* matName = *reinterpret_cast<const char* const*>(mAddr + 0x20);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(matName), 16)) return false;
        _snprintf_s(outName, maxLen, _TRUNCATE, "%s", matName);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outName[0] = '\0';
        return false;
    }
}

static bool SafeIsBodyMaterialIdSEH(uintptr_t unityBase, int32_t matId, bool allowSharedAsset = false) {
    if (!unityBase || matId == 0) return false;
    if (!allowSharedAsset && matId >= 0) return false;
    char matName[64] = {};
    if (!SafeReadMatNameSEH(unityBase, matId, matName, sizeof(matName))) return false;
    // Matches "car_body", "car_body(Clone)", "car_body (Instance)", "car_body(Clone) (Instance)"
    if (strncmp(matName, "car_body", 8) == 0) return true;
    return false;
}

static bool SafeResolveMatAndQueueSEH(uintptr_t unityBase, int32_t matId, void** outNatMat, int32_t* outQueue) {
    if (!unityBase || matId == 0 || !outNatMat) return false;
    *outNatMat = nullptr;
    if (outQueue) *outQueue = 2200;
    const auto pfnIdToObj = GetInstanceIDToObjectFn(unityBase);
    const auto pfnGetRq = GetMatGetRenderQueueFn(unityBase);
    if (!pfnIdToObj || !pfnGetRq) return false;
    __try {
        void* natMat = pfnIdToObj(&matId);
        if (!natMat || !IsReadableMem(reinterpret_cast<uintptr_t>(natMat), 0x58)) return false;
        *outNatMat = natMat;
        if (outQueue) *outQueue = pfnGetRq(natMat);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outNatMat = nullptr;
        return false;
    }
}

static int SafeClassifyMaterialSEH(uintptr_t unityBase, int32_t matId, int32_t* outCurQueue) {
    if (outCurQueue) *outCurQueue = 0;
    if (!unityBase || matId == 0) return kMatSkip;
    const auto pfnIdToObj = GetInstanceIDToObjectFn(unityBase);
    const auto pfnGetRq = GetMatGetRenderQueueFn(unityBase);
    if (!pfnIdToObj || !pfnGetRq) return kMatSkip;
    __try {
        void* natMat = pfnIdToObj(&matId);
        const uintptr_t mAddr = reinterpret_cast<uintptr_t>(natMat);
        if (!IsReadableMem(mAddr, 0x38)) return kMatSkip;
        const char* matName = *reinterpret_cast<const char* const*>(mAddr + 0x20);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(matName), 16)) return kMatSkip;

        if (strncmp(matName, "car_body", 8) == 0) return kMatBody;
        if (strncmp(matName, "car_glass", 9) == 0 || strncmp(matName, "shadowcaster", 12) == 0) {
            return kMatSkip;
        }
        const int32_t curQ = pfnGetRq(natMat);
        if (outCurQueue) *outCurQueue = curQ;
        // Move opaque/car-bucket parts (2200..2300, or previously moved to 2580); leave glass (2600) & decals (3000) intact
        if (curQ >= 2550 && curQ != 2580) return kMatSkip;
        return kMatStealthPart;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kMatSkip;
    }
}

static bool SafeCloneOrAssignRendererMatSEH(uintptr_t unityBase, int32_t rInstId, int32_t slotIdx,
                                            int32_t existingCloneMatId, int32_t* outNewCloneMatId) {
    if (outNewCloneMatId) *outNewCloneMatId = 0;
    if (!unityBase || rInstId == 0 || slotIdx < 0) return false;
    const auto pfnIdToObj = GetInstanceIDToObjectFn(unityBase);
    const auto pfnGetOrClone = GetOrAssignInstantiatedMatFn(unityBase);
    const auto pfnSetMat0 = GetRendererSetMat0Fn(unityBase);
    if (!pfnIdToObj || !pfnGetOrClone || !pfnSetMat0) return false;

    __try {
        void* natR = pfnIdToObj(&rInstId);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(natR), 0x1A0)) return false;

        if (existingCloneMatId < 0) {
            void* cloneMat = pfnIdToObj(&existingCloneMatId);
            if (!IsReadableMem(reinterpret_cast<uintptr_t>(cloneMat), 0x38)) return false;
            if (slotIdx == 0) {
                pfnSetMat0(natR, cloneMat);
            } else {
                void** rvt = *reinterpret_cast<void***>(natR);
                if (!IsReadableMem(reinterpret_cast<uintptr_t>(rvt), 0x110)) return false;
                reinterpret_cast<t_UP_RendererSetMatAt>(rvt[0x100 / 8])(natR, &existingCloneMatId, static_cast<uint32_t>(slotIdx));
            }
            if (outNewCloneMatId) *outNewCloneMatId = existingCloneMatId;
            return true;
        }

        void* clonedMat = pfnGetOrClone(natR, slotIdx);
        const uintptr_t cmAddr = reinterpret_cast<uintptr_t>(clonedMat);
        if (!IsReadableMem(cmAddr, 0x38)) return false;
        const int32_t newId = *reinterpret_cast<const int32_t*>(cmAddr + 0x08);
        if (newId >= 0) return false;
        if (outNewCloneMatId) *outNewCloneMatId = newId;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeRestoreRendererMatSEH(uintptr_t unityBase, int32_t rInstId, int32_t slotIdx, int32_t origAssetMatId) {
    if (!unityBase || rInstId == 0 || origAssetMatId == 0) return false;
    const auto pfnIdToObj = GetInstanceIDToObjectFn(unityBase);
    const auto pfnSetMat0 = GetRendererSetMat0Fn(unityBase);
    if (!pfnIdToObj || !pfnSetMat0) return false;
    __try {
        void* natR = pfnIdToObj(&rInstId);
        void* origMat = pfnIdToObj(&origAssetMatId);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(natR), 0x1A0) ||
            !IsReadableMem(reinterpret_cast<uintptr_t>(origMat), 0x38)) {
            return false;
        }
        if (slotIdx == 0) {
            pfnSetMat0(natR, origMat);
        } else {
            void** rvt = *reinterpret_cast<void***>(natR);
            if (!IsReadableMem(reinterpret_cast<uintptr_t>(rvt), 0x110)) return false;
            reinterpret_cast<t_UP_RendererSetMatAt>(rvt[0x100 / 8])(natR, &origAssetMatId, static_cast<uint32_t>(slotIdx));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeStepTransformNodeSEH(void* tf, void** outGo, void** outChildrenArray, int32_t* outChildCount,
                                     CarRendererRef* outRenderers, int* ioRendererCount, int maxRenderers) {
    if (!tf || !outGo || !outChildrenArray || !outChildCount) return false;
    *outGo = nullptr;
    *outChildrenArray = nullptr;
    *outChildCount = 0;
    __try {
        const uintptr_t t = reinterpret_cast<uintptr_t>(tf);
        if (!IsReadableMem(t, 0x70)) return false;
        void* go = *reinterpret_cast<void* const*>(t + 0x20);
        void* chArr = *reinterpret_cast<void* const*>(t + 0x48);
        int32_t chCnt = *reinterpret_cast<const int32_t*>(t + 0x58);
        *outGo = go;
        if (chCnt > 0 && chCnt <= 512 && IsReadableMem(reinterpret_cast<uintptr_t>(chArr), static_cast<size_t>(chCnt) * 8)) {
            *outChildrenArray = chArr;
            *outChildCount = chCnt;
        }
        if (!go || !outRenderers || !ioRendererCount || *ioRendererCount >= maxRenderers) return true;

        const uintptr_t g = reinterpret_cast<uintptr_t>(go);
        if (!IsReadableMem(g, 0x38)) return true;
        const uintptr_t compArr = *reinterpret_cast<const uintptr_t*>(g + 0x20);
        const uint32_t compCnt = *reinterpret_cast<const uint32_t*>(g + 0x30);
        if (compCnt == 0 || compCnt > 64 || !IsReadableMem(compArr, static_cast<size_t>(compCnt) * 16)) return true;

        for (uint32_t i = 0; i < compCnt && *ioRendererCount < maxRenderers; ++i) {
            const uint32_t cid = *reinterpret_cast<const uint32_t*>(compArr + i * 16);
            // Unity 6000.3: 107 = MeshRenderer, 110 = SkinnedMeshRenderer
            if (cid != 107u && cid != 110u) continue;
            const uintptr_t rnat = *reinterpret_cast<const uintptr_t*>(compArr + i * 16 + 8);
            if (!IsReadableMem(rnat, 0x1A0)) continue;
            const int32_t instId = *reinterpret_cast<const int32_t*>(rnat + 0x08);
            if (instId == 0) continue;

            CarRendererRef& entry = outRenderers[(*ioRendererCount)++];
            entry.instanceId = instId;
            entry.matCount = 0;

            const uintptr_t matPtr = *reinterpret_cast<const uintptr_t*>(rnat + 0x140);
            uint32_t mCnt = *reinterpret_cast<const uint32_t*>(rnat + 0x150);
            if (mCnt > 0 && mCnt <= 16 && IsReadableMem(matPtr, static_cast<size_t>(mCnt) * 4)) {
                entry.matCount = mCnt;
                memcpy(entry.matIds, reinterpret_cast<const void*>(matPtr), static_cast<size_t>(mCnt) * 4);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadPtr(const void* addr, void** outVal);

static void CollectCarRenderersOnMainThread(void* rootGo, std::vector<CarRendererRef>& out) {
    out.clear();
    if (!rootGo) return;
    void* rootTf = UnityBridge::GetTransformOfGameObject(rootGo);
    if (!rootTf) return;

    constexpr int kMaxNodes = 4096;
    constexpr int kMaxRenderers = 1536;
    out.resize(kMaxRenderers);
    int rCount = 0;

    std::vector<void*> stack;
    stack.reserve(512);
    std::unordered_set<void*> visited;
    visited.reserve(1024);
    stack.push_back(rootTf);

    while (!stack.empty() && static_cast<int>(visited.size()) < kMaxNodes) {
        void* tf = stack.back();
        stack.pop_back();
        if (!tf || !visited.insert(tf).second) continue;

        void* go = nullptr;
        void* chArr = nullptr;
        int32_t chCnt = 0;
        if (!SafeStepTransformNodeSEH(tf, &go, &chArr, &chCnt, out.data(), &rCount, kMaxRenderers)) continue;

        if (chArr && chCnt > 0) {
            for (int32_t ci = chCnt - 1; ci >= 0; --ci) {
                void* childTf = nullptr;
                if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(chArr) + static_cast<size_t>(ci) * 8), &childTf) && childTf) {
                    stack.push_back(childTf);
                }
            }
        }
    }
    out.resize(static_cast<size_t>(rCount));
}

// Diagnostic: log the player car hierarchy once (GameObject names + the materials of their renderers).
static void DumpCarHierarchyOnce(uintptr_t unityBase, void* rootGo) {
    void* rootTf = UnityBridge::GetTransformOfGameObject(rootGo);
    if (!rootTf) return;
    Logger::Info("CarFX car hierarchy dump begin");
    struct Node { void* tf; int depth; };
    std::vector<Node> stack;
    stack.push_back({ rootTf, 0 });
    std::unordered_set<void*> visited;
    int lines = 0;
    while (!stack.empty() && lines < 600) {
        Node nd = stack.back();
        stack.pop_back();
        if (!nd.tf || !visited.insert(nd.tf).second) continue;
        void* go = nullptr; void* chArr = nullptr; int32_t chCnt = 0;
        CarRendererRef rr[8]; int rc = 0;
        if (!SafeStepTransformNodeSEH(nd.tf, &go, &chArr, &chCnt, rr, &rc, 8)) continue;
        std::string line(static_cast<size_t>(nd.depth) * 2, ' ');
        line += UnityBridge::GetGameObjectName(go);
        for (int r = 0; r < rc; ++r) {
            line += "  [R:";
            for (uint32_t m = 0; m < rr[r].matCount; ++m) {
                char mn[48] = {};
                if (SafeReadMatNameSEH(unityBase, rr[r].matIds[m], mn, sizeof(mn))) { line += mn; line += ","; }
            }
            line += "]";
        }
        Logger::Info(line);
        ++lines;
        if (chArr && chCnt > 0) {
            for (int32_t ci = chCnt - 1; ci >= 0; --ci) {
                void* childTf = nullptr;
                if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(chArr) + static_cast<size_t>(ci) * 8), &childTf) && childTf) {
                    stack.push_back({ childTf, nd.depth + 1 });
                }
            }
        }
    }
    Logger::Info("CarFX car hierarchy dump end");
}

static int SafeCollectPartsMaterialIdsSEH(uintptr_t partsMap, int32_t* outIds, int maxIds) {
    if (!partsMap || !outIds || maxIds <= 0) return 0;
    int count = 0;
    __try {
        if (!IsReadableMem(partsMap, 0x48)) return 0;
        uintptr_t partDict = *reinterpret_cast<uintptr_t*>(partsMap + 0x40); // PartsMap.m_partMap
        if (!IsReadableMem(partDict, 0x28)) return 0;
        uintptr_t entries = *reinterpret_cast<uintptr_t*>(partDict + 0x18);
        uint32_t pCount = *reinterpret_cast<uint32_t*>(partDict + 0x20);
        if (pCount == 0 || pCount > 256 || !IsReadableMem(entries, 0x20)) return 0;
        uint32_t eLen = *reinterpret_cast<uint32_t*>(entries + 0x18);
        if (pCount > eLen) pCount = eLen;
        if (pCount == 0 || !IsReadableMem(entries, 0x20 + static_cast<size_t>(pCount) * 24)) return 0;

        for (uint32_t i = 0; i < pCount && count < maxIds; ++i) {
            uintptr_t e = entries + 0x20 + static_cast<size_t>(i) * 24;
            if (*reinterpret_cast<int32_t*>(e) < 0) continue;
            uintptr_t part = *reinterpret_cast<uintptr_t*>(e + 16);
            if (!IsReadableMem(part, 0x38)) continue;
            uintptr_t bo = *reinterpret_cast<uintptr_t*>(part + 0x30); // CarPart.m_instanceObjects
            if (!IsReadableMem(bo, 0x20)) continue;
            uintptr_t rl = *reinterpret_cast<uintptr_t*>(bo + 0x18);   // BodyObjects.rendererList
            if (!IsReadableMem(rl, 0x20)) continue;
            uintptr_t rItems = *reinterpret_cast<uintptr_t*>(rl + 0x10);
            uint32_t rCount = *reinterpret_cast<uint32_t*>(rl + 0x18);
            if (rCount == 0 || rCount > 64 || !IsReadableMem(rItems, 0x20)) continue;
            uint32_t rLen = *reinterpret_cast<uint32_t*>(rItems + 0x18);
            if (rCount > rLen) rCount = rLen;
            if (rCount == 0 || !IsReadableMem(rItems, 0x20 + static_cast<size_t>(rCount) * 8)) continue;

            for (uint32_t j = 0; j < rCount && count < maxIds; ++j) {
                uintptr_t mr = *reinterpret_cast<uintptr_t*>(rItems + 0x20 + static_cast<size_t>(j) * 8);
                if (!IsReadableMem(mr, 0x18)) continue;
                uintptr_t rnat = *reinterpret_cast<uintptr_t*>(mr + 0x10);
                if (!IsReadableMem(rnat, 0x158)) continue;
                uintptr_t matPtr = *reinterpret_cast<uintptr_t*>(rnat + 0x140);
                uint32_t matCnt = *reinterpret_cast<uint32_t*>(rnat + 0x150);
                if (matCnt == 0 || matCnt > 16 || !IsReadableMem(matPtr, static_cast<size_t>(matCnt) * 4)) continue;
                const int32_t* ids = reinterpret_cast<const int32_t*>(matPtr);
                for (uint32_t k = 0; k < matCnt && count < maxIds; ++k) {
                    if (ids[k] < 0) { // Only per-renderer instantiated clones (never shared assets)
                        outIds[count++] = ids[k];
                    }
                }
            }
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
}

// queues URUSHI itself assigns to car materials (body / parts); anything else is the game's original queue
static bool IsMovedQueue(int32_t q) {
    return q == 2510 || (q >= 2540 && q <= 2590);
}

static bool SafeSyncMaterialQueueAndDepthSEH(uintptr_t unityBase, int32_t matId, bool applyOverride,
                                             int32_t targetQueue, bool requireBodyName,
                                             int32_t savedOrigQueue, bool* outModified, int32_t* outOrigQueue) {
    if (outModified) *outModified = false;
    if (outOrigQueue) *outOrigQueue = savedOrigQueue;
    if (!unityBase || matId >= 0) return false;

    const auto pfnIdToObj = GetInstanceIDToObjectFn(unityBase);
    const auto pfnGetRq = GetMatGetRenderQueueFn(unityBase);
    const auto pfnSetRq = GetMatSetRenderQueueFn(unityBase);
    const auto pfnSetPass = GetMatSetShaderPassEnabledFn(unityBase);
    if (!pfnIdToObj || !pfnGetRq || !pfnSetRq || !pfnSetPass) return false;

    __try {
        void* natMat = pfnIdToObj(&matId);
        const uintptr_t mAddr = reinterpret_cast<uintptr_t>(natMat);
        if (!IsReadableMem(mAddr, 0x38)) return false;

        const char* matName = *reinterpret_cast<const char* const*>(mAddr + 0x20);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(matName), 16)) return false;
        if (requireBodyName && strncmp(matName, "car_body", 8) != 0) return false;

        const int32_t curQ = pfnGetRq(natMat);
        static const wchar_t kDepthOnlyW[] = L"DepthOnly";
        const UnityManagedSpanWrapper depthOnlySpan{ kDepthOnlyW, 9 };

        if (applyOverride) {
            const int32_t origQ = !IsMovedQueue(curQ) ? curQ : savedOrigQueue;
            if (outOrigQueue) *outOrigQueue = origQ;
            if (curQ != targetQueue) {
                pfnSetRq(natMat, targetQueue);
                pfnSetPass(natMat, &depthOnlySpan, false);
                if (outModified) *outModified = true;
            }
        } else {
            const int32_t restoreQ = (savedOrigQueue > 0 && !IsMovedQueue(savedOrigQueue))
                                     ? savedOrigQueue : 2200;
            if (IsMovedQueue(curQ)) {
                pfnSetRq(natMat, restoreQ);
                pfnSetPass(natMat, &depthOnlySpan, true);
                if (outModified) *outModified = true;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint32_t SafeReadNativeTexIdSEH(void* natTex) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(natTex);
    if (!IsReadableMem(addr, 0x44)) return 0;
    __try {
        return *reinterpret_cast<const uint32_t*>(addr + 0x40);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct BodyMatSlotRef {
    int32_t rendererId = 0;
    int32_t slotIdx = 0;
    int32_t origMatId = 0;
};

static void CollectBodyMaterialSlots(uintptr_t unityBase, void* rootGo, bool allowSharedAsset,
                                     std::vector<BodyMatSlotRef>& outSlots) {
    outSlots.clear();
    if (!unityBase || !rootGo) return;
    std::vector<CarRendererRef> renderers;
    CollectCarRenderersOnMainThread(rootGo, renderers);
    for (const CarRendererRef& r : renderers) {
        for (uint32_t si = 0; si < r.matCount; ++si) {
            const int32_t mid = r.matIds[si];
            if (SafeIsBodyMaterialIdSEH(unityBase, mid, allowSharedAsset)) {
                outSlots.push_back({ r.instanceId, static_cast<int32_t>(si), mid });
            }
        }
    }
}

void* UnityBridge::CreateIsolatedPackedTexture(void* origPack, bool /*packIsDef*/, void* natWhite, void* natBlack,
                                               void** outPackSrv, bool* outCopyOk) {
    if (outPackSrv) *outPackSrv = nullptr;
    if (outCopyOk) *outCopyOk = false;
    if (!s_unityBase) return nullptr;

    const uintptr_t pfnGetNatPtr = GetNativeTexPtrFn(s_unityBase);
    void* srcPack = origPack ? origPack : (natBlack ? natBlack : natWhite);
    void* clonedPack = srcPack ? CloneGameObjectDirect(srcPack) : nullptr;
    void* packSrvPtr = nullptr;
    bool copyOk = false;

    if (clonedPack) {
        if (origPack) {
            copyOk = SafeCopyTextureFullSEH(s_unityBase, origPack, clonedPack);
        }
        SafeCallGetNativeTexPtr(pfnGetNatPtr, clonedPack, &packSrvPtr);
        if (!packSrvPtr) {
            DestroyObjectDirect(clonedPack, 0.0f);
            clonedPack = nullptr;
            void* fallbackSrc = (srcPack == natBlack && natWhite) ? natWhite : (natBlack ? natBlack : natWhite);
            if (fallbackSrc) {
                clonedPack = CloneGameObjectDirect(fallbackSrc);
                if (clonedPack) {
                    copyOk = SafeCopyTextureFullSEH(s_unityBase, fallbackSrc, clonedPack);
                    SafeCallGetNativeTexPtr(pfnGetNatPtr, clonedPack, &packSrvPtr);
                }
            }
        }
    }

    if (!clonedPack || !packSrvPtr) {
        if (clonedPack) DestroyObjectDirect(clonedPack, 0.0f);
        return nullptr;
    }

    if (outPackSrv) *outPackSrv = packSrvPtr;
    if (outCopyOk) *outCopyOk = copyOk;
    return clonedPack;
}

void UnityBridge::UpdateCarFXPlayerTexturesOnMainThread(const std::vector<LiveCarInfo>& liveSnapshot) {
    const uintptr_t unityBase = s_unityBase;
    if (!unityBase) return;

    static ULONGLONG s_nextUpdateMs = 0;
    static bool s_prevWantTransparent = false;
    static bool s_prevWantStealth = false;
    static void* s_prevStealthRootGo = nullptr;
    static void* s_playerIsoRootGo = nullptr;
    static void* s_playerIsoPackTex = nullptr;
    static void* s_playerIsoPackSrv = nullptr;
    static uint32_t s_playerIsoPackId = 0;
    static void* s_loggedDefRootGo = nullptr;
    static std::unordered_map<int32_t, int32_t> s_savedBodyMatQueues;
    static std::unordered_map<int32_t, int32_t> s_savedStealthMatQueues;
    static std::unordered_map<int32_t, int32_t> s_stealthAssetToClone;
    static std::vector<SavedStealthMatSwap> s_savedStealthSwaps;

    auto releasePlayerIsoPackTex = [&]() {
        if (s_playerIsoPackTex) {
            DestroyObjectDirect(s_playerIsoPackTex, 0.0f);
            s_playerIsoPackTex = nullptr;
        }
        s_playerIsoPackSrv = nullptr;
        s_playerIsoPackId = 0;
        s_playerIsoRootGo = nullptr;
    };

    const bool wantTransparent = CarFX::ShouldBypassDepthPrePass();
    const bool wantStealth = CarFX::ShouldHideCarInternals();
    const bool stealthByOrder = Settings::GetBool("carfx", "stealth_order", false);   // experimental: body-first draw order
    CarFX::SetStealthDepthWrite(stealthByOrder && wantStealth);
    const bool modeChanged = (wantTransparent != s_prevWantTransparent);
    const bool stealthChanged = (wantStealth != s_prevWantStealth);

    const ULONGLONG nowMs = GetTickCount64();
    if (!modeChanged && !stealthChanged && nowMs < s_nextUpdateMs) return;

    auto restoreAllStealthMaterials = [&]() {
        if (s_savedStealthMatQueues.empty() && s_savedStealthSwaps.empty() && s_stealthAssetToClone.empty()) return;
        for (const auto& kv : s_savedStealthMatQueues) {
            SafeSyncMaterialQueueAndDepthSEH(unityBase, kv.first, false, 2200, false, kv.second, nullptr, nullptr);
        }
        s_savedStealthMatQueues.clear();
        for (const auto& sw : s_savedStealthSwaps) {
            SafeRestoreRendererMatSEH(unityBase, sw.rendererInstId, sw.slotIdx, sw.origAssetMatId);
        }
        s_savedStealthSwaps.clear();
        s_stealthAssetToClone.clear();
        s_prevStealthRootGo = nullptr;
        Logger::Info("CarFX restored stealth player car part materials -> queue 2200 + DepthOnly ON");
    };

    if (!NativeCarX::IsInSession()) {
        if (!s_savedBodyMatQueues.empty()) {
            for (const auto& kv : s_savedBodyMatQueues) {
                SafeSyncMaterialQueueAndDepthSEH(unityBase, kv.first, false, 2200, true, kv.second, nullptr, nullptr);
            }
            s_savedBodyMatQueues.clear();
            Logger::Info("CarFX session ended: restored car_body(Clone) renderQueues -> 2200 + DepthOnly ON");
        }
        restoreAllStealthMaterials();
        s_prevStealthRootGo = nullptr;
        s_loggedDefRootGo = nullptr;
        releasePlayerIsoPackTex();
        s_prevWantTransparent = false;
        s_prevWantStealth = false;
        s_nextUpdateMs = GetTickCount64() + 250;
        CarFX::TargetTexturesInput empty{};
        CarFX::SetPlayerTargetTextures(CarFX::kTargetBody, empty);
        return;
    }

    if (!wantTransparent && !s_savedBodyMatQueues.empty()) {
        for (const auto& kv : s_savedBodyMatQueues) {
            SafeSyncMaterialQueueAndDepthSEH(unityBase, kv.first, false, 2200, true, kv.second, nullptr, nullptr);
        }
        s_savedBodyMatQueues.clear();
        Logger::Info("CarFX restored car_body(Clone) renderQueues -> 2200 + DepthOnly ON");
    }
    {
        static bool s_prevOrderMode = false;
        if (!wantStealth || s_prevOrderMode != stealthByOrder) {
            restoreAllStealthMaterials();
            if (s_prevOrderMode != stealthByOrder) s_prevOrderMode = stealthByOrder;
        }
    }
    s_prevWantTransparent = wantTransparent;
    s_prevWantStealth = wantStealth;

    void* rootGo = nullptr;
    for (const auto& c : liveSnapshot) {
        if (c.isLocalPlayer) {
            if (c.rootGo) {
                rootGo = c.rootGo;
            } else if (c.rootTf) {
                SafeCheckNativeRootGo(c.rootTf, &rootGo);
            }
            break;
        }
    }
    if (!rootGo) {
        void* rootTf = nullptr;
        FindPlayerCarModel(&rootTf);
        if (rootTf) SafeCheckNativeRootGo(rootTf, &rootGo);
    }
    if (!rootGo) {
        static ULONGLONG s_lastNoRootMs = 0;
        if (nowMs - s_lastNoRootMs > 2000) {
            s_lastNoRootMs = nowMs;
            Logger::Warn("UpdateCarFX: rootGo is null (liveSnapshot size=" + std::to_string(liveSnapshot.size()) + ")");
        }
        s_nextUpdateMs = GetTickCount64() + 250;
        return;
    }

    {
        static void* s_dumpedRoot = nullptr;
        if (s_dumpedRoot != rootGo) {
            s_dumpedRoot = rootGo;
            DumpCarHierarchyOnce(unityBase, rootGo);
        }
    }

    if (s_playerIsoRootGo && s_playerIsoRootGo != rootGo) {
        releasePlayerIsoPackTex();
    }
    if (wantStealth && s_prevStealthRootGo && s_prevStealthRootGo != rootGo) {
        restoreAllStealthMaterials();
    }

    void* s_natWhite = nullptr;
    void* s_natBlack = nullptr;
    void* s_natNormal = nullptr;
    GetBuiltinNativeTextures(unityBase, &s_natWhite, &s_natBlack, &s_natNormal);

    // Internals target: publish the albedo textures of every non-body, non-wheel part so CarFX can recognise their draws.
    {
        std::vector<void*> intTex;
        if (CarFX::IsTargetActive(CarFX::kTargetInternals)) {
            std::vector<CarRendererRef> rs;
            CollectCarRenderersOnMainThread(rootGo, rs);
            const uint32_t partMask = CarFX::GetInternalsPartMask();
            for (const CarRendererRef& r : rs) {
                for (uint32_t si = 0; si < r.matCount; ++si) {
                    const int32_t mid = r.matIds[si];
                    char mn[64] = {};
                    if (!mid || !SafeReadMatNameSEH(unityBase, mid, mn, sizeof(mn))) continue;
                    const uint32_t part = InternalsPartOfMaterial(mn);
                    if (part == 0 || !(partMask & part)) continue;   // body / glass / shadow proxies, or a group switched off
                    void* natMat = SafeIdToObjectSEH(unityBase, mid);
                    void* alb = SafeGetMatTextureSEH(unityBase, natMat, 380);
                    if (alb && !IsDefaultNativeTex(alb, s_natWhite, s_natBlack, s_natNormal)) {
                        void* d3dPtr = nullptr;   // Unity Texture object -> the D3D11 view/resource behind it
                        SafeCallGetNativeTexPtr(GetNativeTexPtrFn(unityBase), alb, &d3dPtr);
                        if (d3dPtr) intTex.push_back(d3dPtr);
                    }
                }
            }
        }
        CarFX::SetInternalsTextures(intTex);
    }

    void* natAlb = nullptr;
    void* natPack = nullptr;
    uint32_t albId = 0, packId = 0;
    uintptr_t partsMap = 0;
    const bool hasMasterTex = SafeExtractMasterMatTexPairSEH(rootGo, 0x8C255AA8u /* BodyTypeID */, &natAlb, &natPack, &albId, &packId, &partsMap);

    // Stealth places car_body at 2510 ([4] right after DrawSky) and all non-glass car parts at 2580 ([7] after car_body, before car_glass=2600),
    // so hardware Depth Test naturally hides engine/chassis behind cloaked body while keeping wheels, arms, lights & interior-through-glass visible!
    const int32_t bodyTargetQueue = (wantStealth && stealthByOrder)
        ? std::clamp(atoi(Settings::GetString("carfx", "body_queue", "2510").c_str()), 2300, 2579) : 2590;
    std::unordered_set<int32_t> bodyMatIdSet;
    if (partsMap && (wantTransparent || wantStealth || !s_savedBodyMatQueues.empty())) {
        int32_t matIds[512] = {};
        const int nMat = SafeCollectPartsMaterialIdsSEH(partsMap, matIds, 512);
        int modifiedCount = 0;
        for (int i = 0; i < nMat; ++i) {
            const int32_t mid = matIds[i];
            if (wantTransparent || !s_savedBodyMatQueues.empty()) {
                int32_t origQ = 2200;
                auto it = s_savedBodyMatQueues.find(mid);
                if (it != s_savedBodyMatQueues.end()) origQ = it->second;
                bool modified = false;
                int32_t outOrigQ = origQ;
                if (SafeSyncMaterialQueueAndDepthSEH(unityBase, mid, wantTransparent, bodyTargetQueue, true, origQ, &modified, &outOrigQ)) {
                    bodyMatIdSet.insert(mid);
                    if (wantTransparent) {
                        s_savedBodyMatQueues.emplace(mid, outOrigQ);
                    } else {
                        s_savedBodyMatQueues.erase(mid);
                    }
                    if (modified) modifiedCount++;
                }
            } else if (SafeIsBodyMaterialIdSEH(unityBase, mid)) {
                bodyMatIdSet.insert(mid);
            }
        }
        if (modifiedCount > 0) {
            Logger::Info("CarFX updated " + std::to_string(modifiedCount) +
                         (wantTransparent ? (" car_body(Clone) mats -> queue " + std::to_string(bodyTargetQueue) + " + DepthOnly OFF")
                                          : " car_body(Clone) mats -> queue 2200 + DepthOnly ON"));
        }
    }

    if (wantStealth && stealthByOrder) {
        s_prevStealthRootGo = rootGo;
        std::vector<CarRendererRef> renderers;
        CollectCarRenderersOnMainThread(rootGo, renderers);

        std::unordered_set<int32_t> skipMatIds = bodyMatIdSet;
        int movedCount = 0;
        int clonedAssetCount = 0;

        for (const CarRendererRef& r : renderers) {
            for (uint32_t slotIdx = 0; slotIdx < r.matCount; ++slotIdx) {
                const int32_t mid = r.matIds[slotIdx];
                if (mid == 0 || skipMatIds.count(mid)) continue;

                int32_t targetMid = mid;
                if (mid > 0) {
                    auto itClone = s_stealthAssetToClone.find(mid);
                    if (itClone != s_stealthAssetToClone.end()) {
                        if (SafeCloneOrAssignRendererMatSEH(unityBase, r.instanceId, static_cast<int32_t>(slotIdx), itClone->second, nullptr)) {
                            s_savedStealthSwaps.push_back({ r.instanceId, static_cast<int32_t>(slotIdx), mid });
                        }
                        continue;
                    }
                    int32_t curQ = 0;
                    const int cls = SafeClassifyMaterialSEH(unityBase, mid, &curQ);
                    if (cls != kMatStealthPart) {
                        skipMatIds.insert(mid);
                        continue;
                    }
                    int32_t newCloneId = 0;
                    if (!SafeCloneOrAssignRendererMatSEH(unityBase, r.instanceId, static_cast<int32_t>(slotIdx), 0, &newCloneId) || newCloneId >= 0) {
                        continue;
                    }
                    s_stealthAssetToClone.emplace(mid, newCloneId);
                    s_savedStealthSwaps.push_back({ r.instanceId, static_cast<int32_t>(slotIdx), mid });
                    targetMid = newCloneId;
                    clonedAssetCount++;
                } else {
                    int32_t curQ = 0;
                    const int cls = SafeClassifyMaterialSEH(unityBase, mid, &curQ);
                    if (cls == kMatBody) {
                        bodyMatIdSet.insert(mid);
                        skipMatIds.insert(mid);
                        SafeSyncMaterialQueueAndDepthSEH(unityBase, mid, wantTransparent, bodyTargetQueue, true, 2200, nullptr, nullptr);
                        continue;
                    }
                    if (cls != kMatStealthPart) {
                        skipMatIds.insert(mid);
                        continue;
                    }
                }

                int32_t origQ = 2200;
                auto itSavedQ = s_savedStealthMatQueues.find(targetMid);
                if (itSavedQ != s_savedStealthMatQueues.end()) origQ = itSavedQ->second;
                bool modified = false;
                int32_t outOrigQ = origQ;
                if (SafeSyncMaterialQueueAndDepthSEH(unityBase, targetMid, true, 2580, false, origQ, &modified, &outOrigQ)) {
                    s_savedStealthMatQueues.emplace(targetMid, outOrigQ);
                    if (modified) movedCount++;
                }
            }
        }
        if (movedCount > 0 || clonedAssetCount > 0) {
            Logger::Info("CarFX stealth staged " + std::to_string(movedCount) +
                         " player part mats -> queue 2580 + DepthOnly OFF (isolated " +
                         std::to_string(clonedAssetCount) + " shared assets)");
        }
    }

    if (wantStealth && !stealthByOrder) {
        // Hide the car's internals (engine, suspension, interior...) from the colour pass: every non-wheel part is given the
        // "shadowcaster_only" material the tyres already use for their shadow proxies. They keep casting shadows but draw no
        // colour, so the scene snapshot taken for the body shader holds the real road behind the car.
        s_prevStealthRootGo = rootGo;
        std::vector<CarRendererRef> renderers;
        CollectCarRenderersOnMainThread(rootGo, renderers);

        int32_t shadowMat = 0;
        for (const CarRendererRef& r : renderers) {
            for (uint32_t slotIdx = 0; slotIdx < r.matCount && !shadowMat; ++slotIdx) {
                char mn[64] = {};
                if (SafeReadMatNameSEH(unityBase, r.matIds[slotIdx], mn, sizeof(mn)) && strcmp(mn, "shadowcaster_only") == 0) {
                    shadowMat = r.matIds[slotIdx];
                }
            }
            if (shadowMat) break;
        }

        static const char* const kKeep[] = { "rim", "yolo", "defaultDiffuse", "BrakeRotor", "Caliper", "wheel_motion", "shadowcaster", "light", "lamp" };
        int hidden = 0;
        if (shadowMat) {
            for (const CarRendererRef& r : renderers) {
                for (uint32_t slotIdx = 0; slotIdx < r.matCount; ++slotIdx) {
                    const int32_t mid = r.matIds[slotIdx];
                    if (mid == 0 || mid == shadowMat) continue;
                    char mn[64] = {};
                    if (!SafeReadMatNameSEH(unityBase, mid, mn, sizeof(mn))) continue;
                    bool keep = false;
                    for (const char* k : kKeep) {
                        if (strstr(mn, k)) { keep = true; break; }
                    }
                    if (keep) continue;
                    int32_t curQ = 0;
                    if (SafeClassifyMaterialSEH(unityBase, mid, &curQ) != kMatStealthPart) continue;
                    if (SafeRestoreRendererMatSEH(unityBase, r.instanceId, static_cast<int32_t>(slotIdx), shadowMat)) {
                        s_savedStealthSwaps.push_back({ r.instanceId, static_cast<int32_t>(slotIdx), mid });
                        ++hidden;
                    }
                }
            }
        }
        if (hidden > 0) {
            Logger::Info("CarFX stealth: hid " + std::to_string(hidden) + " car internal material slots (shadow-only proxy)");
        }
    }

    bool albIsDef = IsDefaultNativeTex(natAlb, s_natWhite, s_natBlack, s_natNormal);
    bool packIsDef = IsDefaultNativeTex(natPack, s_natWhite, s_natBlack, s_natNormal);
    void* forcedPackSrv = nullptr;

    const bool hasIso = (s_playerIsoPackTex != nullptr && s_playerIsoPackSrv != nullptr);
    if (!albIsDef && !hasIso) {
        if (natPack != s_playerIsoPackTex) {
            releasePlayerIsoPackTex();
        }
    } else {
        std::vector<BodyMatSlotRef> bodySlots;
        CollectBodyMaterialSlots(unityBase, rootGo, true, bodySlots);

        if (bodySlots.empty()) {
            if (s_loggedDefRootGo != rootGo) {
                s_loggedDefRootGo = rootGo;
                std::vector<CarRendererRef> allR;
                CollectCarRenderersOnMainThread(rootGo, allR);
                std::unordered_set<int32_t> seenIds;
                std::string sampleNames;
                int shown = 0;
                for (const CarRendererRef& r : allR) {
                    for (uint32_t si = 0; si < r.matCount && shown < 10; ++si) {
                        const int32_t mid = r.matIds[si];
                        if (!mid || !seenIds.insert(mid).second) continue;
                        char mn[48] = {};
                        if (SafeReadMatNameSEH(unityBase, mid, mn, sizeof(mn))) {
                            if (!sampleNames.empty()) sampleNames += ", ";
                            sampleNames += mn;
                            sampleNames += (mid < 0 ? "(C)" : "(A)");
                            shown++;
                        }
                    }
                }
                Logger::Warn("CarFX player fallback: 0 car_body slots found on rootGo (masterOk=" +
                             std::to_string(hasMasterTex ? 1 : 0) + ", renderers=" + std::to_string(allR.size()) +
                             ", mats=[" + sampleNames + "])");
            }
        } else {
            void* firstMat = nullptr;
            void* matAlb = nullptr;
            void* matPack = nullptr;
            if (SafeResolveMatAndQueueSEH(unityBase, bodySlots[0].origMatId, &firstMat, nullptr) && firstMat) {
                matAlb = SafeGetMatTextureSEH(unityBase, firstMat, 380);
                matPack = SafeGetMatTextureSEH(unityBase, firstMat, 1370);
            }

            const bool matAlbIsReal = !IsDefaultNativeTex(matAlb, s_natWhite, s_natBlack, s_natNormal) && (matAlb != s_playerIsoPackTex);
            const bool matPackIsReal = !IsDefaultNativeTex(matPack, s_natWhite, s_natBlack, s_natNormal);
            if (matAlbIsReal) {
                natAlb = matAlb;
                albId = SafeReadNativeTexIdSEH(natAlb);
                albIsDef = false;
            }
            if (matPackIsReal && !hasIso) {
                natPack = matPack;
                packId = SafeReadNativeTexIdSEH(natPack);
                packIsDef = false;
                releasePlayerIsoPackTex();
            }

            if ((albIsDef && packIsDef) || hasIso) {
                bool copyOk = false;
                if (!hasIso) {
                    releasePlayerIsoPackTex();
                    void* isoSrv = nullptr;
                    void* isoTex = CreateIsolatedPackedTexture(matPack, true, s_natWhite, s_natBlack, &isoSrv, &copyOk);
                    if (isoTex && isoSrv) {
                        s_playerIsoPackTex = isoTex;
                        s_playerIsoPackSrv = isoSrv;
                        s_playerIsoPackId = SafeReadNativeTexIdSEH(isoTex);
                        s_playerIsoRootGo = rootGo;
                    }
                }

                if (s_playerIsoPackTex && s_playerIsoPackSrv) {
                    std::unordered_map<int32_t, int32_t> assetToClone;
                    std::unordered_set<int32_t> updatedMats;
                    int assignedSlots = 0;
                    for (const BodyMatSlotRef& bs : bodySlots) {
                        int32_t targetMid = bs.origMatId;
                        if (targetMid > 0) {
                            auto itC = assetToClone.find(targetMid);
                            if (itC != assetToClone.end()) {
                                int32_t outId = 0;
                                if (SafeCloneOrAssignRendererMatSEH(unityBase, bs.rendererId, bs.slotIdx, itC->second, &outId)) {
                                    targetMid = itC->second;
                                }
                            } else {
                                int32_t newMid = 0;
                                if (SafeCloneOrAssignRendererMatSEH(unityBase, bs.rendererId, bs.slotIdx, 0, &newMid) && newMid < 0) {
                                    assetToClone.emplace(bs.origMatId, newMid);
                                    targetMid = newMid;
                                }
                            }
                        }
                        if (targetMid != 0 && updatedMats.insert(targetMid).second) {
                            void* natM = nullptr;
                            if (SafeResolveMatAndQueueSEH(unityBase, targetMid, &natM, nullptr) && natM) {
                                void* curPack = SafeGetMatTextureSEH(unityBase, natM, 1370);
                                if (curPack != s_playerIsoPackTex) {
                                    if (SafeSetMatTextureSEH(unityBase, natM, 1370, s_playerIsoPackTex)) {
                                        assignedSlots++;
                                    }
                                } else {
                                    assignedSlots++;
                                }
                            }
                            if (targetMid < 0 && wantTransparent) {
                                int32_t origQ = 2200, outOrigQ = 2200;
                                auto itQ = s_savedBodyMatQueues.find(targetMid);
                                if (itQ != s_savedBodyMatQueues.end()) origQ = itQ->second;
                                if (SafeSyncMaterialQueueAndDepthSEH(unityBase, targetMid, true, bodyTargetQueue, true, origQ, nullptr, &outOrigQ)) {
                                    s_savedBodyMatQueues.emplace(targetMid, outOrigQ);
                                }
                            }
                        }
                    }
                    natPack = s_playerIsoPackTex;
                    packId = s_playerIsoPackId;
                    forcedPackSrv = s_playerIsoPackSrv;
                    packIsDef = false;
                }

                if (s_loggedDefRootGo != rootGo) {
                    s_loggedDefRootGo = rootGo;
                    char dbg[256];
                    _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
                                "CarFX player default-tex fallback: bodySlots=%zu origPack=%p(0x%X) -> isoTex=%p srv=%p id=0x%X copy=%d packDef=%d",
                                bodySlots.size(), matPack, SafeReadNativeTexIdSEH(matPack),
                                s_playerIsoPackTex, s_playerIsoPackSrv, s_playerIsoPackId,
                                (int)copyOk, (int)packIsDef);
                    Logger::Info(dbg);
                }
            }
        }
    }

    const uintptr_t pfnGetNatPtr = GetNativeTexPtrFn(unityBase); // UnityEngine.Texture::GetNativeTexturePtr
    CarFX::TargetTexturesInput in{};
    in.albedoTexId = albId;
    in.packedTexId = packId;
    in.albedoIsDefault = albIsDef;
    in.packedIsDefault = packIsDef;

    if (natAlb) SafeCallGetNativeTexPtr(pfnGetNatPtr, natAlb, &in.nativeAlbedoPtr);
    if (forcedPackSrv) {
        in.nativePackedPtr = forcedPackSrv;
    } else if (natPack) {
        SafeCallGetNativeTexPtr(pfnGetNatPtr, natPack, &in.nativePackedPtr);
    }
    in.valid = (!in.albedoIsDefault || !in.packedIsDefault) &&
               ((in.nativeAlbedoPtr != nullptr) || (in.nativePackedPtr != nullptr));

    static ULONGLONG s_lastDbgMs = 0;
    if (nowMs - s_lastDbgMs > 2000) {
        s_lastDbgMs = nowMs;
        char dbg[300];
        _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
            "UpdateCarFX: rootGo=%p natAlb=%p natPack=%p pfnGetNatPtr=0x%llX albPtr=%p packPtr=%p albDef=%d packDef=%d valid=%d",
            rootGo, natAlb, natPack, (unsigned long long)pfnGetNatPtr, in.nativeAlbedoPtr, in.nativePackedPtr, (int)albIsDef, (int)packIsDef, (int)in.valid);
        Logger::Info(dbg);
    }

    CarFX::SetPlayerTargetTextures(CarFX::kTargetBody, in);
    s_nextUpdateMs = GetTickCount64() + 250;
}

static bool SafeReadPtr(const void* addr, void** outVal) {
    return SafeReadBytes(addr, outVal, sizeof(void*));
}

static bool SafeReadU32(const void* addr, uint32_t* outVal) {
    return SafeReadBytes(addr, outVal, sizeof(uint32_t));
}

static bool SafeReadU64(const void* addr, uint64_t* outVal) {
    return SafeReadBytes(addr, outVal, sizeof(uint64_t));
}

static bool SafeReadCStr(const void* addr, char* buf, size_t maxLen) {
    __try {
        if (!addr || (uintptr_t)addr < 0x10000 || !buf || maxLen == 0) return false;
        const char* src = reinterpret_cast<const char*>(addr);
        size_t i = 0;
        for (; i < maxLen - 1; ++i) {
            char ch = src[i];
            buf[i] = ch;
            if (ch == '\0') return true;
        }
        buf[maxLen - 1] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (buf && maxLen > 0) buf[0] = '\0';
        return false;
    }
}

static void* SafeCallFind(void* fnPtr, const char* name) {
    __try {
        if (!fnPtr || !name) return nullptr;
        typedef void* (*t_Fn)(const char*);
        return ((t_Fn)fnPtr)(name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void* SafeCallClone(void* fnPtr, void* nativeGo) {
    __try {
        if (!fnPtr || !nativeGo) return nullptr;
        typedef void* (*t_Fn)(void*);
        return ((t_Fn)fnPtr)(nativeGo);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void* SafeCallGCHandleTarget(void* fnPtr, uintptr_t handle) {
    __try {
        if (!fnPtr || !handle) return nullptr;
        typedef void* (*t_Fn)(uintptr_t);
        return ((t_Fn)fnPtr)(handle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool SafeCallDestroy(void* fnPtr, void* nativeObj, float t) {
    __try {
        if (!fnPtr || !nativeObj) return false;
        typedef void (*t_Fn)(void*, float);
        ((t_Fn)fnPtr)(nativeObj, t);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallSetActive(void* fnPtr, void* nativeGo, bool active) {
    __try {
        if (!fnPtr || !nativeGo) return false;
        typedef void (*t_Fn)(void*, bool);
        ((t_Fn)fnPtr)(nativeGo, active);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallSetLayer(void* fnPtr, void* nativeGo, int layer) {
    __try {
        if (!fnPtr || !nativeGo) return false;
        typedef void (*t_Fn)(void*, int);
        ((t_Fn)fnPtr)(nativeGo, layer);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallGetVec3(void* fnPtr, void* nativeTf, UnityVector3* outVec) {
    __try {
        if (!fnPtr || !nativeTf || !outVec) return false;
        typedef void (*t_Fn)(void*, UnityVector3*);
        ((t_Fn)fnPtr)(nativeTf, outVec);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallSetVec3(void* fnPtr, void* nativeTf, const UnityVector3* inVec) {
    __try {
        if (!fnPtr || !nativeTf || !inVec) return false;
        typedef void (*t_Fn)(void*, const UnityVector3*);
        ((t_Fn)fnPtr)(nativeTf, inVec);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallGetVec4(void* fnPtr, void* nativeTf, UnityVector4* outVec) {
    __try {
        if (!fnPtr || !nativeTf || !outVec) return false;
        typedef void (*t_Fn)(void*, UnityVector4*);
        ((t_Fn)fnPtr)(nativeTf, outVec);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallSetVec4(void* fnPtr, void* nativeTf, const UnityVector4* inVec) {
    __try {
        if (!fnPtr || !nativeTf || !inVec) return false;
        typedef void (*t_Fn)(void*, const UnityVector4*);
        ((t_Fn)fnPtr)(nativeTf, inVec);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ================= Math & String Helpers =================

static float VecDot(const UnityVector3& a, const UnityVector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static UnityVector3 VecNormalize(UnityVector3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f) {
        return { v.x / len, v.y / len, v.z / len };
    }
    return { 0.0f, 1.0f, 0.0f };
}

static UnityVector4 MatrixToQuaternion(const float* m) {
    float m00 = m[0], m10 = m[1], m20 = m[2];
    float m01 = m[4], m11 = m[5], m21 = m[6];
    float m02 = m[8], m12 = m[9], m22 = m[10];

    float trace = m00 + m11 + m22;
    UnityVector4 q;
    if (trace > 0.0f) {
        float s = 0.5f / sqrtf(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (m21 - m12) * s;
        q.y = (m02 - m20) * s;
        q.z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        float s = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static UnityVector4 QuatMul(const UnityVector4& a, const UnityVector4& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

static UnityVector4 QuatConj(const UnityVector4& q) {
    return { -q.x, -q.y, -q.z, q.w };
}

static UnityVector4 QuatFromAxisY(float angleRad) {
    float h = angleRad * 0.5f;
    return { 0.0f, sinf(h), 0.0f, cosf(h) };
}

static UnityVector4 ReconstructSpinFreeKnuckle(const float* wheelMat, const float* bodyMat) {
    UnityVector3 right = VecNormalize({ wheelMat[0], wheelMat[1], wheelMat[2] });
    UnityVector3 cUp = { bodyMat[4], bodyMat[5], bodyMat[6] };
    float d = VecDot(cUp, right);
    UnityVector3 up = VecNormalize({ cUp.x - d * right.x, cUp.y - d * right.y, cUp.z - d * right.z });
    UnityVector3 fwd = VecNormalize({
        right.y * up.z - right.z * up.y,
        right.z * up.x - right.x * up.z,
        right.x * up.y - right.y * up.x
    });

    float m[16] = {
        right.x, right.y, right.z, 0.0f,
        up.x,    up.y,    up.z,    0.0f,
        fwd.x,   fwd.y,   fwd.z,   0.0f,
        0.0f,    0.0f,    0.0f,    1.0f
    };
    return MatrixToQuaternion(m);
}

static std::string StripCarSuffix(const std::string& name) {
    size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return name;
    for (size_t i = pos + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return name;
    }
    return name.substr(0, pos);
}

static void EstimateCarExtents(const float* bodyMat, const float wheelMats[4][16], float& outHalfW, float& outHalfL) {
    outHalfW = 0.96f;
    outHalfL = 2.25f;
    if (!bodyMat || !wheelMats) return;

    float wMag = fabsf(wheelMats[0][12]) + fabsf(wheelMats[1][12]) + fabsf(wheelMats[2][12]) + fabsf(wheelMats[3][12]);
    if (wMag < 0.01f) return;

    UnityVector3 right = VecNormalize({ bodyMat[0], bodyMat[1], bodyMat[2] });
    UnityVector3 fwd   = VecNormalize({ bodyMat[8], bodyMat[9], bodyMat[10] });

    UnityVector3 lr = { wheelMats[1][12] - wheelMats[0][12], wheelMats[1][13] - wheelMats[0][13], wheelMats[1][14] - wheelMats[0][14] };
    float track = fabsf(VecDot(lr, right));
    if (track > 1.0f && track < 2.6f) {
        outHalfW = track * 0.5f + 0.20f;
    }

    UnityVector3 fr = {
        0.5f * (wheelMats[0][12] + wheelMats[1][12] - wheelMats[2][12] - wheelMats[3][12]),
        0.5f * (wheelMats[0][13] + wheelMats[1][13] - wheelMats[2][13] - wheelMats[3][13]),
        0.5f * (wheelMats[0][14] + wheelMats[1][14] - wheelMats[2][14] - wheelMats[3][14])
    };
    float wb = fabsf(VecDot(fr, fwd));
    if (wb > 1.6f && wb < 4.0f) {
        outHalfL = wb * 0.5f + 0.85f;
    }
}

// ================= Initialization & Main Thread Hook =================

bool UnityBridge::Init() {
    if (s_initialized) return true;

    HMODULE hUnity = GetModuleHandleW(L"UnityPlayer.dll");
    if (!hUnity) {
        Logger::Warn("UnityPlayer.dll not yet loaded, will retry...");
        return false;
    }

    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), hUnity, &mi, sizeof(mi));
    s_unityBase = reinterpret_cast<uintptr_t>(hUnity);
    s_unitySize = mi.SizeOfImage;

    // Dynamically resolve Unity icall pointers via pattern signatures with safe fallbacks
    pfn_Find                     = reinterpret_cast<t_Find>(PatternScanner::FindOrFallback(hUnity, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 80 39", 0x669CE0, "Find"));
    pfn_CloneSingle              = reinterpret_cast<t_Internal_CloneSingle>(PatternScanner::FindOrFallback(hUnity, "40 53 48 83 EC 20 E8 ? ? ? ? 48 8B D8 48 85 C0 74 20 48 8B 40 18 48 85 C0 75 17 48 8B D3 48 8D 4C 24 30 E8 ? ? ? ? 48 8B 43 18 48 83 C4 20 5B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC 40 53 48 83 EC 20 8B 12 48 8B D9", 0xC4230, "CloneSingle"));
    pfn_Destroy                  = reinterpret_cast<t_Destroy>(PatternScanner::FindOrFallback(hUnity, "40 53 48 83 EC 30 33 DB 0F 29 74 24 20 0F 28 F1 48 85 C9", 0xC3A30, "Destroy"));
    pfn_SetActive                = reinterpret_cast<t_SetActive>(PatternScanner::FindOrFallback(hUnity, "40 57 48 83 EC 20 48 8B F9 84 D2 74 0A 48 83 C4 20 5F E9", 0xBDFC0, "SetActive"));
    pfn_SetLayer                 = reinterpret_cast<t_SetLayer>(PatternScanner::FindOrFallback(hUnity, "4C 8B DC 57 48 81 EC B0 00 00 00 48 8B F9 39 51", 0x2F9270, "SetLayer"));
    pfn_GameObject_get_transform = reinterpret_cast<t_GameObject_get_transform>(PatternScanner::FindOrFallback(hUnity, "40 53 48 83 EC 20 48 8B 41 20 48 8B 58 08 48 83 7B 18 00", 0xBDEC0, "GameObject_get_transform"));

    // Transform property accessors in UnityPlayer (cluster: 0xCCA20..0xCCB90)
    // 1. Unique 46-byte pattern for Transform_Get_Custom_PropRotation (0xCCB40)
    uintptr_t getRotVa = PatternScanner::FindOrFallback(
        hUnity,
        "40 53 48 83 EC 30 48 83 79 38 00 48 8B DA 75 19 8B 41 08 89 44 24 28 48 8D 41 28 48 89 44 24 20 0F 10 44 24 20 0F 11 41 38 4C 8D 41 28 48",
        0xCCB40,
        "Transform_get_rotation"
    );
    pfn_Transform_get_rotation = reinterpret_cast<t_Transform_get_rotation>(getRotVa);
    pfn_Transform_set_rotation = reinterpret_cast<t_Transform_set_rotation>(getRotVa + 0x50);
    pfn_Transform_get_position = reinterpret_cast<t_Transform_get_position>(getRotVa - 0x120);
    pfn_Transform_set_position = reinterpret_cast<t_Transform_set_position>(getRotVa - 0xF0);

    // 2. Cross-verify with anchor pattern for Transform_Get_Custom_PropPosition (0xCCA20)
    uintptr_t anchorGetPos = PatternScanner::Find(
        hUnity,
        "88 81 F2 00 00 00 C3 CC CC CC CC E9 ? ? ? ? CC CC CC CC CC CC CC CC CC CC CC 40 53 48 83 EC 30 48 8B DA 48 8D 54 24 20"
    );
    if (anchorGetPos) {
        uintptr_t posVa = anchorGetPos + 27;
        pfn_Transform_get_position = reinterpret_cast<t_Transform_get_position>(posVa);
        pfn_Transform_set_position = reinterpret_cast<t_Transform_set_position>(posVa + 0x30);
    }
    char tfDbg[256];
    _snprintf_s(tfDbg, sizeof(tfDbg), _TRUNCATE,
                "Transform icalls resolved: get_pos=%p set_pos=%p get_rot=%p set_rot=%p",
                pfn_Transform_get_position, pfn_Transform_set_position,
                pfn_Transform_get_rotation, pfn_Transform_set_rotation);
    Logger::Info(tfDbg);

    pfn_GetMainCameraNative      = reinterpret_cast<t_GetMainCameraNative>(PatternScanner::FindOrFallback(hUnity, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 0D ? ? ? ? BA 05 00 00 00", 0x592900, "GetMainCameraNative"));
    pfn_InternalRaycast          = reinterpret_cast<t_InternalRaycast>(PatternScanner::FindOrFallback(hUnity, "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 41 56 48 81 EC A0 00 00 00 48 8B 3D ? ? ? ? 49 8B D9 0F 29 70 E8 48 8B F2 0F 28 F2", 0x6B6140, "InternalRaycast"));

    uintptr_t targetExecLoopVa = PatternScanner::FindOrFallback(hUnity, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 4C 8B F1", 0x5AC580, "ExecutePlayerLoop");
    void* targetExecLoop = reinterpret_cast<void*>(targetExecLoopVa);
    bool hookOk = HookManager::CreateHook(
        targetExecLoop,
        reinterpret_cast<void*>(Hooked_ExecutePlayerLoop),
        reinterpret_cast<void**>(&s_originalExecutePlayerLoop),
        "UnityPlayer_ExecutePlayerLoop"
    );

    EcsCollision::Init();

    Logger::Info("UnityBridge initialized (Unity 6000.3.19f1). ExecutePlayerLoop hook=" + std::to_string(hookOk));
    s_initialized = true;
    return true;
}

void UnityBridge::Shutdown() {
    s_ghostMirrorMode.store(false);
    s_pendingReplayApply.store(false);
    s_pendingMultiReplayApply.store(false);
    s_pendingSpawn.store(false);
    s_pendingDestroy.store(false);
    s_pendingTransform.store(false);
    s_pendingCollisionSync.store(false);
    s_pendingRepairCar.store(false);

    EcsCollision::Shutdown();

    if (IsOnMainThread()) {
        DestroyAllGhostSlotsInternal();
    } else {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_ghostSlots.clear();
        s_sceneCarCache.clear();
        s_liveCars.clear();
    }

    s_playerCSharpCar = nullptr;
    s_playerNativeObj = nullptr;
    s_initialized = false;
    Logger::Info("UnityBridge shut down.");
}

bool UnityBridge::IsOnMainThread() {
    DWORD mainTid = s_mainThreadId.load(std::memory_order_relaxed);
    return mainTid != 0 && GetCurrentThreadId() == mainTid;
}

DWORD UnityBridge::GetMainThreadId() {
    return s_mainThreadId.load(std::memory_order_relaxed);
}

void UnityBridge::Hooked_ExecutePlayerLoop(void* playerLoopSystem) {
    HookManager::ScopedHookGuard hookGuard;
    if (HookManager::IsUnloading()) {
        if (s_originalExecutePlayerLoop) {
            s_originalExecutePlayerLoop(playerLoopSystem);
        }
        return;
    }

    s_mainThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    static thread_local bool s_inTopLevelLoop = false;
    static auto s_lastTick = std::chrono::steady_clock::now();

    void* rootLoop = nullptr;
    static void* s_rootLoopSlot = nullptr;
    if (!s_rootLoopSlot && s_unityBase) {
        uintptr_t va = PatternScanner::FindRipOrFallback(reinterpret_cast<HMODULE>(s_unityBase),
            "48 89 35 ? ? ? ? 48 89 3D ? ? ? ? EB 07 48 8B 35", 7, 3, 0x2232F50, "rootLoop");
        s_rootLoopSlot = reinterpret_cast<void*>(va);
    }
    if (s_rootLoopSlot) {
        SafeReadPtr(s_rootLoopSlot, &rootLoop);
    }

    bool isTopLevel = !s_inTopLevelLoop && (rootLoop == nullptr || playerLoopSystem == rootLoop);
    if (isTopLevel) {
        s_inTopLevelLoop = true;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - s_lastTick).count();
        s_lastTick = now;
        s_lastMainDt = std::clamp(dt, 0.001f, 0.1f);

        ProcessPendingMainThreadTasks(s_lastMainDt);
        LuaEngine::OnMainUpdate(dt);

        if (s_ghostCollisionEnabled.load(std::memory_order_relaxed) && NativeCarX::IsInSession()) {
            ResolveGhostCollisions();
        }

        if (s_originalExecutePlayerLoop) {
            s_originalExecutePlayerLoop(playerLoopSystem);
        }

        s_inTopLevelLoop = false;
        return;
    }

    if (s_originalExecutePlayerLoop) {
        s_originalExecutePlayerLoop(playerLoopSystem);
    }
}

void UnityBridge::ProcessPendingMainThreadTasks(float dt) {
    if (s_pendingDestroy.exchange(false)) {
        DestroyAllGhostSlotsInternal();
    }

    if (s_pendingSpawn.exchange(false)) {
        SpawnGhostSlotInternal(0);
    }

    if (s_pendingTeleport.exchange(false)) {
        TeleportPose poseCopy{};
        int velMode = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            poseCopy = s_pendingTeleportPose;
            velMode = s_pendingTeleportVelMode;
        }
        ApplyTeleportPlayerInternal(poseCopy, velMode);
    }

    if (s_pendingRepairCar.exchange(false)) {
        ApplyRepairPlayerCarInternal();
    }

    // Periodically sync scene car model cache + per-frame lightweight position matching
    static bool s_wasInSession = false;
    const bool inSession = NativeCarX::IsInSession();
    if (!inSession && s_wasInSession) {
        s_wasInSession = false;
        DestroyAllGhostSlotsInternal();
        UpdateCarFXPlayerTexturesOnMainThread({});
        CarSimOptimizer::UpdateOnMainThread(dt, {});
    } else if (inSession) {
        s_wasInSession = true;
    }

    if (inSession) {
        RefreshLiveCarListOnMainThread(false);
        std::vector<LiveCarInfo> liveSnapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            liveSnapshot = s_liveCars;
        }
        EcsCollision::UpdateOnMainThread(dt, liveSnapshot);
        CarSimOptimizer::UpdateOnMainThread(dt, liveSnapshot);
        NativeCarX::UpdatePerFrame();

        if (s_pendingCollisionSync.exchange(false)) {
            bool col = s_ghostCollisionEnabled.load(std::memory_order_relaxed);
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            for (auto& slot : s_ghostSlots) {
                if (slot.gameObject) {
                    ApplySlotCollisionVisuals(slot, col);
                }
            }
        }

        if (s_ghostMirrorMode.load(std::memory_order_relaxed)) {
            if (s_ghostSlots.empty() || !s_ghostSlots[0].gameObject) {
                SpawnGhostSlotInternal(0);
            }
            if (!s_ghostSlots.empty() && s_ghostSlots[0].gameObject) {
                CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
                if (tele.carPtr) {
                    float calMats[4][16];
                    float whlMats[4][16];
                    for (int w = 0; w < 4; ++w) {
                        memcpy(whlMats[w], tele.wheels[w].matrix.data(), sizeof(float) * 16);
                        memcpy(calMats[w], tele.wheels[w].caliperMatrix.data(), sizeof(float) * 16);
                    }
                    float off = s_ghostMirrorOffset;
                    UnityVector3 worldOffset = {
                        tele.transformMatrix[0] * off,
                        tele.transformMatrix[1] * off,
                        tele.transformMatrix[2] * off
                    };
                    ApplyGhostPoseToSlot(0, tele.transformMatrix.data(), whlMats, calMats, tele.steerAngle, worldOffset, dt);
                    s_ghostSlots[0].replayTeleValid = false;
                }
            }
        } else if (s_pendingMultiReplayApply.exchange(false)) {
            ApplyAllReplayTracksAtTimeInternal(s_pendingMultiReplayTime, s_pendingMultiReplayOffset, dt);
        } else if (s_pendingReplayApply.exchange(false)) {
            if (s_ghostSlots.empty() || !s_ghostSlots[0].gameObject) {
                SpawnGhostSlotInternal(0);
            }
            if (!s_ghostSlots.empty() && s_ghostSlots[0].gameObject) {
                ReplayFrame copyFrame;
                float off = 0.0f;
                {
                    std::lock_guard<std::recursive_mutex> lock(s_mutex);
                    copyFrame = s_pendingReplayFrameData;
                    off = s_pendingReplayOffset;
                }
                UnityVector3 worldOffset = {
                    copyFrame.transformMatrix[0] * off,
                    copyFrame.transformMatrix[1] * off,
                    copyFrame.transformMatrix[2] * off
                };
                ApplyGhostPoseToSlot(0, copyFrame.transformMatrix, copyFrame.wheelMatrices, copyFrame.caliperMatrices, copyFrame.steerAngle, worldOffset, dt);
            }
        } else if (s_pendingTransform.exchange(false)) {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            if (!s_ghostSlots.empty() && s_ghostSlots[0].transform) {
                SetTransformPosRotDirect(s_ghostSlots[0].transform, s_pendingPos, s_pendingRot);
            }
        }

        UpdateCarFXPlayerTexturesOnMainThread(liveSnapshot);
        UpdateCarFXSpatialProximityOnMainThread(liveSnapshot);
    } else if (!s_liveCars.empty() || !s_sceneCarCache.empty()) {
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            s_liveCars.clear();
            s_sceneCarCache.clear();
        }
        EcsCollision::UpdateOnMainThread(dt, {});
        CarSimOptimizer::UpdateOnMainThread(dt, {});
        UpdateCarFXPlayerTexturesOnMainThread({});
        CarFX::ResetGhostTargets();
        CarFX::SetSpatialProximity(CarFX::SpatialProximityInput{}, CarFX::kTargetBody);
    } else {
        EcsCollision::UpdateOnMainThread(dt, {});
        CarSimOptimizer::UpdateOnMainThread(dt, {});
        UpdateCarFXPlayerTexturesOnMainThread({});
        CarFX::SetSpatialProximity(CarFX::SpatialProximityInput{}, CarFX::kTargetBody);
    }
}

// ================= Native / Managed Helpers =================

void* UnityBridge::UnwrapScriptingPtrToNative(void* gcOrObj) {
    uintptr_t raw = reinterpret_cast<uintptr_t>(gcOrObj);
    if (!raw || !s_unityBase) return nullptr;

    void* vtbl = nullptr;
    if (SafeReadPtr(gcOrObj, &vtbl)) {
        uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vtbl);
        if (vtAddr >= s_unityBase && vtAddr < s_unityBase + s_unitySize) {
            return gcOrObj;
        }
    }

    void* csObj = nullptr;
    if ((raw & 1) == 0) {
        void* maybeCached = nullptr;
        if (SafeReadPtr(reinterpret_cast<void*>(raw + 0x10), &maybeCached) && maybeCached) {
            void* nativeVt = nullptr;
            if (SafeReadPtr(maybeCached, &nativeVt)) {
                uintptr_t nvt = reinterpret_cast<uintptr_t>(nativeVt);
                if (nvt >= s_unityBase && nvt < s_unityBase + s_unitySize) {
                    return maybeCached;
                }
            }
        }
        SafeReadPtr(gcOrObj, &csObj);
    } else {
        void* pfnTarget = nullptr;
        void* targetSlot = GetGCHandleTargetPtr(s_unityBase);
        if (targetSlot && SafeReadPtr(targetSlot, &pfnTarget) && pfnTarget) {
            csObj = SafeCallGCHandleTarget(pfnTarget, raw);
        }
    }

    if (csObj) {
        void* nativePtr = nullptr;
        if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(csObj) + 0x10), &nativePtr)) {
            return nativePtr;
        }
    }

    return nullptr;
}

void* UnityBridge::GetTransformOfGameObject(void* nativeGo) {
    if (!nativeGo) return nullptr;
    void* compArray = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeGo) + 0x20), &compArray) || !compArray) {
        return nullptr;
    }
    void* tf = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(compArray) + 0x08), &tf)) {
        return tf;
    }
    return nullptr;
}

void* UnityBridge::GetGameObjectOfTransform(void* nativeTf) {
    if (!nativeTf) return nullptr;
    void* go = nullptr;
    if (SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeTf) + 0x20), &go)) {
        return go;
    }
    return nullptr;
}

std::string UnityBridge::GetGameObjectName(void* nativeGo) {
    if (!nativeGo) return "";
    void* namePtr = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeGo) + 0x50), &namePtr) || !namePtr) {
        return "";
    }
    char buf[128] = { 0 };
    if (SafeReadCStr(namePtr, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "";
}

bool UnityBridge::GetTransformHierarchyPosition(void* nativeTf, UnityVector3& outPos) {
    if (!nativeTf) return false;
    void* hier = nullptr;
    uint32_t idx = 0;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeTf) + 0x28), &hier) || !hier) {
        return false;
    }
    if (!SafeReadU32(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nativeTf) + 0x30), &idx)) {
        return false;
    }
    if (idx > 65536) return false;

    void* localTransforms = nullptr;
    if (!SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hier) + 0x18), &localTransforms) || !localTransforms) {
        return false;
    }

    uintptr_t elemAddr = reinterpret_cast<uintptr_t>(localTransforms) + static_cast<size_t>(idx) * 48;
    float xyz[3] = { 0 };
    if (SafeReadBytes(reinterpret_cast<void*>(elemAddr), xyz, sizeof(xyz))) {
        if (std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2])) {
            outPos.x = xyz[0];
            outPos.y = xyz[1];
            outPos.z = xyz[2];
            return true;
        }
    }
    return false;
}

// ================= Scene Enumeration & Car Visual Cache =================

std::vector<UnityObjectEntry> UnityBridge::GetAllGameObjects(int maxObjects) {
    std::vector<UnityObjectEntry> result;
    if (!s_unityBase) return result;

    void* mgr = nullptr;
    void* mgrSlot = GetGameObjectManagerPtr(s_unityBase);
    if (!mgrSlot || !SafeReadPtr(mgrSlot, &mgr) || !mgr) {
        return result;
    }

    std::unordered_set<void*> seen;
    seen.reserve(4096);
    result.reserve(4096);

    auto addGo = [&](void* go) {
        if (!go || reinterpret_cast<uintptr_t>(go) < 0x10000) return;
        if (!seen.insert(go).second) return;
        if (static_cast<int>(result.size()) >= maxObjects) return;

        UnityObjectEntry entry;
        entry.go = go;
        entry.transform = GetTransformOfGameObject(go);
        if (entry.transform) {
            SafeReadPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry.transform) + 0x68), &entry.parentTransform);
            entry.hasPos = GetTransformHierarchyPosition(entry.transform, entry.pos);
        }
        entry.name = GetGameObjectName(go);
        result.push_back(std::move(entry));
    };

    uintptr_t sentinel = reinterpret_cast<uintptr_t>(mgr) + 0x18;
    uintptr_t node = 0;
    if (SafeReadU64(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgr) + 0x20), &node)) {
        int guard = 0;
        while (node && node != sentinel && guard++ < maxObjects) {
            void* go = nullptr;
            if (SafeReadPtr(reinterpret_cast<void*>(node + 0x10), &go)) {
                addGo(go);
            }
            uintptr_t nextNode = 0;
            if (!SafeReadU64(reinterpret_cast<void*>(node + 0x08), &nextNode)) break;
            node = nextNode;
        }
    }

    uintptr_t entries = 0;
    uint32_t bucketCountMinus8 = 0;
    if (SafeReadU64(mgr, &entries) && entries &&
        SafeReadU32(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgr) + 0x08), &bucketCountMinus8)) {
        uint32_t buckets = bucketCountMinus8 + 8;
        if (buckets > 8192) buckets = 8192;

        for (uint32_t i = 0; i < buckets && static_cast<int>(result.size()) < maxObjects; ++i) {
            uintptr_t bucket = entries + static_cast<size_t>(i) * 24;
            uint32_t flag = 0xFFFFFFFF;
            if (!SafeReadU32(reinterpret_cast<void*>(bucket), &flag) || flag >= 0xFFFFFFFE) {
                continue;
            }
            uintptr_t listSentinel = 0;
            if (!SafeReadU64(reinterpret_cast<void*>(bucket + 0x10), &listSentinel) || listSentinel < 0x10000) {
                continue;
            }
            uintptr_t n = 0;
            if (!SafeReadU64(reinterpret_cast<void*>(listSentinel + 0x08), &n)) {
                continue;
            }
            int guard = 0;
            while (n && n != listSentinel && guard++ < maxObjects && static_cast<int>(result.size()) < maxObjects) {
                void* go = nullptr;
                if (SafeReadPtr(reinterpret_cast<void*>(n + 0x10), &go)) {
                    addGo(go);
                }
                uintptr_t nextN = 0;
                if (!SafeReadU64(reinterpret_cast<void*>(n + 0x08), &nextN)) break;
                n = nextN;
            }
        }
    }

    return result;
}

void* UnityBridge::FindGameObjectByName(const char* name) {
    if (!pfn_Find || !name) return nullptr;
    void* tf = SafeCallFind(reinterpret_cast<void*>(pfn_Find), name);
    if (!tf) return nullptr;
    return GetGameObjectOfTransform(tf);
}

static bool SafeGetSteamPersonaNameSEH(void* pfnFriends, void* pfnGetName, char* outBuf, size_t outSize) {
    if (!pfnFriends || !pfnGetName || !outBuf || outSize == 0) return false;
    __try {
        typedef void* (*t_Friends)();
        typedef const char* (*t_GetName)(void*);
        void* friends = reinterpret_cast<t_Friends>(pfnFriends)();
        if (friends) {
            const char* name = reinterpret_cast<t_GetName>(pfnGetName)(friends);
            if (name && name[0] != '\0') {
                size_t i = 0;
                for (; i + 1 < outSize && name[i] != '\0'; ++i) {
                    outBuf[i] = name[i];
                }
                outBuf[i] = '\0';
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static std::string GetLocalSteamPersonaName() {
    static std::string s_cachedName;
    static bool s_checked = false;
    if (s_checked) return s_cachedName;
    s_checked = true;
    HMODULE hSteam = GetModuleHandleW(L"steam_api64.dll");
    if (!hSteam) {
        hSteam = LoadLibraryW(L"steam_api64.dll");
    }
    if (hSteam) {
        void* pfnFriends = reinterpret_cast<void*>(GetProcAddress(hSteam, "SteamAPI_SteamFriends_v017"));
        void* pfnGetName = reinterpret_cast<void*>(GetProcAddress(hSteam, "SteamAPI_ISteamFriends_GetPersonaName"));
        char buf[128] = { 0 };
        if (SafeGetSteamPersonaNameSEH(pfnFriends, pfnGetName, buf, sizeof(buf))) {
            s_cachedName = buf;
        }
    }
    return s_cachedName;
}

static std::string StripRichTextTags(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    bool insideTag = false;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '<') {
            insideTag = true;
        } else if (str[i] == '>') {
            insideTag = false;
        } else if (!insideTag) {
            result += str[i];
        }
    }
    size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, (last - first + 1));
}

static std::string Utf16ToUtf8(const wchar_t* wstr, int len) {
    if (!wstr || len <= 0) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, len, NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return "";
    std::string out(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &out[0], sizeNeeded, NULL, NULL);
    return out;
}

static bool SafeGetIl2CppStringCharsSEH(void* il2cppStringPtr, const wchar_t** outChars, int32_t* outLen) {
    if (!il2cppStringPtr || !outChars || !outLen) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(il2cppStringPtr);
    if (!IsReadableMem(addr, 0x18)) return false;
    __try {
        int32_t len = *reinterpret_cast<const int32_t*>(addr + 0x10);
        if (len <= 0 || len > 256) return false;
        const wchar_t* chars = reinterpret_cast<const wchar_t*>(addr + 0x14);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(chars), static_cast<size_t>(len) * sizeof(wchar_t))) return false;
        *outChars = chars;
        *outLen = len;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::string ReadIl2CppString(void* il2cppStringPtr) {
    if (!il2cppStringPtr) return "";
    const wchar_t* chars = nullptr;
    int32_t len = 0;
    if (SafeGetIl2CppStringCharsSEH(il2cppStringPtr, &chars, &len) && chars && len > 0) {
        return Utf16ToUtf8(chars, len);
    }
    return "";
}

static uintptr_t SafeFindNickNamesContextSEH(void* nickContextGo) {
    if (!nickContextGo) return 0;
    __try {
        uintptr_t go = reinterpret_cast<uintptr_t>(nickContextGo);
        if (!IsReadableMem(go, 0x38)) return 0;
        uintptr_t compArr = *reinterpret_cast<uintptr_t*>(go + 0x20);
        uint32_t compCount = *reinterpret_cast<uint32_t*>(go + 0x30);
        if (compCount == 0 || compCount > 256 || !IsReadableMem(compArr, static_cast<size_t>(compCount) * 16)) return 0;
        for (uint32_t ci = 0; ci < compCount; ++ci) {
            uint32_t cid = *reinterpret_cast<uint32_t*>(compArr + ci * 16);
            if (cid != 66) continue;
            uintptr_t natComp = *reinterpret_cast<uintptr_t*>(compArr + ci * 16 + 8);
            if (!IsReadableMem(natComp, 0x20)) continue;
            uintptr_t gcSlot = *reinterpret_cast<uintptr_t*>(natComp + 0x18);
            if (!IsReadableMem(gcSlot & ~7ull, sizeof(uintptr_t))) continue;
            uintptr_t managed = *reinterpret_cast<uintptr_t*>(gcSlot & ~7ull);
            if ((managed & 7) || !IsReadableMem(managed, 0x70)) continue;
            uintptr_t klass = *reinterpret_cast<uintptr_t*>(managed);
            if (!IsReadableMem(klass, 0x18)) continue;
            const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
            if (kname && IsReadableMem(reinterpret_cast<uintptr_t>(kname), 30) &&
                strcmp(kname, "UIPlayersNickNamesContext") == 0) {
                return managed;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static bool SafeGetNickDictCountAndEntriesSEH(uintptr_t context, uint32_t* outCount, uintptr_t* outEntries) {
    if (!context || !outCount || !outEntries) return false;
    __try {
        if (!IsReadableMem(context, 0x70)) return false;
        uintptr_t dict = *reinterpret_cast<const uintptr_t*>(context + 0x60);
        if (!IsReadableMem(dict, 0x30)) return false;
        uint32_t count = *reinterpret_cast<const uint32_t*>(dict + 0x20);
        uintptr_t entries = *reinterpret_cast<const uintptr_t*>(dict + 0x18);
        if (!entries || count == 0 || count > 128) return false;
        if (!IsReadableMem(entries, 0x20 + count * 24)) return false;
        *outCount = count;
        *outEntries = entries;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadSingleNickDataSEH(uintptr_t entryBase, void** outNativeTf, void** outIl2CppString, float outPos[3]) {
    if (!outNativeTf || !outIl2CppString || !outPos) return false;
    __try {
        uintptr_t val = *reinterpret_cast<const uintptr_t*>(entryBase + 16);
        if (!IsReadableMem(val, 0x70)) return false;
        uintptr_t carTfManaged = *reinterpret_cast<const uintptr_t*>(val + 0x38);
        void* nativeTf = nullptr;
        if (carTfManaged && IsReadableMem(carTfManaged, 0x20)) {
            nativeTf = *reinterpret_cast<void* const*>(carTfManaged + 0x10);
        }
        *outNativeTf = nativeTf;
        *outIl2CppString = *reinterpret_cast<void* const*>(val + 0x50);
        outPos[0] = *reinterpret_cast<const float*>(val + 0x40);
        outPos[1] = *reinterpret_cast<const float*>(val + 0x44);
        outPos[2] = *reinterpret_cast<const float*>(val + 0x48);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UnityBridge::RefreshLiveCarListOnMainThread(bool forceRescan) {
    if (!IsOnMainThread()) return;

    auto cars = NativeCarX::GetAllCarsTelemetry();
    auto now = std::chrono::steady_clock::now();
    float sinceScan = std::chrono::duration<float>(now - s_lastSceneCarScan).count();

    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    bool needRescan = forceRescan || s_sceneCarCache.empty() || (sinceScan > 1.0f);
    if (!needRescan && cars.size() != s_liveCars.size() && sinceScan > 0.25f) {
        needRescan = true;
    }

    static uintptr_t s_cachedNickContext = 0;

    if (needRescan) {
        s_lastSceneCarScan = now;
        s_cachedNickContext = 0;
        auto objects = GetAllGameObjects(30000);
        std::unordered_map<void*, const UnityObjectEntry*> tfMap;
        tfMap.reserve(objects.size());
        void* nickContextGo = nullptr;
        for (const auto& obj : objects) {
            if (obj.transform) tfMap[obj.transform] = &obj;
            if (!nickContextGo && obj.name == "UIPlayersNickNames(Clone)") {
                nickContextGo = obj.go;
            }
        }

        if (nickContextGo) {
            s_cachedNickContext = SafeFindNickNamesContextSEH(nickContextGo);
        }

        std::unordered_set<void*> ownedClones;
        for (const auto& slot : s_ghostSlots) {
            if (slot.gameObject) ownedClones.insert(slot.gameObject);
        }

        std::vector<SceneCarModelCache> rebuilt;
        for (const auto& obj : objects) {
            if (obj.name != "Model" || !obj.parentTransform) continue;
            if (ownedClones.count(obj.go)) continue;

            auto it = tfMap.find(obj.parentTransform);
            if (it == tfMap.end()) continue;
            const UnityObjectEntry* root = it->second;
            if (root->parentTransform != nullptr) continue; // real cars are top-level scene roots

            SceneCarModelCache item;
            item.unityName = root->name;
            item.rootGo = root->go;
            item.rootTf = root->transform;
            item.modelGo = obj.go;

            CarBehaviourInfo cbInfo{};
            if (SafeInspectCarBehaviourOnRootGoSEH(item.rootGo, &cbInfo)) {
                item.carBehaviour = reinterpret_cast<void*>(cbInfo.carBehaviour);
                item.nativeCarPtr = cbInfo.nativeCarPtr;
                item.raceCarPtr = cbInfo.raceCarPtr;
                item.isPlayer = cbInfo.isPlayer;
            }

            if (pfn_Transform_get_position && SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), item.rootTf, &item.pos)) {
                item.hasPos = true;
            } else if (root->hasPos) {
                item.pos = root->pos;
                item.hasPos = true;
            }
            if (pfn_Transform_get_rotation && item.rootTf) {
                item.hasRot = SafeCallGetVec4(reinterpret_cast<void*>(pfn_Transform_get_rotation), item.rootTf, &item.rot);
            }
            rebuilt.push_back(std::move(item));
        }
        s_sceneCarCache = std::move(rebuilt);
    } else if (pfn_Transform_get_position || pfn_Transform_get_rotation) {
        for (auto& sc : s_sceneCarCache) {
            if (sc.rootTf) {
                if (pfn_Transform_get_position) {
                    sc.hasPos = SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), sc.rootTf, &sc.pos);
                }
                if (pfn_Transform_get_rotation) {
                    sc.hasRot = SafeCallGetVec4(reinterpret_cast<void*>(pfn_Transform_get_rotation), sc.rootTf, &sc.rot);
                }
            }
            if (sc.rootGo && (!sc.carBehaviour || !sc.nativeCarPtr || !sc.raceCarPtr)) {
                CarBehaviourInfo cbInfo{};
                if (SafeInspectCarBehaviourOnRootGoSEH(sc.rootGo, &cbInfo)) {
                    sc.carBehaviour = reinterpret_cast<void*>(cbInfo.carBehaviour);
                    sc.nativeCarPtr = cbInfo.nativeCarPtr;
                    sc.raceCarPtr = cbInfo.raceCarPtr;
                    sc.isPlayer = cbInfo.isPlayer;
                }
            }
        }
    }

    // Extract multiplayer nicknames from UIPlayersNickNamesContext
    struct RemoteNickEntry {
        void* nativeTf = nullptr;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        std::string name;
    };
    std::vector<RemoteNickEntry> remoteNicks;

    uint32_t nickCount = 0;
    uintptr_t nickEntries = 0;
    if (SafeGetNickDictCountAndEntriesSEH(s_cachedNickContext, &nickCount, &nickEntries)) {
        for (uint32_t i = 0; i < nickCount; ++i) {
            uintptr_t entryBase = nickEntries + 0x20 + i * 24;
            void* nativeTf = nullptr;
            void* il2cppStr = nullptr;
            float pos[3] = { 0.0f, 0.0f, 0.0f };
            if (SafeReadSingleNickDataSEH(entryBase, &nativeTf, &il2cppStr, pos)) {
                std::string rawName = ReadIl2CppString(il2cppStr);
                std::string clean = StripRichTextTags(rawName);
                if (!clean.empty()) {
                    RemoteNickEntry rne;
                    rne.nativeTf = nativeTf;
                    rne.name = clean;
                    rne.x = pos[0];
                    rne.y = pos[1];
                    rne.z = pos[2];
                    remoteNicks.push_back(std::move(rne));
                }
            }
        }
    }

    std::string localNick = GetLocalSteamPersonaName();
    if (localNick.empty()) localNick = "Вы";

    for (auto& sc : s_sceneCarCache) {
        if (sc.isPlayer) {
            sc.playerName = localNick;
        } else {
            std::string matchedNick;
            if (sc.rootTf) {
                for (const auto& rne : remoteNicks) {
                    if (rne.nativeTf && rne.nativeTf == sc.rootTf) {
                        matchedNick = rne.name;
                        break;
                    }
                }
            }
            if (matchedNick.empty() && sc.hasPos) {
                float bestDistSq = 25.0f; // within 5m
                for (const auto& rne : remoteNicks) {
                    float dx = rne.x - sc.pos.x;
                    float dy = rne.y - sc.pos.y;
                    float dz = rne.z - sc.pos.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestDistSq) {
                        bestDistSq = d2;
                        matchedNick = rne.name;
                    }
                }
            }
            if (!matchedNick.empty()) {
                sc.playerName = matchedNick;
            }
        }
    }

    // Push authoritative local player car pointer from Unity CarBehaviour::m_isPlayer into NativeCarX
    void* knownPlayerCarPtr = nullptr;
    for (const auto& sc : s_sceneCarCache) {
        if (sc.isPlayer && sc.nativeCarPtr) {
            knownPlayerCarPtr = sc.nativeCarPtr;
            break;
        }
    }
    if (knownPlayerCarPtr) {
        NativeCarX::SetKnownPlayerCarPtr(knownPlayerCarPtr);
    }

    std::vector<LiveCarInfo> updated;
    updated.reserve(cars.size());
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto& c = cars[i];
        LiveCarInfo info;
        info.carPtr = c.carPtr;
        info.x = c.posX;
        info.y = c.posY;
        info.z = c.posZ;
        info.speedKmh = c.speedKmh;

        const SceneCarModelCache* best = nullptr;
        for (const auto& sc : s_sceneCarCache) {
            if (sc.nativeCarPtr && sc.nativeCarPtr == c.carPtr) {
                best = &sc;
                break;
            }
        }
        if (!best) {
            float bestDistSq = 16.0f; // fallback within 4m
            for (const auto& sc : s_sceneCarCache) {
                if (!sc.hasPos) continue;
                float dx = sc.pos.x - c.posX;
                float dy = sc.pos.y - c.posY;
                float dz = sc.pos.z - c.posZ;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestDistSq) {
                    bestDistSq = d2;
                    best = &sc;
                }
            }
        }

        if (best) {
            info.unityName = best->unityName;
            info.playerName = best->playerName;
            info.rootGo = best->rootGo;
            info.rootTf = best->rootTf;
            info.modelGo = best->modelGo;
            info.carBehaviour = best->carBehaviour;
            info.raceCarPtr = best->raceCarPtr;
            if (best->hasPos) {
                info.x = best->pos.x; info.y = best->pos.y; info.z = best->pos.z;
                info.hasPos = true;
            }
            if (best->hasRot) {
                info.qx = best->rot.x; info.qy = best->rot.y; info.qz = best->rot.z; info.qw = best->rot.w;
                info.hasRot = true;
            }
            info.isLocalPlayer = best->isPlayer || (knownPlayerCarPtr ? (c.carPtr == knownPlayerCarPtr) : c.isLocalPlayer);
        } else {
            info.isLocalPlayer = knownPlayerCarPtr ? (c.carPtr == knownPlayerCarPtr) : c.isLocalPlayer;
            info.unityName = info.isLocalPlayer ? "Player Car" : ("Car #" + std::to_string(i + 1));
            info.playerName = info.isLocalPlayer ? localNick : "";
        }
        updated.push_back(std::move(info));
    }

    s_liveCars = std::move(updated);
}

std::vector<LiveCarInfo> UnityBridge::GetLiveCarList() {
    auto cars = NativeCarX::GetAllCarsTelemetry();
    auto ecsStates = EcsCollision::GetCarStates();
    std::string localNick = GetLocalSteamPersonaName();
    if (localNick.empty()) localNick = "Вы";
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    std::vector<LiveCarInfo> out;
    out.reserve(cars.size());
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto& c = cars[i];
        LiveCarInfo info;
        info.carPtr = c.carPtr;
        info.isLocalPlayer = c.isLocalPlayer;
        info.x = c.posX;
        info.y = c.posY;
        info.z = c.posZ;
        info.speedKmh = c.speedKmh;

        bool matched = false;
        for (const auto& cached : s_liveCars) {
            if (cached.carPtr == c.carPtr) {
                info.unityName = cached.unityName;
                info.playerName = cached.playerName;
                info.rootGo = cached.rootGo;
                info.rootTf = cached.rootTf;
                info.modelGo = cached.modelGo;
                info.carBehaviour = cached.carBehaviour;
                info.raceCarPtr = cached.raceCarPtr;
                info.hasPos = cached.hasPos;
                if (cached.hasPos) {
                    info.x = cached.x; info.y = cached.y; info.z = cached.z;
                }
                if (cached.hasRot) {
                    info.qx = cached.qx; info.qy = cached.qy; info.qz = cached.qz; info.qw = cached.qw;
                    info.hasRot = true;
                }
                info.isLocalPlayer = cached.isLocalPlayer || c.isLocalPlayer;
                matched = true;
                break;
            }
        }
        if (!matched) {
            info.unityName = c.isLocalPlayer ? "Player Car" : ("Car #" + std::to_string(i + 1));
            info.playerName = c.isLocalPlayer ? localNick : "";
        }

        const CarGhostState* gst = nullptr;
        for (const auto& st : ecsStates) {
            if (st.carPtr && st.carPtr == c.carPtr) {
                gst = &st;
                break;
            }
        }
        if (!gst) {
            for (const auto& st : ecsStates) {
                if (!st.unityName.empty() && st.unityName == info.unityName) {
                    gst = &st;
                    break;
                }
            }
        }
        if (gst) {
            info.isWrongWay = gst->isWrongWay;
            info.isSelectivelyGhosted = gst->isSelectivelyGhosted;
            info.isGameGhosted = gst->isGameGhosted;
            info.headingDot = gst->headingDot;
            info.distToPlayer = gst->distanceMeters;
        }
        info.ghostOverride = static_cast<int>(EcsCollision::GetCarOverride(info.unityName, reinterpret_cast<uintptr_t>(info.carPtr)));
        out.push_back(std::move(info));
    }
    return out;
}

std::string UnityBridge::LookupCarUnityName(void* carPtr) {
    if (!carPtr) return "";
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (const auto& c : s_liveCars) {
        if (c.carPtr == carPtr) return c.unityName;
    }
    for (const auto& sc : s_sceneCarCache) {
        if (sc.nativeCarPtr == carPtr) return sc.unityName;
    }
    return "";
}

std::string UnityBridge::LookupCarPlayerName(void* carPtr) {
    if (!carPtr) return "";
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (const auto& c : s_liveCars) {
        if (c.carPtr == carPtr && !c.playerName.empty()) return c.playerName;
    }
    for (const auto& sc : s_sceneCarCache) {
        if (sc.nativeCarPtr == carPtr && !sc.playerName.empty()) return sc.playerName;
    }
    return "";
}

void* UnityBridge::FindModelForCarName(const std::string& preferredCarName, void** outRootTf) {
    if (outRootTf) *outRootTf = nullptr;
    if (!preferredCarName.empty()) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (s_sceneCarCache.empty() && IsOnMainThread()) {
            RefreshLiveCarListOnMainThread(true);
        }
        for (const auto& sc : s_sceneCarCache) {
            if (sc.modelGo && sc.unityName == preferredCarName) {
                if (outRootTf) *outRootTf = sc.rootTf;
                return sc.modelGo;
            }
        }
        std::string wantedBase = StripCarSuffix(preferredCarName);
        for (const auto& sc : s_sceneCarCache) {
            if (sc.modelGo && StripCarSuffix(sc.unityName) == wantedBase) {
                if (outRootTf) *outRootTf = sc.rootTf;
                return sc.modelGo;
            }
        }
    }
    return FindPlayerCarModel(outRootTf);
}

void* UnityBridge::FindPlayerCarModel(void** outRootTf) {
    if (outRootTf) *outRootTf = nullptr;

    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer && c.modelGo) {
                if (outRootTf) *outRootTf = c.rootTf;
                return c.modelGo;
            }
        }
        for (const auto& sc : s_sceneCarCache) {
            if (sc.isPlayer && sc.modelGo) {
                if (outRootTf) *outRootTf = sc.rootTf;
                return sc.modelGo;
            }
        }
    }

    CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
    if (!tele.carPtr) return nullptr;

    auto objects = GetAllGameObjects(30000);
    std::unordered_map<void*, const UnityObjectEntry*> tfMap;
    tfMap.reserve(objects.size());
    for (const auto& obj : objects) {
        if (obj.transform) tfMap[obj.transform] = &obj;
    }

    std::unordered_set<void*> ownedClones;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& slot : s_ghostSlots) {
            if (slot.gameObject) ownedClones.insert(slot.gameObject);
        }
    }

    void* bestModelGo = nullptr;
    void* bestRootTf = nullptr;
    float bestDistSq = 9.0f;

    for (const auto& obj : objects) {
        if (obj.name != "Model" || !obj.parentTransform) continue;
        if (ownedClones.count(obj.go)) continue;

        auto it = tfMap.find(obj.parentTransform);
        if (it == tfMap.end()) continue;

        const UnityObjectEntry* parent = it->second;
        UnityVector3 ppos{};
        bool gotPos = false;
        if (IsOnMainThread() && pfn_Transform_get_position) {
            gotPos = SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), parent->transform, &ppos);
        }
        if (!gotPos && parent->hasPos) {
            ppos = parent->pos;
            gotPos = true;
        }
        if (!gotPos) continue;

        float dx = ppos.x - tele.posX;
        float dy = ppos.y - tele.posY;
        float dz = ppos.z - tele.posZ;
        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestModelGo = obj.go;
            bestRootTf = parent->transform;
        }
    }

    if (outRootTf && bestRootTf) {
        *outRootTf = bestRootTf;
    }
    return bestModelGo;
}

// ================= Direct Unity Calls =================

void* UnityBridge::CloneGameObjectDirect(void* nativeGo) {
    if (!pfn_CloneSingle || !nativeGo || !IsOnMainThread()) return nullptr;
    void* gcOrObj = SafeCallClone(reinterpret_cast<void*>(pfn_CloneSingle), nativeGo);
    if (!gcOrObj) return nullptr;
    return UnwrapScriptingPtrToNative(gcOrObj);
}

bool UnityBridge::DestroyObjectDirect(void* nativeObj, float delay) {
    if (!pfn_Destroy || !nativeObj || !IsOnMainThread()) return false;
    return SafeCallDestroy(reinterpret_cast<void*>(pfn_Destroy), nativeObj, delay);
}

bool UnityBridge::SetActiveDirect(void* nativeGo, bool active) {
    if (!pfn_SetActive || !nativeGo || !IsOnMainThread()) return false;
    return SafeCallSetActive(reinterpret_cast<void*>(pfn_SetActive), nativeGo, active);
}

bool UnityBridge::SetLayerDirect(void* nativeGo, int layer) {
    if (!pfn_SetLayer || !nativeGo || !IsOnMainThread()) return false;
    return SafeCallSetLayer(reinterpret_cast<void*>(pfn_SetLayer), nativeGo, layer);
}

bool UnityBridge::GetTransformPosRotDirect(void* nativeTf, UnityVector3& outPos, UnityVector4& outRot) {
    if (!nativeTf || !IsOnMainThread()) return false;
    bool okP = pfn_Transform_get_position && SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), nativeTf, &outPos);
    bool okR = pfn_Transform_get_rotation && SafeCallGetVec4(reinterpret_cast<void*>(pfn_Transform_get_rotation), nativeTf, &outRot);
    return okP && okR;
}

bool UnityBridge::SetTransformPositionDirect(void* nativeTf, const UnityVector3& pos) {
    if (!nativeTf || !IsOnMainThread() || !pfn_Transform_set_position) return false;
    return SafeCallSetVec3(reinterpret_cast<void*>(pfn_Transform_set_position), nativeTf, &pos);
}

bool UnityBridge::SetTransformRotationDirect(void* nativeTf, const UnityVector4& rot) {
    if (!nativeTf || !IsOnMainThread() || !pfn_Transform_set_rotation) return false;
    return SafeCallSetVec4(reinterpret_cast<void*>(pfn_Transform_set_rotation), nativeTf, &rot);
}

bool UnityBridge::SetTransformPosRotDirect(void* nativeTf, const UnityVector3& pos, const UnityVector4& rot) {
    return SetTransformPositionDirect(nativeTf, pos) && SetTransformRotationDirect(nativeTf, rot);
}

void* UnityBridge::GetPlayerCSharpCar() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_playerCSharpCar;
}

void* UnityBridge::GetPlayerNativeObj() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_playerNativeObj;
}

// ================= Ghost Car Pool ("болванки") & Collision =================

void UnityBridge::ApplySlotCollisionVisuals(GhostSlot& slot, bool enabled) {
    if (!IsOnMainThread()) return;
    for (const auto& entry : slot.childGos) {
        if (!entry.go) continue;
        SetLayerDirect(entry.go, enabled ? entry.originalLayer : 2);
    }
    if (slot.liveryColliderGo) {
        SetActiveDirect(slot.liveryColliderGo, enabled);
    }
}

void UnityBridge::ConfigureGhostSlot(GhostSlot& slot, void* clonedGo, float initialSteerAngle) {
    void* clonedTf = GetTransformOfGameObject(clonedGo);
    if (!clonedTf) return;

    slot.bodyTf = clonedTf;
    slot.childGos.clear();
    slot.liveryColliderGo = nullptr;

    uint32_t rootLayer = 8;
    if (SafeReadU32(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(clonedGo) + 0x40), &rootLayer) && rootLayer <= 31) {
        slot.childGos.push_back({ clonedGo, static_cast<int32_t>(rootLayer) });
    } else {
        slot.childGos.push_back({ clonedGo, 8 });
    }

    auto objects = GetAllGameObjects(30000);
    std::unordered_map<void*, void*> parentMap;
    parentMap.reserve(objects.size());
    for (const auto& obj : objects) {
        if (obj.transform) {
            parentMap[obj.transform] = obj.parentTransform;
        }
    }

    std::unordered_set<void*> isDescendant;
    isDescendant.insert(clonedTf);

    auto belongsToClone = [&](void* tf) -> bool {
        void* cur = tf;
        for (int depth = 0; depth < 16 && cur; ++depth) {
            if (isDescendant.count(cur)) return true;
            auto it = parentMap.find(cur);
            if (it == parentMap.end()) break;
            cur = it->second;
        }
        return false;
    };

    for (const auto& obj : objects) {
        if (!obj.transform || obj.go == clonedGo) continue;
        if (!belongsToClone(obj.transform)) continue;

        isDescendant.insert(obj.transform);

        uint32_t origLayer = 8;
        if (!SafeReadU32(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj.go) + 0x40), &origLayer) || origLayer > 31) {
            origLayer = 8;
        }
        slot.childGos.push_back({ obj.go, static_cast<int32_t>(origLayer) });

        if (obj.name == "LiveryCollider") {
            slot.liveryColliderGo = obj.go;
        } else if (obj.name == "WheelFL_root") slot.wheelRootTf[0] = obj.transform;
        else if (obj.name == "WheelFR_root")   slot.wheelRootTf[1] = obj.transform;
        else if (obj.name == "WheelRL_root")   slot.wheelRootTf[2] = obj.transform;
        else if (obj.name == "WheelRR_root")   slot.wheelRootTf[3] = obj.transform;
        else if (obj.name == "WheelFL_hub")    slot.wheelHubTf[0]  = obj.transform;
        else if (obj.name == "WheelFR_hub")    slot.wheelHubTf[1]  = obj.transform;
        else if (obj.name == "WheelRL_hub")    slot.wheelHubTf[2]  = obj.transform;
        else if (obj.name == "WheelRR_hub")    slot.wheelHubTf[3]  = obj.transform;
        else if (obj.name == "FL_Wheel")       slot.wheelBoneTf[0] = obj.transform;
        else if (obj.name == "FR_Wheel")       slot.wheelBoneTf[1] = obj.transform;
        else if (obj.name == "RL_Wheel")       slot.wheelBoneTf[2] = obj.transform;
        else if (obj.name == "RR_Wheel")       slot.wheelBoneTf[3] = obj.transform;
        else if (!slot.steeringWheelTf && obj.name.rfind("SteeringWheel_", 0) == 0) {
            slot.steeringWheelTf = obj.transform;
        }
    }

    ApplySlotCollisionVisuals(slot, s_ghostCollisionEnabled.load(std::memory_order_relaxed));

    if (slot.steeringWheelTf) {
        UnityVector3 bPos{}, swPos{};
        UnityVector4 bRot{}, swRot{};
        if (GetTransformPosRotDirect(clonedTf, bPos, bRot) &&
            GetTransformPosRotDirect(slot.steeringWheelTf, swPos, swRot)) {
            UnityVector4 rel = QuatMul(QuatConj(bRot), swRot);
            slot.steeringRestBodyRel = QuatMul(rel, QuatFromAxisY(-initialSteerAngle * 0.78f));
            slot.hasSteeringRest = true;
        }
    }
}

void UnityBridge::IsolateGhostSlotCarFXTextures(size_t slotIdx, GhostSlot& slot) {
    if (!s_unityBase || !slot.gameObject) return;
    int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(slotIdx));
    if (gTarget == CarFX::kTargetNone) return;

    std::vector<BodyMatSlotRef> bodySlots;
    CollectBodyMaterialSlots(s_unityBase, slot.gameObject, true, bodySlots);
    if (bodySlots.empty()) {
        CarFX::ClearTargetTextures(gTarget);
        return;
    }

    int32_t isoMatId = 0;
    if (!SafeCloneOrAssignRendererMatSEH(s_unityBase, bodySlots[0].rendererId, bodySlots[0].slotIdx, 0, &isoMatId) || isoMatId >= 0) {
        CarFX::ClearTargetTextures(gTarget);
        return;
    }
    int multiSlotCount = (bodySlots[0].slotIdx != 0) ? 1 : 0;
    int assignOk = 1;
    int assignFail = 0;
    for (size_t i = 1; i < bodySlots.size(); ++i) {
        if (bodySlots[i].slotIdx != 0) multiSlotCount++;
        if (SafeCloneOrAssignRendererMatSEH(s_unityBase, bodySlots[i].rendererId, bodySlots[i].slotIdx, isoMatId, nullptr)) {
            assignOk++;
        } else {
            assignFail++;
        }
    }

    void* natMat = nullptr;
    int32_t origQ = 2200;
    if (!SafeResolveMatAndQueueSEH(s_unityBase, isoMatId, &natMat, &origQ) || !natMat) return;

    slot.isolatedBodyMatId = isoMatId;
    slot.savedBodyOrigQueue = origQ;

    void* natWhite = nullptr;
    void* natBlack = nullptr;
    void* natNormal = nullptr;
    GetBuiltinNativeTextures(s_unityBase, &natWhite, &natBlack, &natNormal);

    void* origAlb = SafeGetMatTextureSEH(s_unityBase, natMat, 380);   // u_sTexture
    void* origPack = SafeGetMatTextureSEH(s_unityBase, natMat, 1370); // u_sPackedMap
    const bool albIsDef = IsDefaultNativeTex(origAlb, natWhite, natBlack, natNormal);
    const bool packIsDef = IsDefaultNativeTex(origPack, natWhite, natBlack, natNormal);

    const uintptr_t pfnGetNatPtr = GetNativeTexPtrFn(s_unityBase);
    void* albSrvPtr = nullptr;
    if (origAlb) SafeCallGetNativeTexPtr(pfnGetNatPtr, origAlb, &albSrvPtr);

    void* packSrvPtr = nullptr;
    bool copyOk = false;
    void* clonedPack = CreateIsolatedPackedTexture(origPack, packIsDef, natWhite, natBlack, &packSrvPtr, &copyOk);

    if (clonedPack && packSrvPtr && SafeSetMatTextureSEH(s_unityBase, natMat, 1370, clonedPack)) {
        slot.clonedPackTex = clonedPack;
        slot.carFxIsolated = true;
    } else {
        if (clonedPack) DestroyObjectDirect(clonedPack, 0.0f);
        packSrvPtr = nullptr;
        if (origPack) SafeCallGetNativeTexPtr(pfnGetNatPtr, origPack, &packSrvPtr);
    }

    CarFX::TargetTexturesInput in{};
    in.nativeAlbedoPtr = albSrvPtr;
    in.nativePackedPtr = packSrvPtr;
    in.albedoIsDefault = albIsDef;
    in.packedIsDefault = slot.carFxIsolated ? false : packIsDef;
    in.valid = (in.nativePackedPtr != nullptr) || (in.nativeAlbedoPtr != nullptr);
    CarFX::SetPlayerTargetTextures(gTarget, in);

    char dbg[256];
    _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
                "Ghost[%zu] CarFX isolated: mat=%d q=%d bodySlots=%zu(ok=%d,fail=%d,multi=%d) albSrv=%p(def=%d) packSrv=%p(iso=%d,copy=%d)",
                slotIdx, isoMatId, origQ, bodySlots.size(), assignOk, assignFail, multiSlotCount,
                albSrvPtr, (int)albIsDef, packSrvPtr, (int)slot.carFxIsolated, (int)copyOk);
    Logger::Info(dbg);
}

void UnityBridge::SyncGhostSlotCarFXMaterialsAndShader(size_t slotIdx, GhostSlot& slot, bool allowReplayShader) {
    int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(slotIdx));
    if (gTarget == CarFX::kTargetNone) return;

    if (!allowReplayShader) {
        if (CarFX::IsTargetCompiled(gTarget)) {
            CarFX::UnloadShader(gTarget);
        }
    } else {
        CarFX::ReplayShaderConfig recCfg;
        int recTrackIdx = 0;
        if (ReplayEngine::GetLoadedCarFx(recCfg, &recTrackIdx) && recTrackIdx == static_cast<int>(slotIdx)) {
            if (!slot.replayShaderApplied) {
                slot.replayShaderApplied = true;
                slot.replayShaderSourceKey = recCfg.shaderPath;
                CarFX::ApplyConfigToTarget(gTarget, recCfg);
            }
        } else if (s_ghostMirrorPlayerShader.load(std::memory_order_relaxed)) {
            slot.replayShaderApplied = false;
            CarFX::SyncTargetFromSource(gTarget, CarFX::kTargetBody);
        } else {
            slot.replayShaderApplied = false;
            if (CarFX::IsTargetCompiled(gTarget)) {
                CarFX::UnloadShader(gTarget);
            }
        }
    }

    if (slot.isolatedBodyMatId < 0 && s_unityBase) {
        const bool wantTrans = CarFX::ShouldBypassDepthPrePass(gTarget);
        const bool wantStl = CarFX::ShouldHideCarInternals(gTarget);
        const int32_t targetQ = wantStl ? 2510 : 2590;
        SafeSyncMaterialQueueAndDepthSEH(s_unityBase, slot.isolatedBodyMatId, wantTrans, targetQ, true, slot.savedBodyOrigQueue, nullptr, nullptr);
    }
}

void UnityBridge::DestroyDetachedGhostSlot(size_t slotIdx, GhostSlot& detached) {
    int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(slotIdx));
    if (gTarget != CarFX::kTargetNone) {
        CarFX::UnloadShader(gTarget);
        CarFX::ClearTargetTextures(gTarget);
    }
    if (detached.clonedPackTex) {
        DestroyObjectDirect(detached.clonedPackTex, 0.0f);
    }
    if (detached.gameObject) {
        DestroyObjectDirect(detached.gameObject, 0.0f);
    }
    detached = GhostSlot{};
}

void* UnityBridge::SpawnGhostSlotInternal(size_t slotIdx, const std::string& preferredCarName) {
    // Detach any prior slot under brief lock, then destroy/spawn/isolate outside s_mutex
    // so synchronous Unity Gfx calls (GetNativeTexturePtr/CopyTexture) can never deadlock Present().
    GhostSlot oldSlot;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (slotIdx < s_ghostSlots.size()) {
            oldSlot = std::move(s_ghostSlots[slotIdx]);
            s_ghostSlots[slotIdx] = GhostSlot{};
        }
    }
    DestroyDetachedGhostSlot(slotIdx, oldSlot);

    void* sourceRootTf = nullptr;
    void* modelGo = FindModelForCarName(preferredCarName, &sourceRootTf);
    if (!modelGo) {
        Logger::Warn("Cannot spawn ghost slot " + std::to_string(slotIdx) + ": Car 'Model' GameObject not found.");
        return nullptr;
    }

    std::string carName = sourceRootTf ? GetGameObjectName(GetGameObjectOfTransform(sourceRootTf)) : preferredCarName;
    void* cloneGo = CloneGameObjectDirect(modelGo);
    if (!cloneGo) {
        Logger::Error("Unity Internal_CloneSingle failed for slot " + std::to_string(slotIdx));
        return nullptr;
    }

    void* cloneTf = GetTransformOfGameObject(cloneGo);
    GhostSlot slot;
    slot.gameObject = cloneGo;
    slot.transform = cloneTf;
    slot.sourceCarName = carName;

    CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
    UnityVector3 pos{};
    UnityVector4 rot{};
    if (!sourceRootTf || !GetTransformPosRotDirect(sourceRootTf, pos, rot)) {
        pos = { tele.posX, tele.posY, tele.posZ };
        rot = MatrixToQuaternion(tele.transformMatrix.data());
    }

    if (cloneTf) {
        SetTransformPosRotDirect(cloneTf, pos, rot);
    }

    float initialSteer = (sourceRootTf && carName == LookupCarUnityName(tele.carPtr)) ? tele.steerAngle : 0.0f;
    ConfigureGhostSlot(slot, cloneGo, initialSteer);
    IsolateGhostSlotCarFXTextures(slotIdx, slot);

    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (slotIdx >= s_ghostSlots.size()) {
            s_ghostSlots.resize(slotIdx + 1);
        }
        s_ghostSlots[slotIdx] = std::move(slot);
        if (slotIdx == 0) {
            s_playerNativeObj = modelGo;
        }
    }

    Logger::Info("Spawned 3D Ghost[" + std::to_string(slotIdx) + "] from '" + carName + "/Model(Clone)'");
    return cloneGo;
}

void UnityBridge::DestroyGhostSlotInternal(size_t slotIdx) {
    GhostSlot detached;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (slotIdx >= s_ghostSlots.size()) return;
        detached = std::move(s_ghostSlots[slotIdx]);
        s_ghostSlots[slotIdx] = GhostSlot{};
    }
    DestroyDetachedGhostSlot(slotIdx, detached);
}

void UnityBridge::DestroyAllGhostSlotsInternal() {
    std::vector<GhostSlot> oldSlots;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        oldSlots = std::move(s_ghostSlots);
        s_ghostSlots.clear();
    }
    CarFX::ResetGhostTargets();
    int destroyed = 0;
    for (size_t i = 0; i < oldSlots.size(); ++i) {
        if (oldSlots[i].clonedPackTex) {
            DestroyObjectDirect(oldSlots[i].clonedPackTex, 0.0f);
            oldSlots[i].clonedPackTex = nullptr;
        }
        if (oldSlots[i].gameObject) {
            DestroyObjectDirect(oldSlots[i].gameObject, 0.0f);
            destroyed++;
        }
    }
    if (destroyed > 0) {
        Logger::Info("Destroyed " + std::to_string(destroyed) + " 3D Ghost Car visual(s).");
    }
}

void UnityBridge::ApplyGhostPoseToSlot(size_t slotIdx, const float* bodyMatrix, const float wheelMatrices[4][16], const float caliperMatrices[4][16], float steerAngle, const UnityVector3& worldOffset, float dt) {
    if (!bodyMatrix || !IsOnMainThread()) return;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (slotIdx >= s_ghostSlots.size()) return;
    GhostSlot& slot = s_ghostSlots[slotIdx];
    if (!slot.bodyTf) return;

    UnityVector3 bodyPos = {
        bodyMatrix[12] + worldOffset.x,
        bodyMatrix[13] + worldOffset.y,
        bodyMatrix[14] + worldOffset.z
    };

    if (slot.hasPose && dt > 0.0005f) {
        UnityVector3 rawVel = {
            (bodyPos.x - slot.pos.x) / dt,
            (bodyPos.y - slot.pos.y) / dt,
            (bodyPos.z - slot.pos.z) / dt
        };
        float speedSq = rawVel.x * rawVel.x + rawVel.y * rawVel.y + rawVel.z * rawVel.z;
        if (speedSq < 9000.0f) { // < ~95 m/s (ignores timeline teleports / loops)
            slot.vel.x = slot.vel.x * 0.4f + rawVel.x * 0.6f;
            slot.vel.y = slot.vel.y * 0.4f + rawVel.y * 0.6f;
            slot.vel.z = slot.vel.z * 0.4f + rawVel.z * 0.6f;
        } else {
            slot.vel = { 0.0f, 0.0f, 0.0f };
        }
    } else if (!slot.hasPose) {
        slot.vel = { 0.0f, 0.0f, 0.0f };
        EstimateCarExtents(bodyMatrix, wheelMatrices, slot.halfWidth, slot.halfLength);
    }

    memcpy(slot.lastMatrix, bodyMatrix, sizeof(float) * 16);
    slot.lastMatrix[12] = bodyPos.x;
    slot.lastMatrix[13] = bodyPos.y;
    slot.lastMatrix[14] = bodyPos.z;
    slot.pos = bodyPos;
    slot.hasPose = true;

    UnityVector4 bodyRot = MatrixToQuaternion(bodyMatrix);
    SetTransformPosRotDirect(slot.bodyTf, bodyPos, bodyRot);

    for (int w = 0; w < 4; ++w) {
        const float* wm = wheelMatrices[w];
        const float* cm = caliperMatrices[w];

        UnityVector3 wPos = {
            wm[12] + worldOffset.x,
            wm[13] + worldOffset.y,
            wm[14] + worldOffset.z
        };

        float calMag = fabsf(cm[0]) + fabsf(cm[4]) + fabsf(cm[8]);
        UnityVector4 knuckleRot = (calMag > 0.01f)
            ? MatrixToQuaternion(cm)
            : ReconstructSpinFreeKnuckle(wm, bodyMatrix);

        if (slot.wheelRootTf[w]) {
            SetTransformPosRotDirect(slot.wheelRootTf[w], wPos, knuckleRot);
        }
        if (slot.wheelBoneTf[w]) {
            SetTransformPosRotDirect(slot.wheelBoneTf[w], wPos, knuckleRot);
        }
        if (slot.wheelHubTf[w]) {
            UnityVector4 hubRot = MatrixToQuaternion(wm);
            SetTransformRotationDirect(slot.wheelHubTf[w], hubRot);
        }
    }

    if (slot.steeringWheelTf && slot.hasSteeringRest) {
        UnityVector4 swRot = QuatMul(bodyRot, QuatMul(slot.steeringRestBodyRel, QuatFromAxisY(steerAngle * 0.78f)));
        SetTransformRotationDirect(slot.steeringWheelTf, swRot);
    }
}

void UnityBridge::ApplyGhostPose(const float* bodyMatrix, const float wheelMatrices[4][16], const float caliperMatrices[4][16], float steerAngle, const UnityVector3& worldOffset) {
    ApplyGhostPoseToSlot(0, bodyMatrix, wheelMatrices, caliperMatrices, steerAngle, worldOffset, 0.016f);
}

void UnityBridge::ApplyAllReplayTracksAtTimeInternal(float targetTime, float offsetRight, float dt) {
    uint32_t trackCount = ReplayEngine::GetReplayTrackCount();
    if (trackCount == 0) return;

    const float totalDur = ReplayEngine::GetReplayDuration();

    for (uint32_t t = 0; t < trackCount; ++t) {
        ReplayFrame frame;
        if (!ReplayEngine::GetInterpolatedTrackFrame(static_cast<int>(t), targetTime, frame)) {
            continue;
        }
        bool needSpawn = false;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            needSpawn = (t >= s_ghostSlots.size() || !s_ghostSlots[t].gameObject);
        }
        if (needSpawn && NativeCarX::IsInSession()) {
            SpawnGhostSlotInternal(t, ReplayEngine::GetReplayTrackName(static_cast<int>(t)));
        }

        UnityVector3 worldOffset = {
            frame.transformMatrix[0] * offsetRight,
            frame.transformMatrix[1] * offsetRight,
            frame.transformMatrix[2] * offsetRight
        };
        ApplyGhostPoseToSlot(t, frame.transformMatrix, frame.wheelMatrices, frame.caliperMatrices, frame.steerAngle, worldOffset, dt);

        // Compute clean velocity vector from adjacent replay samples (works even while paused or on frame 0!)
        UnityVector3 velVec{ 0.0f, 0.0f, 0.0f };
        const float eps = 0.04f;
        const float t0 = (std::max)(0.0f, targetTime - eps);
        const float t1 = (totalDur > 0.0f) ? (std::min)(totalDur, targetTime + eps) : (targetTime + eps);
        ReplayFrame f0{}, f1{};
        if ((t1 - t0) > 0.005f &&
            ReplayEngine::GetInterpolatedTrackFrame(static_cast<int>(t), t0, f0) &&
            ReplayEngine::GetInterpolatedTrackFrame(static_cast<int>(t), t1, f1)) {
            const float invSpan = 1.0f / (t1 - t0);
            velVec.x = ((f1.transformMatrix[12] + f1.transformMatrix[0] * offsetRight) -
                        (f0.transformMatrix[12] + f0.transformMatrix[0] * offsetRight)) * invSpan;
            velVec.y = ((f1.transformMatrix[13] + f1.transformMatrix[1] * offsetRight) -
                        (f0.transformMatrix[13] + f0.transformMatrix[1] * offsetRight)) * invSpan;
            velVec.z = ((f1.transformMatrix[14] + f1.transformMatrix[2] * offsetRight) -
                        (f0.transformMatrix[14] + f0.transformMatrix[2] * offsetRight)) * invSpan;
        }

        bool hasActiveSlot = false;
        float rAccel = (frame.flags & (1u << 2)) ? 1.0f : 0.0f;
        float rBrake = (frame.flags & (1u << 0)) ? 1.0f : 0.0f;
        float rHandbrake = (frame.flags & (1u << 1)) ? 1.0f : 0.0f;
        float rSteerDeg = frame.steerAngle * 57.2957795f;
        GhostSlot syncCopy;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            if (t < s_ghostSlots.size() && s_ghostSlots[t].gameObject) {
                GhostSlot& slot = s_ghostSlots[t];
                slot.replayTeleValid = true;
                slot.replaySpeedKmh = frame.speedKmh;
                slot.replayRpm = frame.rpm;
                slot.replayGear = frame.gear;
                slot.replaySteerDeg = rSteerDeg;
                slot.replayAccel = rAccel;
                slot.replayBrake = rBrake;
                slot.replayHandbrake = rHandbrake;
                slot.replayWorldVel = velVec;
                syncCopy.isolatedBodyMatId = slot.isolatedBodyMatId;
                syncCopy.savedBodyOrigQueue = slot.savedBodyOrigQueue;
                syncCopy.replayShaderApplied = slot.replayShaderApplied;
                syncCopy.replayShaderSourceKey = slot.replayShaderSourceKey;
                hasActiveSlot = true;
            }
        }

        if (hasActiveSlot) {
            const int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(t));
            if (gTarget != CarFX::kTargetNone) {
                SyncGhostSlotCarFXMaterialsAndShader(t, syncCopy, true);
                {
                    std::lock_guard<std::recursive_mutex> lock(s_mutex);
                    if (t < s_ghostSlots.size()) {
                        s_ghostSlots[t].replayShaderApplied = syncCopy.replayShaderApplied;
                        s_ghostSlots[t].replayShaderSourceKey = std::move(syncCopy.replayShaderSourceKey);
                    }
                }

                const float speedFactor = std::clamp(frame.speedKmh / 35.0f, 0.0f, 1.0f);
                const float steerRad = fabsf(frame.steerAngle);
                const float driftNorm = std::clamp(steerRad * 2.2f * speedFactor +
                    (rHandbrake > 0.5f ? 0.4f * speedFactor : 0.0f), 0.0f, 1.0f);

                CarFX::SetGameInput(0, frame.speedKmh, gTarget);
                CarFX::SetGameInput(1, frame.rpm, gTarget);
                CarFX::SetGameInput(2, fabsf(rSteerDeg), gTarget);
                CarFX::SetGameInput(3, driftNorm, gTarget);
                CarFX::SetGameInput(4, rAccel, gTarget);
                CarFX::SetGameInput(5, rBrake, gTarget);
                CarFX::SetGameInput(6, static_cast<float>(frame.gear), gTarget);
            }
        }
    }

    // Remove surplus ghost slots if switching to a replay with fewer tracks
    std::vector<GhostSlot> surplus;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (s_ghostSlots.size() > trackCount) {
            surplus.assign(std::make_move_iterator(s_ghostSlots.begin() + trackCount),
                           std::make_move_iterator(s_ghostSlots.end()));
            s_ghostSlots.resize(trackCount);
        }
    }
    for (size_t i = 0; i < surplus.size(); ++i) {
        DestroyDetachedGhostSlot(trackCount + i, surplus[i]);
    }
}

void UnityBridge::QuatToAxes(const UnityVector4& qIn, UnityVector3& right, UnityVector3& up, UnityVector3& fwd) {
    UnityVector4 q = qIn;
    float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq > 1e-8f) {
        float inv = 1.0f / sqrtf(lenSq);
        q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
    } else {
        q = { 0.0f, 0.0f, 0.0f, 1.0f };
    }
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    right = VecNormalize({ 1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy) });
    up    = VecNormalize({ 2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx) });
    fwd   = VecNormalize({ 2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy) });
}

static bool SafeGetMainCameraPoseSEH(void* fnGetCam, void* fnTfGetPos, void* fnTfGetRot, UnityVector3* outPos, UnityVector4* outRot) {
    if (!fnGetCam || (!outPos && !outRot)) return false;
    __try {
        using t_GetCam = void* (*)();
        using t_GetPos = void (*)(void*, UnityVector3*);
        using t_GetRot = void (*)(void*, UnityVector4*);
        void* camNative = reinterpret_cast<t_GetCam>(fnGetCam)();
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(camNative), 0x28)) return false;
        uintptr_t camGo = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(camNative) + 0x20);
        if (!IsReadableMem(camGo, 0x28)) return false;
        uintptr_t compArr = *reinterpret_cast<const uintptr_t*>(camGo + 0x20);
        if (!IsReadableMem(compArr, 0x10)) return false;
        void* camTf = *reinterpret_cast<void* const*>(compArr + 0x08);
        if (reinterpret_cast<uintptr_t>(camTf) < 0x10000) return false;
        if (outPos && fnTfGetPos) reinterpret_cast<t_GetPos>(fnTfGetPos)(camTf, outPos);
        if (outRot && fnTfGetRot) reinterpret_cast<t_GetRot>(fnTfGetRot)(camTf, outRot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// main camera axes (world), refreshed once per frame next to the camera position; consumed by the spatial builder
static float s_camAxes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
static bool s_camAxesValid = false;

static bool SafeGetMainCameraPosSEH(void* fnGetCam, void* fnTfGetPos, UnityVector3* outPos) {
    return SafeGetMainCameraPoseSEH(fnGetCam, fnTfGetPos, nullptr, outPos, nullptr);
}

static bool SafePhysicsRaycastSEH(void* fnRaycast, const UnityRay* ray, float maxDist, UnityRaycastHit* outHit, int layerMask) {
    if (!fnRaycast || !ray || !outHit) return false;
    __try {
        memset(outHit, 0, sizeof(*outHit));
        const uint8_t* p = reinterpret_cast<const uint8_t*>(fnRaycast);
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x09) {
            // Unity icall thunk (PhysicsScene_CUSTOM_Internal_Raycast): expects pointer &sceneHandle
            using t_RayThunk = bool (*)(const uint64_t*, const void*, float, void*, int, int);
            uint64_t sceneHandle = 0;
            return reinterpret_cast<t_RayThunk>(fnRaycast)(&sceneHandle, ray, maxDist, outHit, layerMask, 1);
        } else {
            // Direct Physics::Raycast: expects sceneHandle by value (0)
            using t_RayDirect = bool (*)(uint64_t, const void*, float, void*, int, int);
            return reinterpret_cast<t_RayDirect>(fnRaycast)(0, ray, maxDist, outHit, layerMask, 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static UnityVector3 ClosestPointOnOBB(const UnityVector3& pt,
                                      const UnityVector3& center,
                                      const UnityVector3& right,
                                      const UnityVector3& up,
                                      const UnityVector3& fwd,
                                      float halfW, float halfH, float halfL) {
    UnityVector3 d = { pt.x - center.x, pt.y - center.y, pt.z - center.z };
    float lx = std::clamp(VecDot(d, right), -halfW, halfW);
    float ly = std::clamp(VecDot(d, up),    -halfH, halfH);
    float lz = std::clamp(VecDot(d, fwd),   -halfL, halfL);
    return {
        center.x + right.x * lx + up.x * ly + fwd.x * lz,
        center.y + right.y * lx + up.y * ly + fwd.y * lz,
        center.z + right.z * lx + up.z * ly + fwd.z * lz
    };
}

struct VehicleOBBCandidate {
    int ownerTag = 0; // -1 = local player, 0..999 = remote/AI, 1000+slot = ghost
    UnityVector3 center{};
    UnityVector3 right{ 1.0f, 0.0f, 0.0f };
    UnityVector3 up{ 0.0f, 1.0f, 0.0f };
    UnityVector3 fwd{ 0.0f, 0.0f, 1.0f };
    float halfW = 0.96f;
    float halfH = 0.65f;
    float halfL = 2.25f;
};

struct CarSpatialTracker {
    struct SmoothZone {
        bool valid = false;
        float missAge = 0.0f;
        float localPt[3] = { 0.0f, 0.0f, 0.0f };
        float gap = 999.0f;
        UnityVector3 normal{ 0.0f, 1.0f, 0.0f };
    } zones[11]{};
    bool nearValid = false;
    float nearGap = 999.0f;
    float nearLocalCenter[3] = { 0.0f, 0.0f, 0.0f };
    float nearLocalClosest[3] = { 0.0f, 0.0f, 0.0f };
    float nearLocalRight[3] = { 1.0f, 0.0f, 0.0f };
    float nearLocalFwd[3] = { 0.0f, 0.0f, 1.0f };
    float nearExtents[3] = { 0.96f, 0.65f, 2.25f };
    float smoothDriftAbs = 0.0f;
    float smoothDriftSigned = 0.0f;
    bool hasPrevPos = false;
    UnityVector3 prevPos{};

    void ResetSmooth() {
        for (auto& z : zones) z = SmoothZone{};
        nearValid = false;
        nearGap = 999.0f;
        smoothDriftAbs = 0.0f;
        smoothDriftSigned = 0.0f;
        hasPrevPos = false;
    }
};

static void EvaluateCarSpatialTarget(
    int carfxTarget,
    int selfOwnerTag,
    const UnityVector3& pPos,
    const UnityVector3& pRight,
    const UnityVector3& pUp,
    const UnityVector3& pFwd,
    float halfW, float halfH, float halfL,
    float frontAxleZ, float rearAxleZ,
    bool hasVel, const float rawVel[3],
    bool hasCamPos, const UnityVector3& camPos,
    void* fnRaycast,
    float dt,
    const std::vector<VehicleOBBCandidate>& otherCars,
    CarSpatialTracker& tr)
{
    CarFX::SpatialProximityInput sp{};
    sp.carPos[0] = pPos.x; sp.carPos[1] = pPos.y; sp.carPos[2] = pPos.z; sp.carPos[3] = halfW;
    sp.carRight[0] = pRight.x; sp.carRight[1] = pRight.y; sp.carRight[2] = pRight.z; sp.carRight[3] = halfL;
    sp.carUp[0] = pUp.x; sp.carUp[1] = pUp.y; sp.carUp[2] = pUp.z; sp.carUp[3] = halfH * 2.0f;
    sp.carFwd[0] = pFwd.x; sp.carFwd[1] = pFwd.y; sp.carFwd[2] = pFwd.z; sp.carFwd[3] = 1.0f;
    if (hasCamPos) {
        sp.camPos[0] = camPos.x; sp.camPos[1] = camPos.y; sp.camPos[2] = camPos.z; sp.camPos[3] = 1.0f;
    }
    for (int k = 0; k < 3; ++k) {
        sp.camRight[k] = s_camAxes[0][k];
        sp.camUp[k] = s_camAxes[1][k];
        sp.camFwd[k] = s_camAxes[2][k];
    }
    sp.camFwd[3] = s_camAxesValid ? 1.0f : 0.0f;

    // 1. Volumetric perimeter proximity queries (3D SphereCasts + barrier layer + low-curb CastRays, 8 zones incl. doors)
    struct RawZoneHit {
        bool hit = false;
        float gap = 999.0f;
        UnityVector3 point{};
        UnityVector3 normal{ 0.0f, 1.0f, 0.0f };
    };
    enum ZoneId {
        kZoneFront = 0, kZoneRear, kZoneDoorL, kZoneDoorR,
        kZoneFrontL, kZoneFrontR, kZoneRearL, kZoneRearR,
        kZoneCount
    };
    RawZoneHit rawZones[kZoneCount];
    {
        const uint32_t dotsBelongs         = 0x100u;     // Layer 8: Player Car
        const uint32_t dotsCollidesStatic  = ~(0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u | 0x20000u | 0x40000u | 0x800000u | 0x1000000u);
        const uint32_t dotsCollidesBarrier = 0x00010000u; // Car-only barrier walls (zero road asphalt)
        const int physxLayerMask = static_cast<int>(~((1u << 2) | (1u << 5) | (1u << 8) | (1u << 12) | (1u << 15) | (1u << 17) | (1u << 22)));
        const float maxScanRange = 9.0f;

        const float fbStart = (std::max)(0.35f, halfL - 1.15f);
        const float lrStart = (std::max)(0.10f, halfW - 0.72f);
        const float doorCenterZ  = 0.5f * (frontAxleZ + rearAxleZ);
        const float doorHalfSpan = std::clamp(0.5f * (frontAxleZ - rearAxleZ) - 0.36f, 0.25f, 0.90f);
        const float frontSideZ   = (std::min)(halfL - 0.15f, frontAxleZ + 0.46f);
        const float rearSideZ    = (std::max)(-halfL + 0.15f, rearAxleZ - 0.46f);

        auto recordZoneCandidate = [&](int z, float gap, const UnityVector3& hitPt, const UnityVector3& hitNorm) {
            RawZoneHit& rz = rawZones[z];
            if (!rz.hit || gap < rz.gap) {
                rz.hit = true;
                rz.gap = gap;
                rz.point = hitPt;
                rz.normal = hitNorm;
            }
        };

        auto recordWallHit = [&](int hintZone, const UnityVector3& dir, const UnityVector3& hitPt, const UnityVector3& hitNorm) {
            const UnityVector3 rel = { hitPt.x - pPos.x, hitPt.y - pPos.y, hitPt.z - pPos.z };
            if (VecDot(rel, dir) <= 0.05f) return;

            const float lx = VecDot(rel, pRight);
            const float ly = VecDot(rel, pUp);
            const float lz = VecDot(rel, pFwd);
            if (ly < -0.25f || ly > 2.35f) return;

            const float ex = (std::max)(0.0f, fabsf(lx) - halfW);
            const float ez = (std::max)(0.0f, fabsf(lz) - halfL);
            const float gap = sqrtf(ex * ex + ez * ez);
            if (gap > maxScanRange) return;

            recordZoneCandidate(hintZone, gap, hitPt, hitNorm);

            if (lz >= halfL - 0.18f && fabsf(lx) <= halfW + 0.35f)
                recordZoneCandidate(kZoneFront, gap, hitPt, hitNorm);
            if (lz <= -halfL + 0.18f && fabsf(lx) <= halfW + 0.35f)
                recordZoneCandidate(kZoneRear, gap, hitPt, hitNorm);
            if (lx <= -halfW + 0.18f && lz >= rearAxleZ + 0.08f && lz <= frontAxleZ - 0.08f)
                recordZoneCandidate(kZoneDoorL, gap, hitPt, hitNorm);
            if (lx >= halfW - 0.18f && lz >= rearAxleZ + 0.08f && lz <= frontAxleZ - 0.08f)
                recordZoneCandidate(kZoneDoorR, gap, hitPt, hitNorm);
            if (lx <= -halfW * 0.45f && lz >= frontAxleZ - 0.25f)
                recordZoneCandidate(kZoneFrontL, gap, hitPt, hitNorm);
            if (lx >= halfW * 0.45f && lz >= frontAxleZ - 0.25f)
                recordZoneCandidate(kZoneFrontR, gap, hitPt, hitNorm);
            if (lx <= -halfW * 0.45f && lz <= rearAxleZ + 0.25f)
                recordZoneCandidate(kZoneRearL, gap, hitPt, hitNorm);
            if (lx >= halfW * 0.45f && lz <= rearAxleZ + 0.25f)
                recordZoneCandidate(kZoneRearR, gap, hitPt, hitNorm);
        };

        auto computeEdgeOffset = [&](float sx, float sz, float dx, float dz) -> float {
            float tx = (fabsf(dx) > 1e-4f) ? (((dx > 0.0f ? halfW : -halfW) - sx) / dx) : 999.0f;
            float tz = (fabsf(dz) > 1e-4f) ? (((dz > 0.0f ? halfL : -halfL) - sz) / dz) : 999.0f;
            return (std::max)(0.05f, (std::min)(tx, tz));
        };

        auto isGroundSlope = [&](const UnityVector3& n, float maxSlopeCos) -> bool {
            float lenSq = n.x * n.x + n.y * n.y + n.z * n.z;
            if (lenSq <= 0.25f) return false;
            return fabsf(VecDot(n, pUp)) > maxSlopeCos || fabsf(n.y) > maxSlopeCos;
        };

        auto probeWallSphere = [&](int zone, float sx, float sy, float sz,
                                   float dx, float dy, float dz, float radius,
                                   uint32_t collidesMask, float maxSlopeCos, int maxSlopeHops,
                                   bool allowRayFallback) {
            UnityVector3 dir = VecNormalize({
                pRight.x * dx + pUp.x * dy + pFwd.x * dz,
                pRight.y * dx + pUp.y * dy + pFwd.y * dz,
                pRight.z * dx + pUp.z * dy + pFwd.z * dz
            });
            const float edgeDist = computeEdgeOffset(sx, sz, dx, dz);
            const float inset = radius + 0.04f;
            const float startOffset = (std::max)(0.0f, edgeDist - inset);
            UnityVector3 curOrig = {
                pPos.x + pRight.x * sx + pUp.x * sy + pFwd.x * sz + dir.x * startOffset,
                pPos.y + pRight.y * sx + pUp.y * sy + pFwd.y * sz + dir.y * startOffset,
                pPos.z + pRight.z * sx + pUp.z * sy + pFwd.z * sz + dir.z * startOffset
            };
            float remDist = (edgeDist - startOffset) + maxScanRange;
            float curRadius = radius;
            bool sphereWorked = false;

            for (int hop = 0; hop <= maxSlopeHops && remDist > 0.25f; ++hop) {
                float origArr[3] = { curOrig.x, curOrig.y, curOrig.z };
                float dirArr[3]  = { dir.x, dir.y, dir.z };
                EcsCollision::SphereCastHit shit{};
                bool dotsExecuted = false;
                bool hit = EcsCollision::SphereCast(origArr, curRadius, dirArr, remDist, dotsBelongs, collidesMask, shit, 1, &dotsExecuted);
                sphereWorked = sphereWorked || dotsExecuted;
                if (!hit) break;

                UnityVector3 hitPt{ shit.position[0], shit.position[1], shit.position[2] };
                UnityVector3 hitNorm{ shit.surfaceNormal[0], shit.surfaceNormal[1], shit.surfaceNormal[2] };
                if (isGroundSlope(hitNorm, maxSlopeCos)) {
                    float advance = (std::max)(0.25f, shit.fraction * remDist + 0.14f);
                    if (advance >= remDist - 0.15f) break;
                    curOrig = {
                        hitPt.x + dir.x * (curRadius * 0.55f + 0.12f) + pUp.x * (curRadius + 0.20f),
                        hitPt.y + dir.y * (curRadius * 0.55f + 0.12f) + pUp.y * (curRadius + 0.20f),
                        hitPt.z + dir.z * (curRadius * 0.55f + 0.12f) + pUp.z * (curRadius + 0.20f)
                    };
                    remDist -= advance;
                    curRadius = (std::min)(curRadius, 0.22f);
                    continue;
                }
                recordWallHit(zone, dir, hitPt, hitNorm);
                break;
            }

            if (!sphereWorked && allowRayFallback) {
                const float rayLen = edgeDist + maxScanRange;
                UnityVector3 rayOrig = {
                    pPos.x + pRight.x * sx + pUp.x * sy + pFwd.x * sz,
                    pPos.y + pRight.y * sx + pUp.y * sy + pFwd.y * sz,
                    pPos.z + pRight.z * sx + pUp.z * sy + pFwd.z * sz
                };
                float start[3] = { rayOrig.x, rayOrig.y, rayOrig.z };
                float end[3]   = { rayOrig.x + dir.x * rayLen, rayOrig.y + dir.y * rayLen, rayOrig.z + dir.z * rayLen };
                DotsRaycastHit dhit{};
                if (EcsCollision::CastRay(start, end, dotsBelongs, collidesMask, dhit)) {
                    UnityVector3 hitNorm{ dhit.normal[0], dhit.normal[1], dhit.normal[2] };
                    if (!isGroundSlope(hitNorm, maxSlopeCos)) {
                        recordWallHit(zone, dir, { dhit.point[0], dhit.point[1], dhit.point[2] }, hitNorm);
                    }
                } else if (fnRaycast) {
                    UnityRay ray{ rayOrig, dir };
                    UnityRaycastHit hit{};
                    if (SafePhysicsRaycastSEH(fnRaycast, &ray, rayLen, &hit, physxLayerMask)) {
                        if (!isGroundSlope(hit.normal, maxSlopeCos)) {
                            recordWallHit(zone, dir, hit.point, hit.normal);
                        }
                    }
                }
            }
        };

        auto probeLowCurbRay = [&](int zone, float sx, float sy, float sz, float dx, float dy, float dz) {
            UnityVector3 dir = VecNormalize({
                pRight.x * dx + pUp.x * dy + pFwd.x * dz,
                pRight.y * dx + pUp.y * dy + pFwd.y * dz,
                pRight.z * dx + pUp.z * dy + pFwd.z * dz
            });
            const float rayLen = computeEdgeOffset(sx, sz, dx, dz) + maxScanRange;
            UnityVector3 rayOrig = {
                pPos.x + pRight.x * sx + pUp.x * sy + pFwd.x * sz,
                pPos.y + pRight.y * sx + pUp.y * sy + pFwd.y * sz,
                pPos.z + pRight.z * sx + pUp.z * sy + pFwd.z * sz
            };
            float start[3] = { rayOrig.x, rayOrig.y, rayOrig.z };
            float end[3]   = { rayOrig.x + dir.x * rayLen, rayOrig.y + dir.y * rayLen, rayOrig.z + dir.z * rayLen };
            DotsRaycastHit dhit{};
            if (EcsCollision::CastRay(start, end, dotsBelongs, dotsCollidesStatic, dhit)) {
                UnityVector3 hitNorm{ dhit.normal[0], dhit.normal[1], dhit.normal[2] };
                if (!isGroundSlope(hitNorm, 0.62f)) {
                    recordWallHit(zone, dir, { dhit.point[0], dhit.point[1], dhit.point[2] }, hitNorm);
                }
            }
        };

        struct ProbeDef { int zone; float sx, sy, sz; float dx, dy, dz; };
        const ProbeDef mainProbes[] = {
            { kZoneFront,  -0.62f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneFront,  -0.22f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneFront,   0.22f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneFront,   0.62f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneRear,   -0.62f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneRear,   -0.22f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneRear,    0.22f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneRear,    0.62f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ - doorHalfSpan, -1.00f, 0.0f,  0.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ,                -1.00f, 0.0f,  0.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ + doorHalfSpan, -1.00f, 0.0f,  0.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ,                -0.94f, 0.0f,  0.34f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ,                -0.94f, 0.0f, -0.34f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ - doorHalfSpan,  1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ,                 1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ + doorHalfSpan,  1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ,                 0.94f, 0.0f,  0.34f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ,                 0.94f, 0.0f, -0.34f },
            { kZoneFrontL, -lrStart, 0.62f, frontAxleZ,                 -1.00f, 0.0f,  0.00f },
            { kZoneFrontL, -lrStart, 0.62f, frontSideZ,                 -0.80f, 0.0f,  0.60f },
            { kZoneFrontL, -0.48f,   0.62f, fbStart,                    -0.44f, 0.0f,  0.90f },
            { kZoneFrontR,  lrStart, 0.62f, frontAxleZ,                  1.00f, 0.0f,  0.00f },
            { kZoneFrontR,  lrStart, 0.62f, frontSideZ,                  0.80f, 0.0f,  0.60f },
            { kZoneFrontR,  0.48f,   0.62f, fbStart,                     0.44f, 0.0f,  0.90f },
            { kZoneRearL,  -lrStart, 0.62f, rearAxleZ,                  -1.00f, 0.0f,  0.00f },
            { kZoneRearL,  -lrStart, 0.62f, rearSideZ,                  -0.80f, 0.0f, -0.60f },
            { kZoneRearL,  -0.48f,   0.62f,-fbStart,                    -0.44f, 0.0f, -0.90f },
            { kZoneRearR,   lrStart, 0.62f, rearAxleZ,                   1.00f, 0.0f,  0.00f },
            { kZoneRearR,   lrStart, 0.62f, rearSideZ,                   0.80f, 0.0f, -0.60f },
            { kZoneRearR,   0.48f,   0.62f,-fbStart,                     0.44f, 0.0f, -0.90f },
        };
        for (const auto& pd : mainProbes) {
            probeWallSphere(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz,
                            0.27f, dotsCollidesStatic, 0.72f, 2, true);
        }

        const ProbeDef barrierProbes[] = {
            { kZoneFront,   0.00f,   0.64f,  fbStart,                           0.00f, 0.0f,  1.00f },
            { kZoneRear,    0.00f,   0.64f, -fbStart,                           0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.64f, doorCenterZ - doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorL,  -lrStart, 0.64f, doorCenterZ + doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.64f, doorCenterZ - doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.64f, doorCenterZ + doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneFrontL, -0.45f,   0.64f, frontSideZ,                        -0.71f, 0.0f,  0.71f },
            { kZoneFrontR,  0.45f,   0.64f, frontSideZ,                         0.71f, 0.0f,  0.71f },
            { kZoneRearL,  -0.45f,   0.64f, rearSideZ,                         -0.71f, 0.0f, -0.71f },
            { kZoneRearR,   0.45f,   0.64f, rearSideZ,                          0.71f, 0.0f, -0.71f },
        };
        for (const auto& pd : barrierProbes) {
            probeWallSphere(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz,
                            0.38f, dotsCollidesBarrier, 0.88f, 0, false);
        }

        const ProbeDef highProbes[] = {
            { kZoneFront,   0.00f,   0.98f,  fbStart,    0.00f, 0.0f,  1.00f },
            { kZoneRear,    0.00f,   0.98f, -fbStart,    0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.98f, doorCenterZ,-1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.98f, doorCenterZ, 1.00f, 0.0f,  0.00f },
            { kZoneFrontL, -0.45f,   0.98f, frontSideZ, -0.71f, 0.0f,  0.71f },
            { kZoneFrontR,  0.45f,   0.98f, frontSideZ,  0.71f, 0.0f,  0.71f },
            { kZoneRearL,  -0.45f,   0.98f, rearSideZ,  -0.71f, 0.0f, -0.71f },
            { kZoneRearR,   0.45f,   0.98f, rearSideZ,   0.71f, 0.0f, -0.71f },
        };
        for (const auto& pd : highProbes) {
            probeWallSphere(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz,
                            0.25f, dotsCollidesStatic, 0.72f, 1, false);
        }

        const ProbeDef curbRays[] = {
            { kZoneFront,  -0.45f,   0.24f,  fbStart,                           0.00f, 0.0f,  1.00f },
            { kZoneFront,   0.45f,   0.24f,  fbStart,                           0.00f, 0.0f,  1.00f },
            { kZoneRear,   -0.45f,   0.24f, -fbStart,                           0.00f, 0.0f, -1.00f },
            { kZoneRear,    0.45f,   0.24f, -fbStart,                           0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.24f, doorCenterZ - doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorL,  -lrStart, 0.24f, doorCenterZ + doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.24f, doorCenterZ - doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.24f, doorCenterZ + doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneFrontL, -0.45f,   0.24f, frontSideZ,                        -0.71f, 0.0f,  0.71f },
            { kZoneFrontR,  0.45f,   0.24f, frontSideZ,                         0.71f, 0.0f,  0.71f },
            { kZoneRearL,  -0.45f,   0.24f, rearSideZ,                         -0.71f, 0.0f, -0.71f },
            { kZoneRearR,   0.45f,   0.24f, rearSideZ,                          0.71f, 0.0f, -0.71f },
        };
        for (const auto& pd : curbRays) {
            probeLowCurbRay(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz);
        }
    }

    enum SmoothSlot {
        kSmFront = 0, kSmRear, kSmDoorL, kSmDoorR,
        kSmFrontL, kSmFrontR, kSmRearL, kSmRearR,
        kSmSideL, kSmSideR, kSmNearest, kSmTotal
    };
    float flowSpeed = 6.0f;
    CarFX::GetParamByName(carfxTarget, "flowSpeed", &flowSpeed, 1);
    flowSpeed = std::clamp(flowSpeed, 0.3f, 60.0f);
    const float fadeCeiling = 10.5f;
    const float alphaGap = (flowSpeed >= 28.0f) ? 1.0f : (1.0f - expf(-flowSpeed * dt));
    const float alphaPos = (flowSpeed >= 28.0f) ? 1.0f : (1.0f - expf(-(flowSpeed * 1.45f) * dt));

    auto updateSmoothZone = [&](int slotIdx, const RawZoneHit& raw, float outVec[4]) {
        CarSpatialTracker::SmoothZone& zs = tr.zones[slotIdx];
        if (raw.hit && raw.gap < fadeCeiling) {
            zs.missAge = 0.0f;
            UnityVector3 rel = { raw.point.x - pPos.x, raw.point.y - pPos.y, raw.point.z - pPos.z };
            float lx = VecDot(rel, pRight);
            float ly = VecDot(rel, pUp);
            float lz = VecDot(rel, pFwd);
            if (!zs.valid || zs.gap >= fadeCeiling - 0.1f) {
                zs.valid = true;
                zs.localPt[0] = lx; zs.localPt[1] = ly; zs.localPt[2] = lz;
                const float entryStart = (std::min)(fadeCeiling, (std::max)(raw.gap + 2.8f, 3.8f));
                zs.gap = (alphaGap >= 0.999f) ? raw.gap : (entryStart + (raw.gap - entryStart) * alphaGap);
                zs.normal = raw.normal;
            } else {
                zs.localPt[0] += (lx - zs.localPt[0]) * alphaPos;
                zs.localPt[1] += (ly - zs.localPt[1]) * alphaPos;
                zs.localPt[2] += (lz - zs.localPt[2]) * alphaPos;
                zs.gap        += (raw.gap - zs.gap) * alphaGap;
                zs.normal.x   += (raw.normal.x - zs.normal.x) * alphaPos;
                zs.normal.y   += (raw.normal.y - zs.normal.y) * alphaPos;
                zs.normal.z   += (raw.normal.z - zs.normal.z) * alphaPos;
            }
        } else if (zs.valid) {
            zs.missAge += dt;
            if (zs.missAge > 0.09f || alphaGap >= 0.999f) {
                zs.gap += (fadeCeiling - zs.gap) * alphaGap;
                if (zs.gap >= fadeCeiling - 0.05f || alphaGap >= 0.999f) {
                    zs.valid = false;
                    zs.missAge = 0.0f;
                    zs.gap = 999.0f;
                }
            }
        }

        if (zs.valid) {
            outVec[0] = pPos.x + pRight.x * zs.localPt[0] + pUp.x * zs.localPt[1] + pFwd.x * zs.localPt[2];
            outVec[1] = pPos.y + pRight.y * zs.localPt[0] + pUp.y * zs.localPt[1] + pFwd.y * zs.localPt[2];
            outVec[2] = pPos.z + pRight.z * zs.localPt[0] + pUp.z * zs.localPt[1] + pFwd.z * zs.localPt[2];
            outVec[3] = zs.gap;
        } else {
            outVec[0] = outVec[1] = outVec[2] = 0.0f;
            outVec[3] = 999.0f;
        }
    };

    RawZoneHit rawSideL, rawSideR, rawNearest;
    for (int z : { kZoneFrontL, kZoneDoorL, kZoneRearL }) {
        if (rawZones[z].hit && (!rawSideL.hit || rawZones[z].gap < rawSideL.gap)) rawSideL = rawZones[z];
    }
    for (int z : { kZoneFrontR, kZoneDoorR, kZoneRearR }) {
        if (rawZones[z].hit && (!rawSideR.hit || rawZones[z].gap < rawSideR.gap)) rawSideR = rawZones[z];
    }
    for (int z = 0; z < kZoneCount; ++z) {
        if (rawZones[z].hit && (!rawNearest.hit || rawZones[z].gap < rawNearest.gap)) rawNearest = rawZones[z];
    }

    updateSmoothZone(kSmFront,   rawZones[kZoneFront],  sp.wallFrontHit);
    updateSmoothZone(kSmRear,    rawZones[kZoneRear],   sp.wallRearHit);
    updateSmoothZone(kSmDoorL,   rawZones[kZoneDoorL],  sp.wallDoorLeftHit);
    updateSmoothZone(kSmDoorR,   rawZones[kZoneDoorR],  sp.wallDoorRightHit);
    updateSmoothZone(kSmFrontL,  rawZones[kZoneFrontL], sp.wallFrontLeftHit);
    updateSmoothZone(kSmFrontR,  rawZones[kZoneFrontR], sp.wallFrontRightHit);
    updateSmoothZone(kSmRearL,   rawZones[kZoneRearL],  sp.wallRearLeftHit);
    updateSmoothZone(kSmRearR,   rawZones[kZoneRearR],  sp.wallRearRightHit);
    updateSmoothZone(kSmSideL,   rawSideL,              sp.wallLeftHit);
    updateSmoothZone(kSmSideR,   rawSideR,              sp.wallRightHit);
    updateSmoothZone(kSmNearest, rawNearest,            sp.wallNearestHit);

    sp.wallDirDist[0] = sp.wallFrontHit[3];
    sp.wallDirDist[1] = sp.wallRearHit[3];
    sp.wallDirDist[2] = sp.wallLeftHit[3];
    sp.wallDirDist[3] = sp.wallRightHit[3];

    if (tr.zones[kSmNearest].valid) {
        UnityVector3 n = VecNormalize(tr.zones[kSmNearest].normal);
        sp.wallNearestNormal[0] = n.x;
        sp.wallNearestNormal[1] = n.y;
        sp.wallNearestNormal[2] = n.z;
    }
    int activeMask = 0;
    if (sp.wallFrontHit[3] < fadeCeiling) activeMask |= 1;
    if (sp.wallRearHit[3]  < fadeCeiling) activeMask |= 2;
    if (sp.wallLeftHit[3]  < fadeCeiling) activeMask |= 4;
    if (sp.wallRightHit[3] < fadeCeiling) activeMask |= 8;
    sp.wallNearestNormal[3] = static_cast<float>(activeMask);

    // 2. Nearest other vehicle (OBB surface gap against all candidates excluding self)
    const UnityVector3 pCenter = {
        pPos.x + pUp.x * halfH,
        pPos.y + pUp.y * halfH,
        pPos.z + pUp.z * halfH
    };
    float bestCarGap = 999.0f;
    UnityVector3 bestOCenter{}, bestORight{ 1,0,0 }, bestOFwd{ 0,0,1 }, bestCClosest{};
    float bestOHw = 0.96f, bestOHh = 0.65f, bestOHl = 2.25f;

    for (const auto& cand : otherCars) {
        if (cand.ownerTag == selfOwnerTag) continue;
        float dx = cand.center.x - pCenter.x;
        float dy = cand.center.y - pCenter.y;
        float dz = cand.center.z - pCenter.z;
        if (dx * dx + dy * dy + dz * dz > 1600.0f) continue;

        UnityVector3 cOther = ClosestPointOnOBB(pCenter, cand.center, cand.right, cand.up, cand.fwd, cand.halfW, cand.halfH, cand.halfL);
        UnityVector3 cSelf = ClosestPointOnOBB(cOther, pCenter, pRight, pUp, pFwd, halfW, halfH, halfL);
        cOther = ClosestPointOnOBB(cSelf, cand.center, cand.right, cand.up, cand.fwd, cand.halfW, cand.halfH, cand.halfL);

        float gx = cOther.x - cSelf.x;
        float gy = cOther.y - cSelf.y;
        float gz = cOther.z - cSelf.z;
        float gap = sqrtf(gx * gx + gy * gy + gz * gz);

        if (gap < bestCarGap) {
            bestCarGap = gap;
            bestOCenter = cand.center; bestORight = cand.right; bestOFwd = cand.fwd; bestCClosest = cOther;
            bestOHw = cand.halfW; bestOHh = cand.halfH; bestOHl = cand.halfL;
        }
    }

    auto toLocalVec = [&](const UnityVector3& v, float out3[3]) {
        out3[0] = VecDot(v, pRight); out3[1] = VecDot(v, pUp); out3[2] = VecDot(v, pFwd);
    };
    auto toWorldVec = [&](const float in3[3]) -> UnityVector3 {
        return {
            pRight.x * in3[0] + pUp.x * in3[1] + pFwd.x * in3[2],
            pRight.y * in3[0] + pUp.y * in3[1] + pFwd.y * in3[2],
            pRight.z * in3[0] + pUp.z * in3[1] + pFwd.z * in3[2]
        };
    };

    if (bestCarGap < fadeCeiling) {
        UnityVector3 dc = { bestOCenter.x - pPos.x, bestOCenter.y - pPos.y, bestOCenter.z - pPos.z };
        UnityVector3 dl = { bestCClosest.x - pPos.x, bestCClosest.y - pPos.y, bestCClosest.z - pPos.z };
        toLocalVec(dc, tr.nearLocalCenter);
        toLocalVec(dl, tr.nearLocalClosest);
        toLocalVec(bestORight, tr.nearLocalRight);
        toLocalVec(bestOFwd,   tr.nearLocalFwd);
        tr.nearExtents[0] = bestOHw; tr.nearExtents[1] = bestOHh; tr.nearExtents[2] = bestOHl;

        if (!tr.nearValid || tr.nearGap >= fadeCeiling - 0.1f) {
            tr.nearValid = true;
            const float entryStart = (std::min)(fadeCeiling, (std::max)(bestCarGap + 2.8f, 3.8f));
            tr.nearGap = (alphaGap >= 0.999f) ? bestCarGap : (entryStart + (bestCarGap - entryStart) * alphaGap);
        } else {
            tr.nearGap += (bestCarGap - tr.nearGap) * alphaGap;
        }
        sp.nearCarPos[0] = bestOCenter.x; sp.nearCarPos[1] = bestOCenter.y; sp.nearCarPos[2] = bestOCenter.z;
        sp.nearCarPos[3] = tr.nearGap;
        sp.nearCarRight[0] = bestORight.x; sp.nearCarRight[1] = bestORight.y; sp.nearCarRight[2] = bestORight.z;
        sp.nearCarRight[3] = bestOHw;
        sp.nearCarFwd[0] = bestOFwd.x; sp.nearCarFwd[1] = bestOFwd.y; sp.nearCarFwd[2] = bestOFwd.z;
        sp.nearCarFwd[3] = bestOHl;
        sp.nearCarClosest[0] = bestCClosest.x; sp.nearCarClosest[1] = bestCClosest.y; sp.nearCarClosest[2] = bestCClosest.z;
        sp.nearCarClosest[3] = bestOHh;
    } else if (bestCarGap < 900.0f) {
        tr.nearValid = false;
        tr.nearGap = bestCarGap;
        sp.nearCarPos[0] = bestOCenter.x; sp.nearCarPos[1] = bestOCenter.y; sp.nearCarPos[2] = bestOCenter.z;
        sp.nearCarPos[3] = bestCarGap;
        sp.nearCarRight[0] = bestORight.x; sp.nearCarRight[1] = bestORight.y; sp.nearCarRight[2] = bestORight.z;
        sp.nearCarRight[3] = bestOHw;
        sp.nearCarFwd[0] = bestOFwd.x; sp.nearCarFwd[1] = bestOFwd.y; sp.nearCarFwd[2] = bestOFwd.z;
        sp.nearCarFwd[3] = bestOHl;
        sp.nearCarClosest[0] = bestCClosest.x; sp.nearCarClosest[1] = bestCClosest.y; sp.nearCarClosest[2] = bestCClosest.z;
        sp.nearCarClosest[3] = bestOHh;
    } else if (tr.nearValid) {
        tr.nearGap += (fadeCeiling - tr.nearGap) * alphaGap;
        if (tr.nearGap >= fadeCeiling - 0.05f || alphaGap >= 0.999f) {
            tr.nearValid = false;
            tr.nearGap = 999.0f;
        } else {
            UnityVector3 wC = toWorldVec(tr.nearLocalCenter);
            UnityVector3 wL = toWorldVec(tr.nearLocalClosest);
            UnityVector3 wR = VecNormalize(toWorldVec(tr.nearLocalRight));
            UnityVector3 wF = VecNormalize(toWorldVec(tr.nearLocalFwd));
            sp.nearCarPos[0] = pPos.x + wC.x; sp.nearCarPos[1] = pPos.y + wC.y; sp.nearCarPos[2] = pPos.z + wC.z;
            sp.nearCarPos[3] = tr.nearGap;
            sp.nearCarRight[0] = wR.x; sp.nearCarRight[1] = wR.y; sp.nearCarRight[2] = wR.z;
            sp.nearCarRight[3] = tr.nearExtents[0];
            sp.nearCarFwd[0] = wF.x; sp.nearCarFwd[1] = wF.y; sp.nearCarFwd[2] = wF.z;
            sp.nearCarFwd[3] = tr.nearExtents[2];
            sp.nearCarClosest[0] = pPos.x + wL.x; sp.nearCarClosest[1] = pPos.y + wL.y; sp.nearCarClosest[2] = pPos.z + wL.z;
            sp.nearCarClosest[3] = tr.nearExtents[1];
        }
    }

    // 3. Real vehicle slip/drift angle in degrees (from world velocity vs car forward axis)
    float vel[3] = { rawVel[0], rawVel[1], rawVel[2] };
    bool velOk = hasVel;
    if (!velOk && tr.hasPrevPos && dt > 1e-4f) {
        vel[0] = (pPos.x - tr.prevPos.x) / dt;
        vel[1] = (pPos.y - tr.prevPos.y) / dt;
        vel[2] = (pPos.z - tr.prevPos.z) / dt;
        velOk = true;
    }
    tr.prevPos = pPos;
    tr.hasPrevPos = true;

    float vFwd = vel[0] * pFwd.x + vel[1] * pFwd.y + vel[2] * pFwd.z;
    float vSide = vel[0] * pRight.x + vel[1] * pRight.y + vel[2] * pRight.z;
    float vPlanar = velOk ? sqrtf(vFwd * vFwd + vSide * vSide) : 0.0f;

    auto smoothStepF = [](float edge0, float edge1, float x) {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    float speedGate = smoothStepF(1.2f, 3.8f, vPlanar);
    float fwdCos = (vPlanar > 0.1f) ? (vFwd / vPlanar) : 1.0f;
    float revGate = smoothStepF(-0.78f, -0.30f, fwdCos);

    float rawSignedDeg = (vPlanar > 0.1f) ? (atan2f(vSide, vFwd) * 57.2957795f) : 0.0f;
    float targetSigned = rawSignedDeg * speedGate * revGate;
    float targetAbs = fabsf(targetSigned);

    const float angleRate = (std::max)(flowSpeed * 1.6f, 6.0f);
    const float alphaAngle = (flowSpeed >= 28.0f) ? 1.0f : (1.0f - expf(-angleRate * dt));
    tr.smoothDriftSigned += (targetSigned - tr.smoothDriftSigned) * alphaAngle;
    tr.smoothDriftAbs    += (targetAbs - tr.smoothDriftAbs) * alphaAngle;

    sp.driftState[0] = tr.smoothDriftAbs;
    sp.driftState[1] = tr.smoothDriftSigned;
    sp.driftState[2] = std::clamp(tr.smoothDriftAbs / 65.0f, 0.0f, 1.0f);
    sp.driftState[3] = vPlanar * 3.6f;

    CarFX::SetSpatialProximity(sp, carfxTarget);
}

void UnityBridge::UpdateCarFXSpatialProximityOnMainThread(const std::vector<LiveCarInfo>& liveSnapshot) {
    static CarSpatialTracker s_trackers[CarFX::kTargetCount]{};
    static std::chrono::steady_clock::time_point s_prevSpatTime{};

    auto nowTp = std::chrono::steady_clock::now();
    float dt = 0.016f;
    if (s_prevSpatTime.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(nowTp - s_prevSpatTime).count();
    }
    s_prevSpatTime = nowTp;
    if (dt > 0.40f) {
        for (auto& tr : s_trackers) tr.ResetSmooth();
    }
    dt = std::clamp(dt, 0.0005f, 0.08f);

    UnityVector3 camPos{};
    UnityVector4 camRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasCam = SafeGetMainCameraPoseSEH(reinterpret_cast<void*>(pfn_GetMainCameraNative),
                                           reinterpret_cast<void*>(pfn_Transform_get_position),
                                           reinterpret_cast<void*>(pfn_Transform_get_rotation),
                                           &camPos, &camRot);
    s_camAxesValid = hasCam && pfn_Transform_get_rotation != nullptr;
    if (s_camAxesValid) {
        UnityVector3 cr{}, cu{}, cf{};
        UnityBridge::QuatToAxes(camRot, cr, cu, cf);
        s_camAxes[0][0] = cr.x; s_camAxes[0][1] = cr.y; s_camAxes[0][2] = cr.z;
        s_camAxes[1][0] = cu.x; s_camAxes[1][1] = cu.y; s_camAxes[1][2] = cu.z;
        s_camAxes[2][0] = cf.x; s_camAxes[2][1] = cf.y; s_camAxes[2][2] = cf.z;
    }

    void* localRootTf = nullptr;
    UnityVector3 pPos{};
    UnityVector4 pRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasLocal = false;

    for (const auto& c : liveSnapshot) {
        if (c.isLocalPlayer) {
            localRootTf = c.rootTf;
            pPos = { c.x, c.y, c.z };
            pRot = { c.qx, c.qy, c.qz, c.qw };
            hasLocal = c.hasPos || c.hasRot;
            break;
        }
    }
    if (localRootTf && GetTransformPosRotDirect(localRootTf, pPos, pRot)) {
        if (!std::isnan(pPos.x) && !std::isnan(pPos.y) && !std::isnan(pPos.z)) {
            hasLocal = true;
        }
    }

    CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
    if (!hasLocal && player.carPtr) {
        pPos = { player.posX, player.posY, player.posZ };
        if (!std::isnan(pPos.x) && !std::isnan(pPos.y) && !std::isnan(pPos.z) &&
            (fabsf(pPos.x) + fabsf(pPos.y) + fabsf(pPos.z) > 0.1f)) {
            hasLocal = true;
        }
    }

    UnityVector3 pRight{ 1, 0, 0 }, pUp{ 0, 1, 0 }, pFwd{ 0, 0, 1 };
    float halfW = 0.96f, halfL = 2.20f, halfH = 0.65f;
    float frontAxleZ = halfL * 0.62f;
    float rearAxleZ  = -halfL * 0.62f;
    if (hasLocal) {
        if (localRootTf && (fabsf(pRot.x) + fabsf(pRot.y) + fabsf(pRot.z) + fabsf(pRot.w) > 0.01f)) {
            QuatToAxes(pRot, pRight, pUp, pFwd);
        } else if (player.carPtr) {
            const float* pm = player.transformMatrix.data();
            pRight = VecNormalize({ pm[0], pm[1], pm[2] });
            pUp    = VecNormalize({ pm[4], pm[5], pm[6] });
            pFwd   = VecNormalize({ pm[8], pm[9], pm[10] });
        }
        player = NativeCarX::GetPlayerCarTelemetry();
        if (player.carPtr) {
            float wheelMats[4][16];
            for (int w = 0; w < 4; ++w) {
                memcpy(wheelMats[w], player.wheels[w].matrix.data(), sizeof(float) * 16);
            }
            EstimateCarExtents(player.transformMatrix.data(), wheelMats, halfW, halfL);
            halfW = std::clamp(halfW, 0.75f, 1.35f);
            halfL = std::clamp(halfL, 1.65f, 2.75f);

            float wMag = fabsf(wheelMats[0][12]) + fabsf(wheelMats[1][12]) + fabsf(wheelMats[2][12]) + fabsf(wheelMats[3][12]);
            if (wMag > 0.01f) {
                float wz[4] = {};
                for (int w = 0; w < 4; ++w) {
                    UnityVector3 dw = { wheelMats[w][12] - pPos.x, wheelMats[w][13] - pPos.y, wheelMats[w][14] - pPos.z };
                    wz[w] = VecDot(dw, pFwd);
                }
                std::sort(wz, wz + 4);
                float rZ = 0.5f * (wz[0] + wz[1]);
                float fZ = 0.5f * (wz[2] + wz[3]);
                if (fZ - rZ > 1.35f && fZ - rZ < 4.40f) {
                    frontAxleZ = fZ;
                    rearAxleZ  = rZ;
                }
            }
        }
    }

    // Build shared OBB candidate table: player (-1) + remote cars (0..) + active ghosts (1000+slot)
    std::vector<VehicleOBBCandidate> allCars;
    allCars.reserve(liveSnapshot.size() + 8);
    if (hasLocal) {
        VehicleOBBCandidate selfCand;
        selfCand.ownerTag = -1;
        selfCand.center = { pPos.x + pUp.x * halfH, pPos.y + pUp.y * halfH, pPos.z + pUp.z * halfH };
        selfCand.right = pRight; selfCand.up = pUp; selfCand.fwd = pFwd;
        selfCand.halfW = halfW; selfCand.halfH = halfH; selfCand.halfL = halfL;
        allCars.push_back(selfCand);
    }
    int remoteIdx = 0;
    for (const auto& c : liveSnapshot) {
        if (c.isLocalPlayer) continue;
        UnityVector3 oPos{ c.x, c.y, c.z };
        UnityVector4 oRot{ c.qx, c.qy, c.qz, c.qw };
        if (c.rootTf) {
            GetTransformPosRotDirect(c.rootTf, oPos, oRot);
        } else if (!c.hasPos && !c.hasRot) {
            continue;
        }
        UnityVector3 oR, oU, oF;
        QuatToAxes(oRot, oR, oU, oF);
        VehicleOBBCandidate cand;
        cand.ownerTag = remoteIdx++;
        cand.center = { oPos.x + oU.x * 0.65f, oPos.y + oU.y * 0.65f, oPos.z + oU.z * 0.65f };
        cand.right = oR; cand.up = oU; cand.fwd = oF;
        cand.halfW = 0.96f; cand.halfH = 0.65f; cand.halfL = 2.25f;
        allCars.push_back(cand);
    }

    struct GhostSensorQuery {
        int slotIdx = 0;
        int carfxTarget = CarFX::kTargetNone;
        UnityVector3 pos{}, right{}, up{}, fwd{};
        float halfW = 0.96f, halfH = 0.65f, halfL = 2.25f;
        bool hasVel = false;
        float vel[3] = { 0, 0, 0 };
    };
    std::vector<GhostSensorQuery> activeGhostQueries;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (size_t idx = 0; idx < s_ghostSlots.size(); ++idx) {
            const GhostSlot& slot = s_ghostSlots[idx];
            if (!slot.gameObject || !slot.hasPose) continue;

            UnityVector3 gR = VecNormalize({ slot.lastMatrix[0], slot.lastMatrix[1], slot.lastMatrix[2] });
            UnityVector3 gU = VecNormalize({ slot.lastMatrix[4], slot.lastMatrix[5], slot.lastMatrix[6] });
            UnityVector3 gF = VecNormalize({ slot.lastMatrix[8], slot.lastMatrix[9], slot.lastMatrix[10] });

            VehicleOBBCandidate gCand;
            gCand.ownerTag = 1000 + static_cast<int>(idx);
            gCand.center = { slot.pos.x + gU.x * slot.halfHeight, slot.pos.y + gU.y * slot.halfHeight, slot.pos.z + gU.z * slot.halfHeight };
            gCand.right = gR; gCand.up = gU; gCand.fwd = gF;
            gCand.halfW = slot.halfWidth; gCand.halfH = slot.halfHeight; gCand.halfL = slot.halfLength;
            allCars.push_back(gCand);

            const int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(idx));
            if (gTarget != CarFX::kTargetNone && CarFX::IsTargetActive(gTarget)) {
                GhostSensorQuery q;
                q.slotIdx = static_cast<int>(idx);
                q.carfxTarget = gTarget;
                q.pos = slot.pos; q.right = gR; q.up = gU; q.fwd = gF;
                q.halfW = slot.halfWidth; q.halfH = slot.halfHeight; q.halfL = slot.halfLength;
                if (slot.replayTeleValid) {
                    q.vel[0] = slot.replayWorldVel.x;
                    q.vel[1] = slot.replayWorldVel.y;
                    q.vel[2] = slot.replayWorldVel.z;
                    q.hasVel = true;
                } else {
                    q.vel[0] = slot.vel.x; q.vel[1] = slot.vel.y; q.vel[2] = slot.vel.z;
                    q.hasVel = true;
                }
                activeGhostQueries.push_back(q);
            }
        }
    }

    if (hasLocal) {
        float pVel[3] = { 0.0f, 0.0f, 0.0f };
        bool hasPVel = player.carPtr && NativeCarX::GetCarVelocity(player.carPtr, pVel);
        EvaluateCarSpatialTarget(
            CarFX::kTargetBody, -1,
            pPos, pRight, pUp, pFwd, halfW, halfH, halfL, frontAxleZ, rearAxleZ,
            hasPVel, pVel, hasCam, camPos, reinterpret_cast<void*>(pfn_InternalRaycast),
            dt, allCars, s_trackers[CarFX::kTargetBody]);
    } else {
        CarFX::SetSpatialProximity(CarFX::SpatialProximityInput{}, CarFX::kTargetBody);
    }

    for (const auto& gq : activeGhostQueries) {
        const float gFrontAxleZ = gq.halfL * 0.62f;
        const float gRearAxleZ  = -gq.halfL * 0.62f;
        EvaluateCarSpatialTarget(
            gq.carfxTarget, 1000 + gq.slotIdx,
            gq.pos, gq.right, gq.up, gq.fwd, gq.halfW, gq.halfH, gq.halfL, gFrontAxleZ, gRearAxleZ,
            gq.hasVel, gq.vel, hasCam, camPos, reinterpret_cast<void*>(pfn_InternalRaycast),
            dt, allCars, s_trackers[gq.carfxTarget]);
    }
}

void UnityBridge::ResolveGhostCollisions() {
    CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
    if (!player.carPtr) return;

    float rb[29] = { 0 };
    if (!NativeCarX::GetCarRigidBody(player.carPtr, rb)) return;

    const float* pm = player.transformMatrix.data();
    UnityVector3 pRight = VecNormalize({ pm[0], pm[1], pm[2] });
    UnityVector3 pUp    = VecNormalize({ pm[4], pm[5], pm[6] });
    UnityVector3 pFwd   = VecNormalize({ pm[8], pm[9], pm[10] });

    float wheelMats[4][16];
    for (int w = 0; w < 4; ++w) {
        memcpy(wheelMats[w], player.wheels[w].matrix.data(), sizeof(float) * 16);
    }
    float ePw = 0.96f, ePl = 2.25f;
    float ePh = 0.65f;
    EstimateCarExtents(pm, wheelMats, ePw, ePl);

    UnityVector3 cP = {
        pm[12] + pUp.x * 0.45f,
        pm[13] + pUp.y * 0.45f,
        pm[14] + pUp.z * 0.45f
    };

    bool anyImpact = false;
    bool mirrorOn = s_ghostMirrorMode.load(std::memory_order_relaxed);
    float mirrorOff = s_ghostMirrorOffset;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (size_t idx = 0; idx < s_ghostSlots.size(); ++idx) {
        const GhostSlot& slot = s_ghostSlots[idx];
        if (!slot.gameObject || !slot.hasPose) continue;
        if (mirrorOn && idx == 0 && fabsf(mirrorOff) < 2.2f) continue;

        const float* gm = slot.lastMatrix;
        UnityVector3 gRight = VecNormalize({ gm[0], gm[1], gm[2] });
        UnityVector3 gUp    = VecNormalize({ gm[4], gm[5], gm[6] });
        UnityVector3 gFwd   = VecNormalize({ gm[8], gm[9], gm[10] });

        UnityVector3 cG = {
            gm[12] + gUp.x * 0.45f,
            gm[13] + gUp.y * 0.45f,
            gm[14] + gUp.z * 0.45f
        };

        if (fabsf(cP.y - cG.y) > (ePh + slot.halfHeight)) continue;

        UnityVector3 dC = { cP.x - cG.x, cP.y - cG.y, cP.z - cG.z };

        // Quick broadphase sphere reject (~5.2m combined radius)
        if ((dC.x * dC.x + dC.y * dC.y + dC.z * dC.z) > 28.0f) continue;

        UnityVector3 axes[4] = { pRight, pFwd, gRight, gFwd };
        float minOv = 1e9f;
        UnityVector3 bestAxis = pRight;
        bool separated = false;

        for (int a = 0; a < 4; ++a) {
            UnityVector3 axis = VecNormalize(axes[a]);
            float rP = ePw * fabsf(VecDot(axis, pRight)) + ePl * fabsf(VecDot(axis, pFwd));
            float rG = slot.halfWidth * fabsf(VecDot(axis, gRight)) + slot.halfLength * fabsf(VecDot(axis, gFwd));
            float dist = fabsf(VecDot(dC, axis));
            float ov = (rP + rG) - dist;
            if (ov <= 0.0f) {
                separated = true;
                break;
            }
            if (ov < minOv) {
                minOv = ov;
                bestAxis = axis;
            }
        }

        if (separated || minOv <= 0.002f) continue;

        // Ensure normal points FROM Ghost TOWARD Player
        if (VecDot(dC, bestAxis) < 0.0f) {
            bestAxis.x = -bestAxis.x;
            bestAxis.y = -bestAxis.y;
            bestAxis.z = -bestAxis.z;
        }

        float pen = (std::min)(minOv, 0.75f);

        // Closest point on player OBB to ghost center -> lever arm rP (horizontal)
        UnityVector3 toG = { -dC.x, -dC.y, -dC.z };
        float localRight = std::clamp(VecDot(toG, pRight), -ePw, ePw);
        float localFwd   = std::clamp(VecDot(toG, pFwd),   -ePl, ePl);
        UnityVector3 rP = {
            localRight * pRight.x + localFwd * pFwd.x,
            0.0f,
            localRight * pRight.z + localFwd * pFwd.z
        };

        float wy = rb[27];
        UnityVector3 vContact = {
            rb[23] + wy * rP.z,
            rb[24],
            rb[25] - wy * rP.x
        };

        UnityVector3 vRel = {
            vContact.x - slot.vel.x,
            vContact.y - slot.vel.y,
            vContact.z - slot.vel.z
        };

        float vN = VecDot(vRel, bestAxis);
        float minSepVel = (std::min)(pen * 15.0f, 6.5f);

        if (vN < minSepVel) {
            float dvN = (vN < -0.4f)
                ? (-(1.0f + 0.25f) * vN + minSepVel * 0.45f)
                : (minSepVel - vN);
            dvN = std::clamp(dvN, 0.0f, 16.0f);

            // Tangential metal-on-metal rub friction
            UnityVector3 vTan = {
                vRel.x - bestAxis.x * vN,
                vRel.y - bestAxis.y * vN,
                vRel.z - bestAxis.z * vN
            };
            float tanLen = sqrtf(vTan.x * vTan.x + vTan.y * vTan.y + vTan.z * vTan.z);
            UnityVector3 dV = { bestAxis.x * dvN, bestAxis.y * dvN, bestAxis.z * dvN };
            if (tanLen > 0.2f && dvN > 0.05f) {
                float fricMag = (std::min)(tanLen * 0.25f, dvN * 0.14f);
                dV.x -= (vTan.x / tanLen) * fricMag;
                dV.z -= (vTan.z / tanLen) * fricMag;
            }

            rb[23] += dV.x;
            rb[24] += dV.y * 0.20f;
            rb[25] += dV.z;

            float tauY = (rP.z * dV.x - rP.x * dV.z) * 0.35f;
            rb[27] += std::clamp(tauY, -1.5f, 1.5f);
            anyImpact = true;
        }
    }

    if (anyImpact) {
        NativeCarX::SetCarRigidBody(player.carPtr, rb);
    }
}

static Float4x4 BuildFloat4x4FromPosQuat(const UnityVector3& pos, const UnityVector4& qRaw) {
    float lenSq = qRaw.x * qRaw.x + qRaw.y * qRaw.y + qRaw.z * qRaw.z + qRaw.w * qRaw.w;
    UnityVector4 q = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (lenSq > 1e-8f) {
        float invLen = 1.0f / sqrtf(lenSq);
        q.x = qRaw.x * invLen;
        q.y = qRaw.y * invLen;
        q.z = qRaw.z * invLen;
        q.w = qRaw.w * invLen;
    }

    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    Float4x4 out{};
    // Column 0 (Right), Column 1 (Up), Column 2 (Forward), Column 3 (Translation)
    out.m[0]  = 1.0f - 2.0f * (yy + zz);
    out.m[1]  = 2.0f * (xy + wz);
    out.m[2]  = 2.0f * (xz - wy);
    out.m[3]  = 0.0f;

    out.m[4]  = 2.0f * (xy - wz);
    out.m[5]  = 1.0f - 2.0f * (xx + zz);
    out.m[6]  = 2.0f * (yz + wx);
    out.m[7]  = 0.0f;

    out.m[8]  = 2.0f * (xz + wy);
    out.m[9]  = 2.0f * (yz - wx);
    out.m[10] = 1.0f - 2.0f * (xx + yy);
    out.m[11] = 0.0f;

    out.m[12] = pos.x;
    out.m[13] = pos.y;
    out.m[14] = pos.z;
    out.m[15] = 1.0f;
    return out;
}

bool UnityBridge::GetPlayerCurrentPose(TeleportPose& outPose, std::string* outCarName) {
    outPose = TeleportPose{};
    if (outCarName) outCarName->clear();

    if (IsOnMainThread()) {
        RefreshLiveCarListOnMainThread(false);
    }

    void* carPtr = nullptr;
    void* rootTf = nullptr;
    std::string unityName;
    UnityVector3 cachedPos{};
    UnityVector4 cachedRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasCachedPos = false;
    bool hasCachedRot = false;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer) {
                carPtr = c.carPtr;
                rootTf = c.rootTf;
                unityName = c.unityName;
                if (c.hasPos) {
                    cachedPos = { c.x, c.y, c.z };
                    hasCachedPos = true;
                }
                if (c.hasRot) {
                    cachedRot = { c.qx, c.qy, c.qz, c.qw };
                    hasCachedRot = true;
                }
                break;
            }
        }
        if (!carPtr) {
            for (const auto& sc : s_sceneCarCache) {
                if (sc.isPlayer) {
                    carPtr = sc.nativeCarPtr;
                    rootTf = sc.rootTf;
                    unityName = sc.unityName;
                    if (sc.hasPos) { cachedPos = sc.pos; hasCachedPos = true; }
                    if (sc.hasRot) { cachedRot = sc.rot; hasCachedRot = true; }
                    break;
                }
            }
        }
    }

    CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
    if (!carPtr) {
        carPtr = tele.carPtr ? tele.carPtr : NativeCarX::GetPlayerCarPtr();
    }
    if (!carPtr && !rootTf) return false;
    if (outCarName) *outCarName = unityName;

    if (carPtr && NativeCarX::GetCarRigidBody(carPtr, outPose.rb)) {
        outPose.hasRb = true;
        NativeCarX::GetCarVelocity(carPtr, outPose.linVel);
        NativeCarX::GetCarAngularVelocity(carPtr, outPose.angVel);
    }

    UnityVector3 uPos = hasCachedPos ? cachedPos : UnityVector3{ tele.posX, tele.posY, tele.posZ };
    UnityVector4 uRot = hasCachedRot ? cachedRot : MatrixToQuaternion(tele.transformMatrix.data());
    if (rootTf && IsOnMainThread()) {
        UnityVector3 tfPos{};
        UnityVector4 tfRot{};
        if (GetTransformPosRotDirect(rootTf, tfPos, tfRot)) {
            uPos = tfPos;
            uRot = tfRot;
        }
    }

    outPose.pos[0] = uPos.x; outPose.pos[1] = uPos.y; outPose.pos[2] = uPos.z;
    outPose.rot[0] = uRot.x; outPose.rot[1] = uRot.y; outPose.rot[2] = uRot.z; outPose.rot[3] = uRot.w;
    outPose.hasQuat = true;
    return true;
}

bool UnityBridge::ApplyTeleportPlayerInternal(const TeleportPose& target, int velMode) {
    if (!IsOnMainThread()) return false;

    // Ensure our Unity car cache & CarBehaviour::m_isPlayer mapping are fresh
    RefreshLiveCarListOnMainThread(false);

    void* car = nullptr;
    void* rootTf = nullptr;
    void* raceCarPtr = nullptr;
    std::string carName;

    auto findLocalFromCache = [&]() {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer) {
                car = c.carPtr;
                rootTf = c.rootTf;
                raceCarPtr = c.raceCarPtr;
                carName = c.unityName;
                return;
            }
        }
        for (const auto& sc : s_sceneCarCache) {
            if (sc.isPlayer) {
                car = sc.nativeCarPtr;
                rootTf = sc.rootTf;
                raceCarPtr = sc.raceCarPtr;
                carName = sc.unityName;
                return;
            }
        }
    };

    findLocalFromCache();
    if (!raceCarPtr || !rootTf || !car) {
        RefreshLiveCarListOnMainThread(true);
        findLocalFromCache();
    }
    if (!car) {
        CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
        car = player.carPtr ? player.carPtr : NativeCarX::GetPlayerCarPtr();
    }
    if (!car && !raceCarPtr && !rootTf) return false;

    UnityVector3 uPos = {
        target.hasQuat ? target.pos[0] : target.rb[20],
        target.hasQuat ? target.pos[1] : target.rb[21],
        target.hasQuat ? target.pos[2] : target.rb[22]
    };
    UnityVector4 uRot = { target.rot[0], target.rot[1], target.rot[2], target.rot[3] };
    if (!target.hasQuat) {
        // Fallback: derive quaternion from 3x3 rotation in rb[4..15]
        float m16[16] = {
            target.rb[4], target.rb[5],  target.rb[6],  0.0f,
            target.rb[8], target.rb[9],  target.rb[10], 0.0f,
            target.rb[12],target.rb[13], target.rb[14], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        uRot = MatrixToQuaternion(m16);
    }

    UnityVector3 linVel{ 0.0f, 0.0f, 0.0f };
    UnityVector3 angVel{ 0.0f, 0.0f, 0.0f };
    if (velMode == 1) {
        linVel = {
            target.hasRb ? target.rb[23] : target.linVel[0],
            target.hasRb ? target.rb[24] : target.linVel[1],
            target.hasRb ? target.rb[25] : target.linVel[2]
        };
        angVel = {
            target.hasRb ? target.rb[26] : target.angVel[0],
            target.hasRb ? target.rb[27] : target.angVel[1],
            target.hasRb ? target.rb[28] : target.angVel[2]
        };
    } else if (velMode == 2 && car) {
        float curV[3] = { 0 }, curW[3] = { 0 };
        if (NativeCarX::GetCarVelocity(car, curV)) {
            linVel = { curV[0], curV[1], curV[2] };
        }
        if (NativeCarX::GetCarAngularVelocity(car, curW)) {
            angVel = { curW[0], curW[1], curW[2] };
        }
    }

    // 1. Vanilla CarX2 Teleport pipeline via RaceCar (IRaceCar):
    //    ForceVelocityChange + ForceTransformChange fires ForceChangeForcesCalled & ForceTransformChangeCalled ->
    //    CarProxy pushes CarProxyLocationComponent & CarProxyVelocityComponent into ECS CarWriteTransformSystem /
    //    PhysicsRootWriteTransformSystem so the ECS PhysicsRoot, wheels, rigidbody & camera stay 100% synced.
    HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
    uintptr_t gaBase = reinterpret_cast<uintptr_t>(hGA);
    Float4x4 mat4 = BuildFloat4x4FromPosQuat(uPos, uRot);
    bool vanillaOk = SafeCallRaceCarTeleportSEH(gaBase, raceCarPtr, &mat4, &linVel, &angVel);

    // 2. Sync Unity Transform immediately on MainThread (matches PlaceOnTrack -> SetPositionAndRotation)
    bool tfOk = rootTf && SetTransformPosRotDirect(rootTf, uPos, uRot);

    // Note: Do NOT overwrite NativeCarX rb[20..22] with world XYZ (in CarX2 rb[20..22] is a local/CoM offset ~ (0, 0.43, 0.06));
    // CarProxyLocationComponent & CarProxyVelocityComponent triggered by RaceCar::ForceTransformChange / ForceVelocityChange
    // propagate the new world pose & velocity to ECS CarWriteTransformSystem / PhysicsRootWriteTransformSystem cleanly.
    // Only touch NativeCarX::SetCarRigidBody for velocity updates if vanilla RaceCar failed.
    bool nativeOk = false;
    if (!vanillaOk && car) {
        float rb[29] = { 0 };
        if (NativeCarX::GetCarRigidBody(car, rb)) {
            if (target.hasRb) {
                for (int i = 4; i < 16; ++i) rb[i] = target.rb[i];
            } else if (target.hasQuat) {
                for (int i = 0; i < 12; ++i) rb[4 + i] = mat4.m[i];
            }
            rb[23] = linVel.x; rb[24] = linVel.y; rb[25] = linVel.z;
            rb[26] = angVel.x; rb[27] = angVel.y; rb[28] = angVel.z;
            nativeOk = NativeCarX::SetCarRigidBody(car, rb);
        }
    }

    char msg[256];
    sprintf_s(msg, "Vanilla teleport: car='%s' pos=(%.2f,%.2f,%.2f) velMode=%d raceCar=%d tf=%d native=%d",
              carName.c_str(), uPos.x, uPos.y, uPos.z, velMode,
              vanillaOk ? 1 : 0, tfOk ? 1 : 0, nativeOk ? 1 : 0);
    Logger::Info(msg);

    return vanillaOk || tfOk || nativeOk;
}

bool UnityBridge::RequestTeleportPlayerPose(const TeleportPose& pose, int velMode) {
    if (!NativeCarX::IsInSession()) return false;
    if (IsOnMainThread()) {
        return ApplyTeleportPlayerInternal(pose, velMode);
    }
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_pendingTeleportPose = pose;
    s_pendingTeleportVelMode = velMode;
    s_pendingTeleport.store(true, std::memory_order_release);
    return true;
}

bool UnityBridge::RequestTeleportPlayerRigidBody(const float rb[29], int velMode) {
    if (!rb || !NativeCarX::IsInSession()) return false;
    TeleportPose pose{};
    memcpy(pose.rb, rb, sizeof(pose.rb));
    pose.hasRb = true;
    pose.hasQuat = false;
    pose.pos[0] = rb[20]; pose.pos[1] = rb[21]; pose.pos[2] = rb[22];
    pose.linVel[0] = rb[23]; pose.linVel[1] = rb[24]; pose.linVel[2] = rb[25];
    pose.angVel[0] = rb[26]; pose.angVel[1] = rb[27]; pose.angVel[2] = rb[28];
    return RequestTeleportPlayerPose(pose, velMode);
}

// ================= Public Control APIs =================

void UnityBridge::SetGhostMirrorMode(bool enabled, float offsetRight) {
    s_ghostMirrorOffset = offsetRight;
    s_ghostMirrorMode.store(enabled, std::memory_order_relaxed);
    if (enabled && !HasGhostVisual()) {
        SpawnGhostVisual();
    }
}

bool UnityBridge::IsGhostMirrorMode() {
    return s_ghostMirrorMode.load(std::memory_order_relaxed);
}

void UnityBridge::ApplyReplayFrameToGhost(const ReplayFrame& frame, float offsetRight) {
    if (IsOnMainThread()) {
        bool needSpawn = false;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            needSpawn = (s_ghostSlots.empty() || !s_ghostSlots[0].gameObject);
        }
        if (needSpawn && NativeCarX::IsInSession()) {
            SpawnGhostSlotInternal(0);
        }
        UnityVector3 worldOffset = {
            frame.transformMatrix[0] * offsetRight,
            frame.transformMatrix[1] * offsetRight,
            frame.transformMatrix[2] * offsetRight
        };
        ApplyGhostPoseToSlot(0, frame.transformMatrix, frame.wheelMatrices, frame.caliperMatrices, frame.steerAngle, worldOffset, s_lastMainDt);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_pendingReplayFrameData = frame;
    s_pendingReplayOffset = offsetRight;
    s_pendingReplayApply.store(true);
}

void UnityBridge::ApplyReplayTimeToAllGhosts(float targetTime, float offsetRight) {
    if (IsOnMainThread()) {
        ApplyAllReplayTracksAtTimeInternal(targetTime, offsetRight, s_lastMainDt);
        return;
    }

    s_pendingMultiReplayTime = targetTime;
    s_pendingMultiReplayOffset = offsetRight;
    s_pendingMultiReplayApply.store(true);
}

void UnityBridge::SetGhostCollisionEnabled(bool enabled) {
    bool prev = s_ghostCollisionEnabled.exchange(enabled, std::memory_order_relaxed);
    if (prev == enabled) return;

    if (IsOnMainThread()) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (auto& slot : s_ghostSlots) {
            if (slot.gameObject) {
                ApplySlotCollisionVisuals(slot, enabled);
            }
        }
    } else {
        s_pendingCollisionSync.store(true);
    }
    Logger::Info(std::string("Ghost Collision ") + (enabled ? "ENABLED" : "DISABLED"));
}

bool UnityBridge::IsGhostCollisionEnabled() {
    return s_ghostCollisionEnabled.load(std::memory_order_relaxed);
}

void UnityBridge::SetGhostMirrorPlayerShader(bool enabled) {
    s_ghostMirrorPlayerShader.store(enabled, std::memory_order_relaxed);
}

bool UnityBridge::IsGhostMirrorPlayerShader() {
    return s_ghostMirrorPlayerShader.load(std::memory_order_relaxed);
}

void* UnityBridge::SpawnGhostVisual() {
    s_pendingDestroy.store(false);
    if (IsOnMainThread()) {
        return SpawnGhostSlotInternal(0);
    }
    if (!NativeCarX::IsInSession()) {
        return nullptr;
    }
    s_pendingSpawn.store(true);
    return nullptr;
}

bool UnityBridge::DestroyGhostVisual() {
    s_ghostMirrorMode.store(false);
    s_pendingReplayApply.store(false);
    s_pendingMultiReplayApply.store(false);
    s_pendingSpawn.store(false);
    if (IsOnMainThread()) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        bool had = !s_ghostSlots.empty();
        DestroyAllGhostSlotsInternal();
        return had;
    }
    s_pendingDestroy.store(true);
    return true;
}

bool UnityBridge::HasGhostVisual() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    bool hasAny = false;
    for (const auto& s : s_ghostSlots) {
        if (s.gameObject) { hasAny = true; break; }
    }
    return hasAny || (s_pendingSpawn.load() && !s_pendingDestroy.load());
}

void* UnityBridge::GetGhostVisualGameObject() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_ghostSlots.empty()) return nullptr;
    return s_ghostSlots[0].gameObject;
}

int UnityBridge::GetGhostCount() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    int cnt = 0;
    for (const auto& s : s_ghostSlots) {
        if (s.gameObject) cnt++;
    }
    return cnt;
}

void UnityBridge::SetGhostPositionRotation(const UnityVector3& pos, const UnityVector4& rot) {
    if (IsOnMainThread()) {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (!s_ghostSlots.empty() && s_ghostSlots[0].transform) {
            SetTransformPosRotDirect(s_ghostSlots[0].transform, pos, rot);
        }
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_pendingPos = pos;
    s_pendingRot = rot;
    s_pendingTransform.store(true);
}

bool UnityBridge::GetMainCameraPose(UnityVector3& outPos, UnityVector4& outRot) {
    return SafeGetMainCameraPoseSEH(
        reinterpret_cast<void*>(pfn_GetMainCameraNative),
        reinterpret_cast<void*>(pfn_Transform_get_position),
        reinterpret_cast<void*>(pfn_Transform_get_rotation),
        &outPos, &outRot
    );
}

bool UnityBridge::RaycastWorld(const UnityVector3& origin, const UnityVector3& dir, float maxDist, UnityRaycastHit& outHit, int layerMask) {
    if (!pfn_InternalRaycast) return false;
    UnityRay ray{ origin, dir };
    return SafePhysicsRaycastSEH(reinterpret_cast<void*>(pfn_InternalRaycast), &ray, maxDist, &outHit, layerMask);
}

void UnityBridge::RequestRepairPlayerCar() {
    if (IsOnMainThread()) {
        ApplyRepairPlayerCarInternal();
        return;
    }
    s_pendingRepairCar.store(true);
}

static void* SafeGetCarModelSEH(void* carBehaviour) {
    if (!carBehaviour) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(carBehaviour) + 0x60);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void* SafeGetDeformationBehavioursListSEH(void* deformCtrl) {
    if (!deformCtrl) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(deformCtrl) + 0x20);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool SafeInvokeCarRepairSEH(void* pfn, void* carModel, uint8_t resetVisual) {
    if (!pfn || !carModel) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, uint8_t, void*)>(pfn)(carModel, resetVisual, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeInvokeVisualRepairSEH(void* pfn, void* carModel) {
    if (!pfn || !carModel) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(pfn)(carModel, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeInvokeFullCarRepairSEH(void* pfn, void* defBehavList) {
    if (!pfn || !defBehavList) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(pfn)(defBehavList, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UnityBridge::ApplyRepairPlayerCarInternal() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    void* activePlayer = NativeCarX::GetPlayerCarPtr();
    if (activePlayer) {
        NativeCarX::ResetCarDamage(activePlayer);
        NativeCarX::ResetTireWear(activePlayer);
    }

    HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
    typedef void (__fastcall *t_CarRepair)(void* carModel, uint8_t resetVisual, void* method);
    typedef void (__fastcall *t_VisualRepair)(void* carModel, void* method);
    typedef void (__fastcall *t_FullCarRepair)(void* list, void* method);

    static t_CarRepair pfn_CarRepair = nullptr;
    static t_VisualRepair pfn_VisualRepair = nullptr;
    static t_FullCarRepair pfn_FullCarRepair = nullptr;
    static bool s_repairsResolved = false;

    if (!s_repairsResolved && hGA) {
        s_repairsResolved = true;
        pfn_CarRepair = reinterpret_cast<t_CarRepair>(PatternScanner::FindOrFallback(hGA,
            "40 53 41 55 48 83 EC 38 80 3D ? ? ? ? 00 44 0F B6 EA 48 8B D9",
            GameBuild::Pick({ 0x12FEC20, 0x17C3EE0 }), "CarRepair"));
        pfn_VisualRepair = reinterpret_cast<t_VisualRepair>(PatternScanner::FindFunctionEntryOrFallback(hGA,
            "48 89 6C 24 30 48 89 74 24 38 48 89 7C 24 40 4C 89 74 24 48 48 85 DB 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? B9 02 00 00 00 4C 8B C3",
            GameBuild::Pick({ 0x1108340, 0x15CFBC0 }), 256, "VisualRepair"));
        // The function's own prologue has an identical twin in release, so resolve it through its unique call site.
        pfn_FullCarRepair = reinterpret_cast<t_FullCarRepair>(PatternScanner::FindRipOrFallback(hGA,
            "48 8B 4C 24 20 E8 ? ? ? ? 48 81 C4 C0 00 00 00", 5, 1,
            GameBuild::Pick({ 0x1E570D0, 0x2426DD0 }), 5, "FullCarRepair"));
    }

    bool repaired = false;
    for (const auto& sc : s_sceneCarCache) {
        bool isPlayerCar = sc.isPlayer || (activePlayer && sc.nativeCarPtr == activePlayer);
        if (isPlayerCar && sc.carBehaviour) {
            void* carModel = SafeGetCarModelSEH(sc.carBehaviour);
            if (carModel) {
                if (pfn_CarRepair && SafeInvokeCarRepairSEH(reinterpret_cast<void*>(pfn_CarRepair), carModel, 1)) {
                    Logger::Info("CarExtensions::Repair executed on player car.");
                    repaired = true;
                }
                if (pfn_VisualRepair && SafeInvokeVisualRepairSEH(reinterpret_cast<void*>(pfn_VisualRepair), carModel)) {
                    Logger::Info("CarExtensions::VisualRepair executed on player car.");
                    repaired = true;
                }
            }

            void* deformCtrl = SafeFindDeformationControllerSEH(sc.carBehaviour);
            if (deformCtrl && pfn_FullCarRepair) {
                void* defBehavList = SafeGetDeformationBehavioursListSEH(deformCtrl);
                if (defBehavList) {
                    if (SafeInvokeFullCarRepairSEH(reinterpret_cast<void*>(pfn_FullCarRepair), defBehavList)) {
                        Logger::Info("CarDeformationFeatureController::FullCarRepair executed on player car.");
                        repaired = true;
                    }
                }
            }
            break;
        }
    }

    if (!repaired) {
        Logger::Warn("ApplyRepairPlayerCarInternal: player car or carModel not found in scene car cache.");
    }
}


