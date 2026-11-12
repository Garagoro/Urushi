-- hello_world: minimal example of a cloud script (also used to test the library end to end).
-- Runs in the isolated environment: normal API (ui, carx, urushi.register_window, config ...), no network, no process execution.
local T = i18n.t
local helper = include("lib/greeting.lua")      -- sibling file from this script's own folder

local opened = config.get("opened_count", 0)

urushi.register_window({
    id = "hello_world",
    title = T("Привет, мир", "Hello World"),
    icon = "settings",
    order = 900,
    default_open = false,
    width = 320,
    render = function()
        ui.card(T("Пример cloud-скрипта", "Cloud script example"), nil, function()
            ui.text(helper.greeting(T("Привет", "Hello")))
            ui.text_dim(string.format(T("Нажатий кнопки: %d", "Button clicks: %d"), opened))
            if ui.button(T("Нажми меня##hw_click", "Click me##hw_click"), ui.avail_width(), 28, "primary") then
                opened = opened + 1
                config.set("opened_count", opened)   -- saved to urushi_data/config.json, section "script:cloud:hello_world"
            end
        end)
    end,
})
