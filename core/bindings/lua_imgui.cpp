#include "lua_imgui.h"
#include "../d3d11_hook.h"
#include "../ui_schema.h"
#include <imgui.h>
#include <vector>
#include <string>

int LuaImGui::BeginWindow(lua_State* L) {
    const char* title = luaL_checkstring(L, 1);
    bool hasOpen = (lua_gettop(L) >= 2 && !lua_isnil(L, 2));
    bool open = hasOpen ? (lua_toboolean(L, 2) != 0) : true;

    bool result = ImGui::Begin(title, hasOpen ? &open : nullptr);
    lua_pushboolean(L, result);
    lua_pushboolean(L, open);
    return 2;
}

int LuaImGui::EndWindow(lua_State* L) {
    ImGui::End();
    return 0;
}

int LuaImGui::SetNextWindowSize(lua_State* L) {
    float w = static_cast<float>(luaL_checknumber(L, 1));
    float h = static_cast<float>(luaL_checknumber(L, 2));
    int cond = static_cast<int>(luaL_optinteger(L, 3, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);
    return 0;
}

int LuaImGui::BeginChild(lua_State* L) {
    const char* str_id = luaL_checkstring(L, 1);
    float w = static_cast<float>(luaL_optnumber(L, 2, 0.0f));
    float h = static_cast<float>(luaL_optnumber(L, 3, 0.0f));
    bool border = lua_toboolean(L, 4) != 0;

    bool result = ImGui::BeginChild(str_id, ImVec2(w, h), border);
    lua_pushboolean(L, result);
    return 1;
}

int LuaImGui::EndChild(lua_State* L) {
    ImGui::EndChild();
    return 0;
}

int LuaImGui::Text(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    ImGui::TextUnformatted(text);
    return 0;
}

int LuaImGui::TextColored(lua_State* L) {
    float r = static_cast<float>(luaL_checknumber(L, 1));
    float g = static_cast<float>(luaL_checknumber(L, 2));
    float b = static_cast<float>(luaL_checknumber(L, 3));
    float a = static_cast<float>(luaL_optnumber(L, 4, 1.0f));
    const char* text = luaL_checkstring(L, 5);

    ImGui::TextColored(ImVec4(r, g, b, a), "%s", text);
    return 0;
}

int LuaImGui::TextWrapped(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    ImGui::TextWrapped("%s", text);
    return 0;
}

int LuaImGui::Button(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float w = static_cast<float>(luaL_optnumber(L, 2, 0.0f));
    float h = static_cast<float>(luaL_optnumber(L, 3, 0.0f));

    bool clicked = ImGui::Button(label, ImVec2(w, h));
    lua_pushboolean(L, clicked);
    return 1;
}

int LuaImGui::Checkbox(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool val = lua_toboolean(L, 2) != 0;

    bool changed = ImGui::Checkbox(label, &val);
    lua_pushboolean(L, changed);
    lua_pushboolean(L, val);
    return 2;
}

int LuaImGui::SliderFloat(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float val = static_cast<float>(luaL_checknumber(L, 2));
    float min = static_cast<float>(luaL_checknumber(L, 3));
    float max = static_cast<float>(luaL_checknumber(L, 4));

    bool changed = ImGui::SliderFloat(label, &val, min, max, "%.2f");
    lua_pushboolean(L, changed);
    lua_pushnumber(L, val);
    return 2;
}

int LuaImGui::SliderInt(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int val = static_cast<int>(luaL_checkinteger(L, 2));
    int min = static_cast<int>(luaL_checkinteger(L, 3));
    int max = static_cast<int>(luaL_checkinteger(L, 4));

    bool changed = ImGui::SliderInt(label, &val, min, max);
    lua_pushboolean(L, changed);
    lua_pushinteger(L, val);
    return 2;
}

int LuaImGui::InputText(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    const char* curVal = luaL_optstring(L, 2, "");

    char buffer[256];
    strncpy_s(buffer, curVal, sizeof(buffer) - 1);

    bool changed = ImGui::InputText(label, buffer, sizeof(buffer));
    lua_pushboolean(L, changed);
    lua_pushstring(L, buffer);
    return 2;
}

int LuaImGui::Combo(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int current_item = static_cast<int>(luaL_checkinteger(L, 2)) - 1; // 1-based to 0-based

    if (!lua_istable(L, 3)) {
        lua_pushboolean(L, false);
        lua_pushinteger(L, current_item + 1);
        return 2;
    }

    std::vector<std::string> items;
    int count = static_cast<int>(lua_rawlen(L, 3));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 3, i);
        if (lua_isstring(L, -1)) {
            items.push_back(lua_tostring(L, -1));
        } else {
            items.push_back("");
        }
        lua_pop(L, 1);
    }

    std::vector<const char*> item_ptrs;
    for (const auto& item : items) {
        item_ptrs.push_back(item.c_str());
    }

    bool changed = false;
    if (!item_ptrs.empty()) {
        changed = ImGui::Combo(label, &current_item, item_ptrs.data(), static_cast<int>(item_ptrs.size()));
    }

    lua_pushboolean(L, changed);
    lua_pushinteger(L, current_item + 1); // 0-based to 1-based
    return 2;
}

