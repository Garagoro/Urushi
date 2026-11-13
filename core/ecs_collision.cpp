#include "ecs_collision.h"
#include "pattern_scanner.h"
#include "game_build.h"
#include "unity_bridge.h"
#include "native_carx.h"
#include "logger.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

// Fallback RVAs inside GameAssembly.dll per Steam branch (see game_build.h); signatures are scanned first.
constexpr BranchRva kFbEcsClass             = { 0x8428C98, 0x9770E30 }; // EntityComponentStore class static_fields -> s_chunkStore
constexpr BranchRva kFbChunkMetaClass       = { 0x843CB48, 0x982E7E0 }; // SharedStatic chunk meta table
constexpr BranchRva kFbTypeNamesSS          = { 0x83F57D8, 0x97DCAC8 }; // TypeManager.TypeNames SharedStatic<UnsafeList>
constexpr BranchRva kFbCollisionWorldCastRay= { 0x2717C40, 0x2F28300 }; // Unity.Physics.CollisionWorld.CastRay(RaycastInput, out RaycastHit)
constexpr BranchRva kFbSphereCast           = { 0x5B57270, 0x6B97C00 }; // Unity.Physics.CollisionWorld.SphereCast (two identical generic copies; first is used)
constexpr BranchRva kFbCastRayMiPtr         = { 0x8387960, 0x9849F98 }; // Generic MethodInfo* slot for CollisionWorld.CastRay<RaycastHit>
constexpr BranchRva kFbInitRuntimeMetadata  = { 0x09264A0, 0x0A76750 }; // il2cpp_codegen_initialize_runtime_metadata

static uintptr_t s_rvaEcsClass                  = 0;
static uintptr_t s_rvaChunkMetaClass           = 0;
static uintptr_t s_rvaTypeNamesSS              = 0;
static uintptr_t s_rvaCollisionWorldCastRay    = 0;
static uintptr_t s_rvaCollisionWorldSphereCast = 0;
static uintptr_t s_rvaCastRayMiPtr             = 0;
static uintptr_t s_rvaInitRuntimeMetadata      = 0;

constexpr uint32_t FALLBACK_TIDX_PHYSICS_COLLIDER  = 0x5BA;
constexpr uint32_t FALLBACK_TIDX_PHYSICS_VELOCITY  = 0x5BE;
constexpr uint32_t FALLBACK_TIDX_PHYSICS_WORLD_SGL = 0x5C6;
constexpr uint32_t FALLBACK_TIDX_PLAYER_CAR_TAG    = 0x5E2;
constexpr uint32_t FALLBACK_TIDX_OTHER_CAR_TAG     = 0x5E3;
constexpr uint32_t FALLBACK_TIDX_LOCAL_TO_WORLD    = 0x8A9;
constexpr uint32_t FALLBACK_TIDX_LOCAL_TRANSFORM   = 0x8AA;
constexpr uint32_t FALLBACK_TIDX_GHOST_CAR_TAG     = 0x8B2;

constexpr uint32_t CHUNK_HEADER_BYTES = 0x40;
constexpr uint32_t CHUNK_SIZE_SHIFT   = 14; // 16 KB chunks
constexpr uint32_t CHUNK_PAGE_MASK    = 0x3F;
constexpr uint32_t CHUNK_PAGE_SHIFT   = 6;
constexpr uint32_t MAX_SCAN_PAGES     = 512; // 32,768 chunks max
constexpr uint8_t  COLLIDER_COMPOUND  = 8;
constexpr int32_t  GHOST_GROUP_INDEX  = -7777;
constexpr float    OVERLAP_SAFETY_DIST_SQ = 6.0f * 6.0f;
constexpr float    RELEASE_HYSTERESIS_SEC = 0.45f;

#pragma pack(push, 1)
struct CollisionFilter {
    uint32_t belongsTo;
    uint32_t collidesWith;
    int32_t  groupIndex;
};
#pragma pack(pop)

