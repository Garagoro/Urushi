#include "urushi_ui.h"
#include "d3d11_hook.h"
#include "native_carx.h"
#include "car_sim_optimizer.h"
#include "unity_bridge.h"
#include "updater.h"
#include "logger.h"

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

namespace {

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

struct AccentDef {
    const char* name;
    ImU32 main;
    ImU32 hover;
    ImU32 fg;
    ImU32 soft;
    ImU32 line;
};

constexpr AccentDef kAccents[] = {
    // Amber (Default)
    { "amber",     IM_COL32(229, 154, 56, 255), IM_COL32(240, 170, 74, 255), IM_COL32(18, 16, 13, 255),  IM_COL32(229, 154, 56, 40), IM_COL32(229, 154, 56, 98) },
    // Vermilion
    { "vermilion", IM_COL32(224, 72, 50, 255),  IM_COL32(238, 88, 66, 255),  IM_COL32(255, 255, 255, 255), IM_COL32(224, 72, 50, 40), IM_COL32(224, 72, 50, 102) },
    // Platinum
    { "platinum",  IM_COL32(228, 228, 236, 255),IM_COL32(244, 244, 250, 255),IM_COL32(12, 12, 16, 255),  IM_COL32(228, 228, 236, 34),IM_COL32(228, 228, 236, 90) },
    // Indigo
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

std::recursive_mutex s_mutex;
UrushiUI::Accent s_accent = UrushiUI::Accent::Amber;
bool s_singleTabMode = false;
float s_dockAlpha = 0.0f;
float s_lastDockX = 0.0f, s_lastDockY = 0.0f, s_lastDockW = 0.0f, s_lastDockH = 0.0f;
float s_lastWorkW = 0.0f, s_lastWorkH = 0.0f;

float s_userScale = 0.0f;       // 0.0f = Auto
float s_autoScale = 1.0f;
float s_uiScale = 1.0f;
float s_appliedThemeScale = -1.0f;

bool s_previewActive = false;
float s_previewW = 0.0f;
float s_previewH = 0.0f;
float s_previewForcedScale = -1.0f;

float s_layoutWorkW = 0.0f;
float s_layoutWorkH = 0.0f;
float s_layoutScale = 0.0f;

std::vector<WindowState> s_windows;
std::unordered_map<ImGuiID, float> s_animFloat;
std::vector<std::unique_ptr<CardFrame>> s_cardStack;

// ==================== Helpers ====================

static ImVec4 U32ToVec4(ImU32 col) {
    return ImGui::ColorConvertU32ToFloat4(col);
}

static ImU32 ScaleAlpha(ImU32 col, float mul) {
    if (mul >= 0.999f) return col;
    if (mul <= 0.001f) return col & ~IM_COL32_A_MASK;
    int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * mul);
    a = std::clamp(a, 0, 255);
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

static ImU32 LerpCol(ImU32 a, ImU32 b, float t) {
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

static float AnimateTowards(ImGuiID key, float target, float speed = 18.0f) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;
    auto it = s_animFloat.find(key);
    if (it == s_animFloat.end()) {
        s_animFloat[key] = target;
        return target;
    }
    float alpha = 1.0f - std::exp(-speed * dt);
    it->second = it->second + (target - it->second) * alpha;
    if (std::fabs(target - it->second) < 0.002f) it->second = target;
    return it->second;
}

static const AccentDef& CurrentAccentDef() {
    int idx = static_cast<int>(s_accent);
    if (idx < 0 || idx >= 4) idx = 0;
    return kAccents[idx];
}

static float QueryMonitorDpiFactor() {
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

static float ComputeAutoScaleForViewport(float workW, float workH) {
    if (workW <= 1.0f || workH <= 1.0f) return 1.0f;
    const float hRatio = workH / 1080.0f;
    float candidate = std::clamp(1.0f + (hRatio - 1.0f) * 0.72f, 0.80f, 1.95f);

    if (workW >= 1500.0f && workH >= 860.0f) {
        const float monitorDpi = s_previewActive ? 1.0f : QueryMonitorDpiFactor();
        candidate = std::max(candidate, std::min(monitorDpi, 1.60f));
    } else {
        const float maxSafeW = std::max(0.80f, workW / 1500.0f);
        const float maxSafeH = std::max(0.80f, workH / 880.0f);
        candidate = std::min(candidate, std::min(maxSafeW, maxSafeH));
    }
    return std::clamp(candidate, 0.80f, 2.00f);
}

static ImVec2 ComputeDefaultOrigin(const WindowState& win, float S, float workW, float workH, float targetW, float outerH) {
    float x = win.anchorRight ? (workW - targetW - win.defaultX * S) : (win.defaultX * S);
    float y = win.defaultY * S;
    const float maxPosX = std::max(12.0f * S, workW - targetW - 12.0f * S);
    const float maxPosY = std::max(12.0f * S, workH - std::max(40.0f * S, outerH) - 12.0f * S);
    return ImVec2(std::clamp(x, 12.0f * S, maxPosX), std::clamp(y, 12.0f * S, maxPosY));
}

static WindowState* FindWindowLocked(const std::string& id) {
    for (auto& w : s_windows) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

// Vector 16px icons drawn cleanly via ImDrawList (no emoji slop)
static void DrawModuleIcon(ImDrawList* dl, ImVec2 center, const std::string& icon, ImU32 col, float s) {
    const float r = 6.5f * s;
    const float th = std::max(1.2f, 1.5f * s);
    if (icon == "gauge") {
        dl->PathArcTo(center, r, 3.14159f * 0.85f, 3.14159f * 2.15f, 16);
        dl->PathStroke(col, ImDrawFlags_None, th);
        dl->AddLine(center, ImVec2(center.x + 3.8f * s, center.y - 3.2f * s), col, th);
        dl->AddCircleFilled(center, 1.5f * s, col);
    } else if (icon == "shader") {
        ImVec2 pts[6] = {
            ImVec2(center.x, center.y - r),
            ImVec2(center.x + r * 0.86f, center.y - r * 0.5f),
            ImVec2(center.x + r * 0.86f, center.y + r * 0.5f),
            ImVec2(center.x, center.y + r),
            ImVec2(center.x - r * 0.86f, center.y + r * 0.5f),
            ImVec2(center.x - r * 0.86f, center.y - r * 0.5f)
        };
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, th);
        dl->AddLine(center, pts[0], col, th * 0.85f);
        dl->AddLine(center, pts[2], col, th * 0.85f);
        dl->AddLine(center, pts[4], col, th * 0.85f);
    } else if (icon == "play") {
        ImVec2 p1(center.x - 4.0f * s, center.y - 5.5f * s);
        ImVec2 p2(center.x + 5.5f * s, center.y);
        ImVec2 p3(center.x - 4.0f * s, center.y + 5.5f * s);
        dl->AddTriangle(p1, p2, p3, col, th);
    } else if (icon == "shield") {
        ImVec2 pts[5] = {
            ImVec2(center.x, center.y - 6.5f * s),
            ImVec2(center.x + 5.5f * s, center.y - 4.2f * s),
            ImVec2(center.x + 4.8f * s, center.y + 1.8f * s),
            ImVec2(center.x, center.y + 6.5f * s),
            ImVec2(center.x - 4.8f * s, center.y + 1.8f * s)
        };
        ImVec2 pLeft(center.x - 5.5f * s, center.y - 4.2f * s);
        ImVec2 full[6] = { pts[0], pts[1], pts[2], pts[3], pts[4], pLeft };
        dl->AddPolyline(full, 6, col, ImDrawFlags_Closed, th);
    } else if (icon == "ghost") {
        dl->AddCircle(ImVec2(center.x, center.y - 1.0f * s), 5.0f * s, col, 16, th);
        dl->AddCircleFilled(ImVec2(center.x - 1.8f * s, center.y - 1.5f * s), 1.1f * s, col);
        dl->AddCircleFilled(ImVec2(center.x + 1.8f * s, center.y - 1.5f * s), 1.1f * s, col);
    } else if (icon == "hud") {
        dl->AddRect(ImVec2(center.x - 6.5f * s, center.y - 4.8f * s), ImVec2(center.x + 6.5f * s, center.y + 3.2f * s), col, 2.0f * s, 0, th);
        dl->AddLine(ImVec2(center.x - 3.5f * s, center.y + 6.0f * s), ImVec2(center.x + 3.5f * s, center.y + 6.0f * s), col, th);
        dl->AddLine(ImVec2(center.x, center.y + 3.2f * s), ImVec2(center.x, center.y + 6.0f * s), col, th);
    } else if (icon == "target") {
        dl->AddCircle(center, 5.2f * s, col, 16, th);
        dl->AddCircleFilled(center, 1.8f * s, col);
        dl->AddLine(ImVec2(center.x - 7.4f * s, center.y), ImVec2(center.x - 3.4f * s, center.y), col, th);
        dl->AddLine(ImVec2(center.x + 3.4f * s, center.y), ImVec2(center.x + 7.4f * s, center.y), col, th);
        dl->AddLine(ImVec2(center.x, center.y - 7.4f * s), ImVec2(center.x, center.y - 3.4f * s), col, th);
        dl->AddLine(ImVec2(center.x, center.y + 3.4f * s), ImVec2(center.x, center.y + 7.4f * s), col, th);
    } else if (icon == "wheel") {
        dl->AddCircle(center, r, col, 20, th);
        dl->AddCircle(center, 1.9f * s, col, 12, th * 0.9f);
        dl->AddLine(ImVec2(center.x - r, center.y), ImVec2(center.x - 1.9f * s, center.y), col, th);
        dl->AddLine(ImVec2(center.x + 1.9f * s, center.y), ImVec2(center.x + r, center.y), col, th);
        dl->AddLine(ImVec2(center.x, center.y + 1.9f * s), ImVec2(center.x, center.y + r), col, th);
    } else {
        // settings / gear
        dl->AddCircle(center, 3.0f * s, col, 14, th);
        for (int i = 0; i < 6; ++i) {
            float ang = i * (3.1415926f / 3.0f);
            float cs = std::cos(ang), sn = std::sin(ang);
            dl->AddLine(ImVec2(center.x + cs * 4.2f * s, center.y + sn * 4.2f * s),
                        ImVec2(center.x + cs * 6.5f * s, center.y + sn * 6.5f * s), col, th);
        }
    }
}

// ==================== Floating Dock Rendering ====================

static void RenderDock(bool menuOpen) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;
    const float targetAlpha = menuOpen ? 1.0f : 0.0f;
    s_dockAlpha += (targetAlpha - s_dockAlpha) * (1.0f - std::exp(-20.0f * dt));
    if (!menuOpen && s_dockAlpha < 0.02f) {
        s_dockAlpha = 0.0f;
        return;
    }

    const float S = s_uiScale;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dockW = 228.0f * S;
    const float padX = 12.0f * S;
    const float padY = 12.0f * S;
    const float innerW = dockW - padX * 2.0f;
    const size_t modCount = s_windows.size();
    const float headerTotal = 36.0f * S;
    const float availListH = std::max(80.0f, vp->WorkSize.y - 24.0f * S - padY * 2.0f - headerTotal);
    float btnH = 34.0f * S;
    float btnGap = 4.0f * S;
    float listH = (modCount > 0)
        ? (static_cast<float>(modCount) * btnH + static_cast<float>(modCount - 1) * btnGap)
        : 24.0f * S;
    if (modCount > 0 && listH > availListH) {
        const float shrink = std::max(0.55f, availListH / listH);
        btnH *= shrink;
        btnGap *= shrink;
        listH = static_cast<float>(modCount) * btnH + static_cast<float>(modCount - 1) * btnGap;
    }
    const float dockH = std::min(vp->WorkSize.y - 24.0f * S, padY * 2.0f + headerTotal + listH);
    const float dockX = vp->WorkPos.x + 18.0f * S;
    const float dockY = vp->WorkPos.y + (vp->WorkSize.y - dockH) * 0.5f;

    s_lastWorkW = vp->WorkSize.x;
    s_lastWorkH = vp->WorkSize.y;
    s_lastDockX = dockX - vp->WorkPos.x;
    s_lastDockY = dockY - vp->WorkPos.y;
    s_lastDockW = dockW;
    s_lastDockH = dockH;

    ImGui::SetNextWindowPos(ImVec2(dockX, dockY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dockW, dockH), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_dockAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, btnGap));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColBgDock));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColHairlineStrong));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!menuOpen) flags |= ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##UrushiFloatingDock", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 1. Brand Header
        const ImVec2 hPos = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(hPos, ImVec2(hPos.x + 22.0f * S, hPos.y + 22.0f * S), kColBgCard, 6.0f * S);
        dl->AddRect(hPos, ImVec2(hPos.x + 22.0f * S, hPos.y + 22.0f * S), kColHairlineStrong, 6.0f * S);
        dl->AddCircleFilled(ImVec2(hPos.x + 11.0f * S, hPos.y + 11.0f * S), 4.5f * S, UrushiUI::GetAccentU32());

        if (AppFonts::Large) ImGui::PushFont(AppFonts::Large);
        dl->AddText(ImVec2(hPos.x + 31.0f * S, hPos.y + 1.0f * S), kColTextPrimary, "URUSHI");
        if (AppFonts::Large) ImGui::PopFont();

        std::string verLabel = std::string("v") + URUSHI_VERSION;
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImVec2 verSz = ImGui::CalcTextSize(verLabel.c_str());
        dl->AddText(ImVec2(hPos.x + innerW - verSz.x, hPos.y + 4.0f * S), kColTextMuted, verLabel.c_str());
        if (AppFonts::Small) ImGui::PopFont();

        ImGui::Dummy(ImVec2(innerW, 24.0f * S));
        dl->AddLine(ImVec2(hPos.x, hPos.y + 29.0f * S), ImVec2(hPos.x + innerW, hPos.y + 29.0f * S), kColHairline);
        ImGui::Dummy(ImVec2(innerW, 4.0f * S));

        // 2. Clean panel switcher buttons (no scroll container, no extra footer clutter)
        for (size_t i = 0; i < s_windows.size(); ++i) {
            WindowState& w = s_windows[i];
            ImVec2 rPos = ImGui::GetCursorScreenPos();

            ImGui::PushID(w.id.c_str());
            bool clicked = ImGui::InvisibleButton("##dockbtn", ImVec2(innerW, btnH));
            bool hovered = ImGui::IsItemHovered();
            ImGuiID animKey = ImGui::GetItemID();
            ImGui::PopID();

            float hoverT = AnimateTowards(animKey, hovered ? 1.0f : 0.0f, 20.0f);
            ImU32 bgCol = w.open
                ? LerpCol(IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 26), hoverT)
                : LerpCol(IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 14), hoverT);
            if ((bgCol >> IM_COL32_A_SHIFT) > 0) {
                dl->AddRectFilled(rPos, ImVec2(rPos.x + innerW, rPos.y + btnH), bgCol, 8.0f * S);
            }

            if (w.open) {
                dl->AddRectFilled(
                    ImVec2(rPos.x + 2.0f * S, rPos.y + 7.0f * S),
                    ImVec2(rPos.x + 4.5f * S, rPos.y + btnH - 7.0f * S),
                    UrushiUI::GetAccentU32(), 2.0f * S);
            }

            ImU32 fgCol = w.open ? kColTextPrimary : LerpCol(kColTextSecondary, kColTextPrimary, hoverT);
            DrawModuleIcon(dl, ImVec2(rPos.x + 18.0f * S, rPos.y + btnH * 0.5f), w.icon, fgCol, S);

            float rightX = rPos.x + innerW - 10.0f * S;
            float titleClipMaxX = rightX - 10.0f * S;
            if (w.pinned) titleClipMaxX -= 28.0f * S;

            ImVec2 txtSz = ImGui::CalcTextSize(w.title.c_str());
            dl->PushClipRect(ImVec2(rPos.x + 33.0f * S, rPos.y), ImVec2(std::max(rPos.x + 40.0f * S, titleClipMaxX), rPos.y + btnH), true);
            dl->AddText(ImVec2(rPos.x + 35.0f * S, rPos.y + (btnH - txtSz.y) * 0.5f), fgCol, w.title.c_str());
            dl->PopClipRect();

            ImU32 dotCol = w.open ? UrushiUI::GetAccentU32() : IM_COL32(255, 255, 255, 32);
            dl->AddCircleFilled(ImVec2(rightX - 3.0f * S, rPos.y + btnH * 0.5f), 3.0f * S, dotCol);
            if (w.pinned) {
                if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
                ImVec2 pinSz = ImGui::CalcTextSize("PIN");
                dl->AddText(ImVec2(rightX - 14.0f * S - pinSz.x, rPos.y + (btnH - pinSz.y) * 0.5f), kColTextMuted, "PIN");
                if (AppFonts::Small) ImGui::PopFont();
            }

            if (clicked) {
                w.open = !w.open;
                if (w.open) w.openAnim = 0.0f;
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);
}

