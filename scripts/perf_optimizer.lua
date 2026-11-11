-- scripts/perf_optimizer.lua
local T = i18n.t
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
        ui.card(T("Оптимизатор", "Optimizer"), nil, function()
            ui.text_wrapped(T("Модуль optimizer не загружен.", "optimizer module not loaded."), "red")
        end)
        return
    end

    local cfg = optimizer.get_config()
    local st = optimizer.get_stats()
    local dirty = false

    ui.card(T("Физика мультиплеера", "MP Simulation"), nil, function()
        local en_ch, en_val = ui.toggle(T("Включить оптимизатор##opt_en", "Enable Optimizer##opt_en"), cfg.enabled == true)
        if en_ch then
            cfg.enabled = en_val
            dirty = true
        end

        ui.spacing(4)
        local active = detect_preset(cfg)
        local bw = (ui.avail_width() - 12) / 3.0

        if ui.button(T("Ваниль##opt_p_van", "Stock##opt_p_van"), bw, 28, active == "vanilla" and "primary" or "default") then
            apply_preset("vanilla")
            cfg = optimizer.get_config()
            dirty = false
        end
        ui.same_line(0, 6)
        if ui.button(T("Топ-3##opt_p_o3", "Top-3##opt_p_o3"), bw, 28, active == "optimal3" and "primary" or "default") then
            apply_preset("optimal3")
            cfg = optimizer.get_config()
            dirty = false
        end
        ui.same_line(0, 6)
        if ui.button(T("Макс FPS##opt_p_max", "Max FPS##opt_p_max"), bw, 28, active == "maxfps" and "primary" or "default") then
            apply_preset("maxfps")
            cfg = optimizer.get_config()
            dirty = false
        end

        ui.spacing(4)
        ui.stat_row({
            { label = T("Физика", "Full Sim"), value = tostring(st.full_sim_net_cars or 0), tone = "green" },
            { label = T("Снапшот", "Snapshot"), value = tostring(st.snapshot_net_cars or 0), tone = "accent" },
            { label = T("Статус", "Hooks"), value = st.hooks_installed and T("Активны", "Active") or T("Ожидание", "Wait"), tone = st.hooks_installed and "green" or "yellow" },
        })
    end)

    ui.card(T("Повреждения и износ шин", "Damage & Tire Wear"), nil, function()
        local no_dam = (cfg.disable_damage == true) or (carx and carx.get_no_damage and carx.get_no_damage() == true)
        local no_wear = (cfg.disable_tire_wear == true) or (carx and carx.get_no_tire_wear and carx.get_no_tire_wear() == true)

        local ch_dam, val_dam = ui.toggle(T("Без повреждений (кузов, подвеска)##opt_no_dam", "No Car Damage (Body & Suspension)##opt_no_dam"), no_dam)
        if ch_dam then
            cfg.disable_damage = val_dam
            if carx and carx.set_no_damage then carx.set_no_damage(val_dam) end
            dirty = true
        end

        local ch_wear, val_wear = ui.toggle(T("Без износа шин (бесконечный зацеп)##opt_no_wear", "No Tire Wear (Infinite Grip)##opt_no_wear"), no_wear)
        if ch_wear then
            cfg.disable_tire_wear = val_wear
            if carx and carx.set_no_tire_wear then carx.set_no_tire_wear(val_wear) end
            dirty = true
        end

        ui.spacing(4)
        local bw = (ui.avail_width() - 6) / 2.0
        if ui.button(T("Починить авто##opt_rep_car", "Repair Car##opt_rep_car"), bw, 26, "ghost") then
            if carx and carx.repair_car then carx.repair_car() end
        end
        ui.same_line(0, 6)
        if ui.button(T("Сбросить шины##opt_rep_tyres", "Reset Tires##opt_rep_tyres"), bw, 26, "ghost") then
            if carx and carx.reset_tire_wear then carx.reset_tire_wear() end
        end

        local dam_blocks = st.damage_blocks or 0
        local joint_blocks = st.joint_break_blocks or 0
        if no_dam or no_wear then
            ui.spacing(2)
            ui.stat_row({
                { label = T("Кузов", "Body"), value = no_dam and string.format(T("Блок: %d", "Block: %d"), dam_blocks) or T("Уязвим", "Vulnerable"), tone = no_dam and "green" or "yellow" },
                { label = T("Детали", "Parts"), value = no_dam and string.format(T("Блок: %d", "Block: %d"), joint_blocks) or T("Отрыв", "Detaching"), tone = no_dam and "green" or "yellow" },
                { label = T("Шины", "Tires"), value = no_wear and T("100% зацеп", "100% Grip") or T("Износ вкл", "Wearing"), tone = no_wear and "accent" or "muted" },
            })
        end
    end)

    ui.card(T("Бюджет симуляции", "Simulation Budget"), nil, function()
        local ch_max, val_max = ui.slider_int(
            T("Локальная физика (авто)##opt_n", "Full-Sim Cars##opt_n"),
            cfg.max_full_sim_cars or 3, 1, 16, "%d"
        )
        if ch_max then
            cfg.max_full_sim_cars = val_max
            dirty = true
        end

        local ch_dist, val_dist = ui.slider_float(
            T("Макс. дистанция##opt_d", "Max Distance##opt_d"),
            cfg.max_full_sim_dist or 100.0, 5.0, 100.0, T("%.0f м", "%.0f m")
        )
        if ch_dist then
            cfg.max_full_sim_dist = val_dist
            dirty = true
        end

        if dirty then
            optimizer.set_config(cfg)
        end
    end)

    local total = st.total_net_cars or 0
    if total > 0 then
        ui.card(T("Машины в комнате", "Session Cars"), tostring(total), function()
            local cars = optimizer.get_cars()
            if cars and #cars > 0 then
                for i = 1, #cars do
                    local c = cars[i]
                    local dist_str = c.has_dist and string.format(T("%.1f м", "%.1f m"), c.dist_meters or 0) or "?"
                    local badge_txt = T("ФИЗИКА", "SIM")
                    local badge_tone = "green"
                    if not c.active_full_sim then
                        if c.forced_snapshot then
                            badge_txt = T("СНАПШОТ", "SNAP")
                            badge_tone = "accent"
                        else
                            badge_txt = ">100M"
                            badge_tone = "muted"
                        end
                    end
                    ui.badge(badge_txt, badge_tone)
                    ui.same_line(0, 8)
                    local cname = c.name or "NetCar"
                    if #cname > 20 then cname = cname:sub(1, 18) .. ".." end
                    ui.text(string.format("#%d %s · %s", i, cname, dist_str))
                end
            end
        end)
    else
        ui.text_dim(T("Одиночный заезд (нет сетевых машин).", "Solo session (no remote cars)."))
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "perf",
        title = T("Оптимизатор", "Optimizer"),
        icon = "gauge",
        order = 10,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 396,
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

log.info("URUSHI Performance Optimizer module loaded.")
