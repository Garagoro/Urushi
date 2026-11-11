-- scripts/teleport_mod.lua
local T = i18n.t
local VK_SAVE = 0xDB -- '['
local VK_TP   = 0xDD -- ']'

local state = {
    active_slot = 1,
    slots = {},
    vel_mode = 1, -- 1=Stop, 2=Saved, 3=Keep
    hotkeys_enabled = true,
    status_msg = "",
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

local function slot_name(idx)
    return string.format(T("Точка %d", "Slot %d"), idx)
end

local function save_point(slot_idx)
    local idx = slot_idx or state.active_slot
    if not carx or not carx.get_rigid_body then return false end

    local snap = carx.get_rigid_body()
    if not snap or (not snap.rb and not snap.x) then
        state.status_msg = T("Выйдите на трассу на автомобиле.", "Enter a track first.")
        state.status_tone = "yellow"
        return false
    end

    state.slots[idx] = {
        rb = snap.rb,
        x = snap.x or 0.0, y = snap.y or 0.0, z = snap.z or 0.0,
        qx = snap.qx, qy = snap.qy, qz = snap.qz, qw = snap.qw,
        vel = snap.vel, ang_vel = snap.ang_vel,
        unity_name = snap.unity_name,
        speed_kmh = snap.speed_kmh or 0.0,
        gear = snap.gear or 0,
        saved_at = os.date("%H:%M:%S"),
    }
    state.status_msg = ""
    return true
end

local function teleport_to_point(slot_idx)
    local idx = slot_idx or state.active_slot
    local wp = state.slots[idx]
    if not wp or (not wp.rb and not wp.x) then
        state.status_msg = string.format(T("%s ещё не задана.", "%s is empty."), slot_name(idx))
        state.status_tone = "yellow"
        return false
    end
    if not carx or not carx.teleport_rigid_body then return false end

    local ok = carx.teleport_rigid_body(wp, (state.vel_mode or 1) - 1)
    if ok then
        state.status_msg = ""
    else
        state.status_msg = T("Ошибка телепортации.", "Teleport failed.")
        state.status_tone = "red"
    end
    return ok
end

events.on_key(function(vk, is_down)
    if not is_down or not state.hotkeys_enabled or is_typing() then return end
    if vk == VK_SAVE then save_point(state.active_slot)
    elseif vk == VK_TP then teleport_to_point(state.active_slot) end
end)

local function render_teleport_window()
    local p = carx and carx.get_player_car and carx.get_player_car() or nil
    local active_wp = state.slots[state.active_slot]

    ui.card(T("Позиция", "Position"), nil, function()
        if p then
            local slot_val = active_wp
                and string.format(T("%.1f м", "%.1f m"), distance_3d(p, active_wp))
                or T("Пусто", "Empty")
            ui.stat_row({
                { label = slot_name(state.active_slot), value = slot_val, tone = active_wp and "green" or "muted" },
                { label = T("Скорость", "Speed"), value = string.format(T("%.0f км/ч", "%.0f km/h"), p.speed_kmh or 0.0), tone = "accent" },
            })
            ui.spacing(4)
            ui.text_dim(string.format("XYZ: %.1f · %.1f · %.1f", p.x or 0.0, p.y or 0.0, p.z or 0.0))
        else
            ui.text_dim(T("Выйдите на трассу на автомобиле.", "Enter a track first."))
        end
    end)

    ui.card(T("Чекпоинты", "Checkpoints"), nil, function()
        local slot_labels = T({ "Точка 1", "Точка 2", "Точка 3" }, { "Slot 1", "Slot 2", "Slot 3" })
        local ch_slot, new_slot = ui.segmented("tp_slot_seg", state.active_slot, slot_labels)
        if ch_slot then
            state.active_slot = new_slot
        end
        active_wp = state.slots[state.active_slot]

        ui.spacing(4)
        local bw = (ui.avail_width() - 8) * 0.5
        if ui.button(T("Запомнить [##tp_save", "Save Point [##tp_save"), bw, 32, "primary") then
            save_point(state.active_slot)
        end
        ui.same_line(0, 8)
        local tp_style = active_wp and "default" or "ghost"
        if ui.button(T("Телепорт ]##tp_go", "Teleport ]##tp_go"), bw, 32, tp_style) then
            teleport_to_point(state.active_slot)
        end

        if active_wp then
            ui.spacing(4)
            ui.text_dim(string.format("%s · %.0f %s", active_wp.saved_at or "--", active_wp.speed_kmh or 0, T("км/ч", "km/h")))
            if ui.button(T("Очистить слот##tp_clear", "Clear Slot##tp_clear"), ui.avail_width(), 26, "ghost") then
                state.slots[state.active_slot] = nil
            end
        end
    end)

    ui.card(T("Параметры", "Options"), nil, function()
        ui.text_dim(T("Скорость при телепорте", "Velocity on Teleport"))
        local vel_labels = T({ "Стоп (0)", "Как в точке", "Текущая" }, { "Stop (0)", "Saved", "Keep" })
        local ch_vel, new_vel = ui.segmented("tp_vel_seg", state.vel_mode, vel_labels)
        if ch_vel then
            state.vel_mode = new_vel
        end

        ui.spacing(4)
        local hk_ch, hk_val = ui.toggle(T("Горячие клавиши [ / ]##tp_hk", "Hotkeys [ / ]##tp_hk"), state.hotkeys_enabled)
        if hk_ch then
            state.hotkeys_enabled = hk_val
        end
    end)

    if state.status_msg ~= "" then
        ui.text_wrapped(state.status_msg, state.status_tone or "muted")
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "teleport",
        title = T("Телепорт", "Teleport"),
        icon = "target",
        order = 55,
        default_open = false,
        default_pinned = false,
        x = 710,
        y = 54,
        width = 376,
        render = render_teleport_window,
    })
end

log.info("URUSHI Teleport module loaded.")
