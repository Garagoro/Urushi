#include "lua_audio.h"
#include "../audio_player.h"

#include <string>

static const luaL_Reg kAudioFuncs[] = {
    { "list",        LuaAudio::List },
    { "play",        LuaAudio::Play },
    { "stop",        LuaAudio::Stop },
    { "stop_all",    LuaAudio::StopAll },
    { "set_volume",  LuaAudio::SetVolume },
    { "is_playing",  LuaAudio::IsPlaying },
    { nullptr, nullptr }
};

void LuaAudio::Register(lua_State* L) {
    luaL_newlib(L, kAudioFuncs);
    lua_setglobal(L, "audio");
}

int LuaAudio::List(lua_State* L) {
    auto files = AudioPlayer::ListAudioFiles();
    lua_newtable(L);
    for (size_t i = 0; i < files.size(); ++i) {
        lua_pushstring(L, files[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int LuaAudio::Play(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    AudioPlayer::PlayOptions opts{};
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "loop");
        if (!lua_isnil(L, -1)) {
            opts.loop = (lua_toboolean(L, -1) != 0);
        }
        lua_pop(L, 1);

        lua_getfield(L, 2, "volume");
        if (!lua_isnil(L, -1)) {
            opts.volume = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
    }
    std::string err;
    uint32_t id = AudioPlayer::Play(file, opts, &err);
    if (id > 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
}

int LuaAudio::Stop(lua_State* L) {
    uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    bool ok = AudioPlayer::Stop(id);
    lua_pushboolean(L, ok);
    return 1;
}

int LuaAudio::StopAll(lua_State* L) {
    AudioPlayer::StopAll();
    return 0;
}

int LuaAudio::SetVolume(lua_State* L) {
    uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    float vol = static_cast<float>(luaL_checknumber(L, 2));
    bool ok = AudioPlayer::SetVolume(id, vol);
    lua_pushboolean(L, ok);
    return 1;
}

int LuaAudio::IsPlaying(lua_State* L) {
    uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    bool playing = AudioPlayer::IsPlaying(id);
    lua_pushboolean(L, playing);
    return 1;
}
