#include "lua_urushi.h"
#include "../urushi_ui.h"
#include "../d3d11_hook.h"
#include "../native_carx.h"
#include "../car_sim_optimizer.h"
#include "../logger.h"

#include <imgui.h>
#include <string>
#include <vector>

namespace {

UrushiUI::Tone ParseTone(const char* s) {
    if (!s) return UrushiUI::Tone::Default;
    if (_stricmp(s, "muted") == 0 || _stricmp(s, "dim") == 0) return UrushiUI::Tone::Muted;
    if (_stricmp(s, "accent") == 0) return UrushiUI::Tone::Accent;
    if (_stricmp(s, "green") == 0 || _stricmp(s, "ok") == 0) return UrushiUI::Tone::Green;
    if (_stricmp(s, "red") == 0 || _stricmp(s, "error") == 0 || _stricmp(s, "danger") == 0) return UrushiUI::Tone::Red;
    if (_stricmp(s, "yellow") == 0 || _stricmp(s, "warn") == 0) return UrushiUI::Tone::Yellow;
    return UrushiUI::Tone::Default;
}

UrushiUI::ButtonStyle ParseButtonStyle(const char* s) {
    if (!s) return UrushiUI::ButtonStyle::Default;
    if (_stricmp(s, "primary") == 0 || _stricmp(s, "accent") == 0) return UrushiUI::ButtonStyle::Primary;
    if (_stricmp(s, "danger") == 0 || _stricmp(s, "red") == 0) return UrushiUI::ButtonStyle::Danger;
    if (_stricmp(s, "ghost") == 0) return UrushiUI::ButtonStyle::Ghost;
    return UrushiUI::ButtonStyle::Default;
}

// ==================== urushi.* module ====================

int Lua_UrushiRegisterWindow(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    UrushiUI::WindowRegistration reg;
    lua_getfield(L, 1, "id");
    if (lua_isstring(L, -1)) reg.id = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (reg.id.empty()) {
        Logger::Warn("urushi.register_window: missing 'id'");
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_getfield(L, 1, "title");
    reg.title = lua_isstring(L, -1) ? lua_tostring(L, -1) : reg.id;
    lua_pop(L, 1);

    lua_getfield(L, 1, "icon");
    if (lua_isstring(L, -1)) reg.icon = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "order");
    if (lua_isnumber(L, -1)) reg.order = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "default_open");
    if (lua_isboolean(L, -1)) reg.defaultOpen = (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "default_pinned");
    if (lua_isboolean(L, -1)) reg.defaultPinned = (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "hud_no_inputs");
    if (lua_isboolean(L, -1)) reg.hudNoInputs = (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "anchor_right");
    if (lua_isboolean(L, -1)) reg.anchorRight = (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "x");
    if (lua_isnumber(L, -1)) reg.defaultX = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "default_x");
    if (lua_isnumber(L, -1)) reg.defaultX = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "y");
    if (lua_isnumber(L, -1)) reg.defaultY = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "default_y");
    if (lua_isnumber(L, -1)) reg.defaultY = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "width");
    if (lua_isnumber(L, -1)) reg.width = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "render");
    if (lua_isfunction(L, -1)) {
        reg.luaRenderRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }

    UrushiUI::RegisterWindow(reg);
    lua_pushboolean(L, 1);
    return 1;
}

int Lua_UrushiSetWindowOpen(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    bool open = lua_toboolean(L, 2) != 0;
    UrushiUI::SetWindowOpen(id, open);
    return 0;
}

int Lua_UrushiToggleWindow(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    lua_pushboolean(L, UrushiUI::ToggleWindow(id) ? 1 : 0);
    return 1;
}

int Lua_UrushiIsWindowOpen(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    lua_pushboolean(L, UrushiUI::IsWindowOpen(id) ? 1 : 0);
    return 1;
}

int Lua_UrushiCloseAll(lua_State* /*L*/) {
    UrushiUI::CloseAllUnpinned();
    return 0;
}

int Lua_UrushiSetAccent(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, UrushiUI::SetAccentByName(name) ? 1 : 0);
    return 1;
}

