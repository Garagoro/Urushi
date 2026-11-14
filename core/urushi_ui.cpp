#include "urushi_ui.h"
#include "urushi_ui_internal.h"
#include "d3d11_hook.h"
#include "input_manager.h"
#include "native_carx.h"
#include "car_sim_optimizer.h"
#include "unity_bridge.h"
#include "updater.h"
#include "logger.h"
#include "settings.h"
#include "lua_sandbox.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace UrushiUiInternal;

namespace UrushiUiInternal {

std::recursive_mutex g_mutex;
UrushiUI::Accent g_accent = UrushiUI::Accent::Amber;
float g_customRgb[3] = { 0.30f, 0.78f, 0.96f };
AccentDef g_customAccentDef = {
    "custom",
    IM_COL32(76, 199, 245, 255),
    IM_COL32(102, 212, 250, 255),
    IM_COL32(14, 16, 18, 255),
    IM_COL32(76, 199, 245, 40),
    IM_COL32(76, 199, 245, 102)
};
UrushiUI::Language g_language = []() {
    LANGID lang = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(lang) == LANG_RUSSIAN) ? UrushiUI::Language::Russian : UrushiUI::Language::English;
}();
bool g_singleTabMode = false;

// UI Element Introspection & Config Auto-Discovery
std::mutex g_uiElementMutex;
std::string g_currentRenderingWindow;
std::unordered_map<std::string, std::unordered_map<std::string, UrushiUI::UiElementDesc>> g_uiElements;

// ---- dock order (user drag & drop), stored in urushi_dock.ini next to urushi_settings.ini ----
bool g_dockEdit = false;
bool g_dockOrderLoaded = false;
std::vector<std::string> g_dockOrder;
// drag & drop state (rows slide while one is carried by the cursor)
float g_dockScrollTarget = -1.0f;         // smooth-scroll target of the dock list
std::string g_dragId;                       // id of the row being carried ("" = none)
ImVec2 g_dragPos(0.0f, 0.0f);               // smoothed top-left of the carried row (screen px)
ImVec2 g_dragGrab(0.0f, 0.0f);              // cursor offset inside the carried row
std::unordered_map<std::string, ImVec2> g_rowOff;   // per-row visual offset from its natural slot

float g_dockAlpha = 0.0f;
float g_lastDockX = 0.0f, g_lastDockY = 0.0f, g_lastDockW = 0.0f, g_lastDockH = 0.0f;
float g_lastWorkW = 0.0f, g_lastWorkH = 0.0f;

float g_userScale = 0.0f;       // 0.0f = Auto
float g_autoScale = 1.0f;
float g_uiScale = 1.0f;
float g_appliedThemeScale = -1.0f;

bool g_previewActive = false;
float g_previewW = 0.0f;
float g_previewH = 0.0f;
float g_previewForcedScale = -1.0f;

float g_layoutWorkW = 0.0f;
float g_layoutWorkH = 0.0f;
float g_layoutScale = 0.0f;

std::vector<WindowState> g_windows;
std::unordered_map<ImGuiID, float> g_animFloat;
std::vector<std::unique_ptr<CardFrame>> g_cardStack;

ImVec4 U32ToVec4(ImU32 col) {
    return ImGui::ColorConvertU32ToFloat4(col);
}

ImU32 ScaleAlpha(ImU32 col, float mul) {
    if (mul >= 0.999f) return col;
    if (mul <= 0.001f) return col & ~IM_COL32_A_MASK;
    int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * mul);
    a = std::clamp(a, 0, 255);
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 LerpCol(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    int ar = (a >> IM_COL32_R_SHIFT) & 0xFF, ag = (a >> IM_COL32_G_SHIFT) & 0xFF, ab = (a >> IM_COL32_B_SHIFT) & 0xFF, aa = (a >> IM_COL32_A_SHIFT) & 0xFF;
    int br = (b >> IM_COL32_R_SHIFT) & 0xFF, bg = (b >> IM_COL32_G_SHIFT) & 0xFF, bb = (b >> IM_COL32_B_SHIFT) & 0xFF, ba = (b >> IM_COL32_A_SHIFT) & 0xFF;
    return IM_COL32(
        static_cast<int>(ar + (br - ar) * t),
        static_cast<int>(ag + (bg - ag) * t),
        static_cast<int>(ab + (bb - ab) * t),
        static_cast<int>(aa + (ba - aa) * t)
    );
}

