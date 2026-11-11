#include "lua_steer.h"
#include "../steer_assist.h"

namespace {

void PushBoolField(lua_State* L, const char* key, bool val) {
    lua_pushboolean(L, val ? 1 : 0);
    lua_setfield(L, -2, key);
}

void PushIntField(lua_State* L, const char* key, lua_Integer val) {
    lua_pushinteger(L, val);
    lua_setfield(L, -2, key);
}

void PushFloatField(lua_State* L, const char* key, float val) {
    lua_pushnumber(L, static_cast<lua_Number>(val));
    lua_setfield(L, -2, key);
}

void ReadBoolField(lua_State* L, int idx, const char* key, bool& target) {
    lua_getfield(L, idx, key);
    if (lua_isboolean(L, -1)) {
        target = (lua_toboolean(L, -1) != 0);
    }
    lua_pop(L, 1);
}

void ReadFloatField(lua_State* L, int idx, const char* key, float& target) {
    lua_getfield(L, idx, key);
    if (lua_isnumber(L, -1)) {
        target = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
}

const luaL_Reg kSteerFuncs[] = {
    { "get_config",      LuaSteer::GetConfig },
    { "set_config",      LuaSteer::SetConfig },
    { "set_recommended", LuaSteer::SetRecommended },
    { "set_vanilla",     LuaSteer::SetVanilla },
    { "get_stats",       LuaSteer::GetStats },
    { nullptr, nullptr }
};

} // namespace

void LuaSteer::Register(lua_State* L) {
    luaL_newlib(L, kSteerFuncs);
    lua_setglobal(L, "steer_assist");
}

int LuaSteer::GetConfig(lua_State* L) {
    SteerAssist::Config c = SteerAssist::GetConfig();
    lua_newtable(L);
    PushBoolField(L,  "enabled",                  c.enabled);
    PushFloatField(L, "flip_angle_deg",           c.flipAngleDeg);
    PushBoolField(L,  "disable_early_steer_flip", c.disableEarlySteerFlip);
    PushFloatField(L, "early_steer_flip_deg",     c.earlySteerFlipDeg);
    PushBoolField(L,  "fix_slip_gate",            c.fixSlipGate);
    PushFloatField(L, "center_scale",             c.centerScale);
    return 1;
}

int LuaSteer::SetConfig(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    SteerAssist::Config c = SteerAssist::GetConfig();
    ReadBoolField(L,  1, "enabled",                  c.enabled);
    ReadFloatField(L, 1, "flip_angle_deg",           c.flipAngleDeg);
    ReadBoolField(L,  1, "disable_early_steer_flip", c.disableEarlySteerFlip);
    ReadFloatField(L, 1, "early_steer_flip_deg",     c.earlySteerFlipDeg);
    ReadBoolField(L,  1, "fix_slip_gate",            c.fixSlipGate);
    ReadFloatField(L, 1, "center_scale",             c.centerScale);

    SteerAssist::SetConfig(c);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaSteer::SetRecommended(lua_State* L) {
    SteerAssist::SetRecommendedDefaults();
    lua_pushboolean(L, 1);
    return 1;
}

int LuaSteer::SetVanilla(lua_State* L) {
    SteerAssist::SetVanillaDefaults();
    lua_pushboolean(L, 1);
    return 1;
}

int LuaSteer::GetStats(lua_State* L) {
    SteerAssist::Stats s = SteerAssist::GetStats();
    lua_newtable(L);
    PushBoolField(L,  "initialized",    s.initialized);
    PushBoolField(L,  "native_found",   s.nativeFound);
    PushBoolField(L,  "bytes_verified", s.bytesVerified);
    PushBoolField(L,  "near_alloc_ok",  s.nearAllocOk);
    PushBoolField(L,  "patches_applied",s.patchesApplied);
    PushBoolField(L,  "has_car",        s.hasCar);
    PushIntField(L,   "steer_mode",     s.steerMode);
    PushIntField(L,   "steer_ff_mode",  s.steerFFmode);
    PushIntField(L,   "assist_active",  s.assistActive);
    PushFloatField(L, "speed_kmh",      s.speedKmh);
    PushFloatField(L, "steer_deg",      s.steerDeg);
    PushFloatField(L, "max_steer_deg",  s.maxSteerDeg);
    PushFloatField(L, "ffb_torque",     s.ffbTorque);
    PushFloatField(L, "front_slip_deg", s.frontSlipDeg);
    return 1;
}
