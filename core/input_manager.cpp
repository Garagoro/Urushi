#include "input_manager.h"
#include "d3d11_hook.h"
#include "logger.h"
#include <algorithm>
#include <sstream>

#pragma comment(lib, "user32.lib")

// XInput structures if not including <xinput.h>
#pragma pack(push, 8)
typedef struct _XINPUT_GAMEPAD_LOCAL {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD_LOCAL;

typedef struct _XINPUT_STATE_LOCAL {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD_LOCAL Gamepad;
} XINPUT_STATE_LOCAL;
#pragma pack(pop)

typedef DWORD (WINAPI *t_XInputGetState)(DWORD dwUserIndex, XINPUT_STATE_LOCAL* pState);

std::recursive_mutex InputManager::s_mutex;
bool InputManager::s_initialized = false;

std::array<bool, 256> InputManager::s_keysDown = {};
std::array<bool, 256> InputManager::s_keysPressed = {};
std::array<bool, 256> InputManager::s_keysReleased = {};
std::vector<int> InputManager::s_keysPressedQueue;
std::vector<int> InputManager::s_keysReleasedQueue;

HMODULE InputManager::s_hXInput = nullptr;
void* InputManager::s_pfnXInputGetState = nullptr;
GamepadStateSnapshot InputManager::s_gpSnapshot = {};
int InputManager::s_prevGpButtons = 0;

InputBinding InputManager::s_menuToggleBind = { InputSource::Keyboard, VK_F4, 0, "F4" };

static float NormalizeThumb(SHORT val, SHORT deadzone) {
    if (std::abs(val) <= deadzone) return 0.0f;
    float sign = (val > 0) ? 1.0f : -1.0f;
    float mag = static_cast<float>(std::abs(val) - deadzone) / static_cast<float>(32767 - deadzone);
    return std::clamp(sign * mag, -1.0f, 1.0f);
}

void InputManager::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_initialized) return;

    s_keysDown.fill(false);
    s_keysPressed.fill(false);
    s_keysReleased.fill(false);
    s_keysPressedQueue.clear();
    s_keysReleasedQueue.clear();

    // Dynamically locate XInput (already loaded in CarX 2)
    s_hXInput = GetModuleHandleW(L"xinput1_4.dll");
    if (!s_hXInput) s_hXInput = LoadLibraryW(L"xinput1_4.dll");
    if (!s_hXInput) s_hXInput = LoadLibraryW(L"xinput1_3.dll");
    if (!s_hXInput) s_hXInput = LoadLibraryW(L"xinput9_1_0.dll");

    if (s_hXInput) {
        s_pfnXInputGetState = reinterpret_cast<void*>(GetProcAddress(s_hXInput, "XInputGetState"));
        Logger::Info("InputManager: XInput initialized successfully.");
    } else {
        Logger::Warn("InputManager: XInput library not found; gamepad input disabled.");
    }

    s_initialized = true;
}

void InputManager::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_initialized = false;
    s_pfnXInputGetState = nullptr;
    s_hXInput = nullptr;
}

void InputManager::OnKeyEvent(int vkey, bool isDown) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (vkey < 0 || vkey >= 256) return;

    if (isDown) {
        if (!s_keysDown[vkey]) {
            s_keysPressedQueue.push_back(vkey);
        }
        s_keysDown[vkey] = true;
    } else {
        if (s_keysDown[vkey]) {
            s_keysReleasedQueue.push_back(vkey);
        }
        s_keysDown[vkey] = false;
    }
}

void InputManager::UpdateKeyTransitions() {
    s_keysPressed.fill(false);
    for (int vk : s_keysPressedQueue) {
        if (vk >= 0 && vk < 256) s_keysPressed[vk] = true;
    }
    s_keysPressedQueue.clear();

    s_keysReleased.fill(false);
    for (int vk : s_keysReleasedQueue) {
        if (vk >= 0 && vk < 256) s_keysReleased[vk] = true;
    }
    s_keysReleasedQueue.clear();
}

