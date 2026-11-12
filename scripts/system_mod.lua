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

local CRASH_POLICIES = { "ask", "safe", "normal" }

local function render_stability_card()
    ui.card(T("Стабильность", "Stability"), T("Safe mode и отчёты о крашах", "Safe mode & crash reports"), function()
        ui.text_dim(T("Что делать после краша", "After a crash"))
        local pol = urushi.get_setting("stability", "crash_policy", "ask")
        local idx = (pol == "safe") and 2 or ((pol == "normal") and 3 or 1)
        local labels = T({ "Спрашивать", "Safe mode", "Обычный" }, { "Ask", "Safe mode", "Normal" })
        local ch, new_idx = ui.segmented("sys_crash_policy", idx, labels)
        if ch and CRASH_POLICIES[new_idx] then
            urushi.set_setting("stability", "crash_policy", CRASH_POLICIES[new_idx])
        end

        ui.spacing(4)
        local fs = urushi.get_setting("stability", "force_safe_next", "0") == "1"
        local c2, v2 = ui.toggle(T("Следующий запуск в Safe mode##sys_safe_next", "Next launch in Safe mode##sys_safe_next"), fs)
        if c2 then urushi.set_setting("stability", "force_safe_next", v2) end
        ui.text_dim(T("Safe mode отключает все функции, мод остаётся загруженным.",
                      "Safe mode disables every feature; the mod stays loaded."))

        local crash = urushi.get_last_crash()
        if crash and crash.crashed then
            ui.spacing(4)
            ui.badge(T("Прошлая сессия: краш", "Last session: crash"), "red")
            if crash.summary and crash.summary ~= "" then ui.text_wrapped(crash.summary, "muted") end
            ui.text_dim(crash.in_urushi and T("Падение в коде URUSHI", "Crash inside URUSHI code")
                                         or T("Падение вне кода URUSHI", "Crash outside URUSHI code"))
        end

        ui.spacing(4)
        if ui.button(T("Открыть папку отчётов##sys_crash_dir", "Open crash reports folder##sys_crash_dir"), ui.avail_width(), 28, "ghost") then
            urushi.open_crash_dir()
        end
    end)
end

local function first_line(s)
    s = tostring(s or "")
    return (s:match("^[^\n]*")) or s
end

local function render_scripts_card()
    if not urushi_scripts then return end
    ui.card(T("Скачанные", "Downloaded"), T("Включение и отключение скриптов (имя файла)", "Enable or disable scripts (file name)"), function()
        for _, e in ipairs(urushi_scripts.list) do
            local title
            if e.cloud then
                title = e.cloud_id .. "/" .. ((e.entry and e.entry ~= "") and e.entry or "(shaders)") .. "  v" .. tostring(e.version)
            else
                title = e.id .. ".lua"
            end
            if e.isolated then title = title .. "  [" .. T("изолирован", "isolated") .. "]" end
            local ch, val = ui.toggle(title .. "##scr_" .. e.id, e.enabled)
            if ch then
                e.enabled = val
                urushi.set_setting("scripts", e.id, val)
                urushi_scripts.dirty = true
            end
            if e.status == "error" then
                ui.text_wrapped(T("Ошибка: ", "Error: ") .. first_line(e.err), "red")
            elseif e.user and not e.enabled then
                ui.text_dim(T("Пользовательский скрипт (выключен)", "User script (off)"))
            end
        end

        ui.spacing(4)
        if urushi_scripts.dirty then
            ui.text(T("Изменения применятся после перезагрузки скриптов.", "Changes apply after reloading scripts."), "yellow")
        end
        if ui.button(T("Перезагрузить скрипты##scr_reload", "Reload scripts##scr_reload"), ui.avail_width(), 30,
                     urushi_scripts.dirty and "primary" or nil) then
            if config then config.save() end   -- flush pending changes before the Lua state is recreated
            urushi.reload_scripts()
        end
    end)
end

