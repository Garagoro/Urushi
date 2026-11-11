-- scripts/system_mod.lua
-- URUSHI System Settings, Theme Accent Switcher, Updater, FPS/Net Panel & Hotkey Reference

local ACCENT_KEYS = { "amber", "vermilion", "platinum", "indigo" }
local ACCENT_LABELS = { "Янтарь", "Киноварь", "Платина", "Индиго" }
local SCALE_VALUES = { 0.0, 0.85, 1.00, 1.25, 1.50, 1.75 }
local SCALE_LABELS = { "Авто", "85%", "100%", "125%", "150%", "175%" }

local function find_accent_idx(name)
    for i = 1, #ACCENT_KEYS do
        if ACCENT_KEYS[i] == name then return i end
    end
    return 1
end

local function find_scale_idx(val)
    if not val or val <= 0.01 then return 1 end
    local best_i = 2
    local best_d = 999.0
    for i = 2, #SCALE_VALUES do
        local d = math.abs(SCALE_VALUES[i] - val)
        if d < best_d then
            best_d = d
            best_i = i
        end
    end
    return best_i
end

local function hotkey_row(key_str, desc_str)
    ui.keycap(key_str)
    ui.same_line(116)
    ui.text(desc_str, "muted")
end

local function render_fps_net_window()
    local env = urushi.get_env_stats and urushi.get_env_stats() or { fps = 0, frame_ms = 0, in_session = false, total_net_cars = 0 }
    local fps = env.fps or 0
    local ms = (fps > 0.1) and (1000.0 / fps) or (env.frame_ms or 0)

    local net_txt = "Меню"
    local net_tone = "muted"
    if env.in_session then
        local net_cars = env.total_net_cars or 0
        if net_cars > 0 then
            net_txt = string.format("Онлайн (%d)", net_cars + 1)
            net_tone = "green"
        else
            net_txt = "Соло"
            net_tone = "green"
        end
    end

    ui.card("Производительность", "Рендер и сессия", function()
        ui.stat_row({
            { label = "FPS", value = string.format("%.0f", fps), tone = fps >= 55 and "green" or "yellow" },
            { label = "КАДР", value = string.format("%.1f мс", ms), tone = "muted" },
            { label = "СЕТЬ", value = net_txt, tone = net_tone },
        })
    end)
end