int Lua_UrushiGetAccent(lua_State* L) {
    lua_pushstring(L, UrushiUI::GetAccentName().c_str());
    return 1;
}

int Lua_UrushiSetCustomAccent(lua_State* L) {
    float r = static_cast<float>(luaL_checknumber(L, 1));
    float g = static_cast<float>(luaL_checknumber(L, 2));
    float b = static_cast<float>(luaL_checknumber(L, 3));
    UrushiUI::SetCustomAccent(r, g, b);
    return 0;
}

int Lua_UrushiGetCustomAccent(lua_State* L) {
    float r = 0.3f, g = 0.78f, b = 0.96f;
    UrushiUI::GetCustomAccent(r, g, b);
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    return 3;
}

int Lua_UrushiSetLanguage(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, UrushiUI::SetLanguageByName(name) ? 1 : 0);
    return 1;
}

int Lua_UrushiGetLanguage(lua_State* L) {
    lua_pushstring(L, UrushiUI::GetLanguageName().c_str());
    return 1;
}

int Lua_UrushiSetWindowTitle(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    const char* title = luaL_checkstring(L, 2);
    UrushiUI::SetWindowTitle(id, title);
    return 0;
}

int Lua_UrushiSetSingleMode(lua_State* L) {
    UrushiUI::SetSingleTabMode(lua_toboolean(L, 1) != 0);
    return 0;
}

int Lua_UrushiGetSingleMode(lua_State* L) {
    lua_pushboolean(L, UrushiUI::GetSingleTabMode() ? 1 : 0);
    return 1;
}

int Lua_UrushiIsMenuOpen(lua_State* L) {
    lua_pushboolean(L, D3D11Hook::IsMenuOpen() ? 1 : 0);
    return 1;
}

int Lua_UrushiSetUiScale(lua_State* L) {
    float sc = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    UrushiUI::SetUserScale(sc);
    return 0;
}

int Lua_UrushiGetUiScale(lua_State* L) {
    lua_pushnumber(L, UrushiUI::GetUiScale());
    return 1;
}

int Lua_UrushiGetUserScale(lua_State* L) {
    lua_pushnumber(L, UrushiUI::GetUserScale());
    return 1;
}

int Lua_UrushiGetAutoScale(lua_State* L) {
    lua_pushnumber(L, UrushiUI::GetAutoScale());
    return 1;
}

int Lua_UrushiGetEnvStats(lua_State* L) {
    float fps = 0.0f;
    float dtMs = 0.0f;
    if (ImGui::GetCurrentContext()) {
        fps = ImGui::GetIO().Framerate;
        dtMs = ImGui::GetIO().DeltaTime * 1000.0f;
    }
    const bool inSession = NativeCarX::IsInSession();
    const auto optStats = CarSimOptimizer::GetStats();

    lua_newtable(L);
    lua_pushnumber(L, fps);
    lua_setfield(L, -2, "fps");
    lua_pushnumber(L, dtMs);
    lua_setfield(L, -2, "frame_ms");
    lua_pushboolean(L, inSession ? 1 : 0);
    lua_setfield(L, -2, "in_session");
    lua_pushinteger(L, static_cast<lua_Integer>(optStats.totalNetCars));
    lua_setfield(L, -2, "total_net_cars");
    return 1;
}

// ==================== ui.* Lacquered widget module ====================

int Lua_UiCard(lua_State* L) {
    std::string title;
    std::string subtitle;
    int fnIdx = 0;

    int argc = lua_gettop(L);
    if (argc >= 1 && lua_isfunction(L, 1)) {
        fnIdx = 1;
    } else if (argc >= 2 && lua_isfunction(L, 2)) {
        if (lua_isstring(L, 1)) title = lua_tostring(L, 1);
        fnIdx = 2;
    } else if (argc >= 3 && lua_isfunction(L, 3)) {
        if (lua_isstring(L, 1)) title = lua_tostring(L, 1);
        if (lua_isstring(L, 2)) subtitle = lua_tostring(L, 2);
        fnIdx = 3;
    }

    UrushiUI::BeginCard(title, subtitle);
    if (fnIdx != 0) {
        lua_pushvalue(L, fnIdx);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            Logger::Error("ui.card callback error: " + std::string(err ? err : "unknown"));
            lua_pop(L, 1);
        }
    }
    UrushiUI::EndCard();
    return 0;
}

