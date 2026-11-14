#include "unity_bridge.h"
#include "unity_bridge_internal.h"
#include "carfx.h"
#include "native_carx.h"
#include "replay_engine.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace UnityBridgeInternal;

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

void UnityBridge::IsolateGhostSlotCarFXTextures(size_t slotIdx, GhostSlot& slot) {
    if (!s_unityBase || !slot.gameObject) return;
    int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(slotIdx));
    if (gTarget == CarFX::kTargetNone) return;

    std::vector<BodyMatSlotRef> bodySlots;
    CollectBodyMaterialSlots(s_unityBase, slot.gameObject, true, bodySlots);
    if (bodySlots.empty()) {
        CarFX::ClearTargetTextures(gTarget);
        return;
    }

    int32_t isoMatId = 0;
    if (!SafeCloneOrAssignRendererMatSEH(s_unityBase, bodySlots[0].rendererId, bodySlots[0].slotIdx, 0, &isoMatId) || isoMatId >= 0) {
        CarFX::ClearTargetTextures(gTarget);
        return;
    }
    int multiSlotCount = (bodySlots[0].slotIdx != 0) ? 1 : 0;
    int assignOk = 1;
    int assignFail = 0;
    for (size_t i = 1; i < bodySlots.size(); ++i) {
        if (bodySlots[i].slotIdx != 0) multiSlotCount++;
        if (SafeCloneOrAssignRendererMatSEH(s_unityBase, bodySlots[i].rendererId, bodySlots[i].slotIdx, isoMatId, nullptr)) {
            assignOk++;
        } else {
            assignFail++;
        }
    }

    void* natMat = nullptr;
    int32_t origQ = 2200;
    if (!SafeResolveMatAndQueueSEH(s_unityBase, isoMatId, &natMat, &origQ) || !natMat) return;

    slot.isolatedBodyMatId = isoMatId;
    slot.savedBodyOrigQueue = origQ;

    void* natWhite = nullptr;
    void* natBlack = nullptr;
    void* natNormal = nullptr;
    GetBuiltinNativeTextures(s_unityBase, &natWhite, &natBlack, &natNormal);

    void* origAlb = SafeGetMatTextureSEH(s_unityBase, natMat, 380);   // u_sTexture
    void* origPack = SafeGetMatTextureSEH(s_unityBase, natMat, 1370); // u_sPackedMap
    const bool albIsDef = IsDefaultNativeTex(origAlb, natWhite, natBlack, natNormal);
    const bool packIsDef = IsDefaultNativeTex(origPack, natWhite, natBlack, natNormal);

    const uintptr_t pfnGetNatPtr = GetNativeTexPtrFn(s_unityBase);
    void* albSrvPtr = nullptr;
    if (origAlb) SafeCallGetNativeTexPtr(pfnGetNatPtr, origAlb, &albSrvPtr);

    void* packSrvPtr = nullptr;
    bool copyOk = false;
    void* clonedPack = CreateIsolatedPackedTexture(origPack, packIsDef, natWhite, natBlack, &packSrvPtr, &copyOk);

    if (clonedPack && packSrvPtr && SafeSetMatTextureSEH(s_unityBase, natMat, 1370, clonedPack)) {
        slot.clonedPackTex = clonedPack;
        slot.carFxIsolated = true;
    } else {
        if (clonedPack) DestroyObjectDirect(clonedPack, 0.0f);
        packSrvPtr = nullptr;
        if (origPack) SafeCallGetNativeTexPtr(pfnGetNatPtr, origPack, &packSrvPtr);
    }

    CarFX::TargetTexturesInput in{};
    in.nativeAlbedoPtr = albSrvPtr;
    in.nativePackedPtr = packSrvPtr;
    in.albedoIsDefault = albIsDef;
    in.packedIsDefault = slot.carFxIsolated ? false : packIsDef;
    in.valid = (in.nativePackedPtr != nullptr) || (in.nativeAlbedoPtr != nullptr);
    CarFX::SetPlayerTargetTextures(gTarget, in);

    char dbg[256];
    _snprintf_s(dbg, sizeof(dbg), _TRUNCATE,
                "Ghost[%zu] CarFX isolated: mat=%d q=%d bodySlots=%zu(ok=%d,fail=%d,multi=%d) albSrv=%p(def=%d) packSrv=%p(iso=%d,copy=%d)",
                slotIdx, isoMatId, origQ, bodySlots.size(), assignOk, assignFail, multiSlotCount,
                albSrvPtr, (int)albIsDef, packSrvPtr, (int)slot.carFxIsolated, (int)copyOk);
    Logger::Info(dbg);
}

