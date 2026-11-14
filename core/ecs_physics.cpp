#include "ecs_collision.h"
#include "ecs_collision_internal.h"
#include "pattern_scanner.h"
#include "game_build.h"
#include "logger.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace EcsCollisionInternal;

namespace EcsCollisionInternal {

namespace {
constexpr BranchRva kFbEcsClass             = { 0x8428C98, 0x9770E30 };
constexpr BranchRva kFbChunkMetaClass       = { 0x843CB48, 0x982E7E0 };
constexpr BranchRva kFbTypeNamesSS          = { 0x83F57D8, 0x97DCAC8 };
constexpr BranchRva kFbCollisionWorldCastRay= { 0x2717C40, 0x2F28300 };
constexpr BranchRva kFbSphereCast           = { 0x5B57270, 0x6B97C00 };
constexpr BranchRva kFbCastRayMiPtr         = { 0x8387960, 0x9849F98 };
constexpr BranchRva kFbInitRuntimeMetadata  = { 0x09264A0, 0x0A76750 };
} // namespace

uintptr_t g_rvaEcsClass                  = 0;
uintptr_t g_rvaChunkMetaClass            = 0;
uintptr_t g_rvaTypeNamesSS               = 0;
uintptr_t g_rvaCollisionWorldCastRay     = 0;
uintptr_t g_rvaCollisionWorldSphereCast  = 0;
uintptr_t g_rvaCastRayMiPtr              = 0;
uintptr_t g_rvaInitRuntimeMetadata       = 0;

std::recursive_mutex g_mutex;
uintptr_t g_gaBase = 0;
uintptr_t g_gaSize = 0;
EcsTypeIndices g_types{};
std::vector<uint32_t> g_cachedCarChunks;
uint32_t g_cachedPhysicsWorldChunk = UINT32_MAX;
std::chrono::steady_clock::time_point g_lastChunkScan{};
size_t g_lastSeenCarCount = 0;

bool ResolveGameAssembly() {
    if (g_gaBase) return true;
    HMODULE hMod = GetModuleHandleW(L"GameAssembly.dll");
    if (!hMod) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) return false;
    g_gaBase = reinterpret_cast<uintptr_t>(hMod);
    g_gaSize = mi.SizeOfImage;

    // Branch fallbacks first (0 on unknown builds); signature hits below override them.
    g_rvaEcsClass                  = GameBuild::Pick(kFbEcsClass);
    g_rvaChunkMetaClass            = GameBuild::Pick(kFbChunkMetaClass);
    g_rvaTypeNamesSS               = GameBuild::Pick(kFbTypeNamesSS);
    g_rvaCollisionWorldCastRay     = GameBuild::Pick(kFbCollisionWorldCastRay);
    g_rvaCastRayMiPtr              = GameBuild::Pick(kFbCastRayMiPtr);
    g_rvaInitRuntimeMetadata       = GameBuild::Pick(kFbInitRuntimeMetadata);

    // Dynamically scan GameAssembly.dll for Unity Physics / ECS signatures
    // 1. CastRay callsite -> resolves both CastRay MethodInfo slot and CollisionWorld.CastRay function
    uintptr_t vaCastRaySite = PatternScanner::Find(hMod, "4C 8B 0D ? ? ? ? 4C 8D 45 D0 48 8D 55 10 44 0F 29 6D 10 48 8B CE", "Ecs_CastRayCallSite");
    if (vaCastRaySite) {
        uintptr_t miPtr = PatternScanner::ResolveRip(vaCastRaySite, 7, 3);
        if (miPtr >= g_gaBase) g_rvaCastRayMiPtr = miPtr - g_gaBase;
        uintptr_t crFn = PatternScanner::ResolveRip(vaCastRaySite + 0x41, 5, 1);
        if (crFn >= g_gaBase) g_rvaCollisionWorldCastRay = crFn - g_gaBase;
    }

