#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LiveCarInfo;

enum class CarGhostOverride : int {
    Auto = 0,
    ForceGhost = 1,
    ForceSolid = 2
};

struct SelectiveGhostConfig {
    bool enabled = false;
    bool antiWrongWay = true;
    float minSpeedKmh = 10.0f;
    float dotThreshold = -0.25f;
    float maxDistanceMeters = 120.0f;
    bool showHudAlert = true;
};

struct CarGhostState {
    uint64_t entityKey = 0;
    void* carPtr = nullptr;
    std::string unityName;
    bool isLocalPlayer = false;
    bool isGameGhosted = false;
    bool isWrongWay = false;
    bool isSelectivelyGhosted = false;
    CarGhostOverride overrideMode = CarGhostOverride::Auto;
    float distanceMeters = 0.0f;
    float speedKmh = 0.0f;
    float headingDot = 1.0f;
    uint32_t colliderBelongsTo = 0;
    uint32_t colliderCollidesWith = 0;
    int32_t colliderGroupIndex = 0;
};

// Selective per-player and Anti-Wrong-Way (Kino-style) ECS PhysicsCollider filter controller.
// All GameAssembly ECS memory mutations run strictly on the Unity Main Thread with SEH protection.
class EcsCollision {
public:
    static void Init();
    static void Shutdown();

    static void UpdateOnMainThread(float dt, const std::vector<LiveCarInfo>& liveCars);

    static void SetConfig(const SelectiveGhostConfig& cfg);
    static SelectiveGhostConfig GetConfig();

    static void SetCarOverride(const std::string& unityName, uintptr_t carPtr, CarGhostOverride mode);
    static CarGhostOverride GetCarOverride(const std::string& unityName, uintptr_t carPtr);
    static void ClearOverrides();

    static std::vector<CarGhostState> GetCarStates();
};
