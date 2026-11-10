#include "lua_carfx.h"
#include "../carfx.h"

#include <cstdio>
#include <cstring>

static const luaL_Reg kCarfxFuncs[] = {
    { "load",           LuaCarFX::Load },
    { "unload",         LuaCarFX::Unload },
    { "reload",         LuaCarFX::Reload },
    { "set_enabled",    LuaCarFX::SetEnabled },
    { "is_enabled",     LuaCarFX::IsEnabled },
    { "set_mode",       LuaCarFX::SetMode },
    { "set_stealth",    LuaCarFX::SetStealth },
    { "set_param",      LuaCarFX::SetParam },
    { "get_param",      LuaCarFX::GetParam },
    { "set_tex",        LuaCarFX::SetTex },
    { "clear_tex",      LuaCarFX::ClearTex },
    { "set_game_input", LuaCarFX::SetGameInput },
    { "get_game_input", LuaCarFX::GetGameInput },
    { "list_shaders",   LuaCarFX::ListShaders },
    { "get_status",     LuaCarFX::GetStatus },
    { nullptr, nullptr }
};

void LuaCarFX::Register(lua_State* L) {
    luaL_newlib(L, kCarfxFuncs);
    lua_setglobal(L, "carfx");
}

int LuaCarFX::Load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int target = static_cast<int>(luaL_optinteger(L, 2, CarFX::kTargetBody));
    std::string err;
    bool ok = CarFX::LoadShader(target, path, &err);
    lua_pushboolean(L, ok);
    if (ok) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, err.c_str());
    }
    return 2;
}

int LuaCarFX::Unload(lua_State* L) {
    int target = static_cast<int>(luaL_optinteger(L, 1, CarFX::kTargetBody));
    CarFX::UnloadShader(target);
    return 0;
}

int LuaCarFX::Reload(lua_State* L) {
    int target = static_cast<int>(luaL_optinteger(L, 1, CarFX::kTargetBody));
    std::string err;
    bool ok = CarFX::ReloadShader(target, &err);
    lua_pushboolean(L, ok);
    if (ok) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, err.c_str());
    }
    return 2;
}

int LuaCarFX::SetEnabled(lua_State* L) {
    CarFX::SetEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

int LuaCarFX::IsEnabled(lua_State* L) {
    lua_pushboolean(L, CarFX::IsEnabled());
    return 1;
}

int LuaCarFX::SetMode(lua_State* L) {
    int mode = CarFX::kModeAuto;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 1);
        if (_stricmp(s, "replace") == 0) mode = CarFX::kModeReplace;
        else if (_stricmp(s, "overlay") == 0) mode = CarFX::kModeOverlay;
        else mode = CarFX::kModeAuto;
    } else {
        mode = static_cast<int>(luaL_optinteger(L, 1, CarFX::kModeAuto));
    }
    int target = static_cast<int>(luaL_optinteger(L, 2, CarFX::kTargetBody));
    CarFX::SetOverrideMode(target, mode);
    return 0;
}

int LuaCarFX::SetStealth(lua_State* L) {
    int stealth = CarFX::kStealthAuto;
    if (lua_type(L, 1) == LUA_TBOOLEAN) {
        stealth = lua_toboolean(L, 1) ? CarFX::kStealthOn : CarFX::kStealthOff;
    } else if (lua_type(L, 1) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 1);
        if (_stricmp(s, "on") == 0 || _stricmp(s, "hide") == 0 || _stricmp(s, "true") == 0) stealth = CarFX::kStealthOn;
        else if (_stricmp(s, "off") == 0 || _stricmp(s, "show") == 0 || _stricmp(s, "false") == 0) stealth = CarFX::kStealthOff;
        else stealth = CarFX::kStealthAuto;
    } else {
        stealth = static_cast<int>(luaL_optinteger(L, 1, CarFX::kStealthAuto));
    }
    int target = static_cast<int>(luaL_optinteger(L, 2, CarFX::kTargetBody));
    CarFX::SetOverrideStealth(target, stealth);
    return 0;
}

int LuaCarFX::SetParam(lua_State* L) {
    float vals[4] = {};
    int count = 0;
    for (int i = 2; i <= lua_gettop(L) && count < 4; ++i) {
        vals[count++] = static_cast<float>(luaL_checknumber(L, i));
    }
    if (count == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    bool ok = false;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        int slot = static_cast<int>(lua_tointeger(L, 1));
        ok = CarFX::SetParamSlot(CarFX::kTargetBody, slot, vals, count);
    } else {
        const char* name = luaL_checkstring(L, 1);
        ok = CarFX::SetParamByName(CarFX::kTargetBody, name, vals, count);
    }
    lua_pushboolean(L, ok);
    return 1;
}

int LuaCarFX::GetParam(lua_State* L) {
    float v[4] = {};
    if (lua_type(L, 1) == LUA_TNUMBER) {
        int slot = static_cast<int>(lua_tointeger(L, 1));
        if (!CarFX::GetParamSlot(CarFX::kTargetBody, slot, v)) return 0;
        lua_pushnumber(L, v[0]);
        lua_pushnumber(L, v[1]);
        lua_pushnumber(L, v[2]);
        lua_pushnumber(L, v[3]);
        return 4;
    }
    const char* name = luaL_checkstring(L, 1);
    CarFX::DiagnosticStatus st = CarFX::GetStatus(CarFX::kTargetBody);
    const CarFX::ParamDecl* pd = CarFX::FindParam(st.decl, name);
    if (!pd || !CarFX::GetParamSlot(CarFX::kTargetBody, pd->slot, v)) return 0;
    for (int i = 0; i < pd->count; ++i) {
        lua_pushnumber(L, v[pd->first + i]);
    }
    return pd->count;
}

