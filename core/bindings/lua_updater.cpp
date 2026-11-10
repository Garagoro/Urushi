#include "lua_updater.h"
#include "../updater.h"

namespace {

const char* StateName(Updater::State s) {
    switch (s) {
    case Updater::State::Idle:            return "idle";
    case Updater::State::Checking:        return "checking";
    case Updater::State::UpToDate:        return "up_to_date";
    case Updater::State::UpdateAvailable: return "update_available";
    case Updater::State::Downloading:     return "downloading";
    case Updater::State::Reinjecting:     return "reinjecting";
    case Updater::State::RestartRequired: return "restart_required";
    case Updater::State::Error:           return "error";
    default:                              return "unknown";
    }
}

const luaL_Reg kUpdaterFuncs[] = {
    { "get_status",    LuaUpdater::GetStatus },
    { "check",         LuaUpdater::Check },
    { "install",       LuaUpdater::Install },
    { "set_dismissed", LuaUpdater::SetDismissed },
    { "restart_game",  LuaUpdater::RestartGame },
    { nullptr, nullptr }
};

} // namespace

void LuaUpdater::Register(lua_State* L) {
    luaL_newlib(L, kUpdaterFuncs);
    lua_setglobal(L, "updater");
}

int LuaUpdater::GetStatus(lua_State* L) {
    Updater::StatusSnapshot st = Updater::GetStatus();
    auto setStr = [&](const char* k, const std::string& v) {
        lua_pushstring(L, v.c_str());
        lua_setfield(L, -2, k);
    };
    auto setBool = [&](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    auto setInt = [&](const char* k, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, k);
    };

    lua_newtable(L);
    setStr("state", StateName(st.state));
    setInt("state_code", static_cast<lua_Integer>(st.state));
    setStr("local_version", st.localVersion);
    setStr("remote_version", st.remoteVersion);
    setStr("release_notes", st.releaseNotes);
    setStr("status_text", st.statusText);
    setStr("current_file", st.currentFile);
    setInt("local_link_time", st.localLinkTime);
    setInt("remote_link_time", st.remoteLinkTime);
    setBool("local_newer", st.localBuildNewer);
    setBool("proxy_restart", st.proxyNeedsRestart);
    setBool("dismissed", st.bannerDismissed);
    setInt("files_total", st.filesTotal);
    setInt("files_done", st.filesDone);
    setInt("bytes_total", static_cast<lua_Integer>(st.bytesTotal));
    setInt("bytes_done", static_cast<lua_Integer>(st.bytesDone));

    lua_newtable(L);
    for (size_t i = 0; i < st.files.size(); ++i) {
        lua_newtable(L);
        setStr("path", st.files[i].repoPath);
        setInt("size", static_cast<lua_Integer>(st.files[i].size));
        setBool("is_proxy", st.files[i].isProxy);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "files");
    return 1;
}

int LuaUpdater::Check(lua_State* /*L*/) {
    Updater::CheckAsync(true);
    return 0;
}

int LuaUpdater::Install(lua_State* /*L*/) {
    Updater::StartInstallAsync();
    return 0;
}

int LuaUpdater::SetDismissed(lua_State* L) {
    Updater::SetDismissed(lua_toboolean(L, 1) != 0);
    return 0;
}

int LuaUpdater::RestartGame(lua_State* /*L*/) {
    Updater::RestartGame();
    return 0;
}
