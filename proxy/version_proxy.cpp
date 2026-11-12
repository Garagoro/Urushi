#include <windows.h>
#include <shlwapi.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")

static HMODULE g_realVersionModule = nullptr;
static HMODULE g_coreHookModule = nullptr;
static HANDLE g_workerThread = nullptr;
static HANDLE g_reloadEvent = nullptr;
static bool g_running = true;
static std::wstring g_loadedShadowDll;
static std::wstring g_loadedShadowPdb;
static DWORD g_shadowSeq = 0;

// Original function pointer types
typedef BOOL(WINAPI* t_GetFileVersionInfoA)(LPCSTR, DWORD, DWORD, LPVOID);
typedef int(WINAPI* t_GetFileVersionInfoByHandle)(int, int, int);
typedef BOOL(WINAPI* t_GetFileVersionInfoExA)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI* t_GetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeA)(LPCSTR, LPDWORD);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeExA)(DWORD, LPCSTR, LPDWORD);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* t_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI* t_VerFindFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
typedef DWORD(WINAPI* t_VerFindFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD(WINAPI* t_VerInstallFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
typedef DWORD(WINAPI* t_VerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD(WINAPI* t_VerLanguageNameA)(DWORD, LPSTR, DWORD);
typedef DWORD(WINAPI* t_VerLanguageNameW)(DWORD, LPWSTR, DWORD);
typedef BOOL(WINAPI* t_VerQueryValueA)(LPCVOID, LPCSTR, LPVOID*, PUINT);
typedef BOOL(WINAPI* t_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

static t_GetFileVersionInfoA pfn_GetFileVersionInfoA = nullptr;
static t_GetFileVersionInfoByHandle pfn_GetFileVersionInfoByHandle = nullptr;
static t_GetFileVersionInfoExA pfn_GetFileVersionInfoExA = nullptr;
static t_GetFileVersionInfoExW pfn_GetFileVersionInfoExW = nullptr;
static t_GetFileVersionInfoSizeA pfn_GetFileVersionInfoSizeA = nullptr;
static t_GetFileVersionInfoSizeExA pfn_GetFileVersionInfoSizeExA = nullptr;
static t_GetFileVersionInfoSizeExW pfn_GetFileVersionInfoSizeExW = nullptr;
static t_GetFileVersionInfoSizeW pfn_GetFileVersionInfoSizeW = nullptr;
static t_GetFileVersionInfoW pfn_GetFileVersionInfoW = nullptr;
static t_VerFindFileA pfn_VerFindFileA = nullptr;
static t_VerFindFileW pfn_VerFindFileW = nullptr;
static t_VerInstallFileA pfn_VerInstallFileA = nullptr;
static t_VerInstallFileW pfn_VerInstallFileW = nullptr;
static t_VerLanguageNameA pfn_VerLanguageNameA = nullptr;
static t_VerLanguageNameW pfn_VerLanguageNameW = nullptr;
static t_VerQueryValueA pfn_VerQueryValueA = nullptr;
static t_VerQueryValueW pfn_VerQueryValueW = nullptr;

static void LoadRealVersionDll() {
    if (g_realVersionModule) return;

    WCHAR systemDir[MAX_PATH];
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring realDllPath = std::wstring(systemDir) + L"\\version.dll";

    g_realVersionModule = LoadLibraryW(realDllPath.c_str());
    if (!g_realVersionModule) return;

    pfn_GetFileVersionInfoA = (t_GetFileVersionInfoA)GetProcAddress(g_realVersionModule, "GetFileVersionInfoA");
    pfn_GetFileVersionInfoByHandle = (t_GetFileVersionInfoByHandle)GetProcAddress(g_realVersionModule, "GetFileVersionInfoByHandle");
    pfn_GetFileVersionInfoExA = (t_GetFileVersionInfoExA)GetProcAddress(g_realVersionModule, "GetFileVersionInfoExA");
    pfn_GetFileVersionInfoExW = (t_GetFileVersionInfoExW)GetProcAddress(g_realVersionModule, "GetFileVersionInfoExW");
    pfn_GetFileVersionInfoSizeA = (t_GetFileVersionInfoSizeA)GetProcAddress(g_realVersionModule, "GetFileVersionInfoSizeA");
    pfn_GetFileVersionInfoSizeExA = (t_GetFileVersionInfoSizeExA)GetProcAddress(g_realVersionModule, "GetFileVersionInfoSizeExA");
    pfn_GetFileVersionInfoSizeExW = (t_GetFileVersionInfoSizeExW)GetProcAddress(g_realVersionModule, "GetFileVersionInfoSizeExW");
    pfn_GetFileVersionInfoSizeW = (t_GetFileVersionInfoSizeW)GetProcAddress(g_realVersionModule, "GetFileVersionInfoSizeW");
    pfn_GetFileVersionInfoW = (t_GetFileVersionInfoW)GetProcAddress(g_realVersionModule, "GetFileVersionInfoW");
    pfn_VerFindFileA = (t_VerFindFileA)GetProcAddress(g_realVersionModule, "VerFindFileA");
    pfn_VerFindFileW = (t_VerFindFileW)GetProcAddress(g_realVersionModule, "VerFindFileW");
    pfn_VerInstallFileA = (t_VerInstallFileA)GetProcAddress(g_realVersionModule, "VerInstallFileA");
    pfn_VerInstallFileW = (t_VerInstallFileW)GetProcAddress(g_realVersionModule, "VerInstallFileW");
    pfn_VerLanguageNameA = (t_VerLanguageNameA)GetProcAddress(g_realVersionModule, "VerLanguageNameA");
    pfn_VerLanguageNameW = (t_VerLanguageNameW)GetProcAddress(g_realVersionModule, "VerLanguageNameW");
    pfn_VerQueryValueA = (t_VerQueryValueA)GetProcAddress(g_realVersionModule, "VerQueryValueA");
    pfn_VerQueryValueW = (t_VerQueryValueW)GetProcAddress(g_realVersionModule, "VerQueryValueW");
}

extern "C" {
    BOOL WINAPI _Proxy_GetFileVersionInfoA(LPCSTR filename, DWORD handle, DWORD len, LPVOID data) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoA ? pfn_GetFileVersionInfoA(filename, handle, len, data) : FALSE;
    }
    int WINAPI _Proxy_GetFileVersionInfoByHandle(int h, int a, int b) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoByHandle ? pfn_GetFileVersionInfoByHandle(h, a, b) : 0;
    }
    BOOL WINAPI _Proxy_GetFileVersionInfoExA(DWORD flags, LPCSTR filename, DWORD handle, DWORD len, LPVOID data) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoExA ? pfn_GetFileVersionInfoExA(flags, filename, handle, len, data) : FALSE;
    }
    BOOL WINAPI _Proxy_GetFileVersionInfoExW(DWORD flags, LPCWSTR filename, DWORD handle, DWORD len, LPVOID data) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoExW ? pfn_GetFileVersionInfoExW(flags, filename, handle, len, data) : FALSE;
    }
    DWORD WINAPI _Proxy_GetFileVersionInfoSizeA(LPCSTR filename, LPDWORD handle) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoSizeA ? pfn_GetFileVersionInfoSizeA(filename, handle) : 0;
    }
    DWORD WINAPI _Proxy_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR filename, LPDWORD handle) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoSizeExA ? pfn_GetFileVersionInfoSizeExA(flags, filename, handle) : 0;
    }
    DWORD WINAPI _Proxy_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR filename, LPDWORD handle) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoSizeExW ? pfn_GetFileVersionInfoSizeExW(flags, filename, handle) : 0;
    }
    DWORD WINAPI _Proxy_GetFileVersionInfoSizeW(LPCWSTR filename, LPDWORD handle) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoSizeW ? pfn_GetFileVersionInfoSizeW(filename, handle) : 0;
    }
    BOOL WINAPI _Proxy_GetFileVersionInfoW(LPCWSTR filename, DWORD handle, DWORD len, LPVOID data) {
        LoadRealVersionDll();
        return pfn_GetFileVersionInfoW ? pfn_GetFileVersionInfoW(filename, handle, len, data) : FALSE;
    }
    DWORD WINAPI _Proxy_VerFindFileA(DWORD flags, LPCSTR filename, LPCSTR winDir, LPCSTR appDir, LPSTR curDir, PUINT curDirLen, LPSTR destDir, PUINT destDirLen) {
        LoadRealVersionDll();
        return pfn_VerFindFileA ? pfn_VerFindFileA(flags, filename, winDir, appDir, curDir, curDirLen, destDir, destDirLen) : 0;
    }
    DWORD WINAPI _Proxy_VerFindFileW(DWORD flags, LPCWSTR filename, LPCWSTR winDir, LPCWSTR appDir, LPWSTR curDir, PUINT curDirLen, LPWSTR destDir, PUINT destDirLen) {
        LoadRealVersionDll();
        return pfn_VerFindFileW ? pfn_VerFindFileW(flags, filename, winDir, appDir, curDir, curDirLen, destDir, destDirLen) : 0;
    }
    DWORD WINAPI _Proxy_VerInstallFileA(DWORD flags, LPCSTR srcFileName, LPCSTR destFileName, LPCSTR srcDir, LPCSTR destDir, LPCSTR curDir, LPSTR tmpFile, PUINT tmpFileLen) {
        LoadRealVersionDll();
        return pfn_VerInstallFileA ? pfn_VerInstallFileA(flags, srcFileName, destFileName, srcDir, destDir, curDir, tmpFile, tmpFileLen) : 0;
    }
    DWORD WINAPI _Proxy_VerInstallFileW(DWORD flags, LPCWSTR srcFileName, LPCWSTR destFileName, LPCWSTR srcDir, LPCWSTR destDir, LPCWSTR curDir, LPWSTR tmpFile, PUINT tmpFileLen) {
        LoadRealVersionDll();
        return pfn_VerInstallFileW ? pfn_VerInstallFileW(flags, srcFileName, destFileName, srcDir, destDir, curDir, tmpFile, tmpFileLen) : 0;
    }
    DWORD WINAPI _Proxy_VerLanguageNameA(DWORD lang, LPSTR langName, DWORD size) {
        LoadRealVersionDll();
        return pfn_VerLanguageNameA ? pfn_VerLanguageNameA(lang, langName, size) : 0;
    }
    DWORD WINAPI _Proxy_VerLanguageNameW(DWORD lang, LPWSTR langName, DWORD size) {
        LoadRealVersionDll();
        return pfn_VerLanguageNameW ? pfn_VerLanguageNameW(lang, langName, size) : 0;
    }
    BOOL WINAPI _Proxy_VerQueryValueA(LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT len) {
        LoadRealVersionDll();
        return pfn_VerQueryValueA ? pfn_VerQueryValueA(block, subBlock, buffer, len) : FALSE;
    }
    BOOL WINAPI _Proxy_VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT len) {
        LoadRealVersionDll();
        return pfn_VerQueryValueW ? pfn_VerQueryValueW(block, subBlock, buffer, len) : FALSE;
    }
}

