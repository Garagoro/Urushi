#include "car_sim_optimizer_internal.h"
#include "il2cpp_helpers.h"
#include "native_carx.h"
#include "hook_manager.h"
#include "logger.h"

#include <algorithm>
#include <cmath>

namespace CarSimOptimizerInternal {

std::recursive_mutex s_mutex;
CarSimOptimizer::Config s_cfg{};
CarSimOptimizer::Stats s_stats{};
std::vector<CarSimOptimizer::CarEntry> s_netCarsSnapshot;

uintptr_t s_gaBase = 0;
bool s_noDamageOverride = false;
bool s_nativeExportsReady = false;

void* s_sfDeformSleep = nullptr;
void* s_sfRootSleep = nullptr;
void* s_sfRenderPolicy = nullptr;
void* s_sfOptimizeGeom = nullptr;

DeformBaseline s_deformBase{};
RootSleepBaseline s_rootBase{};
RenderPolicyBaseline s_renderBase{};
OptimizeGeomBaseline s_geomBase{};

std::unordered_set<void*> s_allowedFullSim;
std::unordered_set<void*> s_knownNetCars;
std::unordered_map<void*, std::chrono::steady_clock::time_point> s_slotEnteredAt;
std::chrono::steady_clock::time_point s_lastSwapTime{};
bool s_hasAllowedSet = false;

std::unordered_set<void*> s_effectsDisabledSet;
std::unordered_map<void*, SavedNativeCarState> s_savedNativeCars;

} // namespace CarSimOptimizerInternal

using namespace CarSimOptimizerInternal;

bool CarSimOptimizer::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_stats.initialized) return true;

    HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!hGA) {
        Logger::Warn("CarSimOptimizer: GameAssembly.dll not loaded yet, will retry.");
        return false;
    }

    s_stats.hooksInstalled = InstallHooksAndResolvers(hGA);
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

    for (auto& netEntry : s_netCarsSnapshot) {
        const LiveCarInfo* best = nullptr;
        float bestD2 = 6.25f;
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
        if (best && !best->playerName.empty()) {
            netEntry.name = best->playerName;
        } else if (best && !best->unityName.empty()) {
            netEntry.name = best->unityName;
        } else if (netEntry.name.empty()) {
            netEntry.name = "NetCar #" + std::to_string(netEntry.ownerId);
        }
    }

    if (s_sfDeformSleep) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfDeformSleep);
        Il2CppHelpers::SafeRead(p + 0x14, &s_stats.deformSleepingCars);
        Il2CppHelpers::SafeRead(p + 0x18, &s_stats.deformTransitions);
    }
    if (s_sfRootSleep) {
        uintptr_t p = reinterpret_cast<uintptr_t>(s_sfRootSleep);
        Il2CppHelpers::SafeRead(p + 0x08, &s_stats.rootSleepingRoots);
        Il2CppHelpers::SafeRead(p + 0x0C, &s_stats.rootTransitions);
    }

    s_stats.nativeExportsReady = s_nativeExportsReady;
    s_stats.deformSleepResolved = (s_sfDeformSleep != nullptr);
    s_stats.rootSleepResolved = (s_sfRootSleep != nullptr);
    s_stats.renderPolicyResolved = (s_sfRenderPolicy != nullptr);
    s_stats.optimizeGeomResolved = (s_sfOptimizeGeom != nullptr);
    s_stats.effectsDisabledCars = static_cast<int32_t>(s_effectsDisabledSet.size());
    s_stats.offlineReducedCars = static_cast<int32_t>(s_savedNativeCars.size());
    s_stats.noDamageActive = s_cfg.disableDamage || s_noDamageOverride;
    s_stats.noTireWearActive = s_cfg.disableTireWear || NativeCarX::IsNoTireWear();

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
    if (s_noDamageOverride) {
        clamped.disableDamage = true;
    }
    const bool damageTurnedOn = clamped.disableDamage && !s_cfg.disableDamage;
    s_cfg = clamped;
    if (damageTurnedOn) {
        UnityBridge::RequestPlayerCarRepair();
    }
    if (!s_noDamageOverride) {
        NativeCarX::SetNoDamage(clamped.disableDamage);
    }
    if (clamped.disableTireWear) {
        NativeCarX::SetNoTireWear(true);
    }
}

void CarSimOptimizer::ResetDefaults() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_lastSwapTime = {};
    s_slotEnteredAt.clear();
    s_stats.antiFlickerHolding = false;
    s_cfg = Config{};
    s_noDamageOverride = false;
    NativeCarX::SetNoDamage(false);
    NativeCarX::SetNoTireWear(false);
}

CarSimOptimizer::Stats CarSimOptimizer::GetStats() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_stats;
}

std::vector<CarSimOptimizer::CarEntry> CarSimOptimizer::GetNetCarsSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_netCarsSnapshot;
}

void CarSimOptimizer::SetNoDamageOverride(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_noDamageOverride == enable) return;
    s_noDamageOverride = enable;
    NativeCarX::SetNoDamage(enable || s_cfg.disableDamage);
    if (enable) {
        UnityBridge::RequestPlayerCarRepair();
    }
}

bool CarSimOptimizer::GetNoDamageOverride() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_noDamageOverride || s_cfg.disableDamage;
}

void CarSimOptimizer::SetNoTireWearOverride(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_cfg.disableTireWear = enable;
    NativeCarX::SetNoTireWear(enable);
}
