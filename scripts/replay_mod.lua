-- scripts/replay_mod.lua
local T = i18n.t

local state = {
    ui_tab = 1,
    mode = "IDLE",
    playback_type = "GHOST",
    playback_time = 0.0,
    playback_speed = 1.0,
    playback_loop = true,
    playback_offset = 0.0,
    start_delay = 3.0,
    countdown = 0.0,
    selected_replay_idx = 1,
    saved_replays = {},
    selected_cars = {},
    custom_filename = "drift_run",
    status_msg = "",
    status_tone = "muted",
    target_car_idx = 1,
    active_ghost_ptr = nil,
    frozen_car_ptr = nil,
    using_ghost_clone = false,
    ghost_cfg = {
        enabled = false,
        anti_wrong_way = true,
        min_speed_kmh = 10.0,
        dot_threshold = -0.25,
        max_distance = 120.0,
        show_hud_alert = true,
    },
}

local ghost_settings = {
    enabled = true,
    anti_wrong_way = true,
    show_hud_alert = true,
    min_speed_kmh = 10.0,
    dot_threshold = -0.25,
    max_distance = 120.0,
}
if config and config.bind then
    ghost_settings = config.bind("anticollision", ghost_settings)
end
state.ghost_cfg = ghost_settings

local replay_settings = {
    playback_loop = false,
    ghost_collision = false,
    inherit_carfx = true,
}
if config and config.bind then
    replay_settings = config.bind("replay", replay_settings)
end
state.playback_loop = replay_settings.playback_loop == true

local function sync_selective_ghost_cfg()
    if carx.set_selective_ghost_config then
        local synced = carx.set_selective_ghost_config(ghost_settings)
        if type(synced) == "table" then
            for k, v in pairs(synced) do
                ghost_settings[k] = v
            end
        end
    end
end
sync_selective_ghost_cfg()

if replay_settings.ghost_collision ~= nil and carx and carx.set_ghost_collision then
    carx.set_ghost_collision(replay_settings.ghost_collision)
end
if replay_settings.inherit_carfx ~= nil and replay and replay.set_ghost_mirror_carfx then
    replay.set_ghost_mirror_carfx(replay_settings.inherit_carfx)
end

local function safe_get_cars()
    if carx.get_cars then
        local c = carx.get_cars()
        if c and #c > 0 then return c end
    end
    if carx.get_all_cars then
        local c = carx.get_all_cars()
        if c and #c > 0 then return c end
    end
    return {}
end

local function get_player_list()
    local list = (carx.get_player_list and carx.get_player_list()) or safe_get_cars()
    for _, car in ipairs(list) do
        local key = tostring(car.ptr)
        if state.selected_cars[key] == nil then
            state.selected_cars[key] = (car.is_local_player == true)
        end
    end
    return list
end

