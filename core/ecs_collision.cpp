#include "ecs_collision.h"
#include "ecs_collision_internal.h"
#include "unity_bridge.h"
#include "native_carx.h"
#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace EcsCollisionInternal;

namespace {

struct CachedColliderBaseline {
    uint64_t entityKey = 0;
    void*    blobPtr = nullptr;
    uint8_t  colliderType = 0;
    CollisionFilter origParent{};
    int      childCount = 0;
    void*    childPtrs[8] = { nullptr };
    CollisionFilter origChildren[8]{};
    bool     modifiedCollidesMask = false;
    uint32_t removedMask = 0;
    bool     modifiedGroupIndex = false;
};

struct AutoLatchState {
    float holdTimerSec = 0.0f;
    bool  wasGhosted = false;
};

static SelectiveGhostConfig s_config{};

static std::unordered_map<std::string, CarGhostOverride> s_overridesByName;
static std::unordered_map<uintptr_t, CarGhostOverride>   s_overridesByPtr;
static std::unordered_map<uint64_t, CachedColliderBaseline> s_baselines;
static std::unordered_map<uint64_t, AutoLatchState>      s_autoLatch;
static std::vector<CarGhostState> s_latestStates;


static bool CaptureColliderBaseline(uint64_t entityKey, void* blobPtr, CachedColliderBaseline& out) {
    uintptr_t blob = reinterpret_cast<uintptr_t>(blobPtr);
    if (blob < 0x10000) return false;

    uint8_t btype = 0xFF;
    CollisionFilter pFilter{};
    if (!SafeRead(blob, btype) || !SafeRead(blob + 0x08, pFilter)) return false;
    if (pFilter.belongsTo == 0) return false;

    out = CachedColliderBaseline{};
    out.entityKey = entityKey;
    out.blobPtr = blobPtr;
    out.colliderType = btype;
    out.origParent = pFilter;

    if (btype == COLLIDER_COMPOUND) {
        int32_t nc = 0;
        if (!SafeRead(blob + 0x5C, nc) || nc <= 0 || nc > 8) return false;
        out.childCount = nc;
        for (int k = 0; k < nc; ++k) {
            uintptr_t relPtr = blob + 0x60 + static_cast<size_t>(k) * 40 + 0x1C;
            int32_t rel = 0;
            if (!SafeRead(relPtr, rel) || rel <= 0 || rel > 0x100000) return false;
            uintptr_t kc = relPtr + rel;
            CollisionFilter cf{};
            if (!SafeRead(kc + 0x08, cf) || cf.belongsTo == 0) return false;
            out.childPtrs[k] = reinterpret_cast<void*>(kc);
            out.origChildren[k] = cf;
        }
    }
    return true;
}

static CachedColliderBaseline* EnsureBaseline(uint64_t entityKey, void* blobPtr) {
    auto it = s_baselines.find(entityKey);
    if (it != s_baselines.end() && it->second.blobPtr == blobPtr) {
        return &it->second;
    }
    CachedColliderBaseline fresh;
    if (!CaptureColliderBaseline(entityKey, blobPtr, fresh)) return nullptr;
    auto res = s_baselines.emplace(entityKey, fresh);
    if (!res.second) res.first->second = fresh;
    return &res.first->second;
}

static bool ApplyFilterToCollider(void* colliderPtr, const CollisionFilter& f) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(colliderPtr);
    if (addr < 0x10000) return false;
    return SafeWrite(addr + 0x08, f);
}

