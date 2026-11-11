#include "lua_carx.h"
#include "../native_carx.h"
#include "../unity_bridge.h"
#include "../ecs_collision.h"
#include <chrono>

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

    lua_pushlightuserdata(L, car.carPtr);
    lua_setfield(L, -2, "ptr");

    lua_pushboolean(L, car.isLocalPlayer);
    lua_setfield(L, -2, "is_local_player");

    std::string unityName = UnityBridge::LookupCarUnityName(car.carPtr);
    if (!unityName.empty()) {
        lua_pushstring(L, unityName.c_str());
        lua_setfield(L, -2, "unity_name");
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

    // Wheels
    lua_newtable(L);
    for (int w = 0; w < 4; ++w) {
        lua_newtable(L);

        PushMatrixField(L, "matrix", car.wheels[w].matrix);
        PushMatrixField(L, "caliper_matrix", car.wheels[w].caliperMatrix);

        lua_pushnumber(L, car.wheels[w].smoke);
        lua_setfield(L, -2, "smoke");
        lua_pushnumber(L, car.wheels[w].smokeIntensity);
        lua_setfield(L, -2, "smoke_intensity");
        lua_pushnumber(L, car.wheels[w].camber);
        lua_setfield(L, -2, "camber");
        lua_pushnumber(L, car.wheels[w].toeIn);
        lua_setfield(L, -2, "toe_in");

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
        lua_pushlightuserdata(L, list[i].carPtr);
        lua_setfield(L, -2, "ptr");
        lua_pushstring(L, list[i].unityName.c_str());
        lua_setfield(L, -2, "unity_name");
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

    void* carPtr = lua_touserdata(L, 1);
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
    void* carPtr = lua_touserdata(L, 1);
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
    void* carPtr = lua_touserdata(L, 1);
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
    void* carPtr = lua_touserdata(L, 1);
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
    lua_pushlightuserdata(L, car);
    return 1;
}

int LuaCarX::DestroyCar(lua_State* L) {
    if (!lua_islightuserdata(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    void* carPtr = lua_touserdata(L, 1);
    bool ok = NativeCarX::DestroyCar(carPtr);
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarX::SpawnGhost(lua_State* L) {
    void* ghost = UnityBridge::SpawnGhostVisual();
    if (ghost) {
        lua_pushlightuserdata(L, ghost);
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
    lua_pushlightuserdata(L, ghost);
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
        carPtr = reinterpret_cast<uintptr_t>(lua_touserdata(L, 1));
        unityName = UnityBridge::LookupCarUnityName(reinterpret_cast<void*>(carPtr));
    } else if (lua_isstring(L, 1)) {
        unityName = lua_tostring(L, 1);
    } else if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "ptr");
        if (lua_islightuserdata(L, -1)) carPtr = reinterpret_cast<uintptr_t>(lua_touserdata(L, -1));
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
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }

    TeleportPose pose{};

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
    if (hasX || hasY || hasZ) {
        pose.rb[20] = pose.pos[0];
        pose.rb[21] = pose.pos[1];
        pose.rb[22] = pose.pos[2];
    } else if (!pose.hasRb) {
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
    }

    if (ReadLuaVecField(L, 1, "vel", pose.linVel)) {
        pose.rb[23] = pose.linVel[0];
        pose.rb[24] = pose.linVel[1];
        pose.rb[25] = pose.linVel[2];
    }
    if (ReadLuaVecField(L, 1, "ang_vel", pose.angVel)) {
        pose.rb[26] = pose.angVel[0];
        pose.rb[27] = pose.angVel[1];
        pose.rb[28] = pose.angVel[2];
    }

    int velMode = 0;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        velMode = static_cast<int>(lua_tointeger(L, 2));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 2);
        if (s) {
            if (_stricmp(s, "saved") == 0) velMode = 1;
            else if (_stricmp(s, "keep") == 0) velMode = 2;
        }
    }

    bool ok = UnityBridge::RequestTeleportPlayerPose(pose, velMode);
    lua_pushboolean(L, ok);
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
    { "get_time", LuaCarX::GetTime },
    { "get_delta_time", LuaCarX::GetDeltaTime },
    { nullptr, nullptr }
};

void LuaCarX::Register(lua_State* L) {
    luaL_newlib(L, carx_funcs);
    lua_setglobal(L, "carx");
}
