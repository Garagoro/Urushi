#include "unity_bridge.h"
#include "carfx.h"
#include "ecs_collision.h"
#include "native_carx.h"
#include "replay_engine.h"
#include "hook_manager.h"
#include "lua_engine.h"
#include "logger.h"
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
std::atomic<bool> UnityBridge::s_pendingCollisionSync{ false };
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

    __try {
        uintptr_t go = reinterpret_cast<uintptr_t>(rootGo);
        if (!IsReadableMem(go, 0x38)) return false;
        uintptr_t compArr = *reinterpret_cast<uintptr_t*>(go + 0x20);
        uint32_t compCount = *reinterpret_cast<uint32_t*>(go + 0x30);
        if (compCount == 0 || compCount > 256 || !IsReadableMem(compArr, static_cast<size_t>(compCount) * 16)) return false;

        uintptr_t carBehaviour = 0;
        for (uint32_t ci = 0; ci < compCount; ++ci) {
            uint32_t cid = *reinterpret_cast<uint32_t*>(compArr + ci * 16);
            if (cid != 66) continue; // MonoBehaviour
            uintptr_t natComp = *reinterpret_cast<uintptr_t*>(compArr + ci * 16 + 8);
            if (!IsReadableMem(natComp, 0x20)) continue;
            uintptr_t gcSlot = *reinterpret_cast<uintptr_t*>(natComp + 0x18);
            if (!IsReadableMem(gcSlot & ~7ull, sizeof(uintptr_t))) continue;
            uintptr_t managed = *reinterpret_cast<uintptr_t*>(gcSlot & ~7ull);
            if ((managed & 7) || !IsReadableMem(managed, 0x68)) continue;
            uintptr_t klass = *reinterpret_cast<uintptr_t*>(managed);
            if (!IsReadableMem(klass, 0x18)) continue;
            const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
            if (!IsReadableMem(reinterpret_cast<uintptr_t>(kname), 16)) continue;
            if (kname[0] == 'C' && kname[1] == 'a' && kname[2] == 'r' &&
                strcmp(kname, "CarBehaviour") == 0) {
                carBehaviour = managed;
                break;
            }
        }
        if (carBehaviour < 0x10000) return false;

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
        for (uint32_t i = 0; i < outerCount; ++i) {
            uintptr_t e = outerEntries + 0x20 + static_cast<size_t>(i) * 24;
            int32_t hc = *reinterpret_cast<int32_t*>(e + 0);
            if (hc < 0) continue;
            uint32_t tid = *reinterpret_cast<uint32_t*>(e + 8);
            if (tid == targetTypeId) {
                subDict = *reinterpret_cast<uintptr_t*>(e + 16);
                break;
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

struct UnityManagedSpanWrapper {
    const wchar_t* begin;
    int32_t length;
};

typedef void* (*t_UP_InstanceIDToObject)(const int32_t* instanceId);
typedef int32_t (*t_UP_MatGetRenderQueue)(void* nativeMat);
typedef void (*t_UP_MatSetRenderQueue)(void* nativeMat, int32_t rq);
typedef void (*t_UP_MatSetShaderPassEnabled)(void* nativeMat, const UnityManagedSpanWrapper* passName, bool enabled);
typedef void* (*t_UP_GetOrAssignInstantiatedMat)(void* nativeRenderer, int32_t slotIdx);
typedef void (*t_UP_RendererSetMat0)(void* nativeRenderer, void* nativeMat);
typedef void (*t_UP_RendererSetMatAt)(void* nativeRenderer, const int32_t* pMatId, uint32_t slotIdx);

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

static bool SafeIsBodyMaterialIdSEH(uintptr_t unityBase, int32_t matId) {
    if (!unityBase || matId >= 0) return false;
    const auto pfnIdToObj = reinterpret_cast<t_UP_InstanceIDToObject>(unityBase + 0x107480);
    __try {
        void* natMat = pfnIdToObj(&matId);
        const uintptr_t mAddr = reinterpret_cast<uintptr_t>(natMat);
        if (!IsReadableMem(mAddr, 0x38)) return false;
        const char* matName = *reinterpret_cast<const char* const*>(mAddr + 0x20);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(matName), 16)) return false;
        return strcmp(matName, "car_body(Clone)") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int SafeClassifyMaterialSEH(uintptr_t unityBase, int32_t matId, int32_t* outCurQueue) {
    if (outCurQueue) *outCurQueue = 0;
    if (!unityBase || matId == 0) return kMatSkip;
    const auto pfnIdToObj = reinterpret_cast<t_UP_InstanceIDToObject>(unityBase + 0x107480);
    const auto pfnGetRq = reinterpret_cast<t_UP_MatGetRenderQueue>(unityBase + 0x95eb0);
    __try {
        void* natMat = pfnIdToObj(&matId);
        const uintptr_t mAddr = reinterpret_cast<uintptr_t>(natMat);
        if (!IsReadableMem(mAddr, 0x38)) return kMatSkip;
        const char* matName = *reinterpret_cast<const char* const*>(mAddr + 0x20);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(matName), 16)) return kMatSkip;

        if (strcmp(matName, "car_body(Clone)") == 0) return kMatBody;
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
    const auto pfnIdToObj = reinterpret_cast<t_UP_InstanceIDToObject>(unityBase + 0x107480);
    const auto pfnGetOrClone = reinterpret_cast<t_UP_GetOrAssignInstantiatedMat>(unityBase + 0x47aa00);
    const auto pfnSetMat0 = reinterpret_cast<t_UP_RendererSetMat0>(unityBase + 0x8ce50);

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
    const auto pfnIdToObj = reinterpret_cast<t_UP_InstanceIDToObject>(unityBase + 0x107480);
    const auto pfnSetMat0 = reinterpret_cast<t_UP_RendererSetMat0>(unityBase + 0x8ce50);
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

static bool SafeSyncMaterialQueueAndDepthSEH(uintptr_t unityBase, int32_t matId, bool applyOverride,
                                             int32_t targetQueue, bool requireBodyName,
                                             int32_t savedOrigQueue, bool* outModified, int32_t* outOrigQueue) {
    if (outModified) *outModified = false;
    if (outOrigQueue) *outOrigQueue = savedOrigQueue;
    if (!unityBase || matId >= 0) return false;

    const auto pfnIdToObj = reinterpret_cast<t_UP_InstanceIDToObject>(unityBase + 0x107480);
    const auto pfnGetRq = reinterpret_cast<t_UP_MatGetRenderQueue>(unityBase + 0x95eb0);
    const auto pfnSetRq = reinterpret_cast<t_UP_MatSetRenderQueue>(unityBase + 0x95ef0);
    const auto pfnSetPass = reinterpret_cast<t_UP_MatSetShaderPassEnabled>(unityBase + 0x98530);

    __try {
        void* natMat = pfnIdToObj(&matId);
        const uintptr_t mAddr = reinterpret_cast<uintptr_t>(natMat);
        if (!IsReadableMem(mAddr, 0x38)) return false;

        const char* matName = *reinterpret_cast<const char* const*>(mAddr + 0x20);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(matName), 16)) return false;
        if (requireBodyName && strcmp(matName, "car_body(Clone)") != 0) return false;

        const int32_t curQ = pfnGetRq(natMat);
        static const wchar_t kDepthOnlyW[] = L"DepthOnly";
        const UnityManagedSpanWrapper depthOnlySpan{ kDepthOnlyW, 9 };

        if (applyOverride) {
            const int32_t origQ = (curQ != 2510 && curQ != 2580 && curQ != 2590) ? curQ : savedOrigQueue;
            if (outOrigQueue) *outOrigQueue = origQ;
            if (curQ != targetQueue) {
                pfnSetRq(natMat, targetQueue);
                pfnSetPass(natMat, &depthOnlySpan, false);
                if (outModified) *outModified = true;
            }
        } else {
            const int32_t restoreQ = (savedOrigQueue > 0 && savedOrigQueue != 2510 && savedOrigQueue != 2580 && savedOrigQueue != 2590)
                                     ? savedOrigQueue : 2200;
            if (curQ == 2510 || curQ == 2580 || curQ == 2590) {
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

static void UpdateCarFXPlayerTexturesOnMainThread(uintptr_t unityBase, const std::vector<LiveCarInfo>& liveSnapshot) {
    if (!unityBase) return;

    static ULONGLONG s_nextUpdateMs = 0;
    static bool s_prevWantTransparent = false;
    static bool s_prevWantStealth = false;
    static void* s_prevStealthRootGo = nullptr;
    static std::unordered_map<int32_t, int32_t> s_savedBodyMatQueues;
    static std::unordered_map<int32_t, int32_t> s_savedStealthMatQueues;
    static std::unordered_map<int32_t, int32_t> s_stealthAssetToClone;
    static std::vector<SavedStealthMatSwap> s_savedStealthSwaps;

    const bool wantTransparent = CarFX::ShouldBypassDepthPrePass();
    const bool wantStealth = CarFX::ShouldHideCarInternals();
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
        s_savedBodyMatQueues.clear();
        s_savedStealthMatQueues.clear();
        s_savedStealthSwaps.clear();
        s_stealthAssetToClone.clear();
        s_prevStealthRootGo = nullptr;
        s_prevWantTransparent = wantTransparent;
        s_prevWantStealth = wantStealth;
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
    if (!wantStealth) {
        restoreAllStealthMaterials();
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
        UnityBridge::FindPlayerCarModel(&rootTf);
        if (rootTf) SafeCheckNativeRootGo(rootTf, &rootGo);
    }
    if (!rootGo) {
        s_nextUpdateMs = GetTickCount64() + 250;
        return;
    }

    if (wantStealth && s_prevStealthRootGo && s_prevStealthRootGo != rootGo) {
        restoreAllStealthMaterials();
    }

    // Resolve builtin Unity default textures once so we know if car has 0 decals
    static void* s_natWhite = nullptr;
    static void* s_natBlack = nullptr;
    static void* s_natNormal = nullptr;
    static bool s_builtinsResolved = false;
    if (!s_builtinsResolved) {
        s_builtinsResolved = true;
        ReadBuiltinNativeTexGlobal(unityBase + 0xA8CF0, &s_natWhite);
        ReadBuiltinNativeTexGlobal(unityBase + 0xA8D30, &s_natBlack);
        ReadBuiltinNativeTexGlobal(unityBase + 0xA8E30, &s_natNormal);
    }

    void* natAlb = nullptr;
    void* natPack = nullptr;
    uint32_t albId = 0, packId = 0;
    uintptr_t partsMap = 0;
    if (!SafeExtractMasterMatTexPairSEH(rootGo, 0x8C255AA8u /* BodyTypeID */, &natAlb, &natPack, &albId, &packId, &partsMap)) {
        s_nextUpdateMs = GetTickCount64() + 250;
        return;
    }

    // Stealth places car_body at 2510 ([4] right after DrawSky) and all non-glass car parts at 2580 ([7] after car_body, before car_glass=2600),
    // so hardware Depth Test naturally hides engine/chassis behind cloaked body while keeping wheels, arms, lights & interior-through-glass visible!
    const int32_t bodyTargetQueue = wantStealth ? 2510 : 2590;
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

    if (wantStealth) {
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

    const uintptr_t pfnGetNatPtr = unityBase + 0xA8710; // UnityEngine.Texture::GetNativeTexturePtr_Injected
    CarFX::TargetTexturesInput in{};
    in.albedoTexId = albId;
    in.packedTexId = packId;
    in.albedoIsDefault = (!natAlb || natAlb == s_natWhite || natAlb == s_natBlack || natAlb == s_natNormal);
    in.packedIsDefault = (!natPack || natPack == s_natWhite || natPack == s_natBlack || natPack == s_natNormal);

    if (natAlb) SafeCallGetNativeTexPtr(pfnGetNatPtr, natAlb, &in.nativeAlbedoPtr);
    if (natPack) SafeCallGetNativeTexPtr(pfnGetNatPtr, natPack, &in.nativePackedPtr);
    in.valid = (in.nativeAlbedoPtr != nullptr) || (in.nativePackedPtr != nullptr);

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

    // Exact Unity 6000.3.19f1 icall RVAs extracted from UnityPlayer.dll
    pfn_Find                     = reinterpret_cast<t_Find>(s_unityBase + 0x669CE0);
    pfn_CloneSingle              = reinterpret_cast<t_Internal_CloneSingle>(s_unityBase + 0xC4230);
    pfn_Destroy                  = reinterpret_cast<t_Destroy>(s_unityBase + 0xC3A30);
    pfn_SetActive                = reinterpret_cast<t_SetActive>(s_unityBase + 0xBDFC0);
    pfn_SetLayer                 = reinterpret_cast<t_SetLayer>(s_unityBase + 0xBDFA0);
    pfn_GameObject_get_transform = reinterpret_cast<t_GameObject_get_transform>(s_unityBase + 0xBDEC0);
    pfn_Transform_get_position   = reinterpret_cast<t_Transform_get_position>(s_unityBase + 0xCCA20);
    pfn_Transform_set_position   = reinterpret_cast<t_Transform_set_position>(s_unityBase + 0xCCA50);
    pfn_Transform_get_rotation   = reinterpret_cast<t_Transform_get_rotation>(s_unityBase + 0xCCB40);
    pfn_Transform_set_rotation   = reinterpret_cast<t_Transform_set_rotation>(s_unityBase + 0xCCB90);

    void* targetExecLoop = reinterpret_cast<void*>(s_unityBase + 0x5AC580);
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
    SafeReadPtr(reinterpret_cast<void*>(s_unityBase + 0x2232F50), &rootLoop);

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

    // Periodically sync scene car model cache + per-frame lightweight position matching
    if (NativeCarX::IsInSession()) {
        RefreshLiveCarListOnMainThread(false);
        std::vector<LiveCarInfo> liveSnapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            liveSnapshot = s_liveCars;
        }
        EcsCollision::UpdateOnMainThread(dt, liveSnapshot);
        UpdateCarFXPlayerTexturesOnMainThread(s_unityBase, liveSnapshot);
    } else if (!s_liveCars.empty() || !s_sceneCarCache.empty()) {
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            s_liveCars.clear();
            s_sceneCarCache.clear();
        }
        EcsCollision::UpdateOnMainThread(dt, {});
        UpdateCarFXPlayerTexturesOnMainThread(s_unityBase, {});
    } else {
        EcsCollision::UpdateOnMainThread(dt, {});
        UpdateCarFXPlayerTexturesOnMainThread(s_unityBase, {});
    }

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
        if ((s_ghostSlots.empty() || !s_ghostSlots[0].gameObject) && NativeCarX::IsInSession()) {
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
            }
        }
    } else if (s_pendingMultiReplayApply.exchange(false)) {
        ApplyAllReplayTracksAtTimeInternal(s_pendingMultiReplayTime, s_pendingMultiReplayOffset, dt);
    } else if (s_pendingReplayApply.exchange(false)) {
        if ((s_ghostSlots.empty() || !s_ghostSlots[0].gameObject) && NativeCarX::IsInSession()) {
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
        if (SafeReadPtr(reinterpret_cast<void*>(s_unityBase + 0x22339F0), &pfnTarget) && pfnTarget) {
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
    if (!SafeReadPtr(reinterpret_cast<void*>(s_unityBase + 0x2232770), &mgr) || !mgr) {
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

    if (needRescan) {
        s_lastSceneCarScan = now;
        auto objects = GetAllGameObjects(30000);
        std::unordered_map<void*, const UnityObjectEntry*> tfMap;
        tfMap.reserve(objects.size());
        for (const auto& obj : objects) {
            if (obj.transform) tfMap[obj.transform] = &obj;
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
            UnityVector4 dummyRot{};
            if (pfn_Transform_get_position && SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), item.rootTf, &item.pos)) {
                item.hasPos = true;
            } else if (root->hasPos) {
                item.pos = root->pos;
                item.hasPos = true;
            }
            rebuilt.push_back(std::move(item));
        }
        s_sceneCarCache = std::move(rebuilt);
    } else if (pfn_Transform_get_position) {
        for (auto& sc : s_sceneCarCache) {
            if (sc.rootTf) {
                sc.hasPos = SafeCallGetVec3(reinterpret_cast<void*>(pfn_Transform_get_position), sc.rootTf, &sc.pos);
            }
        }
    }

    std::vector<LiveCarInfo> updated;
    updated.reserve(cars.size());
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto& c = cars[i];
        LiveCarInfo info;
        info.carPtr = c.carPtr;
        info.isLocalPlayer = c.isLocalPlayer;
        info.x = c.posX;
        info.y = c.posY;
        info.z = c.posZ;
        info.speedKmh = c.speedKmh;

        const SceneCarModelCache* best = nullptr;
        float bestDistSq = 16.0f; // within 4m
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

        if (best) {
            info.unityName = best->unityName;
            info.rootGo = best->rootGo;
            info.rootTf = best->rootTf;
            info.modelGo = best->modelGo;
        } else {
            info.unityName = c.isLocalPlayer ? "Player Car" : ("Car #" + std::to_string(i + 1));
        }
        updated.push_back(std::move(info));
    }

    s_liveCars = std::move(updated);
}

std::vector<LiveCarInfo> UnityBridge::GetLiveCarList() {
    auto cars = NativeCarX::GetAllCarsTelemetry();
    auto ecsStates = EcsCollision::GetCarStates();
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
                info.rootGo = cached.rootGo;
                info.rootTf = cached.rootTf;
                info.modelGo = cached.modelGo;
                matched = true;
                break;
            }
        }
        if (!matched) {
            info.unityName = c.isLocalPlayer ? "Player Car" : ("Car #" + std::to_string(i + 1));
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

void* UnityBridge::SpawnGhostSlotInternal(size_t slotIdx, const std::string& preferredCarName) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    if (slotIdx >= s_ghostSlots.size()) {
        s_ghostSlots.resize(slotIdx + 1);
    }
    DestroyGhostSlotInternal(slotIdx);

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
    GhostSlot& slot = s_ghostSlots[slotIdx];
    slot = GhostSlot{};
    slot.gameObject = cloneGo;
    slot.transform = cloneTf;
    slot.sourceCarName = carName;

    if (slotIdx == 0) {
        s_playerNativeObj = modelGo;
    }

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

    Logger::Info("Spawned 3D Ghost[" + std::to_string(slotIdx) + "] from '" + carName + "/Model(Clone)'");
    return cloneGo;
}

void UnityBridge::DestroyGhostSlotInternal(size_t slotIdx) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (slotIdx >= s_ghostSlots.size()) return;
    GhostSlot& slot = s_ghostSlots[slotIdx];
    if (slot.gameObject) {
        DestroyObjectDirect(slot.gameObject, 0.0f);
    }
    slot = GhostSlot{};
}

void UnityBridge::DestroyAllGhostSlotsInternal() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    int destroyed = 0;
    for (size_t i = 0; i < s_ghostSlots.size(); ++i) {
        if (s_ghostSlots[i].gameObject) {
            DestroyObjectDirect(s_ghostSlots[i].gameObject, 0.0f);
            destroyed++;
        }
    }
    s_ghostSlots.clear();
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
    }

    // Remove surplus ghost slots if switching to a replay with fewer tracks
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_ghostSlots.size() > trackCount) {
        for (size_t i = trackCount; i < s_ghostSlots.size(); ++i) {
            DestroyGhostSlotInternal(i);
        }
        s_ghostSlots.resize(trackCount);
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
