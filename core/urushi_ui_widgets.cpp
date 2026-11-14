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

// ==================== Lacquered ImDrawList Widgets ====================

float UrushiUI::CurrentContentWidth() {
    if (!g_cardStack.empty()) {
        return g_cardStack.back()->innerWidth;
    }
    return std::max(60.0f * g_uiScale, ImGui::GetContentRegionAvail().x);
}

void UrushiUI::BeginCard(const std::string& title, const std::string& subtitle) {
    const float S = g_uiScale;
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

    g_cardStack.push_back(std::move(frame));
}

void UrushiUI::EndCard() {
    if (g_cardStack.empty()) return;
    const float S = g_uiScale;
    const float padY = 12.0f * S;
    const float cardR = 10.0f * S;

    std::unique_ptr<CardFrame> frame = std::move(g_cardStack.back());
    g_cardStack.pop_back();

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

bool UrushiUI::Toggle(const std::string& label, bool* value, float width) {
    if (!value) return false;
    const float S = g_uiScale;
    const float w = (width <= 0.0f) ? CurrentContentWidth() : width;
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
    const float S = g_uiScale;
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
    const float S = g_uiScale;
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
    const float S = g_uiScale;
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

static void DrawRefreshIcon(ImDrawList* dl, ImVec2 center, float r, ImU32 col, float th) {
    // 300 degree clockwise circular arc
    dl->PathArcTo(center, r, -2.4f, 2.3f, 20);
    dl->PathStroke(col, ImDrawFlags_None, th);
    // Arrowhead at the end (angle ~ 2.3f)
    const float a = 2.3f;
    const float tx = center.x + r * cosf(a);
    const float ty = center.y + r * sinf(a);
    const float nx = -sinf(a);
    const float ny = cosf(a);
    const float px = -ny;
    const float py = nx;
    const float arrowLen = 3.6f * (th / 1.5f);
    const float arrowWidth = 2.8f * (th / 1.5f);
    ImVec2 pTip(tx, ty);
    ImVec2 pBase(tx - nx * arrowLen, ty - ny * arrowLen);
    ImVec2 p1(pBase.x + px * arrowWidth, pBase.y + py * arrowWidth);
    ImVec2 p2(pBase.x - px * arrowWidth, pBase.y - py * arrowWidth);
    dl->AddTriangleFilled(pTip, p1, p2, col);
}

static void DrawCheckIcon(ImDrawList* dl, ImVec2 center, float r, ImU32 col, float th) {
    ImVec2 p1(center.x - r * 0.7f, center.y);
    ImVec2 p2(center.x - r * 0.15f, center.y + r * 0.55f);
    ImVec2 p3(center.x + r * 0.75f, center.y - r * 0.55f);
    dl->AddLine(p1, p2, col, th);
    dl->AddLine(p2, p3, col, th);
}

static void DrawCloseIcon(ImDrawList* dl, ImVec2 center, float r, ImU32 col, float th) {
    const float sz = r * 0.75f;
    dl->AddLine(ImVec2(center.x - sz, center.y - sz), ImVec2(center.x + sz, center.y + sz), col, th);
    dl->AddLine(ImVec2(center.x + sz, center.y - sz), ImVec2(center.x - sz, center.y + sz), col, th);
}

bool UrushiUI::Button(const std::string& label, float width, float height, ButtonStyle style) {
    const float S = g_uiScale;
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

    if (vis == "@refresh" || vis == "refresh" || vis == "\xe2\x86\xbb" || vis == "\xe2\x9f\xb3") {
        const float iconSz = 14.0f * S;
        ImVec2 ipos(pos.x + (w - iconSz) * 0.5f, pos.y + (height - iconSz) * 0.5f);
        DrawAlphaMask(dl, ipos, iconSz, kLucideRefreshAlpha16, 16, 16, fg);
    } else if (vis == "@check" || vis == "\xe2\x9c\x93") {
        ImVec2 center(pos.x + w * 0.5f, pos.y + height * 0.5f);
        DrawCheckIcon(dl, center, 6.0f * S, fg, std::max(1.2f, 1.8f * S));
    } else if (vis == "@close" || vis == "\xe2\x9c\x95" || vis == "✕") {
        ImVec2 center(pos.x + w * 0.5f, pos.y + height * 0.5f);
        DrawCloseIcon(dl, center, 4.5f * S, fg, std::max(1.2f, 1.6f * S));
    } else {
        ImVec2 tSz = ImGui::CalcTextSize(vis.c_str());
        dl->PushClipRect(ImVec2(bMin.x + 4.0f * S, bMin.y + 1.0f * S), ImVec2(bMax.x - 4.0f * S, bMax.y - 1.0f * S), true);
        float tx = pos.x + (std::max)(6.0f * S, (w - tSz.x) * 0.5f);
        dl->AddText(ImVec2(tx, pos.y + (height - tSz.y) * 0.5f), fg, vis.c_str());
        dl->PopClipRect();
    }

    return clicked;
}

void UrushiUI::StatRow(const std::vector<StatItem>& items) {
    if (items.empty()) return;
    const float S = g_uiScale;
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
    const float S = g_uiScale;
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

static bool ColorEditInternal(const std::string& label, float col[4], bool hasAlpha) {
    const float S = UrushiUI::GetUiScale();
    const float w = UrushiUI::CurrentContentWidth();
    const float rowH = 26.0f * S;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const char* hashPos = strstr(label.c_str(), "##");
    std::string vis = hashPos ? label.substr(0, hashPos - label.c_str()) : label;

    const float cr = std::clamp(col[0], 0.0f, 1.0f);
    const float cg = std::clamp(col[1], 0.0f, 1.0f);
    const float cb = std::clamp(col[2], 0.0f, 1.0f);
    const float ca = hasAlpha ? std::clamp(col[3], 0.0f, 1.0f) : 1.0f;

    const int ri = static_cast<int>(std::round(cr * 255.0f));
    const int gi = static_cast<int>(std::round(cg * 255.0f));
    const int bi = static_cast<int>(std::round(cb * 255.0f));
    const int ai = static_cast<int>(std::round(ca * 255.0f));

    char hexBuf[16];
    if (hasAlpha) snprintf(hexBuf, sizeof(hexBuf), "#%02X%02X%02X%02X", ri, gi, bi, ai);
    else snprintf(hexBuf, sizeof(hexBuf), "#%02X%02X%02X", ri, gi, bi);

    if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
    ImVec2 hexSz = ImGui::CalcTextSize(hexBuf);
    if (AppFonts::Mono) ImGui::PopFont();

    const float swW = 44.0f * S;
    const float swH = 20.0f * S;
    const float swGap = 8.0f * S;
    ImVec2 swMin(pos.x + w - swW, pos.y + (rowH - swH) * 0.5f);
    ImVec2 swMax(swMin.x + swW, swMin.y + swH);
    const float hexX = swMin.x - swGap - hexSz.x;

    ImGui::PushID(label.c_str());
    bool clicked = ImGui::InvisibleButton("##clr_btn", ImVec2(w, rowH));
    bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        ImGui::OpenPopup("##clr_pop");
    }

    dl->PushClipRect(pos, ImVec2(std::max(pos.x + 8.0f * S, hexX - 8.0f * S), pos.y + rowH), true);
    ImVec2 lblSz = ImGui::CalcTextSize(vis.c_str());
    dl->AddText(ImVec2(pos.x, pos.y + (rowH - lblSz.y) * 0.5f),
                hovered ? IM_COL32(255, 255, 255, 255) : kColTextPrimary, vis.c_str());
    dl->PopClipRect();

    if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
    dl->AddText(ImVec2(hexX, pos.y + (rowH - hexSz.y) * 0.5f), kColTextSecondary, hexBuf);
    if (AppFonts::Mono) ImGui::PopFont();

    const float swR = 5.0f * S;
    if (hasAlpha && ai < 255) {
        dl->AddRectFilled(swMin, swMax, IM_COL32(42, 42, 50, 255), swR);
        dl->AddRectFilled(swMin, ImVec2(swMin.x + swW * 0.5f, swMax.y), IM_COL32(ri, gi, bi, 255), swR, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(swMin.x + swW * 0.5f, swMin.y), swMax, IM_COL32(ri, gi, bi, ai), swR, ImDrawFlags_RoundCornersRight);
    } else {
        dl->AddRectFilled(swMin, swMax, IM_COL32(ri, gi, bi, 255), swR);
    }
    dl->AddRect(swMin, swMax, hovered ? UrushiUI::GetAccentU32() : kColHairlineStrong, swR, 0, 1.0f);

    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * S, 12.0f * S));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, U32ToVec4(kColBgDock));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColHairlineStrong));
    if (ImGui::BeginPopup("##clr_pop")) {
        ImGui::PushItemWidth(210.0f * S);
        ImGuiColorEditFlags flags =
            ImGuiColorEditFlags_PickerHueBar |
            ImGuiColorEditFlags_NoSidePreview |
            ImGuiColorEditFlags_NoLabel |
            ImGuiColorEditFlags_NoOptions |
            ImGuiColorEditFlags_DisplayRGB |
            ImGuiColorEditFlags_DisplayHex;
        if (hasAlpha) {
            flags |= ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
        } else {
            flags |= ImGuiColorEditFlags_NoAlpha;
        }
        if (col[0] > 1.0f || col[1] > 1.0f || col[2] > 1.0f) {
            flags |= ImGuiColorEditFlags_HDR;
        }
        if (ImGui::ColorPicker4("##pick", col, flags)) {
            changed = true;
        }
        ImGui::PopItemWidth();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
    ImGui::PopID();

    return changed;
}