void UnityBridge::SyncGhostSlotCarFXMaterialsAndShader(size_t slotIdx, GhostSlot& slot, bool allowReplayShader) {
    int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(slotIdx));
    if (gTarget == CarFX::kTargetNone) return;

    if (!allowReplayShader) {
        if (CarFX::IsTargetCompiled(gTarget)) {
            CarFX::UnloadShader(gTarget);
        }
    } else {
        CarFX::ReplayShaderConfig recCfg;
        int recTrackIdx = 0;
        if (ReplayEngine::GetLoadedCarFx(recCfg, &recTrackIdx) && recTrackIdx == static_cast<int>(slotIdx)) {
            if (!slot.replayShaderApplied) {
                slot.replayShaderApplied = true;
                slot.replayShaderSourceKey = recCfg.shaderPath;
                CarFX::ApplyConfigToTarget(gTarget, recCfg);
            }
        } else if (s_ghostMirrorPlayerShader.load(std::memory_order_relaxed)) {
            slot.replayShaderApplied = false;
            CarFX::SyncTargetFromSource(gTarget, CarFX::kTargetBody);
        } else {
            slot.replayShaderApplied = false;
            if (CarFX::IsTargetCompiled(gTarget)) {
                CarFX::UnloadShader(gTarget);
            }
        }
    }

    if (slot.isolatedBodyMatId < 0 && s_unityBase) {
        const bool wantTrans = CarFX::ShouldBypassDepthPrePass(gTarget);
        const bool wantStl = CarFX::ShouldHideCarInternals(gTarget);
        const int32_t targetQ = wantStl ? 2510 : 2590;
        SafeSyncMaterialQueueAndDepthSEH(s_unityBase, slot.isolatedBodyMatId, wantTrans, targetQ, true, slot.savedBodyOrigQueue, nullptr, nullptr);
    }
}

void UnityBridge::DestroyDetachedGhostSlot(size_t slotIdx, GhostSlot& detached) {
    int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(slotIdx));
    if (gTarget != CarFX::kTargetNone) {
        CarFX::UnloadShader(gTarget);
        CarFX::ClearTargetTextures(gTarget);
    }
    if (detached.clonedPackTex) {
        DestroyObjectDirect(detached.clonedPackTex, 0.0f);
    }
    if (detached.gameObject) {
        DestroyObjectDirect(detached.gameObject, 0.0f);
    }
    detached = GhostSlot{};
}

void* UnityBridge::SpawnGhostSlotInternal(size_t slotIdx, const std::string& preferredCarName) {
    // Detach any prior slot under brief lock, then destroy/spawn/isolate outside s_mutex
    // so synchronous Unity Gfx calls (GetNativeTexturePtr/CopyTexture) can never deadlock Present().
    GhostSlot oldSlot;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (slotIdx < s_ghostSlots.size()) {
            oldSlot = std::move(s_ghostSlots[slotIdx]);
            s_ghostSlots[slotIdx] = GhostSlot{};
        }
    }
    DestroyDetachedGhostSlot(slotIdx, oldSlot);

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
    GhostSlot slot;
    slot.gameObject = cloneGo;
    slot.transform = cloneTf;
    slot.sourceCarName = carName;

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
    IsolateGhostSlotCarFXTextures(slotIdx, slot);

    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (slotIdx >= s_ghostSlots.size()) {
            s_ghostSlots.resize(slotIdx + 1);
        }
        s_ghostSlots[slotIdx] = std::move(slot);
        if (slotIdx == 0) {
            s_playerNativeObj = modelGo;
        }
    }

    Logger::Info("Spawned 3D Ghost[" + std::to_string(slotIdx) + "] from '" + carName + "/Model(Clone)'");
    return cloneGo;
}

void UnityBridge::DestroyGhostSlotInternal(size_t slotIdx) {
    GhostSlot detached;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (slotIdx >= s_ghostSlots.size()) return;
        detached = std::move(s_ghostSlots[slotIdx]);
        s_ghostSlots[slotIdx] = GhostSlot{};
    }
    DestroyDetachedGhostSlot(slotIdx, detached);
}

