-- scripts/initiald_mod.lua
-- URUSHI Initial D Mode: Anime speed lines + cel toon look + drift-reactive music
local T = i18n.t

local settings = {
    enabled = false,
    test_preview = false,       -- manual test preview toggle
    angle_thresh = 25.0,        -- deg
    min_speed = 35.0,           -- km/h
    fade_in = 0.4,              -- sec
    fade_out = 1.2,             -- sec
    speedline_strength = 0.85,  -- 0..1
    music_file = "",            -- chosen file from audio.list()
    music_volume = 0.8,         -- 0..1
    apply_cel = false,          -- toggle cel body shader
    cel_opacity = 0.85,         -- 0..1 anime overlay opacity
}

if config and config.for_script then
    settings = config.for_script("initiald_mod").bind(settings)
elseif config and config.bind then
    settings = config.bind("initiald", settings)
end

local live_state = {
    intensity = 0.0,
    voice_id = nil,
    music_playing = false,
    cel_loaded = false,
    postfx_loaded = false,
    available_tracks = {},
    last_drift_tick = 0,
    in_drift = false,
}

local function refresh_tracks()
    if audio and audio.list then
        live_state.available_tracks = audio.list() or {}
        if (settings.music_file == "" or settings.music_file == nil) and #live_state.available_tracks > 0 then
            settings.music_file = live_state.available_tracks[1]
        end
    end
end

refresh_tracks()

-- ---------------------------------------------------------------------------------------------
-- Drift & Intensity Logic
-- ---------------------------------------------------------------------------------------------
if events and events.on_update then
    events.on_update(function(dt)
        dt = dt or 0.016
        if dt > 0.1 then dt = 0.1 end

        -- If feature toggle is disabled (and not in test preview)
        if not settings.enabled and not settings.test_preview then
            if live_state.intensity > 0 then
                live_state.intensity = math.max(0.0, live_state.intensity - dt * 2.5)
                if postfx and postfx.set_param then
                    postfx.set_param(0, live_state.intensity * (settings.speedline_strength or 0.85))
                end
                if live_state.voice_id and audio and audio.set_volume then
                    audio.set_volume(live_state.voice_id, live_state.intensity * (settings.music_volume or 0.8))
                end
            end
            if live_state.intensity <= 0.001 then
                if live_state.voice_id and audio and audio.stop then
                    audio.stop(live_state.voice_id)
                    live_state.voice_id = nil
                    live_state.music_playing = false
                end
                if postfx and postfx.set_enabled and postfx.is_enabled and postfx.is_enabled() then
                    postfx.set_param(0, 0.0)
                    postfx.set_enabled(false)
                end
            end
            if live_state.cel_loaded and carfx and carfx.unload then
                carfx.unload(0)
                live_state.cel_loaded = false
            end
            return
        end

        -- Handle Cel Shader on body (target 0)
        if settings.apply_cel then
            if not live_state.cel_loaded and carfx and carfx.load then
                carfx.load("livery_cel.hlsl", 0)
                live_state.cel_loaded = true
            end
            if carfx and carfx.set_param then
                carfx.set_param("inkOpacity", settings.cel_opacity or 0.85)
            end
        else
            if live_state.cel_loaded and carfx and carfx.unload then
                carfx.unload(0)
                live_state.cel_loaded = false
            end
        end

        -- Drift telemetry from carx
        local angle = 0.0
        local speed = 0.0
        if carx and carx.get_drift_info then
            local di = carx.get_drift_info()
            if di then
                angle = math.abs(di.angle_deg or 0.0)
                speed = di.speed_kmh or 0.0
            end
        end

        local thresh = settings.angle_thresh or 25.0
        local min_spd = settings.min_speed or 35.0

        -- Drift detection with hysteresis to smooth weight transfers and transitions
        local drift_active = false
        local now = os.clock()
        if angle >= thresh and speed >= min_spd then
            drift_active = true
            live_state.last_drift_tick = now
        elseif live_state.in_drift then
            -- 450 ms grace window for weight flick / counter-steer switchbacks
            local grace_time = 0.45
            local elapsed = now - live_state.last_drift_tick
            if elapsed < grace_time and speed >= math.max(15.0, min_spd - 10.0) then
                drift_active = true
            elseif angle >= math.max(10.0, thresh - 6.0) and speed >= math.max(15.0, min_spd - 8.0) then
                drift_active = true
                live_state.last_drift_tick = now
            end
        end
        live_state.in_drift = drift_active

        -- Target intensity: 0 when idle, scales up to 1.0 when drifting hard (or test_preview)
        local target = 0.0
        if settings.test_preview then
            target = settings.speedline_strength or 0.85
        elseif drift_active then
            local excess = math.max(0.0, angle - (thresh - 5.0))
            target = math.min(1.0, 0.45 + (excess / 15.0) * 0.55)
        end

        -- Smooth intensity using configurable attack/decay rates
        local fade_in_rate = 1.0 / math.max(settings.fade_in or 0.4, 0.05)
        local fade_out_rate = 1.0 / math.max(settings.fade_out or 1.2, 0.05)

        if target > live_state.intensity then
            live_state.intensity = math.min(target, live_state.intensity + dt * fade_in_rate)
        else
            live_state.intensity = math.max(target, live_state.intensity - dt * fade_out_rate)
        end

        -- Drive PostFX speed lines
        if postfx then
            if live_state.intensity > 0.001 then
                if not live_state.postfx_loaded then
                    postfx.load("speedlines.hlsl")
                    live_state.postfx_loaded = true
                end
                if postfx.set_enabled and not postfx.is_enabled() then
                    postfx.set_enabled(true)
                end
                local strength = settings.speedline_strength or 0.85
                postfx.set_param(0, live_state.intensity * strength)
            else
                if postfx.set_enabled and postfx.is_enabled() then
                    postfx.set_param(0, 0.0)
                    postfx.set_enabled(false)
                end
            end
        end

        -- Drive Audio: track starts when intensity rises from 0, fades out and stops when returning to 0
        if audio then
            local max_vol = settings.music_volume or 0.8
            if live_state.intensity > 0.005 then
                local current_vol = live_state.intensity * max_vol
                if not live_state.voice_id or not audio.is_playing(live_state.voice_id) then
                    if settings.music_file and settings.music_file ~= "" then
                        live_state.voice_id = audio.play(settings.music_file, { loop = true, volume = current_vol })
                        live_state.music_playing = (live_state.voice_id ~= nil and live_state.voice_id > 0)
                    end
                else
                    audio.set_volume(live_state.voice_id, current_vol)
                end
            else
                if live_state.voice_id then
                    if audio.is_playing(live_state.voice_id) then
                        audio.stop(live_state.voice_id)
                    end
                    live_state.voice_id = nil
                    live_state.music_playing = false
                end
            end
        end
    end)