struct EcsTypeIndices {
    bool resolvedFromTypeNames = false;
    uint32_t physicsCollider       = FALLBACK_TIDX_PHYSICS_COLLIDER;
    uint32_t physicsVelocity       = FALLBACK_TIDX_PHYSICS_VELOCITY;
    uint32_t physicsWorldSingleton = FALLBACK_TIDX_PHYSICS_WORLD_SGL;
    uint32_t playerCarTag          = FALLBACK_TIDX_PLAYER_CAR_TAG;
    uint32_t otherCarTag           = FALLBACK_TIDX_OTHER_CAR_TAG;
    uint32_t localToWorld          = FALLBACK_TIDX_LOCAL_TO_WORLD;
    uint32_t localTransform        = FALLBACK_TIDX_LOCAL_TRANSFORM;
    uint32_t ghostCarTag           = FALLBACK_TIDX_GHOST_CAR_TAG;
};

struct RawEcsCar {
    uint64_t entityKey = 0;
    void*    blobPtr = nullptr;
    bool     isPlayer = false;
    bool     isGameGhosted = false;
    float    pos[3] = { 0.0f, 0.0f, 0.0f };
    float    fwd[3] = { 0.0f, 0.0f, 1.0f };
    float    vel[3] = { 0.0f, 0.0f, 0.0f };
    bool     hasPos = false;
    bool     hasFwd = false;
    bool     hasVel = false;
};

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

static std::recursive_mutex s_mutex;
static uintptr_t s_gaBase = 0;
static uintptr_t s_gaSize = 0;
static EcsTypeIndices s_types{};
static SelectiveGhostConfig s_config{};

static std::unordered_map<std::string, CarGhostOverride> s_overridesByName;
static std::unordered_map<uintptr_t, CarGhostOverride>   s_overridesByPtr;
static std::unordered_map<uint64_t, CachedColliderBaseline> s_baselines;
static std::unordered_map<uint64_t, AutoLatchState>      s_autoLatch;
static std::vector<uint32_t> s_cachedCarChunks;
static uint32_t s_cachedPhysicsWorldChunk = UINT32_MAX;
static std::vector<CarGhostState> s_latestStates;
static std::chrono::steady_clock::time_point s_lastChunkScan{};
static size_t s_lastSeenCarCount = 0;

// ================= Leaf SEH Memory Helpers =================