int Lua_UiBeginCard(lua_State* L) {
    std::string title = luaL_optstring(L, 1, "");
    std::string sub   = luaL_optstring(L, 2, "");
    UrushiUI::BeginCard(title, sub);
    return 0;
}

int Lua_UiEndCard(lua_State* /*L*/) {
    UrushiUI::EndCard();
    return 0;
}

int Lua_UiToggle(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool val = lua_toboolean(L, 2) != 0;
    bool ch = UrushiUI::Toggle(label, &val);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushboolean(L, val ? 1 : 0);
    return 2;
}

int Lua_UiSliderFloat(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float val = static_cast<float>(luaL_checknumber(L, 2));
    float minV = static_cast<float>(luaL_checknumber(L, 3));
    float maxV = static_cast<float>(luaL_checknumber(L, 4));
    const char* fmt = luaL_optstring(L, 5, "%.2f");
    bool ch = UrushiUI::SliderFloat(label, &val, minV, maxV, fmt);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushnumber(L, val);
    return 2;
}

int Lua_UiSliderInt(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int val = static_cast<int>(luaL_checkinteger(L, 2));
    int minV = static_cast<int>(luaL_checkinteger(L, 3));
    int maxV = static_cast<int>(luaL_checkinteger(L, 4));
    const char* fmt = luaL_optstring(L, 5, "%d");
    bool ch = UrushiUI::SliderInt(label, &val, minV, maxV, fmt);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushinteger(L, val);
    return 2;
}

int Lua_UiSegmented(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    int cur = static_cast<int>(luaL_checkinteger(L, 2)) - 1; // 1-based -> 0-based
    if (!lua_istable(L, 3)) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, cur + 1);
        return 2;
    }
    std::vector<std::string> opts;
    int n = static_cast<int>(lua_rawlen(L, 3));
    opts.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        opts.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    bool ch = UrushiUI::Segmented(id, &cur, opts);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushinteger(L, cur + 1);
    return 2;
}

int Lua_UiButton(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    const float sc = UrushiUI::GetUiScale();
    float w = 0.0f;
    float h = 30.0f * sc;
    UrushiUI::ButtonStyle st = UrushiUI::ButtonStyle::Default;

    const int argc = lua_gettop(L);
    if (argc >= 2 && lua_type(L, 2) == LUA_TSTRING) {
        st = ParseButtonStyle(lua_tostring(L, 2));
        if (argc >= 3 && lua_isnumber(L, 3)) h = static_cast<float>(lua_tonumber(L, 3)) * sc;
    } else {
        if (argc >= 2 && lua_isnumber(L, 2)) w = static_cast<float>(lua_tonumber(L, 2)) * sc;
        if (argc >= 3 && lua_isnumber(L, 3)) h = static_cast<float>(lua_tonumber(L, 3)) * sc;
        if (argc >= 4 && lua_type(L, 4) == LUA_TSTRING) st = ParseButtonStyle(lua_tostring(L, 4));
    }

    lua_pushboolean(L, UrushiUI::Button(label, w, h, st) ? 1 : 0);
    return 1;
}

int Lua_UiStatRow(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    std::vector<UrushiUI::StatItem> items;
    int n = static_cast<int>(lua_rawlen(L, 1));
    items.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            UrushiUI::StatItem it;
            lua_getfield(L, -1, "label");
            if (lua_isstring(L, -1)) it.label = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "value");
            if (lua_isstring(L, -1)) it.value = lua_tostring(L, -1);
            else if (lua_isinteger(L, -1)) it.value = std::to_string(lua_tointeger(L, -1));
            else if (lua_isnumber(L, -1)) it.value = std::to_string(lua_tonumber(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, -1, "tone");
            if (lua_isstring(L, -1)) it.tone = ParseTone(lua_tostring(L, -1));
            lua_pop(L, 1);

            items.push_back(std::move(it));
        }
        lua_pop(L, 1);
    }
    UrushiUI::StatRow(items);
    return 0;
}