float AnimateTowards(ImGuiID key, float target, float speed) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;
    auto it = g_animFloat.find(key);
    if (it == g_animFloat.end()) {
        g_animFloat[key] = target;
        return target;
    }
    float alpha = 1.0f - std::exp(-speed * dt);
    it->second = it->second + (target - it->second) * alpha;
    if (std::fabs(target - it->second) < 0.002f) it->second = target;
    return it->second;
}

void RebuildCustomAccentDef() {
    const float r = std::clamp(g_customRgb[0], 0.0f, 1.0f);
    const float g = std::clamp(g_customRgb[1], 0.0f, 1.0f);
    const float b = std::clamp(g_customRgb[2], 0.0f, 1.0f);
    const int ri = static_cast<int>(std::round(r * 255.0f));
    const int gi = static_cast<int>(std::round(g * 255.0f));
    const int bi = static_cast<int>(std::round(b * 255.0f));
    const int rh = std::min(255, static_cast<int>(std::round((r + (1.0f - r) * 0.16f) * 255.0f)));
    const int gh = std::min(255, static_cast<int>(std::round((g + (1.0f - g) * 0.16f) * 255.0f)));
    const int bh = std::min(255, static_cast<int>(std::round((b + (1.0f - b) * 0.16f) * 255.0f)));
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;

    g_customAccentDef.name  = "custom";
    g_customAccentDef.main  = IM_COL32(ri, gi, bi, 255);
    g_customAccentDef.hover = IM_COL32(rh, gh, bh, 255);
    g_customAccentDef.fg    = (lum > 0.52f) ? IM_COL32(14, 14, 18, 255) : IM_COL32(255, 255, 255, 255);
    g_customAccentDef.soft  = IM_COL32(ri, gi, bi, 40);
    g_customAccentDef.line  = IM_COL32(ri, gi, bi, 102);
}

const AccentDef& CurrentAccentDef() {
    if (g_accent == UrushiUI::Accent::Custom) {
        return g_customAccentDef;
    }
    int idx = static_cast<int>(g_accent);
    if (idx < 0 || idx >= 4) idx = 0;
    return kAccents[idx];
}

float QueryMonitorDpiFactor() {
    using t_GetDpiForWindow = UINT(WINAPI*)(HWND);
    static t_GetDpiForWindow pfnGetDpi = []() -> t_GetDpiForWindow {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        return u32 ? reinterpret_cast<t_GetDpiForWindow>(GetProcAddress(u32, "GetDpiForWindow")) : nullptr;
    }();
    HWND hwnd = D3D11Hook::GetGameWindow();
    if (pfnGetDpi && hwnd) {
        UINT dpi = pfnGetDpi(hwnd);
        if (dpi >= 72 && dpi <= 480) {
            return static_cast<float>(dpi) / 96.0f;
        }
    }
    return 1.0f;
}

float ComputeAutoScaleForViewport(float workW, float workH) {
    if (workW <= 1.0f || workH <= 1.0f) return 1.0f;
    const float hRatio = workH / 1080.0f;
    float candidate = std::clamp(1.0f + (hRatio - 1.0f) * 0.72f, 0.80f, 1.95f);

    if (workW >= 1500.0f && workH >= 860.0f) {
        const float monitorDpi = g_previewActive ? 1.0f : QueryMonitorDpiFactor();
        candidate = std::max(candidate, std::min(monitorDpi, 1.60f));
    } else {
        const float maxSafeW = std::max(0.80f, workW / 1500.0f);
        const float maxSafeH = std::max(0.80f, workH / 880.0f);
        candidate = std::min(candidate, std::min(maxSafeW, maxSafeH));
    }
    return std::clamp(candidate, 0.80f, 2.00f);
}

ImVec2 ComputeDefaultOrigin(const WindowState& win, float S, float workW, float workH, float targetW, float outerH) {
    float x = win.anchorRight ? (workW - targetW - win.defaultX * S) : (win.defaultX * S);
    float y = win.defaultY * S;
    const float maxPosX = std::max(12.0f * S, workW - targetW - 12.0f * S);
    const float maxPosY = std::max(12.0f * S, workH - std::max(40.0f * S, outerH) - 12.0f * S);
    return ImVec2(std::clamp(x, 12.0f * S, maxPosX), std::clamp(y, 12.0f * S, maxPosY));
}

