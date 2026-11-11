#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct LiveCarInfo;

class CarSimOptimizer {
public:
    struct Config {
        bool enabled = false;

        // Damage & Tire Wear Control
        bool disableDamage = false;          // Complete immunity to body mesh deformation, detached parts & bent suspension
        bool disableTireWear = false;        // Infinite tire life, 0% degradation, constant maximum grip

        // Multiplayer server-snapshot distance switcher (DistanceBasedPhysicsSwitchingSubController)
        int maxFullSimCars = 3;             // Local player + (N - 1) nearest remote cars
        float maxFullSimDistMeters = 100.0f; // Vanilla cap is 100.0m
        float hysteresisMeters = 8.0f;       // Distance margin against boundary/rank flicker
        float switchCooldownSec = 1.8f;      // Minimum hold/cooldown time before swapping an occupied slot

        // CarX.DeformationPhysics.Systems.DeformationSkeletonSleepSystem (configured by presets)
        bool tuneDeformationSleep = false;
        float deformSleepDistMeters = 12.0f; // Vanilla: 30.0m
        int deformIdleSteps = 10;            // Vanilla: 30

        // CarX.DeformationPhysics.Systems.PhysicsRootSleepSystem
        bool tunePhysicsRootSleep = false;
        int physicsRootIdleSteps = 12;       // Vanilla: 30

        // Framework.Views.Cars.Components.RemoteCarRenderPolicy & OptimizeCarGeometryComponent
        bool tuneRenderPolicy = false;
        float smallHideDistMeters = 15.0f;   // Vanilla: 25.0m
        float smallRendererRadius = 0.45f;   // Vanilla: 0.40f
        float tinyRendererRadius = 0.22f;    // Vanilla: 0.20f
        bool remoteLodHalfRate = true;       // Vanilla: true
        bool playerBestLod = false;          // Vanilla: false

        // Far-car visual smoke & skidmarks (CarBehaviour::SetCarEffectsEnabled)
        bool limitCarEffects = false;
        int maxEffectsCars = 3;

        // Optional NativeCarX reduction for non-kinematic remote / offline AI cars beyond top-N
        bool tuneOfflinePhysics = false;
        bool useSimpleModeBeyondTopN = false;
        bool useTrafficModeBeyondTopN = false;
        int remoteIntegrationSteps = 2;      // Vanilla remote: 4
    };

    struct CarEntry {
        uintptr_t containerPtr = 0;
        int32_t ownerId = 0;
        std::string name;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float distMeters = 0.0f;
        bool hasDist = false;
        bool vanillaFar = false;
        bool isKinematic = false;
        bool activeFullSim = false;
        bool forcedSnapshot = false;
    };

    struct Stats {
        bool initialized = false;
        bool hooksInstalled = false;
        bool nativeExportsReady = false;
        bool deformSleepResolved = false;
        bool rootSleepResolved = false;
        bool renderPolicyResolved = false;
        bool optimizeGeomResolved = false;

        uint64_t switchingUpdateCalls = 0;
        uint64_t antiFlickerBlocks = 0;
        bool antiFlickerHolding = false;
        uint32_t totalNetCars = 0;
        uint32_t fullSimNetCars = 0;
        uint32_t snapshotNetCars = 0;

        int32_t deformSleepingCars = 0;
        int32_t deformTransitions = 0;
        int32_t rootSleepingRoots = 0;
        int32_t rootTransitions = 0;

        int32_t effectsDisabledCars = 0;
        int32_t offlineReducedCars = 0;

        bool noDamageActive = false;
        bool noTireWearActive = false;
        uint64_t damageBlocks = 0;
        uint64_t jointBreakBlocks = 0;

        // Captured vanilla baselines for UI reference
        float vanillaDeformDist = 30.0f;
        int32_t vanillaDeformIdleSteps = 30;
        int32_t vanillaRootIdleSteps = 30;
        float vanillaSmallHideDist = 25.0f;
        float vanillaSmallRadius = 0.40f;
        float vanillaTinyRadius = 0.20f;
    };

    static bool Init();
    static void Shutdown();

    static void UpdateOnMainThread(float dt, const std::vector<LiveCarInfo>& liveCars);

    static Config GetConfig();
    static void SetConfig(const Config& cfg);
    static void ResetDefaults();

    static Stats GetStats();
    static std::vector<CarEntry> GetNetCarsSnapshot();

    static void SetNoDamageOverride(bool enable);
    static bool GetNoDamageOverride();
    static void SetNoTireWearOverride(bool enable);
};
