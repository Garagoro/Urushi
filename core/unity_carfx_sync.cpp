#include "unity_bridge.h"
#include "unity_bridge_internal.h"
#include "pattern_scanner.h"
#include "carfx.h"
#include "native_carx.h"
#include "logger.h"
#include "settings.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace UnityBridgeInternal;

namespace UnityBridgeInternal {

// ================= CarFX Player Livery Extractor (Validated + Leaf SEH) =================

typedef void* (*t_Tex_GetNativePtr)(void* nativeTex);

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

bool SafeCallGetNativeTexPtr(uintptr_t fnAddr, void* nativeTex, void** outD3DPtr) {
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

t_UP_InstanceIDToObject GetInstanceIDToObjectFn(uintptr_t unityBase) {
    static t_UP_InstanceIDToObject s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "40 56 48 83 EC 20 83 39 00 48 8B F1 75 08 33 C0", 0x107480, "InstanceIDToObject");
        s_cached = reinterpret_cast<t_UP_InstanceIDToObject>(va);
    }
    return s_cached;
}

t_UP_MatGetRenderQueue GetMatGetRenderQueueFn(uintptr_t unityBase) {
    static t_UP_MatGetRenderQueue s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 83 EC 28 83 79 2C 00 7C ? 8B 41 2C 48 83 C4 28 C3", 0x95EB0, "MatGetRenderQueue");
        s_cached = reinterpret_cast<t_UP_MatGetRenderQueue>(va);
    }
    return s_cached;
}

t_UP_MatSetRenderQueue GetMatSetRenderQueueFn(uintptr_t unityBase) {
    static t_UP_MatSetRenderQueue s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "40 57 48 83 EC 20 48 8B F9 39 51 2C 74 ? 80 3D ? ? ? ? 00 89 51 2C", 0x95EF0, "MatSetRenderQueue");
        s_cached = reinterpret_cast<t_UP_MatSetRenderQueue>(va);
    }
    return s_cached;
}

t_UP_MatSetShaderPassEnabled GetMatSetShaderPassEnabledFn(uintptr_t unityBase) {
    static t_UP_MatSetShaderPassEnabled s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "40 53 57 48 83 EC 68 48 8B F9 C6 44 24 30 00", 0x98530, "MatSetShaderPassEnabled");
        s_cached = reinterpret_cast<t_UP_MatSetShaderPassEnabled>(va);
    }
    return s_cached;
}

t_UP_GetOrAssignInstantiatedMat GetOrAssignInstantiatedMatFn(uintptr_t unityBase) {
    static t_UP_GetOrAssignInstantiatedMat s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 10 48 89 6C 24 20 44 88 44 24 18 56 41 56 41 57", 0x47AA00, "GetOrAssignInstantiatedMat");
        s_cached = reinterpret_cast<t_UP_GetOrAssignInstantiatedMat>(va);
    }
    return s_cached;
}

t_UP_RendererSetMat0 GetRendererSetMat0Fn(uintptr_t unityBase) {
    static t_UP_RendererSetMat0 s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B F9 48 8B F2", 0x8CE50, "RendererSetMat0");
        s_cached = reinterpret_cast<t_UP_RendererSetMat0>(va);
    }
    return s_cached;
}

uintptr_t GetNativeTexPtrFn(uintptr_t unityBase) {
    static uintptr_t s_cached = 0;
    if (!s_cached && unityBase) {
        s_cached = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 08 57 48 83 EC 20 48 8B 99 10 01 00 00", 0x4824F0, "GetNativeTexturePtr");
    }
    return s_cached;
}

t_UP_MatGetTexture GetMatGetTextureFn(uintptr_t unityBase) {
    static t_UP_MatGetTexture s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "89 54 24 10 55 56 48 8D 6C 24 B1 48 81 EC D8 00 00 00 48 8B 41 48", 0x603830, "MatGetTexture");
        s_cached = reinterpret_cast<t_UP_MatGetTexture>(va);
    }
    return s_cached;
}

t_UP_MatSetTexture GetMatSetTextureFn(uintptr_t unityBase) {
    static t_UP_MatSetTexture s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 8B C4 89 50 10 55 53 56 48 8D 68 A1 48 81 EC F0 00 00 00", 0x602EB0, "MatSetTexture");
        s_cached = reinterpret_cast<t_UP_MatSetTexture>(va);
    }
    return s_cached;
}

t_UP_GfxCopyTextureFull GetGfxCopyTextureFullFn(uintptr_t unityBase) {
    static t_UP_GfxCopyTextureFull s_cached = nullptr;
    if (!s_cached && unityBase) {
        uintptr_t va = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(unityBase),
            "48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 55 48 8D 6C 24 A9 48 81 EC D0 00 00 00", 0x7E620, "GfxCopyTextureFull");
        s_cached = reinterpret_cast<t_UP_GfxCopyTextureFull>(va);
    }
    return s_cached;
}