static bool SafeReadBytes(const void* addr, void* outBuf, size_t size) {
    __try {
        if (!addr || reinterpret_cast<uintptr_t>(addr) < 0x10000 || !outBuf) return false;
        memcpy(outBuf, addr, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeWriteBytes(void* addr, const void* inBuf, size_t size) {
    __try {
        if (!addr || reinterpret_cast<uintptr_t>(addr) < 0x10000 || !inBuf) return false;
        memcpy(addr, inBuf, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
static bool SafeRead(uintptr_t addr, T& outVal) {
    return SafeReadBytes(reinterpret_cast<const void*>(addr), &outVal, sizeof(T));
}

template <typename T>
static bool SafeWrite(uintptr_t addr, const T& val) {
    return SafeWriteBytes(reinterpret_cast<void*>(addr), &val, sizeof(T));
}

static bool SafeReadCStr(uintptr_t addr, char* buf, size_t maxLen) {
    __try {
        if (addr < 0x10000 || !buf || maxLen == 0) return false;
        const char* src = reinterpret_cast<const char*>(addr);
        for (size_t i = 0; i < maxLen - 1; ++i) {
            char ch = src[i];
            buf[i] = ch;
            if (ch == '\0') return true;
        }
        buf[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (buf && maxLen > 0) buf[0] = '\0';
        return false;
    }
}

// ================= Dynamic ECS Pointer & Type Resolution =================

static bool ResolveGameAssembly() {
    if (s_gaBase) return true;
    HMODULE hMod = GetModuleHandleW(L"GameAssembly.dll");
    if (!hMod) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) return false;
    s_gaBase = reinterpret_cast<uintptr_t>(hMod);
    s_gaSize = mi.SizeOfImage;

    // Branch fallbacks first (0 on unknown builds); signature hits below override them.
    s_rvaEcsClass                  = GameBuild::Pick(kFbEcsClass);
    s_rvaChunkMetaClass            = GameBuild::Pick(kFbChunkMetaClass);
    s_rvaTypeNamesSS               = GameBuild::Pick(kFbTypeNamesSS);
    s_rvaCollisionWorldCastRay     = GameBuild::Pick(kFbCollisionWorldCastRay);
    s_rvaCastRayMiPtr              = GameBuild::Pick(kFbCastRayMiPtr);
    s_rvaInitRuntimeMetadata       = GameBuild::Pick(kFbInitRuntimeMetadata);

    // Dynamically scan GameAssembly.dll for Unity Physics / ECS signatures
    // 1. CastRay callsite -> resolves both CastRay MethodInfo slot and CollisionWorld.CastRay function
    uintptr_t vaCastRaySite = PatternScanner::Find(hMod, "4C 8B 0D ? ? ? ? 4C 8D 45 D0 48 8D 55 10 44 0F 29 6D 10 48 8B CE", "Ecs_CastRayCallSite");
    if (vaCastRaySite) {
        uintptr_t miPtr = PatternScanner::ResolveRip(vaCastRaySite, 7, 3);
        if (miPtr >= s_gaBase) s_rvaCastRayMiPtr = miPtr - s_gaBase;
        uintptr_t crFn = PatternScanner::ResolveRip(vaCastRaySite + 0x41, 5, 1);
        if (crFn >= s_gaBase) s_rvaCollisionWorldCastRay = crFn - s_gaBase;
    }

    // 2. InitRuntimeMetadata
    uintptr_t vaInitMeta = PatternScanner::Find(hMod, "48 83 EC 28 E8 ? ? ? ? F0 83 0C 24 00 48 83 C4 28 C3 CC", "Ecs_InitRuntimeMetadata");
    if (vaInitMeta && vaInitMeta >= s_gaBase) {
        s_rvaInitRuntimeMetadata = vaInitMeta - s_gaBase;
    }

    // 3. TypeNames SharedStatic
    uintptr_t vaTypeNames = PatternScanner::Find(hMod, "48 BA E6 2A 33 62 A9 C5 B6 9D 49 B8 80 32 50 01 13 1B EB 85 33 C9 E8 ? ? ? ? 48 8B 0D", "Ecs_TypeNamesSS");
    if (vaTypeNames) {
        uintptr_t tnPtr = PatternScanner::ResolveRip(vaTypeNames + 0x1B, 7, 3);
        if (tnPtr >= s_gaBase) s_rvaTypeNamesSS = tnPtr - s_gaBase;
    }

    // 4. ChunkMeta Class
    uintptr_t vaChunkMeta = PatternScanner::Find(hMod, "48 BA 90 60 B6 2D 2B 4E 0F E0 33 C9 E8 ? ? ? ? 48 8B 0D", "Ecs_ChunkMetaClass");
    if (vaChunkMeta) {
        uintptr_t cmPtr = PatternScanner::ResolveRip(vaChunkMeta + 0x11, 7, 3);
        if (cmPtr >= s_gaBase) s_rvaChunkMetaClass = cmPtr - s_gaBase;
    }

    // 5. ECS Class (EntityComponentStore)
    uintptr_t vaEcsClass = PatternScanner::Find(hMod, "48 8B 01 81 E2 FF 1F 00 00 8B 04 90 C3", "Ecs_EntityComponentStoreClass");
    if (vaEcsClass) {
        uintptr_t ecsPtr = PatternScanner::ResolveRip(vaEcsClass + 0x45, 7, 3);
        if (ecsPtr >= s_gaBase) s_rvaEcsClass = ecsPtr - s_gaBase;
    }

    // 6. SphereCast
    uintptr_t vaSphere = PatternScanner::FindOrFallback(hMod, "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 90 00 00 00 80 3D ? ? ? ? 00 49 8B D9 0F 29 B4 24 80 00 00 00 48 8B FA 0F 28 F2 48 8B F1 75 ? 48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? 01 48 8B 84 24 D0 00 00 00 4C 8D 4C 24 60 4C 8B 05 ? ? ? ? 48 8D 54 24 70 4C 89 44 24 40 0F 28 D6 48 8B CE", GameBuild::Pick(kFbSphereCast), "Ecs_SphereCast");
    s_rvaCollisionWorldSphereCast = (vaSphere && vaSphere >= s_gaBase) ? vaSphere - s_gaBase : 0;

    return s_gaBase != 0;
}

static void TryResolveTypeIndices() {
    if (s_types.resolvedFromTypeNames || !ResolveGameAssembly()) return;

    uintptr_t tmCls = 0, tmSf = 0, tmList = 0, entries = 0;
    if (!s_rvaTypeNamesSS || !SafeRead(s_gaBase + s_rvaTypeNamesSS, tmCls) || tmCls < 0x10000) return;
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
            s_types.physicsCollider = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Physics.PhysicsVelocity") == 0) {
            s_types.physicsVelocity = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Physics.PhysicsWorldSingleton") == 0) {
            s_types.physicsWorldSingleton = i; ++matched;
        } else if (strcmp(nameBuf, "Framework.Views.Cars.PlayerCarTagComponentData") == 0) {
            s_types.playerCarTag = i; ++matched;
        } else if (strcmp(nameBuf, "Framework.Views.Cars.OtherCarTagComponentData") == 0) {
            s_types.otherCarTag = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Transforms.LocalToWorld") == 0) {
            s_types.localToWorld = i; ++matched;
        } else if (strcmp(nameBuf, "Unity.Transforms.LocalTransform") == 0) {
            s_types.localTransform = i; ++matched;
        } else if (strcmp(nameBuf, "Modules.CarX.Game.DR3.UIView.Components.GhostCarTag") == 0) {
            s_types.ghostCarTag = i; ++matched;
        }
        if (matched >= 8) break;
    }

    if (matched >= 4) {
        s_types.resolvedFromTypeNames = true;
        Logger::Info("EcsCollision resolved TypeManager indices dynamically (" + std::to_string(matched) + "/8).");
    }
}

struct EcsStores {
    uintptr_t chunkStore = 0;
    uintptr_t chunkMetaBase = 0;
};

static bool GetEcsStores(EcsStores& out) {
    if (!ResolveGameAssembly()) return false;

    uintptr_t ecsCls = 0, ecsSf = 0;
    if (!s_rvaEcsClass || !SafeRead(s_gaBase + s_rvaEcsClass, ecsCls) || ecsCls < 0x10000) return false;
    if (!SafeRead(ecsCls + 0xB8, ecsSf) || ecsSf < 0x10000) return false;
    if (!SafeRead(ecsSf + 0x10, out.chunkStore) || out.chunkStore < 0x10000) return false;

    uintptr_t cmCls = 0, cmSf = 0, cmInner = 0;
    if (!s_rvaChunkMetaClass || !SafeRead(s_gaBase + s_rvaChunkMetaClass, cmCls) || cmCls < 0x10000) return false;
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

        if (tidx == s_types.playerCarTag) {
            out.isPlayer = true;
        } else if (tidx == s_types.otherCarTag) {
            out.isOther = true;
        } else if (tidx == s_types.ghostCarTag) {
            out.isGameGhosted = true;
        } else if (tidx == s_types.physicsCollider) {
            out.hasCollider = SafeRead(offsPtr + ti * 4, out.colliderOff);
        } else if (tidx == s_types.physicsVelocity) {
            out.hasVelocity = SafeRead(offsPtr + ti * 4, out.velocityOff) && SafeRead(szsPtr + ti * 2, out.velocitySize);
        } else if (tidx == s_types.localToWorld) {
            out.hasLtw = SafeRead(offsPtr + ti * 4, out.ltwOff) && SafeRead(szsPtr + ti * 2, out.ltwSize);
        } else if (tidx == s_types.localTransform) {
            out.hasLocalTf = SafeRead(offsPtr + ti * 4, out.localTfOff) && SafeRead(szsPtr + ti * 2, out.localTfSize);
        }
    }

    return out.hasCollider && (out.isPlayer || out.isOther);
}

static void RescanCarChunks(const EcsStores& stores) {
    s_cachedCarChunks.clear();
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
                s_cachedCarChunks.push_back(baseCidx + sub);
            }
        }
    }
}

