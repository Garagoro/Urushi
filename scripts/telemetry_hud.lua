-- Telemetry HUD Module (Lacquered Urushi Floating HUD)

local function render_telemetry_hud()
    local player = carx.get_player_car and carx.get_player_car() or nil
    if not player then
        ui.card("Нет данных", "Вне трассы", function()
            ui.text_dim("Выйдите на трассу на автомобиле для отображения телеметрии.")
        end)
        return
    end

    local gear_text = "N"
    if player.gear == 0 then gear_text = "R"
    elseif (player.gear or -1) > 0 then gear_text = tostring(player.gear) end

    ui.card("Телеметрия", nil, function()
        ui.stat_row({
            { label = "КМ/Ч", value = string.format("%.0f", player.speed_kmh or 0.0), tone = "accent" },
            { label = "ПЕРЕДАЧА", value = gear_text, tone = "default" },
            { label = "ОБОРОТЫ", value = string.format("%.0f", player.rpm or 0.0), tone = "muted" },
        })

        ui.spacing(4)
        local rpm_frac = math.min(math.max((player.rpm or 0.0) / 8500.0, 0.0), 1.0)
        ui.progress_bar(rpm_frac, -1, 6)
        ui.spacing(6)

        ui.stat_row({
            { label = "РУЛЬ", value = string.format("%.1f°", (player.steer_angle or 0.0) * 57.2958) },
            { label = "ГАЗ", value = string.format("%.0f%%", (player.accel or 0.0) * 100.0), tone = "green" },
            {
                label = "ТОРМОЗ",
                value = string.format("%.0f%%", (player.brake or 0.0) * 100.0),
                tone = (player.brake or 0.0) > 0.1 and "red" or "muted",
            },
        })

        if (player.handbrake or 0.0) > 0.5 then
            ui.spacing(4)
            ui.badge("РУЧНИК ВКЛ", "yellow")
        end

        ui.spacing(4)
        ui.text_dim(string.format("XYZ: %.1f  %.1f  %.1f", player.x or 0.0, player.y or 0.0, player.z or 0.0))
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "hud",
        title = "Телеметрия HUD",
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
