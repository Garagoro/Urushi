-- CarX2 Teleport & Drift Checkpoint Module ([ / ])
-- Saves full vehicle physics RigidBody state (position, 3D rotation, optional velocity) on Main Thread.

local VK_SAVE = 0xDB -- '['
local VK_TP   = 0xDD -- ']'

local SLOT_LABELS = { "Точка 1", "Точка 2", "Точка 3" }
local VEL_LABELS  = { "Стоп (0)", "Как в точке", "Текущая" }

local state = {
    active_slot = 1,
    slots = {},
    vel_mode = 1, -- 1=Стоп (0 км/ч), 2=Как в точке, 3=Текущая скорость
    hotkeys_enabled = true,
    status_msg = "Готово. Выберите слот и нажмите «Запомнить точку».",
    status_tone = "muted",
}

local function is_typing()
    return imgui and imgui.is_any_item_active and imgui.is_any_item_active()
end

local function distance_3d(a, b)
    if not a or not b then return 0.0 end
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    local dz = (a.z or 0) - (b.z or 0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

local function save_point(slot_idx)
    local idx = slot_idx or state.active_slot
    if not carx or not carx.get_rigid_body then
        state.status_msg = "C++ API телепортации недоступен."
        state.status_tone = "red"
        return false
    end

    local snap = carx.get_rigid_body()
    if not snap or (not snap.rb and not snap.x) then
        state.status_msg = "Автомобиль не найден на трассе."
        state.status_tone = "yellow"
        return false
    end

    state.slots[idx] = {
        rb = snap.rb,
        x = snap.x or 0.0,
        y = snap.y or 0.0,
        z = snap.z or 0.0,
        qx = snap.qx,
        qy = snap.qy,
        qz = snap.qz,
        qw = snap.qw,
        vel = snap.vel,
        ang_vel = snap.ang_vel,
        unity_name = snap.unity_name,
        speed_kmh = snap.speed_kmh or 0.0,
        gear = snap.gear or 0,
        saved_at = os.date("%H:%M:%S"),
    }
    state.status_msg = string.format(
        "%s сохранена (%.1f, %.1f, %.1f | %.0f км/ч)",
        SLOT_LABELS[idx] or ("Точка " .. idx),
        snap.x or 0.0, snap.y or 0.0, snap.z or 0.0, snap.speed_kmh or 0.0
    )
    state.status_tone = "green"
    return true
end

local function teleport_to_point(slot_idx)
    local idx = slot_idx or state.active_slot
    local wp = state.slots[idx]
    if not wp or (not wp.rb and not wp.x) then
        state.status_msg = string.format("%s ещё не задана!", SLOT_LABELS[idx] or ("Точка " .. idx))
        state.status_tone = "yellow"
        return false
    end
    if not carx or not carx.teleport_rigid_body then
        state.status_msg = "C++ API телепортации недоступен."
        state.status_tone = "red"
        return false
    end

    local mode_idx = (state.vel_mode or 1) - 1 -- 0=stop, 1=saved, 2=keep
    local ok = carx.teleport_rigid_body(wp, mode_idx)
    if ok then
        state.status_msg = string.format(
            "Телепорт → %s (%s)",
            SLOT_LABELS[idx] or ("Точка " .. idx),
            VEL_LABELS[state.vel_mode] or "Стоп"
        )
        state.status_tone = "accent"
    else
        state.status_msg = "Не удалось телепортировать авто (нет активной сессии)."
        state.status_tone = "red"
    end
    return ok
end

events.on_key(function(vk, is_down)
    if not is_down or not state.hotkeys_enabled then return end
    if is_typing() then return end

    if vk == VK_SAVE then
        save_point(state.active_slot)
    elseif vk == VK_TP then
        teleport_to_point(state.active_slot)
    end
end)

local function render_teleport_window()
    local p = carx and carx.get_player_car and carx.get_player_car() or nil
    local active_wp = state.slots[state.active_slot]

    local pos_sub = p and ((p.unity_name and p.unity_name ~= "") and p.unity_name or "На трассе") or "Ожидание заезда"
    ui.card("Текущая позиция", pos_sub, function()
        if p then
            ui.stat_row({
                { label = "X", value = string.format("%.1f", p.x or 0.0), tone = "muted" },
                { label = "Y", value = string.format("%.1f", p.y or 0.0), tone = "muted" },
                { label = "Z", value = string.format("%.1f", p.z or 0.0), tone = "muted" },
            })
            ui.spacing(4)
            if active_wp then
                local dist = distance_3d(p, active_wp)
                ui.badge(SLOT_LABELS[state.active_slot] .. " задана", "green")
                ui.same_line(0, 8)
                ui.text(string.format("До точки: %.1f м (%.0f км/ч)", dist, active_wp.speed_kmh or 0.0), "muted")
            else
                ui.badge("Точка не задана", "muted")
                ui.same_line(0, 8)
                ui.text(string.format("Скорость: %.0f км/ч", p.speed_kmh or 0.0), "muted")
            end
        else
            ui.text_wrapped("Выйдите на трассу или тренировку, чтобы сохранять точки и телепортироваться.", "muted")
        end
    end)

    ui.card("Точки телепортации", "Быстрое сохранение позиции и угла", function()
        local ch_slot, new_slot = ui.segmented("tp_slot_seg", state.active_slot, SLOT_LABELS)
        if ch_slot then
            state.active_slot = new_slot
        end
        active_wp = state.slots[state.active_slot]

        ui.spacing(4)
        local bw = (ui.avail_width() - 8) * 0.5
        if ui.button("Запомнить точку##tp_save", bw, 32, "primary") then
            save_point(state.active_slot)
        end
        ui.same_line(0, 8)
        local tp_style = active_wp and "default" or "ghost"
        if ui.button("Телепорт к точке##tp_go", bw, 32, tp_style) then
            teleport_to_point(state.active_slot)
        end

        if active_wp then
            ui.spacing(4)
            ui.text_dim(string.format(
                "Слот %d (%s): X %.1f | Y %.1f | Z %.1f | %.0f км/ч",
                state.active_slot, active_wp.saved_at or "--",
                active_wp.x or 0, active_wp.y or 0, active_wp.z or 0, active_wp.speed_kmh or 0
            ))
            if ui.button("Очистить слот##tp_clear", ui.avail_width(), 26, "ghost") then
                state.slots[state.active_slot] = nil
                state.status_msg = SLOT_LABELS[state.active_slot] .. " очищена."
                state.status_tone = "muted"
            end
        end
    end)

    ui.card("Режим и клавиши", "Настройка импульса при ТП", function()
        ui.text_dim("Скорость автомобиля после телепорта")
        local ch_vel, new_vel = ui.segmented("tp_vel_seg", state.vel_mode, VEL_LABELS)
        if ch_vel then
            state.vel_mode = new_vel
        end

        ui.spacing(4)
        local hk_ch, hk_val = ui.toggle("Горячие клавиши [ и ]##tp_hk", state.hotkeys_enabled)
        if hk_ch then
            state.hotkeys_enabled = hk_val
        end
        ui.text_wrapped("[ — запомнить точку, ] — телепортироваться к ней.", "muted")
    end)

    ui.text_wrapped(state.status_msg, state.status_tone or "muted")
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "teleport",
        title = "Телепорт",
        icon = "target",
        order = 55,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 376,
        render = render_teleport_window,
    })
end

log.info("URUSHI Teleport module (scripts/teleport_mod.lua) loaded.")
