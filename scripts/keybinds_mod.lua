-- scripts/keybinds_mod.lua
-- Dedicated Keybinds & Controls window for CarX2 URUSHI
-- Dynamically aggregates and displays bindings from all active scripts

local function T(ru, en)
    if i18n and i18n.t then return i18n.t(ru, en) end
    local lang = (urushi.get_language and urushi.get_language()) or "ru"
    return (lang == "en") and en or ru
end

local function render_keybinds_window()
    local gp_conn = input and input.is_gamepad_connected and input.is_gamepad_connected()
    ui.badge(
        gp_conn and T("Геймпад: Подключен (XInput)", "Gamepad: Connected (XInput)")
                 or T("Геймпад: Не обнаружен", "Gamepad: Not detected"),
        gp_conn and "green" or "muted"
    )
    ui.spacing(4)

    if not keybinds or not keybinds.get_grouped_actions then
        ui.text(T("Модуль keybinds не загружен", "Keybinds module not loaded"), "red")
        return
    end

    local by_mod, mod_order = keybinds.get_grouped_actions()

    for _, mod_name in ipairs(mod_order) do
        local actions = by_mod[mod_name]
        if actions and #actions > 0 then
            ui.card(mod_name, nil, function()
                for _, act in ipairs(actions) do
                    local widget_id = "kb_" .. act.action_id
                    keybinds.render(widget_id, act.label, act.action_id)
                end
            end)
            ui.spacing(2)
        end
    end

    ui.card(T("Справка", "Help"), nil, function()
        ui.text_dim(T(
            "ЛКМ: нажать и переназначить на клавишу или кнопку геймпада\nПКМ / Del: сбросить бинд · Esc: отмена",
            "LMB: click to rebind to key or gamepad button\nRMB / Del: clear · Esc: cancel"
        ))
        ui.spacing(4)
        if ui.button(T("Сбросить все бинды по умолчанию##kb_reset", "Reset all keybinds to defaults##kb_reset"), ui.avail_width(), 26, "ghost") then
            keybinds.reset_defaults()
        end
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "keybinds",
        title = T("Горячие клавиши", "Keybinds"),
        icon = "keyboard",
        order = 45,
        default_open = false,
        width = 412,
        render = render_keybinds_window,
    })
end
