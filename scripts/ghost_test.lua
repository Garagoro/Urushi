-- scripts/ghost_test.lua
local T = i18n.t

local ghost_state = {
    status_text = "",
    status_tone = "muted",
    scanned_objects = {},
    ghost_visual = nil,
    pending_clone_ptr = nil,
    pending_destroy_ptr = nil,
    mirror_offset = 3.8,
}

local function is_typing()
    return imgui and imgui.is_any_item_active and imgui.is_any_item_active()
end

local function find_player_car_game_object()
    if not unity or not unity.get_all_objects then return nil end
    local objs = unity.get_all_objects()
    for _, obj in ipairs(objs) do
        if obj.name == "Model" or obj.name == "CARXCar" or obj.name == "RecordableDotsRaceCar" then
            return obj
        end
    end
    for _, obj in ipairs(objs) do
        if obj.name and (obj.name:find("Kyoto") or obj.name:find("Nishimo") or obj.name:find("BNM")) then
            return obj
        end
    end
    return nil
end

local function spawn_ghost_action()
    local player = carx.get_player_car()
    if not (player and player.ptr) then
        ghost_state.status_text = T("Выйдите на трассу на автомобиле.", "Enter a track first.")
        ghost_state.status_tone = "yellow"
        return
    end
    if carx.spawn_ghost then
        local g = carx.spawn_ghost()
        if g then
            ghost_state.ghost_visual = g
            if carx.is_ghost_mirror and carx.is_ghost_mirror() and carx.set_ghost_mirror then
                carx.set_ghost_mirror(true, ghost_state.mirror_offset)
            end
            ghost_state.status_text = ""
        else
            ghost_state.status_text = T("Не удалось создать болванку.", "Failed to spawn dummy.")
            ghost_state.status_tone = "red"
        end
    else
        local obj = find_player_car_game_object()
        if obj then
            ghost_state.pending_clone_ptr = obj.ptr
            ghost_state.status_text = ""
        else
            ghost_state.status_text = T("Модель машины не найдена.", "Car model not found.")
            ghost_state.status_tone = "yellow"
        end
    end
end

local function destroy_ghost_action()
    if carx.destroy_ghost then
        carx.destroy_ghost()
    end
    if ghost_state.ghost_visual and unity and unity.destroy then
        ghost_state.pending_destroy_ptr = ghost_state.ghost_visual
    end
    ghost_state.ghost_visual = nil
    ghost_state.status_text = ""
end

local function toggle_mirror_action()
    if carx.set_ghost_mirror and carx.is_ghost_mirror then
        local next_state = not carx.is_ghost_mirror()
        if next_state and (not carx.has_ghost or not carx.has_ghost()) and carx.spawn_ghost then
            ghost_state.ghost_visual = carx.spawn_ghost()
        end
        carx.set_ghost_mirror(next_state, ghost_state.mirror_offset)
    end
end

local function toggle_collision_action()
    if carx.set_ghost_collision and carx.is_ghost_collision then
        carx.set_ghost_collision(not carx.is_ghost_collision())
    end
end

events.on_key(function(key, is_down)
    if not is_down or is_typing() then return end

    if not keybinds and key == 0x77 then -- F8
        if urushi and urushi.toggle_window then urushi.toggle_window("ghost") end
    elseif key == 0x47 then -- G
        spawn_ghost_action()
    elseif key == 0x48 then -- H
        destroy_ghost_action()
    elseif key == 0x4D then -- M
        toggle_mirror_action()
    elseif key == 0x4B then -- K
        toggle_collision_action()
    end
end)

if events.on_main_update then
    events.on_main_update(function(dt)
        if keybinds and keybinds.is_pressed and keybinds.is_pressed("dummy_ghost") then
            if urushi and urushi.toggle_window then urushi.toggle_window("ghost") end
        end
        if ghost_state.pending_destroy_ptr then
            if unity and unity.destroy then
                unity.destroy(ghost_state.pending_destroy_ptr)
            end
            ghost_state.pending_destroy_ptr = nil
        end

        if ghost_state.pending_clone_ptr then
            local src = ghost_state.pending_clone_ptr
            ghost_state.pending_clone_ptr = nil
            if unity and unity.clone then
                local cl = unity.clone(src)
                if cl then
                    local tf = unity.get_transform(cl)
                    local p = carx.get_player_car()
                    if tf and p and unity.set_position then
                        unity.set_position(tf, p.x, p.y, p.z)
                    end
                    if unity.set_layer then unity.set_layer(cl, 2) end
                    ghost_state.ghost_visual = cl
                end
            end
        end
    end)
end

events.on_update(function(dt)
    if carx.get_ghost then
        local g = carx.get_ghost()
        if g then
            ghost_state.ghost_visual = g
        end
    end
end)

