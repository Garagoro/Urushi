#include "unity_bridge.h"
#include "unity_bridge_internal.h"
#include "carfx.h"
#include "ecs_collision.h"
#include "native_carx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

using namespace UnityBridgeInternal;

namespace {

static UnityVector3 ClosestPointOnOBB(const UnityVector3& pt,
                                      const UnityVector3& center,
                                      const UnityVector3& right,
                                      const UnityVector3& up,
                                      const UnityVector3& fwd,
                                      float halfW, float halfH, float halfL) {
    UnityVector3 d = { pt.x - center.x, pt.y - center.y, pt.z - center.z };
    float lx = std::clamp(VecDot(d, right), -halfW, halfW);
    float ly = std::clamp(VecDot(d, up),    -halfH, halfH);
    float lz = std::clamp(VecDot(d, fwd),   -halfL, halfL);
    return {
        center.x + right.x * lx + up.x * ly + fwd.x * lz,
        center.y + right.y * lx + up.y * ly + fwd.y * lz,
        center.z + right.z * lx + up.z * ly + fwd.z * lz
    };
}

struct VehicleOBBCandidate {
    int ownerTag = 0; // -1 = local player, 0..999 = remote/AI, 1000+slot = ghost
    UnityVector3 center{};
    UnityVector3 right{ 1.0f, 0.0f, 0.0f };
    UnityVector3 up{ 0.0f, 1.0f, 0.0f };
    UnityVector3 fwd{ 0.0f, 0.0f, 1.0f };
    float halfW = 0.96f;
    float halfH = 0.65f;
    float halfL = 2.25f;
};

struct CarSpatialTracker {
    struct SmoothZone {
        bool valid = false;
        float missAge = 0.0f;
        float localPt[3] = { 0.0f, 0.0f, 0.0f };
        float gap = 999.0f;
        UnityVector3 normal{ 0.0f, 1.0f, 0.0f };
    } zones[11]{};
    bool nearValid = false;
    float nearGap = 999.0f;
    float nearLocalCenter[3] = { 0.0f, 0.0f, 0.0f };
    float nearLocalClosest[3] = { 0.0f, 0.0f, 0.0f };
    float nearLocalRight[3] = { 1.0f, 0.0f, 0.0f };
    float nearLocalFwd[3] = { 0.0f, 0.0f, 1.0f };
    float nearExtents[3] = { 0.96f, 0.65f, 2.25f };
    float smoothDriftAbs = 0.0f;
    float smoothDriftSigned = 0.0f;
    bool hasPrevPos = false;
    UnityVector3 prevPos{};

