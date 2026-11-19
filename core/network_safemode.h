#pragma once

#include <cstdint>

namespace NetworkSafeMode {

struct Status {
    bool hooksInstalled = false;
    bool inLobby = false;       // ApplyNetworkSafeModeController is active (online multiplayer session)
    bool safeModeActive = false;// NetworkCarSafeModeModel.get_safeMode()
    uint32_t triggersTotal = 0;
};

// Resolves IL2CPP methods and hooks ApplyNetworkSafeModeController::OnActivate / OnDeactivate.
bool Init();

// Removes hooks and clears cached pointers.
void Shutdown();

// Thread-safe request to activate native network SafeMode for `durationMs` (clamped 500..10000 ms).
// Executed on the Unity Main Thread inside TickOnMainThread().
void Trigger(int durationMs, bool forceImmediate = false);

// Optional explicit deactivation (cancels our SafeMode source early).
void Deactivate();

// Must be called on the Unity Main Thread (e.g. from EcsCollision::Update).
void TickOnMainThread();

Status GetStatus();

} // namespace NetworkSafeMode