WindowState* FindWindowLocked(const std::string& id) {
    for (auto& w : g_windows) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

void ApplyThemeMetrics(float S) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(14.0f * S, 12.0f * S);
    s.FramePadding      = ImVec2(10.0f * S, 6.0f * S);
    s.ItemSpacing       = ImVec2(10.0f * S, 8.0f * S);
    s.ItemInnerSpacing  = ImVec2(8.0f * S, 6.0f * S);
    s.CellPadding       = ImVec2(8.0f * S, 6.0f * S);
    s.ScrollbarSize     = std::max(4.0f, 6.0f * S);
    s.GrabMinSize       = std::max(8.0f, 12.0f * S);
    s.WindowRounding    = 12.0f * S;
    s.ChildRounding     = 10.0f * S;
    s.FrameRounding     = 7.0f * S;
    s.PopupRounding     = 8.0f * S;
    s.ScrollbarRounding = 4.0f * S;
    s.GrabRounding      = 6.0f * S;
    s.TabRounding       = 6.0f * S;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.AntiAliasedLines  = true;
    s.AntiAliasedFill   = true;
    s.FontScaleMain     = S;
    g_appliedThemeScale = S;
}

} // namespace UrushiUiInternal

void UrushiUI::Init() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ApplyBaseTheme();
}

void UrushiUI::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_windows.clear();
    g_animFloat.clear();
    g_cardStack.clear();
}

void UrushiUI::PrepareFrame() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!ImGui::GetCurrentContext()) return;

    float workW = 0.0f, workH = 0.0f;
    if (g_previewActive && g_previewW > 1.0f && g_previewH > 1.0f) {
        workW = g_previewW;
        workH = g_previewH;
    } else {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        workW = ds.x;
        workH = ds.y;
    }

    g_autoScale = ComputeAutoScaleForViewport(workW, workH);

    float targetScale = g_autoScale;
    if (g_previewActive && g_previewForcedScale > 0.0f) {
        targetScale = std::clamp(g_previewForcedScale, 0.70f, 2.50f);
    } else if (g_userScale > 0.0f) {
        targetScale = std::clamp(g_userScale, 0.70f, 2.50f);
    }

    g_uiScale = targetScale;
    if (std::fabs(g_appliedThemeScale - g_uiScale) > 0.002f) {
        ApplyThemeMetrics(g_uiScale);
    }
}

void UrushiUI::SetUserScale(float scale) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (scale <= 0.01f) {
        g_userScale = 0.0f;
    } else {
        g_userScale = std::clamp(scale, 0.70f, 2.50f);
    }
}

float UrushiUI::GetUserScale() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_userScale;
}

float UrushiUI::GetAutoScale() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_autoScale;
}

float UrushiUI::GetUiScale() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_uiScale;
}

void UrushiUI::SetPreviewViewport(float w, float h, float forcedScale) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_previewActive = (w > 1.0f && h > 1.0f);
    g_previewW = w;
    g_previewH = h;
    g_previewForcedScale = forcedScale;
}

void UrushiUI::ClearPreviewViewport() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_previewActive = false;
    g_previewW = 0.0f;
    g_previewH = 0.0f;
    g_previewForcedScale = -1.0f;
}

