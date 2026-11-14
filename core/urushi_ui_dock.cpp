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

namespace {


// Official 20x20 anti-aliased SVG alpha bitmaps (rasterized directly from official SVGs)
static const unsigned char kDiscordAlpha20[20 * 20] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,  24,  90,  43,   0,   0,   0,   0,  45,  89,  22,   0,   0,   0,   0,   0,
      0,   0,   1, 105, 201, 255, 255, 204, 173, 192, 192, 173, 206, 255, 255, 200, 104,   1,   0,   0,
      0,   0,  99, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  97,   0,   0,
      0,  10, 234, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 234,  10,   0,
      0, 120, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 113,   0,
      2, 232, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 226,   1,
     74, 255, 255, 255, 255, 255, 214, 250, 255, 255, 255, 255, 250, 214, 255, 255, 255, 255, 255,  59,
    136, 255, 255, 255, 255,  82,   0,  21, 230, 255, 255, 224,  20,   0,  90, 255, 255, 255, 255, 120,
    190, 255, 255, 255, 240,   0,   0,   0, 148, 255, 255, 146,   0,   0,   0, 240, 255, 255, 255, 178,
    227, 255, 255, 255, 254,  19,   0,   0, 179, 255, 255, 182,   0,   0,  18, 254, 255, 255, 255, 212,
    243, 255, 255, 255, 255, 192,  86, 144, 255, 255, 255, 255, 143,  86, 193, 255, 255, 255, 255, 236,
    252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 251,
    243, 255, 255, 255, 234, 235, 255, 255, 255, 255, 255, 255, 255, 255, 233, 235, 255, 255, 255, 239,
     60, 211, 255, 255, 242, 123,  68, 125, 155, 180, 180, 155, 124,  66, 126, 243, 255, 255, 208,  59,
      0,   2, 108, 219, 255, 214,   6,   0,   0,   0,   0,   0,   0,   7, 220, 255, 216, 105,   2,   0,
      0,   0,   0,   0,  67,  44,   0,   0,   0,   0,   0,   0,   0,   0,  48,  64,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static const unsigned char kGithubAlpha20[20 * 20] = {
      0,   0,   0,   0,   0,   0,  26,  98, 145, 177, 176, 144,  96,  24,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,  28, 169, 255, 255, 255, 255, 255, 255, 255, 254, 161,  22,   0,   0,   0,   0,
      0,   0,   0,  68, 241, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 235,  55,   0,   0,   0,
      0,   0,  79, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 250,  63,   0,   0,
      0,  43, 245, 255, 196,  90, 197, 255, 255, 255, 255, 255, 255, 191,  87, 205, 255, 236,  29,   0,
      1, 203, 255, 255, 126,   0,   1,  94,  26,   0,   0,  28,  89,   0,   0, 140, 255, 255, 182,   0,
     83, 255, 255, 255, 126,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 141, 255, 255, 255,  59,
    163, 255, 255, 255,  87,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  97, 255, 255, 255, 136,
    221, 255, 255, 219,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 229, 255, 255, 196,
    241, 255, 255, 169,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 181, 255, 255, 229,
    248, 255, 255, 171,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 182, 255, 255, 247,
    227, 255, 255, 216,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 231, 255, 255, 224,
    182, 255, 255, 255,  53,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  75, 255, 255, 255, 177,
    118, 255, 255, 255, 226,  46,   0,   0,   0,   0,   0,   0,   0,   0,  60, 234, 255, 255, 255, 111,
     18, 244, 227, 122, 234, 253, 191, 113,   3,   0,   0,   9, 121, 201, 255, 255, 255, 255, 236,  12,
      0, 127, 255, 188,  56, 240, 255, 180,   0,   0,   0,   0, 207, 255, 255, 255, 255, 255, 112,   0,
      0,  11, 203, 255,  66,  20,  64,  18,   0,   0,   0,   0, 145, 255, 255, 255, 255, 189,   6,   0,
      0,   0,  14, 193, 241, 123,  86,  64,   0,   0,   0,   0, 141, 255, 255, 255, 176,   9,   0,   0,
      0,   0,   0,   5, 150, 255, 255, 128,   0,   0,   0,   0, 146, 255, 239, 110,   1,   0,   0,   0,
      0,   0,   0,   0,   0,  69, 162,  73,   0,   0,   0,   0,  84, 139,  18,   0,   0,   0,   0,   0,
};

} // namespace