local function render_system_window()
    ui.card("Оформление и окна", "URUSHI Lacquered UI", function()
        ui.text_dim("Цветовой акцент лака")
        local cur_name = urushi.get_accent and urushi.get_accent() or "amber"
        local cur_idx = find_accent_idx(cur_name)
        local a_ch, new_idx = ui.segmented("sys_accent_seg", cur_idx, ACCENT_LABELS)
        if a_ch and ACCENT_KEYS[new_idx] then
            urushi.set_accent(ACCENT_KEYS[new_idx])
        end

        ui.spacing(6)
        ui.text_dim("Масштаб интерфейса (DPI)")
        local user_sc = urushi.get_user_scale and urushi.get_user_scale() or 0.0
        local cur_sc_idx = find_scale_idx(user_sc)
        local s_ch, new_sc_idx = ui.segmented("sys_scale_seg", cur_sc_idx, SCALE_LABELS)
        if s_ch and SCALE_VALUES[new_sc_idx] ~= nil and urushi.set_ui_scale then
            urushi.set_ui_scale(SCALE_VALUES[new_sc_idx])
        end
        local eff_pct = math.floor((urushi.get_ui_scale and urushi.get_ui_scale() or 1.0) * 100 + 0.5)
        local auto_pct = math.floor((urushi.get_auto_scale and urushi.get_auto_scale() or 1.0) * 100 + 0.5)
        local sc_info = (user_sc <= 0.01)
            and string.format("Активно: %d%% (авто по экрану и DPI)", eff_pct)
            or string.format("Активно: %d%% (ручной; авто: %d%%)", eff_pct, auto_pct)
        ui.text_dim(sc_info)

        ui.spacing(6)
        local bw = ui.avail_width()
        if ui.button("Свернуть все незакреплённые окна##sys_close_all", bw, 28, "ghost") then
            urushi.close_all()
        end
    end)

    if updater and updater.get_status then
        local ust = updater.get_status()
        local ver = ust.local_version or "0.1.0"
        local state_tone = "muted"
        if ust.state == "update_available" or ust.state == "restart_required" then
            state_tone = "yellow"
        elseif ust.state == "reinjecting" then
            state_tone = "green"
        elseif ust.state == "downloading" or ust.state == "checking" then
            state_tone = "accent"
        end

        local state_short = "Актуально"
        if ust.state == "update_available" then state_short = "Доступно"
        elseif ust.state == "restart_required" then state_short = "Рестарт"
        elseif ust.state == "reinjecting" then state_short = "Реинжект"
        elseif ust.state == "downloading" then state_short = "Загрузка"
        elseif ust.state == "checking" then state_short = "Проверка"
        elseif ust.status_text and ust.status_text ~= "" then state_short = "Ожидание"
        end

        ui.card("Обновление и ядро", "v" .. ver, function()
            ui.stat_row({
                { label = "Версия", value = "v" .. ver, tone = "accent" },
                { label = "Статус", value = state_short, tone = state_tone },
            })
            if ust.status_text and ust.status_text ~= "" then
                ui.spacing(2)
                ui.text_wrapped(ust.status_text, "muted")
            end

            ui.spacing(4)
            local bw = ui.avail_width()
            if ust.state == "update_available" then
                if ui.button("Установить обновление##sys_upd_inst", bw, 30, "primary") then
                    updater.install()
                end
            elseif ust.state == "restart_required" then
                if ui.button("Перезапустить игру##sys_upd_rest", bw, 30, "danger") then
                    updater.restart_game()
                end
            elseif ust.state ~= "checking" and ust.state ~= "downloading" and ust.state ~= "reinjecting" then
                if ui.button("Проверить обновления##sys_upd_chk", bw, 28) then
                    updater.check()
                end
            end
        end)
    end

    ui.card("Справка", "Горячие клавиши", function()
        if ui.collapsing("Показать сочетания клавиш", false) then
            hotkey_row("F4 / Ins", "Открыть / скрыть панель URUSHI")
            hotkey_row("F5", "Окно оптимизатора FPS")
            hotkey_row("F6", "Старт / стоп записи реплея")
            hotkey_row("F7", "Старт / стоп 3D-призрака реплея")
            hotkey_row("F8", "Окно 3D-болванки и сцены Unity")
            hotkey_row("F9", "Окно CarFX шейдеров")
            hotkey_row("J", "Вкл / выкл Анти-встречку")
            hotkey_row("K", "Коллизия 3D-призраков и болванки")
            hotkey_row("G / H", "Создать / удалить 3D-болванку")
            hotkey_row("M", "Живое зеркало (+3.8 м)")
            hotkey_row("[ / ]", "Запомнить точку / Телепорт")
            hotkey_row("Ctrl+F11", "Перезагрузить C++ ядро")
        end
        ui.spacing(4)
        ui.text_wrapped("Скрипты в scripts/ поддерживают мгновенный Hot-Reload при сохранении.", "muted")
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "fps_net",
        title = "FPS и Сеть",
        icon = "hud",
        order = 65,
        default_open = false,
        default_pinned = true,
        hud_no_inputs = false,
        anchor_right = true,
        x = 20,
        y = 320,
        width = 276,
        render = render_fps_net_window,
    })

    urushi.register_window({
        id = "system",
        title = "Система",
        icon = "settings",
        order = 70,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 420,
        render = render_system_window,
    })
end

log.info("URUSHI System settings module (scripts/system_mod.lua) loaded.")