int Lua_UiCollapsing(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool defOpen = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, UrushiUI::CollapsingSection(label, defOpen) ? 1 : 0);
    return 1;
}

int Lua_UiCombo(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int cur = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    if (!lua_istable(L, 3)) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, cur + 1);
        return 2;
    }
    std::vector<std::string> items;
    int n = static_cast<int>(lua_rawlen(L, 3));
    items.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        items.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    bool ch = UrushiUI::Combo(label, &cur, items);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushinteger(L, cur + 1);
    return 2;
}

int Lua_UiInputText(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    std::string text = luaL_optstring(L, 2, "");
    size_t maxLen = static_cast<size_t>(luaL_optinteger(L, 3, 256));
    bool ch = UrushiUI::InputText(label, text, maxLen);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushstring(L, text.c_str());
    return 2;
}

int Lua_UiColorEdit3(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float rgb[3] = {
        static_cast<float>(luaL_optnumber(L, 2, 1.0)),
        static_cast<float>(luaL_optnumber(L, 3, 1.0)),
        static_cast<float>(luaL_optnumber(L, 4, 1.0))
    };
    bool ch = UrushiUI::ColorEdit3(label, rgb);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushnumber(L, rgb[0]);
    lua_pushnumber(L, rgb[1]);
    lua_pushnumber(L, rgb[2]);
    return 4;
}

int Lua_UiColorEdit4(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float rgba[4] = {
        static_cast<float>(luaL_optnumber(L, 2, 1.0)),
        static_cast<float>(luaL_optnumber(L, 3, 1.0)),
        static_cast<float>(luaL_optnumber(L, 4, 1.0)),
        static_cast<float>(luaL_optnumber(L, 5, 1.0))
    };
    bool ch = UrushiUI::ColorEdit4(label, rgba);
    lua_pushboolean(L, ch ? 1 : 0);
    lua_pushnumber(L, rgba[0]);
    lua_pushnumber(L, rgba[1]);
    lua_pushnumber(L, rgba[2]);
    lua_pushnumber(L, rgba[3]);
    return 5;
}

int Lua_UiProgressBar(lua_State* L) {
    const float sc = UrushiUI::GetUiScale();
    float frac = static_cast<float>(luaL_checknumber(L, 1));
    float w = static_cast<float>(luaL_optnumber(L, 2, -1.0));
    if (w > 0.0f) w *= sc;
    float h = static_cast<float>(luaL_optnumber(L, 3, 8.0)) * sc;
    const char* overlay = luaL_optstring(L, 4, nullptr);
    UrushiUI::ProgressBar(frac, w, h, overlay);
    return 0;
}

int Lua_UiBadge(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    UrushiUI::Tone tone = ParseTone(luaL_optstring(L, 2, "accent"));
    UrushiUI::Badge(text, tone);
    return 0;
}

int Lua_UiKeycap(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    UrushiUI::Keycap(text);
    return 0;
}

int Lua_UiText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    UrushiUI::Tone tone = ParseTone(luaL_optstring(L, 2, nullptr));
    UrushiUI::Text(text, tone);
    return 0;
}

int Lua_UiTextDim(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    UrushiUI::Text(text, UrushiUI::Tone::Muted);
    return 0;
}

int Lua_UiTextWrapped(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    UrushiUI::Tone tone = ParseTone(luaL_optstring(L, 2, nullptr));
    UrushiUI::TextWrapped(text, tone);
    return 0;
}

int Lua_UiTextColored(lua_State* L) {
    float r = static_cast<float>(luaL_checknumber(L, 1));
    float g = static_cast<float>(luaL_checknumber(L, 2));
    float b = static_cast<float>(luaL_checknumber(L, 3));
    float a = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    const char* text = luaL_checkstring(L, 5);
    UrushiUI::TextColored(ImVec4(r, g, b, a), text);
    return 0;
}

