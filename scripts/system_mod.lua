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

-- ==================== Neverlose-Style Unified Scripts Manager ====================
local function get_official_modules()
    local manifest = (urushi.get_manifest and urushi.get_manifest()) or (cloud and cloud.get_manifest and cloud.get_manifest())
    local mods = {}
    if manifest and manifest.scripts then
        local ver = manifest.core_version or "1.3.0"
        for _, s in ipairs(manifest.scripts) do
            local win_id = (s.window_ids and s.window_ids[1]) or s.id
            local alt_win_ids = nil
            if s.window_ids and #s.window_ids > 1 then
                alt_win_ids = {}
                for i = 2, #s.window_ids do
                    table.insert(alt_win_ids, s.window_ids[i])
                end
            end
            table.insert(mods, {
                id = s.id,
                win_id = win_id,
                alt_win_ids = alt_win_ids,
                name = s.name,
                name_ru = s.name_ru or s.name,
                author = "URUSHI Team",
                version = ver,
                desc = s.description or "",
                desc_ru = s.description_ru or s.description or "",
                min_core = s.min_core or "1.0.0",
                default_enabled = (s.default_enabled == true),
            })
        end
    end
    return mods
end

local downloading_scripts = {}
local script_search_query = ""

local function is_script_running(id)
    if urushi_scripts and urushi_scripts.list then
        for _, e in ipairs(urushi_scripts.list) do
            if e.id == id or e.id == ("cloud:" .. id) then
                return e.enabled and (e.status == "ok")
            end
        end
    end
    local v = urushi.get_setting and urushi.get_setting("scripts", id, "0")
    return v == "1"
end

local function is_script_downloaded(id)
    if urushi.script_exists then
        return urushi.script_exists(id)
    end
    if urushi.list_scripts then
        for _, name in ipairs(urushi.list_scripts()) do
            if name == id then return true end
        end
    end
    return false
end

local function start_script(id)
    local is_cloud = (type(id) == "string" and id:sub(1, 6) == "cloud:")
    local bare_id = is_cloud and id:sub(7) or id

    if is_cloud then
        local installed = false
        if cloud and cloud.status then
            local cst = cloud.status()
            if cst and cst.items then
                for _, cit in ipairs(cst.items) do
                    if cit.id == bare_id then
                        installed = cit.installed
                        break
                    end
                end
            end
        end
        if not installed then
            if cloud and cloud.install then cloud.install(bare_id) end
            return
        end
    else
        local downloaded = is_script_downloaded(id)
        if not downloaded then
            downloading_scripts[id] = true
            if cloud and cloud.update_script then
                cloud.update_script(id)
            end
            return
        end
    end

    if urushi.set_setting then urushi.set_setting("scripts", id, "1") end
    if urushi.reload_scripts then urushi.reload_scripts() end
end

local function stop_script(id)
    if urushi.set_setting then urushi.set_setting("scripts", id, "0") end
    if urushi.reload_scripts then urushi.reload_scripts() end
end

local function delete_script(id)
    stop_script(id)
    if urushi.delete_script then
        urushi.delete_script(id)
    end
end

local function any_script_update_available()
    if cloud and cloud.get_script_updates then
        local up_snap = cloud.get_script_updates()
        if up_snap and up_snap.items then
            for _, it in pairs(up_snap.items) do
                if it.state == "update_available" then
                    return true
                end
            end
        end
    end
    return false
end

local last_lib_has_upd = nil
local function update_library_indicator(force)
    local has_upd = any_script_update_available()
    if force or (has_upd ~= last_lib_has_upd) then
        last_lib_has_upd = has_upd
        if urushi and urushi.set_window_title then
            local is_en = (i18n and i18n.is_en and i18n.is_en())
            local base_t = is_en and "Scripts" or "Скрипты"
            urushi.set_window_title("library", base_t .. (has_upd and " •" or ""))
        end
    end
end

