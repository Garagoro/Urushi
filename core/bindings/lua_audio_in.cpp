#include "lua_audio_in.h"
#include "../audio_capture.h"

#include <cstring>
#include <string>
#include <vector>

namespace LuaAudioIn {

static const luaL_Reg kAudioInFuncs[] = {
    { "is_supported",  IsSupported },
    { "list_sources",  ListSources },
    { "set_source",    SetSource },
    { "get_source",    GetSource },
    { "set_enabled",   SetEnabled },
    { "is_enabled",    IsEnabled },
    { "get_status",    GetStatus },
    { "get_levels",    GetLevels },
    { "get_spectrum",  GetSpectrum },
    { "set_gain",      SetGain },
    { "get_gain",      GetGain },
    { "set_smoothing", SetSmoothing },
    { "get_smoothing", GetSmoothing },
    { nullptr, nullptr }
};

void Register(lua_State* L) {
    luaL_newlib(L, kAudioInFuncs);
    lua_setglobal(L, "audio_in");
}

int IsSupported(lua_State* L) {
    lua_createtable(L, 0, 1);
    lua_pushboolean(L, AudioCapture::IsProcessLoopbackSupported());
    lua_setfield(L, -2, "process_loopback");
    return 1;
}

int ListSources(lua_State* L) {
    std::vector<AudioCapture::SourceInfo> list = AudioCapture::ListSources();
    lua_createtable(L, static_cast<int>(list.size()), 0);

    for (size_t i = 0; i < list.size(); ++i) {
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, list[i].pid);
        lua_setfield(L, -2, "pid");
        lua_pushstring(L, list[i].exeName.c_str());
        lua_setfield(L, -2, "exe");
        lua_pushstring(L, list[i].displayName.c_str());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, list[i].isActive);
        lua_setfield(L, -2, "active");
        lua_pushboolean(L, list[i].isGame);
        lua_setfield(L, -2, "is_game");

        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }

    return 1;
}

int SetSource(lua_State* L) {
    const char* src = luaL_checkstring(L, 1);
    bool ok = AudioCapture::SetSource(src ? src : "system");
    lua_pushboolean(L, ok);
    return 1;
}

int GetSource(lua_State* L) {
    std::string src = AudioCapture::GetSource();
    lua_pushstring(L, src.c_str());
    return 1;
}

int SetEnabled(lua_State* L) {
    bool en = (lua_toboolean(L, 1) != 0);
    AudioCapture::SetEnabled(en);
    return 0;
}

int IsEnabled(lua_State* L) {
    lua_pushboolean(L, AudioCapture::IsEnabled());
    return 1;
}

int GetStatus(lua_State* L) {
    std::string state;
    std::string text;
    AudioCapture::GetStatus(&state, &text);
    lua_pushstring(L, state.c_str());
    lua_pushstring(L, text.c_str());
    return 2;
}

int GetLevels(lua_State* L) {
    AudioCapture::Levels lv = AudioCapture::GetLevels();
    lua_createtable(L, 0, 7);
    lua_pushnumber(L, lv.level);
    lua_setfield(L, -2, "level");
    lua_pushnumber(L, lv.bass);
    lua_setfield(L, -2, "bass");
    lua_pushnumber(L, lv.lowMid);
    lua_setfield(L, -2, "low_mid");
    lua_pushnumber(L, lv.mid);
    lua_setfield(L, -2, "mid");
    lua_pushnumber(L, lv.highMid);
    lua_setfield(L, -2, "high_mid");
    lua_pushnumber(L, lv.treble);
    lua_setfield(L, -2, "treble");
    lua_pushnumber(L, lv.beat);
    lua_setfield(L, -2, "beat");
    return 1;
}

int GetSpectrum(lua_State* L) {
    float spec[32] = {};
    AudioCapture::GetSpectrum(spec);
    lua_createtable(L, 32, 0);
    for (int i = 0; i < 32; ++i) {
        lua_pushnumber(L, spec[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int SetGain(lua_State* L) {
    float g = static_cast<float>(luaL_checknumber(L, 1));
    AudioCapture::SetGain(g);
    return 0;
}

int GetGain(lua_State* L) {
    lua_pushnumber(L, AudioCapture::GetGain());
    return 1;
}

int SetSmoothing(lua_State* L) {
    float attack = static_cast<float>(luaL_checknumber(L, 1));
    float release = static_cast<float>(luaL_checknumber(L, 2));
    AudioCapture::SetSmoothing(attack, release);
    return 0;
}

int GetSmoothing(lua_State* L) {
    float attack = 0.0f;
    float release = 0.0f;
    AudioCapture::GetSmoothing(&attack, &release);
    lua_pushnumber(L, attack);
    lua_pushnumber(L, release);
    return 2;
}

} // namespace LuaAudioIn
