#pragma once

#include "unity_bridge.h"
#include "replay_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace UnityBridgeInternal {

struct UnityManagedSpanWrapper {
    const wchar_t* begin;
    int32_t length;
};

typedef void*   (*t_UP_MatGetTexture)(void* natMat, int32_t propId);
typedef void    (*t_UP_MatSetTexture)(void* natMat, int32_t propId, void* natTex);
typedef void    (*t_UP_GfxCopyTextureFull)(void* srcNatTex, void* dstNatTex);
typedef void*   (*t_UP_InstanceIDToObject)(const int32_t* instanceId);
typedef int32_t (*t_UP_MatGetRenderQueue)(void* natMat);
typedef void    (*t_UP_MatSetRenderQueue)(void* natMat, int32_t queue);
typedef void    (*t_UP_MatSetShaderPassEnabled)(void* natMat, const UnityManagedSpanWrapper* passName, bool enabled);
typedef void*   (*t_UP_GetOrAssignInstantiatedMat)(void* natRenderer, int32_t slotIdx);
typedef void    (*t_UP_RendererSetMat0)(void* natRenderer, void* natMat);
typedef void    (*t_UP_RendererSetMatAt)(void* natRenderer, const int32_t* matId, uint32_t slot);

struct CarBehaviourInfo {
    uintptr_t carBehaviour = 0;
    void* nativeCarPtr = nullptr;
    void* raceCarPtr = nullptr;
    bool isPlayer = false;
};

struct Float4x4 {
    float m[16];
};

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

struct BodyMatSlotRef {
    int32_t rendererId = 0;
    int32_t slotIdx = 0;
    int32_t origMatId = 0;
};

// Shared state defined in unity_bridge.cpp
extern ReplayFrame g_pendingReplayFrameData;
extern float g_lastMainDt;
extern std::atomic<bool> g_pendingPlayerCarRepair;
extern float g_camAxes[3][3];
extern bool g_camAxesValid;

// Fast user-mode canonical range check for use inside __try / __except leaf SEH blocks
inline bool IsReadableMem(uintptr_t addr, size_t size) {
    return addr >= 0x10000ULL && addr <= 0x00007FFFFFFEFFFFULL && size > 0 && (addr + size) > addr;
}

// Memory / SEH / icall wrappers (implemented in unity_bridge.cpp)
bool IsReadableMemVQ(uintptr_t addr, size_t size);
bool SafeReadBytes(const void* addr, void* outBuf, size_t size);
bool SafeReadU16(const void* addr, uint16_t* outVal);
bool SafeReadPtr(const void* addr, void** outVal);
bool SafeReadU32(const void* addr, uint32_t* outVal);
bool SafeReadU64(const void* addr, uint64_t* outVal);
bool SafeReadCStr(const void* addr, char* buf, size_t maxLen);

void* SafeCallFind(void* fnPtr, const char* name);
void* SafeCallClone(void* fnPtr, void* nativeGo);
void* SafeCallGCHandleTarget(void* fnPtr, uintptr_t handle);
bool SafeCallDestroy(void* fnPtr, void* nativeObj, float t);
bool SafeCallSetActive(void* fnPtr, void* nativeGo, bool active);
bool SafeCallSetLayer(void* fnPtr, void* nativeGo, int layer);
bool SafeCallGetVec3(void* fnPtr, void* nativeTf, UnityVector3* outVec);
bool SafeCallSetVec3(void* fnPtr, void* nativeTf, const UnityVector3* inVec);
bool SafeCallGetVec4(void* fnPtr, void* nativeTf, UnityVector4* outVec);
bool SafeCallSetVec4(void* fnPtr, void* nativeTf, const UnityVector4* inVec);
bool SafeGetMainCameraPoseSEH(void* fnGetCam, void* fnTfGetPos, void* fnTfGetRot, UnityVector3* outPos, UnityVector4* outRot);
bool SafeGetMainCameraPosSEH(void* fnGetCam, void* fnTfGetPos, UnityVector3* outPos);
bool SafePhysicsRaycastSEH(void* fnRaycast, const UnityRay* ray, float maxDist, UnityRaycastHit* outHit, int layerMask);