void InputManager::PollGamepad() {
    if (!s_pfnXInputGetState) {
        s_gpSnapshot.connected = false;
        return;
    }

    auto pfn = reinterpret_cast<t_XInputGetState>(s_pfnXInputGetState);
    XINPUT_STATE_LOCAL state = {};
    int activeSlot = -1;

    for (DWORD i = 0; i < 4; ++i) {
        if (pfn(i, &state) == 0) { // ERROR_SUCCESS == 0
            activeSlot = static_cast<int>(i);
            break;
        }
    }

    s_gpSnapshot.connected = (activeSlot >= 0);
    s_gpSnapshot.slot = activeSlot;

    if (!s_gpSnapshot.connected) {
        s_gpSnapshot.buttons = 0;
        s_gpSnapshot.buttonsPressed = 0;
        s_gpSnapshot.buttonsReleased = 0;
        s_gpSnapshot.leftTrigger = 0.0f;
        s_gpSnapshot.rightTrigger = 0.0f;
        s_gpSnapshot.leftThumbX = 0.0f;
        s_gpSnapshot.leftThumbY = 0.0f;
        s_gpSnapshot.rightThumbX = 0.0f;
        s_gpSnapshot.rightThumbY = 0.0f;
        s_prevGpButtons = 0;
        return;
    }

    int currentBtns = static_cast<int>(state.Gamepad.wButtons);
    if (state.Gamepad.bLeftTrigger > 50)  currentBtns |= GP_BTN_LT;
    if (state.Gamepad.bRightTrigger > 50) currentBtns |= GP_BTN_RT;

    s_gpSnapshot.buttons = currentBtns;
    s_gpSnapshot.buttonsPressed = currentBtns & (~s_prevGpButtons);
    s_gpSnapshot.buttonsReleased = (~currentBtns) & s_prevGpButtons;
    s_prevGpButtons = currentBtns;

    s_gpSnapshot.leftTrigger = static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
    s_gpSnapshot.rightTrigger = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
    s_gpSnapshot.leftThumbX = NormalizeThumb(state.Gamepad.sThumbLX, 7849);
    s_gpSnapshot.leftThumbY = NormalizeThumb(state.Gamepad.sThumbLY, 7849);
    s_gpSnapshot.rightThumbX = NormalizeThumb(state.Gamepad.sThumbRX, 8689);
    s_gpSnapshot.rightThumbY = NormalizeThumb(state.Gamepad.sThumbRY, 8689);
}

void InputManager::Update() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized) Init();

    UpdateKeyTransitions();
    PollGamepad();

    // Check menu toggle hotkey
    if (s_menuToggleBind.IsValid()) {
        if (IsBindingPressed(s_menuToggleBind)) {
            D3D11Hook::ToggleMenu();
        }
    }
}

bool InputManager::IsKeyDown(int vkey) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (vkey >= 0 && vkey < 256) return s_keysDown[vkey];
    return false;
}

bool InputManager::IsKeyPressed(int vkey) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (vkey >= 0 && vkey < 256) return s_keysPressed[vkey];
    return false;
}

bool InputManager::IsKeyReleased(int vkey) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (vkey >= 0 && vkey < 256) return s_keysReleased[vkey];
    return false;
}

bool InputManager::IsGamepadConnected() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_gpSnapshot.connected;
}

GamepadStateSnapshot InputManager::GetGamepadState() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_gpSnapshot;
}

bool InputManager::IsGamepadDown(int btnMask) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return (s_gpSnapshot.buttons & btnMask) == btnMask;
}

bool InputManager::IsGamepadPressed(int btnMask) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return (s_gpSnapshot.buttonsPressed & btnMask) == btnMask;
}

bool InputManager::IsGamepadReleased(int btnMask) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return (s_gpSnapshot.buttonsReleased & btnMask) == btnMask;
}

