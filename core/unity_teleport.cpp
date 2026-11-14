#include "unity_bridge.h"
#include "unity_bridge_internal.h"
#include "pattern_scanner.h"
#include "game_build.h"
#include "native_carx.h"
#include "logger.h"
#include <cmath>
#include <cstring>
#include <psapi.h>

using namespace UnityBridgeInternal;

namespace {

static bool SafeCallRaceCarTeleportSEH(uintptr_t gaBase, void* raceCarPtr,
                                       const Float4x4* mat,
                                       const UnityVector3* linVel,
                                       const UnityVector3* angVel) {
    if (!gaBase || !raceCarPtr || !mat || !linVel || !angVel) return false;
    __try {
        uintptr_t rc = reinterpret_cast<uintptr_t>(raceCarPtr);
        if (!IsReadableMem(rc, 0x160)) return false;
        uintptr_t klass = *reinterpret_cast<uintptr_t*>(rc);
        if (!IsReadableMem(klass, 0x18)) return false;
        const char* kname = *reinterpret_cast<const char**>(klass + 0x10);
        if (!IsReadableMem(reinterpret_cast<uintptr_t>(kname), 16) ||
            strcmp(kname, "RaceCar") != 0) {
            return false;
        }
        uintptr_t localToWorldObj = *reinterpret_cast<uintptr_t*>(rc + 0x140);
        uintptr_t velObj = *reinterpret_cast<uintptr_t*>(rc + 0x148);
        uintptr_t angVelObj = *reinterpret_cast<uintptr_t*>(rc + 0x150);
        if (!IsReadableMem(localToWorldObj, 0x68) ||
            !IsReadableMem(velObj, 0x20) ||
            !IsReadableMem(angVelObj, 0x20)) {
            return false;
        }

        // Dynamically resolved RVAs in GameAssembly.dll (RaceCar):
        //   ForceVelocityChange(Vector3 velocity, Vector3 angularVelocity)
        //   ForceTransformChange(float4x4 localToWorld)
        using t_ForceVelocityChange = void(__fastcall*)(void*, const UnityVector3*, const UnityVector3*, void*);
        using t_ForceTransformChangeMat = void(__fastcall*)(void*, const Float4x4*, void*);

        static uintptr_t s_rvaVel = 0;
        static uintptr_t s_rvaTf = 0;
        if (!s_rvaVel && gaBase) {
            uintptr_t vaVel = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(gaBase),
                "48 83 EC 38 4C 8B 89 48 01 00 00 4D 85 C9 ? ? F2 0F 10 02",
                GameBuild::Pick({ 0x1DCE130, 0x239D160 }), "ForceVelocityChange");
            s_rvaVel = vaVel ? (vaVel - gaBase) : 0;

            uintptr_t vaTf = PatternScanner::FindOrFallback(reinterpret_cast<HMODULE>(gaBase),
                "48 89 5C 24 08 57 48 83 EC 60 48 8B F9 48 8B DA 48 8B 89 40 01 00 00",
                GameBuild::Pick({ 0x1DCE0B0, 0x239D0E0 }), "ForceTransformChangeMat");
            s_rvaTf = vaTf ? (vaTf - gaBase) : 0;
        }
        if (!s_rvaVel || !s_rvaTf) return false; // unknown build and signatures missed

        auto pfnVel = reinterpret_cast<t_ForceVelocityChange>(gaBase + s_rvaVel);
        auto pfnTf  = reinterpret_cast<t_ForceTransformChangeMat>(gaBase + s_rvaTf);

        if (pfnVel) pfnVel(raceCarPtr, linVel, angVel, nullptr);
        if (pfnTf) pfnTf(raceCarPtr, mat, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static Float4x4 BuildFloat4x4FromPosQuat(const UnityVector3& pos, const UnityVector4& qRaw) {
    float lenSq = qRaw.x * qRaw.x + qRaw.y * qRaw.y + qRaw.z * qRaw.z + qRaw.w * qRaw.w;
    UnityVector4 q = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (lenSq > 1e-8f) {
        float invLen = 1.0f / sqrtf(lenSq);
        q.x = qRaw.x * invLen;
        q.y = qRaw.y * invLen;
        q.z = qRaw.z * invLen;
        q.w = qRaw.w * invLen;
    }

    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    Float4x4 out{};
    // Column 0 (Right), Column 1 (Up), Column 2 (Forward), Column 3 (Translation)
    out.m[0]  = 1.0f - 2.0f * (yy + zz);
    out.m[1]  = 2.0f * (xy + wz);
    out.m[2]  = 2.0f * (xz - wy);
    out.m[3]  = 0.0f;

    out.m[4]  = 2.0f * (xy - wz);
    out.m[5]  = 1.0f - 2.0f * (xx + zz);
    out.m[6]  = 2.0f * (yz + wx);
    out.m[7]  = 0.0f;

    out.m[8]  = 2.0f * (xz + wy);
    out.m[9]  = 2.0f * (yz - wx);
    out.m[10] = 1.0f - 2.0f * (xx + yy);
    out.m[11] = 0.0f;

    out.m[12] = pos.x;
    out.m[13] = pos.y;
    out.m[14] = pos.z;
    out.m[15] = 1.0f;
    return out;
}

} // namespace

bool UnityBridge::GetPlayerCurrentPose(TeleportPose& outPose, std::string* outCarName) {
    outPose = TeleportPose{};
    if (outCarName) outCarName->clear();

    if (IsOnMainThread()) {
        RefreshLiveCarListOnMainThread(false);
    }

    void* carPtr = nullptr;
    void* rootTf = nullptr;
    std::string unityName;
    UnityVector3 cachedPos{};
    UnityVector4 cachedRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasCachedPos = false;
    bool hasCachedRot = false;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer) {
                carPtr = c.carPtr;
                rootTf = c.rootTf;
                unityName = c.unityName;
                if (c.hasPos) {
                    cachedPos = { c.x, c.y, c.z };
                    hasCachedPos = true;
                }
                if (c.hasRot) {
                    cachedRot = { c.qx, c.qy, c.qz, c.qw };
                    hasCachedRot = true;
                }
                break;
            }
        }
        if (!carPtr) {
            for (const auto& sc : s_sceneCarCache) {
                if (sc.isPlayer) {
                    carPtr = sc.nativeCarPtr;
                    rootTf = sc.rootTf;
                    unityName = sc.unityName;
                    if (sc.hasPos) { cachedPos = sc.pos; hasCachedPos = true; }
                    if (sc.hasRot) { cachedRot = sc.rot; hasCachedRot = true; }
                    break;
                }
            }
        }
    }

    CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
    if (!carPtr) {
        carPtr = tele.carPtr ? tele.carPtr : NativeCarX::GetPlayerCarPtr();
    }
    if (!carPtr && !rootTf) return false;
    if (outCarName) *outCarName = unityName;

    if (carPtr && NativeCarX::GetCarRigidBody(carPtr, outPose.rb)) {
        outPose.hasRb = true;
        NativeCarX::GetCarVelocity(carPtr, outPose.linVel);
        NativeCarX::GetCarAngularVelocity(carPtr, outPose.angVel);
    }

    UnityVector3 uPos = hasCachedPos ? cachedPos : UnityVector3{ tele.posX, tele.posY, tele.posZ };
    UnityVector4 uRot = hasCachedRot ? cachedRot : MatrixToQuaternion(tele.transformMatrix.data());
    if (rootTf && IsOnMainThread()) {
        UnityVector3 tfPos{};
        UnityVector4 tfRot{};
        if (GetTransformPosRotDirect(rootTf, tfPos, tfRot)) {
            uPos = tfPos;
            uRot = tfRot;
        }
    }

    outPose.pos[0] = uPos.x; outPose.pos[1] = uPos.y; outPose.pos[2] = uPos.z;
    outPose.rot[0] = uRot.x; outPose.rot[1] = uRot.y; outPose.rot[2] = uRot.z; outPose.rot[3] = uRot.w;
    outPose.hasQuat = true;
    return true;
}