void UrushiUI::ApplyBaseTheme() {
    ApplyThemeMetrics(g_uiScale);
    ImGuiStyle& s = ImGui::GetStyle();

    const AccentDef& acc = CurrentAccentDef();
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = U32ToVec4(kColTextPrimary);
    c[ImGuiCol_TextDisabled]         = U32ToVec4(kColTextMuted);
    c[ImGuiCol_WindowBg]             = U32ToVec4(kColBgWindow);
    c[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]              = U32ToVec4(kColBgDock);
    c[ImGuiCol_Border]               = U32ToVec4(kColHairlineStrong);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]              = U32ToVec4(kColBgControl);
    c[ImGuiCol_FrameBgHovered]       = U32ToVec4(kColBgRaised);
    c[ImGuiCol_FrameBgActive]        = U32ToVec4(kColBgRaisedHover);
    c[ImGuiCol_TitleBg]              = U32ToVec4(kColBgHeader);
    c[ImGuiCol_TitleBgActive]        = U32ToVec4(kColBgHeader);
    c[ImGuiCol_CheckMark]            = U32ToVec4(acc.main);
    c[ImGuiCol_SliderGrab]           = U32ToVec4(acc.main);
    c[ImGuiCol_SliderGrabActive]     = U32ToVec4(acc.hover);
    c[ImGuiCol_Button]               = U32ToVec4(kColBgRaised);
    c[ImGuiCol_ButtonHovered]        = U32ToVec4(kColBgRaisedHover);
    c[ImGuiCol_ButtonActive]         = U32ToVec4(kColBgRaisedActive);
    c[ImGuiCol_Header]               = U32ToVec4(acc.soft);
    c[ImGuiCol_HeaderHovered]        = U32ToVec4(kColBgRaisedHover);
    c[ImGuiCol_HeaderActive]         = U32ToVec4(acc.line);
    c[ImGuiCol_Separator]            = U32ToVec4(kColHairline);
    c[ImGuiCol_SeparatorHovered]     = U32ToVec4(acc.line);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(1.0f, 1.0f, 1.0f, 0.26f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.42f);
    c[ImGuiCol_ScrollbarGrabActive]  = U32ToVec4(acc.main);
    c[ImGuiCol_Tab]                  = U32ToVec4(kColBgControl);
    c[ImGuiCol_TabHovered]           = U32ToVec4(kColBgRaised);
    c[ImGuiCol_TabSelected]          = U32ToVec4(kColBgRaisedActive);
    c[ImGuiCol_TextSelectedBg]       = U32ToVec4(acc.soft);
    c[ImGuiCol_NavCursor]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_NavWindowingHighlight]= U32ToVec4(acc.line);
}

void UrushiUI::BeginLuaReload() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& w : g_windows) {
        w.registeredInPass = false;
        w.luaRenderRef = -1;
    }
    {
        std::lock_guard<std::mutex> elemLock(g_uiElementMutex);
        for (auto& wPair : g_uiElements) {
            for (auto& ePair : wPair.second) {
                ePair.second.initialized = false;
                ePair.second.changed = false;
            }
        }
    }
}

void UrushiUI::ClearLuaRefs(lua_State* L) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& w : g_windows) {
        if (w.luaRenderRef != -1 && L) {
            luaL_unref(L, LUA_REGISTRYINDEX, w.luaRenderRef);
        }
        w.luaRenderRef = -1;
        w.registeredInPass = false;
    }
}

void UrushiUI::EndLuaReload() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_windows.erase(
        std::remove_if(g_windows.begin(), g_windows.end(),
                       [](const WindowState& w) { return !w.registeredInPass; }),
        g_windows.end());
}

void UrushiUI::SetWindowOpen(const std::string& id, bool open) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        if (open && g_singleTabMode) {
            for (auto& other : g_windows) {
                if (other.id != id && !other.pinned) other.open = false;
            }
        }
        w->open = open;
        if (open) w->openAnim = 0.0f;
    }
}

bool UrushiUI::ToggleWindow(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        const bool next = !w->open;
        SetWindowOpen(id, next);
        return next;
    }
    return false;
}

bool UrushiUI::IsWindowOpen(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) return w->open;
    return false;
}

void UrushiUI::CloseAllUnpinned() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& w : g_windows) {
        if (!w.pinned) w.open = false;
    }
}

void UrushiUI::SetWindowPinned(const std::string& id, bool pinned) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) w->pinned = pinned;
}

void UrushiUI::SetWindowShaded(const std::string& id, bool shaded) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) w->shaded = shaded;
}

void UrushiUI::SetWindowPos(const std::string& id, float x, float y) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        w->posX = x;
        w->posY = y;
        w->userMoved = true;
        w->posInitialized = true;
        w->pendingMoveKey = id;
    }
}

void UrushiUI::SetWindowTitle(const std::string& id, const std::string& title) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        w->title = title;
    }
}

