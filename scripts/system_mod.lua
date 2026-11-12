-- scripts/system_mod.lua
local T = i18n.t

local ACCENT_KEYS = { "amber", "vermilion", "platinum", "indigo", "custom" }
local SCALE_VALUES = { 0.0, 0.85, 1.00, 1.25, 1.50, 1.75 }

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
    ui.same_line(110)
    ui.text(desc_str, "muted")
end

local function render_fps_net_window()
    local env = urushi.get_env_stats and urushi.get_env_stats() or { fps = 0, frame_ms = 0, in_session = false, total_net_cars = 0 }
    local fps = env.fps or 0
    local ms = (fps > 0.1) and (1000.0 / fps) or (env.frame_ms or 0)

    local net_txt = T("Меню", "Menu")
    local net_tone = "muted"
    if env.in_session then
        local net_cars = env.total_net_cars or 0
        if net_cars > 0 then
            net_txt = string.format(T("Онлайн (%d)", "Online (%d)"), net_cars + 1)
            net_tone = "green"
        else
            net_txt = T("Соло", "Solo")
            net_tone = "green"
        end
    end

    ui.stat_row({
        { label = "FPS", value = string.format("%.0f", fps), tone = fps >= 55 and "green" or "yellow" },
        { label = T("КАДР", "FRAME"), value = string.format(T("%.1f мс", "%.1f ms"), ms), tone = "muted" },
        { label = T("СЕТЬ", "NET"), value = net_txt, tone = net_tone },
    })
end