bool InputManager::IsBindingDown(const InputBinding& bind) {
    if (!bind.IsValid()) return false;
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    if (bind.source == InputSource::Keyboard) {
        bool mainDown = (bind.code >= 0 && bind.code < 256) && s_keysDown[bind.code];
        if (!mainDown) return false;
        if (bind.modCode != 0) {
            return (bind.modCode >= 0 && bind.modCode < 256) && s_keysDown[bind.modCode];
        }
        return true;
    } else if (bind.source == InputSource::Gamepad) {
        if (!s_gpSnapshot.connected) return false;
        bool mainDown = (s_gpSnapshot.buttons & bind.code) == bind.code;
        if (!mainDown) return false;
        if (bind.modCode != 0) {
            return (s_gpSnapshot.buttons & bind.modCode) == bind.modCode;
        }
        return true;
    }
    return false;
}

bool InputManager::IsBindingPressed(const InputBinding& bind) {
    if (!bind.IsValid()) return false;
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    if (bind.source == InputSource::Keyboard) {
        bool mainPressed = (bind.code >= 0 && bind.code < 256) && s_keysPressed[bind.code];
        if (!mainPressed) return false;
        if (bind.modCode != 0) {
            return (bind.modCode >= 0 && bind.modCode < 256) && s_keysDown[bind.modCode];
        }
        return true;
    } else if (bind.source == InputSource::Gamepad) {
        if (!s_gpSnapshot.connected) return false;
        bool mainPressed = (s_gpSnapshot.buttonsPressed & bind.code) == bind.code;
        if (!mainPressed) return false;
        if (bind.modCode != 0) {
            return (s_gpSnapshot.buttons & bind.modCode) == bind.modCode;
        }
        return true;
    }
    return false;
}

InputBinding InputManager::GetMenuToggleBinding() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_menuToggleBind;
}

void InputManager::SetMenuToggleBinding(const InputBinding& bind) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_menuToggleBind = bind;
}

bool InputManager::PollAnyPressed(InputBinding& outBind, bool& outCancelled, bool& outCleared) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    outCancelled = false;
    outCleared = false;

    // 1. Check Keyboard
    for (int vk = 1; vk < 255; ++vk) {
        if (s_keysPressed[vk]) {
            if (vk == VK_ESCAPE) {
                outCancelled = true;
                return true;
            }
            if (vk == VK_BACK || vk == VK_DELETE) {
                outCleared = true;
                outBind.Clear();
                return true;
            }
            // Skip pure modifier keys alone when pressed down if waiting for chord
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
                continue;
            }

            int mod = 0;
            if (s_keysDown[VK_CONTROL]) mod = VK_CONTROL;
            else if (s_keysDown[VK_SHIFT]) mod = VK_SHIFT;
            else if (s_keysDown[VK_MENU]) mod = VK_MENU;

            outBind.source = InputSource::Keyboard;
            outBind.code = vk;
            outBind.modCode = mod;
            outBind.name = FormatBinding(outBind);
            return true;
        }
    }

    // 2. Check Gamepad
    if (s_gpSnapshot.connected && s_gpSnapshot.buttonsPressed != 0) {
        static const int kCandidateButtons[] = {
            GP_BTN_A, GP_BTN_B, GP_BTN_X, GP_BTN_Y,
            GP_BTN_DPAD_UP, GP_BTN_DPAD_DOWN, GP_BTN_DPAD_LEFT, GP_BTN_DPAD_RIGHT,
            GP_BTN_BACK, GP_BTN_START,
            GP_BTN_LEFT_THUMB, GP_BTN_RIGHT_THUMB,
            GP_BTN_LB, GP_BTN_RB,
            GP_BTN_LT, GP_BTN_RT
        };

        for (int btn : kCandidateButtons) {
            if (s_gpSnapshot.buttonsPressed & btn) {
                int mod = 0;
                // If LB or RB was already held down when this other button was pressed
                if ((s_gpSnapshot.buttons & GP_BTN_LB) && btn != GP_BTN_LB) {
                    mod = GP_BTN_LB;
                } else if ((s_gpSnapshot.buttons & GP_BTN_RB) && btn != GP_BTN_RB) {
                    mod = GP_BTN_RB;
                }

                outBind.source = InputSource::Gamepad;
                outBind.code = btn;
                outBind.modCode = mod;
                outBind.name = FormatBinding(outBind);
                return true;
            }
        }
    }

    return false;
}

