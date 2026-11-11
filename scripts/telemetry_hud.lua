-- scripts/telemetry_hud.lua
local T = i18n.t

local function render_telemetry_hud()
    local player = carx.get_player_car and carx.get_player_car() or nil
    if not player then
        ui.text_dim(T("Вне трассы", "Off Track"))
        return
    end

    local gear_text = "N"
    if player.gear == 0 then gear_text = "R"
    elseif (player.gear or -1) > 0 then gear_text = tostring(player.gear) end

    ui.stat_row({
        { label = T("КМ/Ч", "KM/H"), value = string.format("%.0f", player.speed_kmh or 0.0), tone = "accent" },
        { label = T("ПЕРЕД.", "GEAR"), value = gear_text, tone = "default" },
        { label = T("ОБОР.", "RPM"), value = string.format("%.0f", player.rpm or 0.0), tone = "muted" },
    })

    ui.spacing(4)
    local rpm_frac = math.min(math.max((player.rpm or 0.0) / 8500.0, 0.0), 1.0)
    ui.progress_bar(rpm_frac, -1, 6)
    ui.spacing(4)

    ui.stat_row({
        { label = T("РУЛЬ", "STEER"), value = string.format("%.1f°", (player.steer_angle or 0.0) * 57.2958) },
        { label = T("ГАЗ", "THR"), value = string.format("%.0f%%", (player.accel or 0.0) * 100.0), tone = "green" },
        {
            label = T("ТОРМ.", "BRK"),
            value = string.format("%.0f%%", (player.brake or 0.0) * 100.0),
            tone = (player.brake or 0.0) > 0.1 and "red" or "muted",
        },
    })

    if (player.handbrake or 0.0) > 0.5 then
        ui.spacing(4)
        ui.badge(T("РУЧНИК", "HANDBRAKE"), "yellow")
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "hud",
        title = T("Телеметрия HUD", "Telemetry HUD"),
        icon = "hud",
        order = 60,
        default_open = false,
        default_pinned = true,
        hud_no_inputs = true,
        anchor_right = true,
        x = 20,
        y = 24,
        width = 276,
        render = render_telemetry_hud,
    })
end