UrushiUI::DebugSnapshot UrushiUI::GetDebugSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    DebugSnapshot snap;
    snap.dockX = g_lastDockX;
    snap.dockY = g_lastDockY;
    snap.dockW = g_lastDockW;
    snap.dockH = g_lastDockH;
    snap.workW = g_lastWorkW;
    snap.workH = g_lastWorkH;
    snap.uiScale = g_uiScale;
    snap.autoScale = g_autoScale;
    snap.userScale = g_userScale;
    snap.windows.reserve(g_windows.size());
    for (const auto& w : g_windows) {
        WindowDebugInfo info;
        info.id = w.id;
        info.title = w.title;
        info.icon = w.icon;
        info.order = w.order;
        info.open = w.open;
        info.pinned = w.pinned;
        info.shaded = w.shaded;
        info.hudNoInputs = w.hudNoInputs;
        info.scrollable = w.scrollable;
        info.x = w.posX;
        info.y = w.posY;
        info.width = w.width;
        info.actualW = w.actualW;
        info.actualH = w.actualH;
        info.bodyH = w.lastBodyH * g_uiScale;
        snap.windows.push_back(std::move(info));
    }
    return snap;
}

void UrushiUI::SetSingleTabMode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_singleTabMode = enabled;
}

bool UrushiUI::GetSingleTabMode() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_singleTabMode;
}

void UrushiUI::SetLanguage(Language lang) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_language = lang;
}

bool UrushiUI::SetLanguageByName(const std::string& name) {
    if (_stricmp(name.c_str(), "ru") == 0 || _stricmp(name.c_str(), "russian") == 0) {
        SetLanguage(Language::Russian);
        return true;
    }
    if (_stricmp(name.c_str(), "en") == 0 || _stricmp(name.c_str(), "english") == 0) {
        SetLanguage(Language::English);
        return true;
    }
    return false;
}

UrushiUI::Language UrushiUI::GetLanguage() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_language;
}

std::string UrushiUI::GetLanguageName() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return (g_language == Language::English) ? "en" : "ru";
}

bool UrushiUI::IsEnglish() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_language == Language::English;
}

void UrushiUI::SetAccent(Accent accent) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_accent = accent;
    if (ImGui::GetCurrentContext()) {
        ApplyBaseTheme();
    }
}

bool UrushiUI::SetAccentByName(const std::string& name) {
    if (_stricmp(name.c_str(), "custom") == 0) {
        SetAccent(Accent::Custom);
        return true;
    }
    for (int i = 0; i < 4; ++i) {
        if (_stricmp(name.c_str(), kAccents[i].name) == 0) {
            SetAccent(static_cast<Accent>(i));
            return true;
        }
    }
    return false;
}

void UrushiUI::SetCustomAccent(float r, float g, float b) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_customRgb[0] = std::clamp(r, 0.0f, 1.0f);
    g_customRgb[1] = std::clamp(g, 0.0f, 1.0f);
    g_customRgb[2] = std::clamp(b, 0.0f, 1.0f);
    RebuildCustomAccentDef();
    g_accent = Accent::Custom;
    if (ImGui::GetCurrentContext()) {
        ApplyBaseTheme();
    }
}

void UrushiUI::GetCustomAccent(float& r, float& g, float& b) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    r = g_customRgb[0];
    g = g_customRgb[1];
    b = g_customRgb[2];
}

UrushiUI::Accent UrushiUI::GetAccent() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_accent;
}

std::string UrushiUI::GetAccentName() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return CurrentAccentDef().name;
}

ImU32 UrushiUI::GetAccentU32(float alphaMul)      { return ScaleAlpha(CurrentAccentDef().main, alphaMul); }
ImU32 UrushiUI::GetAccentHoverU32(float alphaMul) { return ScaleAlpha(CurrentAccentDef().hover, alphaMul); }
ImU32 UrushiUI::GetAccentFgU32(float alphaMul)    { return ScaleAlpha(CurrentAccentDef().fg, alphaMul); }
ImU32 UrushiUI::GetAccentSoftU32()                { return CurrentAccentDef().soft; }
ImU32 UrushiUI::GetAccentLineU32()                { return CurrentAccentDef().line; }

ImVec4 UrushiUI::GetAccentVec4(float alpha) {
    return U32ToVec4(GetAccentU32(alpha));
}