// ==================== Floating Windows Rendering ====================

static void RenderFloatingWindows(bool menuOpen, lua_State* L) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float S = s_uiScale;

    const bool firstLayout = (s_layoutWorkW <= 0.0f || s_layoutWorkH <= 0.0f || s_layoutScale <= 0.0f);
    const bool viewportChanged = !firstLayout &&
        (std::fabs(vp->WorkSize.x - s_layoutWorkW) > 1.0f ||
         std::fabs(vp->WorkSize.y - s_layoutWorkH) > 1.0f ||
         std::fabs(S - s_layoutScale) > 0.002f);

    if (!s_previewActive && (firstLayout || viewportChanged)) {
        if (viewportChanged) {
            const float prevMaxWinH = std::min(960.0f * S, std::max(240.0f * S, vp->WorkSize.y * 0.90f));
            for (auto& win : s_windows) {
                const float targetW = std::min(win.width * S, std::max(220.0f * S, vp->WorkSize.x - 24.0f * S));
                if (!win.userMoved) {
                    const float estOuterH = std::min(prevMaxWinH, (40.0f + 22.0f + win.lastBodyH) * S);
                    ImVec2 defPos = ComputeDefaultOrigin(win, S, vp->WorkSize.x, vp->WorkSize.y, targetW, estOuterH);
                    win.posX = defPos.x;
                    win.posY = defPos.y;
                } else {
                    const float oldSpanX = std::max(1.0f, s_layoutWorkW - win.actualW - 24.0f * s_layoutScale);
                    const float oldSpanY = std::max(1.0f, s_layoutWorkH - win.actualH - 24.0f * s_layoutScale);
                    const float fracX = std::clamp((win.posX - 12.0f * s_layoutScale) / oldSpanX, 0.0f, 1.0f);
                    const float fracY = std::clamp((win.posY - 12.0f * s_layoutScale) / oldSpanY, 0.0f, 1.0f);
                    const float newSpanX = std::max(0.0f, vp->WorkSize.x - targetW - 24.0f * S);
                    const float newSpanY = std::max(0.0f, vp->WorkSize.y - std::min(vp->WorkSize.y * 0.85f, win.actualH * (S / std::max(0.1f, s_layoutScale))) - 24.0f * S);
                    win.posX = std::clamp(12.0f * S + fracX * newSpanX, 12.0f * S, std::max(12.0f * S, vp->WorkSize.x - targetW - 12.0f * S));
                    win.posY = std::clamp(12.0f * S + fracY * newSpanY, 12.0f * S, std::max(12.0f * S, vp->WorkSize.y - 120.0f * S));
                }
                win.pendingMoveKey = "relayout";
            }
        }
        s_layoutWorkW = vp->WorkSize.x;
        s_layoutWorkH = vp->WorkSize.y;
        s_layoutScale = S;
    }

    s_lastWorkW = vp->WorkSize.x;
    s_lastWorkH = vp->WorkSize.y;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;

    const float headerH = 40.0f * S;
    const float bodyPadX = 14.0f * S;
    const float bodyPadTop = 10.0f * S;
    const float bodyPadBot = 12.0f * S;
    const float maxWinH = std::min(960.0f * S, std::max(240.0f * S, vp->WorkSize.y * 0.90f));
    const float maxBodyH = std::max(110.0f * S, maxWinH - headerH - bodyPadTop - bodyPadBot);
    const float winRounding = 12.0f * S;

    for (size_t i = 0; i < s_windows.size(); ++i) {
        WindowState& w = s_windows[i];
        const bool shouldShow = w.open && (menuOpen || w.pinned);
        if (!shouldShow || w.luaRenderRef == -1 || !L) continue;

        w.openAnim += (1.0f - w.openAnim) * (1.0f - std::exp(-22.0f * dt));
        if (w.openAnim > 0.99f) w.openAnim = 1.0f;

        const float targetW = std::min(w.width * S, std::max(220.0f * S, vp->WorkSize.x - 24.0f * S));
        const float targetBodyH = std::clamp(w.lastBodyH * S, 28.0f * S, maxBodyH);
        const float outerH = w.shaded ? headerH : (headerH + bodyPadTop + targetBodyH + bodyPadBot);
        w.scrollable = (!w.shaded && (w.lastBodyH * S) > maxBodyH + 1.0f);

        const float maxPosX = std::max(12.0f * S, vp->WorkSize.x - targetW - 12.0f * S);
        const float maxPosY = std::max(12.0f * S, vp->WorkSize.y - std::max(headerH + 40.0f * S, outerH) - 12.0f * S);

        if (s_previewActive) {
            ImVec2 prevPos = ComputeDefaultOrigin(w, S, vp->WorkSize.x, vp->WorkSize.y, targetW, outerH);
            if (w.userMoved) {
                const float refW = (s_layoutWorkW > 1.0f) ? s_layoutWorkW : vp->WorkSize.x;
                const float refH = (s_layoutWorkH > 1.0f) ? s_layoutWorkH : vp->WorkSize.y;
                const float refS = (s_layoutScale > 0.1f) ? s_layoutScale : S;
                const float oldSpanX = std::max(1.0f, refW - w.actualW - 24.0f * refS);
                const float oldSpanY = std::max(1.0f, refH - w.actualH - 24.0f * refS);
                const float fracX = std::clamp((w.posX - 12.0f * refS) / oldSpanX, 0.0f, 1.0f);
                const float fracY = std::clamp((w.posY - 12.0f * refS) / oldSpanY, 0.0f, 1.0f);
                prevPos.x = std::clamp(12.0f * S + fracX * std::max(0.0f, vp->WorkSize.x - targetW - 24.0f * S), 12.0f * S, maxPosX);
                prevPos.y = std::clamp(12.0f * S + fracY * std::max(0.0f, vp->WorkSize.y - outerH - 24.0f * S), 12.0f * S, maxPosY);
            }
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + prevPos.x, vp->WorkPos.y + prevPos.y), ImGuiCond_Always);
        } else if (!w.posInitialized) {
            ImVec2 initPos = ComputeDefaultOrigin(w, S, vp->WorkSize.x, vp->WorkSize.y, targetW, outerH);
            w.posX = initPos.x;
            w.posY = initPos.y;
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + initPos.x, vp->WorkPos.y + initPos.y), ImGuiCond_Always);
            w.posInitialized = true;
        } else if (!w.pendingMoveKey.empty()) {
            w.posX = std::clamp(w.posX, 12.0f * S, maxPosX);
            w.posY = std::clamp(w.posY, 12.0f * S, maxPosY);
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + w.posX, vp->WorkPos.y + w.posY), ImGuiCond_Always);
            w.pendingMoveKey.clear();
        } else {
            // Keep existing window clamped if viewport shrunk or bottom content expanded
            const float cX = std::clamp(w.posX, 6.0f * S, std::max(6.0f * S, vp->WorkSize.x - targetW - 6.0f * S));
            const float cY = std::clamp(w.posY, 6.0f * S, std::max(6.0f * S, vp->WorkSize.y - outerH - 6.0f * S));
            if (std::fabs(cX - w.posX) > 1.0f || std::fabs(cY - w.posY) > 1.0f) {
                w.posX = cX;
                w.posY = cY;
                ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + cX, vp->WorkPos.y + cY), ImGuiCond_Always);
            }
        }

        ImGui::SetNextWindowSize(ImVec2(targetW, outerH), ImGuiCond_Always);

        ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (w.hudNoInputs && !menuOpen) {
            wflags |= ImGuiWindowFlags_NoInputs;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.25f, w.openAnim));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(bodyPadX, bodyPadBot));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColBgWindow));
        ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(w.pinned ? UrushiUI::GetAccentLineU32() : kColHairlineStrong));

        std::string winLabel = "##UrushiWin_" + w.id;
        if (ImGui::Begin(winLabel.c_str(), nullptr, wflags)) {
            const ImVec2 winPos = ImGui::GetWindowPos();
            const ImVec2 winSize = ImGui::GetWindowSize();
            if (!s_previewActive) {
                w.posX = winPos.x - vp->WorkPos.x;
                w.posY = winPos.y - vp->WorkPos.y;
                w.actualW = winSize.x;
                w.actualH = winSize.y;
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImDrawFlags hdrRound = w.shaded ? ImDrawFlags_RoundNone : (ImDrawFlags_RoundTopLeft | ImDrawFlags_RoundTopRight);
            if (w.shaded) {
                dl->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + headerH), kColBgHeader, winRounding);
            } else {
                dl->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + headerH), kColBgHeader, winRounding, hdrRound);
                dl->AddLine(ImVec2(winPos.x, winPos.y + headerH), ImVec2(winPos.x + winSize.x, winPos.y + headerH), kColHairline);
            }

            const float btnSize = 22.0f * S;
            const float btnGap = 4.0f * S;
            const float pinW = 36.0f * S;
            const float ctrlRight = winPos.x + winSize.x - 10.0f * S;
            const float btnTop = winPos.y + (headerH - btnSize) * 0.5f;
            const float shRight = ctrlRight - btnSize - btnGap;
            const float pinRight = shRight - btnSize - btnGap;
            const float titleMaxX = (pinRight - pinW) - 8.0f * S;

            // Title + accent dot inside header (clipped before buttons)
            dl->AddCircleFilled(ImVec2(winPos.x + 17.0f * S, winPos.y + headerH * 0.5f), 3.2f * S, UrushiUI::GetAccentU32());
            ImVec2 tSz = ImGui::CalcTextSize(w.title.c_str());
            dl->PushClipRect(ImVec2(winPos.x + 26.0f * S, winPos.y), ImVec2(std::max(winPos.x + 48.0f * S, titleMaxX), winPos.y + headerH), true);
            dl->AddText(ImVec2(winPos.x + 28.0f * S, winPos.y + (headerH - tSz.y) * 0.5f), kColTextPrimary, w.title.c_str());
            dl->PopClipRect();

            // Header action buttons (Pin, Shade, Close) — only interactive if inputs allowed
            const bool allowClicks = !(w.hudNoInputs && !menuOpen);
            if (allowClicks) {
                ImGui::PushID(w.id.c_str());
                const float btnR = 5.0f * S;
                const float stroke = std::max(1.2f, 1.45f * S);

                // Close button
                ImGui::SetCursorScreenPos(ImVec2(ctrlRight - btnSize, btnTop));
                bool clickClose = ImGui::InvisibleButton("##close", ImVec2(btnSize, btnSize));
                if (ImGui::IsItemHovered()) {
                    dl->AddRectFilled(ImVec2(ctrlRight - btnSize, btnTop), ImVec2(ctrlRight, btnTop + btnSize), IM_COL32(255, 255, 255, 18), btnR);
                }
                ImVec2 cx(ctrlRight - btnSize * 0.5f, btnTop + btnSize * 0.5f);
                ImU32 xCol = ImGui::IsItemHovered() ? kColTextPrimary : kColTextSecondary;
                dl->AddLine(ImVec2(cx.x - 3.5f * S, cx.y - 3.5f * S), ImVec2(cx.x + 3.5f * S, cx.y + 3.5f * S), xCol, stroke);
                dl->AddLine(ImVec2(cx.x - 3.5f * S, cx.y + 3.5f * S), ImVec2(cx.x + 3.5f * S, cx.y - 3.5f * S), xCol, stroke);

                // Shade button
                ImGui::SetCursorScreenPos(ImVec2(shRight - btnSize, btnTop));
                bool clickShade = ImGui::InvisibleButton("##shade", ImVec2(btnSize, btnSize));
                if (ImGui::IsItemHovered()) {
                    dl->AddRectFilled(ImVec2(shRight - btnSize, btnTop), ImVec2(shRight, btnTop + btnSize), IM_COL32(255, 255, 255, 18), btnR);
                }
                ImVec2 sx(shRight - btnSize * 0.5f, btnTop + btnSize * 0.5f);
                ImU32 sCol = ImGui::IsItemHovered() ? kColTextPrimary : kColTextSecondary;
                dl->AddLine(ImVec2(sx.x - 4.0f * S, sx.y), ImVec2(sx.x + 4.0f * S, sx.y), sCol, stroke);
                if (w.shaded) {
                    dl->AddLine(ImVec2(sx.x, sx.y - 4.0f * S), ImVec2(sx.x, sx.y + 4.0f * S), sCol, stroke);
                }

                // Pin pill button
                ImGui::SetCursorScreenPos(ImVec2(pinRight - pinW, btnTop));
                bool clickPin = ImGui::InvisibleButton("##pin", ImVec2(pinW, btnSize));
                bool pinHov = ImGui::IsItemHovered();
                ImU32 pinBg = w.pinned ? UrushiUI::GetAccentSoftU32() : (pinHov ? IM_COL32(255, 255, 255, 14) : 0);
                if ((pinBg >> IM_COL32_A_SHIFT) > 0) {
                    dl->AddRectFilled(ImVec2(pinRight - pinW, btnTop), ImVec2(pinRight, btnTop + btnSize), pinBg, btnR);
                }
                if (w.pinned) {
                    dl->AddRect(ImVec2(pinRight - pinW, btnTop), ImVec2(pinRight, btnTop + btnSize), UrushiUI::GetAccentLineU32(), btnR);
                }
                if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
                ImVec2 pSz = ImGui::CalcTextSize("Pin");
                dl->AddText(ImVec2(pinRight - pinW * 0.5f - pSz.x * 0.5f, btnTop + (btnSize - pSz.y) * 0.5f),
                            w.pinned ? UrushiUI::GetAccentU32() : (pinHov ? kColTextPrimary : kColTextSecondary), "Pin");
                if (AppFonts::Small) ImGui::PopFont();

                // Header drag & double-click zone
                float dragW = std::max(40.0f * S, (pinRight - pinW - 4.0f * S) - winPos.x);
                ImGui::SetCursorScreenPos(winPos);
                ImGui::InvisibleButton("##drag_hdr", ImVec2(dragW, headerH));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    w.shaded = !w.shaded;
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                    w.userMoved = true;
                    ImVec2 delta = ImGui::GetIO().MouseDelta;
                    ImVec2 np(winPos.x + delta.x, winPos.y + delta.y);
                    np.x = std::clamp(np.x, vp->WorkPos.x + 6.0f * S, vp->WorkPos.x + vp->WorkSize.x - winSize.x - 6.0f * S);
                    np.y = std::clamp(np.y, vp->WorkPos.y + 6.0f * S, vp->WorkPos.y + vp->WorkSize.y - 44.0f * S);
                    ImGui::SetWindowPos(np);
                }

                if (clickClose) w.open = false;
                if (clickShade) w.shaded = !w.shaded;
                if (clickPin)   w.pinned = !w.pinned;

                ImGui::PopID();
            }

            if (!w.shaded) {
                const float childW = std::max(80.0f * S, winSize.x - bodyPadX * 2.0f);
                const float childH = std::max(28.0f * S, winSize.y - headerH - bodyPadTop - bodyPadBot);
                ImGui::SetCursorScreenPos(ImVec2(winPos.x + bodyPadX, winPos.y + headerH + bodyPadTop));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * S, 8.0f * S));
                ImGuiWindowFlags bodyFlags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoNavInputs;
                if (!w.scrollable) {
                    bodyFlags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
                }
                if (ImGui::BeginChild("##body", ImVec2(childW, childH), ImGuiChildFlags_None, bodyFlags)) {
                    ImGui::BeginGroup();
                    {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, w.luaRenderRef);
                        if (lua_isfunction(L, -1)) {
                            lua_getglobal(L, "ui");
                            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                                const char* err = lua_tostring(L, -1);
                                Logger::Error("URUSHI UI render error (" + w.id + "): " + std::string(err ? err : "unknown"));
                                lua_pop(L, 1);
                                while (!s_cardStack.empty()) {
                                    UrushiUI::EndCard();
                                }
                            }
                        } else {
                            lua_pop(L, 1);
                        }
                    }
                    ImGui::EndGroup();
                    float measuredH = ImGui::GetItemRectSize().y;
                    if (measuredH >= 6.0f * S) {
                        w.lastBodyH = measuredH / S;
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
            }
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
    }
}