    // 2. InitRuntimeMetadata
    uintptr_t vaInitMeta = PatternScanner::Find(hMod, "48 83 EC 28 E8 ? ? ? ? F0 83 0C 24 00 48 83 C4 28 C3 CC", "Ecs_InitRuntimeMetadata");
    if (vaInitMeta && vaInitMeta >= g_gaBase) {
        g_rvaInitRuntimeMetadata = vaInitMeta - g_gaBase;
    }

    // 3. TypeNames SharedStatic
    uintptr_t vaTypeNames = PatternScanner::Find(hMod, "48 BA E6 2A 33 62 A9 C5 B6 9D 49 B8 80 32 50 01 13 1B EB 85 33 C9 E8 ? ? ? ? 48 8B 0D", "Ecs_TypeNamesSS");
    if (vaTypeNames) {
        uintptr_t tnPtr = PatternScanner::ResolveRip(vaTypeNames + 0x1B, 7, 3);
        if (tnPtr >= g_gaBase) g_rvaTypeNamesSS = tnPtr - g_gaBase;
    }

    // 4. ChunkMeta Class
    uintptr_t vaChunkMeta = PatternScanner::Find(hMod, "48 BA 90 60 B6 2D 2B 4E 0F E0 33 C9 E8 ? ? ? ? 48 8B 0D", "Ecs_ChunkMetaClass");
    if (vaChunkMeta) {
        uintptr_t cmPtr = PatternScanner::ResolveRip(vaChunkMeta + 0x11, 7, 3);
        if (cmPtr >= g_gaBase) g_rvaChunkMetaClass = cmPtr - g_gaBase;
    }

    // 5. ECS Class (EntityComponentStore)
    uintptr_t vaEcsClass = PatternScanner::Find(hMod, "48 8B 01 81 E2 FF 1F 00 00 8B 04 90 C3", "Ecs_EntityComponentStoreClass");
    if (vaEcsClass) {
        uintptr_t ecsPtr = PatternScanner::ResolveRip(vaEcsClass + 0x45, 7, 3);
        if (ecsPtr >= g_gaBase) g_rvaEcsClass = ecsPtr - g_gaBase;
    }

    // 6. SphereCast
    uintptr_t vaSphere = PatternScanner::FindOrFallback(hMod, "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 90 00 00 00 80 3D ? ? ? ? 00 49 8B D9 0F 29 B4 24 80 00 00 00 48 8B FA 0F 28 F2 48 8B F1 75 ? 48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? 01 48 8B 84 24 D0 00 00 00 4C 8D 4C 24 60 4C 8B 05 ? ? ? ? 48 8D 54 24 70 4C 89 44 24 40 0F 28 D6 48 8B CE", GameBuild::Pick(kFbSphereCast), "Ecs_SphereCast");
    g_rvaCollisionWorldSphereCast = (vaSphere && vaSphere >= g_gaBase) ? vaSphere - g_gaBase : 0;

    return g_gaBase != 0;
}