bool UrushiUI::ColorEdit3(const std::string& label, float rgb[3]) {
    if (!rgb) return false;
    float tmp[4] = { rgb[0], rgb[1], rgb[2], 1.0f };
    if (ColorEditInternal(label, tmp, false)) {
        rgb[0] = tmp[0];
        rgb[1] = tmp[1];
        rgb[2] = tmp[2];
        return true;
    }
    return false;
}

bool UrushiUI::ColorEdit4(const std::string& label, float rgba[4]) {
    if (!rgba) return false;
    return ColorEditInternal(label, rgba, true);
}

void UrushiUI::ProgressBar(float fraction, float width, float height, const char* overlay) {
    const float S = g_uiScale;
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
    const float S = g_uiScale;
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

static std::string s_activeListeningKeybindId;

bool UrushiUI::Keybind(const std::string& id, const std::string& label, KeybindState& state) {
    const float S = g_uiScale;
    const float w = CurrentContentWidth();
    const float rowH = 30.0f * S;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool isListening = (s_activeListeningKeybindId == id);

    // Format display text
    std::string keyText;
    if (isListening) {
        keyText = (g_language == Language::English) ? "Press any key..." : "Нажмите кнопку...";
    } else {
        if (state.source == "none" || state.code == 0) {
            keyText = (g_language == Language::English) ? "None" : "Не назначен";
        } else {
            keyText = state.name.empty() ? "Bound" : state.name;
        }
    }

    if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
    ImVec2 tSz = ImGui::CalcTextSize(keyText.c_str());
    if (AppFonts::Mono) ImGui::PopFont();

    const float btnPadX = 12.0f * S;
    const float minBtnW = 84.0f * S;
    const float btnW = (std::max)(minBtnW, tSz.x + btnPadX * 2.0f);
    const float btnH = 24.0f * S;
    const float btnR = 6.0f * S;

    const ImVec2 btnMin(pos.x + w - btnW, pos.y + (rowH - btnH) * 0.5f);
    const ImVec2 btnMax(btnMin.x + btnW, btnMin.y + btnH);

    // Label text on left
    const char* hashPos = strstr(label.c_str(), "##");
    std::string visLabel = hashPos ? label.substr(0, hashPos - label.c_str()) : label;

    dl->PushClipRect(pos, ImVec2(btnMin.x - 8.0f * S, pos.y + rowH), true);
    ImVec2 lblSz = ImGui::CalcTextSize(visLabel.c_str());
    dl->AddText(ImVec2(pos.x, pos.y + (rowH - lblSz.y) * 0.5f), kColTextPrimary, visLabel.c_str());
    dl->PopClipRect();

    // Interactive button for keybind
    ImGui::PushID(id.c_str());
    ImGui::SetCursorScreenPos(btnMin);
    bool clicked = ImGui::InvisibleButton("##kb_btn", ImVec2(btnW, btnH));
    bool hovered = ImGui::IsItemHovered();
    bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    bool changed = false;

    if (isListening) {
        // Subtle pulse for listening border
        float time = static_cast<float>(ImGui::GetTime());
        float pulse = 0.5f + 0.5f * sinf(time * 6.0f);
        ImU32 listeningBorder = ScaleAlpha(GetAccentU32(), 0.6f + 0.4f * pulse);
        ImU32 listeningBg = ScaleAlpha(GetAccentU32(), 0.16f + 0.08f * pulse);

        dl->AddRectFilled(btnMin, btnMax, listeningBg, btnR);
        dl->AddRect(btnMin, btnMax, listeningBorder, btnR, 0, 1.5f * S);

        if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
        dl->AddText(ImVec2(btnMin.x + (btnW - tSz.x) * 0.5f, btnMin.y + (btnH - tSz.y) * 0.5f),
                    GetAccentFgU32(), keyText.c_str());
        if (AppFonts::Mono) ImGui::PopFont();

        // Check for captured input
        InputBinding captured;
        bool cancelled = false;
        bool cleared = false;
        if (InputManager::PollAnyPressed(captured, cancelled, cleared)) {
            if (cancelled) {
                s_activeListeningKeybindId.clear();
            } else if (cleared) {
                state.source = "none";
                state.code = 0;
                state.modCode = 0;
                state.name = (g_language == Language::English) ? "None" : "Не назначен";
                s_activeListeningKeybindId.clear();
                changed = true;
            } else if (captured.IsValid()) {
                state.source = (captured.source == InputSource::Gamepad) ? "gamepad" : "keyboard";
                state.code = captured.code;
                state.modCode = captured.modCode;
                state.name = captured.name;
                s_activeListeningKeybindId.clear();
                changed = true;
            }
        } else if (clicked) {
            // Click again cancels listening
            s_activeListeningKeybindId.clear();
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered) {
            // Click outside cancels listening
            s_activeListeningKeybindId.clear();
        }
    } else {
        // Normal state
        if (rightClicked) {
            // Right-click clears binding
            state.source = "none";
            state.code = 0;
            state.modCode = 0;
            state.name = (g_language == Language::English) ? "None" : "Не назначен";
            changed = true;
        } else if (clicked) {
            s_activeListeningKeybindId = id;
        }

        ImU32 bg = hovered ? kColBgRaisedHover : kColBgControl;
        ImU32 border = hovered ? GetAccentU32(0.8f) : kColHairlineStrong;
        ImU32 txtCol = (state.source == "none" || state.code == 0) ? kColTextMuted : kColTextPrimary;

        dl->AddRectFilled(btnMin, btnMax, bg, btnR);
        dl->AddRect(btnMin, btnMax, border, btnR, 0, 1.0f);

        if (AppFonts::Mono) ImGui::PushFont(AppFonts::Mono);
        dl->AddText(ImVec2(btnMin.x + (btnW - tSz.x) * 0.5f, btnMin.y + (btnH - tSz.y) * 0.5f),
                    txtCol, keyText.c_str());
        if (AppFonts::Mono) ImGui::PopFont();
    }

    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowH));
    return changed;
}

void UrushiUI::Keycap(const std::string& keyText) {
    const float S = g_uiScale;
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
    const float S = g_uiScale;
    float w = CurrentContentWidth();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(pos, ImVec2(pos.x + w, pos.y), kColHairline);
    ImGui::Dummy(ImVec2(w, 2.0f * S));
}

void UrushiUI::Spacing(float px) {
    ImGui::Dummy(ImVec2(0.0f, px));
}
