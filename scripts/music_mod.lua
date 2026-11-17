-- scripts/music_mod.lua
-- URUSHI Music-Reactive Livery Module
local T = i18n.t

if not audio_in then
    log.error("music_mod: native audio_in module is missing")
    return
end

local cfg = config.for_script("music_mod")
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
    end)
end

local function render_spectrum_bars(spec, w, h)
    if not imgui or not imgui.get_window_draw_list then return end
    local dl = imgui.get_window_draw_list()
    local cx, cy = imgui.get_cursor_screen_pos()
    local S = (urushi and urushi.get_scale and urushi.get_scale()) or 1.0

    -- Background frame
    local bg_col = imgui.color_convert_float4_to_u32(0.06, 0.08, 0.11, 0.9)
    local border_col = imgui.color_convert_float4_to_u32(0.2, 0.25, 0.32, 0.4)
    dl:add_rect_filled(cx, cy, cx + w, cy + h, bg_col, 4.0 * S)
    dl:add_rect(cx, cy, cx + w, cy + h, border_col, 4.0 * S)

    local num_bars = 32
    local bar_gap = 2.0 * S
    local total_gaps = (num_bars + 1) * bar_gap
    local bar_w = math.max(1.0, (w - total_gaps) / num_bars)

    for i = 1, num_bars do
        local val = math.max(0.0, math.min(1.0, spec[i] or 0.0))
        local bh = math.max(2.0 * S, val * (h - 6.0 * S))
        local bx = cx + bar_gap + (i - 1) * (bar_w + bar_gap)
        local by = cy + h - 3.0 * S - bh

        -- Color gradient from Cyan (bass) to Magenta (mid) to Golden/White (treble)
        local r = math.min(1.0, (i / num_bars) * 1.5)
        local g = math.max(0.2, 1.0 - math.abs((i - 16) / 16.0) * 0.8)
        local b = math.max(0.3, 1.0 - (i / num_bars))
        local bar_col = imgui.color_convert_float4_to_u32(r, g, b, 0.85 + val * 0.15)

        dl:add_rect_filled(bx, by, bx + bar_w, cy + h - 3.0 * S, bar_col, 2.0 * S)
    end

    ui.dummy(w, h)
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
                badge_tone = "ok"
                badge_label = T("Активно", "Capturing")
            elseif state.status_state == "waiting" then
                badge_tone = "warn"
                badge_label = T("Ожидание источника", "Waiting for app")
            elseif state.status_state == "error" then
                badge_tone = "danger"
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
                              "Note: WASAPI Process Loopback is not supported on this Windows version (requires Windows 10 2004+). Only whole system loopback is available."), "warn")
        end
    end)

    -- 2. Audio Source Picker
    ui.card(T("Источник звука", "Audio Source"), T("Выберите приложение или системный звук", "Choose application or whole system"), function()
        local avail_w = ui.avail_width()

        -- Refresh button
        if ui.button(T("Обновить список приложений##m_ref", "Refresh Apps List##m_ref"), avail_w, 24, "ghost") then
            refresh_sources()
        end
        ui.spacing(4)

        -- "Whole system" option
        local is_system = (settings.source == "system" or settings.source == "" or settings.source == nil)
        local sys_label = T("Весь системный звук (Whole System)##m_src_sys", "Whole System Audio##m_src_sys")
        local sys_ch = ui.selectable(sys_label, is_system)
        if sys_ch and not is_system then
            settings.source = "system"
            audio_in.set_source("system")
            cfg.set("source", "system")
        end

        ui.separator()
        ui.spacing(2)

        -- List running applications
        if #state.sources == 0 then
            ui.text_dim(T("Активные звуковые приложения не обнаружены (включите музыку и нажмите 'Обновить').",
                          "No active audio apps found (play music and click 'Refresh')."))
        else
            ui.text_dim(T("Обнаруженные приложения со звуком:", "Detected audio applications:"))
            ui.spacing(2)

            for idx, src in ipairs(state.sources) do
                local selected = (not is_system and string.lower(settings.source) == string.lower(src.exe))
                local tag = ""
                if src.is_game then
                    tag = " [" .. T("Эта игра", "Game") .. "]"
                elseif src.active then
                    tag = " (" .. T("играет", "active") .. ")"
                end

                local label = string.format("%s (%s)%s##m_src_%d", src.name, src.exe, tag, idx)
                local sel_ch = ui.selectable(label, selected)
                if sel_ch and not selected then
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

        -- 32-band spectrum visualizer canvas
        render_spectrum_bars(spec, avail_w, 54.0)

        ui.spacing(4)

        -- Discrete meters
        ui.text_dim(T("Общий уровень (RMS)", "Overall RMS Level"))
        ui.progress_bar(lv.level or 0.0, avail_w, 14, string.format("%.0f%%", (lv.level or 0.0) * 100))

        ui.spacing(2)
        local col_w = (avail_w - 12) / 3.0

        ui.columns(3)
        ui.text_dim(T("Бас (20-250 Hz)", "Bass (20-250 Hz)"))
        ui.progress_bar(lv.bass or 0.0, col_w, 12, string.format("%.0f%%", (lv.bass or 0.0) * 100))
        ui.next_column()

        ui.text_dim(T("СЧ (250-4k Hz)", "Mids (250-4k Hz)"))
        local mid_val = lv.mid or ((lv.low_mid or 0.0) + (lv.high_mid or 0.0)) * 0.5
        ui.progress_bar(mid_val, col_w, 12, string.format("%.0f%%", mid_val * 100))
        ui.next_column()

        ui.text_dim(T("ВЧ (4k-16k Hz)", "Treble (4k-16k)"))
        ui.progress_bar(lv.treble or 0.0, col_w, 12, string.format("%.0f%%", (lv.treble or 0.0) * 100))
        ui.columns(1)

        ui.spacing(3)
        ui.text_dim(T("Бит-пульс (Kick / Onset Pulse)", "Beat Onset Pulse"))
        ui.progress_bar(lv.beat or 0.0, avail_w, 14, string.format("%.0f%%", (lv.beat or 0.0) * 100))
    end)

    -- 5. CarFX Quick Action
    if carfx and carfx.load then
        ui.card(T("Шейдер ливреи CarFX", "CarFX Music Livery"), nil, function()
            if ui.button(T("Загрузить livery_music.hlsl на кузов##m_apply_hlsl", "Load livery_music.hlsl on Car Body##m_apply_hlsl"), ui.avail_width(), 28, "primary") then
                carfx.load("livery_music.hlsl", 0)
                if carfx.set_enabled then carfx.set_enabled(true) end
            end
        end)
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "music",
        title = T("Музыкальная ливрея", "Music Reactive"),
        icon = "sound",
        order = 36,
        default_open = false,
        default_pinned = false,
        x = 380,
        y = 50,
        width = 440,
        settings = settings,
        render = render_music_window,
    })
end

_G.music_mod = {
    apply = apply_settings,
    refresh_sources = refresh_sources,
}
