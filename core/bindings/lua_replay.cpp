#include "lua_replay.h"
#include "../replay_engine.h"
#include "../native_carx.h"
#include "../unity_bridge.h"
#include <chrono>

static void PushMatrixField(lua_State* L, const char* fieldName, const float m[16]) {
    lua_newtable(L);
    for (int i = 0; i < 16; ++i) {
        lua_pushnumber(L, m[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, fieldName);
}

static auto g_replayClock = std::chrono::steady_clock::now();

int LuaReplay::StartRecord(lua_State* L) {
    const char* trackName = luaL_optstring(L, 1, "Track");
    const char* carName = luaL_optstring(L, 2, "Car");
    std::string resolvedName = carName;
    if (resolvedName.empty() || resolvedName == "Car" || resolvedName == "Player Car") {
        std::string uName = UnityBridge::LookupCarUnityName(NativeCarX::GetPlayerCarPtr());
        if (!uName.empty()) resolvedName = uName;
    }
    bool result = ReplayEngine::StartRecording(trackName, resolvedName);
    lua_pushboolean(L, result);
    return 1;
}

int LuaReplay::StartRecordMulti(lua_State* L) {
    const char* trackName = luaL_optstring(L, 1, "Track");
    std::vector<RecordTarget> targets;

    if (lua_istable(L, 2)) {
        int count = static_cast<int>(luaL_len(L, 2));
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 2, i);
            if (lua_istable(L, -1)) {
                RecordTarget target;
                lua_getfield(L, -1, "ptr");
                if (lua_islightuserdata(L, -1)) {
                    target.carPtr = lua_touserdata(L, -1);
                }
                lua_pop(L, 1);

                lua_getfield(L, -1, "unity_name");
                if (lua_isstring(L, -1)) {
                    target.carName = lua_tostring(L, -1);
                } else {
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "name");
                    if (lua_isstring(L, -1)) {
                        target.carName = lua_tostring(L, -1);
                    }
                }
                lua_pop(L, 1);

                lua_getfield(L, -1, "is_local_player");
                if (lua_isboolean(L, -1)) {
                    target.isLocalPlayer = (lua_toboolean(L, -1) != 0);
                } else {
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "is_local");
                    if (lua_isboolean(L, -1)) {
                        target.isLocalPlayer = (lua_toboolean(L, -1) != 0);
                    }
                }
                lua_pop(L, 1);

                if (target.carPtr) {
                    if (target.carName.empty()) {
                        target.carName = UnityBridge::LookupCarUnityName(target.carPtr);
                    }
                    targets.push_back(std::move(target));
                }
            }
            lua_pop(L, 1);
        }
    }

    bool result = ReplayEngine::StartRecordingMulti(trackName, targets);
    lua_pushboolean(L, result);
    return 1;
}