local function render_system_window()
    local cur_lang = (urushi.get_language and urushi.get_language()) or "ru"
    local lang_idx = (cur_lang == "en") and 2 or 1

    ui.card(T("Интерфейс и тема", "Interface & Theme"), nil, function()
        ui.text_dim(T("Язык / Language", "Language / Язык"))
        local l_ch, new_lang_idx = ui.segmented("sys_lang_seg", lang_idx, { "Русский", "English" })
        if l_ch and urushi.set_language then
            urushi.set_language(new_lang_idx == 2 and "en" or "ru")
            if i18n and i18n.sync_titles then
                i18n.sync_titles()
            end
        end

        ui.spacing(4)
        ui.text_dim(T("Цветовой акцент", "Accent Color"))
        local cur_name = urushi.get_accent and urushi.get_accent() or "amber"
        local cur_idx = find_accent_idx(cur_name)
        local accent_labels = T(
            { "Янтарь", "Алый", "Серебро", "Индиго", "Свой" },
            { "Amber", "Crimson", "Silver", "Indigo", "Custom" }
        )
        local a_ch, new_idx = ui.segmented("sys_accent_seg", cur_idx, accent_labels)
        if a_ch and ACCENT_KEYS[new_idx] then
            urushi.set_accent(ACCENT_KEYS[new_idx])
            cur_name = ACCENT_KEYS[new_idx]
        end

        if cur_name == "custom" and ui.color_edit3 and urushi.get_custom_accent and urushi.set_custom_accent then
            local cr, cg, cb = urushi.get_custom_accent()
            local cch, nr, ng, nb = ui.color_edit3(T("Свой цвет##sys_custom_acc", "Custom Color##sys_custom_acc"), cr, cg, cb)
            if cch then
                urushi.set_custom_accent(nr, ng, nb)
            end
        end

        ui.spacing(4)
        ui.text_dim(T("Масштаб интерфейса", "UI Scale"))
        local user_sc = urushi.get_user_scale and urushi.get_user_scale() or 0.0
        local cur_sc_idx = find_scale_idx(user_sc)
        local scale_labels = { T("Авто", "Auto"), "85%", "100%", "125%", "150%", "175%" }
        local s_ch, new_sc_idx = ui.segmented("sys_scale_seg", cur_sc_idx, scale_labels)
        if s_ch and SCALE_VALUES[new_sc_idx] ~= nil and urushi.set_ui_scale then
            urushi.set_ui_scale(SCALE_VALUES[new_sc_idx])
        end

        local eff_pct = math.floor((urushi.get_ui_scale and urushi.get_ui_scale() or 1.0) * 100 + 0.5)
        ui.text_dim(string.format(T("Активно: %d%%", "Active: %d%%"), eff_pct))

        ui.spacing(4)
        if ui.button(T("Свернуть незакреплённые окна##sys_close_all", "Collapse Unpinned Windows##sys_close_all"), ui.avail_width(), 28, "ghost") then
            urushi.close_all()
        end
    end)

    if updater and updater.get_status then
        local ust = updater.get_status()
        local ver = ust.local_version or "0.2.0"
        local state_tone = "muted"
        local state_short = T("Актуально", "Up to date")

        if ust.state == "update_available" then
            state_tone = "yellow"; state_short = T("Доступно", "Available")
        elseif ust.state == "restart_required" then
            state_tone = "yellow"; state_short = T("Рестарт", "Restart")
        elseif ust.state == "reinjecting" then
            state_tone = "green"; state_short = T("Релоад", "Reloading")
        elseif ust.state == "downloading" then
            state_tone = "accent"; state_short = T("Загрузка", "Downloading")
        elseif ust.state == "checking" then
            state_tone = "accent"; state_short = T("Проверка", "Checking")
        elseif ust.state == "error" then
            state_tone = "red"; state_short = T("Ошибка", "Error")
        end

        ui.card(T("Обновление", "Updater"), "GitHub // Garagoro/Urushi", function()
            ui.stat_row({
                { label = T("Версия", "Version"), value = "v" .. ver, tone = "accent" },
                { label = T("Статус", "Status"), value = state_short, tone = state_tone },
            })
            if ust.state == "error" and ust.status_text and ust.status_text ~= "" then
                ui.spacing(2)
                ui.text_wrapped(ust.status_text, "muted")
            end

            ui.spacing(4)
            local bw = ui.avail_width()
            if ust.state == "update_available" then
                if ui.button(T("Установить обновление##sys_upd_inst", "Install Update##sys_upd_inst"), bw, 30, "primary") then
                    updater.install()
                end
            elseif ust.state == "restart_required" then
                if ui.button(T("Перезапустить игру##sys_upd_rest", "Restart Game##sys_upd_rest"), bw, 30, "danger") then
                    updater.restart_game()
                end
            elseif ust.state ~= "checking" and ust.state ~= "downloading" and ust.state ~= "reinjecting" then
                if ui.button(T("Проверить обновления##sys_upd_chk", "Check for Updates##sys_upd_chk"), bw, 28) then
                    updater.check()
                end
            end
        end)
    end

    ui.card(T("Горячие клавиши и управление", "Keybinds & Controls"), nil, function()
        local gp_conn = input and input.is_gamepad_connected and input.is_gamepad_connected()
        ui.badge(gp_conn and T("Геймпад: Подключен (XInput)", "Gamepad: Connected (XInput)") or T("Геймпад: Не обнаружен", "Gamepad: Not detected"), gp_conn and "green" or "muted")
        ui.spacing(4)

        if keybinds and keybinds.render then
            keybinds.render("kb_menu",        T("Открыть / скрыть меню", "Toggle URUSHI menu"), "menu_toggle")
            keybinds.render("kb_rec",         T("Старт / стоп записи", "Start / stop record"),  "replay_record")
            keybinds.render("kb_ghost",       T("Старт / стоп призрака", "Start / stop ghost"), "replay_ghost")
            keybinds.render("kb_anti",        T("Анти-встречка", "Anti-Wrong-Way"),             "anticollision")
            keybinds.render("kb_tp_save",     T("Запомнить точку (ТП)", "Save point (TP)"),     "tp_save")
            keybinds.render("kb_tp_go",       T("Телепорт в точку", "Teleport to point"),       "tp_teleport")
            keybinds.render("kb_perf",        T("Оптимизатор FPS", "FPS Optimizer"),            "perf_menu")
            keybinds.render("kb_carfx",       T("CarFX шейдеры", "CarFX Shaders"),              "carfx_menu")
            keybinds.render("kb_dummy",       T("3D-болванка", "3D Dummy"),                     "dummy_ghost")
        end

        ui.spacing(4)
        ui.text_dim(T("ЛКМ: нажать и переназначить на клавишу или кнопку геймпада\nПКМ / Del: сбросить · Esc: отмена",
                      "LMB: click to rebind to key or gamepad button\nRMB / Del: clear · Esc: cancel"))
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "fps_net",
        title = T("FPS и Сеть", "FPS & Network"),
        icon = "hud",
        order = 65,
        default_open = false,
        default_pinned = true,
        hud_no_inputs = false,
        anchor_right = true,
        x = 20,
        y = 300,
        width = 276,
        render = render_fps_net_window,
    })

    urushi.register_window({
        id = "system",
        title = T("Система", "System"),
        icon = "settings",
        order = 70,
        default_open = false,
        default_pinned = false,
        x = 290,
        y = 32,
        width = 412,
        render = render_system_window,
    })
end

log.info("URUSHI System settings module loaded.")
