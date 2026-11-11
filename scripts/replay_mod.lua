-- CarX Drift Racing Online 2 Replay, 3D Ghost & Anti-Wrong-Way Mod
-- Open Source under MIT License

local state = {
    ui_tab = 1, -- 1 = Запись, 2 = Воспроизведение
    mode = "IDLE", -- "IDLE", "RECORDING", "PLAYING", "PAUSED"
    playback_type = "GHOST", -- "GHOST" (3D Model Clone) or "LEGACY"
    playback_time = 0.0,
    playback_speed = 1.0,
    playback_loop = true,
    playback_offset = 0.0, -- смещение вбок (м), 0 = точная траектория для тандема
    start_delay = 3.0,
    countdown = 0.0,
    selected_replay_idx = 1,
    saved_replays = {},
    selected_cars = {}, -- ключ: tostring(car.ptr) -> boolean
    custom_filename = "drift_run",
    status_msg = "Готов к работе (F6 - запись, F7 - 3D-призрак)",
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

local function sync_selective_ghost_cfg()
    if carx.set_selective_ghost_config then
        local synced = carx.set_selective_ghost_config(state.ghost_cfg)
        if type(synced) == "table" then
            state.ghost_cfg = synced
        end
    end
end
sync_selective_ghost_cfg()

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
    log.info(string.format("Найдено файлов реплеев: %d", #state.saved_replays))
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
        state.status_msg = "Внимание: ни один автомобиль не обнаружен на трассе!"
        log.warn("Cannot record: no active car found")
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
        state.status_msg = string.format("Идет запись (%d авто)...", #targets)
        log.info(string.format("Запись заезда начата (выбрано машин: %d).", #targets))
    else
        state.status_msg = "Ошибка старта записи в движке!"
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
        state.status_msg = "Запись сохранена: replays/" .. fname .. ".cxr2"
        refresh_replays()
        log.info("Запись сохранена: " .. fname)
    end
end

local function stop_playback()
    if state.frozen_car_ptr then
        if carx.set_car_active then
            carx.set_car_active(state.frozen_car_ptr, true)
            log.info("Восстановлена физика автомобиля после реплея.")
        end
        state.frozen_car_ptr = nil
    end

    if state.using_ghost_clone then
        if carx.destroy_ghost then
            carx.destroy_ghost()
            log.info("Удалены 3D-призраки (Model Clone).")
        end
        state.using_ghost_clone = false
    end

    if state.active_ghost_ptr then
        if carx.destroy_car then
            carx.destroy_car(state.active_ghost_ptr)
            log.info("Удален Ghost автомобиль.")
        end
        state.active_ghost_ptr = nil
    end

    state.mode = "IDLE"
    state.playback_time = 0.0
    state.countdown = 0.0
    state.status_msg = "Воспроизведение остановлено."
end

local function start_playback()
    if #state.saved_replays == 0 then
        refresh_replays()
    end
    if #state.saved_replays == 0 then
        state.status_msg = "В папке replays/ нет сохраненных записей!"
        return
    end

    local item = state.saved_replays[state.selected_replay_idx] or state.saved_replays[1]
    if not item then return end

    replay.load(item.filename)
    if not (replay.is_loaded() and replay.get_duration() > 0) then
        state.status_msg = "Ошибка загрузки файла реплея: " .. item.filename
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
            log.info(string.format("3D-Призрак активирован (треков в записи: %d).", track_count))
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
    state.status_msg = "Воспроизведение: " .. item.filename
    log.info("Воспроизведение начато: " .. item.filename)
end

local function toggle_pause()
    if state.mode == "PLAYING" then
        state.mode = "PAUSED"
        state.status_msg = "Воспроизведение на паузе."
    elseif state.mode == "PAUSED" then
        state.mode = "PLAYING"
        state.status_msg = "Воспроизведение продолжается."
    end
end

-- Горячие клавиши F6 / F7 / J
events.on_key(function(key, is_down)
    if not is_down then return end
    if imgui and imgui.is_any_item_active and imgui.is_any_item_active() then return end

    if key == 0x75 then -- VK_F6
        if state.mode == "RECORDING" then
            stop_recording()
        else
            start_recording()
        end
    end

    if key == 0x76 then -- VK_F7
        if state.mode == "PLAYING" or state.mode == "PAUSED" then
            stop_playback()
        else
            start_playback()
        end
    end

    if key == 0x4A then -- J: Анти-встречка
        if carx.set_selective_ghost_config then
            state.ghost_cfg.enabled = not state.ghost_cfg.enabled
            sync_selective_ghost_cfg()
            state.status_msg = state.ghost_cfg.enabled and "Анти-встречка (Kino-коллизия): ВКЛ" or "Анти-встречка: ВЫКЛ"
            log.info(state.status_msg)
        end
    end
end)

-- Тик телеметрии на Unity Main Thread
local register_tick = events.on_main_update or events.on_update
register_tick(function(dt)
    if not dt or dt <= 0.0 or dt > 0.25 then dt = 0.016 end

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

-- Компактный HUD-радар активной встречки (появляется на трассе только при обнаружении встречки)
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
    local rOpen, keepRadar = imgui.begin_window("Радар встречки [J]", true)
    if rOpen then
        ui.badge("ПРИЗРАК (" .. tostring(#active_ghosts) .. ")", "accent")
        ui.same_line(0, 8)
        ui.text_dim("Коллизия отключена")
        ui.spacing(4)
        for _, c in ipairs(active_ghosts) do
            local reason = c.is_wrong_way and "ВСТРЕЧКА" or (c.ghost_mode == 1 and "РУЧН." or "ЗОНА")
            local tone = c.is_wrong_way and "red" or "yellow"
            ui.badge(reason, tone)
            ui.same_line(0, 8)
            ui.text(string.format("%s  •  %.0fм  %.0f км/ч",
                c.unity_name or "Игрок", c.dist_to_player or 0.0, c.speed_kmh or 0.0))
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

-- ================= ОКНО 1: РЕПЛЕИ 3D =================

local function render_replay_window()
    local tab_ch, new_tab = ui.segmented("rep_tab_seg", state.ui_tab, { "Запись [F6]", "Воспроизведение [F7]" })
    if tab_ch then
        state.ui_tab = new_tab
    end
    ui.spacing(4)

    if state.ui_tab == 1 then
        if state.mode == "RECORDING" then
            ui.card("Идет запись телеметрии", "Multi-Car v3", function()
                local rec_tracks = (replay.get_record_track_count and replay.get_record_track_count()) or 1
                ui.badge("REC", "red")
                ui.same_line(0, 8)
                ui.text("Запись активна")
                ui.spacing(4)

                ui.stat_row({
                    { label = "Авто", value = tostring(rec_tracks), tone = "accent" },
                    { label = "Время", value = string.format("%.1f с", replay.get_record_duration()), tone = "default" },
                    { label = "Кадры", value = tostring(replay.get_record_frame_count()), tone = "muted" },
                })

                ui.spacing(6)
                if ui.button("Остановить и сохранить [F6]##rep_rec_stop", ui.avail_width(), 32, "danger") then
                    stop_recording()
                end
            end)
        else
            ui.card("Новая запись", "Сохранение в replays/*.cxr2", function()
                local changed, new_name = ui.input_text("Имя файла##rep_fname", state.custom_filename, 128)
                if changed then state.custom_filename = new_name end

                ui.spacing(4)
                if ui.button("Начать запись заезда [F6]##rep_rec_start", ui.avail_width(), 32, "primary") then
                    start_recording()
                end
            end)

            local cars = get_player_list()
            local local_car = nil
            for _, c in ipairs(cars) do
                if c.is_local_player then local_car = c; break end
            end

            ui.card("Список машин на трассе", string.format("Найдено: %d", #cars), function()
                local bw = (ui.avail_width() - 8) * 0.5
                if ui.button("Выбрать всех (" .. tostring(#cars) .. ")##rep_all", bw, 26) then
                    for _, c in ipairs(cars) do
                        state.selected_cars[tostring(c.ptr)] = true
                    end
                end
                ui.same_line(0, 8)
                if ui.button("Только моя##rep_me", bw, 26, "ghost") then
                    for _, c in ipairs(cars) do
                        state.selected_cars[tostring(c.ptr)] = (c.is_local_player == true)
                    end
                end

                ui.spacing(4)
                if #cars == 0 then
                    ui.text_dim("На трассе не найдено активных машин.")
                else
                    for idx, c in ipairs(cars) do
                        local key = tostring(c.ptr)
                        local checked = state.selected_cars[key] == true
                        local dist_str = ""
                        if local_car and not c.is_local_player and c.x and local_car.x then
                            local dx, dy, dz = c.x - local_car.x, c.y - local_car.y, c.z - local_car.z
                            local d = math.sqrt(dx * dx + dy * dy + dz * dz)
                            dist_str = string.format("%.0fм | ", d)
                        end
                        local tag = c.is_local_player and " [ВЫ]" or ""
                        local name = c.unity_name or ("Авто #" .. tostring(idx))
                        local label = string.format("#%d %s%s (%s%.0f км/ч)##rec_car_%d",
                            idx, name, tag, dist_str, c.speed_kmh or 0.0, idx)
                        local cb_ch, cb_val = ui.toggle(label, checked)
                        if cb_ch then
                            state.selected_cars[key] = cb_val
                        end
                    end
                end
            end)

            ui.text_wrapped("v3 Multi-Car: кузов, 4 колеса, суппорты, угол руля и дым.", "muted")
        end
    else
        local filenames = {}
        for _, r in ipairs(state.saved_replays) do
            table.insert(filenames, r.filename)
        end

        ui.card("Выбор реплея", string.format("Файлов: %d", #filenames), function()
            if #filenames > 0 then
                local c_ch, new_idx = ui.combo("Файл реплея##rep_file", state.selected_replay_idx, filenames)
                if c_ch then
                    state.selected_replay_idx = new_idx
                    replay.load(filenames[new_idx])
                end
                local sel = state.saved_replays[state.selected_replay_idx]
                if sel then
                    ui.spacing(4)
                    ui.stat_row({
                        { label = "Авто", value = sel.car or "?", tone = "default" },
                        { label = "Треков", value = tostring(sel.tracks or 1), tone = "accent" },
                        { label = "Длина", value = string.format("%.1f с", sel.duration or 0.0), tone = "muted" },
                    })
                end
            else
                ui.text_wrapped("Нет сохраненных реплеев в папке replays/", "yellow")
            end

            ui.spacing(4)
            if ui.button("Обновить список файлов##rep_refresh", ui.avail_width(), 26, "ghost") then
                refresh_replays()
            end
        end)

        ui.card("Режим и управление", state.mode, function()
            if carx.is_ghost_collision and carx.set_ghost_collision then
                local col_cur = carx.is_ghost_collision()
                local col_ch, col_new = ui.toggle("Коллизия призраков (Твёрдый кузов) [K]##rep_col", col_cur)
                if col_ch then
                    carx.set_ghost_collision(col_new)
                end
            end

            ui.spacing(2)
            ui.text_dim("Режим воспроизведения")
            local ptype_idx = (state.playback_type == "GHOST") and 1 or 2
            local pt_ch, new_pt = ui.segmented("rep_ptype_seg", ptype_idx, { "3D-Призрак", "Слот авто (Legacy)" })
            if pt_ch then
                state.playback_type = (new_pt == 1) and "GHOST" or "LEGACY"
            end

            ui.spacing(6)
            local bw = (ui.avail_width() - 8) * 0.5
            if state.mode == "PLAYING" then
                if ui.button("Пауза##rep_pause", bw, 32) then
                    toggle_pause()
                end
                ui.same_line(0, 8)
                if ui.button("Стоп [F7]##rep_stop", bw, 32, "danger") then
                    stop_playback()
                end
            elseif state.mode == "PAUSED" then
                if ui.button("Продолжить##rep_res", bw, 32, "primary") then
                    toggle_pause()
                end
                ui.same_line(0, 8)
                if ui.button("Стоп [F7]##rep_stop2", bw, 32, "danger") then
                    stop_playback()
                end
            else
                if ui.button("Воспроизвести заезд [F7]##rep_play", ui.avail_width(), 32, "primary") then
                    start_playback()
                end
            end

            local max_time = replay.get_duration()
            if max_time > 0 then
                ui.spacing(6)
                local sl_ch, new_time = ui.slider_float("Таймлайн##rep_tl", state.playback_time, 0.0, max_time, "%.2f с")
                if sl_ch then
                    state.playback_time = new_time
                    if state.mode == "PAUSED" and state.using_ghost_clone then
                        apply_ghost_time(state.playback_time)
                    end
                end
                ui.progress_bar(state.playback_time / math.max(max_time, 0.001), -1, 6)
                ui.text_dim(string.format("Позиция: %.2f / %.2f сек", state.playback_time, max_time))
            end

            if state.mode == "PLAYING" and state.countdown > 0 then
                ui.spacing(4)
                ui.badge("СТАРТ", "yellow")
                ui.same_line(0, 8)
                ui.text(string.format("Через %.1f сек — займите позицию", state.countdown))
            end
        end)

        ui.card("Настройки заезда", "Тайминг и смещение", function()
            local sp_ch, new_sp = ui.slider_float("Скорость##rep_spd", state.playback_speed, 0.25, 2.0, "%.2fx")
            if sp_ch then state.playback_speed = new_sp end

            local off_ch, new_off = ui.slider_float("Смещение вбок (м)##rep_off", state.playback_offset, -8.0, 8.0, "%+.1f м")
            if off_ch then state.playback_offset = new_off end

            local del_ch, new_del = ui.slider_float("Задержка старта##rep_del", state.start_delay, 0.0, 10.0, "%.1f с")
            if del_ch then state.start_delay = new_del end

            local lp_ch, new_lp = ui.toggle("Зациклить воспроизведение (Loop)##rep_loop", state.playback_loop)
            if lp_ch then state.playback_loop = new_lp end

            if replay.is_loaded() and replay.get_tracks then
                local tracks = replay.get_tracks()
                if tracks and #tracks > 1 and ui.collapsing(string.format("Треки в памяти (%d авто)", #tracks), false) then
                    for _, tr in ipairs(tracks) do
                        local badge = tr.is_local and " [ВЫ]" or ""
                        ui.text(string.format("%d. %s%s (%d кадр.)", tr.index, tr.car_name or "?", badge, tr.frames or 0))
                    end
                end
            end
        end)
    end

    ui.text_dim(state.status_msg)
end

-- ================= ОКНО 2: АНТИ-ВСТРЕЧКА (KINO) =================

local function render_anticollision_window()
    ui.card("Защита от встречки", "Выборочная коллизия [J]", function()
        local en_ch, en_val = ui.toggle("Включить выборочную коллизию [J]##sg_en", state.ghost_cfg.enabled)
        if en_ch then
            state.ghost_cfg.enabled = en_val
            sync_selective_ghost_cfg()
        end

        local aww_ch, aww_val = ui.toggle("Авто-призрак для встречного потока##sg_aww", state.ghost_cfg.anti_wrong_way)
        if aww_ch then
            state.ghost_cfg.anti_wrong_way = aww_val
            sync_selective_ghost_cfg()
        end

        local hud_ch, hud_val = ui.toggle("Мини-радар встречки на трассе##sg_hud", state.ghost_cfg.show_hud_alert)
        if hud_ch then
            state.ghost_cfg.show_hud_alert = hud_val
            sync_selective_ghost_cfg()
        end

        ui.text_wrapped("Попутные машины и стены остаются твёрдыми; встречные и выбранные игроки становятся прозрачными.", "muted")

        if ui.collapsing("Параметры детектора встречки", false) then
            local s1, v1 = ui.slider_float("Мин. скорость встречки##sg_v", state.ghost_cfg.min_speed_kmh, 0.0, 60.0, "%.0f км/ч")
            if s1 then state.ghost_cfg.min_speed_kmh = v1; sync_selective_ghost_cfg() end

            local s2, v2 = ui.slider_float("Порог встречного курса (dot)##sg_dot", state.ghost_cfg.dot_threshold, -0.85, 0.0, "%.2f")
            if s2 then state.ghost_cfg.dot_threshold = v2; sync_selective_ghost_cfg() end

            local s3, v3 = ui.slider_float("Дистанция (0 = вся карта)##sg_dist", state.ghost_cfg.max_distance, 0.0, 300.0, "%.0f м")
            if s3 then state.ghost_cfg.max_distance = v3; sync_selective_ghost_cfg() end

            ui.text_dim("dot = -0.25 (~105° навстречу). Защита перекрытия: <6м до разъезда.")
        end
    end)

    local cars = get_player_list()
    local other_count = 0
    for _, c in ipairs(cars) do
        if not c.is_local_player then other_count = other_count + 1 end
    end

    ui.card("Игроки на трассе", string.format("Соперников: %d", other_count), function()
        if carx.clear_car_ghost_modes and other_count > 0 then
            if ui.button("Сбросить всё в «Авто»##sg_clear", ui.avail_width(), 26, "ghost") then
                carx.clear_car_ghost_modes()
            end
            ui.spacing(4)
        end

        if other_count == 0 then
            ui.text_dim("Других игроков на трассе пока нет.")
        else
            local drawn = 0
            for idx, c in ipairs(cars) do
                if not c.is_local_player then
                    drawn = drawn + 1
                    local mode = c.ghost_mode or 0
                    local dist = c.dist_to_player or 0.0
                    local spd = c.speed_kmh or 0.0
                    local dot = c.heading_dot or 1.0

                    local status_txt = "ТВЁРДЫЙ"
                    local tone = "green"
                    if not state.ghost_cfg.enabled then
                        status_txt = "ВЫКЛ"
                        tone = "muted"
                    elseif c.is_selectively_ghosted then
                        status_txt = c.is_wrong_way and "ВСТРЕЧКА" or "ПРИЗРАК"
                        tone = "accent"
                    end

                    local dir_tag = (dot < (state.ghost_cfg.dot_threshold or -0.25)) and "встречка" or "попутка"
                    ui.badge(status_txt, tone)
                    ui.same_line(0, 8)
                    ui.text(string.format("%s  •  %.0fм  %.0f км/ч (%s)",
                        c.unity_name or ("Авто #" .. idx), dist, spd, dir_tag))

                    if carx.set_car_ghost_mode then
                        local target_id = c.unity_name or c.ptr
                        local seg_ch, new_m = ui.segmented("sg_car_mode_" .. tostring(idx), mode + 1, { "Авто", "Призрак", "Твёрдый" })
                        if seg_ch then
                            carx.set_car_ghost_mode(target_id, new_m - 1)
                        end
                    end

                    if drawn < other_count then ui.spacing(4) end
                end
            end
        end
    end)

    ui.text_dim("Хоткей: [J] переключить выборочную коллизию в один клик")
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "replay",
        title = "Реплеи 3D",
        icon = "play",
        order = 30,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 430,
        render = render_replay_window,
    })

    urushi.register_window({
        id = "anticollision",
        title = "Анти-встречка",
        icon = "shield",
        order = 40,
        default_open = false,
        default_pinned = false,
        x = 710,
        y = 24,
        width = 420,
        render = render_anticollision_window,
    })
end

log.info("CarX2 Replay, 3D Ghost & Anti-Wrong-Way Mod ready!")