bool UnityBridge::ApplyTeleportPlayerInternal(const TeleportPose& target, int velMode) {
    if (!IsOnMainThread()) return false;

    // Ensure our Unity car cache & CarBehaviour::m_isPlayer mapping are fresh
    RefreshLiveCarListOnMainThread(false);

    void* car = nullptr;
    void* rootTf = nullptr;
    void* raceCarPtr = nullptr;
    std::string carName;

    auto findLocalFromCache = [&]() {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto& c : s_liveCars) {
            if (c.isLocalPlayer) {
                car = c.carPtr;
                rootTf = c.rootTf;
                raceCarPtr = c.raceCarPtr;
                carName = c.unityName;
                return;
            }
        }
        for (const auto& sc : s_sceneCarCache) {
            if (sc.isPlayer) {
                car = sc.nativeCarPtr;
                rootTf = sc.rootTf;
                raceCarPtr = sc.raceCarPtr;
                carName = sc.unityName;
                return;
            }
        }
    };

    findLocalFromCache();
    if (!raceCarPtr || !rootTf || !car) {
        RefreshLiveCarListOnMainThread(true);
        findLocalFromCache();
    }
    if (!car) {
        CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
        car = player.carPtr ? player.carPtr : NativeCarX::GetPlayerCarPtr();
    }
    if (!car && !raceCarPtr && !rootTf) return false;

    UnityVector3 uPos = {
        target.hasQuat ? target.pos[0] : target.rb[20],
        target.hasQuat ? target.pos[1] : target.rb[21],
        target.hasQuat ? target.pos[2] : target.rb[22]
    };
    UnityVector4 uRot = { target.rot[0], target.rot[1], target.rot[2], target.rot[3] };
    if (!target.hasQuat) {
        // Fallback: derive quaternion from 3x3 rotation in rb[4..15]
        float m16[16] = {
            target.rb[4], target.rb[5],  target.rb[6],  0.0f,
            target.rb[8], target.rb[9],  target.rb[10], 0.0f,
            target.rb[12],target.rb[13], target.rb[14], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        uRot = MatrixToQuaternion(m16);
    }

    UnityVector3 linVel{ 0.0f, 0.0f, 0.0f };
    UnityVector3 angVel{ 0.0f, 0.0f, 0.0f };
    if (velMode == 1) {
        linVel = {
            target.hasRb ? target.rb[23] : target.linVel[0],
            target.hasRb ? target.rb[24] : target.linVel[1],
            target.hasRb ? target.rb[25] : target.linVel[2]
        };
        angVel = {
            target.hasRb ? target.rb[26] : target.angVel[0],
            target.hasRb ? target.rb[27] : target.angVel[1],
            target.hasRb ? target.rb[28] : target.angVel[2]
        };
    } else if (velMode == 2 && car) {
        float curV[3] = { 0 }, curW[3] = { 0 };
        if (NativeCarX::GetCarVelocity(car, curV)) {
            linVel = { curV[0], curV[1], curV[2] };
        }
        if (NativeCarX::GetCarAngularVelocity(car, curW)) {
            angVel = { curW[0], curW[1], curW[2] };
        }
    }

    // 1. Vanilla CarX2 Teleport pipeline via RaceCar (IRaceCar):
    //    ForceVelocityChange + ForceTransformChange fires ForceChangeForcesCalled & ForceTransformChangeCalled ->
    //    CarProxy pushes CarProxyLocationComponent & CarProxyVelocityComponent into ECS CarWriteTransformSystem /
    //    PhysicsRootWriteTransformSystem so the ECS PhysicsRoot, wheels, rigidbody & camera stay 100% synced.
    HMODULE hGA = GetModuleHandleW(L"GameAssembly.dll");
    uintptr_t gaBase = reinterpret_cast<uintptr_t>(hGA);
    Float4x4 mat4 = BuildFloat4x4FromPosQuat(uPos, uRot);
    bool vanillaOk = SafeCallRaceCarTeleportSEH(gaBase, raceCarPtr, &mat4, &linVel, &angVel);

    // 2. Sync Unity Transform immediately on MainThread (matches PlaceOnTrack -> SetPositionAndRotation)
    bool tfOk = rootTf && SetTransformPosRotDirect(rootTf, uPos, uRot);

    // Note: Do NOT overwrite NativeCarX rb[20..22] with world XYZ (in CarX2 rb[20..22] is a local/CoM offset ~ (0, 0.43, 0.06));
    // CarProxyLocationComponent & CarProxyVelocityComponent triggered by RaceCar::ForceTransformChange / ForceVelocityChange
    // propagate the new world pose & velocity to ECS CarWriteTransformSystem / PhysicsRootWriteTransformSystem cleanly.
    // Only touch NativeCarX::SetCarRigidBody for velocity updates if vanilla RaceCar failed.
    bool nativeOk = false;
    if (!vanillaOk && car) {
        float rb[29] = { 0 };
        if (NativeCarX::GetCarRigidBody(car, rb)) {
            if (target.hasRb) {
                for (int i = 4; i < 16; ++i) rb[i] = target.rb[i];
            } else if (target.hasQuat) {
                for (int i = 0; i < 12; ++i) rb[4 + i] = mat4.m[i];
            }
            rb[23] = linVel.x; rb[24] = linVel.y; rb[25] = linVel.z;
            rb[26] = angVel.x; rb[27] = angVel.y; rb[28] = angVel.z;
            nativeOk = NativeCarX::SetCarRigidBody(car, rb);
        }
    }

    char msg[256];
    sprintf_s(msg, "Vanilla teleport: car='%s' pos=(%.2f,%.2f,%.2f) velMode=%d raceCar=%d tf=%d native=%d",
              carName.c_str(), uPos.x, uPos.y, uPos.z, velMode,
              vanillaOk ? 1 : 0, tfOk ? 1 : 0, nativeOk ? 1 : 0);
    Logger::Info(msg);

    return vanillaOk || tfOk || nativeOk;
}

bool UnityBridge::RequestTeleportPlayerPose(const TeleportPose& pose, int velMode) {
    if (!NativeCarX::IsInSession()) return false;
    if (IsOnMainThread()) {
        return ApplyTeleportPlayerInternal(pose, velMode);
    }
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_pendingTeleportPose = pose;
    s_pendingTeleportVelMode = velMode;
    s_pendingTeleport.store(true, std::memory_order_release);
    return true;
}

bool UnityBridge::RequestTeleportPlayerRigidBody(const float rb[29], int velMode) {
    if (!rb || !NativeCarX::IsInSession()) return false;
    TeleportPose pose{};
    memcpy(pose.rb, rb, sizeof(pose.rb));
    pose.hasRb = true;
    pose.hasQuat = false;
    pose.pos[0] = rb[20]; pose.pos[1] = rb[21]; pose.pos[2] = rb[22];
    pose.linVel[0] = rb[23]; pose.linVel[1] = rb[24]; pose.linVel[2] = rb[25];
    pose.angVel[0] = rb[26]; pose.angVel[1] = rb[27]; pose.angVel[2] = rb[28];
    return RequestTeleportPlayerPose(pose, velMode);
}
