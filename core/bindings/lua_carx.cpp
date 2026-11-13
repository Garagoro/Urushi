#include "lua_carx.h"
#include "../ptr_registry.h"
#include "../native_carx.h"
#include "../car_sim_optimizer.h"
#include "../unity_bridge.h"
#include "../ecs_collision.h"
#include "../carfx.h"
#include <chrono>
#include <cmath>
#include <algorithm>

static auto g_startTime = std::chrono::steady_clock::now();
static auto g_lastFrameTime = std::chrono::steady_clock::now();
static float g_deltaTime = 0.016f;

static void PushMatrixField(lua_State* L, const char* fieldName, const std::array<float, 16>& m) {
    lua_newtable(L);
    for (int i = 0; i < 16; ++i) {
        lua_pushnumber(L, m[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, fieldName);
}

static void PushCarTelemetry(lua_State* L, const CarTelemetry& car) {
    lua_newtable(L);

    PtrRegistry::Push(L, car.carPtr);
    lua_setfield(L, -2, "ptr");

    lua_pushboolean(L, car.isLocalPlayer);
    lua_setfield(L, -2, "is_local_player");

    std::string unityName = UnityBridge::LookupCarUnityName(car.carPtr);
    if (!unityName.empty()) {
        lua_pushstring(L, unityName.c_str());
        lua_setfield(L, -2, "unity_name");
    }

    std::string playerName = UnityBridge::LookupCarPlayerName(car.carPtr);
    if (!playerName.empty()) {
        lua_pushstring(L, playerName.c_str());
        lua_setfield(L, -2, "player_name");
        lua_pushstring(L, playerName.c_str());
        lua_setfield(L, -2, "name");
    }

    lua_pushnumber(L, car.posX);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, car.posY);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, car.posZ);
    lua_setfield(L, -2, "z");

    lua_pushnumber(L, car.speedKmh);
    lua_setfield(L, -2, "speed_kmh");
    lua_pushnumber(L, car.speedKmh * 0.621371f);
    lua_setfield(L, -2, "speed_mph");

    lua_pushnumber(L, car.rpm);
    lua_setfield(L, -2, "rpm");

    lua_pushinteger(L, car.gear);
    lua_setfield(L, -2, "gear");

    lua_pushnumber(L, car.steerAngle);
    lua_setfield(L, -2, "steer_angle");

    lua_pushnumber(L, car.accel);
    lua_setfield(L, -2, "accel");
    lua_pushnumber(L, car.brake);
    lua_setfield(L, -2, "brake");
    lua_pushnumber(L, car.handbrake);
    lua_setfield(L, -2, "handbrake");
    lua_pushnumber(L, car.clutch);
    lua_setfield(L, -2, "clutch");

    // Matrix
    PushMatrixField(L, "matrix", car.transformMatrix);

    // Live Velocity vector
    float curVel[3] = { 0.0f, 0.0f, 0.0f };
    if (car.carPtr && NativeCarX::GetCarVelocity(car.carPtr, curVel)) {
        lua_newtable(L);
        lua_pushnumber(L, curVel[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, curVel[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, curVel[2]); lua_setfield(L, -2, "z");
        lua_setfield(L, -2, "velocity");
    }

    // Wheels
    lua_newtable(L);
    for (int w = 0; w < 4; ++w) {
        lua_newtable(L);

        PushMatrixField(L, "matrix", car.wheels[w].matrix);
        PushMatrixField(L, "caliper_matrix", car.wheels[w].caliperMatrix);

        lua_pushnumber(L, car.wheels[w].matrix[12]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, car.wheels[w].matrix[13]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, car.wheels[w].matrix[14]); lua_setfield(L, -2, "z");

        lua_pushnumber(L, car.wheels[w].smoke);
        lua_setfield(L, -2, "smoke");
        lua_pushnumber(L, car.wheels[w].smokeIntensity);
        lua_setfield(L, -2, "smoke_intensity");
        lua_pushnumber(L, car.wheels[w].camber);
        lua_setfield(L, -2, "camber");
        lua_pushnumber(L, car.wheels[w].toeIn);
        lua_setfield(L, -2, "toe_in");
        float life = std::clamp(car.wheels[w].zoneRest * 100.0f, 0.0f, 100.0f);
        float wear = std::clamp((1.0f - car.wheels[w].zoneRest) * 100.0f, 0.0f, 100.0f);
        lua_pushnumber(L, wear);
        lua_setfield(L, -2, "wear");
        lua_pushnumber(L, life);
        lua_setfield(L, -2, "life_percent");
        lua_pushnumber(L, car.wheels[w].zoneRest);
        lua_setfield(L, -2, "zone_rest");
        lua_pushnumber(L, car.wheels[w].deformAngle);
        lua_setfield(L, -2, "deform_angle");

        lua_rawseti(L, -2, w + 1);
    }
    lua_setfield(L, -2, "wheels");
}

int LuaCarX::GetPlayerCar(lua_State* L) {
    CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
    if (!player.carPtr) {
        lua_pushnil(L);
        return 1;
    }
    PushCarTelemetry(L, player);
    return 1;
}

int LuaCarX::GetAllCars(lua_State* L) {
    auto cars = NativeCarX::GetAllCarsTelemetry();
    lua_newtable(L);
    for (size_t i = 0; i < cars.size(); ++i) {
        PushCarTelemetry(L, cars[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int LuaCarX::GetPlayerList(lua_State* L) {
    auto list = UnityBridge::GetLiveCarList();
    lua_newtable(L);
    for (size_t i = 0; i < list.size(); ++i) {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        lua_setfield(L, -2, "index");
        PtrRegistry::Push(L, list[i].carPtr);
        lua_setfield(L, -2, "ptr");
        lua_pushstring(L, list[i].unityName.c_str());
        lua_setfield(L, -2, "unity_name");
        lua_pushstring(L, list[i].playerName.c_str());
        lua_setfield(L, -2, "player_name");
        lua_pushstring(L, list[i].playerName.empty() ? list[i].unityName.c_str() : list[i].playerName.c_str());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, list[i].isLocalPlayer);
        lua_setfield(L, -2, "is_local_player");
        lua_pushnumber(L, list[i].x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, list[i].y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, list[i].z);
        lua_setfield(L, -2, "z");
        lua_pushnumber(L, list[i].speedKmh);
        lua_setfield(L, -2, "speed_kmh");
        lua_pushboolean(L, list[i].isWrongWay);
        lua_setfield(L, -2, "is_wrong_way");
        lua_pushboolean(L, list[i].isSelectivelyGhosted);
        lua_setfield(L, -2, "is_selectively_ghosted");
        lua_pushboolean(L, list[i].isGameGhosted);
        lua_setfield(L, -2, "is_game_ghosted");
        lua_pushinteger(L, list[i].ghostOverride);
        lua_setfield(L, -2, "ghost_mode");
        lua_pushnumber(L, list[i].headingDot);
        lua_setfield(L, -2, "heading_dot");
        lua_pushnumber(L, list[i].distToPlayer);
        lua_setfield(L, -2, "dist_to_player");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int LuaCarX::IsInSession(lua_State* L) {
    lua_pushboolean(L, NativeCarX::IsInSession());
    return 1;
}

int LuaCarX::SetCarRenderMatrix(lua_State* L) {
    if (!lua_islightuserdata(L, 1) || !lua_istable(L, 2)) {
        lua_pushboolean(L, false);
        return 1;
    }

    void* carPtr = PtrRegistry::Arg(L, 1);
    float matrix[16];
    for (int i = 0; i < 16; ++i) {
        lua_rawgeti(L, 2, i + 1);
        matrix[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    NativeCarX::SetCarRenderMatrix(carPtr, matrix, 1.0f);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::SetCarActive(lua_State* L) {
    if (!lua_islightuserdata(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    void* carPtr = PtrRegistry::Arg(L, 1);
    bool active = lua_toboolean(L, 2) != 0;
    NativeCarX::SetCarActive(carPtr, active);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::SetCarRPM(lua_State* L) {
    if (!lua_islightuserdata(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    void* carPtr = PtrRegistry::Arg(L, 1);
    float rpm = static_cast<float>(lua_tonumber(L, 2));
    NativeCarX::SetCarRPM(carPtr, rpm);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::SetCarGear(lua_State* L) {
    if (!lua_islightuserdata(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    void* carPtr = PtrRegistry::Arg(L, 1);
    int gear = static_cast<int>(lua_tointeger(L, 2));
    NativeCarX::SetCarGear(carPtr, gear);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::SpawnCar(lua_State* L) {
    void* car = NativeCarX::SpawnCar();
    if (!car) {
        lua_pushnil(L);
        return 1;
    }
    PtrRegistry::Push(L, car);
    return 1;
}

int LuaCarX::DestroyCar(lua_State* L) {
    if (!lua_islightuserdata(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    void* carPtr = PtrRegistry::Arg(L, 1);
    bool ok = NativeCarX::DestroyCar(carPtr);
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::SpawnGhost(lua_State* L) {
    void* ghost = UnityBridge::SpawnGhostVisual();
    if (ghost) {
        PtrRegistry::Push(L, ghost);
        return 1;
    }
    if (UnityBridge::HasGhostVisual()) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaCarX::DestroyGhost(lua_State* L) {
    bool ok = UnityBridge::DestroyGhostVisual();
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::HasGhost(lua_State* L) {
    bool has = UnityBridge::HasGhostVisual();
    lua_pushboolean(L, has);
    return 1;
}

int LuaCarX::GetGhost(lua_State* L) {
    void* ghost = UnityBridge::GetGhostVisualGameObject();
    if (!ghost) {
        lua_pushnil(L);
        return 1;
    }
    PtrRegistry::Push(L, ghost);
    return 1;
}

int LuaCarX::SetGhostMirror(lua_State* L) {
    bool enabled = lua_toboolean(L, 1) != 0;
    float offsetRight = (lua_gettop(L) >= 2 && lua_isnumber(L, 2))
        ? static_cast<float>(lua_tonumber(L, 2))
        : 3.8f;
    UnityBridge::SetGhostMirrorMode(enabled, offsetRight);
    lua_pushboolean(L, UnityBridge::IsGhostMirrorMode());
    return 1;
}

int LuaCarX::IsGhostMirror(lua_State* L) {
    lua_pushboolean(L, UnityBridge::IsGhostMirrorMode());
    return 1;
}

int LuaCarX::SetGhostCollision(lua_State* L) {
    bool enabled = lua_toboolean(L, 1) != 0;
    UnityBridge::SetGhostCollisionEnabled(enabled);
    lua_pushboolean(L, UnityBridge::IsGhostCollisionEnabled());
    return 1;
}

int LuaCarX::IsGhostCollision(lua_State* L) {
    lua_pushboolean(L, UnityBridge::IsGhostCollisionEnabled());
    return 1;
}

int LuaCarX::GetGhostCount(lua_State* L) {
    lua_pushinteger(L, UnityBridge::GetGhostCount());
    return 1;
}

static void PushSelectiveGhostConfig(lua_State* L, const SelectiveGhostConfig& cfg) {
    lua_newtable(L);
    lua_pushboolean(L, cfg.enabled);
    lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, cfg.antiWrongWay);
    lua_setfield(L, -2, "anti_wrong_way");
    lua_pushnumber(L, cfg.minSpeedKmh);
    lua_setfield(L, -2, "min_speed_kmh");
    lua_pushnumber(L, cfg.dotThreshold);
    lua_setfield(L, -2, "dot_threshold");
    lua_pushnumber(L, cfg.maxDistanceMeters);
    lua_setfield(L, -2, "max_distance");
    lua_pushboolean(L, cfg.showHudAlert);
    lua_setfield(L, -2, "show_hud_alert");
}

int LuaCarX::SetSelectiveGhostConfig(lua_State* L) {
    SelectiveGhostConfig cfg = EcsCollision::GetConfig();
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "enabled");
        if (!lua_isnil(L, -1)) cfg.enabled = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "anti_wrong_way");
        if (!lua_isnil(L, -1)) cfg.antiWrongWay = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, 1, "min_speed_kmh");
        if (lua_isnumber(L, -1)) cfg.minSpeedKmh = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "dot_threshold");
        if (lua_isnumber(L, -1)) cfg.dotThreshold = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "max_distance");
        if (lua_isnumber(L, -1)) cfg.maxDistanceMeters = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "show_hud_alert");
        if (!lua_isnil(L, -1)) cfg.showHudAlert = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    } else if (lua_isboolean(L, 1)) {
        cfg.enabled = lua_toboolean(L, 1) != 0;
    }
    EcsCollision::SetConfig(cfg);
    PushSelectiveGhostConfig(L, EcsCollision::GetConfig());
    return 1;
}

int LuaCarX::GetSelectiveGhostConfig(lua_State* L) {
    PushSelectiveGhostConfig(L, EcsCollision::GetConfig());
    return 1;
}

int LuaCarX::SetCarGhostMode(lua_State* L) {
    std::string unityName;
    uintptr_t carPtr = 0;

    if (lua_islightuserdata(L, 1)) {
        carPtr = reinterpret_cast<uintptr_t>(PtrRegistry::Arg(L, 1));
        unityName = UnityBridge::LookupCarUnityName(reinterpret_cast<void*>(carPtr));
    } else if (lua_isstring(L, 1)) {
        unityName = lua_tostring(L, 1);
    } else if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "ptr");
        if (lua_islightuserdata(L, -1)) carPtr = reinterpret_cast<uintptr_t>(PtrRegistry::Arg(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "unity_name");
        if (lua_isstring(L, -1)) unityName = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    CarGhostOverride mode = CarGhostOverride::Auto;
    if (lua_isinteger(L, 2) || lua_isnumber(L, 2)) {
        int m = static_cast<int>(lua_tointeger(L, 2));
        if (m == 1) mode = CarGhostOverride::ForceGhost;
        else if (m == 2) mode = CarGhostOverride::ForceSolid;
    } else if (lua_isstring(L, 2)) {
        const char* s = lua_tostring(L, 2);
        if (s) {
            if (_stricmp(s, "ghost") == 0) mode = CarGhostOverride::ForceGhost;
            else if (_stricmp(s, "solid") == 0) mode = CarGhostOverride::ForceSolid;
        }
    }

    if (unityName.empty() && carPtr == 0) {
        lua_pushboolean(L, false);
        return 1;
    }

    EcsCollision::SetCarOverride(unityName, carPtr, mode);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::ClearCarGhostModes(lua_State* L) {
    EcsCollision::ClearOverrides();
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::GetRigidBody(lua_State* L) {
    TeleportPose pose{};
    std::string carName;
    bool hasPose = UnityBridge::GetPlayerCurrentPose(pose, &carName);

    CarTelemetry player = NativeCarX::GetPlayerCarTelemetry();
    if (!hasPose && !player.carPtr) {
        lua_pushnil(L);
        return 1;
    }

    if (!pose.hasRb && player.carPtr) {
        if (NativeCarX::GetCarRigidBody(player.carPtr, pose.rb)) {
            pose.hasRb = true;
            NativeCarX::GetCarVelocity(player.carPtr, pose.linVel);
            NativeCarX::GetCarAngularVelocity(player.carPtr, pose.angVel);
            if (!hasPose) {
                pose.pos[0] = pose.rb[20];
                pose.pos[1] = pose.rb[21];
                pose.pos[2] = pose.rb[22];
                hasPose = true;
            }
        }
    }

    if (!hasPose && !pose.hasRb) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);

    if (pose.hasRb) {
        lua_newtable(L);
        for (int i = 0; i < 29; ++i) {
            lua_pushnumber(L, pose.rb[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "rb");
    }

    lua_pushnumber(L, pose.pos[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, pose.pos[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, pose.pos[2]); lua_setfield(L, -2, "z");

    if (pose.hasQuat) {
        lua_pushnumber(L, pose.rot[0]); lua_setfield(L, -2, "qx");
        lua_pushnumber(L, pose.rot[1]); lua_setfield(L, -2, "qy");
        lua_pushnumber(L, pose.rot[2]); lua_setfield(L, -2, "qz");
        lua_pushnumber(L, pose.rot[3]); lua_setfield(L, -2, "qw");
    }

    if (!carName.empty()) {
        lua_pushstring(L, carName.c_str());
        lua_setfield(L, -2, "unity_name");
    }

    lua_pushnumber(L, player.speedKmh); lua_setfield(L, -2, "speed_kmh");
    lua_pushnumber(L, player.rpm); lua_setfield(L, -2, "rpm");
    lua_pushinteger(L, player.gear); lua_setfield(L, -2, "gear");

    lua_newtable(L);
    for (int i = 0; i < 3; ++i) { lua_pushnumber(L, pose.linVel[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "vel");

    lua_newtable(L);
    for (int i = 0; i < 3; ++i) { lua_pushnumber(L, pose.angVel[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "ang_vel");

    return 1;
}

static bool ReadLuaVecField(lua_State* L, int tableIdx, const char* name, float out[3]) {
    lua_getfield(L, tableIdx, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, -1, i + 1);
        out[i] = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

static bool ReadLuaFloatField(lua_State* L, int tableIdx, const char* name, float& outVal) {
    lua_getfield(L, tableIdx, name);
    bool ok = lua_isnumber(L, -1) != 0;
    if (ok) {
        outVal = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return ok;
}

int LuaCarX::TeleportRigidBody(lua_State* L) {
    int top = lua_gettop(L);
    TeleportPose pose{};

    // 1. Positional numbers signature: carx.teleport(x, y, z, [yaw_deg], [vel_mode])
    if (top >= 3 && lua_isnumber(L, 1) && lua_isnumber(L, 2) && lua_isnumber(L, 3)) {
        pose.pos[0] = static_cast<float>(lua_tonumber(L, 1));
        pose.pos[1] = static_cast<float>(lua_tonumber(L, 2));
        pose.pos[2] = static_cast<float>(lua_tonumber(L, 3));
        pose.rb[20] = pose.pos[0];
        pose.rb[21] = pose.pos[1];
        pose.rb[22] = pose.pos[2];

        if (top >= 4 && lua_isnumber(L, 4)) {
            float yawDeg = static_cast<float>(lua_tonumber(L, 4));
            float halfRad = (yawDeg * 0.0174532925f) * 0.5f;
            pose.rot[0] = 0.0f;
            pose.rot[1] = sinf(halfRad);
            pose.rot[2] = 0.0f;
            pose.rot[3] = cosf(halfRad);
            pose.hasQuat = true;
        }

        int velMode = 0;
        if (top >= 5 && lua_isnumber(L, 5)) velMode = static_cast<int>(lua_tointeger(L, 5));
        bool ok = UnityBridge::RequestTeleportPlayerPose(pose, velMode);
        lua_pushboolean(L, ok);
        return 1;
    }

    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }

    // Check for nested `.rb` array or direct 29-element array in arg 1
    int rbTableIdx = 0;
    bool pushedSub = false;
    lua_getfield(L, 1, "rb");
    if (lua_istable(L, -1)) {
        rbTableIdx = lua_gettop(L);
        pushedSub = true;
    } else {
        lua_pop(L, 1);
        lua_rawgeti(L, 1, 1);
        if (lua_isnumber(L, -1)) {
            rbTableIdx = 1;
        }
        lua_pop(L, 1);
    }

    if (rbTableIdx != 0) {
        for (int i = 0; i < 29; ++i) {
            lua_rawgeti(L, rbTableIdx, i + 1);
            pose.rb[i] = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
        }
        pose.hasRb = true;
        pose.pos[0] = pose.rb[20]; pose.pos[1] = pose.rb[21]; pose.pos[2] = pose.rb[22];
        pose.linVel[0] = pose.rb[23]; pose.linVel[1] = pose.rb[24]; pose.linVel[2] = pose.rb[25];
        pose.angVel[0] = pose.rb[26]; pose.angVel[1] = pose.rb[27]; pose.angVel[2] = pose.rb[28];
        if (pushedSub) lua_pop(L, 1);
    }

    // Explicit position / quaternion / velocity overrides from snapshot or synthetic table
    bool hasX = ReadLuaFloatField(L, 1, "x", pose.pos[0]);
    bool hasY = ReadLuaFloatField(L, 1, "y", pose.pos[1]);
    bool hasZ = ReadLuaFloatField(L, 1, "z", pose.pos[2]);
    if (!hasX && !hasY && !hasZ && !pose.hasRb) {
        lua_pushboolean(L, false);
        return 1;
    }

    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    if (ReadLuaFloatField(L, 1, "qx", qx) &&
        ReadLuaFloatField(L, 1, "qy", qy) &&
        ReadLuaFloatField(L, 1, "qz", qz) &&
        ReadLuaFloatField(L, 1, "qw", qw)) {
        pose.rot[0] = qx;
        pose.rot[1] = qy;
        pose.rot[2] = qz;
        pose.rot[3] = qw;
        pose.hasQuat = true;
    } else {
        float yawDeg = 0.0f;
        if (ReadLuaFloatField(L, 1, "yaw", yawDeg) || ReadLuaFloatField(L, 1, "yaw_deg", yawDeg)) {
            float halfRad = (yawDeg * 0.0174532925f) * 0.5f;
            pose.rot[0] = 0.0f;
            pose.rot[1] = sinf(halfRad);
            pose.rot[2] = 0.0f;
            pose.rot[3] = cosf(halfRad);
            pose.hasQuat = true;
        }
    }

    int velMode = 0;
    if (ReadLuaVecField(L, 1, "vel", pose.linVel) || ReadLuaVecField(L, 1, "velocity", pose.linVel)) {
        pose.rb[23] = pose.linVel[0];
        pose.rb[24] = pose.linVel[1];
        pose.rb[25] = pose.linVel[2];
        velMode = 1;
    } else {
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        bool hasVx = ReadLuaFloatField(L, 1, "vx", vx);
        bool hasVy = ReadLuaFloatField(L, 1, "vy", vy);
        bool hasVz = ReadLuaFloatField(L, 1, "vz", vz);
        if (hasVx || hasVy || hasVz) {
            pose.linVel[0] = vx; pose.linVel[1] = vy; pose.linVel[2] = vz;
            pose.rb[23] = vx; pose.rb[24] = vy; pose.rb[25] = vz;
            velMode = 1;
        }
    }

    if (ReadLuaVecField(L, 1, "ang_vel", pose.angVel) || ReadLuaVecField(L, 1, "angular_velocity", pose.angVel)) {
        pose.rb[26] = pose.angVel[0];
        pose.rb[27] = pose.angVel[1];
        pose.rb[28] = pose.angVel[2];
    }

    if (lua_type(L, 2) == LUA_TNUMBER) {
        velMode = static_cast<int>(lua_tointeger(L, 2));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 2);
        if (s) {
            if (_stricmp(s, "saved") == 0 || _stricmp(s, "custom") == 0) velMode = 1;
            else if (_stricmp(s, "keep") == 0) velMode = 2;
        }
    }

    bool ok = UnityBridge::RequestTeleportPlayerPose(pose, velMode);
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::GetVelocity(lua_State* L) {
    void* car = nullptr;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
    } else {
        car = NativeCarX::GetPlayerCarPtr();
    }
    float vel[3] = { 0.0f, 0.0f, 0.0f };
    if (car && NativeCarX::GetCarVelocity(car, vel)) {
        lua_newtable(L);
        lua_pushnumber(L, vel[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, vel[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, vel[2]); lua_setfield(L, -2, "z");
        float len = sqrtf(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);
        lua_pushnumber(L, len); lua_setfield(L, -2, "length");
        lua_pushnumber(L, len * 3.6f); lua_setfield(L, -2, "speed_kmh");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaCarX::SetVelocity(lua_State* L) {
    int top = lua_gettop(L);
    void* car = nullptr;
    int argStart = 1;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
        argStart = 2;
    } else {
        car = NativeCarX::GetPlayerCarPtr();
    }
    if (!car) {
        lua_pushboolean(L, false);
        return 1;
    }
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    if (lua_istable(L, argStart)) {
        lua_getfield(L, argStart, "x");
        if (lua_isnumber(L, -1)) vx = static_cast<float>(lua_tonumber(L, -1));
        else {
            lua_pop(L, 1);
            lua_rawgeti(L, argStart, 1);
            if (lua_isnumber(L, -1)) vx = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, argStart, "y");
        if (lua_isnumber(L, -1)) vy = static_cast<float>(lua_tonumber(L, -1));
        else {
            lua_pop(L, 1);
            lua_rawgeti(L, argStart, 2);
            if (lua_isnumber(L, -1)) vy = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, argStart, "z");
        if (lua_isnumber(L, -1)) vz = static_cast<float>(lua_tonumber(L, -1));
        else {
            lua_pop(L, 1);
            lua_rawgeti(L, argStart, 3);
            if (lua_isnumber(L, -1)) vz = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
    } else if (top >= argStart + 2) {
        vx = static_cast<float>(lua_tonumber(L, argStart));
        vy = static_cast<float>(lua_tonumber(L, argStart + 1));
        vz = static_cast<float>(lua_tonumber(L, argStart + 2));
    } else {
        lua_pushboolean(L, false);
        return 1;
    }

    float rb[29] = { 0 };
    if (!NativeCarX::GetCarRigidBody(car, rb)) {
        lua_pushboolean(L, false);
        return 1;
    }
    rb[23] = vx;
    rb[24] = vy;
    rb[25] = vz;
    bool ok = NativeCarX::SetCarRigidBody(car, rb);
    if (car == NativeCarX::GetPlayerCarPtr()) {
        UnityBridge::RequestTeleportPlayerRigidBody(rb, 1);
    }
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::GetAngularVelocity(lua_State* L) {
    void* car = nullptr;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
    } else {
        car = NativeCarX::GetPlayerCarPtr();
    }
    float angVel[3] = { 0.0f, 0.0f, 0.0f };
    if (car && NativeCarX::GetCarAngularVelocity(car, angVel)) {
        lua_newtable(L);
        lua_pushnumber(L, angVel[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, angVel[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, angVel[2]); lua_setfield(L, -2, "z");
        float len = sqrtf(angVel[0]*angVel[0] + angVel[1]*angVel[1] + angVel[2]*angVel[2]);
        lua_pushnumber(L, len); lua_setfield(L, -2, "length");
        lua_pushnumber(L, angVel[1] * 57.2957795f); lua_setfield(L, -2, "yaw_rate_deg");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaCarX::SetAngularVelocity(lua_State* L) {
    int top = lua_gettop(L);
    void* car = nullptr;
    int argStart = 1;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
        argStart = 2;
    } else {
        car = NativeCarX::GetPlayerCarPtr();
    }
    if (!car) {
        lua_pushboolean(L, false);
        return 1;
    }
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    if (lua_istable(L, argStart)) {
        lua_getfield(L, argStart, "x");
        if (lua_isnumber(L, -1)) wx = static_cast<float>(lua_tonumber(L, -1));
        else {
            lua_pop(L, 1);
            lua_rawgeti(L, argStart, 1);
            if (lua_isnumber(L, -1)) wx = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, argStart, "y");
        if (lua_isnumber(L, -1)) wy = static_cast<float>(lua_tonumber(L, -1));
        else {
            lua_pop(L, 1);
            lua_rawgeti(L, argStart, 2);
            if (lua_isnumber(L, -1)) wy = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, argStart, "z");
        if (lua_isnumber(L, -1)) wz = static_cast<float>(lua_tonumber(L, -1));
        else {
            lua_pop(L, 1);
            lua_rawgeti(L, argStart, 3);
            if (lua_isnumber(L, -1)) wz = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
    } else if (top >= argStart + 2) {
        wx = static_cast<float>(lua_tonumber(L, argStart));
        wy = static_cast<float>(lua_tonumber(L, argStart + 1));
        wz = static_cast<float>(lua_tonumber(L, argStart + 2));
    } else {
        lua_pushboolean(L, false);
        return 1;
    }

    float rb[29] = { 0 };
    if (!NativeCarX::GetCarRigidBody(car, rb)) {
        lua_pushboolean(L, false);
        return 1;
    }
    rb[26] = wx;
    rb[27] = wy;
    rb[28] = wz;
    bool ok = NativeCarX::SetCarRigidBody(car, rb);
    if (car == NativeCarX::GetPlayerCarPtr()) {
        UnityBridge::RequestTeleportPlayerRigidBody(rb, 1);
    }
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::ResetCar(lua_State* L) {
    void* car = nullptr;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
    } else {
        car = NativeCarX::GetPlayerCarPtr();
    }
    if (!car) {
        lua_pushboolean(L, false);
        return 1;
    }
    float rb[29] = { 0 };
    if (!NativeCarX::GetCarRigidBody(car, rb)) {
        lua_pushboolean(L, false);
        return 1;
    }
    // Zero out velocities
    rb[23] = 0.0f; rb[24] = 0.0f; rb[25] = 0.0f;
    rb[26] = 0.0f; rb[27] = 0.0f; rb[28] = 0.0f;

    // Upright orientation: keep yaw heading, eliminate roll & pitch
    float fx = rb[8], fz = rb[10];
    float fLen = sqrtf(fx * fx + fz * fz);
    if (fLen > 1e-4f) {
        fx /= fLen; fz /= fLen;
        float rx = fz, rz = -fx;
        rb[0] = rx; rb[1] = 0.0f; rb[2] = rz;
        rb[4] = 0.0f; rb[5] = 1.0f; rb[6] = 0.0f;
        rb[8] = fx; rb[9] = 0.0f; rb[10] = fz;
    }
    // Lift slightly (0.35m) so wheels clear ground collision
    rb[21] += 0.35f;

    bool ok = NativeCarX::SetCarRigidBody(car, rb);
    if (car == NativeCarX::GetPlayerCarPtr()) {
        UnityBridge::RequestTeleportPlayerRigidBody(rb, 0);
    }
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::GetDriftInfo(lua_State* L) {
    void* car = nullptr;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
    } else {
        car = NativeCarX::GetPlayerCarPtr();
    }
    if (!car) {
        lua_pushnil(L);
        return 1;
    }
    CarTelemetry tele{};
    if (car == NativeCarX::GetPlayerCarPtr()) {
        tele = NativeCarX::GetPlayerCarTelemetry();
    } else {
        auto all = NativeCarX::GetAllCarsTelemetry();
        for (const auto& c : all) {
            if (c.carPtr == car) { tele = c; break; }
        }
    }
    const float* m = tele.transformMatrix.data();
    float rLen = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float fLen = sqrtf(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    float rx = (rLen > 1e-4f) ? (m[0] / rLen) : 1.0f;
    float ry = (rLen > 1e-4f) ? (m[1] / rLen) : 0.0f;
    float rz = (rLen > 1e-4f) ? (m[2] / rLen) : 0.0f;

    float fx = (fLen > 1e-4f) ? (m[8] / fLen) : 0.0f;
    float fy = (fLen > 1e-4f) ? (m[9] / fLen) : 0.0f;
    float fz = (fLen > 1e-4f) ? (m[10] / fLen) : 1.0f;

    float vel[3] = { 0 };
    NativeCarX::GetCarVelocity(car, vel);

    float angVel[3] = { 0 };
    NativeCarX::GetCarAngularVelocity(car, angVel);

    float vFwd = vel[0] * fx + vel[1] * fy + vel[2] * fz;
    float vSide = vel[0] * rx + vel[1] * ry + vel[2] * rz;
    float vPlanar = sqrtf(vFwd * vFwd + vSide * vSide);
    float speedKmh = vPlanar * 3.6f;

    float driftAngleDeg = (vPlanar > 0.4f) ? (atan2f(vSide, vFwd) * 57.2957795f) : 0.0f;
    float absAngleDeg = fabsf(driftAngleDeg);
    bool isDrifting = (absAngleDeg >= 8.0f && speedKmh >= 12.0f);

    lua_newtable(L);
    lua_pushnumber(L, driftAngleDeg); lua_setfield(L, -2, "angle");
    lua_pushnumber(L, absAngleDeg); lua_setfield(L, -2, "abs_angle");
    lua_pushnumber(L, speedKmh); lua_setfield(L, -2, "speed_kmh");
    lua_pushnumber(L, vFwd); lua_setfield(L, -2, "forward_speed");
    lua_pushnumber(L, vSide); lua_setfield(L, -2, "lateral_speed");
    lua_pushboolean(L, isDrifting); lua_setfield(L, -2, "is_drifting");
    lua_pushnumber(L, angVel[1] * 57.2957795f); lua_setfield(L, -2, "yaw_rate_deg");

    lua_newtable(L);
    lua_pushnumber(L, vel[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, vel[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, vel[2]); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "velocity");

    lua_newtable(L);
    lua_pushnumber(L, angVel[0]); lua_setfield(L, -2, "x");
    lua_pushnumber(L, angVel[1]); lua_setfield(L, -2, "y");
    lua_pushnumber(L, angVel[2]); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "angular_velocity");

    lua_newtable(L);
    lua_pushnumber(L, fx); lua_setfield(L, -2, "x");
    lua_pushnumber(L, fy); lua_setfield(L, -2, "y");
    lua_pushnumber(L, fz); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "heading");

    lua_newtable(L);
    lua_pushnumber(L, rx); lua_setfield(L, -2, "x");
    lua_pushnumber(L, ry); lua_setfield(L, -2, "y");
    lua_pushnumber(L, rz); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "right");

    return 1;
}

int LuaCarX::GetCamera(lua_State* L) {
    UnityVector3 camPos{};
    UnityVector4 camRot{ 0.0f, 0.0f, 0.0f, 1.0f };
    if (!UnityBridge::GetMainCameraPose(camPos, camRot)) {
        lua_pushnil(L);
        return 1;
    }
    UnityVector3 right{ 1, 0, 0 }, up{ 0, 1, 0 }, fwd{ 0, 0, 1 };
    UnityBridge::QuatToAxes(camRot, right, up, fwd);

    lua_newtable(L);
    lua_pushnumber(L, camPos.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, camPos.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, camPos.z); lua_setfield(L, -2, "z");

    lua_pushnumber(L, camRot.x); lua_setfield(L, -2, "qx");
    lua_pushnumber(L, camRot.y); lua_setfield(L, -2, "qy");
    lua_pushnumber(L, camRot.z); lua_setfield(L, -2, "qz");
    lua_pushnumber(L, camRot.w); lua_setfield(L, -2, "qw");

    lua_newtable(L);
    lua_pushnumber(L, fwd.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, fwd.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, fwd.z); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "fwd");

    lua_newtable(L);
    lua_pushnumber(L, right.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, right.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, right.z); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "right");

    lua_newtable(L);
    lua_pushnumber(L, up.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, up.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, up.z); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "up");

    return 1;
}

int LuaCarX::Raycast(lua_State* L) {
    UnityVector3 orig{ 0.0f, 0.0f, 0.0f };
    UnityVector3 dir{ 0.0f, -1.0f, 0.0f };
    float maxDist = 100.0f;
    int layerMask = -1;

    int top = lua_gettop(L);
    if (lua_istable(L, 1) && lua_istable(L, 2)) {
        lua_getfield(L, 1, "x"); if (lua_isnumber(L, -1)) orig.x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "y"); if (lua_isnumber(L, -1)) orig.y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "z"); if (lua_isnumber(L, -1)) orig.z = (float)lua_tonumber(L, -1); lua_pop(L, 1);

        lua_getfield(L, 2, "x"); if (lua_isnumber(L, -1)) dir.x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "y"); if (lua_isnumber(L, -1)) dir.y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "z"); if (lua_isnumber(L, -1)) dir.z = (float)lua_tonumber(L, -1); lua_pop(L, 1);

        if (top >= 3 && lua_isnumber(L, 3)) maxDist = (float)lua_tonumber(L, 3);
        if (top >= 4 && lua_isnumber(L, 4)) layerMask = (int)lua_tointeger(L, 4);
    } else if (top >= 6) {
        orig.x = (float)luaL_checknumber(L, 1);
        orig.y = (float)luaL_checknumber(L, 2);
        orig.z = (float)luaL_checknumber(L, 3);
        dir.x  = (float)luaL_checknumber(L, 4);
        dir.y  = (float)luaL_checknumber(L, 5);
        dir.z  = (float)luaL_checknumber(L, 6);
        if (top >= 7 && lua_isnumber(L, 7)) maxDist = (float)lua_tonumber(L, 7);
        if (top >= 8 && lua_isnumber(L, 8)) layerMask = (int)lua_tointeger(L, 8);
    } else {
        lua_pushnil(L);
        return 1;
    }

    UnityRaycastHit hit{};
    bool hasHit = UnityBridge::RaycastWorld(orig, dir, maxDist, hit, layerMask);

    lua_newtable(L);
    lua_pushboolean(L, hasHit);
    lua_setfield(L, -2, "hit");

    if (hasHit) {
        lua_pushnumber(L, hit.point.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, hit.point.y); lua_setfield(L, -2, "y");
        lua_pushnumber(L, hit.point.z); lua_setfield(L, -2, "z");

        lua_pushnumber(L, hit.normal.x); lua_setfield(L, -2, "nx");
        lua_pushnumber(L, hit.normal.y); lua_setfield(L, -2, "ny");
        lua_pushnumber(L, hit.normal.z); lua_setfield(L, -2, "nz");

        lua_pushnumber(L, hit.distance); lua_setfield(L, -2, "distance");
        lua_pushinteger(L, hit.colliderId); lua_setfield(L, -2, "collider_id");
    }
    return 1;
}

int LuaCarX::GetProximity(lua_State* L) {
    int target = static_cast<int>(luaL_optinteger(L, 1, CarFX::kTargetBody));
    CarFX::SpatialProximityInput sp = CarFX::GetSpatialProximity(target);

    auto pushVec4 = [&](const char* k, const float v[4], const char* wKey) {
        lua_newtable(L);
        lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
        lua_pushnumber(L, v[3]); lua_setfield(L, -2, wKey ? wKey : "w");
        lua_setfield(L, -2, k);
    };

    lua_newtable(L);
    pushVec4("car_pos", sp.carPos, "half_w");
    pushVec4("car_right", sp.carRight, "half_l");
    pushVec4("car_up", sp.carUp, "height");
    pushVec4("car_fwd", sp.carFwd, "valid");
    pushVec4("cam_pos", sp.camPos, "valid");
    pushVec4("near_car_pos", sp.nearCarPos, "gap");
    pushVec4("near_car_closest", sp.nearCarClosest, "half_h");

    lua_pushnumber(L, sp.wallDirDist[0]); lua_setfield(L, -2, "wall_front");
    lua_pushnumber(L, sp.wallDirDist[1]); lua_setfield(L, -2, "wall_rear");
    lua_pushnumber(L, sp.wallDirDist[2]); lua_setfield(L, -2, "wall_left");
    lua_pushnumber(L, sp.wallDirDist[3]); lua_setfield(L, -2, "wall_right");
    lua_pushnumber(L, sp.wallDoorLeftHit[3]); lua_setfield(L, -2, "wall_door_left");
    lua_pushnumber(L, sp.wallDoorRightHit[3]); lua_setfield(L, -2, "wall_door_right");
    lua_pushnumber(L, sp.wallFrontLeftHit[3]); lua_setfield(L, -2, "wall_front_left");
    lua_pushnumber(L, sp.wallFrontRightHit[3]); lua_setfield(L, -2, "wall_front_right");
    lua_pushnumber(L, sp.wallRearLeftHit[3]); lua_setfield(L, -2, "wall_rear_left");
    lua_pushnumber(L, sp.wallRearRightHit[3]); lua_setfield(L, -2, "wall_rear_right");
    pushVec4("wall_nearest", sp.wallNearestHit, "gap");
    pushVec4("wall_front_hit", sp.wallFrontHit, "gap");
    pushVec4("wall_rear_hit", sp.wallRearHit, "gap");
    pushVec4("wall_left_hit", sp.wallLeftHit, "gap");
    pushVec4("wall_right_hit", sp.wallRightHit, "gap");
    pushVec4("wall_door_left_hit", sp.wallDoorLeftHit, "gap");
    pushVec4("wall_door_right_hit", sp.wallDoorRightHit, "gap");
    pushVec4("wall_front_left_hit", sp.wallFrontLeftHit, "gap");
    pushVec4("wall_front_right_hit", sp.wallFrontRightHit, "gap");
    pushVec4("wall_rear_left_hit", sp.wallRearLeftHit, "gap");
    pushVec4("wall_rear_right_hit", sp.wallRearRightHit, "gap");
    lua_pushnumber(L, sp.driftState[0]); lua_setfield(L, -2, "drift_deg");
    lua_pushnumber(L, sp.driftState[1]); lua_setfield(L, -2, "drift_signed_deg");
    lua_pushnumber(L, sp.driftState[2]); lua_setfield(L, -2, "drift_norm65");
    lua_pushnumber(L, sp.driftState[3]); lua_setfield(L, -2, "speed_kmh");
    return 1;
}

int LuaCarX::GetTime(lua_State* L) {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_startTime).count();
    lua_pushnumber(L, elapsed);
    return 1;
}

int LuaCarX::GetDeltaTime(lua_State* L) {
    auto now = std::chrono::steady_clock::now();
    g_deltaTime = std::chrono::duration<float>(now - g_lastFrameTime).count();
    g_lastFrameTime = now;
    lua_pushnumber(L, g_deltaTime);
    return 1;
}

int LuaCarX::SetNoDamage(lua_State* L) {
    bool enable = lua_toboolean(L, 1) != 0;
    NativeCarX::SetNoDamage(enable);
    CarSimOptimizer::SetNoDamageOverride(enable);
    return 0;
}

int LuaCarX::GetNoDamage(lua_State* L) {
    lua_pushboolean(L, NativeCarX::IsNoDamage());
    return 1;
}

int LuaCarX::SetNoTireWear(lua_State* L) {
    bool enable = lua_toboolean(L, 1) != 0;
    NativeCarX::SetNoTireWear(enable);
    CarSimOptimizer::SetNoTireWearOverride(enable);
    return 0;
}

int LuaCarX::GetNoTireWear(lua_State* L) {
    lua_pushboolean(L, NativeCarX::IsNoTireWear());
    return 1;
}

int LuaCarX::RepairCar(lua_State* L) {
    void* car = nullptr;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
    }
    NativeCarX::ResetCarDamage(car);
    NativeCarX::ResetTireWear(car);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::ResetTireWear(lua_State* L) {
    void* car = nullptr;
    if (lua_islightuserdata(L, 1)) {
        car = PtrRegistry::Arg(L, 1);
    }
    NativeCarX::ResetTireWear(car);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::GetTyreConfig(lua_State* L) {
    TyreConfig cfg = NativeCarX::GetTyreConfig();
    lua_newtable(L);

    lua_pushboolean(L, cfg.infiniteWear);
    lua_setfield(L, -2, "infinite_wear");

    lua_pushnumber(L, cfg.wearMultiplier);
    lua_setfield(L, -2, "wear_multiplier");

    lua_pushnumber(L, cfg.desiredLife);
    lua_setfield(L, -2, "desired_life");

    lua_pushboolean(L, cfg.overrideGrip);
    lua_setfield(L, -2, "override_grip");

    lua_pushnumber(L, cfg.frontGrip);
    lua_setfield(L, -2, "front_grip");

    lua_pushnumber(L, cfg.rearGrip);
    lua_setfield(L, -2, "rear_grip");

    lua_pushnumber(L, cfg.ballisticGrip);
    lua_setfield(L, -2, "ballistic_grip");

    lua_pushboolean(L, cfg.lockTemperature);
    lua_setfield(L, -2, "lock_temp");

    lua_pushnumber(L, cfg.targetTemperature);
    lua_setfield(L, -2, "target_temp");

    lua_pushboolean(L, cfg.overridePressure);
    lua_setfield(L, -2, "override_pressure");

    lua_pushnumber(L, cfg.frontPressure);
    lua_setfield(L, -2, "front_pressure");

    lua_pushnumber(L, cfg.rearPressure);
    lua_setfield(L, -2, "rear_pressure");

    lua_pushnumber(L, cfg.baselineFrontPressure);
    lua_setfield(L, -2, "baseline_front_pressure");

    lua_pushnumber(L, cfg.baselineRearPressure);
    lua_setfield(L, -2, "baseline_rear_pressure");

    return 1;
}

int LuaCarX::SetTyreConfig(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }

    TyreConfig cfg = NativeCarX::GetTyreConfig();

    lua_getfield(L, 1, "infinite_wear");
    if (lua_isboolean(L, -1)) cfg.infiniteWear = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "wear_multiplier");
    if (lua_isnumber(L, -1)) cfg.wearMultiplier = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "desired_life");
    if (lua_isnumber(L, -1)) cfg.desiredLife = std::clamp((float)lua_tonumber(L, -1), 0.0f, 1.0f);
    lua_pop(L, 1);

    lua_getfield(L, 1, "override_grip");
    if (lua_isboolean(L, -1)) cfg.overrideGrip = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "front_grip");
    if (lua_isnumber(L, -1)) cfg.frontGrip = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "rear_grip");
    if (lua_isnumber(L, -1)) cfg.rearGrip = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "ballistic_grip");
    if (lua_isnumber(L, -1)) cfg.ballisticGrip = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "lock_temp");
    if (lua_isboolean(L, -1)) cfg.lockTemperature = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "target_temp");
    if (lua_isnumber(L, -1)) cfg.targetTemperature = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "override_pressure");
    if (lua_isboolean(L, -1)) cfg.overridePressure = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "front_pressure");
    if (lua_isnumber(L, -1)) cfg.frontPressure = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "rear_pressure");
    if (lua_isnumber(L, -1)) cfg.rearPressure = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    NativeCarX::SetTyreConfig(cfg);
    lua_pushboolean(L, true);
    return 1;
}

int LuaCarX::GetTyreLive(lua_State* L) {
    TyreLiveStats st = NativeCarX::GetTyreLiveStats();
    lua_newtable(L);

    lua_pushboolean(L, st.valid);
    lua_setfield(L, -2, "valid");

    lua_newtable(L);
    for (int i = 0; i < 4; ++i) {
        lua_newtable(L);

        lua_pushnumber(L, st.wheels[i].life);
        lua_setfield(L, -2, "life");

        lua_pushnumber(L, st.wheels[i].temp);
        lua_setfield(L, -2, "temp");

        lua_pushnumber(L, st.wheels[i].pressureKpa);
        lua_setfield(L, -2, "pressure_kpa");

        lua_pushnumber(L, st.wheels[i].friction);
        lua_setfield(L, -2, "friction");

        lua_pushnumber(L, st.wheels[i].ballisticGrip);
        lua_setfield(L, -2, "ballistic");

        lua_pushnumber(L, st.wheels[i].wearFactor);
        lua_setfield(L, -2, "wear_factor");

        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "wheels");

    return 1;
}

int LuaCarX::ResetTyreDefaults(lua_State* L) {
    NativeCarX::ResetTyreDefaults();
    lua_pushboolean(L, true);
    return 1;
}

static const luaL_Reg carx_funcs[] = {
    { "get_player_car", LuaCarX::GetPlayerCar },
    { "get_all_cars", LuaCarX::GetAllCars },
    { "get_cars", LuaCarX::GetAllCars },
    { "get_player_list", LuaCarX::GetPlayerList },
    { "is_in_session", LuaCarX::IsInSession },
    { "set_car_render_matrix", LuaCarX::SetCarRenderMatrix },
    { "set_car_active", LuaCarX::SetCarActive },
    { "set_car_rpm", LuaCarX::SetCarRPM },
    { "set_car_gear", LuaCarX::SetCarGear },
    { "spawn_car", LuaCarX::SpawnCar },
    { "destroy_car", LuaCarX::DestroyCar },
    { "spawn_ghost", LuaCarX::SpawnGhost },
    { "destroy_ghost", LuaCarX::DestroyGhost },
    { "has_ghost", LuaCarX::HasGhost },
    { "get_ghost", LuaCarX::GetGhost },
    { "set_ghost_mirror", LuaCarX::SetGhostMirror },
    { "is_ghost_mirror", LuaCarX::IsGhostMirror },
    { "set_ghost_collision", LuaCarX::SetGhostCollision },
    { "is_ghost_collision", LuaCarX::IsGhostCollision },
    { "get_ghost_count", LuaCarX::GetGhostCount },
    { "set_selective_ghost_config", LuaCarX::SetSelectiveGhostConfig },
    { "get_selective_ghost_config", LuaCarX::GetSelectiveGhostConfig },
    { "set_car_ghost_mode", LuaCarX::SetCarGhostMode },
    { "clear_car_ghost_modes", LuaCarX::ClearCarGhostModes },
    { "get_rigid_body", LuaCarX::GetRigidBody },
    { "teleport_rigid_body", LuaCarX::TeleportRigidBody },
    { "teleport_player", LuaCarX::TeleportRigidBody },
    { "teleport", LuaCarX::TeleportRigidBody },
    { "get_velocity", LuaCarX::GetVelocity },
    { "set_velocity", LuaCarX::SetVelocity },
    { "get_angular_velocity", LuaCarX::GetAngularVelocity },
    { "set_angular_velocity", LuaCarX::SetAngularVelocity },
    { "get_drift_info", LuaCarX::GetDriftInfo },
    { "get_drift", LuaCarX::GetDriftInfo },
    { "get_camera", LuaCarX::GetCamera },
    { "raycast", LuaCarX::Raycast },
    { "reset_car", LuaCarX::ResetCar },
    { "get_proximity", LuaCarX::GetProximity },
    { "get_time", LuaCarX::GetTime },
    { "get_delta_time", LuaCarX::GetDeltaTime },
    { "set_no_damage", LuaCarX::SetNoDamage },
    { "get_no_damage", LuaCarX::GetNoDamage },
    { "set_no_tire_wear", LuaCarX::SetNoTireWear },
    { "get_no_tire_wear", LuaCarX::GetNoTireWear },
    { "repair_car", LuaCarX::RepairCar },
    { "reset_tire_wear", LuaCarX::ResetTireWear },
    { "get_tyre_config", LuaCarX::GetTyreConfig },
    { "set_tyre_config", LuaCarX::SetTyreConfig },
    { "get_tyre_live", LuaCarX::GetTyreLive },
    { "reset_tyre_defaults", LuaCarX::ResetTyreDefaults },
    { nullptr, nullptr }
};

void LuaCarX::Register(lua_State* L) {
    luaL_newlib(L, carx_funcs);
    lua_setglobal(L, "carx");
}