local function refresh_replays()
    state.saved_replays = replay.list_saved() or {}
    if #state.saved_replays > 0 and state.selected_replay_idx > #state.saved_replays then
        state.selected_replay_idx = 1
    end
    log.info(string.format("Found replay files: %d", #state.saved_replays))
end
refresh_replays()

local function apply_ghost_time(t)
    if replay.apply_all_to_ghosts then
        replay.apply_all_to_ghosts(t, state.playback_offset)
    elseif replay.apply_to_ghost then
        replay.apply_to_ghost(t, state.playback_offset)
    end
end

local function start_recording()
    local cars = get_player_list()
    if #cars == 0 then
        state.status_msg = T("На трассе нет машин.", "No active car on track.")
        state.status_tone = "yellow"
        return
    end

    local targets = {}
    for _, car in ipairs(cars) do
        local key = tostring(car.ptr)
        if state.selected_cars[key] then
            table.insert(targets, {
                ptr = car.ptr,
                unity_name = car.unity_name or (car.is_local_player and "Player Car" or "Car"),
                is_local_player = car.is_local_player == true,
            })
        end
    end

    if #targets == 0 then
        local player = carx.get_player_car()
        if player and player.ptr then
            state.selected_cars[tostring(player.ptr)] = true
            table.insert(targets, {
                ptr = player.ptr,
                unity_name = player.unity_name or "Player Car",
                is_local_player = true,
            })
        end
    end

    local ok = false
    if replay.start_record_multi and #targets > 0 then
        ok = replay.start_record_multi("CarX Track", targets)
    else
        ok = replay.start_record("CarX Track", "Player Car")
    end

    if ok then
        state.ui_tab = 1
        state.mode = "RECORDING"
        state.status_msg = ""
    else
        state.status_msg = T("Ошибка старта записи.", "Failed to start recording.")
        state.status_tone = "red"
    end
end

local function stop_recording()
    if replay.is_recording() then
        local fname = state.custom_filename
        if not fname or fname == "" then
            fname = "drift_" .. os.date("%Y%m%d_%H%M%S")
        end
        replay.stop_record(fname)
        state.mode = "IDLE"
        state.status_msg = T("Сохранено: ", "Saved: ") .. fname .. ".cxr2"
        state.status_tone = "green"
        refresh_replays()
    end
end

local function stop_playback()
    if state.frozen_car_ptr then
        if carx.set_car_active then
            carx.set_car_active(state.frozen_car_ptr, true)
        end
        state.frozen_car_ptr = nil
    end

    if state.using_ghost_clone then
        if carx.destroy_ghost then
            carx.destroy_ghost()
        end
        state.using_ghost_clone = false
    end

    if state.active_ghost_ptr then
        if carx.destroy_car then
            carx.destroy_car(state.active_ghost_ptr)
        end
        state.active_ghost_ptr = nil
    end

    state.mode = "IDLE"
    state.playback_time = 0.0
    state.countdown = 0.0
    state.status_msg = ""
end

local function start_playback()
    if #state.saved_replays == 0 then
        refresh_replays()
    end
    if #state.saved_replays == 0 then
        state.status_msg = T("Нет сохранённых реплеев.", "No saved replays found.")
        state.status_tone = "yellow"
        return
    end

    local item = state.saved_replays[state.selected_replay_idx] or state.saved_replays[1]
    if not item then return end

    replay.load(item.filename)
    if not (replay.is_loaded() and replay.get_duration() > 0) then
        state.status_msg = T("Ошибка загрузки: ", "Load failed: ") .. item.filename
        state.status_tone = "red"
        return
    end

    if carx.set_ghost_mirror then
        carx.set_ghost_mirror(false)
    end

    local track_count = (replay.get_track_count and replay.get_track_count()) or 1

    if state.playback_type == "GHOST" then
        if replay.apply_all_to_ghosts or (carx.spawn_ghost and replay.apply_to_ghost) then
            if track_count == 1 and carx.spawn_ghost then
                carx.spawn_ghost()
            end
            state.using_ghost_clone = true
            apply_ghost_time(0.0)
        else
            local cars = safe_get_cars()
            if #cars > 1 then
                state.target_car_idx = 2
            elseif carx.spawn_car then
                state.active_ghost_ptr = carx.spawn_car()
            end
        end
    else
        local cars = safe_get_cars()
        if #cars > 1 then
            state.target_car_idx = 2
        elseif carx.spawn_car then
            state.active_ghost_ptr = carx.spawn_car()
        end
    end

    state.ui_tab = 2
    state.mode = "PLAYING"
    state.countdown = state.start_delay
    state.playback_time = 0.0
    state.status_msg = ""
end

local function toggle_pause()
    if state.mode == "PLAYING" then
        state.mode = "PAUSED"
    elseif state.mode == "PAUSED" then
        state.mode = "PLAYING"
    end
end

if keybinds and keybinds.register then
    keybinds.register("replay_record", { label = T("Старт / стоп записи", "Start / stop record"), mod_name = T("3D Реплеи", "3D Replays"), order = 1 })
    keybinds.register("replay_ghost",  { label = T("Старт / стоп призрака", "Start / stop ghost"), mod_name = T("3D Реплеи", "3D Replays"), order = 2 })
    keybinds.register("dummy_ghost",   { label = T("3D-болванка", "3D Dummy"),                     mod_name = T("3D Реплеи", "3D Replays"), order = 3 })
    keybinds.register("anticollision", { label = T("Анти-встречка", "Anti-Wrong-Way"),             mod_name = T("Анти-встречка", "Anti-Wrong-Way"), order = 1 })
end

events.on_key(function(key, is_down)
    if not is_down then return end
    if imgui and imgui.is_any_item_active and imgui.is_any_item_active() then return end

    -- Fallback raw keys if keybinds module is not loaded
    if not keybinds then
        if key == 0x75 then
            if state.mode == "RECORDING" then stop_recording() else start_recording() end
        elseif key == 0x76 then
            if state.mode == "PLAYING" or state.mode == "PAUSED" then stop_playback() else start_playback() end
        elseif key == 0x4A then
            if carx.set_selective_ghost_config then
                state.ghost_cfg.enabled = not state.ghost_cfg.enabled
                sync_selective_ghost_cfg()
            end
        end
    end
end)

local register_tick = events.on_main_update or events.on_update
register_tick(function(dt)
    if not dt or dt <= 0.0 or dt > 0.25 then dt = 0.016 end

    -- Check unified keybinds (Keyboard + Gamepad)
    if keybinds and keybinds.is_pressed then
        if keybinds.is_pressed("replay_record") then
            if state.mode == "RECORDING" then
                stop_recording()
            else
                start_recording()
            end
        end

        if keybinds.is_pressed("replay_ghost") then
            if state.mode == "PLAYING" or state.mode == "PAUSED" then
                stop_playback()
            else
                start_playback()
            end
        end

        if keybinds.is_pressed("anticollision") then
            if carx.set_selective_ghost_config then
                state.ghost_cfg.enabled = not state.ghost_cfg.enabled
                sync_selective_ghost_cfg()
            end
        end
    end

    if state.mode == "RECORDING" then
        if replay.record_all_frames then
            replay.record_all_frames()
        else
            local player = carx.get_player_car()
            if player and player.matrix then
                replay.record_frame()
            end
        end
    elseif state.mode == "PLAYING" then
        if state.countdown > 0 then
            state.countdown = state.countdown - dt
            if state.using_ghost_clone then
                apply_ghost_time(0.0)
            end
        else
            state.playback_time = state.playback_time + (dt * state.playback_speed)
            local max_time = replay.get_duration()

            if state.playback_time >= max_time then
                if state.playback_loop then
                    state.playback_time = 0.0
                    state.countdown = state.start_delay
                    if state.using_ghost_clone then
                        apply_ghost_time(0.0)
                    end
                    return
                else
                    stop_playback()
                    return
                end
            end

            if state.using_ghost_clone then
                apply_ghost_time(state.playback_time)
            else
                local frame = replay.get_frame(state.playback_time)
                if frame and frame.matrix then
                    local car_target_ptr = state.active_ghost_ptr
                    if not car_target_ptr then
                        local cars = safe_get_cars()
                        if #cars > 1 then
                            local target = cars[state.target_car_idx] or cars[2]
                            if target then car_target_ptr = target.ptr end
                        end
                    end

                    if car_target_ptr then
                        carx.set_car_render_matrix(car_target_ptr, frame.matrix)
                        if carx.set_car_rpm and frame.rpm then
                            carx.set_car_rpm(car_target_ptr, frame.rpm)
                        end
                        if carx.set_car_gear and frame.gear then
                            carx.set_car_gear(car_target_ptr, frame.gear)
                        end
                    end
                end
            end
        end
    end
end)

local function draw_ghost_radar()
    if not state.ghost_cfg.enabled or not state.ghost_cfg.show_hud_alert then return end
    if not (carx.is_in_session and carx.is_in_session()) then return end

    local cars = get_player_list()
    local active_ghosts = {}
    for _, c in ipairs(cars) do
        if not c.is_local_player and c.is_selectively_ghosted then
            local d = c.dist_to_player or 0.0
            if d <= 150.0 or c.ghost_mode == 1 then
                table.insert(active_ghosts, c)
            end
        end
    end
    if #active_ghosts == 0 then return end

    if imgui and imgui.set_next_window_size then
        imgui.set_next_window_size(310, 0, 2)
    end
    local rOpen, keepRadar = imgui.begin_window(T("Радар встречки [J]##ww_radar", "Wrong-Way Radar [J]##ww_radar"), true)
    if rOpen then
        ui.badge(string.format(T("ПРИЗРАК (%d)", "GHOST (%d)"), #active_ghosts), "accent")
        ui.same_line(0, 8)
        ui.text_dim(T("Коллизия отключена", "Collision disabled"))
        ui.spacing(4)
        for _, c in ipairs(active_ghosts) do
            local reason = c.is_wrong_way and T("ВСТРЕЧКА", "WRONG-WAY") or (c.ghost_mode == 1 and T("РУЧН.", "MANUAL") or T("ЗОНА", "ZONE"))
            local tone = c.is_wrong_way and "red" or "yellow"
            ui.badge(reason, tone)
            ui.same_line(0, 8)
            local d_name = (c.player_name and c.player_name ~= "") and c.player_name or (c.name or (c.unity_name or "Player"))
            ui.text(string.format(T("%s · %.0fм · %.0f км/ч", "%s · %.0fm · %.0f km/h"),
                d_name, c.dist_to_player or 0.0, c.speed_kmh or 0.0))
        end
    end
    imgui.end_window()
    if keepRadar == false then
        state.ghost_cfg.show_hud_alert = false
        sync_selective_ghost_cfg()
    end
end

events.on_render(function()
    draw_ghost_radar()
end)

local function render_replay_window()
    local tab_ch, new_tab = ui.segmented("rep_tab_seg", state.ui_tab, T({ "Запись [F6]", "Повтор [F7]" }, { "Record [F6]", "Playback [F7]" }))
    if tab_ch then
        state.ui_tab = new_tab
    end
    ui.spacing(4)

    if state.ui_tab == 1 then
        if state.mode == "RECORDING" then
            ui.card(T("Идет запись", "Recording"), nil, function()
                local rec_tracks = (replay.get_record_track_count and replay.get_record_track_count()) or 1
                ui.badge("REC", "red")
                ui.spacing(4)

                ui.stat_row({
                    { label = T("Авто", "Cars"), value = tostring(rec_tracks), tone = "accent" },
                    { label = T("Время", "Time"), value = string.format(T("%.1f с", "%.1f s"), replay.get_record_duration()), tone = "default" },
                    { label = T("Кадры", "Frames"), value = tostring(replay.get_record_frame_count()), tone = "muted" },
                })

                local rec_key_name = (keybinds and keybinds.get and keybinds.get("replay_record").name) or "F6"
                ui.spacing(4)
                if ui.button(string.format(T("Остановить и сохранить [%s]##rep_rec_stop", "Stop & Save [%s]##rep_rec_stop"), rec_key_name), ui.avail_width(), 32, "danger") then
                    stop_recording()
                end
            end)
        else
            ui.card(T("Новая запись", "New Recording"), nil, function()
                local changed, new_name = ui.input_text(T("Имя файла##rep_fname", "File Name##rep_fname"), state.custom_filename, 128)
                if changed then state.custom_filename = new_name end

                local rec_key_name = (keybinds and keybinds.get and keybinds.get("replay_record").name) or "F6"
                ui.spacing(4)
                if ui.button(string.format(T("Начать запись [%s]##rep_rec_start", "Start Recording [%s]##rep_rec_start"), rec_key_name), ui.avail_width(), 32, "primary") then
                    start_recording()
                end

                if keybinds and keybinds.render then
                    ui.spacing(2)
                    keybinds.render("rep_rec_hk", T("Кнопка записи", "Record Button"), "replay_record")
                end
            end)

            local cars = get_player_list()
            local local_car = nil
            for _, c in ipairs(cars) do
                if c.is_local_player then local_car = c; break end
            end

            ui.card(T("Машины в записи", "Track Cars"), string.format("%d", #cars), function()
                local bw = (ui.avail_width() - 8) * 0.5
                if ui.button(string.format(T("Все (%d)##rep_all", "All (%d)##rep_all"), #cars), bw, 26) then
                    for _, c in ipairs(cars) do
                        state.selected_cars[tostring(c.ptr)] = true
                    end
                end
                ui.same_line(0, 8)
                if ui.button(T("Только моя##rep_me", "Only Mine##rep_me"), bw, 26, "ghost") then
                    for _, c in ipairs(cars) do
                        state.selected_cars[tostring(c.ptr)] = (c.is_local_player == true)
                    end
                end

                ui.spacing(4)
                if #cars == 0 then
                    ui.text_dim(T("Нет активных машин на трассе.", "No active cars on track."))
                else
                    for idx, c in ipairs(cars) do
                        local key = tostring(c.ptr)
                        local checked = state.selected_cars[key] == true
                        local dist_str = ""
                        if local_car and not c.is_local_player and c.x and local_car.x then
                            local dx, dy, dz = c.x - local_car.x, c.y - local_car.y, c.z - local_car.z
                            dist_str = string.format(T("%.0fм · ", "%.0fm · "), math.sqrt(dx * dx + dy * dy + dz * dz))
                        end
                        local p_name = (c.player_name and c.player_name ~= "") and c.player_name or c.name
                        local name = (p_name and p_name ~= "") and p_name or (c.unity_name or ("Car #" .. tostring(idx)))
                        local tag = c.is_local_player and T(" [ВЫ]", " [YOU]") or ""
                        if c.is_local_player and (name == T("Вы", "You") or name == "[ВЫ]" or name == "[YOU]") then
                            tag = ""
                        end
                        local label = string.format(T("#%d %s%s (%s%.0f км/ч)##rec_car_%d", "#%d %s%s (%s%.0f km/h)##rec_car_%d"),
                            idx, name, tag, dist_str, c.speed_kmh or 0.0, idx)
                        local cb_ch, cb_val = ui.toggle(label, checked)
                        if cb_ch then
                            state.selected_cars[key] = cb_val
                        end
                    end
                end
            end)
        end
    else
        local filenames = {}
        for _, r in ipairs(state.saved_replays) do
            table.insert(filenames, r.filename)
        end

        ui.card(T("Файл реплея", "Replay File"), nil, function()
            if #filenames > 0 then
                local c_ch, new_idx = ui.combo(T("Выбор .cxr2##rep_file", "Select .cxr2##rep_file"), state.selected_replay_idx, filenames)
                if c_ch then
                    state.selected_replay_idx = new_idx
                    replay.load(filenames[new_idx])
                end
                local sel = state.saved_replays[state.selected_replay_idx]
                if sel then
                    ui.spacing(4)
                    ui.stat_row({
                        { label = T("Авто", "Car"), value = sel.car or "?", tone = "default" },
                        { label = T("Треков", "Tracks"), value = tostring(sel.tracks or 1), tone = "accent" },
                        { label = T("Длина", "Length"), value = string.format(T("%.1f с", "%.1f s"), sel.duration or 0.0), tone = "muted" },
                    })
                    if sel.carfx_shader and sel.carfx_shader ~= "" then
                        ui.spacing(4)
                        ui.badge("CarFX", "accent")
                        ui.same_line(0, 8)
                        ui.text(sel.carfx_shader)
                    end
                end
            else
                ui.text_wrapped(T("Папка replays/ пуста.", "No replays in replays/ folder."), "yellow")
            end

            ui.spacing(4)
            if ui.button(T("Обновить список##rep_refresh", "Refresh List##rep_refresh"), ui.avail_width(), 26, "ghost") then
                refresh_replays()
            end
        end)

        ui.card(T("Воспроизведение", "Playback"), nil, function()
            local bw = (ui.avail_width() - 8) * 0.5
            if state.mode == "PLAYING" then
                if ui.button(T("Пауза##rep_pause", "Pause##rep_pause"), bw, 32) then
                    toggle_pause()
                end
                ui.same_line(0, 8)
                if ui.button(T("Стоп [F7]##rep_stop", "Stop [F7]##rep_stop"), bw, 32, "danger") then
                    stop_playback()
                end
            elseif state.mode == "PAUSED" then
                if ui.button(T("Продолжить##rep_res", "Resume##rep_res"), bw, 32, "primary") then
                    toggle_pause()
                end
                ui.same_line(0, 8)
                if ui.button(T("Стоп [F7]##rep_stop2", "Stop [F7]##rep_stop2"), bw, 32, "danger") then
                    stop_playback()
                end
            else
                if ui.button(T("Воспроизвести [F7]##rep_play", "Play Ghost [F7]##rep_play"), ui.avail_width(), 32, "primary") then
                    start_playback()
                end
            end

            local max_time = replay.get_duration()
            if max_time > 0 then
                ui.spacing(4)
                local fmt_tl = string.format("%.2f / %.2f %s", state.playback_time, max_time, T("с", "s"))
                local sl_ch, new_time = ui.slider_float(T("Таймлайн##rep_tl", "Timeline##rep_tl"), state.playback_time, 0.0, max_time, fmt_tl)
                if sl_ch then
                    state.playback_time = new_time
                    if state.mode == "PAUSED" and state.using_ghost_clone then
                        apply_ghost_time(state.playback_time)
                    end
                end
            end

            if state.mode == "PLAYING" and state.countdown > 0 then
                ui.spacing(4)
                ui.badge(T("СТАРТ", "START"), "yellow")
                ui.same_line(0, 8)
                ui.text(string.format(T("%.1f сек — займите позицию", "%.1fs — take position"), state.countdown))
            end

            ui.spacing(4)
            if carx.is_ghost_collision and carx.set_ghost_collision then
                local col_cur = carx.is_ghost_collision()
                local col_ch, col_new = ui.toggle(T("Коллизия призрака [K]##rep_col", "Ghost Collision [K]##rep_col"), col_cur)
                if col_ch then
                    carx.set_ghost_collision(col_new)
                    replay_settings.ghost_collision = col_new
                end
            end

            local lp_ch, new_lp = ui.toggle(T("Зациклить (Loop)##rep_loop", "Loop Playback##rep_loop"), state.playback_loop)
            if lp_ch then
                state.playback_loop = new_lp
                replay_settings.playback_loop = new_lp
            end

            local sp_ch, new_sp = ui.slider_float(T("Скорость##rep_spd", "Speed##rep_spd"), state.playback_speed, 0.25, 2.0, "%.2fx")
            if sp_ch then state.playback_speed = new_sp end

            local off_ch, new_off = ui.slider_float(T("Смещение вбок##rep_off", "Lateral Offset##rep_off"), state.playback_offset, -8.0, 8.0, T("%+.1f м", "%+.1f m"))
            if off_ch then state.playback_offset = new_off end

            local del_ch, new_del = ui.slider_float(T("Задержка старта##rep_del", "Start Delay##rep_del"), state.start_delay, 0.0, 10.0, T("%.1f с", "%.1f s"))
            if del_ch then state.start_delay = new_del end

            if ui.collapsing(T("Дополнительно##rep_adv", "Advanced##rep_adv"), false) then
                if replay.get_ghost_mirror_carfx and replay.set_ghost_mirror_carfx then
                    local mir_cur = replay.get_ghost_mirror_carfx()
                    local m_ch, m_val = ui.toggle(T("Наследовать CarFX (если не записан)##rep_cfx_mir", "Use Active CarFX if Unsaved##rep_cfx_mir"), mir_cur)
                    if m_ch then
                        replay.set_ghost_mirror_carfx(m_val)
                        replay_settings.inherit_carfx = m_val
                    end
                end

                ui.text_dim(T("Режим спавна", "Spawn Method"))
                local ptype_idx = (state.playback_type == "GHOST") and 1 or 2
                local pt_ch, new_pt = ui.segmented("rep_ptype_seg", ptype_idx, T({ "3D-Призрак", "Слот авто" }, { "3D Ghost", "Car Slot" }))
                if pt_ch then
                    state.playback_type = (new_pt == 1) and "GHOST" or "LEGACY"
                end

                if replay.is_loaded() and replay.get_tracks then
                    local tracks = replay.get_tracks()
                    if tracks and #tracks > 1 then
                        ui.spacing(4)
                        for _, tr in ipairs(tracks) do
                            local badge = tr.is_local and T(" [ВЫ]", " [YOU]") or ""
                            ui.text_dim(string.format("%d. %s%s (%d)", tr.index, tr.car_name or "?", badge, tr.frames or 0))
                        end
                    end
                end
            end
        end)
    end

    if state.status_msg ~= "" then
        ui.text_wrapped(state.status_msg, state.status_tone or "muted")
    end
end

local function render_anticollision_window()
    ui.card(T("Выборочная коллизия", "Selective Collision"), nil, function()
        local anti_name = (keybinds and keybinds.get and keybinds.get("anticollision").name) or "J"
        local en_ch, en_val = ui.toggle(string.format(T("Включить Анти-встречку [%s]##sg_en", "Enable Anti-Wrong-Way [%s]##sg_en"), anti_name), state.ghost_cfg.enabled)
        if en_ch then
            state.ghost_cfg.enabled = en_val
            sync_selective_ghost_cfg()
        end

        if keybinds and keybinds.render then
            keybinds.render("sg_hk_btn", T("Кнопка переключения", "Toggle Button"), "anticollision")
            ui.spacing(2)
        end

        local aww_ch, aww_val = ui.toggle(T("Авто-призрак встречных##sg_aww", "Auto-Ghost Wrong-Way##sg_aww"), state.ghost_cfg.anti_wrong_way)
        if aww_ch then
            state.ghost_cfg.anti_wrong_way = aww_val
            sync_selective_ghost_cfg()
        end

        local hud_ch, hud_val = ui.toggle(T("Радар на экране##sg_hud", "On-Screen Radar##sg_hud"), state.ghost_cfg.show_hud_alert)
        if hud_ch then
            state.ghost_cfg.show_hud_alert = hud_val
            sync_selective_ghost_cfg()
        end

        if ui.collapsing(T("Параметры детектора##sg_det", "Detector Settings##sg_det"), false) then
            local s1, v1 = ui.slider_float(T("Мин. скорость##sg_v", "Min Speed##sg_v"), state.ghost_cfg.min_speed_kmh, 0.0, 60.0, T("%.0f км/ч", "%.0f km/h"))
            if s1 then state.ghost_cfg.min_speed_kmh = v1; sync_selective_ghost_cfg() end

            local s2, v2 = ui.slider_float(T("Порог курса (dot)##sg_dot", "Heading Threshold (dot)##sg_dot"), state.ghost_cfg.dot_threshold, -0.85, 0.0, "%.2f")
            if s2 then state.ghost_cfg.dot_threshold = v2; sync_selective_ghost_cfg() end

            local s3, v3 = ui.slider_float(T("Дистанция (0 = вся карта)##sg_dist", "Max Range (0 = unlimited)##sg_dist"), state.ghost_cfg.max_distance, 0.0, 300.0, T("%.0f м", "%.0f m"))
            if s3 then state.ghost_cfg.max_distance = v3; sync_selective_ghost_cfg() end
        end
    end)

    local cars = get_player_list()
    local other_count = 0
    for _, c in ipairs(cars) do
        if not c.is_local_player then other_count = other_count + 1 end
    end

    ui.card(T("Игроки на трассе", "Track Players"), tostring(other_count), function()
        if other_count == 0 then
            ui.text_dim(T("Других игроков на трассе пока нет.", "No other players on track."))
        else
            if carx.clear_car_ghost_modes then
                if ui.button(T("Сбросить всё в «Авто»##sg_clear", "Reset All to Auto##sg_clear"), ui.avail_width(), 26, "ghost") then
                    carx.clear_car_ghost_modes()
                end
                ui.spacing(4)
            end

            local drawn = 0
            for idx, c in ipairs(cars) do
                if not c.is_local_player then
                    drawn = drawn + 1
                    local mode = c.ghost_mode or 0
                    local dist = c.dist_to_player or 0.0
                    local spd = c.speed_kmh or 0.0
                    local dot = c.heading_dot or 1.0

                    local status_txt = T("ТВЁРДЫЙ", "SOLID")
                    local tone = "green"
                    if not state.ghost_cfg.enabled then
                        status_txt = T("ВЫКЛ", "OFF")
                        tone = "muted"
                    elseif c.is_selectively_ghosted then
                        status_txt = c.is_wrong_way and T("ВСТРЕЧКА", "WRONG-WAY") or T("ПРИЗРАК", "GHOST")
                        tone = "accent"
                    end

                    local dir_tag = (dot < (state.ghost_cfg.dot_threshold or -0.25)) and T("встреч.", "wrong") or T("попутка", "same")
                    local p_name = (c.player_name and c.player_name ~= "") and c.player_name or c.name
                    local car_label = (p_name and p_name ~= "") and p_name or (c.unity_name or ("Car #" .. idx))
                    ui.badge(status_txt, tone)
                    ui.same_line(0, 8)
                    ui.text(string.format(T("%s · %.0fм · %.0f км/ч (%s)", "%s · %.0fm · %.0f km/h (%s)"),
                        car_label, dist, spd, dir_tag))

                    if carx.set_car_ghost_mode then
                        local target_id = c.unity_name or c.ptr
                        local seg_ch, new_m = ui.segmented("sg_car_mode_" .. tostring(idx), mode + 1, T({ "Авто", "Призрак", "Твёрдый" }, { "Auto", "Ghost", "Solid" }))
                        if seg_ch then
                            carx.set_car_ghost_mode(target_id, new_m - 1)
                        end
                    end

                    if drawn < other_count then ui.spacing(4) end
                end
            end
        end
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "replay",
        title = T("Реплеи 3D", "3D Replays"),
        icon = "play",
        order = 30,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 416,
        settings = replay_settings,
        render = render_replay_window,
    })

    urushi.register_window({
        id = "anticollision",
        title = T("Анти-встречка", "Anti-Wrong-Way"),
        icon = "shield",
        order = 40,
        default_open = false,
        default_pinned = false,
        x = 700,
        y = 24,
        width = 412,
        settings = ghost_settings,
        render = render_anticollision_window,
    })
end

log.info("CarX2 Replay, 3D Ghost & Anti-Wrong-Way Mod ready!")
