#include "unity_bridge.h"
#include "unity_bridge_internal.h"
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

using namespace UnityBridgeInternal;

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
std::atomic<bool> UnityBridge::s_pendingMultiReplayApply{ false };
float UnityBridge::s_pendingMultiReplayTime = 0.0f;
float UnityBridge::s_pendingMultiReplayOffset = 0.0f;
std::atomic<bool> UnityBridge::s_ghostCollisionEnabled{ false };
std::atomic<bool> UnityBridge::s_ghostMirrorPlayerShader{ true };
std::atomic<bool> UnityBridge::s_pendingCollisionSync{ false };
std::atomic<bool> UnityBridge::s_pendingTeleport{ false };
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


namespace UnityBridgeInternal {

ReplayFrame g_pendingReplayFrameData{};
float g_lastMainDt = 0.016f;
std::atomic<bool> g_pendingPlayerCarRepair{ false };
float g_camAxes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
bool g_camAxesValid = false;

bool SafeReadBytes(const void* addr, void* outBuf, size_t size) {
    __try {
        if (!addr || (uintptr_t)addr < 0x10000 || !outBuf) return false;
        memcpy(outBuf, addr, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadU16(const void* addr, uint16_t* outVal) {
    return SafeReadBytes(addr, outVal, sizeof(uint16_t));
}

bool IsReadableMemVQ(uintptr_t addr, size_t size) {
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

bool SafeReadPtr(const void* addr, void** outVal) {
    return SafeReadBytes(addr, outVal, sizeof(void*));
}

bool SafeReadU32(const void* addr, uint32_t* outVal) {
    return SafeReadBytes(addr, outVal, sizeof(uint32_t));
}

bool SafeReadU64(const void* addr, uint64_t* outVal) {
    return SafeReadBytes(addr, outVal, sizeof(uint64_t));
}

bool SafeReadCStr(const void* addr, char* buf, size_t maxLen) {
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

void* SafeCallFind(void* fnPtr, const char* name) {
    __try {
        if (!fnPtr || !name) return nullptr;
        typedef void* (*t_Fn)(const char*);
        return ((t_Fn)fnPtr)(name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* SafeCallClone(void* fnPtr, void* nativeGo) {
    __try {
        if (!fnPtr || !nativeGo) return nullptr;
        typedef void* (*t_Fn)(void*);
        return ((t_Fn)fnPtr)(nativeGo);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* SafeCallGCHandleTarget(void* fnPtr, uintptr_t handle) {
    __try {
        if (!fnPtr || !handle) return nullptr;
        typedef void* (*t_Fn)(uintptr_t);
        return ((t_Fn)fnPtr)(handle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SafeCallDestroy(void* fnPtr, void* nativeObj, float t) {
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

bool SafeCallSetActive(void* fnPtr, void* nativeGo, bool active) {
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

bool SafeCallSetLayer(void* fnPtr, void* nativeGo, int layer) {
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

bool SafeCallGetVec3(void* fnPtr, void* nativeTf, UnityVector3* outVec) {
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

bool SafeCallSetVec3(void* fnPtr, void* nativeTf, const UnityVector3* inVec) {
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

bool SafeCallGetVec4(void* fnPtr, void* nativeTf, UnityVector4* outVec) {
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

bool SafeCallSetVec4(void* fnPtr, void* nativeTf, const UnityVector4* inVec) {
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

float VecDot(const UnityVector3& a, const UnityVector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

UnityVector3 VecNormalize(UnityVector3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f) {
        return { v.x / len, v.y / len, v.z / len };
    }
    return { 0.0f, 1.0f, 0.0f };
}

UnityVector4 MatrixToQuaternion(const float* m) {
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

UnityVector4 QuatMul(const UnityVector4& a, const UnityVector4& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

UnityVector4 QuatConj(const UnityVector4& q) {
    return { -q.x, -q.y, -q.z, q.w };
}

UnityVector4 QuatFromAxisY(float angleRad) {
    float h = angleRad * 0.5f;
    return { 0.0f, sinf(h), 0.0f, cosf(h) };
}

UnityVector4 ReconstructSpinFreeKnuckle(const float* wheelMat, const float* bodyMat) {
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

std::string StripCarSuffix(const std::string& name) {
    size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return name;
    for (size_t i = pos + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return name;
    }
    return name.substr(0, pos);
}

void EstimateCarExtents(const float* bodyMat, const float wheelMats[4][16], float& outHalfW, float& outHalfL) {
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

bool SafeGetMainCameraPoseSEH(void* fnGetCam, void* fnTfGetPos, void* fnTfGetRot, UnityVector3* outPos, UnityVector4* outRot) {
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

bool SafeGetMainCameraPosSEH(void* fnGetCam, void* fnTfGetPos, UnityVector3* outPos) {
    return SafeGetMainCameraPoseSEH(fnGetCam, fnTfGetPos, nullptr, outPos, nullptr);
}

bool SafePhysicsRaycastSEH(void* fnRaycast, const UnityRay* ray, float maxDist, UnityRaycastHit* outHit, int layerMask) {
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

} // namespace UnityBridgeInternal

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

    // Pre-cache all Unity icalls upfront during initialization to eliminate mid-game lazy pattern scans
    GetInstanceIDToObjectFn(s_unityBase);
    GetMatGetTextureFn(s_unityBase);
    GetMatSetTextureFn(s_unityBase);
    GetGfxCopyTextureFullFn(s_unityBase);
    GetNativeTexPtrFn(s_unityBase);
    GetMatGetRenderQueueFn(s_unityBase);
    GetMatSetRenderQueueFn(s_unityBase);
    GetMatSetShaderPassEnabledFn(s_unityBase);
    GetOrAssignInstantiatedMatFn(s_unityBase);
    GetRendererSetMat0Fn(s_unityBase);
    GetGameObjectManagerPtr(s_unityBase);
    GetGCHandleTargetPtr(s_unityBase);

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
        g_lastMainDt = std::clamp(dt, 0.001f, 0.1f);

        ProcessPendingMainThreadTasks(g_lastMainDt);
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

    if (g_pendingPlayerCarRepair.exchange(false)) {
        RepairPlayerCarOnMainThread();
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
                    copyFrame = g_pendingReplayFrameData;
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
        ApplyGhostPoseToSlot(0, frame.transformMatrix, frame.wheelMatrices, frame.caliperMatrices, frame.steerAngle, worldOffset, g_lastMainDt);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    g_pendingReplayFrameData = frame;
    s_pendingReplayOffset = offsetRight;
    s_pendingReplayApply.store(true);
}

void UnityBridge::ApplyReplayTimeToAllGhosts(float targetTime, float offsetRight) {
    if (IsOnMainThread()) {
        ApplyAllReplayTracksAtTimeInternal(targetTime, offsetRight, g_lastMainDt);
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

void UnityBridge::RequestPlayerCarRepair() {
    g_pendingPlayerCarRepair.store(true, std::memory_order_release);
}
