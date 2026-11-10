-- CarX2ScriptHook: Тест создания АФК-призрака (Визуальная 3D-болванка Unity + Инспектор сцены)
-- Open Source under MIT License

local ghost_state = {
    is_open = true,
    ghost_visual = nil,
    spawn_pos = { x = 0.0, y = 0.0, z = 0.0 },
    mirror_offset = 3.8,
    status_text = "Ожидание команды. Выйдите на трассу и нажмите [G], [M] или кнопку ниже.",
    scanned_objects = {},
    matched_car_go = nil,
    pending_clone_ptr = nil,
    pending_destroy_ptr = nil
}

local function is_typing()
    if imgui.is_any_item_active then
        return imgui.is_any_item_active()
    end
    return false
end

-- Поиск модели машины игрока через нативный мост UnityBridge
local function find_player_car_game_object()
    local player = carx.get_player_car()
    if not player or not player.ptr then return nil end

    if unity and unity.find_player_car_model then
        local model_go, root_tf = unity.find_player_car_model()
        if model_go then
            local name = unity.get_name and unity.get_name(model_go) or "Model"
            local parent_name = ""
            if root_tf and memory and memory.read_ptr then
                local root_go = memory.read_ptr(tonumber(tostring(root_tf):match(":%s*(%x+)"), 16) + 0x20)
                if root_go and unity.get_name then
                    parent_name = unity.get_name(root_go) or ""
                end
            end
            return {
                ptr = model_go,
                name = (parent_name ~= "" and (parent_name .. "/" .. name) or name),
                transform = unity.get_transform and unity.get_transform(model_go) or nil
            }, 0.0
        end
    end

    -- Fallback: перебор объектов сцены
    if not unity or not unity.get_all_objects then
        return nil
    end

    local px, py, pz = player.x or 0, player.y or 0, player.z or 0
    local objects = unity.get_all_objects()
    ghost_state.scanned_objects = objects

    local best_obj = nil
    local best_dist = 999999.0
    for i = 1, #objects do
        local obj = objects[i]
        if obj.name == "Model" and obj.x then
            local dx = obj.x - px
            local dy = obj.y - py
            local dz = obj.z - pz
            local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
            if dist < 3.0 and dist < best_dist then
                best_dist = dist
                best_obj = obj
            end
        end
    end

    return best_obj, best_dist
end

-- Спавн 3D-болванки (без физики, чистый визуал Model(Clone))
local function spawn_ghost_action()
    local player = carx.get_player_car()
    if not player or not player.ptr then
        ghost_state.status_text = "Ошибка: Машина игрока не найдена. Зайдите на трассу!"
        return
    end

    if ghost_state.pending_clone_ptr or (ghost_state.ghost_visual and not carx.has_ghost()) then
        if unity and unity.destroy and ghost_state.ghost_visual then
            ghost_state.pending_destroy_ptr = ghost_state.ghost_visual
        end
        ghost_state.ghost_visual = nil
    end

    ghost_state.spawn_pos = { x = player.x or 0, y = player.y or 0, z = player.z or 0 }

    if carx.spawn_ghost then
        local res = carx.spawn_ghost()
        if res then
            if type(res) == "userdata" then
                ghost_state.ghost_visual = res
                ghost_state.status_text = string.format("3D-болванка создана в точке (%.1f, %.1f, %.1f)!", player.x, player.y, player.z)
            else
                ghost_state.status_text = "Запрос спавна отправлен в Main Thread Unity..."
            end
            log.info(ghost_state.status_text)
        else
            ghost_state.status_text = "Не удалось найти объект 'Model' машины игрока на трассе."
            log.warn(ghost_state.status_text)
        end
    else
        ghost_state.status_text = "Функция carx.spawn_ghost недоступна."
    end
end

-- Удаление болванки
local function destroy_ghost_action()
    if carx.destroy_ghost then
        carx.destroy_ghost()
    end
    if ghost_state.ghost_visual and not carx.has_ghost() then
        ghost_state.pending_destroy_ptr = ghost_state.ghost_visual
    end

    ghost_state.ghost_visual = nil
    ghost_state.status_text = "Болванка удалена с трассы."
    log.info(ghost_state.status_text)
end