static void CollectRawEcsCars(const EcsStores& stores, size_t liveCarCount, std::vector<RawEcsCar>& outCars) {
    outCars.clear();
    auto now = std::chrono::steady_clock::now();
    float sinceScan = std::chrono::duration<float>(now - s_lastChunkScan).count();

    bool needRescan = s_cachedCarChunks.empty() || sinceScan > 0.5f || liveCarCount != s_lastSeenCarCount;
    bool cacheInvalid = false;

    if (!needRescan) {
        for (uint32_t cidx : s_cachedCarChunks) {
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
        s_lastChunkScan = now;
        s_lastSeenCarCount = liveCarCount;
    }

    for (uint32_t cidx : s_cachedCarChunks) {
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

// ================= Public API =================

void EcsCollision::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    ResolveGameAssembly();
    TryResolveTypeIndices();
}

void EcsCollision::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (NativeCarX::IsInSession()) {
        EcsStores stores;
        if (GetEcsStores(stores)) {
            std::vector<RawEcsCar> ecsCars;
            CollectRawEcsCars(stores, s_lastSeenCarCount, ecsCars);
            std::unordered_set<void*> activeBlobs;
            for (const auto& c : ecsCars) activeBlobs.insert(c.blobPtr);
            RestoreAllModifiedBaselines(&activeBlobs);
        }
    }
    s_baselines.clear();
    s_autoLatch.clear();
    s_cachedCarChunks.clear();
    s_latestStates.clear();
}

void EcsCollision::SetConfig(const SelectiveGhostConfig& cfg) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_config = cfg;
    s_config.minSpeedKmh = std::clamp(s_config.minSpeedKmh, 0.0f, 120.0f);
    s_config.dotThreshold = std::clamp(s_config.dotThreshold, -0.95f, 0.25f);
    s_config.maxDistanceMeters = std::clamp(s_config.maxDistanceMeters, 0.0f, 1000.0f);
}