static void ApplyThemeMetrics(float S) {
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
    s_appliedThemeScale = S;
}

} // namespace

// ==================== Lifecycle & Configuration ====================

void UrushiUI::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    ApplyBaseTheme();
}

void UrushiUI::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_windows.clear();
    s_animFloat.clear();
    s_cardStack.clear();
}

void UrushiUI::PrepareFrame() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!ImGui::GetCurrentContext()) return;

    float workW = 0.0f, workH = 0.0f;
    if (s_previewActive && s_previewW > 1.0f && s_previewH > 1.0f) {
        workW = s_previewW;
        workH = s_previewH;
    } else {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        workW = ds.x;
        workH = ds.y;
    }

    s_autoScale = ComputeAutoScaleForViewport(workW, workH);

    float targetScale = s_autoScale;
    if (s_previewActive && s_previewForcedScale > 0.0f) {
        targetScale = std::clamp(s_previewForcedScale, 0.70f, 2.50f);
    } else if (s_userScale > 0.0f) {
        targetScale = std::clamp(s_userScale, 0.70f, 2.50f);
    }

    s_uiScale = targetScale;
    if (std::fabs(s_appliedThemeScale - s_uiScale) > 0.002f) {
        ApplyThemeMetrics(s_uiScale);
    }
}

void UrushiUI::SetUserScale(float scale) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (scale <= 0.01f) {
        s_userScale = 0.0f;
    } else {
        s_userScale = std::clamp(scale, 0.70f, 2.50f);
    }
}