void TryResolveTypeIndices() {
    if (g_types.resolvedFromTypeNames || !ResolveGameAssembly()) return;

    uintptr_t tmCls = 0, tmSf = 0, tmList = 0, entries = 0;
    if (!g_rvaTypeNamesSS || !SafeRead(g_gaBase + g_rvaTypeNamesSS, tmCls) || tmCls < 0x10000) return;
    if (!SafeRead(tmCls + 0xB8, tmSf) || tmSf < 0x10000) return;
    if (!SafeRead(tmSf, tmList) || tmList < 0x10000) return;
    if (!SafeRead(tmList, entries) || entries < 0x10000) return;

    int matched = 0;
    char nameBuf[160] = { 0 };
    for (uint32_t i = 0; i < 4096; ++i) {
        uintptr_t entryAddr = entries + static_cast<size_t>(i) * 24;
        uintptr_t strPtr = 0;
        uint32_t len = 0;
        if (!SafeRead(entryAddr, strPtr) || !SafeRead(entryAddr + 8, len)) break;
        if (strPtr < 0x10000 || len == 0 || len >= sizeof(nameBuf)) continue;
        if (!SafeReadCStr(strPtr, nameBuf, sizeof(nameBuf))) continue;

        if (strcmp(nameBuf, "Unity.Physics.PhysicsCollider") == 0) {
            g_types.physicsCollider = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Physics.PhysicsVelocity") == 0) {
            g_types.physicsVelocity = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Physics.PhysicsWorldSingleton") == 0) {
            g_types.physicsWorldSingleton = i; ++matched;
        } else if (strcmp(nameBuf, "Framework.Views.Cars.PlayerCarTagComponentData") == 0) {
            g_types.playerCarTag = i; ++matched;
        } else if (strcmp(nameBuf, "Framework.Views.Cars.OtherCarTagComponentData") == 0) {
            g_types.otherCarTag = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Transforms.LocalToWorld") == 0) {
            g_types.localToWorld = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Transforms.LocalTransform") == 0) {
            g_types.localTransform = i; ++matched;
        } else if (strcmp(nameBuf, "Modules.CarX.Game.DR3.UIView.Components.GhostCarTag") == 0) {
            g_types.ghostCarTag = i; ++matched;
        }
        if (matched >= 8) break;
    }

    if (matched >= 4) {
        g_types.resolvedFromTypeNames = true;
        Logger::Info("EcsCollision resolved TypeManager indices dynamically (" + std::to_string(matched) + "/8).");
    }
}

bool GetEcsStores(EcsStores& out) {
    if (!ResolveGameAssembly()) return false;

    uintptr_t ecsCls = 0, ecsSf = 0;
    if (!g_rvaEcsClass || !SafeRead(g_gaBase + g_rvaEcsClass, ecsCls) || ecsCls < 0x10000) return false;
    if (!SafeRead(ecsCls + 0xB8, ecsSf) || ecsSf < 0x10000) return false;
    if (!SafeRead(ecsSf + 0x10, out.chunkStore) || out.chunkStore < 0x10000) return false;

    uintptr_t cmCls = 0, cmSf = 0, cmInner = 0;
    if (!g_rvaChunkMetaClass || !SafeRead(g_gaBase + g_rvaChunkMetaClass, cmCls) || cmCls < 0x10000) return false;
    if (!SafeRead(cmCls + 0xB8, cmSf) || cmSf < 0x10000) return false;
    if (!SafeRead(cmSf, cmInner) || cmInner < 0x10000) return false;
    if (!SafeRead(cmInner, out.chunkMetaBase) || out.chunkMetaBase < 0x10000) return false;

    return true;
}

struct ChunkLayout {
    uint32_t entityCount = 0;
    uintptr_t dataBase = 0;
    bool isPlayer = false;
    bool isOther = false;
    bool isGameGhosted = false;
    uint32_t colliderOff = 0;
    bool hasCollider = false;
    uint32_t velocityOff = 0;
    uint16_t velocitySize = 0;
    bool hasVelocity = false;
    uint32_t ltwOff = 0;
    uint16_t ltwSize = 0;
    bool hasLtw = false;
    uint32_t localTfOff = 0;
    uint16_t localTfSize = 0;
    bool hasLocalTf = false;
};

