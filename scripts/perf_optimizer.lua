-- scripts/perf_optimizer.lua
-- URUSHI: Оптимизатор физики и FPS в мультиплеере (Top-N ближайших на локальной физике, остальные — серверные снапшоты)

local VK_F5 = 0x74

local function apply_preset(preset_name)
    if not optimizer or not optimizer.set_config then return end
    if preset_name == "vanilla" then
        optimizer.reset_defaults()
    elseif preset_name == "optimal3" then
        optimizer.set_config({
            enabled = true,
            max_full_sim_cars = 3,
            max_full_sim_dist = 80.0,
            hysteresis_meters = 8.0,
            switch_cooldown_sec = 1.8,
            tune_deform_sleep = true,
            deform_sleep_dist = 12.0,
            deform_idle_steps = 10,
            tune_root_sleep = false,
            tune_render_policy = true,
            small_hide_dist = 15.0,
            small_renderer_radius = 0.45,
            tiny_renderer_radius = 0.22,
            remote_lod_half_rate = true,
            player_best_lod = false,
            limit_car_effects = false,
            max_effects_cars = 3,
            tune_offline_physics = false,
        })
    elseif preset_name == "maxfps" then
        optimizer.set_config({
            enabled = true,
            max_full_sim_cars = 1,
            max_full_sim_dist = 35.0,
            hysteresis_meters = 8.0,
            switch_cooldown_sec = 2.0,
            tune_deform_sleep = true,
            deform_sleep_dist = 6.0,
            deform_idle_steps = 5,
            tune_root_sleep = true,
            root_idle_steps = 8,
            tune_render_policy = true,
            small_hide_dist = 10.0,
            small_renderer_radius = 0.55,
            tiny_renderer_radius = 0.28,
            remote_lod_half_rate = true,
            player_best_lod = false,
            limit_car_effects = true,
            max_effects_cars = 1,
            tune_offline_physics = true,
            use_simple_mode = true,
            use_traffic_mode = false,
            remote_integration_steps = 1,
        })
    end
end

local function detect_preset(cfg)
    if not cfg.enabled then return "vanilla" end
    if (cfg.max_full_sim_cars or 0) == 3 and math.abs((cfg.max_full_sim_dist or 0) - 80.0) < 0.5 then
        return "optimal3"
    end
    if (cfg.max_full_sim_cars or 0) == 1 and math.abs((cfg.max_full_sim_dist or 0) - 35.0) < 0.5 then
        return "maxfps"
    end
    return nil
end

