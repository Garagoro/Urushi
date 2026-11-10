#pragma once
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

struct ReplayFrame;

struct UnityVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct UnityVector4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct UnityObjectEntry {
    void* go = nullptr;
    void* transform = nullptr;
    void* parentTransform = nullptr;
    std::string name;
    UnityVector3 pos;
    bool hasPos = false;
};

struct LiveCarInfo {
    void* carPtr = nullptr;
    std::string unityName;
    void* rootGo = nullptr;
    void* rootTf = nullptr;
    void* modelGo = nullptr;
    bool isLocalPlayer = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float speedKmh = 0.0f;
    bool isWrongWay = false;
    bool isSelectivelyGhosted = false;
    bool isGameGhosted = false;
    int ghostOverride = 0;
    float headingDot = 1.0f;
    float distToPlayer = 0.0f;
};

class UnityBridge {
public:
    static bool Init();
    static void Shutdown();

    static bool IsOnMainThread();
    static DWORD GetMainThreadId();

    // Native / Managed object helpers
    static void* UnwrapScriptingPtrToNative(void* gcOrObj);
    static void* GetTransformOfGameObject(void* nativeGo);
    static void* GetGameObjectOfTransform(void* nativeTf);
    static std::string GetGameObjectName(void* nativeGo);
    static bool GetTransformHierarchyPosition(void* nativeTf, UnityVector3& outPos);

    // Scene enumeration & player/car lookup
    static std::vector<UnityObjectEntry> GetAllGameObjects(int maxObjects = 30000);
    static void* FindGameObjectByName(const char* name);
    static void* FindPlayerCarModel(void** outRootTf = nullptr);
    static std::vector<LiveCarInfo> GetLiveCarList();
    static std::string LookupCarUnityName(void* carPtr);

    // Direct Unity calls (safe only on Unity Main Thread!)
    static void* CloneGameObjectDirect(void* nativeGo);
    static bool DestroyObjectDirect(void* nativeObj, float delay = 0.0f);
    static bool SetActiveDirect(void* nativeGo, bool active);
    static bool SetLayerDirect(void* nativeGo, int layer);
    static bool GetTransformPosRotDirect(void* nativeTf, UnityVector3& outPos, UnityVector4& outRot);
    static bool SetTransformPositionDirect(void* nativeTf, const UnityVector3& pos);
    static bool SetTransformRotationDirect(void* nativeTf, const UnityVector4& rot);
    static bool SetTransformPosRotDirect(void* nativeTf, const UnityVector3& pos, const UnityVector4& rot);

    // Player Car tracking (legacy + direct)
    static void* GetPlayerCSharpCar();
    static void* GetPlayerNativeObj();

    // Ghost Car(s) ("болванки")
    static void* SpawnGhostVisual();
    static bool DestroyGhostVisual();
    static bool HasGhostVisual();
    static void* GetGhostVisualGameObject();
    static int GetGhostCount();
    static void SetGhostPositionRotation(const UnityVector3& pos, const UnityVector4& rot);

    // Stage 2 + Multi-Car + Collision
    static void ApplyGhostPose(const float* bodyMatrix, const float wheelMatrices[4][16], const float caliperMatrices[4][16], float steerAngle, const UnityVector3& worldOffset);
    static void SetGhostMirrorMode(bool enabled, float offsetRight = 3.8f);
    static bool IsGhostMirrorMode();
    static void ApplyReplayFrameToGhost(const ReplayFrame& frame, float offsetRight = 0.0f);
    static void ApplyReplayTimeToAllGhosts(float targetTime, float offsetRight = 0.0f);

    static void SetGhostCollisionEnabled(bool enabled);
    static bool IsGhostCollisionEnabled();

private:
    struct ChildLayerInfo {
        void* go = nullptr;
        int32_t originalLayer = 8;
    };

    struct GhostSlot {
        void* gameObject = nullptr;
        void* transform = nullptr;
        void* bodyTf = nullptr;
        void* liveryColliderGo = nullptr;
        void* wheelRootTf[4] = { nullptr };
        void* wheelHubTf[4] = { nullptr };
        void* wheelBoneTf[4] = { nullptr };
        void* steeringWheelTf = nullptr;
        UnityVector4 steeringRestBodyRel{};
        bool hasSteeringRest = false;
        std::vector<ChildLayerInfo> childGos;
        std::string sourceCarName;