-- Переключение живого зеркала (Stage 2: кузов + колеса + суппорты + руль)
local function toggle_mirror_action()
    if not carx.set_ghost_mirror then
        ghost_state.status_text = "Функция carx.set_ghost_mirror недоступна (нужен новый DLL)."
        return
    end
    local current = carx.is_ghost_mirror and carx.is_ghost_mirror() or false
    local next_state = not current
    carx.set_ghost_mirror(next_state, ghost_state.mirror_offset)
    if next_state then
        ghost_state.status_text = string.format("Живое зеркало ВКЛ (смещение %+.1f м): проверяйте колеса, суппорты и руль!", ghost_state.mirror_offset)
    else
        ghost_state.status_text = "Живое зеркало ВЫКЛ."
    end
    log.info(ghost_state.status_text)
end

-- Переключение коллизии болванки (вкл: твёрдый кузов + физический отскок, выкл: проезд насквозь)
local function toggle_collision_action()
    if not carx.set_ghost_collision then
        ghost_state.status_text = "Функция carx.set_ghost_collision недоступна (нужен новый DLL)."
        return
    end
    local current = carx.is_ghost_collision and carx.is_ghost_collision() or false
    local next_state = not current
    carx.set_ghost_collision(next_state)
    if next_state then
        ghost_state.status_text = "Коллизия болванки ВКЛ: кузов твёрдый (можно тереться дверьми!)."
    else
        ghost_state.status_text = "Коллизия болванки ВЫКЛ: призрак прозрачный."
    end
    log.info(ghost_state.status_text)
end

-- Хоткеи: F8 - меню, G - болванка, H - удалить, M - живое зеркало, K - коллизия
events.on_key(function(key, is_down)
    if not is_down then return end

    if key == 0x77 then -- F8
        ghost_state.is_open = not ghost_state.is_open
    end

    if key == 0x47 and not is_typing() then -- G
        spawn_ghost_action()
    end

    if key == 0x48 and not is_typing() then -- H
        destroy_ghost_action()
    end

    if key == 0x4D and not is_typing() then -- M
        toggle_mirror_action()
    end

    if key == 0x4B and not is_typing() then -- K
        toggle_collision_action()
    end
end)

-- Операции инспектора на Unity Main Thread (клонирование/удаление кастомных объектов)
if events.on_main_update then
    events.on_main_update(function(dt)
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
                    ghost_state.status_text = string.format("Клонирован вручную -> %s", tostring(cl))
                else
                    ghost_state.status_text = "unity.clone вернул nil!"
                end
            end
        end
    end)
end

-- Обновление состояния визуала
events.on_update(function(dt)
    if carx.get_ghost then
        local g = carx.get_ghost()
        if g then
            ghost_state.ghost_visual = g
        elseif not (carx.has_ghost and carx.has_ghost()) then
            -- оставляем локальный клон из инспектора, если есть
        end
    end
end)