// Math / car geometry helpers (implemented in unity_bridge.cpp)
float VecDot(const UnityVector3& a, const UnityVector3& b);
UnityVector3 VecNormalize(UnityVector3 v);
UnityVector4 MatrixToQuaternion(const float* m);
UnityVector4 QuatMul(const UnityVector4& a, const UnityVector4& b);
UnityVector4 QuatConj(const UnityVector4& q);
UnityVector4 QuatFromAxisY(float angleRad);
UnityVector4 ReconstructSpinFreeKnuckle(const float* wheelMat, const float* bodyMat);
std::string StripCarSuffix(const std::string& name);
void EstimateCarExtents(const float* bodyMat, const float wheelMats[4][16], float& outHalfW, float& outHalfL);

// Scene / object resolvers (implemented in unity_scene.cpp)
void* GetGameObjectManagerPtr(uintptr_t unityBase);
void* GetGCHandleTargetPtr(uintptr_t unityBase);
uintptr_t SafeFindCarBehaviourOnRootGoSEH(void* rootGo);
bool SafeInspectCarBehaviourOnRootGoSEH(void* rootGo, CarBehaviourInfo* outInfo);

// Material / texture / renderer resolvers (implemented in unity_carfx_sync.cpp)
t_UP_InstanceIDToObject GetInstanceIDToObjectFn(uintptr_t unityBase);
t_UP_MatGetRenderQueue GetMatGetRenderQueueFn(uintptr_t unityBase);
t_UP_MatSetRenderQueue GetMatSetRenderQueueFn(uintptr_t unityBase);
t_UP_MatSetShaderPassEnabled GetMatSetShaderPassEnabledFn(uintptr_t unityBase);
t_UP_GetOrAssignInstantiatedMat GetOrAssignInstantiatedMatFn(uintptr_t unityBase);
t_UP_RendererSetMat0 GetRendererSetMat0Fn(uintptr_t unityBase);
uintptr_t GetNativeTexPtrFn(uintptr_t unityBase);
t_UP_MatGetTexture GetMatGetTextureFn(uintptr_t unityBase);
t_UP_MatSetTexture GetMatSetTextureFn(uintptr_t unityBase);
t_UP_GfxCopyTextureFull GetGfxCopyTextureFullFn(uintptr_t unityBase);
void GetBuiltinNativeTextures(uintptr_t unityBase, void** outWhite, void** outBlack, void** outNormal);
bool IsDefaultNativeTex(void* natTex, void* natWhite, void* natBlack, void* natNormal);
bool SafeCallGetNativeTexPtr(uintptr_t fnAddr, void* nativeTex, void** outD3DPtr);
void* SafeGetMatTextureSEH(uintptr_t unityBase, void* natMat, int32_t propId);
bool SafeSetMatTextureSEH(uintptr_t unityBase, void* natMat, int32_t propId, void* natTex);
bool SafeResolveMatAndQueueSEH(uintptr_t unityBase, int32_t matId, void** outNatMat, int32_t* outQueue);
bool SafeCloneOrAssignRendererMatSEH(uintptr_t unityBase, int32_t rInstId, int32_t slotIdx,
                                     int32_t existingCloneMatId, int32_t* outNewCloneMatId);
bool SafeSyncMaterialQueueAndDepthSEH(uintptr_t unityBase, int32_t matId, bool applyOverride,
                                      int32_t targetQueue, bool requireBodyName, int32_t savedOrigQueue,
                                      bool* outModified, int32_t* outOrigQueue);
void CollectBodyMaterialSlots(uintptr_t unityBase, void* rootGo, bool allowSharedAsset,
                              std::vector<BodyMatSlotRef>& outSlots);

} // namespace UnityBridgeInternal