int LuaImGui::SameLine(lua_State* L) {
    float offset_from_start_x = static_cast<float>(luaL_optnumber(L, 1, 0.0f));
    float spacing = static_cast<float>(luaL_optnumber(L, 2, -1.0f));
    ImGui::SameLine(offset_from_start_x, spacing);
    return 0;
}

int LuaImGui::Separator(lua_State* L) {
    ImGui::Separator();
    return 0;
}

int LuaImGui::Spacing(lua_State* L) {
    ImGui::Spacing();
    return 0;
}

int LuaImGui::ProgressBar(lua_State* L) {
    float fraction = static_cast<float>(luaL_checknumber(L, 1));
    float w = static_cast<float>(luaL_optnumber(L, 2, -1.0f));
    float h = static_cast<float>(luaL_optnumber(L, 3, 0.0f));
    const char* overlay = luaL_optstring(L, 4, nullptr);

    ImGui::ProgressBar(fraction, ImVec2(w, h), overlay);
    return 0;
}

int LuaImGui::BeginTabBar(lua_State* L) {
    const char* str_id = luaL_checkstring(L, 1);
    bool result = ImGui::BeginTabBar(str_id);
    lua_pushboolean(L, result);
    return 1;
}

int LuaImGui::EndTabBar(lua_State* L) {
    ImGui::EndTabBar();
    return 0;
}

int LuaImGui::BeginTabItem(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool result = ImGui::BeginTabItem(label);
    lua_pushboolean(L, result);
    return 1;
}

int LuaImGui::EndTabItem(lua_State* L) {
    ImGui::EndTabItem();
    return 0;
}

int LuaImGui::CollapsingHeader(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool result = ImGui::CollapsingHeader(label);
    lua_pushboolean(L, result);
    return 1;
}

int LuaImGui::IsMenuOpen(lua_State* L) {
    lua_pushboolean(L, D3D11Hook::IsMenuOpen());
    return 1;
}

int LuaImGui::SetMenuOpen(lua_State* L) {
    bool open = lua_toboolean(L, 1) != 0;
    D3D11Hook::SetMenuOpen(open);
    return 0;
}

int LuaImGui::ToggleMenu(lua_State* L) {
    D3D11Hook::ToggleMenu();
    lua_pushboolean(L, D3D11Hook::IsMenuOpen());
    return 1;
}

int LuaImGui::IsAnyItemActive(lua_State* L) {
    lua_pushboolean(L, ImGui::IsAnyItemActive());
    return 1;
}

int LuaImGui::PushFont(lua_State* L) {
    const char* fontName = luaL_checkstring(L, 1);
    ImFont* f = AppFonts::Regular;
    if (_stricmp(fontName, "large") == 0) f = AppFonts::Large;
    else if (_stricmp(fontName, "small") == 0) f = AppFonts::Small;
    else if (_stricmp(fontName, "huge") == 0) f = AppFonts::Huge;
    else if (_stricmp(fontName, "mono") == 0) f = AppFonts::Mono;
    if (f) ImGui::PushFont(f);
    return 0;
}

int LuaImGui::PopFont(lua_State* L) {
    ImGui::PopFont();
    return 0;
}

int LuaImGui::RegisterPanel(lua_State* L) {
    bool ok = UiSchema::RegisterPanelFromLua(L, 1);
    lua_pushboolean(L, ok);
    return 1;
}

int LuaImGui::SetValue(lua_State* L) {
    const char* panelId = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (lua_isboolean(L, 3)) {
        UiSchema::SetBool(panelId, key, lua_toboolean(L, 3) != 0);
    } else if (lua_isinteger(L, 3)) {
        UiSchema::SetInt(panelId, key, static_cast<int>(lua_tointeger(L, 3)));
    } else if (lua_isnumber(L, 3)) {
        UiSchema::SetFloat(panelId, key, static_cast<float>(lua_tonumber(L, 3)));
    } else if (lua_isstring(L, 3)) {
        UiSchema::SetString(panelId, key, lua_tostring(L, 3));
    }
    return 0;
}