local function render_optimizer_window()
    if not optimizer or not optimizer.get_config then
        ui.card("Оптимизатор недоступен", "Модуль C++ optimizer", function()
            ui.text_wrapped("Модуль C++ optimizer не загружен (нажмите Ctrl+F11 или перезапустите игру).", "red")
        end)
        return
    end

    local cfg = optimizer.get_config()
    local st = optimizer.get_stats()
    local dirty = false

    ui.card("Режимы и статус", "Серверные снапшоты MP", function()
        local en_ch, en_val = ui.toggle("Включить оптимизатор симуляции##opt_en", cfg.enabled == true)
        if en_ch then
            cfg.enabled = en_val
            dirty = true
        end

        ui.spacing(4)
        local active = detect_preset(cfg)
        local bw = (ui.avail_width() - 12) / 3.0

        if ui.button("Ваниль##opt_p_van", bw, 28, active == "vanilla" and "primary" or "default") then
            apply_preset("vanilla")
            cfg = optimizer.get_config()
            dirty = false
        end
        ui.same_line(0, 6)
        if ui.button("Топ-3##opt_p_o3", bw, 28, active == "optimal3" and "primary" or "default") then
            apply_preset("optimal3")
            cfg = optimizer.get_config()
            dirty = false
        end
        ui.same_line(0, 6)
        if ui.button("Макс FPS##opt_p_max", bw, 28, active == "maxfps" and "primary" or "default") then
            apply_preset("maxfps")
            cfg = optimizer.get_config()
            dirty = false
        end

        ui.spacing(6)
        ui.stat_row({
            {
                label = "Хуки MP",
                value = st.hooks_installed and "Активны" or "Ожидание",
                tone = st.hooks_installed and "green" or "yellow",
            },
            {
                label = "Переключений",
                value = tostring(st.switching_calls or 0),
                tone = "muted",
            },
            {
                label = "Анти-моргание",
                value = st.anti_flicker_holding and "Удерживает" or "Дежурит",
                tone = st.anti_flicker_holding and "green" or "accent",
            },
        })
    end)

    ui.card("Лимит локальной физики", "Top-N ближайших машин", function()
        local ch_max, val_max = ui.slider_int(
            "Машин на локальной физике (вкл. вас)##opt_n",
            cfg.max_full_sim_cars or 3, 1, 16, "%d маш."
        )
        if ch_max then
            cfg.max_full_sim_cars = val_max
            dirty = true
        end

        local ch_dist, val_dist = ui.slider_float(
            "Макс. дистанция физики (м)##opt_d",
            cfg.max_full_sim_dist or 100.0, 5.0, 100.0, "%.0f м"
        )
        if ch_dist then
            cfg.max_full_sim_dist = val_dist
            dirty = true
        end

        if dirty then
            optimizer.set_config(cfg)
        end

        local remote_slots = math.max(0, (cfg.max_full_sim_cars or 3) - 1)
        ui.spacing(2)
        ui.text_wrapped(
            string.format(
                "Бюджет: ваша машина + %d ближ. (до %.0f м). Защита: +%.0f м / %.1f с (блоков: %d).",
                remote_slots,
                cfg.max_full_sim_dist or 100.0,
                cfg.hysteresis_meters or 8.0,
                cfg.switch_cooldown_sec or 1.8,
                st.anti_flicker_blocks or 0
            ),
            "muted"
        )
    end)

    local summary_sub = string.format("%d сет. | %d физ.", st.total_net_cars or 0, st.full_sim_net_cars or 0)
    ui.card("Машины в комнате", summary_sub, function()
        ui.stat_row({
            { label = "Всего", value = tostring(st.total_net_cars or 0) },
            { label = "Физика", value = tostring(st.full_sim_net_cars or 0), tone = "green" },
            { label = "Снапшот", value = tostring(st.snapshot_net_cars or 0), tone = "accent" },
        })

        local cars = optimizer.get_cars()
        if cars and #cars > 0 then
            ui.separator()
            for i = 1, #cars do
                local c = cars[i]
                local dist_str = c.has_dist and string.format("%.1f м", c.dist_meters or 0) or "? м"
                local badge_txt = "ФИЗИКА"
                local badge_tone = "green"
                if not c.active_full_sim then
                    if c.forced_snapshot then
                        badge_txt = "СНАПШОТ"
                        badge_tone = "accent"
                    else
                        badge_txt = ">100М"
                        badge_tone = "muted"
                    end
                end
                ui.badge(badge_txt, badge_tone)
                ui.same_line(0, 8)
                local cname = c.name or "NetCar"
                if #cname > 20 then cname = cname:sub(1, 18) .. ".." end
                ui.text(string.format("#%d %s | %s", i, cname, dist_str))
            end
        else
            ui.spacing(4)
            ui.text_dim("Нет удалённых сетевых машин (одиночный заезд или лобби).")
        end
    end)

    ui.text_dim("Хоткей: [F5] открыть или скрыть окно оптимизатора")
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "perf",
        title = "Оптимизатор",
        icon = "gauge",
        order = 10,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 416,
        render = render_optimizer_window,
    })
end

events.on_key(function(key, is_down)
    if is_down and key == VK_F5 then
        if imgui and imgui.is_any_item_active and imgui.is_any_item_active() then return end
        if urushi and urushi.toggle_window then
            urushi.toggle_window("perf")
        end
    end
end)

log.info("URUSHI Performance Optimizer module (scripts/perf_optimizer.lua) loaded.")