static void RestoreBaseline(CachedColliderBaseline& base) {
    if (!base.modifiedCollidesMask && !base.modifiedGroupIndex) return;

    CollisionFilter curP{};
    if (SafeRead(reinterpret_cast<uintptr_t>(base.blobPtr) + 0x08, curP)) {
        CollisionFilter restoredP = curP;
        if (base.modifiedCollidesMask) {
            restoredP.collidesWith |= (base.origParent.collidesWith & base.removedMask);
        }
        if (base.modifiedGroupIndex) {
            restoredP.groupIndex = base.origParent.groupIndex;
        }
        ApplyFilterToCollider(base.blobPtr, restoredP);
    }

    if (base.colliderType == COLLIDER_COMPOUND) {
        for (int k = 0; k < base.childCount; ++k) {
            if (!base.childPtrs[k]) continue;
            CollisionFilter curC{};
            if (SafeRead(reinterpret_cast<uintptr_t>(base.childPtrs[k]) + 0x08, curC)) {
                CollisionFilter restoredC = curC;
                if (base.modifiedCollidesMask) {
                    restoredC.collidesWith |= (base.origChildren[k].collidesWith & base.removedMask);
                }
                if (base.modifiedGroupIndex) {
                    restoredC.groupIndex = base.origChildren[k].groupIndex;
                }
                ApplyFilterToCollider(base.childPtrs[k], restoredC);
            }
        }
    }

    base.modifiedCollidesMask = false;
    base.removedMask = 0;
    base.modifiedGroupIndex = false;
}

static void RestoreAllModifiedBaselines(const std::unordered_set<void*>* validActiveBlobs = nullptr) {
    for (auto& kv : s_baselines) {
        CachedColliderBaseline& b = kv.second;
        if (!b.modifiedCollidesMask && !b.modifiedGroupIndex) continue;
        if (validActiveBlobs && validActiveBlobs->find(b.blobPtr) == validActiveBlobs->end()) {
            b.modifiedCollidesMask = false;
            b.modifiedGroupIndex = false;
            continue;
        }
        RestoreBaseline(b);
    }
}

static void ApplyBitmaskGhost(CachedColliderBaseline& target, uint32_t playerBelongsMask) {
    if (target.modifiedGroupIndex) {
        RestoreBaseline(target);
    }

    uint32_t combinedChildMask = 0;
    if (target.colliderType == COLLIDER_COMPOUND && target.childCount > 0) {
        for (int k = 0; k < target.childCount; ++k) {
            CollisionFilter cf = target.origChildren[k];
            SafeRead(reinterpret_cast<uintptr_t>(target.childPtrs[k]) + 0x08, cf);
            // If we hadn't modified yet, refresh clean baseline collidesWith first
            if (!target.modifiedCollidesMask) {
                target.origChildren[k] = cf;
            }
            cf.collidesWith = target.origChildren[k].collidesWith & ~playerBelongsMask;
            combinedChildMask |= cf.collidesWith;
            ApplyFilterToCollider(target.childPtrs[k], cf);
        }
    }

    CollisionFilter pf = target.origParent;
    if (target.colliderType == COLLIDER_COMPOUND && target.childCount > 0) {
        pf.collidesWith = combinedChildMask | (target.origParent.collidesWith & ~playerBelongsMask & ~0x300u);
        pf.collidesWith &= ~playerBelongsMask;
    } else {
        pf.collidesWith &= ~playerBelongsMask;
    }
    ApplyFilterToCollider(target.blobPtr, pf);

    target.modifiedCollidesMask = true;
    target.removedMask = playerBelongsMask;
}

static void ApplyGroupIndexGhost(CachedColliderBaseline& target) {
    if (target.modifiedCollidesMask) {
        RestoreBaseline(target);
    }

    CollisionFilter pf = target.origParent;
    pf.groupIndex = GHOST_GROUP_INDEX;
    ApplyFilterToCollider(target.blobPtr, pf);

    if (target.colliderType == COLLIDER_COMPOUND) {
        for (int k = 0; k < target.childCount; ++k) {
            CollisionFilter cf = target.origChildren[k];
            cf.groupIndex = GHOST_GROUP_INDEX;
            ApplyFilterToCollider(target.childPtrs[k], cf);
        }
    }

    target.modifiedGroupIndex = true;
}

// ================= Heading / Wrong-Way Evaluation =================

static bool Normalize2D(float x, float z, float& outX, float& outZ) {
    float len = sqrtf(x * x + z * z);
    if (len < 1e-4f) return false;
    outX = x / len;
    outZ = z / len;
    return true;
}

