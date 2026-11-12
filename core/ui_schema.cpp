#include "ui_schema.h"
#include "urushi_ui.h"
#include "lua_engine.h"
#include "logger.h"

#include <imgui.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <algorithm>

std::vector<ScriptPanel> UiSchema::s_panels;
std::recursive_mutex UiSchema::s_mutex;

void UiSchema::Init() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_panels.clear();
}

void UiSchema::Shutdown() {
    ClearPanels();
}

void UiSchema::ClearPanels() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    lua_State* L = LuaEngine::GetState();
    if (L) {
        for (auto& panel : s_panels) {
            if (panel.luaOnActionRef != -1) {
                luaL_unref(L, LUA_REGISTRYINDEX, panel.luaOnActionRef);
                panel.luaOnActionRef = -1;
            }
            if (panel.luaOnChangeRef != -1) {
                luaL_unref(L, LUA_REGISTRYINDEX, panel.luaOnChangeRef);
                panel.luaOnChangeRef = -1;
            }
        }
    }
    s_panels.clear();
}

ScriptPanel* UiSchema::FindPanel(const std::string& panelId) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    for (auto& p : s_panels) {
        if (p.id == panelId) return &p;
    }
    return nullptr;
}

static UiNode ParseSingleNode(lua_State* L, int idx, ScriptPanel& panel) {
    UiNode node;
    if (!lua_istable(L, idx)) return node;

    lua_getfield(L, idx, "type");
    std::string typeStr = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    if (typeStr == "card") node.type = UiNodeType::Card;
    else if (typeStr == "section") node.type = UiNodeType::Section;
    else if (typeStr == "row") node.type = UiNodeType::Row;
    else if (typeStr == "action_row") node.type = UiNodeType::ActionRow;
    else if (typeStr == "separator") node.type = UiNodeType::Separator;
    else if (typeStr == "spacing") node.type = UiNodeType::Spacing;
    else if (typeStr == "heading") node.type = UiNodeType::Heading;
    else if (typeStr == "text") node.type = UiNodeType::Text;
    else if (typeStr == "help") node.type = UiNodeType::Help;
    else if (typeStr == "slider" || typeStr == "slider_float") node.type = UiNodeType::SliderFloat;
    else if (typeStr == "slider_int") node.type = UiNodeType::SliderInt;
    else if (typeStr == "checkbox") node.type = UiNodeType::Checkbox;
    else if (typeStr == "combo") node.type = UiNodeType::Combo;
    else if (typeStr == "text_input") node.type = UiNodeType::TextInput;
    else if (typeStr == "button") node.type = UiNodeType::Button;

    lua_getfield(L, idx, "id");
    if (lua_isstring(L, -1)) node.id = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "label");
    if (lua_isstring(L, -1)) node.label = lua_tostring(L, -1);
    else if (lua_getfield(L, idx, "title"), lua_isstring(L, -1)) node.label = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "text");
    if (lua_isstring(L, -1)) node.text = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "style");
    if (lua_isstring(L, -1)) node.style = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "action");
    if (lua_isstring(L, -1)) node.action = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "min");
    if (lua_isnumber(L, -1)) node.minVal = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, idx, "max");
    if (lua_isnumber(L, -1)) node.maxVal = static_cast<float>(lua_tonumber(L, -1));
    else if (node.type == UiNodeType::SliderFloat || node.type == UiNodeType::SliderInt) node.maxVal = 1.0f;
    lua_pop(L, 1);

    // Initial default value if not already in panel caches
    lua_getfield(L, idx, "default");
    if (!node.id.empty()) {
        if (node.type == UiNodeType::SliderFloat) {
            float defVal = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
            if (panel.floatValues.find(node.id) == panel.floatValues.end()) {
                panel.floatValues[node.id] = defVal;
            }
        } else if (node.type == UiNodeType::SliderInt) {
            int defVal = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
            if (panel.intValues.find(node.id) == panel.intValues.end()) {
                panel.intValues[node.id] = defVal;
            }
        } else if (node.type == UiNodeType::Checkbox) {
            bool defVal = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) != 0) : false;
            if (panel.boolValues.find(node.id) == panel.boolValues.end()) {
                panel.boolValues[node.id] = defVal;
            }
        } else if (node.type == UiNodeType::TextInput) {
            std::string defVal = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            if (panel.stringValues.find(node.id) == panel.stringValues.end()) {
                panel.stringValues[node.id] = defVal;
            }
        }
    }
    lua_pop(L, 1);

    // Combo options
    lua_getfield(L, idx, "options");
    if (lua_istable(L, -1)) {
        int optCount = static_cast<int>(lua_rawlen(L, -1));
        for (int o = 1; o <= optCount; ++o) {
            lua_rawgeti(L, -1, o);
            if (lua_isstring(L, -1)) node.options.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // Recursive children
    lua_getfield(L, idx, "children");
    if (lua_istable(L, -1)) {
        int childCount = static_cast<int>(lua_rawlen(L, -1));
        for (int ch = 1; ch <= childCount; ++ch) {
            lua_rawgeti(L, -1, ch);
            if (lua_istable(L, -1)) {
                node.children.push_back(ParseSingleNode(L, lua_gettop(L), panel));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    return node;
}

static void ParseNodeList(lua_State* L, int tableIdx, std::vector<UiNode>& outList, ScriptPanel& panel) {
    if (!lua_istable(L, tableIdx)) return;
    int count = static_cast<int>(lua_rawlen(L, tableIdx));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, tableIdx, i);
        if (lua_istable(L, -1)) {
            outList.push_back(ParseSingleNode(L, lua_gettop(L), panel));
        }
        lua_pop(L, 1);
    }
}

bool UiSchema::RegisterPanelFromLua(lua_State* L, int tableIdx) {
    if (!lua_istable(L, tableIdx)) return false;
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    // Convert relative index to absolute index
    int absIdx = lua_absindex(L, tableIdx);

    std::string id;
    lua_getfield(L, absIdx, "id");
    if (lua_isstring(L, -1)) id = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (id.empty()) {
        Logger::Error("RegisterPanel: 'id' is required");
        return false;
    }

    ScriptPanel* existing = FindPanel(id);
    ScriptPanel panel;
    if (existing) {
        panel = *existing; // keep existing value states
        panel.rootNodes.clear();
    } else {
        panel.id = id;
    }

    lua_getfield(L, absIdx, "title");
    if (lua_isstring(L, -1)) panel.title = lua_tostring(L, -1);
    else if (panel.title.empty()) panel.title = panel.id;
    lua_pop(L, 1);

    lua_getfield(L, absIdx, "description");
    if (lua_isstring(L, -1)) panel.description = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, absIdx, "category");
    if (lua_isstring(L, -1)) panel.category = lua_tostring(L, -1);
    lua_pop(L, 1);

    // Initial values map
    lua_getfield(L, absIdx, "values");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -2)) {
                std::string k = lua_tostring(L, -2);
                if (lua_isboolean(L, -1)) {
                    panel.boolValues[k] = (lua_toboolean(L, -1) != 0);
                } else if (lua_isinteger(L, -1)) {
                    panel.intValues[k] = static_cast<int>(lua_tointeger(L, -1));
                } else if (lua_isnumber(L, -1)) {
                    panel.floatValues[k] = static_cast<float>(lua_tonumber(L, -1));
                } else if (lua_isstring(L, -1)) {
                    panel.stringValues[k] = lua_tostring(L, -1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // Callbacks
    lua_getfield(L, absIdx, "on_action");
    if (lua_isfunction(L, -1)) {
        if (panel.luaOnActionRef != -1) luaL_unref(L, LUA_REGISTRYINDEX, panel.luaOnActionRef);
        panel.luaOnActionRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }

    lua_getfield(L, absIdx, "on_change");
    if (lua_isfunction(L, -1)) {
        if (panel.luaOnChangeRef != -1) luaL_unref(L, LUA_REGISTRYINDEX, panel.luaOnChangeRef);
        panel.luaOnChangeRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }

    // Parse schema
    lua_getfield(L, absIdx, "schema");
    if (lua_istable(L, -1)) {
        ParseNodeList(L, lua_gettop(L), panel.rootNodes, panel);
    }
    lua_pop(L, 1);

    if (existing) {
        *existing = panel;
    } else {
        s_panels.push_back(panel);
    }

    Logger::Info("Registered UI panel: '" + panel.id + "' (" + panel.title + ")");
    return true;
}

void UiSchema::PushPanelValuesToLua(lua_State* L, const ScriptPanel& panel) {
    lua_newtable(L);
    for (const auto& kv : panel.floatValues) {
        lua_pushnumber(L, kv.second);
        lua_setfield(L, -2, kv.first.c_str());
    }
    for (const auto& kv : panel.intValues) {
        lua_pushinteger(L, kv.second);
        lua_setfield(L, -2, kv.first.c_str());
    }
    for (const auto& kv : panel.boolValues) {
        lua_pushboolean(L, kv.second ? 1 : 0);
        lua_setfield(L, -2, kv.first.c_str());
    }
    for (const auto& kv : panel.stringValues) {
        lua_pushstring(L, kv.second.c_str());
        lua_setfield(L, -2, kv.first.c_str());
    }
}

void UiSchema::DispatchAction(ScriptPanel& panel, const std::string& actionId) {
    if (panel.luaOnActionRef == -1) return;
    lua_State* L = LuaEngine::GetState();
    if (!L) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, panel.luaOnActionRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_pushstring(L, actionId.c_str());
    PushPanelValuesToLua(L, panel);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        Logger::Error("Lua on_action error: " + std::string(err ? err : "unknown"));
        lua_pop(L, 1);
    }
}

void UiSchema::DispatchChange(ScriptPanel& panel, const std::string& paramId) {
    if (panel.luaOnChangeRef == -1) return;
    lua_State* L = LuaEngine::GetState();
    if (!L) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, panel.luaOnChangeRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_pushstring(L, paramId.c_str());

    // Push new value
    if (panel.floatValues.find(paramId) != panel.floatValues.end()) {
        lua_pushnumber(L, panel.floatValues[paramId]);
    } else if (panel.intValues.find(paramId) != panel.intValues.end()) {
        lua_pushinteger(L, panel.intValues[paramId]);
    } else if (panel.boolValues.find(paramId) != panel.boolValues.end()) {
        lua_pushboolean(L, panel.boolValues[paramId] ? 1 : 0);
    } else if (panel.stringValues.find(paramId) != panel.stringValues.end()) {
        lua_pushstring(L, panel.stringValues[paramId].c_str());
    } else {
        lua_pushnil(L);
    }

    // Push all values
    PushPanelValuesToLua(L, panel);

    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        Logger::Error("Lua on_change error: " + std::string(err ? err : "unknown"));
        lua_pop(L, 1);
    }
}