local function render_scripts_manager_window()
    local w = ui.avail_width()
    local up_snap = (cloud and cloud.get_script_updates) and cloud.get_script_updates() or { checking = false, items = {} }

    -- Pack download button & progress (first thing visible when official scripts are missing)
    local official_modules = get_official_modules()
    local missing_count = 0
    for _, mod in ipairs(official_modules) do
        if not is_script_downloaded(mod.id) then
            missing_count = missing_count + 1
        end
    end

    local pst = (cloud and cloud.get_pack_download_status and cloud.get_pack_download_status())
    local pack_downloading = (pst and pst.downloading) or (cloud and cloud.is_pack_downloading and cloud.is_pack_downloading()) or false
    if pack_downloading then
        local cur_f = (pst and pst.current_file ~= "") and pst.current_file or "..."
        local cur_n = pst and pst.downloaded_files or 0
        local tot_n = pst and pst.total_files or 0
        local p_text = string.format("Загрузка официального пака: %d/%d (%s)...", cur_n, tot_n, cur_f)
        local p_text_en = string.format("Downloading official pack: %d/%d (%s)...", cur_n, tot_n, cur_f)
        ui.badge(T(p_text, p_text_en), "accent")
        local frac = 0.0
        if tot_n > 0 then frac = cur_n / tot_n end
        ui.progress_bar(frac, w, 6)
        ui.spacing(3)
    elseif missing_count > 0 then
        if ui.button(T(string.format("⬇ Скачать официальный пак скриптов (%d)##dl_all_pack", missing_count),
                       string.format("⬇ Download Official Script Pack (%d)##dl_all_pack", missing_count)), w, 28, "primary") then
            if cloud and cloud.download_pack then
                cloud.download_pack()
            end
        end
        ui.spacing(3)
    end

    -- Search bar
    local s_ch, s_text = ui.search_input("##scripts_search", script_search_query, T("Поиск скриптов...", "Search scripts..."))
    if s_ch then
        script_search_query = s_text
    end
    ui.spacing(4)

    -- Search query matching helper
    local query = script_search_query:lower()
    local function query_matches(name, name_ru, id, desc, desc_ru, author)
        if query == "" then return true end
        if name and name:lower():find(query, 1, true) then return true end
        if name_ru and name_ru:lower():find(query, 1, true) then return true end
        if id and id:lower():find(query, 1, true) then return true end
        if desc and desc:lower():find(query, 1, true) then return true end
        if desc_ru and desc_ru:lower():find(query, 1, true) then return true end
        if author and author:lower():find(query, 1, true) then return true end
        return false
    end

    local total_shown = 0
    -- Official Modules List (Neverlose-Style Compact Rows)
    for _, mod in ipairs(official_modules) do
        if query_matches(mod.name, mod.name_ru, mod.id, mod.desc, mod.desc_ru, mod.author) then
            total_shown = total_shown + 1
            local running = is_script_running(mod.id)
            local downloaded = is_script_downloaded(mod.id)
            local downloading = downloading_scripts[mod.id] == true
            local it = up_snap.items and up_snap.items[mod.id]
            local has_upd = it and (it.state == "update_available")
            local is_upd_ing = it and (it.state == "updating")
            local is_incompat = it and (it.state == "incompatible")
            if is_upd_ing then downloading = true end

            local tag = ""
            if is_incompat then
                tag = T("требуется URUSHI v" .. (mod.min_core or ""), "requires URUSHI v" .. (mod.min_core or ""))
            end

            local act = ui.script_row({
                id = mod.id,
                title = T(mod.name_ru, mod.name),
                author = mod.author,
                version = mod.version,
                desc = T(mod.desc_ru, mod.desc),
                running = running,
                downloaded = downloaded,
                downloading = downloading,
                has_update = has_upd and not is_incompat,
                disabled = is_incompat,
                tag = tag,
            })

            if act == "load" and not is_incompat then
                start_script(mod.id)
            elseif act == "stop" then
                stop_script(mod.id)
            end
        end
    end

    -- User scripts
    local user_scripts = {}
    if urushi.list_scripts then
        local known = { main = true, i18n = true, config = true, keybinds = true, il2cpp = true, system_mod = true, welcome = true }
        for _, m in ipairs(official_modules) do known[m.id] = true end
        for _, name in ipairs(urushi.list_scripts()) do
            if not known[name] then
                table.insert(user_scripts, name)
            end
        end
    end

    if #user_scripts > 0 then
        local shown_user_hdr = false
        for _, uname in ipairs(user_scripts) do
            if query_matches(uname, nil, uname, nil, nil, "Local") then
                total_shown = total_shown + 1
                if not shown_user_hdr then
                    shown_user_hdr = true
                    ui.spacing(4)
                    ui.separator()
                    ui.spacing(2)
                    ui.text(T("Пользовательские скрипты", "User Scripts"), "accent")
                    ui.spacing(2)
                end

                local running = is_script_running(uname)
                local act = ui.script_row({
                    id = uname,
                    title = uname,
                    author = T("Локальный", "Local"),
                    version = "lua",
                    desc = uname .. ".lua",
                    running = running,
                    downloaded = true,
                    downloading = false,
                    has_update = false,
                })

                if act == "load" then
                    start_script(uname)
                elseif act == "stop" then
                    stop_script(uname)
                end
            end
        end
    end

    -- Community scripts (verified catalog)
    if cloud and cloud.status then
        local cst = cloud.status()
        if cst and cst.items and #cst.items > 0 then
            local shown_community_hdr = false
            for _, cit in ipairs(cst.items) do
                local cdesc = T((cit.description_ru ~= "" and cit.description_ru or cit.description), cit.description)
                if query_matches(cit.name, cit.description_ru, cit.id, cit.description, nil, cit.author) then
                    total_shown = total_shown + 1
                    if not shown_community_hdr then
                        shown_community_hdr = true
                        ui.spacing(4)
                        ui.separator()
                        ui.spacing(2)
                        ui.text(T("Сообщество (verified)", "Community (verified)"), "accent")
                        ui.spacing(2)
                    end

                    local running = is_script_running("cloud:" .. cit.id)
                    local act = ui.script_row({
                        id = "cld_" .. cit.id,
                        title = cit.name,
                        author = (cit.author ~= "" and cit.author or "Community"),
                        version = cit.version,
                        desc = cdesc,
                        running = running,
                        downloaded = cit.installed,
                        downloading = (cit.state == "installing"),
                        has_update = false,
                    })

                    if act == "load" then
                        if cit.installed then
                            start_script("cloud:" .. cit.id)
                        else
                            cloud.install(cit.id)
                        end
                    elseif act == "stop" then
                        stop_script("cloud:" .. cit.id)
                    end
                end
            end
        end
    end

    if total_shown == 0 and script_search_query ~= "" then
        ui.spacing(8)
        ui.text_dim(T("Ничего не найдено по запросу: \"" .. script_search_query .. "\"",
                      "No scripts found matching: \"" .. script_search_query .. "\""))
        ui.spacing(4)
        if ui.button(T("Очистить поиск##clr_srch", "Clear search##clr_srch"), 130, 24, "ghost") then
            script_search_query = ""
        end
    end
