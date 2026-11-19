-- scripts/music_mod.lua
-- URUSHI Music-Reactive Livery Module
local T = i18n.t

if not audio_in then
    log.error("music_mod: native audio_in module is missing")
    return
end

local cfg = config.for_script("music")
local settings = cfg.auto({
    enabled = false,
    source = "system",
    gain = 1.0,
    attack = 0.65,
    release = 0.18,
})

-- State
local state = {
    sources = {},
    last_refresh = 0,
    status_state = "off",
    status_text = "",
    supported_loopback = true,
}

-- Check process loopback feature support
local sup = audio_in.is_supported and audio_in.is_supported()
if sup and sup.process_loopback ~= nil then
    state.supported_loopback = sup.process_loopback
end

local function refresh_sources()
    if audio_in and audio_in.list_sources then
        state.sources = audio_in.list_sources() or {}
        state.last_refresh = os.clock()
    end
end

-- Apply saved settings to native core
local function apply_settings()
    if not audio_in then return end
    if audio_in.set_enabled then audio_in.set_enabled(settings.enabled == true) end
    if audio_in.set_source then audio_in.set_source(settings.source or "system") end
    if audio_in.set_gain then audio_in.set_gain(settings.gain or 1.0) end
    if audio_in.set_smoothing then audio_in.set_smoothing(settings.attack or 0.65, settings.release or 0.18) end
end

-- Apply on load
apply_settings()
refresh_sources()

-- Periodic status refresh
if events and events.on_render then
    events.on_render(function()
        if not audio_in then return end
        local st, txt = audio_in.get_status()
        state.status_state = st or "off"
        state.status_text = txt or ""
        if audio_in.is_enabled then
            local live_en = (audio_in.is_enabled() == true)
            if live_en ~= (settings.enabled == true) then
                settings.enabled = live_en
                cfg.set("enabled", live_en)
            end
        end
    end)
end

local BAND_LABELS = { "Sub", "Bass", "LoM", "Mid", "HiM", "Pres", "Bril", "Air" }

local function render_spectrum_bars(spec, avail_w)
    -- Clean 8-band spectrum meter without colliding text inside narrow pills
    local bands = 8
    local gap = 4.0
    local bw = math.max(16.0, (avail_w - (bands - 1) * gap) / bands)
    for b = 1, bands do
        local s_idx = (b - 1) * 4 + 1
        local peak = 0.0
        for k = 0, 3 do
            local v = spec[s_idx + k] or 0.0
            if v > peak then peak = v end
        end
        peak = math.max(0.0, math.min(1.0, peak))
        ui.progress_bar(peak, bw, 8, "")
        if b < bands then
            ui.same_line(0, gap)
        end
    end
end