void GetBuiltinNativeTextures(uintptr_t unityBase, void** outWhite, void** outBlack, void** outNormal) {
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

bool IsDefaultNativeTex(void* natTex, void* natWhite, void* natBlack, void* natNormal) {
    return !natTex || natTex == natWhite || natTex == natBlack || natTex == natNormal;
}

void* SafeGetMatTextureSEH(uintptr_t unityBase, void* natMat, int32_t propId) {
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

bool SafeSetMatTextureSEH(uintptr_t unityBase, void* natMat, int32_t propId, void* natTex) {
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
    if (has("car_body") || has("car_glass") || has("shadowcaster") || has("wheel_motion")) return 0;
    if (has("rim") || has("yolo") || has("defaultdiffuse")) return CarFX::kPartWheels;
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

bool SafeResolveMatAndQueueSEH(uintptr_t unityBase, int32_t matId, void** outNatMat, int32_t* outQueue) {
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

bool SafeCloneOrAssignRendererMatSEH(uintptr_t unityBase, int32_t rInstId, int32_t slotIdx,
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
    if (!Settings::GetBool("debug", "dump_hierarchy", false)) return;
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

bool SafeSyncMaterialQueueAndDepthSEH(uintptr_t unityBase, int32_t matId, bool applyOverride,
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


void CollectBodyMaterialSlots(uintptr_t unityBase, void* rootGo, bool allowSharedAsset,
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

} // namespace UnityBridgeInternal

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
    // Cache per rootGo & partMask to avoid walking 300+ GameObject hierarchy nodes and reading SEH properties every 2 seconds.
    static void* s_prevInternalsRootGo = nullptr;
    static uint32_t s_prevInternalsPartMask = 0;
    static bool s_prevInternalsActive = false;
    const bool internalsActive = CarFX::IsTargetActive(CarFX::kTargetInternals);
    const uint32_t partMask = internalsActive ? CarFX::GetInternalsPartMask() : 0;
    if (rootGo != s_prevInternalsRootGo || partMask != s_prevInternalsPartMask || internalsActive != s_prevInternalsActive) {
        s_prevInternalsRootGo = rootGo;
        s_prevInternalsPartMask = partMask;
        s_prevInternalsActive = internalsActive;

        std::vector<void*> intTex;
        if (internalsActive) {
            std::vector<CarRendererRef> rs;
            CollectCarRenderersOnMainThread(rootGo, rs);
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

    // Also collect any additional car_body material texture pairs (e.g. separate body/kit/livery TIDs)
    {
        std::vector<BodyMatSlotRef> extraSlots;
        CollectBodyMaterialSlots(unityBase, rootGo, true, extraSlots);
        std::unordered_set<int32_t> seenMids;
        for (const BodyMatSlotRef& bs : extraSlots) {
            if (!bs.origMatId || !seenMids.insert(bs.origMatId).second) continue;
            void* matObj = nullptr;
            if (!SafeResolveMatAndQueueSEH(unityBase, bs.origMatId, &matObj, nullptr) || !matObj) continue;
            void* mAlb = SafeGetMatTextureSEH(unityBase, matObj, 380);
            void* mPack = SafeGetMatTextureSEH(unityBase, matObj, 1370);
            const bool mAlbDef = IsDefaultNativeTex(mAlb, s_natWhite, s_natBlack, s_natNormal) || (mAlb == s_playerIsoPackTex);
            const bool mPackDef = IsDefaultNativeTex(mPack, s_natWhite, s_natBlack, s_natNormal);
            if (mAlbDef && mPackDef) continue;

            void* d3dA = nullptr;
            void* d3dP = nullptr;
            if (!mAlbDef && mAlb) SafeCallGetNativeTexPtr(pfnGetNatPtr, mAlb, &d3dA);
            if (!mPackDef && mPack) SafeCallGetNativeTexPtr(pfnGetNatPtr, mPack, &d3dP);
            if (!d3dA && !d3dP) continue;
            if (d3dA == in.nativeAlbedoPtr && d3dP == in.nativePackedPtr) continue;

            bool dup = false;
            for (const auto& ep : in.extraPairs) {
                if (ep.nativeAlbedoPtr == d3dA && ep.nativePackedPtr == d3dP) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                CarFX::TargetTexturesInput::ExtraTexPair ep{};
                ep.nativeAlbedoPtr = d3dA;
                ep.nativePackedPtr = d3dP;
                ep.albedoIsDefault = (d3dA == nullptr);
                ep.packedIsDefault = (d3dP == nullptr);
                in.extraPairs.push_back(ep);
            }
        }
    }

    in.valid = ((!in.albedoIsDefault || !in.packedIsDefault) &&
                ((in.nativeAlbedoPtr != nullptr) || (in.nativePackedPtr != nullptr))) ||
               !in.extraPairs.empty();

    CarFX::SetPlayerTargetTextures(CarFX::kTargetBody, in);
    s_nextUpdateMs = GetTickCount64() + (in.valid ? 2000 : 250);
}