float UrushiUI::GetUserScale() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_userScale;
}

float UrushiUI::GetAutoScale() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_autoScale;
}

float UrushiUI::GetUiScale() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_uiScale;
}

void UrushiUI::SetPreviewViewport(float w, float h, float forcedScale) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_previewActive = (w > 1.0f && h > 1.0f);
    s_previewW = w;
    s_previewH = h;
    s_previewForcedScale = forcedScale;
}

void UrushiUI::ClearPreviewViewport() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_previewActive = false;
    s_previewW = 0.0f;
    s_previewH = 0.0f;
    s_previewForcedScale = -1.0f;
}

void UrushiUI::ApplyBaseTheme() {
    ApplyThemeMetrics(s_uiScale);
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
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (auto& w : s_windows) {
        w.registeredInPass = false;
        w.luaRenderRef = -1;
    }
}

void UrushiUI::ClearLuaRefs(lua_State* L) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (auto& w : s_windows) {
        if (w.luaRenderRef != -1 && L) {
            luaL_unref(L, LUA_REGISTRYINDEX, w.luaRenderRef);
        }
        w.luaRenderRef = -1;
        w.registeredInPass = false;
    }
}

void UrushiUI::EndLuaReload() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_windows.erase(
        std::remove_if(s_windows.begin(), s_windows.end(),
                       [](const WindowState& w) { return !w.registeredInPass; }),
        s_windows.end());
}