local function render_ghost_window()
    local player = carx.get_player_car()
    local active = (ghost_state.ghost_visual ~= nil) or (carx.has_ghost and carx.has_ghost())
    local ghost_count = (carx.get_ghost_count and carx.get_ghost_count()) or (active and 1 or 0)
    local mirror_active = carx.is_ghost_mirror and carx.is_ghost_mirror() or false
    local col_on = carx.is_ghost_collision and carx.is_ghost_collision() or false

    ui.card(T("Состояние", "Status"), nil, function()
        ui.stat_row({
            {
                label = T("Игрок", "Speed"),
                value = (player and player.ptr) and string.format(T("%.0f км/ч", "%.0f km/h"), player.speed_kmh or 0.0) or T("Вне трассы", "Off Track"),
                tone = (player and player.ptr) and "green" or "yellow",
            },
            {
                label = T("Болванки", "Dummies"),
                value = tostring(ghost_count),
                tone = active and "accent" or "muted",
            },
            {
                label = T("Зеркало", "Mirror"),
                value = mirror_active and T("ВКЛ", "ON") or T("ВЫКЛ", "OFF"),
                tone = mirror_active and "green" or "muted",
            },
        })
    end)

    ui.card(T("Управление", "Controls"), nil, function()
        local full_w = ui.avail_width()
        if not active then
            if ui.button(T("Создать 3D-болванку [G]##gh_spawn", "Spawn 3D Dummy [G]##gh_spawn"), full_w, 32, "primary") then
                spawn_ghost_action()
            end
        else
            if ui.button(T("Удалить болванку [H]##gh_del", "Remove Dummy [H]##gh_del"), full_w, 32, "danger") then
                destroy_ghost_action()
            end
        end

        ui.spacing(4)
        local bw = (full_w - 8) * 0.5
        if ui.button(mirror_active and T("Зеркало: ВКЛ [M]##gh_mir", "Mirror: ON [M]##gh_mir") or T("Живое зеркало [M]##gh_mir", "Live Mirror [M]##gh_mir"), bw, 28, mirror_active and "primary" or "default") then
            toggle_mirror_action()
        end
        ui.same_line(0, 8)
        if ui.button(col_on and T("Коллизия: ТВЁРД [K]##gh_col", "Collision: SOLID [K]##gh_col") or T("Коллизия: ПРИЗР [K]##gh_col", "Collision: GHOST [K]##gh_col"), bw, 28, col_on and "primary" or "default") then
            toggle_collision_action()
        end

        ui.spacing(4)
        local off_changed, new_off = ui.slider_float(T("Смещение зеркала##gh_off", "Mirror Offset##gh_off"), ghost_state.mirror_offset, -10.0, 10.0, T("%+.1f м", "%+.1f m"))
        if off_changed then
            ghost_state.mirror_offset = new_off
            if mirror_active and carx.set_ghost_mirror then
                carx.set_ghost_mirror(true, ghost_state.mirror_offset)
            end
        end

        if ghost_state.status_text ~= "" then
            ui.spacing(2)
            ui.text_wrapped(ghost_state.status_text, ghost_state.status_tone)
        end
    end)

    if ui.collapsing(T("Инспектор сцены (Debug)##gh_insp", "Scene Inspector (Debug)##gh_insp"), false) then
        ui.card(nil, nil, function()
            local bw = (ui.avail_width() - 8) * 0.5
            if ui.button(T("Сканировать##gh_scan", "Scan Scene##gh_scan"), bw, 26) then
                if unity and unity.get_all_objects then
                    ghost_state.scanned_objects = unity.get_all_objects()
                end
            end
            ui.same_line(0, 8)
            if ui.button(T("Найти авто##gh_find", "Find Car##gh_find"), bw, 26, "ghost") then
                local obj = find_player_car_game_object()
                ghost_state.status_text = obj and string.format("%s (%s)", obj.name, tostring(obj.ptr)) or T("Не найдено", "Not found")
                ghost_state.status_tone = obj and "accent" or "yellow"
            end

            if #ghost_state.scanned_objects > 0 then
                ui.spacing(4)
                local shown = 0
                for i = 1, #ghost_state.scanned_objects do
                    local obj = ghost_state.scanned_objects[i]
                    local name = obj.name or ""
                    if name == "Model" or name:find("_0") or name:find("_1") or name:find("Kyoto") or name:find("Nishimo") or name:find("BNM") or name:find("Car") then
                        shown = shown + 1
                        if shown <= 16 then
                            if ui.button(T("Клон##gh_cl_", "Clone##gh_cl_") .. tostring(i), 62, 22, "ghost") then
                                ghost_state.pending_clone_ptr = obj.ptr
                            end
                            ui.same_line(0, 8)
                            if #name > 26 then name = name:sub(1, 24) .. ".." end
                            ui.text(string.format("[%d] %s", i, name))
                        end
                    end
                end
            end
        end)
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "ghost",
        title = T("Болванка 3D", "3D Dummy"),
        icon = "ghost",
        order = 50,
        default_open = false,
        default_pinned = false,
        x = 294,
        y = 54,
        width = 396,
        render = render_ghost_window,
    })
end

log.info("Ghost 3D dummy module loaded.")