int LuaReplay::RecordFrame(lua_State* L) {
    if (!ReplayEngine::IsRecording()) {
        lua_pushboolean(L, false);
        return 1;
    }

    CarTelemetry tele = NativeCarX::GetPlayerCarTelemetry();
    if (!tele.carPtr) {
        lua_pushboolean(L, false);
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    float currentTime = std::chrono::duration<float>(now - g_replayClock).count();

    ReplayEngine::RecordFrame(tele, currentTime);
    lua_pushboolean(L, true);
    return 1;
}

int LuaReplay::RecordAllFrames(lua_State* L) {
    if (!ReplayEngine::IsRecording()) {
        lua_pushboolean(L, false);
        return 1;
    }

    auto allCars = NativeCarX::GetAllCarsTelemetry();
    if (allCars.empty()) {
        lua_pushboolean(L, false);
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    float currentTime = std::chrono::duration<float>(now - g_replayClock).count();

    ReplayEngine::RecordMultiFrame(allCars, currentTime);
    lua_pushboolean(L, true);
    return 1;
}

int LuaReplay::StopRecord(lua_State* L) {
    const char* filename = luaL_optstring(L, 1, "");
    bool result = ReplayEngine::StopRecording(filename);
    lua_pushboolean(L, result);
    return 1;
}

int LuaReplay::IsRecording(lua_State* L) {
    lua_pushboolean(L, ReplayEngine::IsRecording());
    return 1;
}

int LuaReplay::GetRecordDuration(lua_State* L) {
    lua_pushnumber(L, ReplayEngine::GetCurrentRecordDuration());
    return 1;
}

int LuaReplay::GetRecordFrameCount(lua_State* L) {
    lua_pushinteger(L, ReplayEngine::GetCurrentRecordFrameCount());
    return 1;
}

int LuaReplay::GetRecordTrackCount(lua_State* L) {
    lua_pushinteger(L, ReplayEngine::GetCurrentRecordTrackCount());
    return 1;
}

int LuaReplay::LoadReplay(lua_State* L) {
    const char* filename = luaL_checkstring(L, 1);
    bool result = ReplayEngine::LoadReplay(filename);
    lua_pushboolean(L, result);
    return 1;
}

int LuaReplay::UnloadReplay(lua_State* L) {
    ReplayEngine::UnloadReplay();
    return 0;
}

int LuaReplay::IsLoaded(lua_State* L) {
    lua_pushboolean(L, ReplayEngine::IsReplayLoaded());
    return 1;
}

int LuaReplay::GetDuration(lua_State* L) {
    lua_pushnumber(L, ReplayEngine::GetReplayDuration());
    return 1;
}

int LuaReplay::GetFrameCount(lua_State* L) {
    lua_pushinteger(L, ReplayEngine::GetReplayFrameCount());
    return 1;
}

int LuaReplay::GetTrackCount(lua_State* L) {
    lua_pushinteger(L, ReplayEngine::GetReplayTrackCount());
    return 1;
}

int LuaReplay::GetTracks(lua_State* L) {
    uint32_t cnt = ReplayEngine::GetReplayTrackCount();
    lua_newtable(L);
    for (uint32_t i = 0; i < cnt; ++i) {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        lua_setfield(L, -2, "index");
        lua_pushstring(L, ReplayEngine::GetReplayTrackName(static_cast<int>(i)).c_str());
        lua_setfield(L, -2, "car_name");
        lua_pushboolean(L, ReplayEngine::IsReplayTrackLocal(static_cast<int>(i)));
        lua_setfield(L, -2, "is_local");
        lua_pushinteger(L, ReplayEngine::GetReplayTrackFrameCount(static_cast<int>(i)));
        lua_setfield(L, -2, "frames");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int LuaReplay::GetFrame(lua_State* L) {
    float time = static_cast<float>(luaL_checknumber(L, 1));
    int trackIdx = static_cast<int>(luaL_optinteger(L, 2, 1)) - 1;
    ReplayFrame frame;
    if (!ReplayEngine::GetInterpolatedTrackFrame(trackIdx, time, frame)) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushnumber(L, frame.time);
    lua_setfield(L, -2, "time");

    lua_pushnumber(L, frame.speedKmh);
    lua_setfield(L, -2, "speed_kmh");
    lua_pushnumber(L, frame.rpm);
    lua_setfield(L, -2, "rpm");
    lua_pushinteger(L, frame.gear);
    lua_setfield(L, -2, "gear");
    lua_pushnumber(L, frame.steerAngle);
    lua_setfield(L, -2, "steer_angle");

    lua_pushboolean(L, (frame.flags & (1 << 0)) != 0);
    lua_setfield(L, -2, "brake");
    lua_pushboolean(L, (frame.flags & (1 << 1)) != 0);
    lua_setfield(L, -2, "handbrake");
    lua_pushboolean(L, (frame.flags & (1 << 2)) != 0);
    lua_setfield(L, -2, "accel");

    // Transform Matrix (16 floats)
    PushMatrixField(L, "matrix", frame.transformMatrix);

    // Position helper
    lua_pushnumber(L, frame.transformMatrix[12]);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, frame.transformMatrix[13]);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, frame.transformMatrix[14]);
    lua_setfield(L, -2, "z");

    // Wheels
    lua_newtable(L);
    for (int w = 0; w < 4; ++w) {
        lua_newtable(L);
        PushMatrixField(L, "matrix", frame.wheelMatrices[w]);
        PushMatrixField(L, "caliper_matrix", frame.caliperMatrices[w]);
        lua_pushnumber(L, frame.wheelSmoke[w]);
        lua_setfield(L, -2, "smoke");
        lua_rawseti(L, -2, w + 1);
    }
    lua_setfield(L, -2, "wheels");

    return 1;
}

int LuaReplay::ApplyToGhost(lua_State* L) {
    float time = static_cast<float>(luaL_checknumber(L, 1));
    float offsetRight = (lua_gettop(L) >= 2 && lua_isnumber(L, 2))
        ? static_cast<float>(lua_tonumber(L, 2))
        : 0.0f;

    ReplayFrame frame;
    if (!ReplayEngine::GetInterpolatedFrame(time, frame)) {
        lua_pushboolean(L, false);
        return 1;
    }

    UnityBridge::ApplyReplayFrameToGhost(frame, offsetRight);
    lua_pushboolean(L, true);
    return 1;
}

int LuaReplay::ApplyAllToGhosts(lua_State* L) {
    float time = static_cast<float>(luaL_checknumber(L, 1));
    float offsetRight = (lua_gettop(L) >= 2 && lua_isnumber(L, 2))
        ? static_cast<float>(lua_tonumber(L, 2))
        : 0.0f;

    if (!ReplayEngine::IsReplayLoaded()) {
        lua_pushboolean(L, false);
        return 1;
    }

    UnityBridge::ApplyReplayTimeToAllGhosts(time, offsetRight);
    lua_pushboolean(L, true);
    return 1;
}

int LuaReplay::ListSaved(lua_State* L) {
    auto list = ReplayEngine::ListSavedReplays();
    lua_newtable(L);
    for (size_t i = 0; i < list.size(); ++i) {
        lua_newtable(L);
        lua_pushstring(L, list[i].filename.c_str());
        lua_setfield(L, -2, "filename");
        lua_pushstring(L, list[i].trackName.c_str());
        lua_setfield(L, -2, "track");
        lua_pushstring(L, list[i].carName.c_str());
        lua_setfield(L, -2, "car");
        lua_pushnumber(L, list[i].duration);
        lua_setfield(L, -2, "duration");
        lua_pushinteger(L, list[i].frameCount);
        lua_setfield(L, -2, "frames");
        lua_pushinteger(L, list[i].trackCount);
        lua_setfield(L, -2, "tracks");
        lua_pushstring(L, list[i].recordedDate.c_str());
        lua_setfield(L, -2, "date");

        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

static const luaL_Reg replay_funcs[] = {
    { "start_record", LuaReplay::StartRecord },
    { "start_record_multi", LuaReplay::StartRecordMulti },
    { "record_frame", LuaReplay::RecordFrame },
    { "record_all_frames", LuaReplay::RecordAllFrames },
    { "stop_record", LuaReplay::StopRecord },
    { "is_recording", LuaReplay::IsRecording },
    { "get_record_duration", LuaReplay::GetRecordDuration },
    { "get_record_frame_count", LuaReplay::GetRecordFrameCount },
    { "get_record_track_count", LuaReplay::GetRecordTrackCount },
    { "load", LuaReplay::LoadReplay },
    { "unload", LuaReplay::UnloadReplay },
    { "is_loaded", LuaReplay::IsLoaded },
    { "get_duration", LuaReplay::GetDuration },
    { "get_frame_count", LuaReplay::GetFrameCount },
    { "get_track_count", LuaReplay::GetTrackCount },
    { "get_tracks", LuaReplay::GetTracks },
    { "get_frame", LuaReplay::GetFrame },
    { "sample", LuaReplay::GetFrame },
    { "apply_to_ghost", LuaReplay::ApplyToGhost },
    { "apply_all_to_ghosts", LuaReplay::ApplyAllToGhosts },
    { "list_saved", LuaReplay::ListSaved },
    { nullptr, nullptr }
};

void LuaReplay::Register(lua_State* L) {
    luaL_newlib(L, replay_funcs);
    lua_setglobal(L, "replay");
}