-- Графический интерфейс ImGui
events.on_render(function()
    if not ghost_state.is_open then return end

    if imgui.set_next_window_size then
        imgui.set_next_window_size(560, 600, 2)
    end

    local is_visible, should_open = imgui.begin_window("👻 3D-болванка & Инспектор Unity [F8]", true)
    if not is_visible then
        imgui.end_window()
        ghost_state.is_open = should_open
        return
    end

    local player = carx.get_player_car()

    imgui.text_colored(0.4, 0.8, 1.0, 1.0, "Машина игрока:")
    imgui.separator()

    if player and player.ptr then
        imgui.text_colored(0.2, 1.0, 0.3, 1.0, string.format("Координаты: X=%.2f, Y=%.2f, Z=%.2f | Скорость: %.1f км/ч", player.x or 0, player.y or 0, player.z or 0, player.speed_kmh or 0))
    else
        imgui.text_colored(1.0, 0.4, 0.4, 1.0, "[НЕ НАЙДЕНА] Зайдите на трассу!")
    end

    imgui.spacing()
    imgui.text_colored(1.0, 0.8, 0.2, 1.0, "Статус болванки:")
    imgui.separator()

    local active = (ghost_state.ghost_visual ~= nil) or (carx.has_ghost and carx.has_ghost())
    local ghost_count = (carx.get_ghost_count and carx.get_ghost_count()) or (active and 1 or 0)
    if active then
        imgui.text_colored(0.2, 1.0, 0.3, 1.0, string.format("[АКТИВЕН] 3D-болванок на трассе: %d", ghost_count))
        if ghost_state.ghost_visual then
            imgui.text(string.format("Unity GameObject: %s", tostring(ghost_state.ghost_visual)))
        else
            imgui.text("Создание клона на Main Thread...")
        end
    else
        imgui.text_colored(0.7, 0.7, 0.7, 1.0, "[НЕ СОЗДАН] Нажмите [G] или кнопку ниже")
    end

    imgui.spacing()
    if not active then
        if imgui.button("👻 Создать 3D-болванку на моем месте (G)", 520, 36) then
            spawn_ghost_action()
        end
    else
        if imgui.button("❌ Удалить болванку с трассы (H)", 520, 36) then
            destroy_ghost_action()
        end
    end

    imgui.spacing()
    local mirror_active = carx.is_ghost_mirror and carx.is_ghost_mirror() or false
    local mirror_label = mirror_active
        and "⏹ Выключить Живое Зеркало (Stage 2) [M]"
        or  "🔄 Включить Живое Зеркало +3.8м (колеса + суппорты + руль) [M]"
    if imgui.button(mirror_label, 520, 34) then
        toggle_mirror_action()
    end
    if imgui.slider_float then
        local off_changed, new_off = imgui.slider_float("Смещение зеркала (м)", ghost_state.mirror_offset, -10.0, 10.0)
        if off_changed then
            ghost_state.mirror_offset = new_off
            if mirror_active and carx.set_ghost_mirror then
                carx.set_ghost_mirror(true, ghost_state.mirror_offset)
            end
        end
    end

    imgui.spacing()
    local col_on = carx.is_ghost_collision and carx.is_ghost_collision() or false
    local col_label = col_on
        and "🛡 Коллизия болванки: ВКЛ (Твёрдый кузов) [K]"
        or  "👻 Коллизия болванки: ВЫКЛ (Прозрачный) [K]"
    if imgui.button(col_label, 520, 32) then
        toggle_collision_action()
    end

    imgui.spacing()
    if imgui.text_wrapped then
        imgui.text_wrapped(ghost_state.status_text)
    else
        imgui.text(ghost_state.status_text)
    end

    imgui.spacing()
    imgui.separator()
    imgui.text_colored(0.9, 0.5, 1.0, 1.0, "🔍 Инспектор объектов сцены Unity:")

    if imgui.button("Сканировать все GameObjects сцены", 260, 28) then
        if unity and unity.get_all_objects then
            ghost_state.scanned_objects = unity.get_all_objects()
            ghost_state.status_text = string.format("Найдено объектов сцены: %d", #ghost_state.scanned_objects)
        else
            ghost_state.status_text = "Модуль unity.get_all_objects не найден."
        end
    end

    imgui.same_line()
    if imgui.button("Найти модель машины", 250, 28) then
        local obj, dist = find_player_car_game_object()
        if obj then
            ghost_state.status_text = string.format("Найдена модель: '%s' (ptr=%s)", obj.name, tostring(obj.ptr))
            log.info(ghost_state.status_text)
        else
            ghost_state.status_text = "Объект 'Model' машины игрока не найден."
        end
    end

    if #ghost_state.scanned_objects > 0 then
        imgui.text(string.format("Всего объектов: %d. Подходящие корневые узлы:", #ghost_state.scanned_objects))
        local shown = 0
        for i = 1, #ghost_state.scanned_objects do
            local obj = ghost_state.scanned_objects[i]
            local name = obj.name or ""
            if name == "Model" or name:find("_0") or name:find("_1") or name:find("Kyoto") or name:find("Nishimo") or name:find("BNM") or name:find("Car") then
                shown = shown + 1
                if shown <= 18 then
                    imgui.text(string.format("[%d] %s (%s)", i, name, tostring(obj.ptr)))
                    imgui.same_line()
                    if imgui.button("Клонировать##" .. tostring(i), 110, 20) then
                        ghost_state.pending_clone_ptr = obj.ptr
                        ghost_state.status_text = string.format("Запрошено клонирование '%s'...", name)
                    end
                end
            end
        end
    end

    imgui.spacing()
    imgui.text_colored(0.5, 0.5, 0.5, 1.0, "Хоткеи: [F8] Меню | [G] Болванка | [H] Удалить | [M] Зеркало | [K] Коллизия")

    imgui.end_window()
    ghost_state.is_open = should_open
end)

log.info("Модуль ghost_test.lua (v2 Main Thread Model Cloner) успешно загружен")
