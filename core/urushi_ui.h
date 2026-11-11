#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <imgui.h>

struct lua_State;

class UrushiUI {
public:
    enum class Accent : int {
        Amber = 0,     // Default URUSHI warm amber (#E59A38)
        Vermilion = 1, // Japanese lacquer red (#E04832)
        Platinum = 2,  // Monochrome silver (#E4E4EC)
        Indigo = 3,    // Restrained dark indigo (#8B7CF6)
        Custom = 4     // User-selected custom RGB accent
    };

    enum class Language : int {
        Russian = 0,
        English = 1
    };

    enum class ButtonStyle : int {
        Default = 0,
        Primary = 1,
        Danger = 2,
        Ghost = 3
    };

    enum class Tone : int {
        Default = 0,
        Muted = 1,
        Accent = 2,
        Green = 3,
        Red = 4,
        Yellow = 5
    };

    struct StatItem {
        std::string label;
        std::string value;
        Tone tone = Tone::Default;
    };

    struct WindowRegistration {
        std::string id;
        std::string title;
        std::string icon = "settings";
        int order = 100;
        bool defaultOpen = false;
        bool defaultPinned = false;
        bool hudNoInputs = false;
        bool anchorRight = false;
        float defaultX = 264.0f;
        float defaultY = 36.0f;
        float width = 410.0f;
        int luaRenderRef = -1;
    };

    struct WindowDebugInfo {
        std::string id;
        std::string title;
        std::string icon;
        int order = 100;
        bool open = false;
        bool pinned = false;
        bool shaded = false;
        bool hudNoInputs = false;
        bool scrollable = false;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float actualW = 0.0f;
        float actualH = 0.0f;
        float bodyH = 0.0f;
    };

    struct DebugSnapshot {
        float dockX = 0.0f;
        float dockY = 0.0f;
        float dockW = 0.0f;
        float dockH = 0.0f;
        float workW = 0.0f;
        float workH = 0.0f;
        float uiScale = 1.0f;
        float autoScale = 1.0f;
        float userScale = 0.0f;
        std::vector<WindowDebugInfo> windows;
    };

    static void Init();
    static void Shutdown();
    static void ApplyBaseTheme();
    static void PrepareFrame();

    // Scale & multi-resolution support (0.0f = Auto from resolution & DPI)
    static void SetUserScale(float scale);
    static float GetUserScale();
    static float GetAutoScale();
    static float GetUiScale();
    static void SetPreviewViewport(float w, float h, float forcedScale = -1.0f);
    static void ClearPreviewViewport();

    // Preserves window geometry/open/pin state across Lua FileWatcher reloads
    static void BeginLuaReload();
    static void EndLuaReload();
    static void ClearLuaRefs(lua_State* L);

    static void RegisterWindow(const WindowRegistration& reg);
    static void Render(bool menuOpen, lua_State* L);

    // Window & shell controls
    static void SetWindowOpen(const std::string& id, bool open);
    static bool ToggleWindow(const std::string& id);
    static bool IsWindowOpen(const std::string& id);
    static void SetWindowPinned(const std::string& id, bool pinned);
    static void SetWindowShaded(const std::string& id, bool shaded);
    static void SetWindowPos(const std::string& id, float x, float y);
    static void SetWindowTitle(const std::string& id, const std::string& title);
    static void CloseAllUnpinned();
    static DebugSnapshot GetDebugSnapshot();

    static void SetSingleTabMode(bool enabled);
    static bool GetSingleTabMode();

    static void SetLanguage(Language lang);
    static bool SetLanguageByName(const std::string& name);
    static Language GetLanguage();
    static std::string GetLanguageName();
    static bool IsEnglish();

    static void SetAccent(Accent accent);
    static bool SetAccentByName(const std::string& name);
    static Accent GetAccent();
    static std::string GetAccentName();
    static void SetCustomAccent(float r, float g, float b);
    static void GetCustomAccent(float& r, float& g, float& b);

    // Shared theme palette accessors (used by Updater, UiSchema, widgets)
    static ImU32 GetAccentU32(float alphaMul = 1.0f);
    static ImU32 GetAccentHoverU32(float alphaMul = 1.0f);
    static ImU32 GetAccentFgU32(float alphaMul = 1.0f);
    static ImU32 GetAccentSoftU32();
    static ImU32 GetAccentLineU32();
    static ImVec4 GetAccentVec4(float alpha = 1.0f);
    static ImVec4 GetToneVec4(Tone tone);
    static ImU32 GetToneU32(Tone tone, float alphaMul = 1.0f);

    // Lacquered ImDrawList Widgets
    static void BeginCard(const std::string& title = "", const std::string& subtitle = "");
    static void EndCard();

    static bool Toggle(const std::string& label, bool* value);
    static bool SliderFloat(const std::string& label, float* value, float minVal, float maxVal, const char* fmt = "%.2f");
    static bool SliderInt(const std::string& label, int* value, int minVal, int maxVal, const char* fmt = "%d");
    static bool Segmented(const std::string& id, int* currentIdx, const std::vector<std::string>& options);
    static bool Button(const std::string& label, float width = 0.0f, float height = 30.0f, ButtonStyle style = ButtonStyle::Default);
    static void StatRow(const std::vector<StatItem>& items);
    static bool CollapsingSection(const std::string& label, bool defaultOpen = false);

    static bool Combo(const std::string& label, int* currentIdx, const std::vector<std::string>& items);
    static bool InputText(const std::string& label, std::string& text, size_t maxLen = 256);
    static bool ColorEdit3(const std::string& label, float rgb[3]);
    static bool ColorEdit4(const std::string& label, float rgba[4]);
    static void ProgressBar(float fraction, float width = -1.0f, float height = 10.0f, const char* overlay = nullptr);

    static void Badge(const std::string& text, Tone tone = Tone::Accent);
    static void Keycap(const std::string& keyText);
    static void Text(const std::string& text, Tone tone = Tone::Default);
    static void TextWrapped(const std::string& text, Tone tone = Tone::Default);
    static void TextColored(const ImVec4& col, const std::string& text);
    static void Separator();
    static void Spacing(float px = 6.0f);

    static float CurrentContentWidth();
};