void UrushiUI::RegisterWindow(const WindowRegistration& reg) {
    if (reg.id.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    if (WindowState* existing = FindWindowLocked(reg.id)) {
        existing->title = reg.title;
        existing->icon = reg.icon;
        existing->order = reg.order;
        existing->hudNoInputs = reg.hudNoInputs;
        existing->anchorRight = reg.anchorRight;
        existing->defaultX = reg.defaultX;
        existing->defaultY = reg.defaultY;
        existing->width = reg.width;
        if (!existing->userMoved) {
            if (s_layoutWorkW > 1.0f && s_layoutWorkH > 1.0f) {
                const float targetW = std::min(existing->width * s_uiScale, std::max(220.0f * s_uiScale, s_layoutWorkW - 24.0f * s_uiScale));
                ImVec2 defPos = ComputeDefaultOrigin(*existing, s_uiScale, s_layoutWorkW, s_layoutWorkH, targetW, existing->actualH);
                existing->posX = defPos.x;
                existing->posY = defPos.y;
            } else {
                existing->posX = reg.defaultX * s_uiScale;
                existing->posY = reg.defaultY * s_uiScale;
            }
            existing->pendingMoveKey = "reload";
        }
        existing->luaRenderRef = reg.luaRenderRef;
        existing->registeredInPass = true;
    } else {
        WindowState st;
        st.id = reg.id;
        st.title = reg.title;
        st.icon = reg.icon;
        st.order = reg.order;
        st.open = reg.defaultOpen;
        st.pinned = reg.defaultPinned;
        st.hudNoInputs = reg.hudNoInputs;
        st.anchorRight = reg.anchorRight;
        st.defaultX = reg.defaultX;
        st.defaultY = reg.defaultY;
        st.posX = reg.defaultX * s_uiScale;
        st.posY = reg.defaultY * s_uiScale;
        st.width = reg.width;
        st.luaRenderRef = reg.luaRenderRef;
        st.registeredInPass = true;
        s_windows.push_back(std::move(st));
    }

    std::sort(s_windows.begin(), s_windows.end(), [](const WindowState& a, const WindowState& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.title < b.title;
    });
}

void UrushiUI::Render(bool menuOpen, lua_State* L) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    RenderDock(menuOpen);
    RenderFloatingWindows(menuOpen, L);
}

void UrushiUI::SetWindowOpen(const std::string& id, bool open) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        if (open && s_singleTabMode) {
            for (auto& other : s_windows) {
                if (other.id != id && !other.pinned) other.open = false;
            }
        }
        w->open = open;
        if (open) w->openAnim = 0.0f;
    }
}

bool UrushiUI::ToggleWindow(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        const bool next = !w->open;
        SetWindowOpen(id, next);
        return next;
    }
    return false;
}

bool UrushiUI::IsWindowOpen(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (WindowState* w = FindWindowLocked(id)) return w->open;
    return false;
}

void UrushiUI::CloseAllUnpinned() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (auto& w : s_windows) {
        if (!w.pinned) w.open = false;
    }
}

void UrushiUI::SetWindowPinned(const std::string& id, bool pinned) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (WindowState* w = FindWindowLocked(id)) w->pinned = pinned;
}

void UrushiUI::SetWindowShaded(const std::string& id, bool shaded) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (WindowState* w = FindWindowLocked(id)) w->shaded = shaded;
}

void UrushiUI::SetWindowPos(const std::string& id, float x, float y) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (WindowState* w = FindWindowLocked(id)) {
        w->posX = x;
        w->posY = y;
        w->userMoved = true;
        w->posInitialized = true;
        w->pendingMoveKey = id;
    }
}

UrushiUI::DebugSnapshot UrushiUI::GetDebugSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    DebugSnapshot snap;
    snap.dockX = s_lastDockX;
    snap.dockY = s_lastDockY;
    snap.dockW = s_lastDockW;
    snap.dockH = s_lastDockH;
    snap.workW = s_lastWorkW;
    snap.workH = s_lastWorkH;
    snap.uiScale = s_uiScale;
    snap.autoScale = s_autoScale;
    snap.userScale = s_userScale;
    snap.windows.reserve(s_windows.size());
    for (const auto& w : s_windows) {
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
        info.bodyH = w.lastBodyH * s_uiScale;
        snap.windows.push_back(std::move(info));
    }
    return snap;
}

void UrushiUI::SetSingleTabMode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_singleTabMode = enabled;
}

bool UrushiUI::GetSingleTabMode() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_singleTabMode;
}

void UrushiUI::SetAccent(Accent accent) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_accent = accent;
    if (ImGui::GetCurrentContext()) {
        ApplyBaseTheme();
    }
}

bool UrushiUI::SetAccentByName(const std::string& name) {
    for (int i = 0; i < 4; ++i) {
        if (_stricmp(name.c_str(), kAccents[i].name) == 0) {
            SetAccent(static_cast<Accent>(i));
            return true;
        }
    }
    return false;
}

UrushiUI::Accent UrushiUI::GetAccent() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_accent;
}

std::string UrushiUI::GetAccentName() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
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