int LuaImGui::GetValue(lua_State* L) {
    const char* panelId = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);
    ScriptPanel* p = UiSchema::FindPanel(panelId);
    if (!p) {
        lua_pushnil(L);
        return 1;
    }
    if (p->floatValues.find(key) != p->floatValues.end()) {
        lua_pushnumber(L, p->floatValues[key]);
    } else if (p->intValues.find(key) != p->intValues.end()) {
        lua_pushinteger(L, p->intValues[key]);
    } else if (p->boolValues.find(key) != p->boolValues.end()) {
        lua_pushboolean(L, p->boolValues[key] ? 1 : 0);
    } else if (p->stringValues.find(key) != p->stringValues.end()) {
        lua_pushstring(L, p->stringValues[key].c_str());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int LuaImGui::SetOptions(lua_State* L) {
    const char* panelId = luaL_checkstring(L, 1);
    const char* nodeId = luaL_checkstring(L, 2);
    if (!lua_istable(L, 3)) return 0;

    std::vector<std::string> opts;
    int count = static_cast<int>(lua_rawlen(L, 3));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 3, i);
        if (lua_isstring(L, -1)) {
            opts.push_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    UiSchema::SetOptions(panelId, nodeId, opts);
    return 0;
}

int LuaImGui::GetDisplaySize(lua_State* L) {
    ImVec2 sz = ImGui::GetIO().DisplaySize;
    lua_pushnumber(L, sz.x);
    lua_pushnumber(L, sz.y);
    return 2;
}

int LuaImGui::DrawLine(lua_State* L) {
    float x1 = static_cast<float>(luaL_checknumber(L, 1));
    float y1 = static_cast<float>(luaL_checknumber(L, 2));
    float x2 = static_cast<float>(luaL_checknumber(L, 3));
    float y2 = static_cast<float>(luaL_checknumber(L, 4));
    float r = static_cast<float>(luaL_checknumber(L, 5));
    float g = static_cast<float>(luaL_checknumber(L, 6));
    float b = static_cast<float>(luaL_checknumber(L, 7));
    float a = static_cast<float>(luaL_optnumber(L, 8, 1.0f));
    float thickness = static_cast<float>(luaL_optnumber(L, 9, 1.0f));

    ImU32 col = ImColor(r, g, b, a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl) dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, thickness);
    return 0;
}

int LuaImGui::DrawRect(lua_State* L) {
    float x1 = static_cast<float>(luaL_checknumber(L, 1));
    float y1 = static_cast<float>(luaL_checknumber(L, 2));
    float x2 = static_cast<float>(luaL_checknumber(L, 3));
    float y2 = static_cast<float>(luaL_checknumber(L, 4));
    float r = static_cast<float>(luaL_checknumber(L, 5));
    float g = static_cast<float>(luaL_checknumber(L, 6));
    float b = static_cast<float>(luaL_checknumber(L, 7));
    float a = static_cast<float>(luaL_optnumber(L, 8, 1.0f));
    float rounding = static_cast<float>(luaL_optnumber(L, 9, 0.0f));
    float thickness = static_cast<float>(luaL_optnumber(L, 10, 1.0f));

    ImU32 col = ImColor(r, g, b, a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl) dl->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), col, rounding, 0, thickness);
    return 0;
}

int LuaImGui::DrawRectFilled(lua_State* L) {
    float x1 = static_cast<float>(luaL_checknumber(L, 1));
    float y1 = static_cast<float>(luaL_checknumber(L, 2));
    float x2 = static_cast<float>(luaL_checknumber(L, 3));
    float y2 = static_cast<float>(luaL_checknumber(L, 4));
    float r = static_cast<float>(luaL_checknumber(L, 5));
    float g = static_cast<float>(luaL_checknumber(L, 6));
    float b = static_cast<float>(luaL_checknumber(L, 7));
    float a = static_cast<float>(luaL_optnumber(L, 8, 1.0f));
    float rounding = static_cast<float>(luaL_optnumber(L, 9, 0.0f));

    ImU32 col = ImColor(r, g, b, a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl) dl->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), col, rounding);
    return 0;
}

int LuaImGui::DrawCircle(lua_State* L) {
    float cx = static_cast<float>(luaL_checknumber(L, 1));
    float cy = static_cast<float>(luaL_checknumber(L, 2));
    float radius = static_cast<float>(luaL_checknumber(L, 3));
    float r = static_cast<float>(luaL_checknumber(L, 4));
    float g = static_cast<float>(luaL_checknumber(L, 5));
    float b = static_cast<float>(luaL_checknumber(L, 6));
    float a = static_cast<float>(luaL_optnumber(L, 7, 1.0f));
    int segs = static_cast<int>(luaL_optinteger(L, 8, 0));
    float thickness = static_cast<float>(luaL_optnumber(L, 9, 1.0f));

    ImU32 col = ImColor(r, g, b, a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl) dl->AddCircle(ImVec2(cx, cy), radius, col, segs, thickness);
    return 0;
}

