#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

class LuaImGui {
public:
    static void Register(lua_State* L);

    static int BeginWindow(lua_State* L);
    static int EndWindow(lua_State* L);
    static int SetNextWindowSize(lua_State* L);
    static int BeginChild(lua_State* L);
    static int EndChild(lua_State* L);
    static int Text(lua_State* L);
    static int TextColored(lua_State* L);
    static int TextWrapped(lua_State* L);
    static int Button(lua_State* L);
    static int Checkbox(lua_State* L);
    static int SliderFloat(lua_State* L);
    static int SliderInt(lua_State* L);
    static int InputText(lua_State* L);
    static int Combo(lua_State* L);
    static int SameLine(lua_State* L);
    static int Separator(lua_State* L);
    static int Spacing(lua_State* L);
    static int ProgressBar(lua_State* L);
    static int BeginTabBar(lua_State* L);
    static int EndTabBar(lua_State* L);
    static int BeginTabItem(lua_State* L);
    static int EndTabItem(lua_State* L);
    static int CollapsingHeader(lua_State* L);
    static int IsMenuOpen(lua_State* L);
    static int SetMenuOpen(lua_State* L);
    static int ToggleMenu(lua_State* L);
    static int IsAnyItemActive(lua_State* L);

    static int PushFont(lua_State* L);
    static int PopFont(lua_State* L);

    // Declarative schema UI
    static int RegisterPanel(lua_State* L);
    static int SetValue(lua_State* L);
    static int GetValue(lua_State* L);
    static int SetOptions(lua_State* L);
};