local function render_music_window()
    if not audio_in then
        ui.card("Error", nil, function()
            ui.text_wrapped(T("Модуль audio_in не найден.", "Native audio_in module is missing."), "red")
        end)
        return
    end

    -- 1. Status & Master Controls
    ui.card(T("Управление захватом", "Capture Controls"), nil, function()
        local en_ch, new_en = ui.toggle(T("Включить захват звука##m_en", "Enable Audio Capture##m_en"), settings.enabled == true)
        if en_ch then
            settings.enabled = new_en
            audio_in.set_enabled(new_en)
            cfg.set("enabled", new_en)
        end

        ui.spacing(4)
        local badge_tone = "muted"
        local badge_label = T("Выключено", "Disabled")
        if settings.enabled then
            if state.status_state == "capturing" then
                badge_tone = "green"
                badge_label = T("Активно", "Capturing")
            elseif state.status_state == "waiting" then
                badge_tone = "yellow"
                badge_label = T("Ожидание источника", "Waiting for app")
            elseif state.status_state == "error" then
                badge_tone = "red"
                badge_label = T("Ошибка", "Error")
            end
        end

        ui.badge(badge_label, badge_tone)
        if state.status_text and state.status_text ~= "" then
            ui.same_line(0, 10)
            ui.text_dim(state.status_text)
        end

        if not state.supported_loopback then
            ui.spacing(3)
            ui.text_wrapped(T("Внимание: WASAPI Process Loopback не поддерживается этой версией Windows (требуется Windows 10 2004+). Доступен только системный захват (Whole System).",
                              "Note: WASAPI Process Loopback is not supported on this Windows version (requires Windows 10 2004+). Only whole system loopback is available."), "yellow")
        end
    end)

    -- 2. Audio Source Picker
    ui.card(T("Источник звука", "Audio Source"), T("Выберите приложение или системный звук", "Choose application or whole system"), function()
        local avail_w = ui.avail_width()

        if ui.button(T("Обновить список приложений##m_ref", "Refresh Apps List##m_ref"), avail_w, 26, "ghost") then
            refresh_sources()
        end
        ui.spacing(4)

        local is_system = (settings.source == "system" or settings.source == "" or settings.source == nil)
        local sys_prefix = is_system and "● " or ""
        local sys_label = sys_prefix .. T("Весь системный звук (Whole System)##m_src_sys", "Whole System Audio##m_src_sys")
        if ui.button(sys_label, avail_w, 26, is_system and "primary" or "ghost") and not is_system then
            settings.source = "system"
            audio_in.set_source("system")
            cfg.set("source", "system")
        end

        ui.separator()
        ui.spacing(2)

        if #state.sources == 0 then
            ui.text_dim(T("Активные звуковые приложения не обнаружены (включите музыку и нажмите 'Обновить').",
                          "No active audio apps found (play music and click 'Refresh')."))
        else
            ui.text_dim(T("Обнаруженные приложения со звуком:", "Detected audio applications:"))
            ui.spacing(2)

            for idx, src in ipairs(state.sources) do
                local selected = (not is_system and string.lower(settings.source or "") == string.lower(src.exe or ""))
                local tag = ""
                if src.is_game then
                    tag = " [" .. T("Эта игра", "Game") .. "]"
                elseif src.active then
                    tag = " (" .. T("играет", "active") .. ")"
                end

                local prefix = selected and "● " or ""
                local label = string.format("%s%s (%s)%s##m_src_%d", prefix, src.name or "App", src.exe or "?", tag, idx)
                if ui.button(label, avail_w, 24, selected and "primary" or "ghost") and not selected then
                    settings.source = src.exe
                    audio_in.set_source(src.exe)
                    cfg.set("source", src.exe)
                end
            end
        end
    end)

    -- 3. Tuning & Smoothing Sliders
    ui.card(T("Калибровка и сглаживание", "Tuning & Response"), nil, function()
        local g_ch, new_g = ui.slider_float(T("Чувствительность (Gain)##m_g", "Gain Multiplier##m_g"), settings.gain or 1.0, 0.1, 4.0, "%.2fx")
        if g_ch then
            settings.gain = new_g
            audio_in.set_gain(new_g)
            cfg.set("gain", new_g)
        end

        local a_ch, new_a = ui.slider_float(T("Атака (Attack)##m_a", "Attack Speed##m_a"), settings.attack or 0.65, 0.05, 1.0, "%.2f")
        if a_ch then
            settings.attack = new_a
            audio_in.set_smoothing(new_a, settings.release or 0.18)
            cfg.set("attack", new_a)
        end

        local r_ch, new_r = ui.slider_float(T("Спад (Release)##m_r", "Release Decay##m_r"), settings.release or 0.18, 0.02, 0.8, "%.2f")
        if r_ch then
            settings.release = new_r
            audio_in.set_smoothing(settings.attack or 0.65, new_r)
            cfg.set("release", new_r)
        end
    end)

    -- 4. Live Audio Visualizer & Frequency Meters
    ui.card(T("Монитор частот и бит-пульса", "Frequency & Beat Monitor"), nil, function()
        local lv = audio_in.get_levels and audio_in.get_levels() or { level = 0, bass = 0, mid = 0, treble = 0, beat = 0 }
        local spec = audio_in.get_spectrum and audio_in.get_spectrum() or {}
        local avail_w = ui.avail_width()

        render_spectrum_bars(spec, avail_w)

        ui.spacing(4)
        ui.stat_row({
            { label = T("Бас", "Bass"),    value = string.format("%.0f%%", (lv.bass or 0.0) * 100),   tone = "accent" },
            { label = T("СЧ", "Mid"),      value = string.format("%.0f%%", (lv.mid or 0.0) * 100),    tone = "default" },
            { label = T("ВЧ", "Treble"),   value = string.format("%.0f%%", (lv.treble or 0.0) * 100), tone = "default" },
            { label = T("Бит", "Beat"),    value = string.format("%.0f%%", (lv.beat or 0.0) * 100),   tone = (lv.beat or 0.0) > 0.35 and "green" or "muted" },
        })

        ui.spacing(4)
        ui.text_dim(T("Общий уровень (RMS)", "Overall RMS Level"))
        ui.progress_bar(lv.level or 0.0, avail_w, 12, string.format("%.0f%%", (lv.level or 0.0) * 100))

        ui.spacing(2)
        local col_w = (avail_w - 12.0) / 3.0
        ui.progress_bar(lv.bass or 0.0, col_w, 10, string.format(T("Бас %.0f%%", "Bass %.0f%%"), (lv.bass or 0.0) * 100))
        ui.same_line(0, 6)
        ui.progress_bar(lv.mid or 0.0, col_w, 10, string.format(T("СЧ %.0f%%", "Mid %.0f%%"), (lv.mid or 0.0) * 100))
        ui.same_line(0, 6)
        ui.progress_bar(lv.treble or 0.0, col_w, 10, string.format(T("ВЧ %.0f%%", "High %.0f%%"), (lv.treble or 0.0) * 100))

        ui.spacing(2)
        ui.progress_bar(lv.beat or 0.0, avail_w, 12, string.format(T("Бит-пульс: %.0f%%", "Beat Pulse: %.0f%%"), (lv.beat or 0.0) * 100))
    end)

    -- 5. CarFX Quick Action
    if carfx and carfx.load then
        ui.card(T("Шейдер ливреи CarFX", "CarFX Music Livery"), nil, function()
            local bw = (ui.avail_width() - 6.0) * 0.5
            if ui.button(T("На кузов (livery_music)##m_apply_body", "Load on Body##m_apply_body"), bw, 28, "primary") then
                local ok = carfx.load("livery_music.hlsl", 0)
                if ok then
                    if carfx.set_enabled then carfx.set_enabled(true) end
                    if carfx.set_target_enabled then carfx.set_target_enabled(0, true) end
                    if config then
                        config.set("carfx", "body_enabled", true)
                        config.set("carfx", "selected_shader", "livery_music.hlsl")
                    end
                end
            end
            ui.same_line(0, 6)
            if ui.button(T("На салон/детали##m_apply_int", "Load on Internals##m_apply_int"), bw, 28, "ghost") then
                local ok = carfx.load("livery_music.hlsl", 1)
                if ok then
                    if carfx.set_enabled then carfx.set_enabled(true) end
                    if carfx.set_target_enabled then carfx.set_target_enabled(1, true) end
                    if config then
                        config.set("carfx", "internals_enabled", true)
                        config.set("carfx", "internals_shader", "livery_music.hlsl")
                    end
                end
            end
        end)
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "music",
        title = T("Музыкальная ливрея", "Music Reactive"),
        icon = "hud",
        order = 15,
        default_open = false,
        default_pinned = false,
        x = 380,
        y = 50,
        width = 440,
        settings = settings,
        render = render_music_window,
    })
end

log.info("Music-reactive livery module loaded.")