end

-- ==================== Updater Logic & Preferences ====================
local upd_cfg = {
    auto_check = true,
    auto_install = false,   -- default FALSE: no automatic background freezes during gameplay
    check_interval = 180,
}
if config and config.bind then
    upd_cfg = config.bind("updater", upd_cfg)
end
-- Force auto_install to false so existing configs with true won't lag the user
upd_cfg.auto_install = false

local last_check_time = os.clock()

if events and events.on_update then
    events.on_update(function(dt)
        update_library_indicator()

        -- Watch downloading scripts
        if next(downloading_scripts) ~= nil then
            local up_snap = (cloud and cloud.get_script_updates) and cloud.get_script_updates() or { items = {} }
            for id, _ in pairs(downloading_scripts) do
                local it = up_snap.items and up_snap.items[id]
                if it and (it.state == "updated" or it.state == "up_to_date") then
                    downloading_scripts[id] = nil
                    if urushi.set_setting then urushi.set_setting("scripts", id, "1") end
                    if urushi.reload_scripts then urushi.reload_scripts() end
                elseif it and (it.state == "error" or it.state == "incompatible") then
                    downloading_scripts[id] = nil
                end
            end
        end

        if not updater or not updater.get_status then return end
        local now = os.clock()
        local interval = tonumber(upd_cfg.check_interval) or 180

        -- Periodic background check ONLY checks for updates; downloading & reinjection are strictly manual via UI button.
        if upd_cfg.auto_check and (now - last_check_time >= interval) then
            last_check_time = now
            local ust = updater.get_status()
            if ust.state == "idle" or ust.state == "up_to_date" then
                updater.check()
            end
        end
    end)
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
            update_library_indicator(true)
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

    ui.card(T("Режим разработчика", "Developer Mode"), "Named Pipe & MCP", function()
        local dev_on = (urushi.is_dev_mode and urushi.is_dev_mode()) or false
        local d_ch, d_val = ui.toggle(T("Dev mode (MCP)##sys_dev_mcp", "Dev mode (MCP)##sys_dev_mcp"), dev_on)
        if d_ch and urushi.set_dev_mode then
            urushi.set_dev_mode(d_val)
        end
        ui.spacing(2)
        ui.text_dim(T("Включает DevPipe (\\\\.\\pipe\\urushi_dev) для подключения ИИ-ассистентов и отладки.",
                      "Enables DevPipe (\\\\.\\pipe\\urushi_dev) for AI assistant MCP connections & debugging."))
    end)

    if updater and updater.get_status then
        local ust = updater.get_status()
        local ver = ust.local_version or "0.3.1"
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
            elseif ust.state == "downloading" and ust.current_file and ust.current_file ~= "" then
                ui.spacing(2)
                local cur = ust.current_file:match("[^/\\]+$") or ust.current_file
                ui.text_dim(string.format("%s (%d/%d)", cur, ust.files_done or 0, ust.files_total or 0))
            end

            ui.spacing(4)
            local ac_ch, ac_val = ui.toggle(T("Авто-проверка обновлений (уведомления)##sys_upd_ac", "Auto-check for updates (notify only)##sys_upd_ac"), upd_cfg.auto_check)
            if ac_ch then
                upd_cfg.auto_check = ac_val
                if config then config.mark_dirty() end
            end

            ui.spacing(6)
            local bw = ui.avail_width()
            if ust.state == "update_available" then
                local ver_target = ust.remote_version or "?"
                if ui.button(T(string.format("Скачать и обновить до v%s (Реинжект)##sys_upd_inst", ver_target),
                               string.format("Download & Update to v%s (Reinject)##sys_upd_inst", ver_target)),
                             bw, 34, "primary") then
                    updater.install()
                end
                ui.spacing(3)
                if ui.button(T("Реинжектить текущую версию (Hot-Reload)##sys_upd_reinj_curr", "Reinject Current Version (Hot-Reload)##sys_upd_reinj_curr"), bw, 28, "ghost") then
                    if updater.reinject then updater.reinject() end
                end
            elseif ust.state == "restart_required" then
                if ui.button(T("Реинжектить на лету##sys_upd_reinj_live", "Reinject Live##sys_upd_reinj_live"), bw, 30, "accent") then
                    if updater.reinject then updater.reinject() end
                end
                ui.spacing(2)
                if ui.button(T("Перезапустить игру##sys_upd_rest", "Restart Game##sys_upd_rest"), bw, 26, "ghost") then
                    updater.restart_game()
                end
            elseif ust.state == "downloading" or ust.state == "reinjecting" or ust.state == "checking" then
                ui.text_dim(T("Пожалуйста, подождите...", "Please wait..."))
            else
                if ui.button(T("Перезагрузить URUSHI (Реинжект DLL)##sys_upd_reinj", "Hot-Reload URUSHI DLL##sys_upd_reinj"), bw, 32, "primary") then
                    if updater.reinject then updater.reinject() end
                end
                ui.spacing(3)
                if ui.button(T("Проверить обновления на GitHub##sys_upd_chk", "Check for Updates on GitHub##sys_upd_chk"), bw, 28, "secondary") then
                    last_check_time = os.clock()
                    updater.check()
                end
            end
        end)
    end

    render_stability_card()
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "fps_net",
        title = T("FPS и Сеть", "FPS & Network"),
        icon = "hud",
        order = 70,
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
        order = 90,
        default_open = false,
        default_pinned = false,
        x = 290,
        y = 32,
        width = 412,
        render = render_system_window,
    })

    urushi.register_window({
        id = "library",
        title = T("Скрипты", "Scripts"),
        icon = "library",
        order = 85,
        default_open = false,
        default_pinned = false,
        x = 310,
        y = 60,
        width = 460,
        max_height = 460,
        render = render_scripts_manager_window,
    })
end

log.info("URUSHI System settings module loaded.")
