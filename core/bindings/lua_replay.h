#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaReplay {
public:
    static void Register(lua_State* L);

    static int StartRecord(lua_State* L);
    static int StartRecordMulti(lua_State* L);
    static int RecordFrame(lua_State* L);
    static int RecordAllFrames(lua_State* L);
    static int StopRecord(lua_State* L);
    static int IsRecording(lua_State* L);
    static int GetRecordDuration(lua_State* L);
    static int GetRecordFrameCount(lua_State* L);
    static int GetRecordTrackCount(lua_State* L);
    static int LoadReplay(lua_State* L);
    static int UnloadReplay(lua_State* L);
    static int IsLoaded(lua_State* L);
    static int GetDuration(lua_State* L);
    static int GetFrameCount(lua_State* L);
    static int GetTrackCount(lua_State* L);
    static int GetTracks(lua_State* L);
    static int GetFrame(lua_State* L);
    static int ApplyToGhost(lua_State* L);
    static int ApplyAllToGhosts(lua_State* L);
    static int ListSaved(lua_State* L);
};