local function render_library_card()
    if not cloud then return end
    ui.card(T("Библиотека скриптов", "Script Library"),
            T("Проверенные скрипты (ветка verified)", "Reviewed scripts (verified branch)"), function()
        local st = cloud.status()
        local w = ui.avail_width()
        if st.state == "fetching" then
            ui.text(T("Загрузка списка...", "Loading list..."), "accent")
        elseif ui.button(T("Обновить список##lib_refresh", "Refresh list##lib_refresh"), w, 28) then
            cloud.refresh()
        end
        if st.state == "idle" then
            ui.text_dim(T("Ничего не скачивается автоматически. Нажмите «Обновить список».",
                          "Nothing is downloaded automatically. Press \"Refresh list\"."))
        elseif st.state == "error" then
            ui.text_wrapped(T("Ошибка: ", "Error: ") .. tostring(st.error), "red")
        elseif st.state == "ready" and #st.items == 0 then
            ui.text_dim(T("В каталоге пока нет скриптов.", "The catalog is empty."))
        end

        for _, it in ipairs(st.items) do
            ui.separator()
            ui.text(it.id .. "  v" .. it.version, "accent")
            if it.author ~= "" then ui.text_dim(it.author) end
            local desc = T((it.description_ru ~= "" and it.description_ru or it.description), it.description)
            if desc ~= "" then ui.text_wrapped(desc, "muted") end

            if it.state == "installing" then
                ui.text(T("Установка...", "Installing..."), "accent")
            elseif not it.installed then
                if ui.button(T("Установить##lib_i_", "Install##lib_i_") .. it.id, w, 28, "primary") then cloud.install(it.id) end
            else
                if it.update then
                    if ui.button(T("Обновить до v", "Update to v") .. it.version .. "##lib_u_" .. it.id, w, 28, "primary") then
                        cloud.install(it.id)
                    end
                else
                    ui.badge(T("Установлено", "Installed") .. " v" .. it.installed_version, "green")
                end
                if ui.button(T("Удалить##lib_r_", "Remove##lib_r_") .. it.id, w, 26, "ghost") then
                    local ok, err = cloud.remove(it.id)
                    if ok then urushi_scripts.dirty = true end
                end
                if urushi_scripts.cloud_loaded[it.id] ~= it.installed_version then
                    ui.text(T("Нажмите «Перезагрузить скрипты», чтобы применить.", "Press \"Reload scripts\" to apply."), "yellow")
                    urushi_scripts.dirty = true
                end
            end
            if it.state == "error" then ui.text_wrapped(T("Ошибка: ", "Error: ") .. it.error, "red") end
        end
    end)
end

-- ==================== persisted UI preferences (scripts/config.lua -> urushi_data/config.json) ====================
local function apply_saved_prefs()
    if not config then return end
    local lang = config.get("ui", "language")
    if lang and urushi.set_language then urushi.set_language(lang) end
    local accent = config.get("ui", "accent")
    if accent and urushi.set_accent then urushi.set_accent(accent) end
    local ca = config.get("ui", "custom_accent")
    if type(ca) == "table" and urushi.set_custom_accent then urushi.set_custom_accent(ca[1] or 1, ca[2] or 1, ca[3] or 1) end
    local sc = config.get("ui", "scale")
    if sc and urushi.set_ui_scale then urushi.set_ui_scale(sc) end
end
apply_saved_prefs()

-- Separate "Library" window: script toggles (installed / built-in / user) + the cloud script library
local library_tab = 1
local function render_library_window()
    local ch, idx = ui.segmented("lib_tab_seg", library_tab, T({ "Скачанные", "Облачные" }, { "Downloaded", "Cloud" }))
    if ch then library_tab = idx end
    if library_tab == 1 then render_scripts_card() else render_library_card() end
end

local function render_system_window()
    local cur_lang = (urushi.get_language and urushi.get_language()) or "ru"
    local lang_idx = (cur_lang == "en") and 2 or 1

    ui.card(T("Интерфейс и тема", "Interface & Theme"), nil, function()
        ui.text_dim(T("Язык / Language", "Language / Язык"))
        local l_ch, new_lang_idx = ui.segmented("sys_lang_seg", lang_idx, { "Русский", "English" })
        if l_ch and urushi.set_language then
            urushi.set_language(new_lang_idx == 2 and "en" or "ru")
            if config then config.set("ui", "language", new_lang_idx == 2 and "en" or "ru") end
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
            if config then config.set("ui", "accent", cur_name) end
        end

        if cur_name == "custom" and ui.color_edit3 and urushi.get_custom_accent and urushi.set_custom_accent then
            local cr, cg, cb = urushi.get_custom_accent()
            local cch, nr, ng, nb = ui.color_edit3(T("Свой цвет##sys_custom_acc", "Custom Color##sys_custom_acc"), cr, cg, cb)
            if cch then
                urushi.set_custom_accent(nr, ng, nb)
                if config then config.set("ui", "custom_accent", { nr, ng, nb }) end
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
            if config then config.set("ui", "scale", SCALE_VALUES[new_sc_idx]) end
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

    render_stability_card()

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

    urushi.register_window({
        id = "library",
        title = T("Библиотека", "Library"),
        icon = "library",
        order = 68,
        default_open = false,
        default_pinned = false,
        x = 310,
        y = 60,
        width = 430,
        render = render_library_window,
    })
end

log.info("URUSHI System settings module loaded.")
