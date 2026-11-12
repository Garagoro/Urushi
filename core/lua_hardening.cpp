#include "lua_hardening.h"
#include "logger.h"

#include <windows.h>
#include <cwchar>

namespace LuaHardening {
namespace {

constexpr ULONGLONG kDataBudgetBytes = 64ull * 1024ull * 1024ull;

std::wstring g_exeDir;

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool HasPrefixNoCase(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool ValidComponent(const std::wstring& c) {
    if (c.empty() || c.size() > 64) return false;
    if (c[0] == L'.' || c.back() == L'.' || c.back() == L' ') return false;
    for (wchar_t ch : c) {
        const bool ok = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                        ch == L'_' || ch == L'-' || ch == L'.' || ch == L' ' || ch == L'(' || ch == L')';
        if (!ok) return false;
    }
    // Reserved DOS device names (also with an extension: "NUL.txt")
    std::wstring stem = c.substr(0, c.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    static const wchar_t* kReserved[] = { L"CON", L"PRN", L"AUX", L"NUL" };
    for (const wchar_t* r : kReserved) if (EqualNoCase(stem, r)) return false;
    if (stem.size() == 4 && (HasPrefixNoCase(stem, L"COM") || HasPrefixNoCase(stem, L"LPT")) && stem[3] >= L'0' && stem[3] <= L'9') return false;
    return true;
}

bool AllowedDataExt(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = name.substr(dot);
    return EqualNoCase(ext, L".json") || EqualNoCase(ext, L".txt") || EqualNoCase(ext, L".cfg") || EqualNoCase(ext, L".csv");
}

ULONGLONG DirSize(const std::wstring& dir, int depth) {
    if (depth > 6) return 0;
    ULONGLONG total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSize(dir + L"\\" + fd.cFileName, depth + 1);
        else total += (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

void EnsureDirs(const std::wstring& dir) {
    for (size_t i = 3; i < dir.size(); ++i) {
        if (dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

// ---------------------------------------------------------------- io.open
int SafeOpen(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");
    if (!IsIoAllowed(g_exeDir.c_str(), path, mode)) {
        Logger::Warn(std::string("Lua io.open denied: ") + path + " (" + mode + ")");
        lua_pushnil(L);
        lua_pushfstring(L, "%s: access denied (scripts may only use .json/.txt/.cfg/.csv files in urushi_data/)", path);
        lua_pushinteger(L, 13);
        return 3;
    }
    const int base = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushstring(L, path);
    lua_pushstring(L, mode);
    lua_call(L, 2, LUA_MULTRET);
    return lua_gettop(L) - base;
}

// ---------------------------------------------------------------- load (text chunks only)
int SafeLoad(lua_State* L) {
    const int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    if (n >= 2) lua_pushvalue(L, 2); else lua_pushnil(L);
    lua_pushstring(L, "t");                 // bytecode can corrupt the VM / native memory: never allowed
    int nargs = 3;
    if (n >= 4) { lua_pushvalue(L, 4); nargs = 4; }
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L) - n;
}

void CopyFields(lua_State* L, int srcIdx, const char* const* names, int dstIdx) {
    srcIdx = lua_absindex(L, srcIdx);
    dstIdx = lua_absindex(L, dstIdx);
    for (; *names; ++names) {
        lua_getfield(L, srcIdx, *names);
        lua_setfield(L, dstIdx, *names);
    }
}

} // namespace

bool IsIoAllowed(const wchar_t* exeDir, const char* pathUtf8, const char* mode, std::wstring* outFull) {
    if (!exeDir || !pathUtf8 || !mode) return false;

    // mode: [rwa] followed by any of '+' / 'b' (at most once each)
    if (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a') return false;
    bool plus = false, bin = false;
    for (const char* m = mode + 1; *m; ++m) {
        if (*m == '+' && !plus) plus = true;
        else if (*m == 'b' && !bin) bin = true;
        else return false;
    }
    const bool write = (mode[0] != 'r') || plus;

    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8, -1, nullptr, 0);
    if (len <= 1 || len > 260) return false;
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8, -1, &w[0], len);

    for (wchar_t ch : w) {
        if (ch < 0x20 || wcschr(L"<>\"|?*", ch)) return false;
    }
    // no device/UNC/extended paths, no alternate data streams: a ':' is only allowed as the drive-letter colon
    if (w.size() >= 2 && ((w[0] == L'\\' && w[1] == L'\\') || (w[0] == L'/' && w[1] == L'/'))) return false;
    const size_t firstColon = w.find(L':');
    if (firstColon != std::wstring::npos && (firstColon != 1 || w.find(L':', 2) != std::wstring::npos)) return false;

    wchar_t full[MAX_PATH * 2] = {};
    const DWORD fl = GetFullPathNameW(w.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (fl == 0 || fl >= sizeof(full) / sizeof(full[0])) return false;
    std::wstring f(full);
    for (wchar_t& c : f) if (c == L'/') c = L'\\';

    const std::wstring root = std::wstring(exeDir);
    const std::wstring dataRoot = root + L"\\urushi_data\\";
    const std::wstring scriptsRoot = root + L"\\scripts\\";

    std::wstring rest;
    bool inData = false;
    if (HasPrefixNoCase(f, dataRoot)) { inData = true; rest = f.substr(dataRoot.size()); }
    else if (HasPrefixNoCase(f, scriptsRoot)) { rest = f.substr(scriptsRoot.size()); }
    else return false;

    if (write && !inData) return false;     // scripts/ is read-only for scripts: nothing can create or alter code

    // every component of the remainder must be a plain name
    size_t start = 0;
    std::wstring last;
    int comps = 0;
    while (start <= rest.size()) {
        size_t slash = rest.find(L'\\', start);
        if (slash == std::wstring::npos) slash = rest.size();
        last = rest.substr(start, slash - start);
        if (!ValidComponent(last) || ++comps > 6) return false;
        start = slash + 1;
    }
    if (!AllowedDataExt(last)) return false;   // never .lua/.dll/.exe/.bat/... (and no extension-less files)

    if (write) {
        EnsureDirs(dataRoot.substr(0, dataRoot.size() - 1));
        // refuse writes through links, and cap the total size of the data folder
        const DWORD attr = GetFileAttributesW(f.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;
        if (DirSize(dataRoot.substr(0, dataRoot.size() - 1), 0) > kDataBudgetBytes) return false;
        const size_t lastSlash = f.rfind(L'\\');
        if (lastSlash != std::wstring::npos) EnsureDirs(f.substr(0, lastSlash));
    }
    if (outFull) *outFull = f;
    return true;
}

void OpenSafeLibs(lua_State* L) {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_exeDir = buf;
    const size_t slash = g_exeDir.rfind(L'\\');
    if (slash != std::wstring::npos) g_exeDir.resize(slash);

    // Only the libraries we want. NOT opened: package (require / C loaders).
    // os, io and debug are opened first so that we can copy the safe subset out of them.
    static const luaL_Reg kLibs[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_OSLIBNAME, luaopen_os },
        { LUA_IOLIBNAME, luaopen_io },
        { LUA_DBLIBNAME, luaopen_debug },
        { nullptr, nullptr }
    };
    for (const luaL_Reg* lib = kLibs; lib->func; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    // ---- base: no dofile/loadfile/collectgarbage, text-only load
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "collectgarbage");
    lua_getglobal(L, "load");
    lua_pushcclosure(L, SafeLoad, 1);
    lua_setglobal(L, "load");

    // ---- string.dump produces bytecode
    lua_getglobal(L, "string");
    lua_pushnil(L); lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    // ---- os: time functions only (no execute/remove/rename/exit/getenv/tmpname/setlocale)
    {
        static const char* const kOs[] = { "time", "clock", "date", "difftime", nullptr };
        lua_getglobal(L, "os");
        lua_newtable(L);
        CopyFields(L, -2, kOs, -1);
        lua_setglobal(L, "os");
        lua_pop(L, 1);
    }

    // ---- debug: traceback only (setupvalue/sethook/getregistry would defeat the sandbox)
    {
        static const char* const kDbg[] = { "traceback", nullptr };
        lua_getglobal(L, "debug");
        lua_newtable(L);
        CopyFields(L, -2, kDbg, -1);
        lua_setglobal(L, "debug");
        lua_pop(L, 1);
    }

    // ---- io: open (restricted) + type. No popen/tmpfile/lines/input/output/stdout.
    {
        lua_getglobal(L, "io");
        lua_newtable(L);
        lua_getfield(L, -2, "open");
        lua_pushcclosure(L, SafeOpen, 1);
        lua_setfield(L, -2, "open");
        lua_getfield(L, -2, "type");
        lua_setfield(L, -2, "type");
        lua_setglobal(L, "io");
        lua_pop(L, 1);
    }
}

} // namespace LuaHardening