void UiSchema::SetFloat(const std::string& panelId, const std::string& key, float val) {
    ScriptPanel* p = FindPanel(panelId);
    if (p) p->floatValues[key] = val;
}

void UiSchema::SetInt(const std::string& panelId, const std::string& key, int val) {
    ScriptPanel* p = FindPanel(panelId);
    if (p) p->intValues[key] = val;
}

void UiSchema::SetBool(const std::string& panelId, const std::string& key, bool val) {
    ScriptPanel* p = FindPanel(panelId);
    if (p) p->boolValues[key] = val;
}

void UiSchema::SetString(const std::string& panelId, const std::string& key, const std::string& val) {
    ScriptPanel* p = FindPanel(panelId);
    if (p) p->stringValues[key] = val;
}

void UiSchema::SetOptions(const std::string& panelId, const std::string& nodeId, const std::vector<std::string>& opts) {
    ScriptPanel* p = FindPanel(panelId);
    if (!p) return;
    auto updateNode = [&](auto self, UiNode& n) -> bool {
        if (n.id == nodeId) {
            n.options = opts;
            return true;
        }
        for (auto& child : n.children) {
            if (self(self, child)) return true;
        }
        return false;
    };
    for (auto& root : p->rootNodes) {
        if (updateNode(updateNode, root)) break;
    }
}

