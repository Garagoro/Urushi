#pragma once

#include "urushi_ui.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace UrushiUiInternal {

// Quiet Chrome Lacquer Colors
constexpr ImU32 kColBgDock         = IM_COL32(11, 11, 15, 240);
constexpr ImU32 kColBgWindow       = IM_COL32(16, 16, 21, 240);
constexpr ImU32 kColBgHeader       = IM_COL32(13, 13, 17, 246);
constexpr ImU32 kColBgCard         = IM_COL32(23, 23, 28, 250);
constexpr ImU32 kColBgControl      = IM_COL32(14, 14, 18, 255);
constexpr ImU32 kColBgRaised       = IM_COL32(33, 33, 41, 255);
constexpr ImU32 kColBgRaisedHover  = IM_COL32(43, 43, 54, 255);
constexpr ImU32 kColBgRaisedActive = IM_COL32(52, 52, 64, 255);

constexpr ImU32 kColHairline       = IM_COL32(255, 255, 255, 16);
constexpr ImU32 kColHairlineStrong = IM_COL32(255, 255, 255, 28);

constexpr ImU32 kColTextPrimary    = IM_COL32(244, 244, 247, 255);
constexpr ImU32 kColTextSecondary  = IM_COL32(154, 156, 166, 255);
constexpr ImU32 kColTextMuted      = IM_COL32(112, 114, 124, 255);

constexpr ImU32 kColGreen          = IM_COL32(52, 199, 123, 255);
constexpr ImU32 kColRed            = IM_COL32(232, 76, 76, 255);
constexpr ImU32 kColYellow         = IM_COL32(238, 190, 65, 255);

constexpr int kDockMaxVisible = 12;

struct AccentDef {
    const char* name;
    ImU32 main;
    ImU32 hover;
    ImU32 fg;
    ImU32 soft;
    ImU32 line;
};

constexpr AccentDef kAccents[] = {
    { "amber",     IM_COL32(229, 154, 56, 255), IM_COL32(240, 170, 74, 255), IM_COL32(18, 16, 13, 255),  IM_COL32(229, 154, 56, 40), IM_COL32(229, 154, 56, 98) },
    { "vermilion", IM_COL32(224, 72, 50, 255),  IM_COL32(238, 88, 66, 255),  IM_COL32(255, 255, 255, 255), IM_COL32(224, 72, 50, 40), IM_COL32(224, 72, 50, 102) },
    { "platinum",  IM_COL32(228, 228, 236, 255),IM_COL32(244, 244, 250, 255),IM_COL32(12, 12, 16, 255),  IM_COL32(228, 228, 236, 34),IM_COL32(228, 228, 236, 90) },
    { "indigo",    IM_COL32(139, 124, 246, 255),IM_COL32(156, 143, 248, 255),IM_COL32(255, 255, 255, 255), IM_COL32(139, 124, 246, 40),IM_COL32(139, 124, 246, 102) },
};

struct WindowState {
    std::string id;
    std::string title;
    std::string icon;
    int order = 100;
    bool open = false;
    bool pinned = false;
    bool shaded = false;
    bool hudNoInputs = false;
    bool scrollable = false;
    bool userMoved = false;
    bool anchorRight = false;
    float defaultX = 264.0f;
    float defaultY = 36.0f;
    float posX = 264.0f;
    float posY = 36.0f;
    float width = 410.0f;
    float lastBodyH = 120.0f;
    float actualW = 410.0f;
    float actualH = 180.0f;
    int luaRenderRef = -1;
    bool registeredInPass = true;
    bool posInitialized = false;
    float openAnim = 1.0f;
    std::string pendingMoveKey;
};

struct CardFrame {
    ImDrawListSplitter splitter;
    ImVec2 startPos{};
    float width = 0.0f;
    float innerWidth = 0.0f;
};

// Shared UI state (defined in urushi_ui.cpp)
extern std::recursive_mutex g_mutex;
extern UrushiUI::Accent g_accent;
extern float g_customRgb[3];
extern AccentDef g_customAccentDef;
extern UrushiUI::Language g_language;
extern bool g_singleTabMode;

extern std::mutex g_uiElementMutex;
extern std::string g_currentRenderingWindow;
extern std::unordered_map<std::string, std::unordered_map<std::string, UrushiUI::UiElementDesc>> g_uiElements;

extern bool g_dockEdit;
extern bool g_dockOrderLoaded;
extern std::vector<std::string> g_dockOrder;
extern float g_dockScrollTarget;
extern std::string g_dragId;
extern ImVec2 g_dragPos;
extern ImVec2 g_dragGrab;
extern std::unordered_map<std::string, ImVec2> g_rowOff;

extern float g_dockAlpha;
extern float g_lastDockX, g_lastDockY, g_lastDockW, g_lastDockH;
extern float g_lastWorkW, g_lastWorkH;

extern float g_userScale;
extern float g_autoScale;
extern float g_uiScale;
extern float g_appliedThemeScale;

extern bool g_previewActive;
extern float g_previewW;
extern float g_previewH;
extern float g_previewForcedScale;

extern float g_layoutWorkW;
extern float g_layoutWorkH;
extern float g_layoutScale;

extern std::vector<WindowState> g_windows;
extern std::unordered_map<ImGuiID, float> g_animFloat;
extern std::vector<std::unique_ptr<CardFrame>> g_cardStack;

// Shared helpers (implemented in urushi_ui.cpp)
ImVec4 U32ToVec4(ImU32 col);
ImU32 ScaleAlpha(ImU32 col, float mul);
ImU32 LerpCol(ImU32 a, ImU32 b, float t);
float AnimateTowards(ImGuiID key, float target, float speed = 18.0f);
void RebuildCustomAccentDef();
const AccentDef& CurrentAccentDef();
float QueryMonitorDpiFactor();
float ComputeAutoScaleForViewport(float workW, float workH);
ImVec2 ComputeDefaultOrigin(const WindowState& win, float S, float workW, float workH, float targetW, float outerH);
WindowState* FindWindowLocked(const std::string& id);
void ApplyThemeMetrics(float S);

// Dock & floating window rendering (implemented in urushi_ui_dock.cpp)
std::wstring DockOrderPath();
void LoadDockOrder();
int DockRank(const std::string& id);
void DrawModuleIcon(ImDrawList* dl, ImVec2 center, const std::string& icon, ImU32 col, float s);
extern const unsigned char kLucideRefreshAlpha16[16 * 16];
void DrawAlphaMask(ImDrawList* dl, ImVec2 pos, float size, const unsigned char* mask, int w, int h, ImU32 baseCol);
void RenderDock(bool menuOpen);
void RenderFloatingWindows(bool menuOpen, lua_State* L);

} // namespace UrushiUiInternal