    void ResetSmooth() {
        for (auto& z : zones) z = SmoothZone{};
        nearValid = false;
        nearGap = 999.0f;
        smoothDriftAbs = 0.0f;
        smoothDriftSigned = 0.0f;
        hasPrevPos = false;
    }
};

static void EvaluateCarSpatialTarget(
    int carfxTarget,
    int selfOwnerTag,
    const UnityVector3& pPos,
    const UnityVector3& pRight,
    const UnityVector3& pUp,
    const UnityVector3& pFwd,
    float halfW, float halfH, float halfL,
    float frontAxleZ, float rearAxleZ,
    bool hasVel, const float rawVel[3],
    bool hasCamPos, const UnityVector3& camPos,
    void* fnRaycast,
    float dt,
    const std::vector<VehicleOBBCandidate>& otherCars,
    CarSpatialTracker& tr)
{
    CarFX::SpatialProximityInput sp{};
    sp.carPos[0] = pPos.x; sp.carPos[1] = pPos.y; sp.carPos[2] = pPos.z; sp.carPos[3] = halfW;
    sp.carRight[0] = pRight.x; sp.carRight[1] = pRight.y; sp.carRight[2] = pRight.z; sp.carRight[3] = halfL;
    sp.carUp[0] = pUp.x; sp.carUp[1] = pUp.y; sp.carUp[2] = pUp.z; sp.carUp[3] = halfH * 2.0f;
    sp.carFwd[0] = pFwd.x; sp.carFwd[1] = pFwd.y; sp.carFwd[2] = pFwd.z; sp.carFwd[3] = 1.0f;
    if (hasCamPos) {
        sp.camPos[0] = camPos.x; sp.camPos[1] = camPos.y; sp.camPos[2] = camPos.z; sp.camPos[3] = 1.0f;
    }
    for (int k = 0; k < 3; ++k) {
        sp.camRight[k] = g_camAxes[0][k];
        sp.camUp[k] = g_camAxes[1][k];
        sp.camFwd[k] = g_camAxes[2][k];
    }
    sp.camFwd[3] = g_camAxesValid ? 1.0f : 0.0f;

    // 1. Volumetric perimeter proximity queries (3D SphereCasts + barrier layer + low-curb CastRays, 8 zones incl. doors)
    struct RawZoneHit {
        bool hit = false;
        float gap = 999.0f;
        UnityVector3 point{};
        UnityVector3 normal{ 0.0f, 1.0f, 0.0f };
    };
    enum ZoneId {
        kZoneFront = 0, kZoneRear, kZoneDoorL, kZoneDoorR,
        kZoneFrontL, kZoneFrontR, kZoneRearL, kZoneRearR,
        kZoneCount
    };
    RawZoneHit rawZones[kZoneCount];
    {
        const uint32_t dotsBelongs         = 0x100u;     // Layer 8: Player Car
        const uint32_t dotsCollidesStatic  = ~(0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u | 0x20000u | 0x40000u | 0x800000u | 0x1000000u);
        const uint32_t dotsCollidesBarrier = 0x00010000u; // Car-only barrier walls (zero road asphalt)
        const int physxLayerMask = static_cast<int>(~((1u << 2) | (1u << 5) | (1u << 8) | (1u << 12) | (1u << 15) | (1u << 17) | (1u << 22)));
        const float maxScanRange = 9.0f;

        const float fbStart = (std::max)(0.35f, halfL - 1.15f);
        const float lrStart = (std::max)(0.10f, halfW - 0.72f);
        const float doorCenterZ  = 0.5f * (frontAxleZ + rearAxleZ);
        const float doorHalfSpan = std::clamp(0.5f * (frontAxleZ - rearAxleZ) - 0.36f, 0.25f, 0.90f);
        const float frontSideZ   = (std::min)(halfL - 0.15f, frontAxleZ + 0.46f);
        const float rearSideZ    = (std::max)(-halfL + 0.15f, rearAxleZ - 0.46f);

        auto recordZoneCandidate = [&](int z, float gap, const UnityVector3& hitPt, const UnityVector3& hitNorm) {
            RawZoneHit& rz = rawZones[z];
            if (!rz.hit || gap < rz.gap) {
                rz.hit = true;
                rz.gap = gap;
                rz.point = hitPt;
                rz.normal = hitNorm;
            }
        };

        auto recordWallHit = [&](int hintZone, const UnityVector3& dir, const UnityVector3& hitPt, const UnityVector3& hitNorm) {
            const UnityVector3 rel = { hitPt.x - pPos.x, hitPt.y - pPos.y, hitPt.z - pPos.z };
            if (VecDot(rel, dir) <= 0.05f) return;

            const float lx = VecDot(rel, pRight);
            const float ly = VecDot(rel, pUp);
            const float lz = VecDot(rel, pFwd);
            if (ly < -0.25f || ly > 2.35f) return;

            const float ex = (std::max)(0.0f, fabsf(lx) - halfW);
            const float ez = (std::max)(0.0f, fabsf(lz) - halfL);
            const float gap = sqrtf(ex * ex + ez * ez);
            if (gap > maxScanRange) return;

            recordZoneCandidate(hintZone, gap, hitPt, hitNorm);

            if (lz >= halfL - 0.18f && fabsf(lx) <= halfW + 0.35f)
                recordZoneCandidate(kZoneFront, gap, hitPt, hitNorm);
            if (lz <= -halfL + 0.18f && fabsf(lx) <= halfW + 0.35f)
                recordZoneCandidate(kZoneRear, gap, hitPt, hitNorm);
            if (lx <= -halfW + 0.18f && lz >= rearAxleZ + 0.08f && lz <= frontAxleZ - 0.08f)
                recordZoneCandidate(kZoneDoorL, gap, hitPt, hitNorm);
            if (lx >= halfW - 0.18f && lz >= rearAxleZ + 0.08f && lz <= frontAxleZ - 0.08f)
                recordZoneCandidate(kZoneDoorR, gap, hitPt, hitNorm);
            if (lx <= -halfW * 0.45f && lz >= frontAxleZ - 0.25f)
                recordZoneCandidate(kZoneFrontL, gap, hitPt, hitNorm);
            if (lx >= halfW * 0.45f && lz >= frontAxleZ - 0.25f)
                recordZoneCandidate(kZoneFrontR, gap, hitPt, hitNorm);
            if (lx <= -halfW * 0.45f && lz <= rearAxleZ + 0.25f)
                recordZoneCandidate(kZoneRearL, gap, hitPt, hitNorm);
            if (lx >= halfW * 0.45f && lz <= rearAxleZ + 0.25f)
                recordZoneCandidate(kZoneRearR, gap, hitPt, hitNorm);
        };

        auto computeEdgeOffset = [&](float sx, float sz, float dx, float dz) -> float {
            float tx = (fabsf(dx) > 1e-4f) ? (((dx > 0.0f ? halfW : -halfW) - sx) / dx) : 999.0f;
            float tz = (fabsf(dz) > 1e-4f) ? (((dz > 0.0f ? halfL : -halfL) - sz) / dz) : 999.0f;
            return (std::max)(0.05f, (std::min)(tx, tz));
        };

        auto isGroundSlope = [&](const UnityVector3& n, float maxSlopeCos) -> bool {
            float lenSq = n.x * n.x + n.y * n.y + n.z * n.z;
            if (lenSq <= 0.25f) return false;
            return fabsf(VecDot(n, pUp)) > maxSlopeCos || fabsf(n.y) > maxSlopeCos;
        };

        auto probeWallSphere = [&](int zone, float sx, float sy, float sz,
                                   float dx, float dy, float dz, float radius,
                                   uint32_t collidesMask, float maxSlopeCos, int maxSlopeHops,
                                   bool allowRayFallback) {
            UnityVector3 dir = VecNormalize({
                pRight.x * dx + pUp.x * dy + pFwd.x * dz,
                pRight.y * dx + pUp.y * dy + pFwd.y * dz,
                pRight.z * dx + pUp.z * dy + pFwd.z * dz
            });
            const float edgeDist = computeEdgeOffset(sx, sz, dx, dz);
            const float inset = radius + 0.04f;
            const float startOffset = (std::max)(0.0f, edgeDist - inset);
            UnityVector3 curOrig = {
                pPos.x + pRight.x * sx + pUp.x * sy + pFwd.x * sz + dir.x * startOffset,
                pPos.y + pRight.y * sx + pUp.y * sy + pFwd.y * sz + dir.y * startOffset,
                pPos.z + pRight.z * sx + pUp.z * sy + pFwd.z * sz + dir.z * startOffset
            };
            float remDist = (edgeDist - startOffset) + maxScanRange;
            float curRadius = radius;
            bool sphereWorked = false;

            for (int hop = 0; hop <= maxSlopeHops && remDist > 0.25f; ++hop) {
                float origArr[3] = { curOrig.x, curOrig.y, curOrig.z };
                float dirArr[3]  = { dir.x, dir.y, dir.z };
                EcsCollision::SphereCastHit shit{};
                bool dotsExecuted = false;
                bool hit = EcsCollision::SphereCast(origArr, curRadius, dirArr, remDist, dotsBelongs, collidesMask, shit, 1, &dotsExecuted);
                sphereWorked = sphereWorked || dotsExecuted;
                if (!hit) break;

                UnityVector3 hitPt{ shit.position[0], shit.position[1], shit.position[2] };
                UnityVector3 hitNorm{ shit.surfaceNormal[0], shit.surfaceNormal[1], shit.surfaceNormal[2] };
                if (isGroundSlope(hitNorm, maxSlopeCos)) {
                    float advance = (std::max)(0.25f, shit.fraction * remDist + 0.14f);
                    if (advance >= remDist - 0.15f) break;
                    curOrig = {
                        hitPt.x + dir.x * (curRadius * 0.55f + 0.12f) + pUp.x * (curRadius + 0.20f),
                        hitPt.y + dir.y * (curRadius * 0.55f + 0.12f) + pUp.y * (curRadius + 0.20f),
                        hitPt.z + dir.z * (curRadius * 0.55f + 0.12f) + pUp.z * (curRadius + 0.20f)
                    };
                    remDist -= advance;
                    curRadius = (std::min)(curRadius, 0.22f);
                    continue;
                }
                recordWallHit(zone, dir, hitPt, hitNorm);
                break;
            }

            if (!sphereWorked && allowRayFallback) {
                const float rayLen = edgeDist + maxScanRange;
                UnityVector3 rayOrig = {
                    pPos.x + pRight.x * sx + pUp.x * sy + pFwd.x * sz,
                    pPos.y + pRight.y * sx + pUp.y * sy + pFwd.y * sz,
                    pPos.z + pRight.z * sx + pUp.z * sy + pFwd.z * sz
                };
                float start[3] = { rayOrig.x, rayOrig.y, rayOrig.z };
                float end[3]   = { rayOrig.x + dir.x * rayLen, rayOrig.y + dir.y * rayLen, rayOrig.z + dir.z * rayLen };
                DotsRaycastHit dhit{};
                if (EcsCollision::CastRay(start, end, dotsBelongs, collidesMask, dhit)) {
                    UnityVector3 hitNorm{ dhit.normal[0], dhit.normal[1], dhit.normal[2] };
                    if (!isGroundSlope(hitNorm, maxSlopeCos)) {
                        recordWallHit(zone, dir, { dhit.point[0], dhit.point[1], dhit.point[2] }, hitNorm);
                    }
                } else if (fnRaycast) {
                    UnityRay ray{ rayOrig, dir };
                    UnityRaycastHit hit{};
                    if (SafePhysicsRaycastSEH(fnRaycast, &ray, rayLen, &hit, physxLayerMask)) {
                        if (!isGroundSlope(hit.normal, maxSlopeCos)) {
                            recordWallHit(zone, dir, hit.point, hit.normal);
                        }
                    }
                }
            }
        };

        auto probeLowCurbRay = [&](int zone, float sx, float sy, float sz, float dx, float dy, float dz) {
            UnityVector3 dir = VecNormalize({
                pRight.x * dx + pUp.x * dy + pFwd.x * dz,
                pRight.y * dx + pUp.y * dy + pFwd.y * dz,
                pRight.z * dx + pUp.z * dy + pFwd.z * dz
            });
            const float rayLen = computeEdgeOffset(sx, sz, dx, dz) + maxScanRange;
            UnityVector3 rayOrig = {
                pPos.x + pRight.x * sx + pUp.x * sy + pFwd.x * sz,
                pPos.y + pRight.y * sx + pUp.y * sy + pFwd.y * sz,
                pPos.z + pRight.z * sx + pUp.z * sy + pFwd.z * sz
            };
            float start[3] = { rayOrig.x, rayOrig.y, rayOrig.z };
            float end[3]   = { rayOrig.x + dir.x * rayLen, rayOrig.y + dir.y * rayLen, rayOrig.z + dir.z * rayLen };
            DotsRaycastHit dhit{};
            if (EcsCollision::CastRay(start, end, dotsBelongs, dotsCollidesStatic, dhit)) {
                UnityVector3 hitNorm{ dhit.normal[0], dhit.normal[1], dhit.normal[2] };
                if (!isGroundSlope(hitNorm, 0.62f)) {
                    recordWallHit(zone, dir, { dhit.point[0], dhit.point[1], dhit.point[2] }, hitNorm);
                }
            }
        };

        struct ProbeDef { int zone; float sx, sy, sz; float dx, dy, dz; };
        const ProbeDef mainProbes[] = {
            { kZoneFront,  -0.62f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneFront,  -0.22f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneFront,   0.22f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneFront,   0.62f, 0.62f,  fbStart,   0.00f, 0.0f,  1.00f },
            { kZoneRear,   -0.62f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneRear,   -0.22f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneRear,    0.22f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneRear,    0.62f, 0.62f, -fbStart,   0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ - doorHalfSpan, -1.00f, 0.0f,  0.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ,                -1.00f, 0.0f,  0.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ + doorHalfSpan, -1.00f, 0.0f,  0.00f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ,                -0.94f, 0.0f,  0.34f },
            { kZoneDoorL,  -lrStart, 0.62f, doorCenterZ,                -0.94f, 0.0f, -0.34f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ - doorHalfSpan,  1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ,                 1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ + doorHalfSpan,  1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ,                 0.94f, 0.0f,  0.34f },
            { kZoneDoorR,   lrStart, 0.62f, doorCenterZ,                 0.94f, 0.0f, -0.34f },
            { kZoneFrontL, -lrStart, 0.62f, frontAxleZ,                 -1.00f, 0.0f,  0.00f },
            { kZoneFrontL, -lrStart, 0.62f, frontSideZ,                 -0.80f, 0.0f,  0.60f },
            { kZoneFrontL, -0.48f,   0.62f, fbStart,                    -0.44f, 0.0f,  0.90f },
            { kZoneFrontR,  lrStart, 0.62f, frontAxleZ,                  1.00f, 0.0f,  0.00f },
            { kZoneFrontR,  lrStart, 0.62f, frontSideZ,                  0.80f, 0.0f,  0.60f },
            { kZoneFrontR,  0.48f,   0.62f, fbStart,                     0.44f, 0.0f,  0.90f },
            { kZoneRearL,  -lrStart, 0.62f, rearAxleZ,                  -1.00f, 0.0f,  0.00f },
            { kZoneRearL,  -lrStart, 0.62f, rearSideZ,                  -0.80f, 0.0f, -0.60f },
            { kZoneRearL,  -0.48f,   0.62f,-fbStart,                    -0.44f, 0.0f, -0.90f },
            { kZoneRearR,   lrStart, 0.62f, rearAxleZ,                   1.00f, 0.0f,  0.00f },
            { kZoneRearR,   lrStart, 0.62f, rearSideZ,                   0.80f, 0.0f, -0.60f },
            { kZoneRearR,   0.48f,   0.62f,-fbStart,                     0.44f, 0.0f, -0.90f },
        };
        for (const auto& pd : mainProbes) {
            probeWallSphere(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz,
                            0.27f, dotsCollidesStatic, 0.72f, 2, true);
        }

        const ProbeDef barrierProbes[] = {
            { kZoneFront,   0.00f,   0.64f,  fbStart,                           0.00f, 0.0f,  1.00f },
            { kZoneRear,    0.00f,   0.64f, -fbStart,                           0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.64f, doorCenterZ - doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorL,  -lrStart, 0.64f, doorCenterZ + doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.64f, doorCenterZ - doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.64f, doorCenterZ + doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneFrontL, -0.45f,   0.64f, frontSideZ,                        -0.71f, 0.0f,  0.71f },
            { kZoneFrontR,  0.45f,   0.64f, frontSideZ,                         0.71f, 0.0f,  0.71f },
            { kZoneRearL,  -0.45f,   0.64f, rearSideZ,                         -0.71f, 0.0f, -0.71f },
            { kZoneRearR,   0.45f,   0.64f, rearSideZ,                          0.71f, 0.0f, -0.71f },
        };
        for (const auto& pd : barrierProbes) {
            probeWallSphere(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz,
                            0.38f, dotsCollidesBarrier, 0.88f, 0, false);
        }

        const ProbeDef highProbes[] = {
            { kZoneFront,   0.00f,   0.98f,  fbStart,    0.00f, 0.0f,  1.00f },
            { kZoneRear,    0.00f,   0.98f, -fbStart,    0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.98f, doorCenterZ,-1.00f, 0.0f,  0.00f },
            { kZoneDoorR,   lrStart, 0.98f, doorCenterZ, 1.00f, 0.0f,  0.00f },
            { kZoneFrontL, -0.45f,   0.98f, frontSideZ, -0.71f, 0.0f,  0.71f },
            { kZoneFrontR,  0.45f,   0.98f, frontSideZ,  0.71f, 0.0f,  0.71f },
            { kZoneRearL,  -0.45f,   0.98f, rearSideZ,  -0.71f, 0.0f, -0.71f },
            { kZoneRearR,   0.45f,   0.98f, rearSideZ,   0.71f, 0.0f, -0.71f },
        };
        for (const auto& pd : highProbes) {
            probeWallSphere(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz,
                            0.25f, dotsCollidesStatic, 0.72f, 1, false);
        }

        const ProbeDef curbRays[] = {
            { kZoneFront,  -0.45f,   0.24f,  fbStart,                           0.00f, 0.0f,  1.00f },
            { kZoneFront,   0.45f,   0.24f,  fbStart,                           0.00f, 0.0f,  1.00f },
            { kZoneRear,   -0.45f,   0.24f, -fbStart,                           0.00f, 0.0f, -1.00f },
            { kZoneRear,    0.45f,   0.24f, -fbStart,                           0.00f, 0.0f, -1.00f },
            { kZoneDoorL,  -lrStart, 0.24f, doorCenterZ - doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorL,  -lrStart, 0.24f, doorCenterZ + doorHalfSpan * 0.5f, -1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.24f, doorCenterZ - doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneDoorR,   lrStart, 0.24f, doorCenterZ + doorHalfSpan * 0.5f,  1.0f,  0.0f,  0.0f },
            { kZoneFrontL, -0.45f,   0.24f, frontSideZ,                        -0.71f, 0.0f,  0.71f },
            { kZoneFrontR,  0.45f,   0.24f, frontSideZ,                         0.71f, 0.0f,  0.71f },
            { kZoneRearL,  -0.45f,   0.24f, rearSideZ,                         -0.71f, 0.0f, -0.71f },
            { kZoneRearR,   0.45f,   0.24f, rearSideZ,                          0.71f, 0.0f, -0.71f },
        };
        for (const auto& pd : curbRays) {
            probeLowCurbRay(pd.zone, pd.sx, pd.sy, pd.sz, pd.dx, pd.dy, pd.dz);
        }
    }

    enum SmoothSlot {
        kSmFront = 0, kSmRear, kSmDoorL, kSmDoorR,
        kSmFrontL, kSmFrontR, kSmRearL, kSmRearR,
        kSmSideL, kSmSideR, kSmNearest, kSmTotal
    };
    float flowSpeed = 6.0f;
    CarFX::GetParamByName(carfxTarget, "flowSpeed", &flowSpeed, 1);
    flowSpeed = std::clamp(flowSpeed, 0.3f, 60.0f);
    const float fadeCeiling = 10.5f;
    const float alphaGap = (flowSpeed >= 28.0f) ? 1.0f : (1.0f - expf(-flowSpeed * dt));
    const float alphaPos = (flowSpeed >= 28.0f) ? 1.0f : (1.0f - expf(-(flowSpeed * 1.45f) * dt));

    auto updateSmoothZone = [&](int slotIdx, const RawZoneHit& raw, float outVec[4]) {
        CarSpatialTracker::SmoothZone& zs = tr.zones[slotIdx];
        if (raw.hit && raw.gap < fadeCeiling) {
            zs.missAge = 0.0f;
            UnityVector3 rel = { raw.point.x - pPos.x, raw.point.y - pPos.y, raw.point.z - pPos.z };
            float lx = VecDot(rel, pRight);
            float ly = VecDot(rel, pUp);
            float lz = VecDot(rel, pFwd);
            if (!zs.valid || zs.gap >= fadeCeiling - 0.1f) {
                zs.valid = true;
                zs.localPt[0] = lx; zs.localPt[1] = ly; zs.localPt[2] = lz;
                const float entryStart = (std::min)(fadeCeiling, (std::max)(raw.gap + 2.8f, 3.8f));
                zs.gap = (alphaGap >= 0.999f) ? raw.gap : (entryStart + (raw.gap - entryStart) * alphaGap);
                zs.normal = raw.normal;
            } else {
                zs.localPt[0] += (lx - zs.localPt[0]) * alphaPos;
                zs.localPt[1] += (ly - zs.localPt[1]) * alphaPos;
                zs.localPt[2] += (lz - zs.localPt[2]) * alphaPos;
                zs.gap        += (raw.gap - zs.gap) * alphaGap;
                zs.normal.x   += (raw.normal.x - zs.normal.x) * alphaPos;
                zs.normal.y   += (raw.normal.y - zs.normal.y) * alphaPos;
                zs.normal.z   += (raw.normal.z - zs.normal.z) * alphaPos;
            }
        } else if (zs.valid) {
            zs.missAge += dt;
            if (zs.missAge > 0.09f || alphaGap >= 0.999f) {
                zs.gap += (fadeCeiling - zs.gap) * alphaGap;
                if (zs.gap >= fadeCeiling - 0.05f || alphaGap >= 0.999f) {
                    zs.valid = false;
                    zs.missAge = 0.0f;
                    zs.gap = 999.0f;
                }
            }
        }

        if (zs.valid) {
            outVec[0] = pPos.x + pRight.x * zs.localPt[0] + pUp.x * zs.localPt[1] + pFwd.x * zs.localPt[2];
            outVec[1] = pPos.y + pRight.y * zs.localPt[0] + pUp.y * zs.localPt[1] + pFwd.y * zs.localPt[2];
            outVec[2] = pPos.z + pRight.z * zs.localPt[0] + pUp.z * zs.localPt[1] + pFwd.z * zs.localPt[2];
            outVec[3] = zs.gap;
        } else {
            outVec[0] = outVec[1] = outVec[2] = 0.0f;
            outVec[3] = 999.0f;
        }
    };

    RawZoneHit rawSideL, rawSideR, rawNearest;
    for (int z : { kZoneFrontL, kZoneDoorL, kZoneRearL }) {
        if (rawZones[z].hit && (!rawSideL.hit || rawZones[z].gap < rawSideL.gap)) rawSideL = rawZones[z];
    }
    for (int z : { kZoneFrontR, kZoneDoorR, kZoneRearR }) {
        if (rawZones[z].hit && (!rawSideR.hit || rawZones[z].gap < rawSideR.gap)) rawSideR = rawZones[z];
    }
    for (int z = 0; z < kZoneCount; ++z) {
        if (rawZones[z].hit && (!rawNearest.hit || rawZones[z].gap < rawNearest.gap)) rawNearest = rawZones[z];
    }

    updateSmoothZone(kSmFront,   rawZones[kZoneFront],  sp.wallFrontHit);
    updateSmoothZone(kSmRear,    rawZones[kZoneRear],   sp.wallRearHit);
    updateSmoothZone(kSmDoorL,   rawZones[kZoneDoorL],  sp.wallDoorLeftHit);
    updateSmoothZone(kSmDoorR,   rawZones[kZoneDoorR],  sp.wallDoorRightHit);
    updateSmoothZone(kSmFrontL,  rawZones[kZoneFrontL], sp.wallFrontLeftHit);
    updateSmoothZone(kSmFrontR,  rawZones[kZoneFrontR], sp.wallFrontRightHit);
    updateSmoothZone(kSmRearL,   rawZones[kZoneRearL],  sp.wallRearLeftHit);
    updateSmoothZone(kSmRearR,   rawZones[kZoneRearR],  sp.wallRearRightHit);
    updateSmoothZone(kSmSideL,   rawSideL,              sp.wallLeftHit);
    updateSmoothZone(kSmSideR,   rawSideR,              sp.wallRightHit);
    updateSmoothZone(kSmNearest, rawNearest,            sp.wallNearestHit);

    sp.wallDirDist[0] = sp.wallFrontHit[3];
    sp.wallDirDist[1] = sp.wallRearHit[3];
    sp.wallDirDist[2] = sp.wallLeftHit[3];
    sp.wallDirDist[3] = sp.wallRightHit[3];

    if (tr.zones[kSmNearest].valid) {
        UnityVector3 n = VecNormalize(tr.zones[kSmNearest].normal);
        sp.wallNearestNormal[0] = n.x;
        sp.wallNearestNormal[1] = n.y;
        sp.wallNearestNormal[2] = n.z;
    }
    int activeMask = 0;
    if (sp.wallFrontHit[3] < fadeCeiling) activeMask |= 1;
    if (sp.wallRearHit[3]  < fadeCeiling) activeMask |= 2;
    if (sp.wallLeftHit[3]  < fadeCeiling) activeMask |= 4;
    if (sp.wallRightHit[3] < fadeCeiling) activeMask |= 8;
    sp.wallNearestNormal[3] = static_cast<float>(activeMask);

    // 2. Nearest other vehicle (OBB surface gap against all candidates excluding self)
    const UnityVector3 pCenter = {
        pPos.x + pUp.x * halfH,
        pPos.y + pUp.y * halfH,
        pPos.z + pUp.z * halfH
    };
    float bestCarGap = 999.0f;
    UnityVector3 bestOCenter{}, bestORight{ 1,0,0 }, bestOFwd{ 0,0,1 }, bestCClosest{};
    float bestOHw = 0.96f, bestOHh = 0.65f, bestOHl = 2.25f;

    for (const auto& cand : otherCars) {
        if (cand.ownerTag == selfOwnerTag) continue;
        float dx = cand.center.x - pCenter.x;
        float dy = cand.center.y - pCenter.y;
        float dz = cand.center.z - pCenter.z;
        if (dx * dx + dy * dy + dz * dz > 1600.0f) continue;

        UnityVector3 cOther = ClosestPointOnOBB(pCenter, cand.center, cand.right, cand.up, cand.fwd, cand.halfW, cand.halfH, cand.halfL);
        UnityVector3 cSelf = ClosestPointOnOBB(cOther, pCenter, pRight, pUp, pFwd, halfW, halfH, halfL);
        cOther = ClosestPointOnOBB(cSelf, cand.center, cand.right, cand.up, cand.fwd, cand.halfW, cand.halfH, cand.halfL);

        float gx = cOther.x - cSelf.x;
        float gy = cOther.y - cSelf.y;
        float gz = cOther.z - cSelf.z;
        float gap = sqrtf(gx * gx + gy * gy + gz * gz);

        if (gap < bestCarGap) {
            bestCarGap = gap;
            bestOCenter = cand.center; bestORight = cand.right; bestOFwd = cand.fwd; bestCClosest = cOther;
            bestOHw = cand.halfW; bestOHh = cand.halfH; bestOHl = cand.halfL;
        }
    }

    auto toLocalVec = [&](const UnityVector3& v, float out3[3]) {
        out3[0] = VecDot(v, pRight); out3[1] = VecDot(v, pUp); out3[2] = VecDot(v, pFwd);
    };
    auto toWorldVec = [&](const float in3[3]) -> UnityVector3 {
        return {
            pRight.x * in3[0] + pUp.x * in3[1] + pFwd.x * in3[2],
            pRight.y * in3[0] + pUp.y * in3[1] + pFwd.y * in3[2],
            pRight.z * in3[0] + pUp.z * in3[1] + pFwd.z * in3[2]
        };
    };

    if (bestCarGap < fadeCeiling) {
        UnityVector3 dc = { bestOCenter.x - pPos.x, bestOCenter.y - pPos.y, bestOCenter.z - pPos.z };
        UnityVector3 dl = { bestCClosest.x - pPos.x, bestCClosest.y - pPos.y, bestCClosest.z - pPos.z };
        toLocalVec(dc, tr.nearLocalCenter);
        toLocalVec(dl, tr.nearLocalClosest);
        toLocalVec(bestORight, tr.nearLocalRight);
        toLocalVec(bestOFwd,   tr.nearLocalFwd);
        tr.nearExtents[0] = bestOHw; tr.nearExtents[1] = bestOHh; tr.nearExtents[2] = bestOHl;

        if (!tr.nearValid || tr.nearGap >= fadeCeiling - 0.1f) {
            tr.nearValid = true;
            const float entryStart = (std::min)(fadeCeiling, (std::max)(bestCarGap + 2.8f, 3.8f));
            tr.nearGap = (alphaGap >= 0.999f) ? bestCarGap : (entryStart + (bestCarGap - entryStart) * alphaGap);
        } else {
            tr.nearGap += (bestCarGap - tr.nearGap) * alphaGap;
        }
        sp.nearCarPos[0] = bestOCenter.x; sp.nearCarPos[1] = bestOCenter.y; sp.nearCarPos[2] = bestOCenter.z;
        sp.nearCarPos[3] = tr.nearGap;
        sp.nearCarRight[0] = bestORight.x; sp.nearCarRight[1] = bestORight.y; sp.nearCarRight[2] = bestORight.z;
        sp.nearCarRight[3] = bestOHw;
        sp.nearCarFwd[0] = bestOFwd.x; sp.nearCarFwd[1] = bestOFwd.y; sp.nearCarFwd[2] = bestOFwd.z;
        sp.nearCarFwd[3] = bestOHl;
        sp.nearCarClosest[0] = bestCClosest.x; sp.nearCarClosest[1] = bestCClosest.y; sp.nearCarClosest[2] = bestCClosest.z;
        sp.nearCarClosest[3] = bestOHh;
    } else if (bestCarGap < 900.0f) {
        tr.nearValid = false;
        tr.nearGap = bestCarGap;
        sp.nearCarPos[0] = bestOCenter.x; sp.nearCarPos[1] = bestOCenter.y; sp.nearCarPos[2] = bestOCenter.z;
        sp.nearCarPos[3] = bestCarGap;
        sp.nearCarRight[0] = bestORight.x; sp.nearCarRight[1] = bestORight.y; sp.nearCarRight[2] = bestORight.z;
        sp.nearCarRight[3] = bestOHw;
        sp.nearCarFwd[0] = bestOFwd.x; sp.nearCarFwd[1] = bestOFwd.y; sp.nearCarFwd[2] = bestOFwd.z;
        sp.nearCarFwd[3] = bestOHl;
        sp.nearCarClosest[0] = bestCClosest.x; sp.nearCarClosest[1] = bestCClosest.y; sp.nearCarClosest[2] = bestCClosest.z;
        sp.nearCarClosest[3] = bestOHh;
    } else if (tr.nearValid) {
        tr.nearGap += (fadeCeiling - tr.nearGap) * alphaGap;
        if (tr.nearGap >= fadeCeiling - 0.05f || alphaGap >= 0.999f) {
            tr.nearValid = false;
            tr.nearGap = 999.0f;
        } else {
            UnityVector3 wC = toWorldVec(tr.nearLocalCenter);
            UnityVector3 wL = toWorldVec(tr.nearLocalClosest);
            UnityVector3 wR = VecNormalize(toWorldVec(tr.nearLocalRight));
            UnityVector3 wF = VecNormalize(toWorldVec(tr.nearLocalFwd));
            sp.nearCarPos[0] = pPos.x + wC.x; sp.nearCarPos[1] = pPos.y + wC.y; sp.nearCarPos[2] = pPos.z + wC.z;
            sp.nearCarPos[3] = tr.nearGap;
            sp.nearCarRight[0] = wR.x; sp.nearCarRight[1] = wR.y; sp.nearCarRight[2] = wR.z;
            sp.nearCarRight[3] = tr.nearExtents[0];
            sp.nearCarFwd[0] = wF.x; sp.nearCarFwd[1] = wF.y; sp.nearCarFwd[2] = wF.z;
            sp.nearCarFwd[3] = tr.nearExtents[2];
            sp.nearCarClosest[0] = pPos.x + wL.x; sp.nearCarClosest[1] = pPos.y + wL.y; sp.nearCarClosest[2] = pPos.z + wL.z;
            sp.nearCarClosest[3] = tr.nearExtents[1];
        }
    }

    // 3. Real vehicle slip/drift angle in degrees (from world velocity vs car forward axis)
    float vel[3] = { rawVel[0], rawVel[1], rawVel[2] };
    bool velOk = hasVel;
    if (!velOk && tr.hasPrevPos && dt > 1e-4f) {
        vel[0] = (pPos.x - tr.prevPos.x) / dt;
        vel[1] = (pPos.y - tr.prevPos.y) / dt;
        vel[2] = (pPos.z - tr.prevPos.z) / dt;
        velOk = true;
    }
    tr.prevPos = pPos;
    tr.hasPrevPos = true;

    float vFwd = vel[0] * pFwd.x + vel[1] * pFwd.y + vel[2] * pFwd.z;
    float vSide = vel[0] * pRight.x + vel[1] * pRight.y + vel[2] * pRight.z;
    float vPlanar = velOk ? sqrtf(vFwd * vFwd + vSide * vSide) : 0.0f;

    auto smoothStepF = [](float edge0, float edge1, float x) {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    float speedGate = smoothStepF(1.2f, 3.8f, vPlanar);
    float fwdCos = (vPlanar > 0.1f) ? (vFwd / vPlanar) : 1.0f;
    float revGate = smoothStepF(-0.78f, -0.30f, fwdCos);

    float rawSignedDeg = (vPlanar > 0.1f) ? (atan2f(vSide, vFwd) * 57.2957795f) : 0.0f;
    float targetSigned = rawSignedDeg * speedGate * revGate;
    float targetAbs = fabsf(targetSigned);

    const float angleRate = (std::max)(flowSpeed * 1.6f, 6.0f);
    const float alphaAngle = (flowSpeed >= 28.0f) ? 1.0f : (1.0f - expf(-angleRate * dt));
    tr.smoothDriftSigned += (targetSigned - tr.smoothDriftSigned) * alphaAngle;
    tr.smoothDriftAbs    += (targetAbs - tr.smoothDriftAbs) * alphaAngle;

    sp.driftState[0] = tr.smoothDriftAbs;
    sp.driftState[1] = tr.smoothDriftSigned;
    sp.driftState[2] = std::clamp(tr.smoothDriftAbs / 65.0f, 0.0f, 1.0f);
    sp.driftState[3] = vPlanar * 3.6f;

    CarFX::SetSpatialProximity(sp, carfxTarget);
}



} // namespace

void UnityBridge::UpdateCarFXSpatialProximityOnMainThread(const std::vector<LiveCarInfo>& liveSnapshot) {
    static CarSpatialTracker s_trackers[CarFX::kTargetCount]{};
    static std::chrono::steady_clock::time_point s_prevSpatTime{};

    auto nowTp = std::chrono::steady_clock::now();
    float dt = 0.016f;
    if (s_prevSpatTime.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(nowTp - s_prevSpatTime).count();
    }
    s_prevSpatTime = nowTp;
    if (dt > 0.40f) {
        for (auto& tr : s_trackers) tr.ResetSmooth();
    }
    dt = std::clamp(dt, 0.0005f, 0.08f);

    UnityVector3 camPos{};
    UnityVector4 camRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasCam = SafeGetMainCameraPoseSEH(reinterpret_cast<void*>(pfn_GetMainCameraNative),
                                           reinterpret_cast<void*>(pfn_Transform_get_position),
                                           reinterpret_cast<void*>(pfn_Transform_get_rotation),
                                           &camPos, &camRot);
    g_camAxesValid = hasCam && pfn_Transform_get_rotation != nullptr;
    if (g_camAxesValid) {
        UnityVector3 cr{}, cu{}, cf{};
        UnityBridge::QuatToAxes(camRot, cr, cu, cf);
        g_camAxes[0][0] = cr.x; g_camAxes[0][1] = cr.y; g_camAxes[0][2] = cr.z;
        g_camAxes[1][0] = cu.x; g_camAxes[1][1] = cu.y; g_camAxes[1][2] = cu.z;
        g_camAxes[2][0] = cf.x; g_camAxes[2][1] = cf.y; g_camAxes[2][2] = cf.z;
    }

    void* localRootTf = nullptr;
    UnityVector3 pPos{};
    UnityVector4 pRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasLocal = false;

    for (const auto& c : liveSnapshot) {
        if (c.isLocalPlayer) {
            localRootTf = c.rootTf;
            pPos = { c.x, c.y, c.z };
            pRot = { c.qx, c.qy, c.qz, c.qw };
            hasLocal = c.hasPos || c.hasRot;
            break;
        }
    }
    if (localRootTf && GetTransformPosRotDirect(localRootTf, pPos, pRot)) {
        if (!std::isnan(pPos.x) && !std::isnan(pPos.y) && !std::isnan(pPos.z)) {
            hasLocal = true;
        }
    }

    CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
    if (!hasLocal && player.carPtr) {
        pPos = { player.posX, player.posY, player.posZ };
        if (!std::isnan(pPos.x) && !std::isnan(pPos.y) && !std::isnan(pPos.z) &&
            (fabsf(pPos.x) + fabsf(pPos.y) + fabsf(pPos.z) > 0.1f)) {
            hasLocal = true;
        }
    }

    UnityVector3 pRight{ 1, 0, 0 }, pUp{ 0, 1, 0 }, pFwd{ 0, 0, 1 };
    float halfW = 0.96f, halfL = 2.20f, halfH = 0.65f;
    float frontAxleZ = halfL * 0.62f;
    float rearAxleZ  = -halfL * 0.62f;
    if (hasLocal) {
        if (localRootTf && (fabsf(pRot.x) + fabsf(pRot.y) + fabsf(pRot.z) + fabsf(pRot.w) > 0.01f)) {
            QuatToAxes(pRot, pRight, pUp, pFwd);
        } else if (player.carPtr) {
            const float* pm = player.transformMatrix.data();
            pRight = VecNormalize({ pm[0], pm[1], pm[2] });
            pUp    = VecNormalize({ pm[4], pm[5], pm[6] });
            pFwd   = VecNormalize({ pm[8], pm[9], pm[10] });
        }
        player = NativeCarX::GetPlayerCarTelemetry();
        if (player.carPtr) {
            float wheelMats[4][16];
            for (int w = 0; w < 4; ++w) {
                memcpy(wheelMats[w], player.wheels[w].matrix.data(), sizeof(float) * 16);
            }
            EstimateCarExtents(player.transformMatrix.data(), wheelMats, halfW, halfL);
            halfW = std::clamp(halfW, 0.75f, 1.35f);
            halfL = std::clamp(halfL, 1.65f, 2.75f);

            float wMag = fabsf(wheelMats[0][12]) + fabsf(wheelMats[1][12]) + fabsf(wheelMats[2][12]) + fabsf(wheelMats[3][12]);
            if (wMag > 0.01f) {
                float wz[4] = {};
                for (int w = 0; w < 4; ++w) {
                    UnityVector3 dw = { wheelMats[w][12] - pPos.x, wheelMats[w][13] - pPos.y, wheelMats[w][14] - pPos.z };
                    wz[w] = VecDot(dw, pFwd);
                }
                std::sort(wz, wz + 4);
                float rZ = 0.5f * (wz[0] + wz[1]);
                float fZ = 0.5f * (wz[2] + wz[3]);
                if (fZ - rZ > 1.35f && fZ - rZ < 4.40f) {
                    frontAxleZ = fZ;
                    rearAxleZ  = rZ;
                }
            }
        }
    }

    // Build shared OBB candidate table: player (-1) + remote cars (0..) + active ghosts (1000+slot)
    std::vector<VehicleOBBCandidate> allCars;
    allCars.reserve(liveSnapshot.size() + 8);
    if (hasLocal) {
        VehicleOBBCandidate selfCand;
        selfCand.ownerTag = -1;
        selfCand.center = { pPos.x + pUp.x * halfH, pPos.y + pUp.y * halfH, pPos.z + pUp.z * halfH };
        selfCand.right = pRight; selfCand.up = pUp; selfCand.fwd = pFwd;
        selfCand.halfW = halfW; selfCand.halfH = halfH; selfCand.halfL = halfL;
        allCars.push_back(selfCand);
    }
    int remoteIdx = 0;
    for (const auto& c : liveSnapshot) {
        if (c.isLocalPlayer) continue;
        UnityVector3 oPos{ c.x, c.y, c.z };
        UnityVector4 oRot{ c.qx, c.qy, c.qz, c.qw };
        if (c.rootTf) {
            GetTransformPosRotDirect(c.rootTf, oPos, oRot);
        } else if (!c.hasPos && !c.hasRot) {
            continue;
        }
        UnityVector3 oR, oU, oF;
        QuatToAxes(oRot, oR, oU, oF);
        VehicleOBBCandidate cand;
        cand.ownerTag = remoteIdx++;
        cand.center = { oPos.x + oU.x * 0.65f, oPos.y + oU.y * 0.65f, oPos.z + oU.z * 0.65f };
        cand.right = oR; cand.up = oU; cand.fwd = oF;
        cand.halfW = 0.96f; cand.halfH = 0.65f; cand.halfL = 2.25f;
        allCars.push_back(cand);
    }

    struct GhostSensorQuery {
        int slotIdx = 0;
        int carfxTarget = CarFX::kTargetNone;
        UnityVector3 pos{}, right{}, up{}, fwd{};
        float halfW = 0.96f, halfH = 0.65f, halfL = 2.25f;
        bool hasVel = false;
        float vel[3] = { 0, 0, 0 };
    };
    std::vector<GhostSensorQuery> activeGhostQueries;
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (size_t idx = 0; idx < s_ghostSlots.size(); ++idx) {
            const GhostSlot& slot = s_ghostSlots[idx];
            if (!slot.gameObject || !slot.hasPose) continue;

            UnityVector3 gR = VecNormalize({ slot.lastMatrix[0], slot.lastMatrix[1], slot.lastMatrix[2] });
            UnityVector3 gU = VecNormalize({ slot.lastMatrix[4], slot.lastMatrix[5], slot.lastMatrix[6] });
            UnityVector3 gF = VecNormalize({ slot.lastMatrix[8], slot.lastMatrix[9], slot.lastMatrix[10] });

            VehicleOBBCandidate gCand;
            gCand.ownerTag = 1000 + static_cast<int>(idx);
            gCand.center = { slot.pos.x + gU.x * slot.halfHeight, slot.pos.y + gU.y * slot.halfHeight, slot.pos.z + gU.z * slot.halfHeight };
            gCand.right = gR; gCand.up = gU; gCand.fwd = gF;
            gCand.halfW = slot.halfWidth; gCand.halfH = slot.halfHeight; gCand.halfL = slot.halfLength;
            allCars.push_back(gCand);

            const int gTarget = CarFX::GhostTargetFromSlot(static_cast<int>(idx));
            if (gTarget != CarFX::kTargetNone && CarFX::IsTargetActive(gTarget)) {
                GhostSensorQuery q;
                q.slotIdx = static_cast<int>(idx);
                q.carfxTarget = gTarget;
                q.pos = slot.pos; q.right = gR; q.up = gU; q.fwd = gF;
                q.halfW = slot.halfWidth; q.halfH = slot.halfHeight; q.halfL = slot.halfLength;
                if (slot.replayTeleValid) {
                    q.vel[0] = slot.replayWorldVel.x;
                    q.vel[1] = slot.replayWorldVel.y;
                    q.vel[2] = slot.replayWorldVel.z;
                    q.hasVel = true;
                } else {
                    q.vel[0] = slot.vel.x; q.vel[1] = slot.vel.y; q.vel[2] = slot.vel.z;
                    q.hasVel = true;
                }
                activeGhostQueries.push_back(q);
            }
        }
    }