// ==================== Lacquered ImDrawList Widgets ====================

float UrushiUI::CurrentContentWidth() {
    if (!s_cardStack.empty()) {
        return s_cardStack.back()->innerWidth;
    }
    return std::max(60.0f * s_uiScale, ImGui::GetContentRegionAvail().x);
}

void UrushiUI::BeginCard(const std::string& title, const std::string& subtitle) {
    const float S = s_uiScale;
    const float padX = 14.0f * S;
    const float padY = 12.0f * S;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto frame = std::make_unique<CardFrame>();
    frame->startPos = ImGui::GetCursorScreenPos();
    frame->width = CurrentContentWidth();
    frame->innerWidth = std::max(40.0f * S, frame->width - padX * 2.0f);

    frame->splitter.Split(dl, 2);
    frame->splitter.SetCurrentChannel(dl, 1);

    ImGui::SetCursorScreenPos(ImVec2(frame->startPos.x + padX, frame->startPos.y + padY));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(frame->innerWidth);

    if (!title.empty()) {
        dl->AddText(ImGui::GetCursorScreenPos(), kColTextPrimary, title.c_str());
        ImGui::Dummy(ImVec2(frame->innerWidth, ImGui::GetTextLineHeight()));
    }
    if (!subtitle.empty()) {
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImVec2 sPos = ImGui::GetCursorScreenPos();
        dl->AddText(sPos, kColTextSecondary, subtitle.c_str());
        ImGui::Dummy(ImVec2(frame->innerWidth, ImGui::GetTextLineHeight()));
        if (AppFonts::Small) ImGui::PopFont();
    }
    if (!title.empty() || !subtitle.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f * S));
    }

    s_cardStack.push_back(std::move(frame));
}

void UrushiUI::EndCard() {
    if (s_cardStack.empty()) return;
    const float S = s_uiScale;
    const float padY = 12.0f * S;
    const float cardR = 10.0f * S;

    std::unique_ptr<CardFrame> frame = std::move(s_cardStack.back());
    s_cardStack.pop_back();

    ImGui::PopItemWidth();
    ImGui::EndGroup();

    ImVec2 itemMax = ImGui::GetItemRectMax();
    float bodyBottom = (std::max)(frame->startPos.y + padY * 2.0f, itemMax.y);
    ImVec2 cardEnd(frame->startPos.x + frame->width, bodyBottom + padY);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    frame->splitter.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(frame->startPos, cardEnd, kColBgCard, cardR);
    dl->AddRect(frame->startPos, cardEnd, kColHairline, cardR);
    frame->splitter.Merge(dl);

    ImGui::SetCursorScreenPos(ImVec2(frame->startPos.x, cardEnd.y));
    ImGui::Dummy(ImVec2(frame->width, 0.0f));
}

bool UrushiUI::Toggle(const std::string& label, bool* value) {
    if (!value) return false;
    const float S = s_uiScale;
    const float w = CurrentContentWidth();
    const float rowH = 26.0f * S;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool clicked = ImGui::InvisibleButton(label.c_str(), ImVec2(w, rowH));
    bool hovered = ImGui::IsItemHovered();
    ImGuiID key = ImGui::GetItemID();

    if (clicked) {
        *value = !(*value);
    }

    float t = AnimateTowards(key, *value ? 1.0f : 0.0f, 18.0f);
    float hov = AnimateTowards(key ^ 0x9E3779B9u, hovered ? 1.0f : 0.0f, 20.0f);

    // Strip '##' ImGui ID suffix for visible text
    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;

    // Pill switch on the right
    const float swW = 38.0f * S, swH = 20.0f * S;
    ImVec2 swMin(pos.x + w - swW, pos.y + (rowH - swH) * 0.5f);
    ImVec2 swMax(swMin.x + swW, swMin.y + swH);

    ImVec2 txtSz = ImGui::CalcTextSize(vis.c_str());
    dl->PushClipRect(pos, ImVec2((std::max)(pos.x + 8.0f * S, swMin.x - 8.0f * S), pos.y + rowH), true);
    dl->AddText(ImVec2(pos.x, pos.y + (rowH - txtSz.y) * 0.5f),
                LerpCol(kColTextPrimary, IM_COL32(255, 255, 255, 255), hov * 0.5f), vis.c_str());
    dl->PopClipRect();

    ImU32 trackOff = LerpCol(IM_COL32(255, 255, 255, 34), IM_COL32(255, 255, 255, 48), hov);
    ImU32 trackCol = LerpCol(trackOff, GetAccentU32(), t);
    dl->AddRectFilled(swMin, swMax, trackCol, swH * 0.5f);

    const float knobR = (7.0f + hov * 0.5f) * S;
    float knobX = (swMin.x + 10.0f * S) + (swW - 20.0f * S) * t;
    float knobY = swMin.y + swH * 0.5f;
    ImU32 knobCol = LerpCol(IM_COL32(244, 244, 248, 255), GetAccentFgU32(), t);
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, knobCol, 16);

    return clicked;
}

static float DrawSliderHeader(ImDrawList* dl, ImVec2 pos, float w, float S, const char* vis, const char* valBuf) {
    ImVec2 lblSz = ImGui::CalcTextSize(vis);
    if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
    ImVec2 valSz = ImGui::CalcTextSize(valBuf);
    const float chipW = valSz.x + 14.0f * S;
    const float chipH = valSz.y + 4.0f * S;
    const float chipR = 5.0f * S;
    ImVec2 chipMin(pos.x + w - chipW, pos.y - 1.0f * S);
    ImVec2 chipMax(pos.x + w, pos.y - 1.0f * S + chipH);
    dl->AddRectFilled(chipMin, chipMax, kColBgControl, chipR);
    dl->AddRect(chipMin, chipMax, kColHairline, chipR);
    dl->AddText(ImVec2(chipMin.x + 7.0f * S, chipMin.y + 2.0f * S), kColTextPrimary, valBuf);
    if (AppFonts::Mono) ImGui::PopFont();

    dl->PushClipRect(pos, ImVec2((std::max)(pos.x + 8.0f * S, chipMin.x - 6.0f * S), pos.y + (std::max)(lblSz.y, chipH) + 2.0f * S), true);
    dl->AddText(pos, kColTextPrimary, vis);
    dl->PopClipRect();

    return pos.y + std::max(lblSz.y, chipH) + 3.0f * S;
}

static void DrawSliderTrackVisual(ImDrawList* dl, ImVec2 pos, float trackTop, float w, float hitH, float S, float pct, ImGuiID key, bool hovered, bool active) {
    float knobScale = AnimateTowards(key, (hovered || active) ? 1.0f : 0.0f, 22.0f);
    const float pad = 6.0f * S;
    const float trkY = trackTop + hitH * 0.5f;
    const float trkH = std::max(2.0f, 4.0f * S);
    const float trkR = trkH * 0.5f;
    dl->AddRectFilled(ImVec2(pos.x, trkY - trkR), ImVec2(pos.x + w, trkY + trkR), IM_COL32(255, 255, 255, 24), trkR);

    float fillX = pos.x + pad + std::max(0.0f, (w - pad * 2.0f) * pct);
    dl->AddRectFilled(ImVec2(pos.x, trkY - trkR), ImVec2(fillX, trkY + trkR), UrushiUI::GetAccentU32(), trkR);

    float thumbR = (6.0f + knobScale * 1.6f) * S;
    dl->AddCircleFilled(ImVec2(fillX, trkY), thumbR + 1.5f * S, kColBgDock, 16);
    dl->AddCircleFilled(ImVec2(fillX, trkY), thumbR, IM_COL32(246, 246, 250, 255), 16);
}

