#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <array>
#include <mutex>

enum class InputSource : int {
    None = 0,
    Keyboard = 1,
    Gamepad = 2
};

// Gamepad button bitmasks (compatible with XINPUT_GAMEPAD_*)
constexpr int GP_BTN_DPAD_UP        = 0x0001;
constexpr int GP_BTN_DPAD_DOWN      = 0x0002;
constexpr int GP_BTN_DPAD_LEFT      = 0x0004;
constexpr int GP_BTN_DPAD_RIGHT     = 0x0008;
constexpr int GP_BTN_START          = 0x0010;
constexpr int GP_BTN_BACK           = 0x0020;
constexpr int GP_BTN_LEFT_THUMB     = 0x0040; // L3
constexpr int GP_BTN_RIGHT_THUMB    = 0x0080; // R3
constexpr int GP_BTN_LB             = 0x0100;
constexpr int GP_BTN_RB             = 0x0200;
constexpr int GP_BTN_LT             = 0x0400; // Trigger threshold
constexpr int GP_BTN_RT             = 0x0800; // Trigger threshold
constexpr int GP_BTN_A              = 0x1000;
constexpr int GP_BTN_B              = 0x2000;
constexpr int GP_BTN_X              = 0x4000;
constexpr int GP_BTN_Y              = 0x8000;

struct InputBinding {
    InputSource source = InputSource::None;
    int code = 0;       // VK code or Gamepad button bitmask
    int modCode = 0;    // Optional modifier (LB, RB, Ctrl, Shift, etc.)
    std::string name;   // Display name (cached or custom)

    bool IsValid() const {
        return source != InputSource::None && code != 0;
    }

    void Clear() {
        source = InputSource::None;
        code = 0;
        modCode = 0;
        name.clear();
    }
};

struct GamepadStateSnapshot {
    bool connected = false;
    int slot = -1;
    int buttons = 0;
    int buttonsPressed = 0;
    int buttonsReleased = 0;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    float leftThumbX = 0.0f;
    float leftThumbY = 0.0f;
    float rightThumbX = 0.0f;
    float rightThumbY = 0.0f;
};

class InputManager {
public:
    static void Init();
    static void Shutdown();
    static void Update(); // Called once per frame in Hooked_Present
    static void OnKeyEvent(int vkey, bool isDown);

    // Polling queries
    static bool IsKeyDown(int vkey);
    static bool IsKeyPressed(int vkey);
    static bool IsKeyReleased(int vkey);

    static bool IsGamepadConnected();
    static GamepadStateSnapshot GetGamepadState();
    static bool IsGamepadDown(int btnMask);
    static bool IsGamepadPressed(int btnMask);
    static bool IsGamepadReleased(int btnMask);

    // High level binding check
    static bool IsBindingDown(const InputBinding& bind);
    static bool IsBindingPressed(const InputBinding& bind);

    // Capture helper for listening keybind widget
    // Returns true if a key or button was pressed, filling outBind.
    // If Escape was pressed, returns true with outBind.source == InputSource::None (cancelled).
    // If Backspace/Delete was pressed, returns true with outBind.source == InputSource::None (cleared).
    static bool PollAnyPressed(InputBinding& outBind, bool& outCancelled, bool& outCleared);

    // Menu toggle binding (native fallback + configurable)
    static InputBinding GetMenuToggleBinding();
    static void SetMenuToggleBinding(const InputBinding& bind);

    // Helpers
    static std::string GetKeyName(int vkey);
    static std::string GetGamepadButtonName(int btnMask);
    static std::string FormatBinding(const InputBinding& bind);

private:
    static void PollGamepad();
    static void UpdateKeyTransitions();

    static std::recursive_mutex s_mutex;
    static bool s_initialized;

    // Keyboard state
    static std::array<bool, 256> s_keysDown;
    static std::array<bool, 256> s_keysPressed;
    static std::array<bool, 256> s_keysReleased;
    static std::vector<int> s_keysPressedQueue;
    static std::vector<int> s_keysReleasedQueue;

    // Gamepad state
    static HMODULE s_hXInput;
    static void* s_pfnXInputGetState;
    static GamepadStateSnapshot s_gpSnapshot;
    static int s_prevGpButtons;

    // Menu toggle
    static InputBinding s_menuToggleBind;
};