int Lua_UiSameLine(lua_State* L) {
    const float sc = UrushiUI::GetUiScale();
    float off = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    if (off > 0.0f) off *= sc;
    float sp  = static_cast<float>(luaL_optnumber(L, 2, -1.0));
    if (sp >= 0.0f) sp *= sc;
    ImGui::SameLine(off, sp);
    return 0;
}

int Lua_UiSeparator(lua_State* /*L*/) {
    UrushiUI::Separator();
    return 0;
}

int Lua_UiSpacing(lua_State* L) {
    float px = static_cast<float>(luaL_optnumber(L, 1, 6.0)) * UrushiUI::GetUiScale();
    UrushiUI::Spacing(px);
    return 0;
}

int Lua_UiAvailWidth(lua_State* L) {
    const float sc = UrushiUI::GetUiScale();
    lua_pushnumber(L, (sc > 0.01f) ? (UrushiUI::CurrentContentWidth() / sc) : UrushiUI::CurrentContentWidth());
    return 1;
}

const luaL_Reg kUrushiFuncs[] = {
    { "register_window", Lua_UrushiRegisterWindow },
    { "set_window_open", Lua_UrushiSetWindowOpen },
    { "toggle_window",   Lua_UrushiToggleWindow },
    { "is_window_open",  Lua_UrushiIsWindowOpen },
    { "close_all",       Lua_UrushiCloseAll },
    { "set_accent",        Lua_UrushiSetAccent },
    { "get_accent",        Lua_UrushiGetAccent },
    { "set_custom_accent", Lua_UrushiSetCustomAccent },
    { "get_custom_accent", Lua_UrushiGetCustomAccent },
    { "set_language",      Lua_UrushiSetLanguage },
    { "get_language",      Lua_UrushiGetLanguage },
    { "set_window_title",  Lua_UrushiSetWindowTitle },
    { "set_single_mode", Lua_UrushiSetSingleMode },
    { "get_single_mode", Lua_UrushiGetSingleMode },
    { "is_menu_open",    Lua_UrushiIsMenuOpen },
    { "get_env_stats",   Lua_UrushiGetEnvStats },
    { "set_ui_scale",    Lua_UrushiSetUiScale },
    { "get_ui_scale",    Lua_UrushiGetUiScale },
    { "get_user_scale",  Lua_UrushiGetUserScale },
    { "get_auto_scale",  Lua_UrushiGetAutoScale },
    { nullptr, nullptr }
};

const luaL_Reg kUiFuncs[] = {
    { "card",         Lua_UiCard },
    { "begin_card",   Lua_UiBeginCard },
    { "end_card",     Lua_UiEndCard },
    { "toggle",       Lua_UiToggle },
    { "checkbox",     Lua_UiToggle },
    { "slider_float", Lua_UiSliderFloat },
    { "slider_int",   Lua_UiSliderInt },
    { "segmented",    Lua_UiSegmented },
    { "button",       Lua_UiButton },
    { "stat_row",     Lua_UiStatRow },
    { "collapsing",   Lua_UiCollapsing },
    { "combo",        Lua_UiCombo },
    { "input_text",   Lua_UiInputText },
    { "color_edit3",  Lua_UiColorEdit3 },
    { "color_edit4",  Lua_UiColorEdit4 },
    { "progress_bar", Lua_UiProgressBar },
    { "badge",        Lua_UiBadge },
    { "keycap",       Lua_UiKeycap },
    { "text",         Lua_UiText },
    { "text_dim",     Lua_UiTextDim },
    { "text_wrapped", Lua_UiTextWrapped },
    { "text_colored", Lua_UiTextColored },
    { "same_line",    Lua_UiSameLine },
    { "separator",    Lua_UiSeparator },
    { "spacing",      Lua_UiSpacing },
    { "avail_width",  Lua_UiAvailWidth },
    { nullptr, nullptr }
};

} // namespace

void LuaUrushi::Register(lua_State* L) {
    luaL_newlib(L, kUrushiFuncs);
    lua_setglobal(L, "urushi");

    luaL_newlib(L, kUiFuncs);
    lua_setglobal(L, "ui");
}