namespace UrushiUiInternal {

// Official Lucide refresh-cw 16x16 anti-aliased SVG alpha bitmap
extern const unsigned char kLucideRefreshAlpha16[16 * 16] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,  60, 119, 168, 169, 122,  69,   0,   0, 115, 111,   0,
      0,   0,   0,  18, 169, 255, 255, 224, 220, 255, 255, 195,  36, 214, 213,   0,
      0,   0,  22, 230, 253, 144,  34,   0,   0,  24, 133, 254, 249, 227, 213,   0,
      0,   0, 176, 253,  57,   0,   0,   0,   0,   1, 105, 156, 245, 255, 226,   0,
      0,  42, 255, 134,   0,   0,   0,   0,   0,  15, 243, 255, 255, 255, 234,   0,
      0, 112, 255,  26,   0,   0,   0,   0,   0,   0,  31,  64,  64,  85,  34,   0,
      0, 182, 239,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 115, 111,   0,
      0, 115, 108,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 240, 187,   0,
      0,  37,  77,  64,  64,  40,   0,   0,   0,   0,   0,   0,  31, 255, 136,   0,
      0, 241, 255, 255, 255, 255,  19,   0,   0,   0,   0,   0, 139, 255,  85,   0,
      0, 227, 255, 244, 155, 128,   1,   0,   0,   0,   0,  56, 253, 204,   8,   0,
      0, 214, 227, 249, 253, 132,  22,   0,   0,   0, 100, 248, 230,  27,   0,   0,
      0, 214, 213,  37, 194, 255, 255, 218, 209, 241, 255, 161,  17,   0,   0,   0,
      0, 115, 108,   0,   1,  69, 123, 170, 168, 119,  58,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

void DrawAlphaMask(ImDrawList* dl, ImVec2 pos, float size, const unsigned char* mask, int w, int h, ImU32 baseCol) {
    const float pixelW = size / static_cast<float>(w);
    const float pixelH = size / static_cast<float>(h);
    const ImVec4 base = ImGui::ColorConvertU32ToFloat4(baseCol);

    for (int y = 0; y < h; ++y) {
        int runStart = -1;
        float alphaSum = 0.0f;
        for (int x = 0; x <= w; ++x) {
            unsigned char a = (x < w) ? mask[y * w + x] : 0;
            if (a > 20) {
                if (runStart < 0) {
                    runStart = x;
                    alphaSum = a / 255.0f;
                } else {
                    alphaSum += a / 255.0f;
                }
            } else {
                if (runStart >= 0) {
                    int len = x - runStart;
                    float avgAlpha = std::min(1.0f, (alphaSum / len) * 1.15f);
                    ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, base.w * avgAlpha));
                    dl->AddRectFilled(
                        ImVec2(pos.x + runStart * pixelW, pos.y + y * pixelH),
                        ImVec2(pos.x + x * pixelW, pos.y + (y + 1) * pixelH),
                        col);
                    runStart = -1;
                    alphaSum = 0.0f;
                }
            }
        }
    }
}

std::wstring DockOrderPath() {
    const std::wstring dir = Settings::GameDir();
    return dir.empty() ? std::wstring() : dir + L"\\urushi_dock.ini";
}
void LoadDockOrder() {
    if (g_dockOrderLoaded) return;
    const std::wstring path = DockOrderPath();
    if (path.empty()) return;
    g_dockOrderLoaded = true;
    const int n = static_cast<int>(GetPrivateProfileIntW(L"order", L"count", 0, path.c_str()));
    for (int i = 0; i < n && i < 512; ++i) {
        wchar_t buf[256] = {};
        GetPrivateProfileStringW(L"order", std::to_wstring(i).c_str(), L"", buf, 256, path.c_str());
        if (buf[0]) g_dockOrder.push_back(Logger::ToNarrow(buf));
    }
}
int DockRank(const std::string& id) {
    for (size_t i = 0; i < g_dockOrder.size(); ++i) if (g_dockOrder[i] == id) return static_cast<int>(i);
    return 1 << 20;   // not saved yet: after the ones the user ordered
}