static bool InspectCarChunk(const EcsStores& stores, uint32_t cidx, ChunkLayout& out) {
    uint32_t page = cidx >> CHUNK_PAGE_SHIFT;
    uint32_t sub  = cidx & CHUNK_PAGE_MASK;
    uintptr_t cpage = 0;
    if (!SafeRead(stores.chunkStore + static_cast<size_t>(page) * 8, cpage) || cpage < 0x10000) {
        return false;
    }

    uintptr_t metaPtr = stores.chunkMetaBase + static_cast<size_t>(cidx) * 16;
    uintptr_t arch = 0;
    uint32_t countMeta = 0;
    if (!SafeRead(metaPtr, arch) || arch < 0x10000) return false;
    if (!SafeRead(metaPtr + 8, countMeta)) return false;

    uint32_t count = countMeta & 0xFFFF;
    if (count == 0 || count >= 512) return false;

    uint32_t tcount = 0;
    uintptr_t typesPtr = 0, offsPtr = 0, szsPtr = 0;
    if (!SafeRead(arch + 0xA8, tcount) || tcount == 0 || tcount >= 160) return false;
    if (!SafeRead(arch + 0x70, typesPtr) || typesPtr < 0x10000) return false;
    if (!SafeRead(arch + 0xE0, offsPtr) || offsPtr < 0x10000) return false;
    if (!SafeRead(arch + 0xE8, szsPtr) || szsPtr < 0x10000) return false;

    out = ChunkLayout{};
    out.entityCount = count;
    out.dataBase = cpage + (static_cast<uintptr_t>(sub) << CHUNK_SIZE_SHIFT) + CHUNK_HEADER_BYTES;

    for (uint32_t ti = 0; ti < tcount; ++ti) {
        uint32_t rawType = 0;
        if (!SafeRead(typesPtr + ti * 4, rawType)) return false;
        uint32_t tidx = rawType & 0xFFFF;

        if (tidx == g_types.playerCarTag) {
            out.isPlayer = true;
        } else if (tidx == g_types.otherCarTag) {
            out.isOther = true;
        } else if (tidx == g_types.ghostCarTag) {
            out.isGameGhosted = true;
        } else if (tidx == g_types.physicsCollider) {
            out.hasCollider = SafeRead(offsPtr + ti * 4, out.colliderOff);
        } else if (tidx == g_types.physicsVelocity) {
            out.hasVelocity = SafeRead(offsPtr + ti * 4, out.velocityOff) && SafeRead(szsPtr + ti * 2, out.velocitySize);
        } else if (tidx == g_types.localToWorld) {
            out.hasLtw = SafeRead(offsPtr + ti * 4, out.ltwOff) && SafeRead(szsPtr + ti * 2, out.ltwSize);
        } else if (tidx == g_types.localTransform) {
            out.hasLocalTf = SafeRead(offsPtr + ti * 4, out.localTfOff) && SafeRead(szsPtr + ti * 2, out.localTfSize);
        }
    }

    return out.hasCollider && (out.isPlayer || out.isOther);
}

static void RescanCarChunks(const EcsStores& stores) {
    g_cachedCarChunks.clear();
    uint32_t lastValidPage = 0;
    for (uint32_t page = 0; page < MAX_SCAN_PAGES; ++page) {
        if (page > 128 && page > lastValidPage + 48) break;
        uintptr_t cpage = 0;
        if (!SafeRead(stores.chunkStore + static_cast<size_t>(page) * 8, cpage) || cpage < 0x10000) {
            continue;
        }
        lastValidPage = page;
        uint32_t baseCidx = page << CHUNK_PAGE_SHIFT;
        for (uint32_t sub = 0; sub < 64; ++sub) {
            ChunkLayout layout;
            if (InspectCarChunk(stores, baseCidx + sub, layout)) {
                g_cachedCarChunks.push_back(baseCidx + sub);
            }
        }
    }
}