float UiSchema::GetFloat(const std::string& panelId, const std::string& key, float defaultVal) {
    ScriptPanel* p = FindPanel(panelId);
    if (p && p->floatValues.find(key) != p->floatValues.end()) return p->floatValues[key];
    return defaultVal;
}

int UiSchema::GetInt(const std::string& panelId, const std::string& key, int defaultVal) {
    ScriptPanel* p = FindPanel(panelId);
    if (p && p->intValues.find(key) != p->intValues.end()) return p->intValues[key];
    return defaultVal;
}

bool UiSchema::GetBool(const std::string& panelId, const std::string& key, bool defaultVal) {
    ScriptPanel* p = FindPanel(panelId);
    if (p && p->boolValues.find(key) != p->boolValues.end()) return p->boolValues[key];
    return defaultVal;
}

std::string UiSchema::GetString(const std::string& panelId, const std::string& key, const std::string& defaultVal) {
    ScriptPanel* p = FindPanel(panelId);
    if (p && p->stringValues.find(key) != p->stringValues.end()) return p->stringValues[key];
    return defaultVal;
}

void UiSchema::DrawUiNode(UiNode& node, ScriptPanel& panel) {
    if (!node.id.empty()) {
        ImGui::PushID(node.id.c_str());
    } else {
        ImGui::PushID(&node);
    }

    switch (node.type) {
    case UiNodeType::Card:
    case UiNodeType::Section: {
        std::string title = node.label.empty() ? node.id : node.label;
        if (node.style == "accent") {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.85f, 0.30f, 0.35f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.95f, 0.35f, 0.40f, 0.50f));
        } else if (node.style == "primary") {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.50f, 0.85f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.60f, 0.95f, 0.50f));
        } else if (node.style == "green") {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.70f, 0.40f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.80f, 0.45f, 0.50f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.14f, 0.17f, 0.23f, 0.70f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.18f, 0.22f, 0.30f, 0.85f));
        }

        if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(10.0f);
            ImGui::Spacing();
            for (auto& child : node.children) {
                DrawUiNode(child, panel);
            }
            ImGui::Spacing();
            ImGui::Unindent(10.0f);
        }
        ImGui::PopStyleColor(2);
        break;
    }
    case UiNodeType::Row:
    case UiNodeType::ActionRow: {
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) ImGui::SameLine();
            DrawUiNode(node.children[i], panel);
        }
        break;
    }
    case UiNodeType::Separator:
        ImGui::Separator();
        break;
    case UiNodeType::Spacing:
        ImGui::Spacing();
        break;
    case UiNodeType::Heading:
        UrushiUI::Spacing(4.0f);
        UrushiUI::Text(node.text.empty() ? node.label : node.text, UrushiUI::Tone::Accent);
        UrushiUI::Separator();
        break;
    case UiNodeType::Text: {
        const std::string& txt = (!node.id.empty() && panel.stringValues.find(node.id) != panel.stringValues.end())
            ? panel.stringValues[node.id]
            : (node.text.empty() ? node.label : node.text);
        UrushiUI::Tone tone = UrushiUI::Tone::Default;
        if (node.style == "dim") tone = UrushiUI::Tone::Muted;
        else if (node.style == "error") tone = UrushiUI::Tone::Red;
        else if (node.style == "accent") tone = UrushiUI::Tone::Accent;
        else if (node.style == "green") tone = UrushiUI::Tone::Green;
        else if (node.style == "yellow") tone = UrushiUI::Tone::Yellow;
        UrushiUI::TextWrapped(txt, tone);
        break;
    }
    case UiNodeType::Help:
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", node.text.c_str());
        }
        break;
    case UiNodeType::SliderFloat: {
        float& val = panel.floatValues[node.id];
        if (UrushiUI::SliderFloat(node.label, &val, node.minVal, node.maxVal, "%.2f")) {
            DispatchChange(panel, node.id);
        }
        break;
    }
    case UiNodeType::SliderInt: {
        int& val = panel.intValues[node.id];
        if (UrushiUI::SliderInt(node.label, &val, static_cast<int>(node.minVal), static_cast<int>(node.maxVal))) {
            DispatchChange(panel, node.id);
        }
        break;
    }
    case UiNodeType::Checkbox: {
        bool& val = panel.boolValues[node.id];
        if (UrushiUI::Toggle(node.label, &val)) {
            DispatchChange(panel, node.id);
        }
        break;
    }
    case UiNodeType::Combo: {
        int& idx = panel.intValues[node.id];
        if (!node.options.empty()) {
            if (UrushiUI::Combo(node.label, &idx, node.options)) {
                if (idx >= 0 && idx < static_cast<int>(node.options.size())) {
                    panel.stringValues[node.id] = node.options[idx];
                }
                DispatchChange(panel, node.id);
            }
        }
        break;
    }
    case UiNodeType::TextInput: {
        std::string& val = panel.stringValues[node.id];
        if (UrushiUI::InputText(node.label, val, 256)) {
            DispatchChange(panel, node.id);
        }
        break;
    }
    case UiNodeType::Button: {
        UrushiUI::ButtonStyle bStyle = UrushiUI::ButtonStyle::Default;
        if (node.style == "primary" || node.style == "green") bStyle = UrushiUI::ButtonStyle::Primary;
        else if (node.style == "accent") bStyle = UrushiUI::ButtonStyle::Danger;
        bool clicked = UrushiUI::Button(node.label, 0.0f, 30.0f, bStyle);
        if (clicked && !node.action.empty()) {
            DispatchAction(panel, node.action);
        }
        break;
    }
    default:
        break;
    }

    ImGui::PopID();
}

void UiSchema::RenderAll(bool menuOpen) {
    if (!menuOpen) return;
    // Panels dispatch into Lua (on_action / on_change): take the Lua lock first (same order as
    // LuaEngine::OnRender -> UI) so the state cannot be closed/reloaded underneath us.
    std::lock_guard<std::recursive_mutex> luaLock(LuaEngine::Mutex());
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_panels.empty()) return;

    for (auto& panel : s_panels) {
        if (!panel.visible) continue;

        ImGui::SetNextWindowSize(ImVec2(480, 520), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(panel.title.c_str(), &panel.visible)) {
            for (auto& node : panel.rootNodes) {
                DrawUiNode(node, panel);
            }
        }
        ImGui::End();
    }
}