// Vector 16px icons drawn via ImDrawList
void DrawModuleIcon(ImDrawList* dl, ImVec2 center, const std::string& icon, ImU32 col, float s) {
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
    } else if (icon == "library") {
        // three books on a shelf
        const float w = 3.0f * s, g = 1.2f * s, base = center.y + 6.0f * s;
        float x = center.x - 5.4f * s;
        const float hs[3] = { 10.0f * s, 12.5f * s, 9.0f * s };
        for (int i = 0; i < 3; ++i) {
            dl->AddRect(ImVec2(x, base - hs[i]), ImVec2(x + w, base), col, 0.0f, ImDrawFlags_None, th * 0.9f);
            x += w + g;
        }
    } else if (icon == "wheel") {
        dl->AddCircle(center, r, col, 20, th);
        dl->AddCircle(center, 1.9f * s, col, 12, th * 0.9f);
        dl->AddLine(ImVec2(center.x - r, center.y), ImVec2(center.x - 1.9f * s, center.y), col, th);
        dl->AddLine(ImVec2(center.x + 1.9f * s, center.y), ImVec2(center.x + r, center.y), col, th);
        dl->AddLine(ImVec2(center.x, center.y + 1.9f * s), ImVec2(center.x, center.y + r), col, th);
    } else if (icon == "keyboard") {
        dl->AddRect(ImVec2(center.x - 6.5f * s, center.y - 4.5f * s),
                    ImVec2(center.x + 6.5f * s, center.y + 4.5f * s), col, 1.5f * s, 0, th);
        dl->AddLine(ImVec2(center.x - 4.0f * s, center.y - 2.0f * s), ImVec2(center.x - 2.5f * s, center.y - 2.0f * s), col, th);
        dl->AddLine(ImVec2(center.x - 0.7f * s, center.y - 2.0f * s), ImVec2(center.x + 0.8f * s, center.y - 2.0f * s), col, th);
        dl->AddLine(ImVec2(center.x + 2.5f * s, center.y - 2.0f * s), ImVec2(center.x + 4.0f * s, center.y - 2.0f * s), col, th);
        dl->AddLine(ImVec2(center.x - 3.5f * s, center.y + 1.8f * s), ImVec2(center.x + 3.5f * s, center.y + 1.8f * s), col, th);
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

void RenderDock(bool menuOpen) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;
    if (!menuOpen) { g_dockEdit = false; g_dragId.clear(); }   // closing the menu leaves reorder mode
    const float targetAlpha = menuOpen ? 1.0f : 0.0f;
    g_dockAlpha += (targetAlpha - g_dockAlpha) * (1.0f - std::exp(-20.0f * dt));
    if (!menuOpen && g_dockAlpha < 0.02f) {
        g_dockAlpha = 0.0f;
        return;
    }

    const float S = g_uiScale;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dockW = 246.0f * S;
    const float padX = 12.0f * S;
    const float padY = 12.0f * S;
    const float innerW = dockW - padX * 2.0f;
    const size_t modCount = g_windows.size();
    const float headerTotal = 36.0f * S;
    const float availListH = std::max(80.0f, vp->WorkSize.y - 24.0f * S - padY * 2.0f - headerTotal);
    const float btnH = 34.0f * S;
    const float btnGap = 4.0f * S;
    // At most kDockMaxVisible entries are shown at once (fewer if the screen is short); the rest scrolls with the wheel.
    size_t visibleCount = std::min<size_t>(modCount, kDockMaxVisible);
    while (visibleCount > 3 && static_cast<float>(visibleCount) * (btnH + btnGap) - btnGap > availListH) --visibleCount;
    const bool scrolling = modCount > visibleCount;
    const float listH = (modCount > 0)
        ? (static_cast<float>(visibleCount) * btnH + static_cast<float>(visibleCount - 1) * btnGap)
        : 24.0f * S;
    const float dockH = std::min(vp->WorkSize.y - 24.0f * S, padY * 2.0f + headerTotal + listH);
    const float dockX = vp->WorkPos.x + 18.0f * S;
    const float dockY = vp->WorkPos.y + (vp->WorkSize.y - dockH) * 0.5f;

    g_lastWorkW = vp->WorkSize.x;
    g_lastWorkH = vp->WorkSize.y;
    g_lastDockX = dockX - vp->WorkPos.x;
    g_lastDockY = dockY - vp->WorkPos.y;
    g_lastDockW = dockW;
    g_lastDockH = dockH;

    ImGui::SetNextWindowPos(ImVec2(dockX, dockY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dockW, dockH), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_dockAlpha);
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

        // Discord link button
        {
            const float tw = 20.0f * S;
            const ImVec2 tp(hPos.x + 98.0f * S, hPos.y + 1.0f * S);
            ImGui::SetCursorScreenPos(tp);
            if (ImGui::InvisibleButton("##dock_discord", ImVec2(tw, 20.0f * S))) {
                ShellExecuteW(nullptr, L"open", L"https://discord.gg/jS6cc49EbT", nullptr, nullptr, SW_SHOWNORMAL);
            }
            const bool hv = ImGui::IsItemHovered();
            if (hv) {
                dl->AddRectFilled(tp, ImVec2(tp.x + tw, tp.y + 20.0f * S), IM_COL32(88, 101, 242, 45), 4.0f * S);
                dl->AddRect(tp, ImVec2(tp.x + tw, tp.y + 20.0f * S), IM_COL32(88, 101, 242, 180), 4.0f * S);
                ImGui::SetTooltip("%s", g_language == UrushiUI::Language::Russian
                    ? u8"Discord: сообщество URUSHI (чат и поддержка)"
                    : "Discord: URUSHI community (chat & support)");
            }
            const ImU32 col = hv ? IM_COL32(88, 101, 242, 255) : kColTextMuted;
            DrawAlphaMask(dl, ImVec2(tp.x + 3.0f * S, tp.y + 3.0f * S), 14.0f * S, kDiscordAlpha20, 20, 20, col);
        }

        // GitHub link button
        {
            const float tw = 20.0f * S;
            const ImVec2 tp(hPos.x + 124.0f * S, hPos.y + 1.0f * S);
            ImGui::SetCursorScreenPos(tp);
            if (ImGui::InvisibleButton("##dock_github", ImVec2(tw, 20.0f * S))) {
                ShellExecuteW(nullptr, L"open", L"https://github.com/Garagoro/Urushi", nullptr, nullptr, SW_SHOWNORMAL);
            }
            const bool hv = ImGui::IsItemHovered();
            if (hv) {
                dl->AddRectFilled(tp, ImVec2(tp.x + tw, tp.y + 20.0f * S), IM_COL32(255, 255, 255, 25), 4.0f * S);
                dl->AddRect(tp, ImVec2(tp.x + tw, tp.y + 20.0f * S), IM_COL32(255, 255, 255, 120), 4.0f * S);
                ImGui::SetTooltip("%s", g_language == UrushiUI::Language::Russian
                    ? u8"GitHub: репозиторий, исходный код и релизы"
                    : "GitHub: repository, source code & releases");
            }
            const ImU32 col = hv ? IM_COL32(255, 255, 255, 255) : kColTextMuted;
            DrawAlphaMask(dl, ImVec2(tp.x + 3.0f * S, tp.y + 3.0f * S), 14.0f * S, kGithubAlpha20, 20, 20, col);
        }

        std::string verLabel = std::string("v") + URUSHI_VERSION;
        if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
        ImVec2 verSz = ImGui::CalcTextSize(verLabel.c_str());
        dl->AddText(ImVec2(hPos.x + innerW - verSz.x, hPos.y + 4.0f * S), kColTextMuted, verLabel.c_str());
        if (AppFonts::Small) ImGui::PopFont();

        // reorder toggle (left of the version label)
        {
            const float tw = 20.0f * S;
            const ImVec2 tp(hPos.x + innerW - verSz.x - 6.0f * S - tw, hPos.y + 1.0f * S);
            ImGui::SetCursorScreenPos(tp);
            if (ImGui::InvisibleButton("##dockedit", ImVec2(tw, 20.0f * S))) {
                g_dockEdit = !g_dockEdit;
            }
            const bool hv = ImGui::IsItemHovered();
            if (g_dockEdit || hv) {
                dl->AddRectFilled(tp, ImVec2(tp.x + tw, tp.y + 20.0f * S),
                                  g_dockEdit ? UrushiUI::GetAccentU32() : IM_COL32(255, 255, 255, 20), 4.0f * S);
            }
            const ImU32 gc = g_dockEdit ? IM_COL32(20, 20, 24, 255) : (hv ? kColTextPrimary : kColTextMuted);
            for (int k = 0; k < 3; ++k) {   // three bars = "reorder"
                const float y = tp.y + (6.0f + 4.0f * k) * S;
                dl->AddLine(ImVec2(tp.x + 4.0f * S, y), ImVec2(tp.x + tw - 4.0f * S, y), gc, 1.5f * S);
            }
            if (hv) ImGui::SetTooltip("%s", g_language == UrushiUI::Language::Russian
                ? u8"Изменить порядок (перетаскивание)" : "Reorder (drag & drop)");
            ImGui::SetCursorScreenPos(hPos);
        }

        ImGui::Dummy(ImVec2(innerW, 24.0f * S));
        dl->AddLine(ImVec2(hPos.x, hPos.y + 29.0f * S), ImVec2(hPos.x + innerW, hPos.y + 29.0f * S), kColHairline);
        ImGui::Dummy(ImVec2(innerW, 4.0f * S));

        // 2. Rows. Hit-testing uses the natural layout; visuals are drawn at animated offsets, so rows
        //    slide out of the way while one is carried by the cursor and settle into the nearest slot on release.
        const float listW = scrolling ? innerW - 8.0f * S : innerW;   // leave room for the scrollbar
        const float step = btnH + btnGap;
        if (scrolling) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * S);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("##dockscroll", ImVec2(innerW, listH), false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
            dl = ImGui::GetWindowDrawList();
            // smooth wheel scrolling: the wheel moves a target, the view eases towards it (fast enough to feel instant)
            const float cur = ImGui::GetScrollY();
            if (g_dockScrollTarget < 0.0f) g_dockScrollTarget = cur;
            if (ImGui::IsWindowHovered() && g_dragId.empty() && ImGui::GetIO().MouseWheel != 0.0f)
                g_dockScrollTarget -= ImGui::GetIO().MouseWheel * step * 1.5f;
            if (!g_dragId.empty()) g_dockScrollTarget = cur;   // edge auto-scroll while carrying drives the view directly
            g_dockScrollTarget = std::clamp(g_dockScrollTarget, 0.0f, ImGui::GetScrollMaxY());
            if (std::fabs(g_dockScrollTarget - cur) > 0.5f)
                ImGui::SetScrollY(cur + (g_dockScrollTarget - cur) * (1.0f - std::exp(-22.0f * dt)));
            else
                ImGui::SetScrollY(g_dockScrollTarget);
        } else {
            g_dockScrollTarget = -1.0f;
        }
        const ImVec2 listTop = ImGui::GetCursorScreenPos();            // natural position of row 0 (scroll included)
        const int rowCount = static_cast<int>(g_windows.size());
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        auto drawRow = [&](ImDrawList* d, ImVec2 rPos, const WindowState& w, float hoverT, float lift) {
            ImU32 bgCol = w.open
                ? LerpCol(IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 26), hoverT)
                : LerpCol(IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 14), hoverT);
            if (lift > 0.0f) {
                // carried row: shadow + solid card + accent outline
                const ImVec2 a0(rPos.x - 2.0f * S, rPos.y - 2.0f * S), a1(rPos.x + listW + 2.0f * S, rPos.y + btnH + 2.0f * S);
                d->AddRectFilled(ImVec2(a0.x, a0.y + 5.0f * S), ImVec2(a1.x, a1.y + 5.0f * S), IM_COL32(0, 0, 0, static_cast<int>(90 * lift)), 10.0f * S);
                d->AddRectFilled(a0, a1, IM_COL32(30, 30, 38, 250), 10.0f * S);
                d->AddRect(a0, a1, UrushiUI::GetAccentU32(), 10.0f * S, 0, 1.2f * S);
            } else if ((bgCol >> IM_COL32_A_SHIFT) > 0) {
                d->AddRectFilled(rPos, ImVec2(rPos.x + listW, rPos.y + btnH), bgCol, 8.0f * S);
            }
            if (w.open) {
                d->AddRectFilled(ImVec2(rPos.x + 2.0f * S, rPos.y + 7.0f * S),
                                 ImVec2(rPos.x + 4.5f * S, rPos.y + btnH - 7.0f * S), UrushiUI::GetAccentU32(), 2.0f * S);
            }
            const ImU32 fgCol = (w.open || lift > 0.0f) ? kColTextPrimary : LerpCol(kColTextSecondary, kColTextPrimary, hoverT);
            DrawModuleIcon(d, ImVec2(rPos.x + 18.0f * S, rPos.y + btnH * 0.5f), w.icon, fgCol, S);

            const float rightX = rPos.x + listW - 10.0f * S;
            float titleClipMaxX = rightX - 10.0f * S;
            if (w.pinned) titleClipMaxX -= 28.0f * S;
            const ImVec2 txtSz = ImGui::CalcTextSize(w.title.c_str());
            d->PushClipRect(ImVec2(rPos.x + 33.0f * S, rPos.y - 4.0f * S), ImVec2(std::max(rPos.x + 40.0f * S, titleClipMaxX), rPos.y + btnH + 4.0f * S), true);
            d->AddText(ImVec2(rPos.x + 35.0f * S, rPos.y + (btnH - txtSz.y) * 0.5f), fgCol, w.title.c_str());
            d->PopClipRect();

            if (g_dockEdit) {
                for (int k = 0; k < 3; ++k) {   // grip bars: the row can be picked up
                    const float y = rPos.y + btnH * 0.5f + (k - 1) * 4.0f * S;
                    d->AddLine(ImVec2(rightX - 8.0f * S, y), ImVec2(rightX + 2.0f * S, y),
                               lift > 0.0f ? UrushiUI::GetAccentU32() : kColTextMuted, 1.4f * S);
                }
            } else {
                const ImU32 dotCol = w.open ? UrushiUI::GetAccentU32() : IM_COL32(255, 255, 255, 32);
                d->AddCircleFilled(ImVec2(rightX - 3.0f * S, rPos.y + btnH * 0.5f), 3.0f * S, dotCol);
                if (w.pinned) {
                    if (AppFonts::Small) ImGui::PushFont(AppFonts::Small);
                    const ImVec2 pinSz = ImGui::CalcTextSize("PIN");
                    d->AddText(ImVec2(rightX - 14.0f * S - pinSz.x, rPos.y + (btnH - pinSz.y) * 0.5f), kColTextMuted, "PIN");
                    if (AppFonts::Small) ImGui::PopFont();
                }
            }
        };

        // carried row: where it is in the list, and where it would land
        int dragIdx = -1;
        if (!g_dragId.empty()) {
            for (int i = 0; i < rowCount; ++i) if (g_windows[i].id == g_dragId) dragIdx = i;
            if (dragIdx < 0 || !g_dockEdit) { g_dragId.clear(); dragIdx = -1; }
        }
        int insertIdx = dragIdx;
        if (dragIdx >= 0) {
            const float k = 1.0f - std::exp(-30.0f * dt);   // magnet: the row chases the cursor
            g_dragPos.x += (mouse.x - g_dragGrab.x - g_dragPos.x) * k;
            g_dragPos.y += (mouse.y - g_dragGrab.y - g_dragPos.y) * k;
            insertIdx = std::clamp(static_cast<int>(std::floor((g_dragPos.y - listTop.y) / step + 0.5f)), 0, rowCount - 1);
            if (scrolling) {   // edge auto-scroll while carrying
                const float top = ImGui::GetWindowPos().y, bot = top + listH;
                if (mouse.y < top + 26.0f * S) ImGui::SetScrollY(ImGui::GetScrollY() - 420.0f * S * dt);
                else if (mouse.y > bot - 26.0f * S) ImGui::SetScrollY(ImGui::GetScrollY() + 420.0f * S * dt);
            }
        }

        const float animK = 1.0f - std::exp(-18.0f * dt);
        for (int i = 0; i < rowCount; ++i) {
            WindowState& w = g_windows[i];
            const ImVec2 nat = ImGui::GetCursorScreenPos();

            ImGui::PushID(w.id.c_str());
            const bool clicked = ImGui::InvisibleButton("##dockbtn", ImVec2(listW, btnH));
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            const ImGuiID animKey = ImGui::GetItemID();
            ImGui::PopID();

            ImVec2& off = g_rowOff[w.id];
            if (g_dockEdit && g_dragId.empty() && active && ImGui::IsMouseDragging(0, 4.0f * S)) {
                const ImVec2 cp = ImGui::GetIO().MouseClickedPos[0];
                g_dragId = w.id;
                g_dragPos = ImVec2(nat.x + off.x, nat.y + off.y);
                g_dragGrab = ImVec2(cp.x - g_dragPos.x, cp.y - g_dragPos.y);
                dragIdx = i;
                insertIdx = i;
            }

            ImVec2 tgt(0.0f, 0.0f);
            if (dragIdx >= 0 && i != dragIdx) {
                int disp = (i > dragIdx) ? i - 1 : i;   // index with the carried row taken out
                if (disp >= insertIdx) ++disp;          // ...and a gap opened at the landing slot
                tgt.y = static_cast<float>(disp - i) * step;
            }
            off.x += (tgt.x - off.x) * animK;
            off.y += (tgt.y - off.y) * animK;
            if (std::fabs(off.x - tgt.x) < 0.2f) off.x = tgt.x;
            if (std::fabs(off.y - tgt.y) < 0.2f) off.y = tgt.y;

            const float hoverT = AnimateTowards(animKey, (hovered && dragIdx < 0) ? 1.0f : 0.0f, 20.0f);
            if (i == dragIdx) {
                // landing slot marker (the card itself is drawn on top of everything below)
                const ImVec2 sp(nat.x, listTop.y + static_cast<float>(insertIdx) * step);
                dl->AddRectFilled(sp, ImVec2(sp.x + listW, sp.y + btnH), IM_COL32(255, 255, 255, 10), 8.0f * S);
                dl->AddRect(sp, ImVec2(sp.x + listW, sp.y + btnH), UrushiUI::GetAccentU32(), 8.0f * S, 0, 1.0f * S);
            } else {
                drawRow(dl, ImVec2(nat.x + off.x, nat.y + off.y), w, hoverT, 0.0f);
            }

            if (!g_dockEdit && clicked) {
                w.open = !w.open;
                if (w.open) w.openAnim = 0.0f;
            }
        }

        if (dragIdx >= 0) {
            // the carried card floats above everything (may leave the dock)
            drawRow(ImGui::GetForegroundDrawList(), g_dragPos, g_windows[dragIdx], 1.0f, 1.0f);

            if (!ImGui::IsMouseDown(0)) {
                // release: commit the nearest slot; every row keeps its current screen position and glides into place
                const int from = dragIdx, to = insertIdx;
                std::vector<int> newIndex(rowCount);
                for (int i = 0; i < rowCount; ++i) {
                    int ni = i;
                    if (i == from) ni = to;
                    else if (from < to && i > from && i <= to) ni = i - 1;
                    else if (from > to && i >= to && i < from) ni = i + 1;
                    newIndex[i] = ni;
                }
                for (int i = 0; i < rowCount; ++i) {
                    ImVec2& o = g_rowOff[g_windows[i].id];
                    if (i == from) {
                        o = ImVec2(g_dragPos.x - listTop.x, g_dragPos.y - (listTop.y + static_cast<float>(to) * step));
                    } else {
                        o.y += static_cast<float>(i - newIndex[i]) * step;
                    }
                }
                WindowState moved = std::move(g_windows[from]);
                g_windows.erase(g_windows.begin() + from);
                g_windows.insert(g_windows.begin() + to, std::move(moved));
                g_dragId.clear();

                g_dockOrder.clear();
                for (const auto& w2 : g_windows) g_dockOrder.push_back(w2.id);
                const std::wstring path = DockOrderPath();
                if (!path.empty()) {
                    WritePrivateProfileStringW(L"order", L"count", std::to_wstring(g_dockOrder.size()).c_str(), path.c_str());
                    for (size_t k = 0; k < g_dockOrder.size(); ++k)
                        WritePrivateProfileStringW(L"order", std::to_wstring(k).c_str(), Logger::ToWide(g_dockOrder[k]).c_str(), path.c_str());
                }
            }
        }
        if (scrolling) {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);
}

