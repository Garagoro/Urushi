#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#ifndef URUSHI_VERSION
#define URUSHI_VERSION "1.2.0"
#endif

namespace Updater {

enum class State : int {
    Idle = 0,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    Reinjecting,
    RestartRequired,
    Error
};

struct PendingFileInfo {
    std::string repoPath;
    uint64_t size = 0;
    bool isProxy = false;
};

struct StatusSnapshot {
    State state = State::Idle;
    std::string localVersion;
    std::string remoteVersion;
    std::string releaseNotes;
    std::string statusText;
    std::string currentFile;
    uint32_t localLinkTime = 0;
    uint32_t remoteLinkTime = 0;
    bool localBuildNewer = false;
    bool proxyNeedsRestart = false;
    bool bannerDismissed = false;
    uint32_t filesTotal = 0;
    uint32_t filesDone = 0;
    uint64_t bytesTotal = 0;
    uint64_t bytesDone = 0;
    std::vector<PendingFileInfo> files;
};

// Initializes updater state and kicks off an async startup version check.
void Init(const std::wstring& exeDir, HMODULE selfModule);

// Cancels background HTTP/install work and joins the worker thread safely before DLL unload.
void Shutdown();

// Triggers an async check against GitHub (no-op if already checking/downloading).
void CheckAsync(bool userInitiated = false);

// Downloads pending files atomically and triggers hot-reinject via Local\UrushiReload_<pid>.
void StartInstallAsync();

// Triggers hot-reinject via Local\UrushiReload_<pid> on demand.
void Reinject();

// Launches delayed Steam relaunch (AppID 1826420) and exits the current game process.
void RestartGame();

// Shows/hides the floating update banner.
void SetDismissed(bool dismissed);

// Thread-safe snapshot for ImGui / Lua bindings.
StatusSnapshot GetStatus();

// Renders the non-intrusive update notification banner in D3D11 Present.
void RenderUI(bool menuOpen);

} // namespace Updater
