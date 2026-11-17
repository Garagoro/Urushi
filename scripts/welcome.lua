-- scripts/welcome.lua
-- First-run welcome window for URUSHI

local function is_script_downloaded(id)
    if urushi and urushi.script_exists then
        return urushi.script_exists(id)
    end
    if urushi and urushi.list_scripts then
        for _, name in ipairs(urushi.list_scripts()) do
            if name == id then return true end
        end
    end
    return false
end

local function get_missing_official_scripts()
    local mf = (urushi.get_manifest and urushi.get_manifest()) or (cloud and cloud.get_manifest and cloud.get_manifest())
    if not mf or not mf.scripts then return {} end
    local missing = {}
    for _, s in ipairs(mf.scripts) do
        if not is_script_downloaded(s.id) then
            table.insert(missing, s)
        end
    end
    return missing
end

local function render_welcome_window()
    local w = ui.avail_width()
    local is_ru = not (i18n and i18n.is_en and i18n.is_en())

    -- 1. Language selector
    ui.card(T("Язык интерфейса", "Interface Language"), nil, function()
        local lang_idx = is_ru and 1 or 2
        local l_ch, new_lang_idx = ui.segmented("welc_lang_seg", lang_idx, { "Русский", "English" })
        if l_ch and urushi.set_language then
            local code = (new_lang_idx == 2) and "en" or "ru"
            urushi.set_language(code)
            if config then config.set("ui", "language", code) end
            if i18n and i18n.sync_titles then i18n.sync_titles() end
        end
    end)
    ui.spacing(4)

    -- 2. Official Pack Download Block (only if official scripts are missing)
    local missing = get_missing_official_scripts()
    local pst = cloud and cloud.get_pack_download_status and cloud.get_pack_download_status()
    local is_downloading = pst and pst.downloading
    local is_failed = pst and pst.completed and (not pst.success)
    local is_succeeded = pst and pst.completed and pst.success

    if #missing > 0 or is_downloading or is_failed then
        ui.begin_card(T("Официальные скрипты и шейдеры", "Official Scripts & Shaders"), nil)

        if is_downloading then
            local cur_n = pst.downloaded_files or 0
            local tot_n = (pst.total_files and pst.total_files > 0) and pst.total_files or #missing
            local cur_f = (pst.current_file ~= "") and pst.current_file or "..."
            local mb_cur = (pst.downloaded_bytes or 0) / (1024 * 1024)
            local mb_tot = (pst.total_bytes or 0) / (1024 * 1024)

            local status_text_ru = string.format("Загрузка: %d из %d файлов (%.1f / %.1f МБ)", cur_n, tot_n, mb_cur, mb_tot)
            local status_text_en = string.format("Downloading: %d of %d files (%.1f / %.1f MB)", cur_n, tot_n, mb_cur, mb_tot)
            ui.text(T(status_text_ru, status_text_en), "accent")
            ui.spacing(2)

            local frac = (tot_n > 0) and (cur_n / tot_n) or 0.0
            ui.progress_bar(frac, w, 8)
            ui.spacing(2)
            ui.text_dim(T("Файл: ", "File: ") .. cur_f)

        elseif is_failed then
            ui.text_colored(1.0, 0.35, 0.35, T("Ошибка скачивания: ", "Download error: ") .. tostring(pst.error))
            ui.spacing(3)

            if ui.button(T("Повторить попытку##welc_retry", "Retry##welc_retry"), w, 28, "primary") then
                if cloud and cloud.download_pack then
                    cloud.download_pack()
                end
            end
            ui.spacing(3)

            ui.text_dim(T("Вы можете скачать полный архив с GitHub Releases:", "You can download the full package from GitHub Releases:"))
            local gh_url = "https://github.com/Garagoro/Urushi/releases/latest"
            ui.input_text("##welc_gh_url", gh_url)

        elseif is_succeeded then
            ui.badge(T("Официальный пак успешно установлен!", "Official pack installed successfully!"), "green")

        else
            -- Initial state: NO network requests made prior to user clicking the button
            ui.text_dim(T("У вас установлена базовая mod.io сборка. Хотите установить официальный набор скриптов (HUD, телеметрия, оптимизатор FPS, реплеи, CarFX)?",
                          "You are running the minimal mod.io package. Would you like to install the official script & shader pack (HUD, telemetry, FPS optimizer, replays, CarFX)?"))
            ui.spacing(3)

            local btn_text = T(string.format("⬇ Установить официальный пак (%d)##welc_dl", #missing),
                               string.format("⬇ Install Official Pack (%d)##welc_dl", #missing))
            if ui.button(btn_text, w, 30, "primary") then
                if cloud and cloud.download_pack then
                    cloud.download_pack()
                end
            end
            ui.spacing(2)

            if ui.button(T("Выберу сам (открыть Скрипты)##welc_skip", "I'll pick myself (open Scripts)##welc_skip"), w, 24, "ghost") then
                if urushi.set_window_open then
                    urushi.set_window_open("library", true)
                    urushi.set_window_open("welcome", false)
                end
                if urushi.mark_first_run_done then
                    urushi.mark_first_run_done()
                end
            end
        end

        ui.end_card()
        ui.spacing(4)
    end

    -- 3. Controls / Hotkeys
    ui.card(T("Горячие клавиши", "Hotkeys & Controls"), nil, function()
        local menu_key = "F4"
        if keybinds and keybinds.get then
            local kb = keybinds.get("menu_toggle")
            if kb and kb.name and kb.name ~= "" then
                menu_key = kb.name
            end
        end

        ui.keycap(menu_key)
        ui.same_line(100)
        ui.text_dim(T("Открыть / скрыть оверлей URUSHI", "Toggle URUSHI menu overlay"))

        ui.spacing(4)
        ui.keycap("Ctrl + F11")
        ui.same_line(100)
        ui.text_dim(T("Горячая перезагрузка всех скриптов", "Hot-reload all Lua scripts"))
    end)
    ui.spacing(6)

    -- 4. Done button
    if ui.button(T("Готово##welc_done", "Done##welc_done"), w, 32, "primary") then
        if urushi.mark_first_run_done then
            urushi.mark_first_run_done()
        end
        if urushi.set_window_open then
            urushi.set_window_open("welcome", false)
        end
    end
end

-- Only register welcome window on first run
local is_first = (urushi and urushi.is_first_run and urushi.is_first_run())
if is_first then
    if urushi and urushi.register_window then
        urushi.register_window({
            id = "welcome",
            title = T("Добро пожаловать", "Welcome"),
            icon = "star",
            order = 1,
            default_open = true,
            default_pinned = false,
            x = 320,
            y = 70,
            width = 440,
            render = render_welcome_window,
        })
    end
end