        // Pose & velocity tracking for OBB collision
        bool hasPose = false;
        float lastMatrix[16] = { 0 };
        UnityVector3 pos{};
        UnityVector3 vel{};
        float halfWidth = 0.96f;
        float halfHeight = 0.65f;
        float halfLength = 2.25f;
    };

    struct SceneCarModelCache {
        std::string unityName;
        void* rootGo = nullptr;
        void* rootTf = nullptr;
        void* modelGo = nullptr;
        UnityVector3 pos{};
        bool hasPos = false;
    };

    static void Hooked_ExecutePlayerLoop(void* playerLoopSystem);
    static void ProcessPendingMainThreadTasks(float dt);
    static void RefreshLiveCarListOnMainThread(bool forceRescan = false);
    static void* FindModelForCarName(const std::string& preferredCarName, void** outRootTf = nullptr);

    static void* SpawnGhostSlotInternal(size_t slotIdx, const std::string& preferredCarName = "");
    static void DestroyGhostSlotInternal(size_t slotIdx);
    static void DestroyAllGhostSlotsInternal();
    static void ConfigureGhostSlot(GhostSlot& slot, void* clonedGo, float initialSteerAngle);
    static void ApplySlotCollisionVisuals(GhostSlot& slot, bool enabled);
    static void ApplyGhostPoseToSlot(size_t slotIdx, const float* bodyMatrix, const float wheelMatrices[4][16], const float caliperMatrices[4][16], float steerAngle, const UnityVector3& worldOffset, float dt);
    static void ApplyAllReplayTracksAtTimeInternal(float targetTime, float offsetRight, float dt);
    static void ResolveGhostCollisions();

    typedef void (*t_ExecutePlayerLoop)(void* playerLoopSystem);
    static t_ExecutePlayerLoop s_originalExecutePlayerLoop;

    // Unity internal calls (from UnityPlayer.dll 6000.3.19f1)
    typedef void* (*t_Find)(const char* name);
    typedef void* (*t_Internal_CloneSingle)(void* nativeObj);
    typedef void  (*t_Destroy)(void* nativeObj, float t);
    typedef void  (*t_SetActive)(void* nativeGo, bool active);
    typedef void  (*t_SetLayer)(void* nativeGo, int layer);
    typedef void* (*t_GameObject_get_transform)(void* nativeGo);
    typedef void  (*t_Transform_set_position)(void* nativeTf, const UnityVector3* pos);
    typedef void  (*t_Transform_set_rotation)(void* nativeTf, const UnityVector4* rot);
    typedef void  (*t_Transform_get_position)(void* nativeTf, UnityVector3* outPos);
    typedef void  (*t_Transform_get_rotation)(void* nativeTf, UnityVector4* outRot);

    static t_Find pfn_Find;
    static t_Internal_CloneSingle pfn_CloneSingle;
    static t_Destroy pfn_Destroy;
    static t_SetActive pfn_SetActive;
    static t_SetLayer pfn_SetLayer;
    static t_GameObject_get_transform pfn_GameObject_get_transform;
    static t_Transform_set_position pfn_Transform_set_position;
    static t_Transform_set_rotation pfn_Transform_set_rotation;
    static t_Transform_get_position pfn_Transform_get_position;
    static t_Transform_get_rotation pfn_Transform_get_rotation;

    static uintptr_t s_unityBase;
    static uintptr_t s_unitySize;
    static std::recursive_mutex s_mutex;
    static void* s_playerCSharpCar;
    static void* s_playerNativeObj;
    static std::vector<GhostSlot> s_ghostSlots;
    static std::vector<SceneCarModelCache> s_sceneCarCache;
    static std::vector<LiveCarInfo> s_liveCars;
    static std::chrono::steady_clock::time_point s_lastSceneCarScan;
    static bool s_initialized;

    static std::atomic<DWORD> s_mainThreadId;
    static std::atomic<bool> s_pendingSpawn;
    static std::atomic<bool> s_pendingDestroy;
    static std::atomic<bool> s_pendingTransform;
    static std::atomic<bool> s_ghostMirrorMode;
    static float s_ghostMirrorOffset;
    static std::atomic<bool> s_pendingReplayApply;
    static float s_pendingReplayOffset;
    static std::atomic<bool> s_pendingMultiReplayApply;
    static float s_pendingMultiReplayTime;
    static float s_pendingMultiReplayOffset;
    static std::atomic<bool> s_ghostCollisionEnabled;
    static std::atomic<bool> s_pendingCollisionSync;
    static UnityVector3 s_pendingPos;
    static UnityVector4 s_pendingRot;
};
