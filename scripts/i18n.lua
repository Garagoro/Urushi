-- scripts/i18n.lua
local i18n = {}

function i18n.is_en()
    return urushi and urushi.get_language and urushi.get_language() == "en"
end

function i18n.t(ru, en)
    return i18n.is_en() and en or ru
end

local WINDOW_TITLES = {
    perf          = { "Оптимизатор",    "Optimizer" },
    carfx         = { "CarFX Шейдеры",  "CarFX Shaders" },
    replay        = { "Реплеи 3D",      "3D Replays" },
    steer_assist  = { "Помощник руля",  "Steering Assist" },
    tires         = { "Кузов и шины",   "Body & Tires" },
    anticollision = { "Анти-встречка",  "Anti-Wrong-Way" },
    ghost         = { "Болванка 3D",    "3D Dummy" },
    teleport      = { "Телепорт",       "Teleport" },
    hud           = { "Телеметрия HUD", "Telemetry HUD" },
    fps_net       = { "FPS и Сеть",     "FPS & Network" },
    system        = { "Система",        "System" },
    library       = { "Библиотека",     "Library" },
    keybinds      = { "Горячие клавиши", "Keybinds" },
}

function i18n.sync_titles()
    if not urushi or not urushi.set_window_title then return end
    local en = i18n.is_en()
    for id, pair in pairs(WINDOW_TITLES) do
        urushi.set_window_title(id, en and pair[2] or pair[1])
    end
end

_G.i18n = i18n
return i18n