static bool ComputeCarMotionDir(const RawEcsCar& ecsCar, const LiveCarInfo* matchedLive, float& outDirX, float& outDirZ, float& outSpeedKmh) {
    float vx = ecsCar.vel[0], vz = ecsCar.vel[2];
    bool hasVel = ecsCar.hasVel;

    if (!hasVel && matchedLive && matchedLive->carPtr) {
        float nv[3] = { 0 };
        if (NativeCarX::GetCarVelocity(matchedLive->carPtr, nv)) {
            vx = nv[0]; vz = nv[2];
            hasVel = std::isfinite(vx) && std::isfinite(vz);
        }
    }

    if (hasVel) {
        float spdMs = sqrtf(vx * vx + vz * vz);
        outSpeedKmh = spdMs * 3.6f;
        if (spdMs >= 1.2f && Normalize2D(vx, vz, outDirX, outDirZ)) {
            return true;
        }
    } else if (matchedLive) {
        outSpeedKmh = matchedLive->speedKmh;
    } else {
        outSpeedKmh = 0.0f;
    }

    if (ecsCar.hasFwd && Normalize2D(ecsCar.fwd[0], ecsCar.fwd[2], outDirX, outDirZ)) {
        return true;
    }
    return false;
}


} // namespace

// ================= Public Ghost API =================

void EcsCollision::Init() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ResolveGameAssembly();
    TryResolveTypeIndices();
}

void EcsCollision::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (NativeCarX::IsInSession()) {
        EcsStores stores;
        if (GetEcsStores(stores)) {
            std::vector<RawEcsCar> ecsCars;
            CollectRawEcsCars(stores, g_lastSeenCarCount, ecsCars);
            std::unordered_set<void*> activeBlobs;
            for (const auto& c : ecsCars) activeBlobs.insert(c.blobPtr);
            RestoreAllModifiedBaselines(&activeBlobs);
        }
    }
    s_baselines.clear();
    s_autoLatch.clear();
    g_cachedCarChunks.clear();
    s_latestStates.clear();
}

void EcsCollision::SetConfig(const SelectiveGhostConfig& cfg) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    s_config = cfg;
    s_config.minSpeedKmh = std::clamp(s_config.minSpeedKmh, 0.0f, 120.0f);
    s_config.dotThreshold = std::clamp(s_config.dotThreshold, -0.95f, 0.25f);
    s_config.maxDistanceMeters = std::clamp(s_config.maxDistanceMeters, 0.0f, 1000.0f);
}

SelectiveGhostConfig EcsCollision::GetConfig() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return s_config;
}

void EcsCollision::SetCarOverride(const std::string& unityName, uintptr_t carPtr, CarGhostOverride mode) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!unityName.empty()) {
        if (mode == CarGhostOverride::Auto) s_overridesByName.erase(unityName);
        else s_overridesByName[unityName] = mode;
    }
    if (carPtr) {
        if (mode == CarGhostOverride::Auto) s_overridesByPtr.erase(carPtr);
        else s_overridesByPtr[carPtr] = mode;
    }
}

CarGhostOverride EcsCollision::GetCarOverride(const std::string& unityName, uintptr_t carPtr) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!unityName.empty()) {
        auto it = s_overridesByName.find(unityName);
        if (it != s_overridesByName.end()) return it->second;
    }
    if (carPtr) {
        auto it = s_overridesByPtr.find(carPtr);
        if (it != s_overridesByPtr.end()) return it->second;
    }
    return CarGhostOverride::Auto;
}

void EcsCollision::ClearOverrides() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    s_overridesByName.clear();
    s_overridesByPtr.clear();
}

std::vector<CarGhostState> EcsCollision::GetCarStates() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return s_latestStates;
}