void CollectRawEcsCars(const EcsStores& stores, size_t liveCarCount, std::vector<RawEcsCar>& outCars) {
    outCars.clear();
    auto now = std::chrono::steady_clock::now();
    float sinceScan = std::chrono::duration<float>(now - g_lastChunkScan).count();

    bool needRescan = g_cachedCarChunks.empty() || sinceScan > 0.5f || liveCarCount != g_lastSeenCarCount;
    bool cacheInvalid = false;

    if (!needRescan) {
        for (uint32_t cidx : g_cachedCarChunks) {
            ChunkLayout layout;
            if (!InspectCarChunk(stores, cidx, layout)) {
                cacheInvalid = true;
                break;
            }
        }
        if (cacheInvalid) needRescan = true;
    }

    if (needRescan) {
        RescanCarChunks(stores);
        g_lastChunkScan = now;
        g_lastSeenCarCount = liveCarCount;
    }

    for (uint32_t cidx : g_cachedCarChunks) {
        ChunkLayout layout;
        if (!InspectCarChunk(stores, cidx, layout)) continue;

        for (uint32_t ei = 0; ei < layout.entityCount; ++ei) {
            uint64_t ent = 0;
            uintptr_t blob = 0;
            if (!SafeRead(layout.dataBase + ei * 8, ent) || ent == 0) continue;
            if (!SafeRead(layout.dataBase + layout.colliderOff + ei * 8, blob) || blob < 0x10000) continue;

            RawEcsCar car;
            car.entityKey = ent;
            car.blobPtr = reinterpret_cast<void*>(blob);
            car.isPlayer = layout.isPlayer;
            car.isGameGhosted = layout.isGameGhosted;

            if (layout.hasLocalTf && layout.localTfSize >= 12) {
                if (SafeReadBytes(reinterpret_cast<const void*>(layout.dataBase + layout.localTfOff + ei * layout.localTfSize),
                                  car.pos, sizeof(float) * 3)) {
                    car.hasPos = std::isfinite(car.pos[0]) && std::isfinite(car.pos[1]) && std::isfinite(car.pos[2]);
                }
            }
            if (layout.hasLtw && layout.ltwSize >= 64) {
                float ltw[16] = { 0 };
                if (SafeReadBytes(reinterpret_cast<const void*>(layout.dataBase + layout.ltwOff + ei * layout.ltwSize),
                                  ltw, sizeof(ltw))) {
                    if (!car.hasPos && std::isfinite(ltw[12])) {
                        car.pos[0] = ltw[12]; car.pos[1] = ltw[13]; car.pos[2] = ltw[14];
                        car.hasPos = true;
                    }
                    if (std::isfinite(ltw[8]) && std::isfinite(ltw[10])) {
                        car.fwd[0] = ltw[8]; car.fwd[1] = ltw[9]; car.fwd[2] = ltw[10];
                        car.hasFwd = true;
                    }
                }
            }
            if (layout.hasVelocity && layout.velocitySize >= 12) {
                if (SafeReadBytes(reinterpret_cast<const void*>(layout.dataBase + layout.velocityOff + ei * layout.velocitySize),
                                  car.vel, sizeof(float) * 3)) {
                    car.hasVel = std::isfinite(car.vel[0]) && std::isfinite(car.vel[2]);
                }
            }
            outCars.push_back(car);
        }
    }
}

// ================= Collider Blob Baseline & Filter Mutation =================

} // namespace EcsCollisionInternal

// ================= DOTS Physics Queries (CastRay / SphereCast) =================

static bool InspectPhysicsWorldChunk(const EcsStores& stores, uint32_t cidx, uintptr_t& outPtr) {
    uint32_t page = cidx >> CHUNK_PAGE_SHIFT;
    uint32_t sub  = cidx & CHUNK_PAGE_MASK;
    uintptr_t cpage = 0;
    if (!SafeRead(stores.chunkStore + static_cast<size_t>(page) * 8, cpage) || cpage < 0x10000) {
        return false;
    }

    uintptr_t metaPtr = stores.chunkMetaBase + static_cast<size_t>(cidx) * 16;
    uintptr_t arch = 0;
    uint32_t countMeta = 0;
    if (!SafeRead(metaPtr, arch) || arch < 0x10000) return false;
    if (!SafeRead(metaPtr + 8, countMeta)) return false;

    uint32_t count = countMeta & 0xFFFF;
    if (count == 0 || count >= 16) return false;

    uint32_t tcount = 0;
    uintptr_t typesPtr = 0, offsPtr = 0, szsPtr = 0;
    if (!SafeRead(arch + 0xA8, tcount) || tcount == 0 || tcount >= 160) return false;
    if (!SafeRead(arch + 0x70, typesPtr) || typesPtr < 0x10000) return false;
    if (!SafeRead(arch + 0xE0, offsPtr) || offsPtr < 0x10000) return false;
    if (!SafeRead(arch + 0xE8, szsPtr) || szsPtr < 0x10000) return false;

    for (uint32_t ti = 0; ti < tcount; ++ti) {
        uint32_t rawType = 0;
        if (!SafeRead(typesPtr + ti * 4, rawType)) return false;
        if ((rawType & 0xFFFF) != g_types.physicsWorldSingleton) continue;

        uint32_t off = 0;
        uint16_t sz = 0;
        if (!SafeRead(offsPtr + ti * 4, off) || !SafeRead(szsPtr + ti * 2, sz) || sz < 0x100) {
            return false;
        }
        uintptr_t ptr = cpage + (static_cast<uintptr_t>(sub) << CHUNK_SIZE_SHIFT) + CHUNK_HEADER_BYTES + off;
        uintptr_t bodiesPtr = 0;
        uint32_t nTotal = 0;
        if (!SafeRead(ptr + 0x00, bodiesPtr) || bodiesPtr < 0x10000) return false;
        if (!SafeRead(ptr + 0x08, nTotal) || nTotal == 0 || nTotal > 200000) return false;
        outPtr = ptr;
        return true;
    }
    return false;
}

