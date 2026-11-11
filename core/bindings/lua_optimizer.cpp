#include "lua_optimizer.h"
#include "../car_sim_optimizer.h"

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

void PushStringField(lua_State* L, const char* key, const std::string& val) {
    lua_pushstring(L, val.c_str());
    lua_setfield(L, -2, key);
}

void ReadBoolField(lua_State* L, int idx, const char* key, bool& target) {
    lua_getfield(L, idx, key);
    if (lua_isboolean(L, -1)) {
        target = (lua_toboolean(L, -1) != 0);
    }
    lua_pop(L, 1);
}

void ReadIntField(lua_State* L, int idx, const char* key, int& target) {
    lua_getfield(L, idx, key);
    if (lua_isinteger(L, -1)) {
        target = static_cast<int>(lua_tointeger(L, -1));
    } else if (lua_isnumber(L, -1)) {
        target = static_cast<int>(lua_tonumber(L, -1));
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

const luaL_Reg kOptimizerFuncs[] = {
    { "get_config",     LuaOptimizer::GetConfig },
    { "set_config",     LuaOptimizer::SetConfig },
    { "reset_defaults", LuaOptimizer::ResetDefaults },
    { "get_stats",      LuaOptimizer::GetStats },
    { "get_cars",       LuaOptimizer::GetCars },
    { nullptr, nullptr }
};

} // namespace

void LuaOptimizer::Register(lua_State* L) {
    luaL_newlib(L, kOptimizerFuncs);
    lua_setglobal(L, "optimizer");
}

int LuaOptimizer::GetConfig(lua_State* L) {
    CarSimOptimizer::Config c = CarSimOptimizer::GetConfig();
    lua_newtable(L);
    PushBoolField(L,  "enabled",                  c.enabled);
    PushIntField(L,   "max_full_sim_cars",        c.maxFullSimCars);
    PushFloatField(L, "max_full_sim_dist",        c.maxFullSimDistMeters);
    PushFloatField(L, "hysteresis_meters",        c.hysteresisMeters);
    PushFloatField(L, "switch_cooldown_sec",      c.switchCooldownSec);
    PushBoolField(L,  "tune_deform_sleep",        c.tuneDeformationSleep);
    PushFloatField(L, "deform_sleep_dist",        c.deformSleepDistMeters);
    PushIntField(L,   "deform_idle_steps",        c.deformIdleSteps);
    PushBoolField(L,  "tune_root_sleep",          c.tunePhysicsRootSleep);
    PushIntField(L,   "root_idle_steps",          c.physicsRootIdleSteps);
    PushBoolField(L,  "tune_render_policy",       c.tuneRenderPolicy);
    PushFloatField(L, "small_hide_dist",          c.smallHideDistMeters);
    PushFloatField(L, "small_renderer_radius",    c.smallRendererRadius);
    PushFloatField(L, "tiny_renderer_radius",     c.tinyRendererRadius);
    PushBoolField(L,  "remote_lod_half_rate",     c.remoteLodHalfRate);
    PushBoolField(L,  "player_best_lod",          c.playerBestLod);
    PushBoolField(L,  "limit_car_effects",        c.limitCarEffects);
    PushIntField(L,   "max_effects_cars",         c.maxEffectsCars);
    PushBoolField(L,  "tune_offline_physics",     c.tuneOfflinePhysics);
    PushBoolField(L,  "use_simple_mode",          c.useSimpleModeBeyondTopN);
    PushBoolField(L,  "use_traffic_mode",         c.useTrafficModeBeyondTopN);
    PushIntField(L,   "remote_integration_steps", c.remoteIntegrationSteps);
    return 1;
}

int LuaOptimizer::SetConfig(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    CarSimOptimizer::Config c = CarSimOptimizer::GetConfig();
    ReadBoolField(L,  1, "enabled",                  c.enabled);
    ReadIntField(L,   1, "max_full_sim_cars",        c.maxFullSimCars);
    ReadFloatField(L, 1, "max_full_sim_dist",        c.maxFullSimDistMeters);
    ReadFloatField(L, 1, "hysteresis_meters",        c.hysteresisMeters);
    ReadFloatField(L, 1, "switch_cooldown_sec",      c.switchCooldownSec);
    ReadBoolField(L,  1, "tune_deform_sleep",        c.tuneDeformationSleep);
    ReadFloatField(L, 1, "deform_sleep_dist",        c.deformSleepDistMeters);
    ReadIntField(L,   1, "deform_idle_steps",        c.deformIdleSteps);
    ReadBoolField(L,  1, "tune_root_sleep",          c.tunePhysicsRootSleep);
    ReadIntField(L,   1, "root_idle_steps",          c.physicsRootIdleSteps);
    ReadBoolField(L,  1, "tune_render_policy",       c.tuneRenderPolicy);
    ReadFloatField(L, 1, "small_hide_dist",          c.smallHideDistMeters);
    ReadFloatField(L, 1, "small_renderer_radius",    c.smallRendererRadius);
    ReadFloatField(L, 1, "tiny_renderer_radius",     c.tinyRendererRadius);
    ReadBoolField(L,  1, "remote_lod_half_rate",     c.remoteLodHalfRate);
    ReadBoolField(L,  1, "player_best_lod",          c.playerBestLod);
    ReadBoolField(L,  1, "limit_car_effects",        c.limitCarEffects);
    ReadIntField(L,   1, "max_effects_cars",         c.maxEffectsCars);
    ReadBoolField(L,  1, "tune_offline_physics",     c.tuneOfflinePhysics);
    ReadBoolField(L,  1, "use_simple_mode",          c.useSimpleModeBeyondTopN);
    ReadBoolField(L,  1, "use_traffic_mode",         c.useTrafficModeBeyondTopN);
    ReadIntField(L,   1, "remote_integration_steps", c.remoteIntegrationSteps);

    CarSimOptimizer::SetConfig(c);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaOptimizer::ResetDefaults(lua_State* L) {
    CarSimOptimizer::ResetDefaults();
    lua_pushboolean(L, 1);
    return 1;
}

int LuaOptimizer::GetStats(lua_State* L) {
    CarSimOptimizer::Stats s = CarSimOptimizer::GetStats();
    lua_newtable(L);
    PushBoolField(L,  "initialized",               s.initialized);
    PushBoolField(L,  "hooks_installed",           s.hooksInstalled);
    PushBoolField(L,  "native_exports_ready",      s.nativeExportsReady);
    PushBoolField(L,  "deform_sleep_resolved",     s.deformSleepResolved);
    PushBoolField(L,  "root_sleep_resolved",       s.rootSleepResolved);
    PushBoolField(L,  "render_policy_resolved",    s.renderPolicyResolved);
    PushBoolField(L,  "optimize_geom_resolved",    s.optimizeGeomResolved);
    PushIntField(L,   "switching_calls",           static_cast<lua_Integer>(s.switchingUpdateCalls));
    PushIntField(L,   "anti_flicker_blocks",       static_cast<lua_Integer>(s.antiFlickerBlocks));
    PushBoolField(L,  "anti_flicker_holding",      s.antiFlickerHolding);
    PushIntField(L,   "total_net_cars",            static_cast<lua_Integer>(s.totalNetCars));
    PushIntField(L,   "full_sim_net_cars",         static_cast<lua_Integer>(s.fullSimNetCars));
    PushIntField(L,   "snapshot_net_cars",         static_cast<lua_Integer>(s.snapshotNetCars));
    PushIntField(L,   "deform_sleeping_cars",      s.deformSleepingCars);
    PushIntField(L,   "deform_transitions",        s.deformTransitions);
    PushIntField(L,   "root_sleeping_roots",       s.rootSleepingRoots);
    PushIntField(L,   "root_transitions",          s.rootTransitions);
    PushIntField(L,   "effects_disabled_cars",     s.effectsDisabledCars);
    PushIntField(L,   "offline_reduced_cars",      s.offlineReducedCars);
    PushFloatField(L, "vanilla_deform_dist",       s.vanillaDeformDist);
    PushIntField(L,   "vanilla_deform_idle_steps", s.vanillaDeformIdleSteps);
    PushIntField(L,   "vanilla_root_idle_steps",   s.vanillaRootIdleSteps);
    PushFloatField(L, "vanilla_small_hide_dist",   s.vanillaSmallHideDist);
    PushFloatField(L, "vanilla_small_radius",      s.vanillaSmallRadius);
    PushFloatField(L, "vanilla_tiny_radius",       s.vanillaTinyRadius);
    return 1;
}

int LuaOptimizer::GetCars(lua_State* L) {
    auto cars = CarSimOptimizer::GetNetCarsSnapshot();
    lua_newtable(L);
    for (size_t i = 0; i < cars.size(); ++i) {
        const auto& car = cars[i];
        lua_newtable(L);
        PushIntField(L,    "owner_id",        car.ownerId);
        PushStringField(L, "name",            car.name);
        PushFloatField(L,  "x",               car.x);
        PushFloatField(L,  "y",               car.y);
        PushFloatField(L,  "z",               car.z);
        PushFloatField(L,  "dist_meters",     car.distMeters);
        PushBoolField(L,   "has_dist",        car.hasDist);
        PushBoolField(L,   "vanilla_far",     car.vanillaFar);
        PushBoolField(L,   "is_kinematic",    car.isKinematic);
        PushBoolField(L,   "active_full_sim", car.activeFullSim);
        PushBoolField(L,   "forced_snapshot", car.forcedSnapshot);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