void EcsCollision::UpdateOnMainThread(float dt, const std::vector<LiveCarInfo>& liveCars) {
    if (!UnityBridge::IsOnMainThread()) return;

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!NativeCarX::IsInSession()) {
        s_baselines.clear();
        s_autoLatch.clear();
        g_cachedCarChunks.clear();
        s_latestStates.clear();
        return;
    }

    TryResolveTypeIndices();

    EcsStores stores;
    if (!GetEcsStores(stores)) return;

    std::vector<RawEcsCar> ecsCars;
    CollectRawEcsCars(stores, liveCars.size(), ecsCars);
    if (ecsCars.empty()) {
        s_latestStates.clear();
        return;
    }

    std::unordered_set<void*> activeBlobs;
    std::unordered_set<uint64_t> activeEntities;
    for (const auto& ec : ecsCars) {
        activeBlobs.insert(ec.blobPtr);
        activeEntities.insert(ec.entityKey);
    }

    // Match each ECS car entity to LiveCarInfo for stable naming and UI mapping.
    std::vector<const LiveCarInfo*> matchedLive(ecsCars.size(), nullptr);
    std::vector<bool> liveUsed(liveCars.size(), false);

    for (size_t i = 0; i < ecsCars.size(); ++i) {
        if (!ecsCars[i].isPlayer) continue;
        for (size_t j = 0; j < liveCars.size(); ++j) {
            if (!liveUsed[j] && liveCars[j].isLocalPlayer) {
                matchedLive[i] = &liveCars[j];
                liveUsed[j] = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < ecsCars.size(); ++i) {
        if (matchedLive[i] || !ecsCars[i].hasPos) continue;
        float bestD2 = 25.0f; // within 5m
        size_t bestIdx = SIZE_MAX;
        for (size_t j = 0; j < liveCars.size(); ++j) {
            if (liveUsed[j]) continue;
            float dx = liveCars[j].x - ecsCars[i].pos[0];
            float dy = liveCars[j].y - ecsCars[i].pos[1];
            float dz = liveCars[j].z - ecsCars[i].pos[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) {
                bestD2 = d2;
                bestIdx = j;
            }
        }
        if (bestIdx != SIZE_MAX) {
            matchedLive[i] = &liveCars[bestIdx];
            liveUsed[bestIdx] = true;
        }
    }

    // Prune stale baselines/latches cleanly.
    for (auto it = s_baselines.begin(); it != s_baselines.end();) {
        if (activeEntities.find(it->first) == activeEntities.end()) it = s_baselines.erase(it);
        else ++it;
    }
    for (auto it = s_autoLatch.begin(); it != s_autoLatch.end();) {
        if (activeEntities.find(it->first) == activeEntities.end()) it = s_autoLatch.erase(it);
        else ++it;
    }

    // Locate Player ECS car & reference heading/position.
    size_t playerIdx = SIZE_MAX;
    for (size_t i = 0; i < ecsCars.size(); ++i) {
        if (ecsCars[i].isPlayer) {
            playerIdx = i;
            break;
        }
    }

    if (playerIdx == SIZE_MAX) {
        RestoreAllModifiedBaselines(&activeBlobs);
        s_latestStates.clear();
        return;
    }

    CachedColliderBaseline* playerBase = EnsureBaseline(ecsCars[playerIdx].entityKey, ecsCars[playerIdx].blobPtr);
    if (!playerBase) {
        RestoreAllModifiedBaselines(&activeBlobs);
        return;
    }

    float pDirX = 0.0f, pDirZ = 1.0f, pSpeedKmh = 0.0f;
    bool hasPDir = ComputeCarMotionDir(ecsCars[playerIdx], matchedLive[playerIdx], pDirX, pDirZ, pSpeedKmh);
    const uint32_t playerBelongs = playerBase->origParent.belongsTo;

    std::vector<CarGhostState> states;
    states.reserve(ecsCars.size());

    bool needPlayerGroupIndex = false;

    for (size_t i = 0; i < ecsCars.size(); ++i) {
        const RawEcsCar& ec = ecsCars[i];
        const LiveCarInfo* lc = matchedLive[i];

        CachedColliderBaseline* base = EnsureBaseline(ec.entityKey, ec.blobPtr);

        CarGhostState st;
        st.entityKey = ec.entityKey;
        st.carPtr = lc ? lc->carPtr : nullptr;
        st.unityName = lc ? lc->unityName : (ec.isPlayer ? "Player Car" : "Opponent");
        st.isLocalPlayer = ec.isPlayer;
        st.isGameGhosted = ec.isGameGhosted;

        float oDirX = 0.0f, oDirZ = 1.0f, oSpeedKmh = 0.0f;
        bool hasODir = ComputeCarMotionDir(ec, lc, oDirX, oDirZ, oSpeedKmh);
        st.speedKmh = oSpeedKmh;

        float d2 = 1e9f;
        if (ec.hasPos && ecsCars[playerIdx].hasPos) {
            float dx = ec.pos[0] - ecsCars[playerIdx].pos[0];
            float dy = ec.pos[1] - ecsCars[playerIdx].pos[1];
            float dz = ec.pos[2] - ecsCars[playerIdx].pos[2];
            d2 = dx * dx + dy * dy + dz * dz;
            st.distanceMeters = sqrtf(d2);
        }

        if (!ec.isPlayer && hasPDir && hasODir) {
            st.headingDot = pDirX * oDirX + pDirZ * oDirZ;
        } else {
            st.headingDot = 1.0f;
        }

        if (!ec.isPlayer) {
            st.overrideMode = GetCarOverride(st.unityName, reinterpret_cast<uintptr_t>(st.carPtr));

            bool rawWrongWay = false;
            if (s_config.enabled && s_config.antiWrongWay && hasPDir && hasODir) {
                bool speedOk = (oSpeedKmh >= s_config.minSpeedKmh) || (s_config.minSpeedKmh <= 1.0f);
                bool distOk = (s_config.maxDistanceMeters <= 0.0f) || (st.distanceMeters <= s_config.maxDistanceMeters);
                if (speedOk && distOk && st.headingDot < s_config.dotThreshold) {
                    rawWrongWay = true;
                }
            }

            AutoLatchState& latch = s_autoLatch[ec.entityKey];
            bool latchedWrongWay = rawWrongWay;
            if (!s_config.enabled || !s_config.antiWrongWay || st.overrideMode != CarGhostOverride::Auto) {
                latch.wasGhosted = false;
                latch.holdTimerSec = 0.0f;
                latchedWrongWay = rawWrongWay;
            } else if (rawWrongWay) {
                latch.wasGhosted = true;
                latch.holdTimerSec = RELEASE_HYSTERESIS_SEC;
                latchedWrongWay = true;
            } else if (latch.wasGhosted) {
                // Keep ghosted while cars are overlapping (< 6m) or within release debounce window
                if (d2 < OVERLAP_SAFETY_DIST_SQ) {
                    latch.holdTimerSec = RELEASE_HYSTERESIS_SEC;
                    latchedWrongWay = true;
                } else if (latch.holdTimerSec > 0.0f) {
                    latch.holdTimerSec -= dt;
                    latchedWrongWay = true;
                } else {
                    latch.wasGhosted = false;
                    latchedWrongWay = false;
                }
            }
            st.isWrongWay = rawWrongWay;

            bool shouldGhost = false;
            if (s_config.enabled) {
                if (st.overrideMode == CarGhostOverride::ForceGhost) {
                    shouldGhost = true;
                } else if (st.overrideMode == CarGhostOverride::ForceSolid) {
                    shouldGhost = false;
                } else {
                    shouldGhost = latchedWrongWay;
                }
            }

            if (base) {
                if (shouldGhost) {
                    if ((base->origParent.belongsTo & playerBelongs) == 0) {
                        ApplyBitmaskGhost(*base, playerBelongs);
                    } else {
                        ApplyGroupIndexGhost(*base);
                        needPlayerGroupIndex = true;
                    }
                    st.isSelectivelyGhosted = true;
                } else {
                    RestoreBaseline(*base);
                    st.isSelectivelyGhosted = false;
                }
            }
        }

        if (base) {
            CollisionFilter curP = base->origParent;
            SafeRead(reinterpret_cast<uintptr_t>(base->blobPtr) + 0x08, curP);
            st.colliderBelongsTo = curP.belongsTo;
            st.colliderCollidesWith = curP.collidesWith;
            st.colliderGroupIndex = curP.groupIndex;
        }
        states.push_back(std::move(st));
    }

    if (needPlayerGroupIndex && s_config.enabled) {
        ApplyGroupIndexGhost(*playerBase);
    } else {
        RestoreBaseline(*playerBase);
    }

    s_latestStates = std::move(states);
}