static uintptr_t ResolveCollisionWorldPtr() {
    TryResolveTypeIndices();
    EcsStores stores{};
    if (!GetEcsStores(stores)) return 0;

    uintptr_t cwPtr = 0;
    if (g_cachedPhysicsWorldChunk != UINT32_MAX &&
        InspectPhysicsWorldChunk(stores, g_cachedPhysicsWorldChunk, cwPtr)) {
        return cwPtr;
    }

    g_cachedPhysicsWorldChunk = UINT32_MAX;
    uint32_t lastValidPage = 0;
    for (uint32_t page = 0; page < MAX_SCAN_PAGES; ++page) {
        if (page > 128 && page > lastValidPage + 48) break;
        uintptr_t cpage = 0;
        if (!SafeRead(stores.chunkStore + static_cast<size_t>(page) * 8, cpage) || cpage < 0x10000) {
            continue;
        }
        lastValidPage = page;
        uint32_t baseCidx = page << CHUNK_PAGE_SHIFT;
        for (uint32_t sub = 0; sub < 64; ++sub) {
            if (InspectPhysicsWorldChunk(stores, baseCidx + sub, cwPtr)) {
                g_cachedPhysicsWorldChunk = baseCidx + sub;
                return cwPtr;
            }
        }
    }
    return 0;
}