int LuaCarFX::SetTex(lua_State* L) {
    int slot = static_cast<int>(luaL_checkinteger(L, 1));
    const char* path = luaL_checkstring(L, 2);
    std::string err;
    bool ok = CarFX::SetUserTexture(CarFX::kTargetBody, slot, path, &err);
    lua_pushboolean(L, ok);
    if (ok) lua_pushnil(L);
    else lua_pushstring(L, err.c_str());
    return 2;
}

int LuaCarFX::ClearTex(lua_State* L) {
    int slot = static_cast<int>(luaL_checkinteger(L, 1));
    CarFX::ClearUserTexture(CarFX::kTargetBody, slot);
    return 0;
}

int LuaCarFX::SetGameInput(lua_State* L) {
    int idx = static_cast<int>(luaL_checkinteger(L, 1));
    float val = static_cast<float>(luaL_checknumber(L, 2));
    CarFX::SetGameInput(idx, val);
    return 0;
}

int LuaCarFX::GetGameInput(lua_State* L) {
    int idx = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushnumber(L, CarFX::GetGameInput(idx));
    return 1;
}

int LuaCarFX::ListShaders(lua_State* L) {
    auto list = CarFX::ListAvailableShaders();
    lua_newtable(L);
    for (size_t i = 0; i < list.size(); ++i) {
        lua_pushstring(L, list[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int LuaCarFX::GetStatus(lua_State* L) {
    int target = static_cast<int>(luaL_optinteger(L, 1, CarFX::kTargetBody));
    CarFX::DiagnosticStatus st = CarFX::GetStatus(target);

    auto setBool = [&](const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); };
    auto setInt = [&](const char* k, lua_Integer v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); };
    auto setStr = [&](const char* k, const char* v) { lua_pushstring(L, v ? v : ""); lua_setfield(L, -2, k); };
    auto setHex = [&](const char* k, uintptr_t v) {
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%llX", static_cast<unsigned long long>(v));
        lua_pushstring(L, buf);
        lua_setfield(L, -2, k);
    };

    lua_newtable(L);
    setBool("initialized", st.initialized);
    setBool("enabled", st.enabled);
    setBool("hooked_draw", st.hookedDraw);
    setBool("compiled", st.compiled);
    setInt("active_mode", st.activeMode);
    setInt("resolved_mode", st.resolvedMode);
    setInt("active_stealth", st.activeStealth);
    setBool("decl_stealth", st.declStealth);
    setBool("resolved_stealth", st.resolvedStealth);
    setBool("uses_scene", st.usesScene);
    setBool("uses_depth", st.usesDepth);
    setBool("uses_livery", st.usesLivery);

    setInt("total_draws", st.totalDrawsFrame);
    setInt("matched_draws", st.matchedDrawsFrame);
    setInt("matched_last_frame", st.matchedDrawsLastFrame);
    setInt("scene_copies", st.sceneCopiesTotal);
    setInt("depth_copies", st.depthCopiesTotal);
    setInt("compiles", st.compilesTotal);
    setInt("failures", st.compileFailures);

    setBool("body_valid", st.bodyTargetsValid);
    setBool("body_albedo_default", st.bodyAlbedoIsDefault);
    setBool("body_packed_default", st.bodyPackedIsDefault);
    setInt("body_albedo_id", st.bodyAlbedoTexId);
    setInt("body_packed_id", st.bodyPackedTexId);
    setHex("body_albedo_srv", st.bodyAlbedoSrv);
    setHex("body_packed_srv", st.bodyPackedSrv);

    setInt("last_slot_mask", st.lastSlotMask);
    setInt("last_rt_w", st.lastRtWidth);
    setInt("last_rt_h", st.lastRtHeight);
    setInt("last_rt_fmt", st.lastRtFormat);
    setHex("last_game_ps", st.lastGamePs);

    setStr("shader_path", st.shaderPath.c_str());
    setStr("last_error", st.lastError.c_str());

    // Declared @param / @color table with current runtime values
    lua_newtable(L);
    for (int i = 0; i < st.decl.paramCount; ++i) {
        const CarFX::ParamDecl& p = st.decl.param[i];
        float slotVals[4] = {};
        CarFX::GetParamSlot(target, p.slot, slotVals);

        lua_newtable(L);
        setStr("name", p.name);
        setStr("desc", p.desc);
        setInt("slot", p.slot);
        setInt("first", p.first);
        setInt("count", p.count);
        setBool("color", p.color);
        lua_pushnumber(L, p.lo); lua_setfield(L, -2, "lo");
        lua_pushnumber(L, p.hi); lua_setfield(L, -2, "hi");

        lua_newtable(L);
        for (int c = 0; c < p.count; ++c) {
            lua_pushnumber(L, slotVals[p.first + c]);
            lua_rawseti(L, -2, c + 1);
        }
        lua_setfield(L, -2, "values");

        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "params");

    // Declared @tex table
    lua_newtable(L);
    int texOutIdx = 1;
    for (int i = 0; i < CarFX::kTextures; ++i) {
        const CarFX::TexDecl& t = st.decl.tex[i];
        if (!t.declared) continue;
        lua_newtable(L);
        setInt("slot", i);
        setStr("name", t.name);
        setBool("coverage", t.coverage);
        setStr("desc", t.desc);
        lua_rawseti(L, -2, texOutIdx++);
    }
    lua_setfield(L, -2, "textures");

    return 1;
}