ImU32 UrushiUI::GetToneU32(Tone tone, float alphaMul) {
    switch (tone) {
    case Tone::Muted:  return ScaleAlpha(kColTextSecondary, alphaMul);
    case Tone::Accent: return GetAccentU32(alphaMul);
    case Tone::Green:  return ScaleAlpha(kColGreen, alphaMul);
    case Tone::Red:    return ScaleAlpha(kColRed, alphaMul);
    case Tone::Yellow: return ScaleAlpha(kColYellow, alphaMul);
    default:           return ScaleAlpha(kColTextPrimary, alphaMul);
    }
}

ImVec4 UrushiUI::GetToneVec4(Tone tone) {
    return U32ToVec4(GetToneU32(tone));
}

// UI Element Introspection & Config Auto-Discovery
void UrushiUI::SetCurrentRenderingWindow(const std::string& winId) {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    g_currentRenderingWindow = winId;
}

std::string UrushiUI::GetCurrentRenderingWindow() {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    return g_currentRenderingWindow;
}

void UrushiUI::ParseElementId(const std::string& input, std::string& outLabel, std::string& outId) {
    size_t pos = input.find("##");
    if (pos != std::string::npos) {
        outLabel = input.substr(0, pos);
        outId = input.substr(pos + 2);
        if (outId.empty()) outId = outLabel;
    } else {
        outLabel = input;
        outId = input;
    }
}

void UrushiUI::RecordUiElement(const std::string& winId, const std::string& elemId, const std::string& label,
                              const std::string& type, bool bVal, double nVal, const std::string& sVal, bool changed) {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    std::string wKey = winId.empty() ? "general" : winId;
    auto& elem = g_uiElements[wKey][elemId];
    elem.id = elemId;
    elem.label = label;
    elem.type = type;
    elem.boolValue = bVal;
    elem.numValue = nVal;
    elem.strValue = sVal;
    elem.changed = changed;
    elem.initialized = true;
}

bool UrushiUI::ConsumeStoredBool(const std::string& /*winId*/, const std::string& /*elemId*/, bool& /*val*/) {
    return false;
}

bool UrushiUI::ConsumeStoredDouble(const std::string& /*winId*/, const std::string& /*elemId*/, double& /*val*/) {
    return false;
}

bool UrushiUI::ConsumeStoredInt(const std::string& /*winId*/, const std::string& /*elemId*/, int& /*val*/) {
    return false;
}

bool UrushiUI::ConsumeStoredString(const std::string& /*winId*/, const std::string& /*elemId*/, std::string& /*val*/) {
    return false;
}

void UrushiUI::SetStoredUiElement(const std::string& winId, const std::string& elemId,
                                  const std::string& type, bool bVal, double nVal, const std::string& sVal) {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    std::string wKey = winId.empty() ? "general" : winId;
    auto& elem = g_uiElements[wKey][elemId];
    elem.id = elemId;
    elem.type = type;
    elem.boolValue = bVal;
    elem.numValue = nVal;
    elem.strValue = sVal;
    elem.hasStored = true;
    elem.initialized = false;
}

std::vector<std::pair<std::string, std::vector<UrushiUI::UiElementDesc>>> UrushiUI::GetAllUiElements() {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    std::vector<std::pair<std::string, std::vector<UiElementDesc>>> result;
    result.reserve(g_uiElements.size());
    for (const auto& wPair : g_uiElements) {
        std::vector<UiElementDesc> elems;
        elems.reserve(wPair.second.size());
        for (const auto& ePair : wPair.second) {
            elems.push_back(ePair.second);
        }
        result.emplace_back(wPair.first, std::move(elems));
    }
    return result;
}

std::vector<UrushiUI::UiElementDesc> UrushiUI::GetWindowUiElements(const std::string& winId) {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    std::vector<UiElementDesc> result;
    std::string wKey = winId.empty() ? "general" : winId;
    auto it = g_uiElements.find(wKey);
    if (it != g_uiElements.end()) {
        result.reserve(it->second.size());
        for (const auto& pair : it->second) {
            result.push_back(pair.second);
        }
    }
    return result;
}

void UrushiUI::ClearUiElements() {
    std::lock_guard<std::mutex> lock(g_uiElementMutex);
    g_uiElements.clear();
}
