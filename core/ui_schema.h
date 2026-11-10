#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

struct lua_State;

enum class UiNodeType {
    Unknown,
    Card,
    Section,
    Row,
    ActionRow,
    Separator,
    Spacing,
    Heading,
    Text,
    Help,
    SliderFloat,
    SliderInt,
    Checkbox,
    Combo,
    TextInput,
    Button
};

struct UiNode {
    UiNodeType type = UiNodeType::Unknown;
    std::string id;
    std::string label;
    std::string text;
    std::string style;
    std::string action;

    float minVal = 0.0f;
    float maxVal = 1.0f;

    std::vector<std::string> options;
    std::vector<UiNode> children;
};

struct ScriptPanel {
    std::string id;
    std::string title;
    std::string description;
    std::string category;
    bool visible = true;

    int luaOnActionRef = -1;
    int luaOnChangeRef = -1;

    std::vector<UiNode> rootNodes;

    std::unordered_map<std::string, float> floatValues;
    std::unordered_map<std::string, int> intValues;
    std::unordered_map<std::string, bool> boolValues;
    std::unordered_map<std::string, std::string> stringValues;
};

class UiSchema {
public:
    static void Init();
    static void Shutdown();

    static bool RegisterPanelFromLua(lua_State* L, int tableIdx);
    static void RenderAll(bool menuOpen);

    static void DispatchAction(ScriptPanel& panel, const std::string& actionId);
    static void DispatchChange(ScriptPanel& panel, const std::string& paramId);

    static void SetFloat(const std::string& panelId, const std::string& key, float val);
    static void SetInt(const std::string& panelId, const std::string& key, int val);
    static void SetBool(const std::string& panelId, const std::string& key, bool val);
    static void SetString(const std::string& panelId, const std::string& key, const std::string& val);
    static void SetOptions(const std::string& panelId, const std::string& nodeId, const std::vector<std::string>& opts);

    static float GetFloat(const std::string& panelId, const std::string& key, float defaultVal = 0.0f);
    static int GetInt(const std::string& panelId, const std::string& key, int defaultVal = 0);
    static bool GetBool(const std::string& panelId, const std::string& key, bool defaultVal = false);
    static std::string GetString(const std::string& panelId, const std::string& key, const std::string& defaultVal = "");

    static void ClearPanels();
    static ScriptPanel* FindPanel(const std::string& panelId);

private:
    static void DrawUiNode(UiNode& node, ScriptPanel& panel);
    static void PushPanelValuesToLua(lua_State* L, const ScriptPanel& panel);

    static std::vector<ScriptPanel> s_panels;
    static std::recursive_mutex s_mutex;
};