static void* SafeGetOrInitCastRayMISeh(uintptr_t gaBase) {
    __try {
        if (!g_rvaCastRayMiPtr) return nullptr;
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(gaBase + g_rvaCastRayMiPtr);
        if (*slot == 0 && g_rvaInitRuntimeMetadata) {
            auto initFn = reinterpret_cast<void(__fastcall*)(void*)>(gaBase + g_rvaInitRuntimeMetadata);
            initFn(slot);
        }
        uintptr_t val = *slot;
        return (val > 0x10000) ? reinterpret_cast<void*>(val) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool SafeDotsCastRaySEH(void* fnCastRay, void* cwPtr, void* miPtr,
                               const float* start3, const float* end3,
                               uint32_t belongsTo, uint32_t collidesWith,
                               uint32_t* outDwords16) {
    __try {
        alignas(16) uint8_t inputBuf[0x90] = { 0 };

        // RaycastInput layout:
        // +0x00: CollisionFilter (belongsTo, collidesWith, groupIndex = 0)
        *reinterpret_cast<uint32_t*>(inputBuf + 0x00) = belongsTo;
        *reinterpret_cast<uint32_t*>(inputBuf + 0x04) = collidesWith;
        *reinterpret_cast<int32_t*>(inputBuf + 0x08)  = 0;

        // +0x0C: Start (float3)
        float* pStart = reinterpret_cast<float*>(inputBuf + 0x0C);
        pStart[0] = start3[0];
        pStart[1] = start3[1];
        pStart[2] = start3[2];

        // +0x18: Direction (float3 = end - start)
        float* pDir = reinterpret_cast<float*>(inputBuf + 0x18);
        pDir[0] = end3[0] - start3[0];
        pDir[1] = end3[1] - start3[1];
        pDir[2] = end3[2] - start3[2];

        // +0x24: InverseDirection (float3)
        float* pInv = reinterpret_cast<float*>(inputBuf + 0x24);
        pInv[0] = (fabsf(pDir[0]) > 1e-6f) ? (1.0f / pDir[0]) : 1e6f;
        pInv[1] = (fabsf(pDir[1]) > 1e-6f) ? (1.0f / pDir[1]) : 1e6f;
        pInv[2] = (fabsf(pDir[2]) > 1e-6f) ? (1.0f / pDir[2]) : 1e6f;

        alignas(16) uint8_t hitBuf[0x40] = { 0 };
        *reinterpret_cast<float*>(hitBuf + 0x00) = 1.0f;

        auto castFn = reinterpret_cast<uint8_t(__fastcall*)(void*, const void*, void*, void*)>(fnCastRay);
        uint8_t hit = castFn(cwPtr, inputBuf, hitBuf, miPtr);
        if (hit && outDwords16) {
            memcpy(outDwords16, hitBuf, 16 * sizeof(uint32_t));
        }
        return hit != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EcsCollision::CastRay(const float start[3], const float end[3],
                           uint32_t belongsTo, uint32_t collidesWith,
                           DotsRaycastHit& outHit) {
    outHit = DotsRaycastHit{};
    if (!ResolveGameAssembly()) return false;

    uintptr_t cwPtr = ResolveCollisionWorldPtr();
    if (!cwPtr) return false;

    void* miPtr = SafeGetOrInitCastRayMISeh(g_gaBase);
    if (!miPtr) return false;

    if (!g_rvaCollisionWorldCastRay) return false;
    void* fnCastRay = reinterpret_cast<void*>(g_gaBase + g_rvaCollisionWorldCastRay);

    if (!SafeDotsCastRaySEH(fnCastRay, reinterpret_cast<void*>(cwPtr), miPtr,
                            start, end,
                            belongsTo, collidesWith, outHit.rawDwords)) {
        return false;
    }

    const float* rf = reinterpret_cast<const float*>(outHit.rawDwords);
    outHit.fraction = std::clamp(rf[0], 0.0f, 1.0f);
    outHit.point[0] = start[0] + (end[0] - start[0]) * outHit.fraction;
    outHit.point[1] = start[1] + (end[1] - start[1]) * outHit.fraction;
    outHit.point[2] = start[2] + (end[2] - start[2]) * outHit.fraction;

    // Verified on live Unity.Physics.RaycastHit (64 bytes): Position @ +0x28..+0x33, SurfaceNormal @ +0x34..+0x3F
    float nx = rf[13], ny = rf[14], nz = rf[15];
    float nLen = sqrtf(nx * nx + ny * ny + nz * nz);
    if (nLen > 1e-4f) {
        outHit.normal[0] = nx / nLen;
        outHit.normal[1] = ny / nLen;
        outHit.normal[2] = nz / nLen;
    }
    return true;
}

namespace {

struct alignas(4) DotsCollisionFilterRaw {
    uint32_t belongsTo;
    uint32_t collidesWith;
    int32_t  groupIndex;
};

struct alignas(16) DotsColliderCastHitRaw {
    uint8_t bytes[0x40]; // Unity.Physics.ColliderCastHit is 0x3C bytes
};

static bool SafeDotsSphereCastSEH(void* fnSphereCast, void* cwPtr,
                                  const float* origin3, float radius,
                                  const float* dir3, float maxDistance,
                                  uint32_t belongsTo, uint32_t collidesWith,
                                  uint8_t queryInteraction,
                                  DotsColliderCastHitRaw* outRawHit) {
    __try {
        alignas(16) float origAligned[4] = { origin3[0], origin3[1], origin3[2], 0.0f };
        alignas(16) float dirAligned[4]  = { dir3[0],    dir3[1],    dir3[2],    0.0f };
        DotsCollisionFilterRaw flt{ belongsTo, collidesWith, 0 };
        DotsColliderCastHitRaw hitBuf = {};
        *reinterpret_cast<float*>(hitBuf.bytes + 0x00) = 1.0f;

        typedef uint8_t(__fastcall* t_SphereCast)(
            void* cw,
            const float* origin,
            float radius,
            const float* direction,
            float maxDistance,
            void* outColliderCastHit,
            const DotsCollisionFilterRaw* filter,
            uint8_t queryInteraction,
            const void* methodInfo);

        auto castFn = reinterpret_cast<t_SphereCast>(fnSphereCast);
        uint8_t hit = castFn(cwPtr, origAligned, radius, dirAligned, maxDistance,
                             &hitBuf, &flt, queryInteraction, nullptr);
        if (hit && outRawHit) {
            memcpy(outRawHit, &hitBuf, sizeof(DotsColliderCastHitRaw));
        }
        return hit != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool EcsCollision::SphereCast(const float origin[3], float radius, const float dir[3], float maxDistance,
                              uint32_t belongsTo, uint32_t collidesWith,
                              SphereCastHit& outHit, uint8_t queryInteraction,
                              bool* outQueryExecuted) {
    outHit = SphereCastHit{};
    if (outQueryExecuted) *outQueryExecuted = false;
    if (!origin || !dir) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(origin[i]) || !std::isfinite(dir[i])) return false;
    }
    if (!std::isfinite(radius) || radius < 0.005f || !std::isfinite(maxDistance) || maxDistance < 0.005f) {
        return false;
    }
    const float dLen = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dLen < 1e-4f) return false;
    const float uDir[3] = { dir[0] / dLen, dir[1] / dLen, dir[2] / dLen };

    if (!ResolveGameAssembly()) return false;
    uintptr_t cwPtr = ResolveCollisionWorldPtr();
    if (!cwPtr) return false;
    if (outQueryExecuted) *outQueryExecuted = true;

    if (!g_rvaCollisionWorldSphereCast) return false;
    void* fnSphereCast = reinterpret_cast<void*>(g_gaBase + g_rvaCollisionWorldSphereCast);
    DotsColliderCastHitRaw rawHit{};
    if (!SafeDotsSphereCastSEH(fnSphereCast, reinterpret_cast<void*>(cwPtr),
                               origin, radius, uDir, maxDistance,
                               belongsTo, collidesWith, queryInteraction, &rawHit)) {
        return false;
    }

    // Verified from GameAssembly.dll 0x185B57270 / 0x182098B30 / 0x18178A94A:
    // ColliderCastHit (0x3C bytes): Fraction@+0x00, RigidBodyIndex@+0x04, Position@+0x20, SurfaceNormal@+0x2C
    const float rawFrac = *reinterpret_cast<const float*>(rawHit.bytes + 0x00);
    const int32_t rbIdx = *reinterpret_cast<const int32_t*>(rawHit.bytes + 0x04);
    const float* posPtr = reinterpret_cast<const float*>(rawHit.bytes + 0x20);
    const float* nrmPtr = reinterpret_cast<const float*>(rawHit.bytes + 0x2C);

    if (!std::isfinite(rawFrac) ||
        !std::isfinite(posPtr[0]) || !std::isfinite(posPtr[1]) || !std::isfinite(posPtr[2])) {
        return false;
    }

    outHit.hasHit = true;
    outHit.fraction = std::clamp(rawFrac, 0.0f, 1.0f);
    outHit.rigidBodyIndex = rbIdx;
    outHit.position[0] = posPtr[0];
    outHit.position[1] = posPtr[1];
    outHit.position[2] = posPtr[2];

    float nx = nrmPtr[0], ny = nrmPtr[1], nz = nrmPtr[2];
    if (std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz)) {
        float nLen = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nLen > 1e-4f) {
            outHit.surfaceNormal[0] = nx / nLen;
            outHit.surfaceNormal[1] = ny / nLen;
            outHit.surfaceNormal[2] = nz / nLen;
        }
    }
    return true;
}