// ==================== Floating Windows Rendering ====================

void RenderFloatingWindows(bool menuOpen, lua_State* L) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float S = g_uiScale;

    const bool firstLayout = (g_layoutWorkW <= 0.0f || g_layoutWorkH <= 0.0f || g_layoutScale <= 0.0f);
    const bool viewportChanged = !firstLayout &&
        (std::fabs(vp->WorkSize.x - g_layoutWorkW) > 1.0f ||
         std::fabs(vp->WorkSize.y - g_layoutWorkH) > 1.0f ||
         std::fabs(S - g_layoutScale) > 0.002f);

    if (!g_previewActive && (firstLayout || viewportChanged)) {
        if (viewportChanged) {
            const float prevMaxWinH = std::min(960.0f * S, std::max(240.0f * S, vp->WorkSize.y * 0.90f));
            for (auto& win : g_windows) {
                const float targetW = std::min(win.width * S, std::max(220.0f * S, vp->WorkSize.x - 24.0f * S));
                if (!win.userMoved) {
                    const float estOuterH = std::min(prevMaxWinH, (40.0f + 22.0f + win.lastBodyH) * S);
                    ImVec2 defPos = ComputeDefaultOrigin(win, S, vp->WorkSize.x, vp->WorkSize.y, targetW, estOuterH);
                    win.posX = defPos.x;
                    win.posY = defPos.y;
                } else {
                    const float oldSpanX = std::max(1.0f, g_layoutWorkW - win.actualW - 24.0f * g_layoutScale);
                    const float oldSpanY = std::max(1.0f, g_layoutWorkH - win.actualH - 24.0f * g_layoutScale);
                    const float fracX = std::clamp((win.posX - 12.0f * g_layoutScale) / oldSpanX, 0.0f, 1.0f);
                    const float fracY = std::clamp((win.posY - 12.0f * g_layoutScale) / oldSpanY, 0.0f, 1.0f);
                    const float newSpanX = std::max(0.0f, vp->WorkSize.x - targetW - 24.0f * S);
                    const float newSpanY = std::max(0.0f, vp->WorkSize.y - std::min(vp->WorkSize.y * 0.85f, win.actualH * (S / std::max(0.1f, g_layoutScale))) - 24.0f * S);
                    win.posX = std::clamp(12.0f * S + fracX * newSpanX, 12.0f * S, std::max(12.0f * S, vp->WorkSize.x - targetW - 12.0f * S));
                    win.posY = std::clamp(12.0f * S + fracY * newSpanY, 12.0f * S, std::max(12.0f * S, vp->WorkSize.y - 120.0f * S));
                }
                win.pendingMoveKey = "relayout";
            }
        }
        g_layoutWorkW = vp->WorkSize.x;
        g_layoutWorkH = vp->WorkSize.y;
        g_layoutScale = S;
    }

    g_lastWorkW = vp->WorkSize.x;
    g_lastWorkH = vp->WorkSize.y;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;

    const float headerH = 40.0f * S;
    const float bodyPadX = 14.0f * S;
    const float bodyPadTop = 10.0f * S;
    const float bodyPadBot = 12.0f * S;
    const float maxWinH = std::min(960.0f * S, std::max(240.0f * S, vp->WorkSize.y * 0.90f));
    const float maxBodyH = std::max(110.0f * S, maxWinH - headerH - bodyPadTop - bodyPadBot);
    const float winRounding = 12.0f * S;

    for (size_t i = 0; i < g_windows.size(); ++i) {
        WindowState& w = g_windows[i];
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

        if (g_previewActive) {
            ImVec2 prevPos = ComputeDefaultOrigin(w, S, vp->WorkSize.x, vp->WorkSize.y, targetW, outerH);
            if (w.userMoved) {
                const float refW = (g_layoutWorkW > 1.0f) ? g_layoutWorkW : vp->WorkSize.x;
                const float refH = (g_layoutWorkH > 1.0f) ? g_layoutWorkH : vp->WorkSize.y;
                const float refS = (g_layoutScale > 0.1f) ? g_layoutScale : S;
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
            if (!g_previewActive) {
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
                        UrushiUI::SetCurrentRenderingWindow(w.id);
                        lua_rawgeti(L, LUA_REGISTRYINDEX, w.luaRenderRef);
                        if (lua_isfunction(L, -1)) {
                            lua_getglobal(L, "ui");
                            if (LuaSandbox::PCall(L, 1, 0, "window_render") != LUA_OK) {
                                const char* err = lua_tostring(L, -1);
                                Logger::Error("URUSHI UI render error (" + w.id + "): " + std::string(err ? err : "unknown"));
                                lua_pop(L, 1);
                                while (!g_cardStack.empty()) {
                                    UrushiUI::EndCard();
                                }
                            }
                        } else {
                            lua_pop(L, 1);
                        }
                        UrushiUI::SetCurrentRenderingWindow("");
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

} // namespace UrushiUiInternal

void UrushiUI::RegisterWindow(const WindowRegistration& reg) {
    if (reg.id.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

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
            if (g_layoutWorkW > 1.0f && g_layoutWorkH > 1.0f) {
                const float targetW = std::min(existing->width * g_uiScale, std::max(220.0f * g_uiScale, g_layoutWorkW - 24.0f * g_uiScale));
                ImVec2 defPos = ComputeDefaultOrigin(*existing, g_uiScale, g_layoutWorkW, g_layoutWorkH, targetW, existing->actualH);
                existing->posX = defPos.x;
                existing->posY = defPos.y;
            } else {
                existing->posX = reg.defaultX * g_uiScale;
                existing->posY = reg.defaultY * g_uiScale;
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
        st.posX = reg.defaultX * g_uiScale;
        st.posY = reg.defaultY * g_uiScale;
        st.width = reg.width;
        st.luaRenderRef = reg.luaRenderRef;
        st.registeredInPass = true;
        g_windows.push_back(std::move(st));
    }

    LoadDockOrder();
    std::sort(g_windows.begin(), g_windows.end(), [](const WindowState& a, const WindowState& b) {
        const int ra = DockRank(a.id), rb = DockRank(b.id);
        if (ra != rb) return ra < rb;
        if (a.order != b.order) return a.order < b.order;
        return a.title < b.title;
    });
}

void UrushiUI::Render(bool menuOpen, lua_State* L) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    RenderDock(menuOpen);
    RenderFloatingWindows(menuOpen, L);
}
