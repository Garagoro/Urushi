-- CarX Drift Racing Online 2 Replay & Ghost Mod
-- Open Source under MIT License

local state = {
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
        show_hud_alert = true
    }
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
                is_local_player = car.is_local_player == true
            })
        end
    end

    -- Fallback: если никого не отметили, пишем машину игрока
    if #targets == 0 then
        local player = carx.get_player_car()
        if player and player.ptr then
            state.selected_cars[tostring(player.ptr)] = true
            table.insert(targets, {
                ptr = player.ptr,
                unity_name = player.unity_name or "Player Car",
                is_local_player = true
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
        state.mode = "RECORDING"
        state.status_msg = string.format("[REC] Идет запись (%d авто)...", #targets)
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

-- Hotkeys listener
events.on_key(function(key, is_down)
    if not is_down then return end
    if imgui.is_any_item_active and imgui.is_any_item_active() then return end

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

    if key == 0x4A then -- J: Toggle Selective Player Collision / Anti-Wrong-Way
        if carx.set_selective_ghost_config then
            state.ghost_cfg.enabled = not state.ghost_cfg.enabled
            sync_selective_ghost_cfg()
            state.status_msg = state.ghost_cfg.enabled and "Анти-встречка (Kino-коллизия): ВКЛ" or "Анти-встречка: ВЫКЛ"
            log.info(state.status_msg)
        end
    end
end)

-- Main loop tick: sample telemetry on Unity Main Thread when available
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

    if imgui.set_next_window_size then
        imgui.set_next_window_size(320, 0, 2)
    end
    local rOpen, _ = imgui.begin_window("Kino Ghost Radar [J]", true)
    if rOpen then
        imgui.text_colored(0.35, 0.90, 1.0, 1.0, string.format("Коллизия отключена (%d):", #active_ghosts))
        for _, c in ipairs(active_ghosts) do
            local reason = c.is_wrong_way and "ВСТРЕЧКА" or (c.ghost_mode == 1 and "РУЧН." or "ЗОНА")
            imgui.text_colored(1.0, 0.85, 0.35, 1.0,
                string.format("  [%s] %s (%.0fм | %.0f км/ч)",
                    reason, c.unity_name or "Игрок", c.dist_to_player or 0.0, c.speed_kmh or 0.0))
        end
    end
    imgui.end_window()
end

-- Main UI Window
events.on_render(function()
    draw_ghost_radar()
    if not imgui.is_menu_open() then return end

    if imgui.set_next_window_size then
        imgui.set_next_window_size(530, 560, 2)
    end

    local isOpen, keepOpen = imgui.begin_window("URUSHI // Replay & Ghost Mod [F4]", true)
    if isOpen then
        if updater and updater.get_status then
            local ust = updater.get_status()
            local uver = ust.local_version or "0.1.0"
            imgui.text_colored(0.55, 0.85, 1.0, 1.0, string.format("URUSHI v%s | %s", uver, ust.status_text or ""))
            imgui.same_line()
            if ust.state == "update_available" then
                if imgui.button("Обновить", 95, 22) then
                    updater.install()
                end
            elseif ust.state == "restart_required" then
                if imgui.button("Рестарт", 95, 22) then
                    updater.restart_game()
                end
            elseif ust.state ~= "checking" and ust.state ~= "downloading" and ust.state ~= "reinjecting" then
                if imgui.button("Проверить", 95, 22) then
                    updater.check()
                end
            end
            imgui.separator()
        end
        if imgui.begin_tab_bar("MainTabs") then

            -- ===================== TAB 1: RECORD =====================
            if imgui.begin_tab_item("[REC] Запись заезда") then
                imgui.spacing()

                if state.mode == "RECORDING" then
                    imgui.text_colored(1.0, 0.3, 0.3, 1.0, "[● REC] ИДЕТ ЗАПИСЬ ТЕЛЕМЕТРИИ...")
                    imgui.spacing()
                    local rec_tracks = (replay.get_record_track_count and replay.get_record_track_count()) or 1
                    imgui.text(string.format("Авто в записи:   %d", rec_tracks))
                    imgui.text(string.format("Время заезда:    %.2f сек", replay.get_record_duration()))
                    imgui.text(string.format("Кадров записано: %d", replay.get_record_frame_count()))
                    imgui.spacing()

                    if imgui.button("[■] Остановить и сохранить (F6)", 310, 36) then
                        stop_recording()
                    end
                else
                    imgui.text_colored(0.4, 0.85, 0.45, 1.0, "[ ГОТОВ ] Отметьте нужные машины и нажмите F6")
                    imgui.spacing()

                    local changed, new_name = imgui.input_text("Имя файла", state.custom_filename)
                    if changed then state.custom_filename = new_name end
                    imgui.spacing()

                    if imgui.button("[●] Начать запись заезда (F6)", 310, 36) then
                        start_recording()
                    end

                    imgui.spacing()
                    imgui.separator()
                    imgui.text_colored(0.85, 0.85, 0.35, 1.0, "Список машин на трассе (Player List):")

                    local cars = get_player_list()
                    local local_car = nil
                    for _, c in ipairs(cars) do
                        if c.is_local_player then local_car = c; break end
                    end

                    if imgui.button("Выбрать всех (" .. tostring(#cars) .. ")", 160, 24) then
                        for _, c in ipairs(cars) do
                            state.selected_cars[tostring(c.ptr)] = true
                        end
                    end
                    imgui.same_line()
                    if imgui.button("Только моя машина", 160, 24) then
                        for _, c in ipairs(cars) do
                            state.selected_cars[tostring(c.ptr)] = (c.is_local_player == true)
                        end
                    end

                    imgui.spacing()
                    if #cars == 0 then
                        imgui.text_colored(1.0, 0.5, 0.4, 1.0, "  На трассе не найдено активных машин.")
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
                            local cb_changed, cb_val = imgui.checkbox(label, checked)
                            if cb_changed then
                                state.selected_cars[key] = cb_val
                            end
                        end
                    end
                end

                imgui.spacing()
                imgui.separator()
                imgui.text_colored(0.65, 0.68, 0.75, 1.0, "v3 Запись (Multi-Car): кузов, 4 колеса, суппорты, руль и дым.")
                imgui.end_tab_item()
            end

            -- ===================== TAB 2: PLAYBACK =====================
            if imgui.begin_tab_item("[PLAY] Воспроизведение") then
                imgui.spacing()

                local filenames = {}
                for _, r in ipairs(state.saved_replays) do
                    table.insert(filenames, r.filename)
                end

                if #filenames > 0 then
                    local combo_changed, new_idx = imgui.combo("Файл реплея", state.selected_replay_idx, filenames)
                    if combo_changed then
                        state.selected_replay_idx = new_idx
                        replay.load(filenames[new_idx])
                    end
                    local sel = state.saved_replays[state.selected_replay_idx]
                    if sel then
                        imgui.text_colored(0.7, 0.85, 1.0, 1.0,
                            string.format("Авто: %s | Машин: %d | %.2f сек", sel.car or "?", sel.tracks or 1, sel.duration or 0.0))
                    end
                else
                    imgui.text_colored(1.0, 0.4, 0.4, 1.0, "Нет сохраненных реплеев в папке replays/")
                end

                imgui.spacing()
                if imgui.button("Обновить список файлов", 210, 26) then
                    refresh_replays()
                end

                imgui.spacing()
                imgui.separator()
                imgui.spacing()

                -- Ghost Collision Toggle (shared globally with [K] in ghost_test.lua)
                if carx.is_ghost_collision and carx.set_ghost_collision then
                    local col_cur = carx.is_ghost_collision()
                    local col_changed, col_new = imgui.checkbox("Коллизия призраков (Твёрдый кузов) [K]", col_cur)
                    if col_changed then
                        carx.set_ghost_collision(col_new)
                    end
                end

                imgui.spacing()
                imgui.text_colored(0.85, 0.85, 0.35, 1.0, "Режим призрака:")
                if imgui.button(state.playback_type == "GHOST" and "[x] 3D-Призрак (Model Clone)" or "[ ] 3D-Призрак (Model Clone)", 230, 26) then
                    state.playback_type = "GHOST"
                end
                imgui.same_line()
                if imgui.button(state.playback_type == "LEGACY" and "[x] Доп. слот авто (Legacy)" or "[ ] Доп. слот авто (Legacy)", 220, 26) then
                    state.playback_type = "LEGACY"
                end

                imgui.spacing()

                if state.mode == "PLAYING" then
                    if imgui.button("[❚❚] Пауза", 110, 32) then
                        toggle_pause()
                    end
                    imgui.same_line()
                    if imgui.button("[■] Стоп (F7)", 110, 32) then
                        stop_playback()
                    end
                elseif state.mode == "PAUSED" then
                    if imgui.button("[▶] Продолжить", 130, 32) then
                        toggle_pause()
                    end
                    imgui.same_line()
                    if imgui.button("[■] Стоп (F7)", 110, 32) then
                        stop_playback()
                    end
                else
                    if imgui.button("[▶] Воспроизвести заезд (F7)", 250, 34) then
                        start_playback()
                    end
                end

                local max_time = replay.get_duration()
                if max_time > 0 then
                    imgui.spacing()
                    local slider_changed, new_time = imgui.slider_float("Таймлайн", state.playback_time, 0.0, max_time)
                    if slider_changed then
                        state.playback_time = new_time
                        if state.mode == "PAUSED" and state.using_ghost_clone then
                            apply_ghost_time(state.playback_time)
                        end
                    end
                    imgui.text(string.format("Время: %.2f / %.2f сек", state.playback_time, max_time))
                end

                imgui.spacing()
                local speed_changed, new_speed = imgui.slider_float("Скорость", state.playback_speed, 0.25, 2.0)
                if speed_changed then state.playback_speed = new_speed end

                local off_changed, new_off = imgui.slider_float("Смещение вбок (м)", state.playback_offset, -8.0, 8.0)
                if off_changed then state.playback_offset = new_off end

                local delay_changed, new_delay = imgui.slider_float("Задержка старта (сек)", state.start_delay, 0.0, 10.0)
                if delay_changed then state.start_delay = new_delay end

                local loop_changed, new_loop = imgui.checkbox("Зациклить (Loop)", state.playback_loop)
                if loop_changed then state.playback_loop = new_loop end

                if replay.is_loaded() and replay.get_tracks then
                    local tracks = replay.get_tracks()
                    if tracks and #tracks > 1 then
                        imgui.spacing()
                        imgui.text_colored(0.65, 0.9, 0.75, 1.0, string.format("Треки в памяти (%d авто):", #tracks))
                        for _, tr in ipairs(tracks) do
                            local badge = tr.is_local and " [ВЫ]" or ""
                            imgui.text(string.format("  %d. %s%s (%d кадров)", tr.index, tr.car_name or "?", badge, tr.frames or 0))
                        end
                    end
                end

                if state.mode == "PLAYING" and state.countdown > 0 then
                    imgui.spacing()
                    imgui.text_colored(1.0, 0.9, 0.2, 1.0, string.format("Старт через: %.1f сек (займите позицию)", state.countdown))
                end

                imgui.end_tab_item()
            end

            -- ===================== TAB 3: SELECTIVE COLLISION (KINO) =====================
            if imgui.begin_tab_item("[KINO] Встречка / Игроки") then
                imgui.spacing()
                imgui.text_colored(0.45, 0.90, 1.0, 1.0, "Выборочная коллизия (Kino-режим):")
                imgui.text("Отключает столкновения только со встречкой или выбранными игроками.")
                imgui.text_colored(0.65, 0.85, 0.65, 1.0, "Машины в вашу сторону (попутка) и стены остаются твёрдыми.")
                imgui.spacing()
                imgui.separator()

                local en_ch, en_val = imgui.checkbox("Включить выборочную коллизию [J]", state.ghost_cfg.enabled)
                if en_ch then
                    state.ghost_cfg.enabled = en_val
                    sync_selective_ghost_cfg()
                end

                local aww_ch, aww_val = imgui.checkbox("Авто-призрак для встречки (Анти-встречка)", state.ghost_cfg.anti_wrong_way)
                if aww_ch then
                    state.ghost_cfg.anti_wrong_way = aww_val
                    sync_selective_ghost_cfg()
                end

                local hud_ch, hud_val = imgui.checkbox("Показывать мини-радар на трассе (Ghost Radar)", state.ghost_cfg.show_hud_alert)
                if hud_ch then
                    state.ghost_cfg.show_hud_alert = hud_val
                    sync_selective_ghost_cfg()
                end

                if imgui.collapsing_header("Параметры детектора встречного движения") then
                    local s1, v1 = imgui.slider_float("Мин. скорость встречки (км/ч)", state.ghost_cfg.min_speed_kmh, 0.0, 60.0)
                    if s1 then state.ghost_cfg.min_speed_kmh = v1; sync_selective_ghost_cfg() end

                    local s2, v2 = imgui.slider_float("Порог встречного курса (dot)", state.ghost_cfg.dot_threshold, -0.85, 0.0)
                    if s2 then state.ghost_cfg.dot_threshold = v2; sync_selective_ghost_cfg() end

                    local s3, v3 = imgui.slider_float("Дистанция (м, 0 = вся карта)", state.ghost_cfg.max_distance, 0.0, 300.0)
                    if s3 then state.ghost_cfg.max_distance = v3; sync_selective_ghost_cfg() end

                    imgui.text_colored(0.65, 0.68, 0.75, 1.0, "dot = -0.25 (~105° навстречу). Защита перекрытия: <6м до разъезда.")
                end

                imgui.spacing()
                imgui.separator()
                imgui.text_colored(0.85, 0.85, 0.35, 1.0, "Выборочная коллизия по игрокам:")
                if carx.clear_car_ghost_modes then
                    imgui.same_line()
                    if imgui.button("Сбросить всё в Авто##sg_clear", 160, 22) then
                        carx.clear_car_ghost_modes()
                    end
                end
                imgui.spacing()

                local cars = get_player_list()
                local other_count = 0
                for idx, c in ipairs(cars) do
                    if not c.is_local_player then
                        other_count = other_count + 1
                        local mode = c.ghost_mode or 0
                        local dist = c.dist_to_player or 0.0
                        local spd = c.speed_kmh or 0.0
                        local dot = c.heading_dot or 1.0

                        local status_txt = "ТВЁРДЫЙ"
                        local sr, sg, sb = 0.45, 0.90, 0.45
                        if not state.ghost_cfg.enabled then
                            status_txt = "ВЫКЛ (Твёрдый)"
                            sr, sg, sb = 0.65, 0.68, 0.75
                        elseif c.is_selectively_ghosted then
                            status_txt = c.is_wrong_way and "ПРИЗРАК (Встречка)" or "ПРИЗРАК"
                            sr, sg, sb = 0.35, 0.90, 1.0
                        end

                        local dir_tag = (dot < (state.ghost_cfg.dot_threshold or -0.25)) and "встречка" or "попутка"
                        imgui.text(string.format("#%d %s | %.0fм | %.0f км/ч | %s (dot %.2f)",
                            idx, c.unity_name or ("Авто #" .. idx), dist, spd, dir_tag, dot))
                        imgui.same_line()
                        imgui.text_colored(sr, sg, sb, 1.0, "[" .. status_txt .. "]")

                        if carx.set_car_ghost_mode then
                            local target_id = c.unity_name or c.ptr
                            if imgui.button((mode == 0 and "[x] Авто" or "Авто") .. "##sg_a_" .. idx, 95, 22) then
                                carx.set_car_ghost_mode(target_id, 0)
                            end
                            imgui.same_line()
                            if imgui.button((mode == 1 and "[x] Призрак" or "Призрак") .. "##sg_g_" .. idx, 110, 22) then
                                carx.set_car_ghost_mode(target_id, 1)
                            end
                            imgui.same_line()
                            if imgui.button((mode == 2 and "[x] Твёрдый" or "Твёрдый") .. "##sg_s_" .. idx, 110, 22) then
                                carx.set_car_ghost_mode(target_id, 2)
                            end
                        end
                    end
                end

                if other_count == 0 then
                    imgui.text_colored(0.75, 0.75, 0.80, 1.0, "  Других игроков на трассе пока нет.")
                end

                imgui.end_tab_item()
            end

            -- ===================== TAB 4: INFO =====================
            if imgui.begin_tab_item("[INFO] О моде") then
                imgui.spacing()
                imgui.text_colored(0.4, 0.8, 1.0, 1.0, "CarX Drift Racing Online 2 Replay & Ghost Mod")
                imgui.text("Лицензия: MIT (Полностью открытый и свободный код)")
                imgui.spacing()
                imgui.separator()
                imgui.text("Горячие клавиши:")
                imgui.text("  F4 / Insert  - Открыть / скрыть меню мода")
                imgui.text("  F6           - Быстрый старт / стоп записи (Player List)")
                imgui.text("  F7           - Быстрый запуск / стоп 3D-призраков")
                imgui.text("  F8           - Меню 3D-болванки и сцены Unity")
                imgui.text("  M            - Живое зеркало (+3.8 м)")
                imgui.text("  K            - Вкл / выкл коллизию 3D-призраков реплея")
                imgui.text("  J            - Вкл / выкл анти-встречку (Kino-коллизия)")
                imgui.text("  F9           - Горячая перезагрузка C++ ядра (Hot-Reload DLL)")
                imgui.spacing()
                imgui.separator()
                imgui.text_colored(0.35, 0.85, 0.45, 1.0, "Живой Hot-Reload скриптов:")
                imgui.text("Файлы в scripts/ можно редактировать на лету без перезапуска игры.")
                imgui.end_tab_item()
            end

            imgui.end_tab_bar()
        end
    end
    imgui.end_window()
end)

log.info("CarX2 Replay & Ghost Mod v2 (Multi-Car & Collision) ready!")
