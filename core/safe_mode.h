#pragma once

// Safe mode keeps URUSHI loaded (D3D11 hook + ImGui + this UI) but keeps every feature off:
// no Lua scripts, no game/Unity/NativeCarX hooks, no CarFX, optimizer, replays, updater, dev pipe.
// The user can leave Safe mode at runtime, which starts the feature set without restarting the game.
namespace SafeMode {

enum class Mode {
    Booting,         // before the decision
    AwaitingChoice,  // previous session crashed, policy == ask: dialog is shown, nothing started yet
    Safe,            // features off
    Starting,        // leaving safe mode, features are being brought up
    Full             // normal operation
};

// Decide the startup mode from the previous crash marker, crash policy and the "safe next launch" flag.
// Returns true if the full feature set should start right away.
bool Decide();

// Mark the feature set as running (called by dllmain after StartFeatures on the normal path).
void MarkFull();

Mode GetMode();
bool IsFull();                 // features active (scripts, hooks, CarFX ...)
bool FeaturesStarted();        // Phase B has been started at least once (needed for correct shutdown)

// Called once from dllmain: the routine that brings the feature set up (runs on a worker thread).
void SetStartFeaturesCallback(void (*fn)());

// Leave Safe mode / answer the dialog "start normally".
void RequestFull(bool remember);
// Answer the dialog "stay in Safe mode".
void ChooseSafe(bool remember);

// Draw the dialog / safe-mode window. Call every frame between ImGui::NewFrame and ImGui::Render.
void RenderUI(bool menuOpen);

} // namespace SafeMode
