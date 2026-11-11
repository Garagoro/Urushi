-- scripts/tires_mod.lua
-- URUSHI Body & Tires Module
local T = i18n.t

local active_tab = "body_wear" -- "body_wear", "thermal", "telemetry"

local function render_tires_window()
    if not carx or not carx.get_tyre_config then
        ui.card(T("Кузов и шины", "Body & Tires"), nil, function()
            ui.text_wrapped(T("Модуль carx.tyre не загружен.", "carx.tyre module not loaded."), "red")
        end)
        return
    end

    local cfg = carx.get_tyre_config()
    local live = carx.get_tyre_live()
    local dirty = false

    -- Top Tab Switcher
    local tab_w = (ui.avail_width() - 8) / 3.0
    if ui.button(T("Кузов и Износ##t_tab_1", "Body & Wear##t_tab_1"), tab_w, 28, active_tab == "body_wear" and "primary" or "ghost") then
        active_tab = "body_wear"
    end
    ui.same_line(0, 4)
    if ui.button(T("Термо и Давление##t_tab_2", "Thermal & Press##t_tab_2"), tab_w, 28, active_tab == "thermal" and "primary" or "ghost") then
        active_tab = "thermal"
    end
    ui.same_line(0, 4)
    if ui.button(T("Телеметрия 4WD##t_tab_3", "Telemetry 4WD##t_tab_3"), tab_w, 28, active_tab == "telemetry" and "primary" or "ghost") then
        active_tab = "telemetry"
    end

    ui.spacing(4)

    -- TAB 1: BODY PROTECTION & TIRE WEAR
    if active_tab == "body_wear" then
        ui.card(T("Защита кузова (Деформация)", "Body Protection (Deformation)"), nil, function()
            local no_dam = (carx and carx.get_no_damage and carx.get_no_damage() == true)
            local ch_dam, val_dam = ui.toggle(T("Без повреждений (кузов, бампера, подвеска)##t_no_dam", "No Damage (Body, Bumpers & Suspension)##t_no_dam"), no_dam)
            if ch_dam then
                if carx and carx.set_no_damage then carx.set_no_damage(val_dam) end
            end

            local opt_st = (optimizer and optimizer.get_stats) and optimizer.get_stats() or {}
            local dam_blocks = opt_st.damage_blocks or 0
            local joint_blocks = opt_st.joint_break_blocks or 0

            ui.spacing(2)
            ui.stat_row({
                { label = T("Кузов", "Body"), value = no_dam and string.format(T("Блок: %d", "Block: %d"), dam_blocks) or T("Уязвим", "Vulnerable"), tone = no_dam and "green" or "yellow" },
                { label = T("Детали", "Parts"), value = no_dam and string.format(T("Блок: %d", "Block: %d"), joint_blocks) or T("Отрыв", "Detaching"), tone = no_dam and "green" or "yellow" },
                { label = T("Геометрия", "Geometry"), value = no_dam and T("Идеал", "Locked") or T("Сток", "Stock"), tone = no_dam and "green" or "muted" },
            })
        end)

        ui.card(T("Режим износа шин", "Tire Wear Mode"), nil, function()
            local ch_inf, val_inf = ui.toggle(T("Бесконечные шины (заморозка износа)##tire_inf", "Infinite Tires (Freeze Wear)##tire_inf"), cfg.infinite_wear == true)
            if ch_inf then
                cfg.infinite_wear = val_inf
                dirty = true
            end

            local life_pct = math.floor((cfg.desired_life or 1.0) * 100.0 + 0.5)
            local ch_life, val_life = ui.slider_int(T("Остаток протектора##tire_life", "Tread Life##tire_life"), life_pct, 0, 100, "%d %%")
            if ch_life then
                cfg.desired_life = val_life / 100.0
                dirty = true
            end

            if not cfg.infinite_wear then
                local ch_wm, val_wm = ui.slider_float(T("Скорость износа##tire_wm", "Wear Rate Multiplier##tire_wm"), cfg.wear_multiplier or 1.0, 0.0, 3.0, "%.2fx")
                if ch_wm then
                    cfg.wear_multiplier = val_wm
                    dirty = true
                end
            end

            ui.spacing(4)
            local bw = (ui.avail_width() - 6) * 0.5
            if ui.button(T("100% Свежая резина (80°C)##tire_b_fresh", "100% Fresh Tires (80°C)##tire_b_fresh"), bw, 28, "primary") then
                cfg.desired_life = 1.0
                cfg.lock_temp = true
                cfg.target_temp = 80.0
                if carx.reset_tire_wear then carx.reset_tire_wear() end
                dirty = true
            end
            ui.same_line(0, 6)
            if ui.button(T("Стертая (0% слики)##tire_b_bald", "Bald Tires (0%)##tire_b_bald"), bw, 28, "ghost") then
                cfg.desired_life = 0.0
                dirty = true
            end

            ui.spacing(4)
            if ui.button(T("Сбросить все настройки в стандарт машины (гараж)##tire_btn_reset_all", "Reset All to Car Setup (Garage)##tire_btn_reset_all"), ui.avail_width(), 28, "danger") then
                if carx.reset_tyre_defaults then carx.reset_tyre_defaults() end
                dirty = false
            end
        end)

    -- TAB 2: THERMAL & PRESSURE
    elseif active_tab == "thermal" then
        ui.card(T("Температура шин (°C)", "Tire Temperature (°C)"), nil, function()
            local ch_lt, val_lt = ui.toggle(T("Зафиксировать температуру##tire_lt", "Lock Operating Temperature##tire_lt"), cfg.lock_temp == true)
            if ch_lt then
                cfg.lock_temp = val_lt
                dirty = true
            end

            local ch_tt, val_tt = ui.slider_float(T("Целевая температура##tire_tt", "Target Temp##tire_tt"), cfg.target_temp or 80.0, 20.0, 130.0, "%.0f °C")
            if ch_tt then
                cfg.target_temp = val_tt
                cfg.lock_temp = true
                dirty = true
            end

            ui.spacing(4)
            local bw = (ui.avail_width() - 6) * 0.5
            if ui.button(T("Прогрев 80°C (Дрифт)##tire_warm", "Warm Up 80°C (Drift)##tire_warm"), bw, 26, "primary") then
                cfg.target_temp = 80.0
                cfg.lock_temp = true
                dirty = true
            end
            ui.same_line(0, 6)
            if ui.button(T("Холодные 25°C##tire_cold", "Cold 25°C##tire_cold"), bw, 26, "ghost") then
                cfg.target_temp = 25.0
                cfg.lock_temp = true
                dirty = true
            end

            ui.spacing(4)
            ui.text_wrapped(T("В CarX температура сильно влияет на сцепление: холодная резина (<60°C) 'мылит', перегретая (>105°C) теряет боковой зацеп. Идеальный диапазон: 75°C..85°C.",
                              "In CarX, tyre temp affects friction: cold tyres (<60°C) slide like soap, overheated (>105°C) lose lateral grip. Optimal range: 75°C..85°C."), "accent")
        end)

        ui.card(T("Давление в шинах", "Tire Pressure"), nil, function()
            local base_fp = cfg.baseline_front_pressure or 145.0
            local base_rp = cfg.baseline_rear_pressure or 95.0

            ui.text_dim(string.format(T("Тюнинг из гаража: перед %.0f kPa (%.2f bar) · зад %.0f kPa (%.2f bar)",
                                        "Garage setup: front %.0f kPa (%.2f bar) · rear %.0f kPa (%.2f bar)"),
                                      base_fp, base_fp * 0.01, base_rp, base_rp * 0.01))

            local ch_op, val_op = ui.toggle(T("Кастомное давление##tire_op", "Override Tire Pressure##tire_op"), cfg.override_pressure == true)
            if ch_op then
                cfg.override_pressure = val_op
                if not val_op then
                    cfg.front_pressure = base_fp
                    cfg.rear_pressure = base_rp
                end
                dirty = true
            end

            local ch_fp, val_fp = ui.slider_float(T("Переднее давление##tire_fp", "Front Pressure##tire_fp"), cfg.front_pressure or base_fp, 50.0, 250.0, "%.0f kPa")
            if ch_fp then
                cfg.front_pressure = val_fp
                cfg.override_pressure = true
                dirty = true
            end

            local ch_rp, val_rp = ui.slider_float(T("Заднее давление##tire_rp", "Rear Pressure##tire_rp"), cfg.rear_pressure or base_rp, 50.0, 250.0, "%.0f kPa")
            if ch_rp then
                cfg.rear_pressure = val_rp
                cfg.override_pressure = true
                dirty = true
            end

            ui.spacing(4)
            local bw = (ui.avail_width() - 6) * 0.5
            if ui.button(T("Вернуть тюнинг авто##tire_p_stk", "Restore Car Setup##tire_p_stk"), bw, 26, "ghost") then
                cfg.front_pressure = base_fp
                cfg.rear_pressure = base_rp
                cfg.override_pressure = false
                dirty = true
            end
            ui.same_line(0, 6)
            if ui.button(T("Высокое (200/180)##tire_p_hi", "High Press (200/180)##tire_p_hi"), bw, 26, "ghost") then
                cfg.front_pressure = 200.0
                cfg.rear_pressure = 180.0
                cfg.override_pressure = true
                dirty = true
            end
        end)

    -- TAB 3: LIVE TELEMETRY
    elseif active_tab == "telemetry" then
        if live and live.valid and live.wheels and #live.wheels >= 4 then
            ui.card(T("Телеметрия 4 колес (Live)", "4 Wheels Live Telemetry"), nil, function()
                local names = {
                    T("Переднее левое (FL)", "Front Left (FL)"),
                    T("Переднее правое (FR)", "Front Right (FR)"),
                    T("Заднее левое (RL)", "Rear Left (RL)"),
                    T("Заднее правое (RR)", "Rear Right (RR)"),
                }

                for i = 1, 4 do
                    local w = live.wheels[i]
                    local t_col = "green"
                    local temp = w.temp or 80.0
                    local status_txt = T("Оптимум", "Optimum")
                    if temp < 55.0 then
                        t_col = "blue"
                        status_txt = T("Холодная", "Cold")
                    elseif temp > 105.0 then
                        t_col = "red"
                        status_txt = T("Перегрев", "Overheated")
                    elseif temp > 92.0 then
                        t_col = "yellow"
                        status_txt = T("Горячая", "Hot")
                    end

                    local life_pct = math.floor((w.life or 1.0) * 100.0 + 0.5)
                    local l_col = life_pct > 50 and "green" or (life_pct > 20 and "yellow" or "red")

                    ui.text(names[i])
                    ui.stat_row({
                        { label = T("Протектор", "Tread"), value = string.format("%d%%", life_pct), tone = l_col },
                        { label = T("Темп.", "Temp"), value = string.format("%.1f°C", temp), tone = t_col },
                        { label = T("Давление", "Press"), value = string.format("%.0f kPa", w.pressure_kpa or 0), tone = "muted" },
                        { label = T("Статус", "Status"), value = status_txt, tone = t_col },
                    })
                    if i < 4 then ui.spacing(2) end
                end
            end)
        else
            ui.card(T("Телеметрия", "Telemetry"), nil, function()
                ui.text_dim(T("Машина не загружена на трассе.", "No active car on track."))
            end)
        end
    end

    if dirty then
        carx.set_tyre_config(cfg)
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "tires",
        title = T("Кузов и шины", "Body & Tires"),
        icon = "wheel",
        order = 38,
        default_open = false,
        default_pinned = false,
        x = 350,
        y = 30,
        width = 412,
        render = render_tires_window,
    })
end

_G.tires_mod = {
    set_tab = function(t) active_tab = t end,
    get_tab = function() return active_tab end
}

log.info("URUSHI Body & Tires module loaded.")