SelectiveGhostConfig EcsCollision::GetConfig() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_config;
}

void EcsCollision::SetCarOverride(const std::string& unityName, uintptr_t carPtr, CarGhostOverride mode) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_overridesByName.clear();
    s_overridesByPtr.clear();
}

std::vector<CarGhostState> EcsCollision::GetCarStates() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_latestStates;
}

void EcsCollision::UpdateOnMainThread(float dt, const std::vector<LiveCarInfo>& liveCars) {
    if (!UnityBridge::IsOnMainThread()) return;

    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!NativeCarX::IsInSession()) {
        s_baselines.clear();
        s_autoLatch.clear();
        s_cachedCarChunks.clear();
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
        if ((rawType & 0xFFFF) != s_types.physicsWorldSingleton) continue;

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
    if (s_cachedPhysicsWorldChunk != UINT32_MAX &&
        InspectPhysicsWorldChunk(stores, s_cachedPhysicsWorldChunk, cwPtr)) {
        return cwPtr;
    }

    s_cachedPhysicsWorldChunk = UINT32_MAX;
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
                s_cachedPhysicsWorldChunk = baseCidx + sub;
                return cwPtr;
            }
        }
    }
    return 0;
}

static void* SafeGetOrInitCastRayMISeh(uintptr_t gaBase) {
    __try {
        if (!s_rvaCastRayMiPtr) return nullptr;
        uintptr_t* slot = reinterpret_cast<uintptr_t*>(gaBase + s_rvaCastRayMiPtr);
        if (*slot == 0 && s_rvaInitRuntimeMetadata) {
            auto initFn = reinterpret_cast<void(__fastcall*)(void*)>(gaBase + s_rvaInitRuntimeMetadata);
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

    void* miPtr = SafeGetOrInitCastRayMISeh(s_gaBase);
    if (!miPtr) return false;

    if (!s_rvaCollisionWorldCastRay) return false;
    void* fnCastRay = reinterpret_cast<void*>(s_gaBase + s_rvaCollisionWorldCastRay);

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

    if (!s_rvaCollisionWorldSphereCast) return false;
    void* fnSphereCast = reinterpret_cast<void*>(s_gaBase + s_rvaCollisionWorldSphereCast);
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
