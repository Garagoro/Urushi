-- Telemetry HUD Module
local show_hud = true

events.on_render(function()
    if not show_hud then return end

    local player = carx.get_player_car()
    if not player then return end

    local isOpen, keepOpen = imgui.begin_window("Telemetry HUD", true)
    if isOpen then
        imgui.push_font("mono")

        imgui.text_colored(0.4, 0.8, 1.0, 1.0, string.format("Speed:     %6.1f km/h", player.speed_kmh))
        imgui.text_colored(1.0, 0.6, 0.2, 1.0, string.format("RPM:       %6.0f", player.rpm))

        local gear_text = "N"
        if player.gear == 0 then gear_text = "R"
        elseif player.gear > 0 then gear_text = tostring(player.gear) end
        imgui.text(string.format("Gear:           %s", gear_text))

        imgui.separator()
        imgui.text(string.format("Steer:     %6.1f°", player.steer_angle * 57.2958))
        imgui.text(string.format("Pos:       [%6.1f, %6.1f, %6.1f]", player.x, player.y, player.z))

        imgui.separator()
        imgui.text(string.format("Throttle:  %5.0f%%", player.accel * 100))
        imgui.text(string.format("Brake:     %5.0f%%", player.brake * 100))
        imgui.text(string.format("Handbrake: %s", player.handbrake > 0.5 and "ON" or "OFF"))

        imgui.pop_font()
    end
    imgui.end_window()
end)