bool UrushiUI::SliderFloat(const std::string& label, float* value, float minVal, float maxVal, const char* fmt) {
    if (!value || maxVal <= minVal) return false;
    const float S = s_uiScale;
    const float w = CurrentContentWidth();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;

    char valBuf[64];
    snprintf(valBuf, sizeof(valBuf), fmt ? fmt : "%.2f", *value);

    const float trackTop = DrawSliderHeader(dl, pos, w, S, vis.c_str(), valBuf);
    const float hitH = 18.0f * S;
    ImGui::SetCursorScreenPos(ImVec2(pos.x, trackTop));
    ImGui::InvisibleButton(label.c_str(), ImVec2(w, hitH));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiID key = ImGui::GetItemID();

    bool changed = false;
    if (active && ImGui::IsMouseDown(0)) {
        float mouseX = ImGui::GetIO().MousePos.x;
        float pad = 6.0f * S;
        float t = std::clamp((mouseX - (pos.x + pad)) / std::max(1.0f, w - pad * 2.0f), 0.0f, 1.0f);
        float newVal = minVal + (maxVal - minVal) * t;
        if (std::fabs(newVal - *value) > 1e-5f) {
            *value = newVal;
            changed = true;
        }
    }

    float pct = std::clamp((*value - minVal) / (maxVal - minVal), 0.0f, 1.0f);
    DrawSliderTrackVisual(dl, pos, trackTop, w, hitH, S, pct, key, hovered, active);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, trackTop + hitH));
    ImGui::Dummy(ImVec2(w, 2.0f * S));
    return changed;
}

bool UrushiUI::SliderInt(const std::string& label, int* value, int minVal, int maxVal, const char* fmt) {
    if (!value || maxVal <= minVal) return false;
    const float S = s_uiScale;
    const float w = CurrentContentWidth();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;

    char valBuf[64];
    snprintf(valBuf, sizeof(valBuf), fmt ? fmt : "%d", *value);

    const float trackTop = DrawSliderHeader(dl, pos, w, S, vis.c_str(), valBuf);
    const float hitH = 18.0f * S;
    ImGui::SetCursorScreenPos(ImVec2(pos.x, trackTop));
    ImGui::InvisibleButton(label.c_str(), ImVec2(w, hitH));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiID key = ImGui::GetItemID();

    bool changed = false;
    if (active && ImGui::IsMouseDown(0)) {
        float mouseX = ImGui::GetIO().MousePos.x;
        float pad = 6.0f * S;
        float t = std::clamp((mouseX - (pos.x + pad)) / std::max(1.0f, w - pad * 2.0f), 0.0f, 1.0f);
        int newVal = minVal + static_cast<int>(std::round(static_cast<float>(maxVal - minVal) * t));
        newVal = std::clamp(newVal, minVal, maxVal);
        if (newVal != *value) {
            *value = newVal;
            changed = true;
        }
    }

    float pct = std::clamp(static_cast<float>(*value - minVal) / static_cast<float>(maxVal - minVal), 0.0f, 1.0f);
    DrawSliderTrackVisual(dl, pos, trackTop, w, hitH, S, pct, key, hovered, active);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, trackTop + hitH));
    ImGui::Dummy(ImVec2(w, 2.0f * S));
    return changed;
}

bool UrushiUI::Segmented(const std::string& id, int* currentIdx, const std::vector<std::string>& options) {
    if (!currentIdx || options.empty()) return false;
    const float S = s_uiScale;
    const int n = static_cast<int>(options.size());
    *currentIdx = std::clamp(*currentIdx, 0, n - 1);

    const float w = CurrentContentWidth();
    const float h = 34.0f * S;
    const float pad = 3.0f * S;
    const float segW = (w - pad * 2.0f) / static_cast<float>(n);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), kColBgControl, 8.0f * S);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), kColHairline, 8.0f * S);

    ImGui::PushID(id.c_str());
    ImGuiID pillKey = ImGui::GetID("##seg_pill");
    float animIdx = AnimateTowards(pillKey, static_cast<float>(*currentIdx), 20.0f);

    ImVec2 pillMin(pos.x + pad + animIdx * segW, pos.y + pad);
    ImVec2 pillMax(pillMin.x + segW, pos.y + h - pad);
    dl->AddRectFilled(pillMin, pillMax, kColBgRaised, 6.0f * S);
    dl->AddRect(pillMin, pillMax, kColHairlineStrong, 6.0f * S);

    bool changed = false;
    for (int i = 0; i < n; ++i) {
        ImVec2 sPos(pos.x + pad + static_cast<float>(i) * segW, pos.y + pad);
        ImGui::SetCursorScreenPos(sPos);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h - pad * 2.0f))) {
            if (*currentIdx != i) {
                *currentIdx = i;
                changed = true;
            }
        }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();

        ImU32 txtCol = (*currentIdx == i) ? kColTextPrimary : (hov ? kColTextPrimary : kColTextSecondary);
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImVec2 tSz = ImGui::CalcTextSize(options[i].c_str());
        const float segH = h - pad * 2.0f;
        dl->PushClipRect(ImVec2(sPos.x + 3.0f * S, sPos.y), ImVec2(sPos.x + segW - 3.0f * S, sPos.y + segH), true);
        float tx = sPos.x + (std::max)(4.0f * S, (segW - tSz.x) * 0.5f);
        dl->AddText(ImVec2(tx, sPos.y + (segH - tSz.y) * 0.5f), txtCol, options[i].c_str());
        dl->PopClipRect();
        if (AppFonts::Small) ImGui::PopFont();
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 2.0f * S));
    return changed;
}

bool UrushiUI::Button(const std::string& label, float width, float height, ButtonStyle style) {
    const float S = s_uiScale;
    float w = (width <= 0.0f) ? CurrentContentWidth() : width;
    if (height <= 0.0f) height = 30.0f * S;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool clicked = ImGui::InvisibleButton(label.c_str(), ImVec2(w, height));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiID key = ImGui::GetItemID();

    float hovT = AnimateTowards(key, hovered ? 1.0f : 0.0f, 22.0f);
    const float pressInset = active ? (1.0f * S) : 0.0f;
    const float btnR = 7.0f * S;

    ImU32 bg = kColBgRaised;
    ImU32 border = kColHairlineStrong;
    ImU32 fg = kColTextPrimary;

    if (style == ButtonStyle::Primary) {
        bg = LerpCol(GetAccentU32(), GetAccentHoverU32(), hovT);
        border = 0;
        fg = GetAccentFgU32();
    } else if (style == ButtonStyle::Danger) {
        bg = LerpCol(IM_COL32(190, 56, 56, 48), IM_COL32(224, 68, 68, 76), hovT);
        border = IM_COL32(232, 76, 76, 110);
        fg = IM_COL32(252, 165, 165, 255);
    } else if (style == ButtonStyle::Ghost) {
        bg = LerpCol(IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 18), hovT);
        border = kColHairline;
        fg = hovered ? kColTextPrimary : kColTextSecondary;
    } else {
        bg = LerpCol(kColBgRaised, kColBgRaisedHover, hovT);
        border = kColHairlineStrong;
    }

    ImVec2 bMin(pos.x + pressInset, pos.y + pressInset);
    ImVec2 bMax(pos.x + w - pressInset, pos.y + height - pressInset);
    dl->AddRectFilled(bMin, bMax, bg, btnR);
    if ((border >> IM_COL32_A_SHIFT) > 0) {
        dl->AddRect(bMin, bMax, border, btnR);
    }

    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;
    ImVec2 tSz = ImGui::CalcTextSize(vis.c_str());
    dl->PushClipRect(ImVec2(bMin.x + 4.0f * S, bMin.y + 1.0f * S), ImVec2(bMax.x - 4.0f * S, bMax.y - 1.0f * S), true);
    float tx = pos.x + (std::max)(6.0f * S, (w - tSz.x) * 0.5f);
    dl->AddText(ImVec2(tx, pos.y + (height - tSz.y) * 0.5f), fg, vis.c_str());
    dl->PopClipRect();

    return clicked;
}

