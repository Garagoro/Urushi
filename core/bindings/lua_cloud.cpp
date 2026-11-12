#include "lua_cloud.h"
#include "../cloud_scripts.h"
#include "../lua_sandbox.h"
#include "../logger.h"

namespace {

void SetStr(lua_State* L, const char* k, const std::string& v) {
    lua_pushstring(L, v.c_str());
    lua_setfield(L, -2, k);
}
void SetBool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v ? 1 : 0);
    lua_setfield(L, -2, k);
}

int Lua_CloudRefresh(lua_State*) {
    CloudScripts::RefreshAsync();
    return 0;
}

// cloud.status() -> { state = "idle|fetching|ready|error", error, ref, items = { {...}, ... } }
int Lua_CloudStatus(lua_State* L) {
    const CloudScripts::Snapshot s = CloudScripts::GetSnapshot();
    lua_newtable(L);
    const char* st = "idle";
    switch (s.state) {
    case CloudScripts::CatalogState::Fetching: st = "fetching"; break;
    case CloudScripts::CatalogState::Ready: st = "ready"; break;
    case CloudScripts::CatalogState::Error: st = "error"; break;
    default: break;
    }
    SetStr(L, "state", st);
    SetStr(L, "error", s.error);
    SetStr(L, "ref", s.ref);
    lua_newtable(L);
    int idx = 1;
    for (const auto& it : s.items) {
        lua_newtable(L);
        SetStr(L, "id", it.id);
        SetStr(L, "name", it.name);
        SetStr(L, "name_ru", it.nameRu);
        SetStr(L, "author", it.author);
        SetStr(L, "version", it.version);
        SetStr(L, "description", it.description);
        SetStr(L, "description_ru", it.descriptionRu);
        SetStr(L, "min_urushi", it.minUrushi);
        SetBool(L, "installed", it.installed);
        SetStr(L, "installed_version", it.installedVersion);
        SetBool(L, "update", it.updateAvailable);
        SetStr(L, "state", it.state);
        SetStr(L, "error", it.error);
        lua_rawseti(L, -2, idx++);
    }
    lua_setfield(L, -2, "items");
    return 1;
}

int Lua_CloudInstall(lua_State* L) {
    CloudScripts::InstallAsync(luaL_checkstring(L, 1));
    return 0;
}

int Lua_CloudRemove(lua_State* L) {
    std::string err;
    if (CloudScripts::Remove(luaL_checkstring(L, 1), err)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
}

// cloud.installed() -> { {id, name, name_ru, version}, ... }
int Lua_CloudInstalled(lua_State* L) {
    lua_newtable(L);
    int idx = 1;
    for (const auto& i : CloudScripts::ListInstalled()) {
        lua_newtable(L);
        SetStr(L, "id", i.id);
        SetStr(L, "name", i.name);
        SetStr(L, "name_ru", i.nameRu);
        SetStr(L, "version", i.version);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// cloud.load(id) -> true | false, error.  Runs the installed entry file inside the sandbox.
// While it runs, the global _CLOUD_SCRIPT_DIR holds the script's own folder (for dofile of siblings).
int Lua_CloudLoad(lua_State* L) {
    const std::string id = luaL_checkstring(L, 1);
    std::wstring file, dir;
    if (!CloudScripts::ResolveEntry(id, file, dir)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "not installed");
        return 2;
    }
    const std::string fileUtf8 = Logger::ToNarrow(file);
    const std::string dirUtf8 = Logger::ToNarrow(dir);

    (void)dirUtf8;
    // optional 2nd argument: environment table (scripts from the cloud always run isolated, see scripts/main.lua)
    int st = LuaSandbox::LoadFileText(L, fileUtf8.c_str(), lua_istable(L, 2) ? 2 : 0);
    if (st == LUA_OK) st = LuaSandbox::PCall(L, 0, 0, "cloud_script", 8000);

    if (st == LUA_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;
}

// cloud.include(id, "lib/helper.lua", env) -> first return value of the included file.
// Lets a multi-file cloud script load its own sibling files (never anything outside its folder).
int Lua_CloudInclude(lua_State* L) {
    const std::string id = luaL_checkstring(L, 1);
    const std::string rel = luaL_checkstring(L, 2);
    std::wstring entryFile, dir;
    if (!CloudScripts::ValidRelPath(rel) || !CloudScripts::ResolveEntry(id, entryFile, dir)) {
        return luaL_error(L, "include: invalid file or script not installed");
    }
    std::wstring file = dir + L"\\" + Logger::ToWide(rel);
    for (wchar_t& c : file) if (c == L'/') c = L'\\';
    const std::string fileUtf8 = Logger::ToNarrow(file);
    int st = LuaSandbox::LoadFileText(L, fileUtf8.c_str(), lua_istable(L, 3) ? 3 : 0);
    if (st == LUA_OK) st = LuaSandbox::PCall(L, 0, 1, "cloud_include", 8000);
    if (st != LUA_OK) return lua_error(L);
    return 1;
}

const luaL_Reg kCloudFuncs[] = {
    { "refresh",   Lua_CloudRefresh },
    { "status",    Lua_CloudStatus },
    { "install",   Lua_CloudInstall },
    { "remove",    Lua_CloudRemove },
    { "installed", Lua_CloudInstalled },
    { "load",      Lua_CloudLoad },
    { "include",   Lua_CloudInclude },
    { nullptr, nullptr }
};

} // namespace

void LuaCloud::Register(lua_State* L) {
    luaL_newlib(L, kCloudFuncs);
    lua_setglobal(L, "cloud");
}