std::string InputManager::GetKeyName(int vkey) {
    if (vkey >= 'A' && vkey <= 'Z') return std::string(1, static_cast<char>(vkey));
    if (vkey >= '0' && vkey <= '9') return std::string(1, static_cast<char>(vkey));
    if (vkey >= VK_F1 && vkey <= VK_F12) return "F" + std::to_string(vkey - VK_F1 + 1);
    if (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9) return "Num " + std::to_string(vkey - VK_NUMPAD0);

    switch (vkey) {
    case VK_SPACE:    return "Space";
    case VK_TAB:      return "Tab";
    case VK_RETURN:   return "Enter";
    case VK_ESCAPE:   return "Esc";
    case VK_BACK:     return "Backspace";
    case VK_INSERT:   return "Insert";
    case VK_DELETE:   return "Delete";
    case VK_HOME:     return "Home";
    case VK_END:      return "End";
    case VK_PRIOR:    return "Page Up";
    case VK_NEXT:     return "Page Down";
    case VK_UP:       return "Up";
    case VK_DOWN:     return "Down";
    case VK_LEFT:     return "Left";
    case VK_RIGHT:    return "Right";
    case VK_CONTROL:  return "Ctrl";
    case VK_SHIFT:    return "Shift";
    case VK_MENU:     return "Alt";
    case VK_OEM_4:    return "[";
    case VK_OEM_6:    return "]";
    case VK_OEM_1:    return ";";
    case VK_OEM_7:    return "'";
    case VK_OEM_COMMA:  return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_MINUS:  return "-";
    case VK_OEM_PLUS:   return "=";
    case VK_OEM_2:    return "/";
    case VK_OEM_5:    return "\\";
    case VK_OEM_3:    return "`";
    default: {
        char buf[32];
        snprintf(buf, sizeof(buf), "Key %d", vkey);
        return buf;
    }
    }
}

std::string InputManager::GetGamepadButtonName(int btnMask) {
    switch (btnMask) {
    case GP_BTN_DPAD_UP:     return "Pad Up";
    case GP_BTN_DPAD_DOWN:   return "Pad Down";
    case GP_BTN_DPAD_LEFT:   return "Pad Left";
    case GP_BTN_DPAD_RIGHT:  return "Pad Right";
    case GP_BTN_START:       return "Start";
    case GP_BTN_BACK:        return "Back";
    case GP_BTN_LEFT_THUMB:  return "L3";
    case GP_BTN_RIGHT_THUMB: return "R3";
    case GP_BTN_LB:          return "LB";
    case GP_BTN_RB:          return "RB";
    case GP_BTN_LT:          return "LT";
    case GP_BTN_RT:          return "RT";
    case GP_BTN_A:           return "A";
    case GP_BTN_B:           return "B";
    case GP_BTN_X:           return "X";
    case GP_BTN_Y:           return "Y";
    default: {
        char buf[32];
        snprintf(buf, sizeof(buf), "Button 0x%X", btnMask);
        return buf;
    }
    }
}

std::string InputManager::FormatBinding(const InputBinding& bind) {
    if (!bind.IsValid()) return "None";

    std::ostringstream ss;
    if (bind.source == InputSource::Gamepad) {
        ss << "\xF0\x9F\x8E\xAE "; // 🎮 Controller glyph
        if (bind.modCode != 0) {
            ss << GetGamepadButtonName(bind.modCode) << " + ";
        }
        ss << GetGamepadButtonName(bind.code);
    } else if (bind.source == InputSource::Keyboard) {
        if (bind.modCode != 0) {
            ss << GetKeyName(bind.modCode) << " + ";
        }
        ss << GetKeyName(bind.code);
    }
    return ss.str();
}