int LuaImGui::DrawCircleFilled(lua_State* L) {
    float cx = static_cast<float>(luaL_checknumber(L, 1));
    float cy = static_cast<float>(luaL_checknumber(L, 2));
    float radius = static_cast<float>(luaL_checknumber(L, 3));
    float r = static_cast<float>(luaL_checknumber(L, 4));
    float g = static_cast<float>(luaL_checknumber(L, 5));
    float b = static_cast<float>(luaL_checknumber(L, 6));
    float a = static_cast<float>(luaL_optnumber(L, 7, 1.0f));
    int segs = static_cast<int>(luaL_optinteger(L, 8, 0));

    ImU32 col = ImColor(r, g, b, a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl) dl->AddCircleFilled(ImVec2(cx, cy), radius, col, segs);
    return 0;
}

int LuaImGui::DrawText2D(lua_State* L) {
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    const char* text = luaL_checkstring(L, 3);
    float r = static_cast<float>(luaL_optnumber(L, 4, 1.0f));
    float g = static_cast<float>(luaL_optnumber(L, 5, 1.0f));
    float b = static_cast<float>(luaL_optnumber(L, 6, 1.0f));
    float a = static_cast<float>(luaL_optnumber(L, 7, 1.0f));
    float fontSize = static_cast<float>(luaL_optnumber(L, 8, 0.0f));

    ImU32 col = ImColor(r, g, b, a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (dl) {
        if (fontSize > 1.0f) {
            dl->AddText(ImGui::GetFont(), fontSize, ImVec2(x, y), col, text);
        } else {
            dl->AddText(ImVec2(x, y), col, text);
        }
    }
    return 0;
}

static const luaL_Reg imgui_funcs[] = {
    { "begin_window", LuaImGui::BeginWindow },
    { "end_window", LuaImGui::EndWindow },
    { "set_next_window_size", LuaImGui::SetNextWindowSize },
    { "begin_child", LuaImGui::BeginChild },
    { "end_child", LuaImGui::EndChild },
    { "text", LuaImGui::Text },
    { "text_colored", LuaImGui::TextColored },
    { "text_wrapped", LuaImGui::TextWrapped },
    { "button", LuaImGui::Button },
    { "checkbox", LuaImGui::Checkbox },
    { "slider_float", LuaImGui::SliderFloat },
    { "slider_int", LuaImGui::SliderInt },
    { "input_text", LuaImGui::InputText },
    { "combo", LuaImGui::Combo },
    { "same_line", LuaImGui::SameLine },
    { "separator", LuaImGui::Separator },
    { "spacing", LuaImGui::Spacing },
    { "progress_bar", LuaImGui::ProgressBar },
    { "begin_tab_bar", LuaImGui::BeginTabBar },
    { "end_tab_bar", LuaImGui::EndTabBar },
    { "begin_tab_item", LuaImGui::BeginTabItem },
    { "end_tab_item", LuaImGui::EndTabItem },
    { "collapsing_header", LuaImGui::CollapsingHeader },
    { "is_menu_open", LuaImGui::IsMenuOpen },
    { "set_menu_open", LuaImGui::SetMenuOpen },
    { "toggle_menu", LuaImGui::ToggleMenu },
    { "is_any_item_active", LuaImGui::IsAnyItemActive },
    { "push_font", LuaImGui::PushFont },
    { "pop_font", LuaImGui::PopFont },
    { "register_panel", LuaImGui::RegisterPanel },
    { "set_value", LuaImGui::SetValue },
    { "get_value", LuaImGui::GetValue },
    { "set_options", LuaImGui::SetOptions },
    { "get_display_size", LuaImGui::GetDisplaySize },
    { "draw_line", LuaImGui::DrawLine },
    { "draw_rect", LuaImGui::DrawRect },
    { "draw_rect_filled", LuaImGui::DrawRectFilled },
    { "draw_circle", LuaImGui::DrawCircle },
    { "draw_circle_filled", LuaImGui::DrawCircleFilled },
    { "draw_text", LuaImGui::DrawText2D },
    { nullptr, nullptr }
};

static const luaL_Reg gui_funcs[] = {
    { "register_panel", LuaImGui::RegisterPanel },
    { "set_value", LuaImGui::SetValue },
    { "get_value", LuaImGui::GetValue },
    { "set_options", LuaImGui::SetOptions },
    { "toggle_menu", LuaImGui::ToggleMenu },
    { "is_menu_open", LuaImGui::IsMenuOpen },
    { "set_menu_open", LuaImGui::SetMenuOpen },
    { nullptr, nullptr }
};

void LuaImGui::Register(lua_State* L) {
    luaL_newlib(L, imgui_funcs);
    lua_setglobal(L, "imgui");

    luaL_newlib(L, gui_funcs);
    lua_setglobal(L, "gui");
}