static std::wstring GetExecutableDirectory() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

static std::wstring GetShadowDirectory() {
    WCHAR tempDir[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempDir);
    if (len == 0 || len >= MAX_PATH) {
        return GetExecutableDirectory() + L"\\.shadow";
    }
    std::wstring dir(tempDir);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir += L'\\';
    }
    dir += L"urushi_live";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static void SweepOldShadowFiles(const std::wstring& shadowDir) {
    const wchar_t* patterns[] = { L"\\urushi_*.*", L"\\CarX2ScriptHook_*.*" };
    for (const auto* pat : patterns) {
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW((shadowDir + pat).c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring full = shadowDir + L"\\" + fd.cFileName;
                    DeleteFileW(full.c_str());
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
}

static void UnloadCoreModule() {
    if (g_coreHookModule) {
        typedef void (*t_Unload)();
        t_Unload pUnload = (t_Unload)GetProcAddress(g_coreHookModule, "ScriptHook_Unload");
        if (pUnload) {
            pUnload();
        }
        FreeLibrary(g_coreHookModule);
        g_coreHookModule = nullptr;
        Sleep(50);
    }
    if (!g_loadedShadowDll.empty()) {
        DeleteFileW(g_loadedShadowDll.c_str());
        g_loadedShadowDll.clear();
    }
    if (!g_loadedShadowPdb.empty()) {
        DeleteFileW(g_loadedShadowPdb.c_str());
        g_loadedShadowPdb.clear();
    }
}

static void LoadCoreShadow(const std::wstring& exeDir) {
    if (g_coreHookModule) return;
    std::wstring srcDll = exeDir + L"\\urushi.dll";
    if (GetFileAttributesW(srcDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        srcDll = exeDir + L"\\CarX2ScriptHook.dll";
    }
    if (GetFileAttributesW(srcDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    std::wstring shadowDir = GetShadowDirectory();
    const DWORD pid = GetCurrentProcessId();
    const DWORD seq = ++g_shadowSeq;
    wchar_t nameBuf[128] = {};
    wsprintfW(nameBuf, L"\\urushi_%lu_%lu", pid, seq);
    std::wstring shadowDll = shadowDir + nameBuf + L".dll";
    std::wstring shadowPdb = shadowDir + nameBuf + L".pdb";

    // Shadow-copy so the game process NEVER locks <gameDir>\urushi.dll
    if (CopyFileW(srcDll.c_str(), shadowDll.c_str(), FALSE)) {
        std::wstring srcPdb = exeDir + L"\\urushi.pdb";
        if (GetFileAttributesW(srcPdb.c_str()) == INVALID_FILE_ATTRIBUTES) {
            srcPdb = exeDir + L"\\CarX2ScriptHook.pdb";
        }
        if (GetFileAttributesW(srcPdb.c_str()) != INVALID_FILE_ATTRIBUTES &&
            CopyFileW(srcPdb.c_str(), shadowPdb.c_str(), FALSE)) {
            g_loadedShadowPdb = shadowPdb;
        }
        g_coreHookModule = LoadLibraryW(shadowDll.c_str());
        if (g_coreHookModule) {
            g_loadedShadowDll = shadowDll;
            return;
        }
        DeleteFileW(shadowDll.c_str());
        if (!g_loadedShadowPdb.empty()) {
            DeleteFileW(g_loadedShadowPdb.c_str());
            g_loadedShadowPdb.clear();
        }
    }

    // Fallback to direct load only if shadow copy failed
    g_coreHookModule = LoadLibraryW(srcDll.c_str());
}

static DWORD WINAPI LoaderThread(LPVOID) {
    // Wait for the game and Unity subsystems to be initialized
    while (!GetModuleHandleW(L"UnityPlayer.dll") && g_running) {
        Sleep(100);
    }
    if (!g_running) return 0;

    // Give Unity another 1.5 seconds to settle graphics & audio
    Sleep(1500);
    if (!g_running) return 0;

    std::wstring exeDir = GetExecutableDirectory();
    // Clean up any staged version.dll.old left over after a proxy self-update
    DeleteFileW((exeDir + L"\\version.dll.old").c_str());

    wchar_t evName[64] = {};
    wsprintfW(evName, L"Local\\UrushiReload_%lu", GetCurrentProcessId());
    g_reloadEvent = CreateEventW(nullptr, FALSE, FALSE, evName);

    SweepOldShadowFiles(GetShadowDirectory());
    LoadCoreShadow(exeDir);

    bool prevCtrlF10 = false;
    bool prevCtrlF11 = false;

    // Ctrl+F10: cleanly detach/unload CarX2ScriptHook.dll from process
    // Ctrl+F11 or UrushiReload_<pid> event: hot-reinject fresh shadow copy from game folder
    while (g_running) {
        DWORD waitMs = g_reloadEvent ? WaitForSingleObject(g_reloadEvent, 50) : (Sleep(50), WAIT_TIMEOUT);
        if (!g_running) break;

        if (waitMs == WAIT_OBJECT_0) {
            Sleep(120);
            UnloadCoreModule();
            Sleep(150);
            LoadCoreShadow(exeDir);
            continue;
        }

        const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

        const bool ctrlF10 = ctrlDown && f10Down;
        const bool ctrlF11 = ctrlDown && f11Down;

        if (ctrlF10 && !prevCtrlF10) {
            UnloadCoreModule();
        }
        if (ctrlF11 && !prevCtrlF11) {
            UnloadCoreModule();
            Sleep(150);
            LoadCoreShadow(exeDir);
        }

        prevCtrlF10 = ctrlF10;
        prevCtrlF11 = ctrlF11;
    }

    if (g_reloadEvent) {
        CloseHandle(g_reloadEvent);
        g_reloadEvent = nullptr;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        g_workerThread = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_running = false;
        if (lpvReserved == nullptr) {
            if (g_reloadEvent) SetEvent(g_reloadEvent);
            if (g_workerThread) {
                WaitForSingleObject(g_workerThread, 1500);
                CloseHandle(g_workerThread);
                g_workerThread = nullptr;
            }
            UnloadCoreModule();
            if (g_realVersionModule) {
                FreeLibrary(g_realVersionModule);
                g_realVersionModule = nullptr;
            }
        }
        break;
    }
    return TRUE;
}