void UrushiUI::StatRow(const std::vector<StatItem>& items) {
    if (items.empty()) return;
    const float S = s_uiScale;
    const float w = CurrentContentWidth();
    const float gap = 8.0f * S;
    const int n = static_cast<int>(items.size());
    const float cellW = (w - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float cellH = 54.0f * S;
    const float cellR = 9.0f * S;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < n; ++i) {
        ImVec2 cMin(pos.x + static_cast<float>(i) * (cellW + gap), pos.y);
        ImVec2 cMax(cMin.x + cellW, cMin.y + cellH);
        dl->AddRectFilled(cMin, cMax, kColBgCard, cellR);
        dl->AddRect(cMin, cMax, kColHairline, cellR);

        dl->PushClipRect(ImVec2(cMin.x + 6.0f * S, cMin.y + 4.0f * S), ImVec2(cMax.x - 6.0f * S, cMax.y - 4.0f * S), true);

        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        dl->AddText(ImVec2(cMin.x + 12.0f * S, cMin.y + 9.0f * S), kColTextSecondary, items[i].label.c_str());
        if (AppFonts::Small) ImGui::PopFont();

        ImFont* valFont = AppFonts::Large ? AppFonts::Large : AppFonts::Mono;
        if (valFont) ImGui::PushFont(valFont);
        ImVec2 valSz = ImGui::CalcTextSize(items[i].value.c_str());
        if (valFont) ImGui::PopFont();

        if (valSz.x > cellW - 22.0f * S && AppFonts::Regular) {
            valFont = AppFonts::Regular;
            ImGui::PushFont(valFont);
            valSz = ImGui::CalcTextSize(items[i].value.c_str());
            ImGui::PopFont();
            if (valSz.x > cellW - 22.0f * S && AppFonts::Small) {
                valFont = AppFonts::Small;
            }
        }

        if (valFont) ImGui::PushFont(valFont);
        ImVec2 drawSz = ImGui::CalcTextSize(items[i].value.c_str());
        float valY = (cMin.y + 26.0f * S) + (20.0f * S - drawSz.y) * 0.5f;
        dl->AddText(ImVec2(cMin.x + 12.0f * S, (std::max)(cMin.y + 25.0f * S, valY)), GetToneU32(items[i].tone), items[i].value.c_str());
        if (valFont) ImGui::PopFont();

        dl->PopClipRect();
    }

    ImGui::Dummy(ImVec2(w, cellH));
}

bool UrushiUI::CollapsingSection(const std::string& label, bool defaultOpen) {
    const float S = s_uiScale;
    const float w = CurrentContentWidth();
    const float h = 28.0f * S;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID stateId = ImGui::GetID(label.c_str());
    bool isOpen = storage->GetBool(stateId, defaultOpen);

    if (ImGui::InvisibleButton(label.c_str(), ImVec2(w, h))) {
        isOpen = !isOpen;
        storage->SetBool(stateId, isOpen);
    }
    bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), hov ? IM_COL32(255, 255, 255, 12) : IM_COL32(255, 255, 255, 5), 6.0f * S);

    // Chevron
    ImVec2 c(pos.x + 12.0f * S, pos.y + h * 0.5f);
    if (isOpen) {
        dl->AddTriangleFilled(ImVec2(c.x - 4.0f * S, c.y - 2.0f * S), ImVec2(c.x + 4.0f * S, c.y - 2.0f * S), ImVec2(c.x, c.y + 3.0f * S), kColTextSecondary);
    } else {
        dl->AddTriangleFilled(ImVec2(c.x - 2.0f * S, c.y - 4.0f * S), ImVec2(c.x + 3.0f * S, c.y), ImVec2(c.x - 2.0f * S, c.y + 4.0f * S), kColTextSecondary);
    }

    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;
    ImVec2 tSz = ImGui::CalcTextSize(vis.c_str());
    dl->AddText(ImVec2(pos.x + 24.0f * S, pos.y + (h - tSz.y) * 0.5f), kColTextPrimary, vis.c_str());

    return isOpen;
}

bool UrushiUI::Combo(const std::string& label, int* currentIdx, const std::vector<std::string>& items) {
    if (!currentIdx) return false;
    const float w = CurrentContentWidth();
    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;
    if (!vis.empty()) {
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImGui::TextColored(U32ToVec4(kColTextSecondary), "%s", vis.c_str());
        if (AppFonts::Small) ImGui::PopFont();
    }
    std::string hiddenLabel = hashPos ? std::string(hashPos) : ("##" + label);

    ImGui::PushItemWidth(w);
    std::vector<const char*> ptrs;
    ptrs.reserve(items.size());
    for (const auto& it : items) ptrs.push_back(it.c_str());
    bool ch = !ptrs.empty() && ImGui::Combo(hiddenLabel.c_str(), currentIdx, ptrs.data(), static_cast<int>(ptrs.size()));
    ImGui::PopItemWidth();
    return ch;
}

bool UrushiUI::InputText(const std::string& label, std::string& text, size_t maxLen) {
    const float w = CurrentContentWidth();
    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;
    if (!vis.empty()) {
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImGui::TextColored(U32ToVec4(kColTextSecondary), "%s", vis.c_str());
        if (AppFonts::Small) ImGui::PopFont();
    }
    std::string hiddenLabel = hashPos ? std::string(hashPos) : ("##" + label);

    ImGui::PushItemWidth(w);
    const size_t bufLen = (maxLen + 1 > 256) ? (maxLen + 1) : 256;
    std::vector<char> buf(bufLen, 0);
    strncpy_s(buf.data(), buf.size(), text.c_str(), buf.size() - 1);
    bool ch = ImGui::InputText(hiddenLabel.c_str(), buf.data(), buf.size());
    if (ch) text = buf.data();
    ImGui::PopItemWidth();
    return ch;
}

void UrushiUI::ProgressBar(float fraction, float width, float height, const char* overlay) {
    const float S = s_uiScale;
    float w = (width <= 0.0f) ? CurrentContentWidth() : width;
    if (height <= 0.0f) height = 14.0f * S;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height), kColBgControl, height * 0.5f);
    if (fraction > 0.0f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + std::max(height, w * fraction), pos.y + height), GetAccentU32(), height * 0.5f);
    }
    if (overlay && overlay[0]) {
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImVec2 oSz = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2(pos.x + (w - oSz.x) * 0.5f, pos.y + (height - oSz.y) * 0.5f), kColTextPrimary, overlay);
        if (AppFonts::Small) ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(w, height));
}

void UrushiUI::Badge(const std::string& text, Tone tone) {
    const float S = s_uiScale;
    if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
    ImVec2 tSz = ImGui::CalcTextSize(text.c_str());
    float bw = tSz.x + 22.0f * S;
    float bh = tSz.y + 6.0f * S;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 dotCol = GetToneU32(tone);
    dl->AddRectFilled(pos, ImVec2(pos.x + bw, pos.y + bh), kColBgControl, 6.0f * S);
    dl->AddRect(pos, ImVec2(pos.x + bw, pos.y + bh), kColHairline, 6.0f * S);
    dl->AddCircleFilled(ImVec2(pos.x + 8.0f * S, pos.y + bh * 0.5f), 2.5f * S, dotCol);
    dl->AddText(ImVec2(pos.x + 15.0f * S, pos.y + 3.0f * S), dotCol, text.c_str());
    if (AppFonts::Small) ImGui::PopFont();

    ImGui::Dummy(ImVec2(bw, bh));
}

void UrushiUI::Keycap(const std::string& keyText) {
    const float S = s_uiScale;
    if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
    ImVec2 tSz = ImGui::CalcTextSize(keyText.c_str());
    float kw = tSz.x + 12.0f * S;
    float kh = tSz.y + 4.0f * S;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + kw, pos.y + kh), kColBgControl, 5.0f * S);
    dl->AddRect(pos, ImVec2(pos.x + kw, pos.y + kh), kColHairlineStrong, 5.0f * S);
    dl->AddText(ImVec2(pos.x + 6.0f * S, pos.y + 2.0f * S), kColTextPrimary, keyText.c_str());
    if (AppFonts::Mono) ImGui::PopFont();

    ImGui::Dummy(ImVec2(kw, kh));
}

void UrushiUI::Text(const std::string& text, Tone tone) {
    ImGui::TextColored(GetToneVec4(tone), "%s", text.c_str());
}

void UrushiUI::TextWrapped(const std::string& text, Tone tone) {
    float wrapX = ImGui::GetCursorPosX() + CurrentContentWidth();
    ImGui::PushTextWrapPos(wrapX);
    ImGui::TextColored(GetToneVec4(tone), "%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void UrushiUI::TextColored(const ImVec4& col, const std::string& text) {
    ImGui::TextColored(col, "%s", text.c_str());
}

void UrushiUI::Separator() {
    const float S = s_uiScale;
    float w = CurrentContentWidth();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(pos, ImVec2(pos.x + w, pos.y), kColHairline);
    ImGui::Dummy(ImVec2(w, 2.0f * S));
}

void UrushiUI::Spacing(float px) {
    ImGui::Dummy(ImVec2(0.0f, px));
}
