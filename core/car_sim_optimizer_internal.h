#pragma once
#include "car_sim_optimizer.h"
#include "unity_bridge.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CarSimOptimizerInternal {

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

extern std::recursive_mutex s_mutex;
extern CarSimOptimizer::Config s_cfg;
extern CarSimOptimizer::Stats s_stats;
extern std::vector<CarSimOptimizer::CarEntry> s_netCarsSnapshot;

extern uintptr_t s_gaBase;
extern bool s_noDamageOverride;
extern bool s_nativeExportsReady;

extern void* s_sfDeformSleep;
extern void* s_sfRootSleep;
extern void* s_sfRenderPolicy;
extern void* s_sfOptimizeGeom;

extern DeformBaseline s_deformBase;
extern RootSleepBaseline s_rootBase;
extern RenderPolicyBaseline s_renderBase;
extern OptimizeGeomBaseline s_geomBase;

extern std::unordered_set<void*> s_allowedFullSim;
extern std::unordered_set<void*> s_knownNetCars;
extern std::unordered_map<void*, std::chrono::steady_clock::time_point> s_slotEnteredAt;
extern std::chrono::steady_clock::time_point s_lastSwapTime;
extern bool s_hasAllowedSet;

extern std::unordered_set<void*> s_effectsDisabledSet;
extern std::unordered_map<void*, SavedNativeCarState> s_savedNativeCars;

bool InstallHooksAndResolvers(HMODULE hGA);
void ResolveNativeExportsIfNeeded();
void ResolveStaticsIfNeeded();
void SyncStaticSubsystems(const CarSimOptimizer::Config& cfg, bool forceRestore);
void UpdateCarEffectsOnMainThread(const CarSimOptimizer::Config& cfg,
                                  const std::vector<LiveCarInfo>& liveCars,
                                  bool forceRestore);
void UpdateNativeCarPhysicsOnMainThread(const CarSimOptimizer::Config& cfg,
                                        const std::vector<LiveCarInfo>& liveCars,
                                        bool forceRestore);

} // namespace CarSimOptimizerInternal