void UnityBridge::DestroyAllGhostSlotsInternal() {
    std::vector<GhostSlot> oldSlots;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        oldSlots = std::move(s_ghostSlots);
        s_ghostSlots.clear();
    }
    CarFX::ResetGhostTargets();
    int destroyed = 0;
    for (size_t i = 0; i < oldSlots.size(); ++i) {
        if (oldSlots[i].clonedPackTex) {
            DestroyObjectDirect(oldSlots[i].clonedPackTex, 0.0f);
            oldSlots[i].clonedPackTex = nullptr;
        }
        if (oldSlots[i].gameObject) {
            DestroyObjectDirect(oldSlots[i].gameObject, 0.0f);
            destroyed++;
        }
    }
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

    const float totalDur = ReplayEngine::GetReplayDuration();

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

        // Compute clean velocity vector from adjacent replay samples (works even while paused or on frame 0!)
        UnityVector3 velVec{ 0.0f, 0.0f, 0.0f };
        const float eps = 0.04f;
        const float t0 = (std::max)(0.0f, targetTime - eps);
        const float t1 = (totalDur > 0.0f) ? (std::min)(totalDur, targetTime + eps) : (targetTime + eps);
        ReplayFrame f0{}, f1{};
        if ((t1 - t0) > 0.005f &&
            ReplayEngine::GetInterpolatedTrackFrame(static_cast<int>(t), t0, f0) &&
            ReplayEngine::GetInterpolatedTrackFrame(static_cast<int>(t), t1, f1)) {
            const float invSpan = 1.0f / (t1 - t0);
            velVec.x = ((f1.transformMatrix[12] + f1.transformMatrix[0] * offsetRight) -
                        (f0.transformMatrix[12] + f0.transformMatrix[0] * offsetRight)) * invSpan;
            velVec.y = ((f1.transformMatrix[13] + f1.transformMatrix[1] * offsetRight) -
                        (f0.transformMatrix[13] + f0.transformMatrix[1] * offsetRight)) * invSpan;
            velVec.z = ((f1.transformMatrix[14] + f1.transformMatrix[2] * offsetRight) -
                        (f0.transformMatrix[14] + f0.transformMatrix[2] * offsetRight)) * invSpan;
        }

        bool hasActiveSlot = false;
        float rAccel = (frame.flags & (1u << 2)) ? 1.0f : 0.0f;
        float rBrake = (frame.flags & (1u << 0)) ? 1.0f : 0.0f;
        float rHandbrake = (frame.flags & (1u << 1)) ? 1.0f : 0.0f;
        float rSteerDeg = frame.steerAngle * 57.2957795f;
        GhostSlot syncCopy;
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            if (t < s_ghostSlots.size() && s_ghostSlots[t].gameObject) {
                GhostSlot& slot = s_ghostSlots[t];
                slot.replayTeleValid = true;
                slot.replaySpeedKmh = frame.speedKmh;
                slot.replayRpm = frame.rpm;
                slot.replayGear = frame.gear;
                slot.replaySteerDeg = rSteerDeg;
                slot.replayAccel = rAccel;
                slot.replayBrake = rBrake;
                slot.replayHandbrake = rHandbrake;
                slot.replayWorldVel = velVec;
                syncCopy.isolatedBodyMatId = slot.isolatedBodyMatId;
                syncCopy.savedBodyOrigQueue = slot.savedBodyOrigQueue;
                syncCopy.replayShaderApplied = slot.replayShaderApplied;
                syncCopy.replayShaderSourceKey = slot.replayShaderSourceKey;
                hasActiveSlot = true;
            }
        }

        if (hasActiveSlot) {
            const int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(t));
            if (gTarget != CarFX::kTargetNone) {
                SyncGhostSlotCarFXMaterialsAndShader(t, syncCopy, true);
                {
                    std::lock_guard<std::recursive_mutex> lock(s_mutex);
                    if (t < s_ghostSlots.size()) {
                        s_ghostSlots[t].replayShaderApplied = syncCopy.replayShaderApplied;
                        s_ghostSlots[t].replayShaderSourceKey = std::move(syncCopy.replayShaderSourceKey);
                    }
                }

                const float speedFactor = std::clamp(frame.speedKmh / 35.0f, 0.0f, 1.0f);
                const float steerRad = fabsf(frame.steerAngle);
                const float driftNorm = std::clamp(steerRad * 2.2f * speedFactor +
                    (rHandbrake > 0.5f ? 0.4f * speedFactor : 0.0f), 0.0f, 1.0f);

                CarFX::SetGameInput(0, frame.speedKmh, gTarget);
                CarFX::SetGameInput(1, frame.rpm, gTarget);
                CarFX::SetGameInput(2, fabsf(rSteerDeg), gTarget);
                CarFX::SetGameInput(3, driftNorm, gTarget);
                CarFX::SetGameInput(4, rAccel, gTarget);
                CarFX::SetGameInput(5, rBrake, gTarget);
                CarFX::SetGameInput(6, static_cast<float>(frame.gear), gTarget);
            }
        }
    }

    // Remove surplus ghost slots if switching to a replay with fewer tracks
    std::vector<GhostSlot> surplus;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (s_ghostSlots.size() > trackCount) {
            surplus.assign(std::make_move_iterator(s_ghostSlots.begin() + trackCount),
                           std::make_move_iterator(s_ghostSlots.end()));
            s_ghostSlots.resize(trackCount);
        }
    }
    for (size_t i = 0; i < surplus.size(); ++i) {
        DestroyDetachedGhostSlot(trackCount + i, surplus[i]);
    }
}

void UnityBridge::QuatToAxes(const UnityVector4& qIn, UnityVector3& right, UnityVector3& up, UnityVector3& fwd) {
    UnityVector4 q = qIn;
    float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq > 1e-8f) {
        float inv = 1.0f / sqrtf(lenSq);
        q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
    } else {
        q = { 0.0f, 0.0f, 0.0f, 1.0f };
    }
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    right = VecNormalize({ 1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy) });
    up    = VecNormalize({ 2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx) });
    fwd   = VecNormalize({ 2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy) });
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