    if (hasLocal) {
        float pVel[3] = { 0.0f, 0.0f, 0.0f };
        bool hasPVel = player.carPtr && NativeCarX::GetCarVelocity(player.carPtr, pVel);
        EvaluateCarSpatialTarget(
            CarFX::kTargetBody, -1,
            pPos, pRight, pUp, pFwd, halfW, halfH, halfL, frontAxleZ, rearAxleZ,
            hasPVel, pVel, hasCam, camPos, reinterpret_cast<void*>(pfn_InternalRaycast),
            dt, allCars, s_trackers[CarFX::kTargetBody]);
    } else {
        CarFX::SetSpatialProximity(CarFX::SpatialProximityInput{}, CarFX::kTargetBody);
    }

    for (const auto& gq : activeGhostQueries) {
        const float gFrontAxleZ = gq.halfL * 0.62f;
        const float gRearAxleZ  = -gq.halfL * 0.62f;
        EvaluateCarSpatialTarget(
            gq.carfxTarget, 1000 + gq.slotIdx,
            gq.pos, gq.right, gq.up, gq.fwd, gq.halfW, gq.halfH, gq.halfL, gFrontAxleZ, gRearAxleZ,
            gq.hasVel, gq.vel, hasCam, camPos, reinterpret_cast<void*>(pfn_InternalRaycast),
            dt, allCars, s_trackers[gq.carfxTarget]);
    }
}