end

-- ---------------------------------------------------------------------------------------------
-- UI Rendering
-- ---------------------------------------------------------------------------------------------
local function render_window()
    ui.card(T("Initial D Режим", "Initial D Mode"), nil, function()
        ui.bind_toggle(T("Включить режим##id_en", "Enable Mode##id_en"), settings, "enabled")

        ui.spacing(4)
        ui.text_wrapped(
            T("Автоматические аниме линии скорости и Eurobeat саундтрек при заносе.",
              "Automatic anime speed lines and Eurobeat music during drift."),
            "muted"
        )

        ui.spacing(6)
        local pct = live_state.intensity
        ui.progress_bar(pct, -1, 14, string.format("Drift Intensity: %.0f%%", pct * 100))
    end)

    ui.card(T("Условия заноса", "Trigger Conditions"), nil, function()
        ui.bind_slider_float(T("Порог угла заноса##id_ang", "Angle Threshold##id_ang"), settings, "angle_thresh", 10.0, 60.0, "%.0f°")
        ui.bind_slider_float(T("Мин. скорость##id_spd", "Min Speed##id_spd"), settings, "min_speed", 10.0, 100.0, "%.0f km/h")
        ui.bind_slider_float(T("Плавность нарастания (Fade In)##id_fi", "Fade In Attack Time##id_fi"), settings, "fade_in", 0.1, 2.0, "%.2f s")
        ui.bind_slider_float(T("Плавность затухания (Fade Out)##id_fo", "Fade Out Release Time##id_fo"), settings, "fade_out", 0.2, 5.0, "%.2f s")
    end)

    ui.card(T("Визуальные эффекты", "Visual Effects"), nil, function()
        ui.bind_toggle(T("Тест линий скорости (Предпросмотр)##id_test", "Test Speed Lines (Preview)##id_test"), settings, "test_preview")
        ui.bind_slider_float(T("Сила линий скорости##id_lines", "Speed Lines Strength##id_lines"), settings, "speedline_strength", 0.0, 1.0, "%.2f")
        ui.bind_toggle(T("Anime / Manga наложение на кузов##id_cel", "Anime / Manga Body Overlay##id_cel"), settings, "apply_cel")
        if settings.apply_cel then
            ui.bind_slider_float(T("Плотность аниме-контуров##id_cel_op", "Anime Contour Opacity##id_cel_op"), settings, "cel_opacity", 0.1, 1.0, "%.2f")
        end
        ui.spacing(4)
        if ui.button(T("Починить кузов (Вернуть детали на место)##id_rep", "Repair Body (Snap Parts Back)##id_rep"), -1, 26, "secondary") then
            if carx and carx.repair_car then
                carx.repair_car()
            end
        end
    end)

    ui.card(T("Музыка (Eurobeat)", "Audio (Eurobeat Track)"), nil, function()
        ui.bind_slider_float(T("Громкость музыки##id_vol", "Music Volume##id_vol"), settings, "music_volume", 0.0, 1.0, "%.2f")

        ui.spacing(4)
        if ui.button(T("Обновить список треков##id_ref", "Refresh Audio List##id_ref"), -1, 26, "secondary") then
            refresh_tracks()
        end

        ui.spacing(4)
        if #live_state.available_tracks == 0 then
            ui.text_wrapped(
                T("В папке urushi_data/audio/ пока нет файлов.\nПоместите свои треки (.mp3 или .wav) в urushi_data/audio/.",
                  "No audio tracks in urushi_data/audio/.\nPlace your own .mp3 or .wav tracks into urushi_data/audio/."),
                "yellow"
            )
        else
            ui.text(T("Выбор трека:", "Select Track:"), "muted")
            for _, trk in ipairs(live_state.available_tracks) do
                local is_selected = (settings.music_file == trk)
                if ui.button((is_selected and "▶ " or "   ") .. trk .. "##id_t_" .. trk, -1, 24, is_selected and "primary" or "ghost") then
                    settings.music_file = trk
                    if config and config.mark_dirty then config.mark_dirty() end
                    if live_state.voice_id and audio and audio.stop then
                        audio.stop(live_state.voice_id)
                        live_state.voice_id = audio.play(trk, { loop = true, volume = live_state.intensity * (settings.music_volume or 0.8) })
                    end
                end
            end
        end
    end)
end

urushi.register_window({
    id = "initiald",
    title = T("Initial D режим", "Initial D Mode"),
    icon = "⚡",
    order = 45,
    default_open = false,
    settings = settings,
    render = render_window,
})
