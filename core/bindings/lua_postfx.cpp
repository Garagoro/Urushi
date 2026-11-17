#include "lua_postfx.h"
#include "../postfx.h"

#include <cstring>
#include <string>

static const luaL_Reg kPostfxFuncs[] = {
    { "get_shader_list", LuaPostFX::GetShaderList },
    { "load",            LuaPostFX::Load },
    { "unload",          LuaPostFX::Unload },
    { "set_enabled",     LuaPostFX::SetEnabled },
    { "is_enabled",      LuaPostFX::IsEnabled },
    { "set_param",       LuaPostFX::SetParam },
    { "get_params",      LuaPostFX::GetParams },
    { nullptr, nullptr }
};

void LuaPostFX::Register(lua_State* L) {
    luaL_newlib(L, kPostfxFuncs);
    lua_setglobal(L, "postfx");
}

int LuaPostFX::GetShaderList(lua_State* L) {
    auto list = PostFX::ListShaders();
    lua_newtable(L);
    for (size_t i = 0; i < list.size(); ++i) {
        lua_pushstring(L, list[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int LuaPostFX::Load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::string err;
    bool ok = PostFX::LoadShader(path, &err);
    lua_pushboolean(L, ok);
    if (ok) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, err.c_str());
    }
    return 2;
}

int LuaPostFX::Unload(lua_State* L) {
    PostFX::UnloadShader();
    return 0;
}

int LuaPostFX::SetEnabled(lua_State* L) {
    PostFX::SetEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

int LuaPostFX::IsEnabled(lua_State* L) {
    lua_pushboolean(L, PostFX::IsEnabled());
    return 1;
}

int LuaPostFX::SetParam(lua_State* L) {
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
        ok = PostFX::SetParamSlot(slot, vals, count);
    } else {
        const char* name = luaL_checkstring(L, 1);
        ok = PostFX::SetParamByName(name, vals, count);
    }
    lua_pushboolean(L, ok);
    return 1;
}

int LuaPostFX::GetParams(lua_State* L) {
    CarFX::ShaderDecl decl = PostFX::GetCurrentDecl();
    lua_newtable(L);
    for (int i = 0; i < decl.paramCount; ++i) {
        const auto& p = decl.param[i];
        float slotVals[4] = {};
        PostFX::GetParamSlot(p.slot, slotVals);

        lua_newtable(L);
        lua_pushstring(L, p.name); lua_setfield(L, -2, "name");
        lua_pushstring(L, p.desc); lua_setfield(L, -2, "desc");
        lua_pushinteger(L, p.slot); lua_setfield(L, -2, "slot");
        lua_pushinteger(L, p.first); lua_setfield(L, -2, "first");
        lua_pushinteger(L, p.count); lua_setfield(L, -2, "count");
        lua_pushboolean(L, p.color); lua_setfield(L, -2, "color");
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
    return 1;
}
