#pragma once

#include "ecs_collision.h"
#include "game_build.h"
#include "il2cpp_helpers.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace EcsCollisionInternal {

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

struct EcsStores {
    uintptr_t chunkStore = 0;
    uintptr_t chunkMetaBase = 0;
};

// Shared state (defined in ecs_physics.cpp)
extern std::recursive_mutex g_mutex;
extern uintptr_t g_gaBase;
extern uintptr_t g_gaSize;
extern EcsTypeIndices g_types;
extern std::vector<uint32_t> g_cachedCarChunks;
extern uint32_t g_cachedPhysicsWorldChunk;
extern std::chrono::steady_clock::time_point g_lastChunkScan;
extern size_t g_lastSeenCarCount;

extern uintptr_t g_rvaEcsClass;
extern uintptr_t g_rvaChunkMetaClass;
extern uintptr_t g_rvaTypeNamesSS;
extern uintptr_t g_rvaCollisionWorldCastRay;
extern uintptr_t g_rvaCollisionWorldSphereCast;
extern uintptr_t g_rvaCastRayMiPtr;
extern uintptr_t g_rvaInitRuntimeMetadata;

// Shared SEH memory helpers
inline bool SafeReadBytes(const void* addr, void* outBuf, size_t size) {
    __try {
        if (!addr || !outBuf || !Il2CppHelpers::IsCanonPtr(reinterpret_cast<uintptr_t>(addr), size)) return false;
        memcpy(outBuf, addr, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool SafeWriteBytes(void* addr, const void* inBuf, size_t size) {
    __try {
        if (!addr || !inBuf || !Il2CppHelpers::IsCanonPtr(reinterpret_cast<uintptr_t>(addr), size)) return false;
        memcpy(addr, inBuf, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
inline bool SafeRead(uintptr_t addr, T& outVal) {
    return Il2CppHelpers::SafeRead<T>(addr, &outVal);
}

template <typename T>
inline bool SafeWrite(uintptr_t addr, const T& val) {
    return Il2CppHelpers::SafeWrite<T>(addr, val);
}

inline bool SafeReadCStr(uintptr_t addr, char* buf, size_t maxLen) {
    return Il2CppHelpers::SafeReadCString(addr, buf, maxLen);
}

// ECS world / chunk traversal (implemented in ecs_physics.cpp)
bool ResolveGameAssembly();
void TryResolveTypeIndices();
bool GetEcsStores(EcsStores& out);
void CollectRawEcsCars(const EcsStores& stores, size_t liveCarCount, std::vector<RawEcsCar>& outCars);

// Ghost state reset hook (implemented in ecs_ghost.cpp)
void ResetPhysicsCaches();

} // namespace EcsCollisionInternal
