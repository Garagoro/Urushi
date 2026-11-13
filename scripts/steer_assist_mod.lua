-- scripts/steer_assist_mod.lua
local T = i18n.t

local function is_recommended_preset(cfg)
    return cfg.enabled == true
        and math.abs((cfg.flip_angle_deg or 0.0) - 155.0) < 0.5
        and cfg.disable_early_steer_flip == true
        and cfg.fix_slip_gate == true
        and math.abs((cfg.center_scale or 0.0) - 0.30) < 0.01
end

local sa_settings = (steer_assist and steer_assist.get_config and steer_assist.get_config()) or {}
if config and config.bind then
    sa_settings = config.bind("steer_assist", sa_settings)
end
if steer_assist and steer_assist.set_config and type(sa_settings) == "table" and sa_settings.enabled ~= nil then
    steer_assist.set_config(sa_settings)
end

local function render_steer_assist_window()
    if not steer_assist or not steer_assist.get_config then
        ui.card(T("Помощник руля", "Steering Assist"), nil, function()
            ui.text_wrapped(T("Модуль steer_assist не загружен.", "steer_assist module not loaded."), "red")
        end)
        return
    end

    local cfg = sa_settings
    local st = steer_assist.get_stats()
    local dirty = false

    local patch_txt = T("Сток", "Stock")
    local patch_tone = "muted"
    if st.patches_applied then
        patch_txt = string.format("%.0f°", cfg.flip_angle_deg or 155.0)
        patch_tone = "green"
    elseif not st.native_found then
        patch_txt = T("Ожидание", "Waiting")
        patch_tone = "yellow"
    elseif not st.bytes_verified then
        patch_txt = T("Не совпал", "Mismatch")
        patch_tone = "red"
    end

    local slip_txt = (cfg.enabled and cfg.fix_slip_gate) and T("Снят", "Fixed") or T("Сток", "Stock")
    local slip_tone = (cfg.enabled and cfg.fix_slip_gate) and "green" or "yellow"
    local snap_txt = (cfg.enabled and cfg.disable_early_steer_flip)
        and T("Выкл", "Off")
        or (cfg.enabled and string.format("%.0f°", cfg.early_steer_flip_deg or 135.0) or "90°")

    ui.card(T("Режим помощника DR2", "DR2 Steering Assist"), nil, function()
        local ch_en, val_en = ui.toggle(T("Включить фикс помощника##sa_en", "Enable Assist Fix##sa_en"), cfg.enabled == true)
        if ch_en then
            cfg.enabled = val_en
            steer_assist.set_config(cfg)
            if config then config.mark_dirty() end
        end

        ui.spacing(4)
        local is_rec = is_recommended_preset(cfg)
        local is_van = (cfg.enabled == false)
        local bw = (ui.avail_width() - 6) * 0.5

        if ui.button(T("155° Баквард##sa_p_rec", "155° Backwards##sa_p_rec"), bw, 28, is_rec and "primary" or "default") then
            steer_assist.set_recommended()
            local cur = steer_assist.get_config()
            for k, v in pairs(cur) do sa_settings[k] = v end
            if config then config.mark_dirty() end
        end
        ui.same_line(0, 6)
        if ui.button(T("Сток DR2##sa_p_van", "Stock DR2##sa_p_van"), bw, 28, is_van and "primary" or "default") then
            steer_assist.set_vanilla()
            local cur = steer_assist.get_config()
            for k, v in pairs(cur) do sa_settings[k] = v end
            if config then config.mark_dirty() end
        end

        ui.spacing(4)
        ui.stat_row({
            { label = T("Порог", "Flip"), value = patch_txt, tone = patch_tone },
            { label = T("Блок ±15.6°", "±15.6° Bug"), value = slip_txt, tone = slip_tone },
            { label = T("Срыв 90°", "90° Snap"), value = snap_txt, tone = (cfg.enabled and cfg.disable_early_steer_flip) and "accent" or "muted" },
        })
    end)

    ui.card(T("Пороги и поведение", "Thresholds"), nil, function()
        local ch_flip, val_flip = ui.slider_float(
            T("Переброс 360° / задний ход##sa_flip", "360° / Reverse Flip Angle##sa_flip"),
            cfg.flip_angle_deg or 155.0, 90.0, 180.0, "%.0f°"
        )
        if ch_flip then
            cfg.flip_angle_deg = val_flip
            if (cfg.early_steer_flip_deg or 135.0) > val_flip then
                cfg.early_steer_flip_deg = val_flip
            end
            steer_assist.set_config(cfg)
            if config then config.mark_dirty() end
        end

        local ch_early, val_early = ui.toggle(
            T("Убрать срыв на 90°##sa_no90", "Disable 90° Spin Snap##sa_no90"),
            cfg.disable_early_steer_flip == true
        )
        if ch_early then
            cfg.disable_early_steer_flip = val_early
            steer_assist.set_config(cfg)
            if config then config.mark_dirty() end
        end

        if not cfg.disable_early_steer_flip then
            local ch_edeg, val_edeg = ui.slider_float(
                T("Срыв при полном руле##sa_edeg", "Full-Steer Spin Flip##sa_edeg"),
                cfg.early_steer_flip_deg or 135.0, 90.0, cfg.flip_angle_deg or 155.0, "%.0f°"
            )
            if ch_edeg then
                cfg.early_steer_flip_deg = val_edeg
                steer_assist.set_config(cfg)
                if config then config.mark_dirty() end
            end
        end

        local ch_slip, val_slip = ui.toggle(
            T("Снять блок сноса ±15.6°##sa_slip", "Fix ±15.6° Asymmetric Slip Lock##sa_slip"),
            cfg.fix_slip_gate == true
        )
        if ch_slip then
            cfg.fix_slip_gate = val_slip
            steer_assist.set_config(cfg)
            if config then config.mark_dirty() end
        end

        if ui.collapsing(T("Тонкая настройка##sa_adv", "Advanced##sa_adv"), false) then
            local ch_cs, val_cs = ui.slider_float(
                T("Возврат ассиста (сток 0.30x)##sa_cs", "Centering Ratio (Stock 0.30x)##sa_cs"),
                cfg.center_scale or 0.30, 0.15, 0.85, "%.2fx"
            )
            if ch_cs then
                cfg.center_scale = val_cs
                steer_assist.set_config(cfg)
                if config then config.mark_dirty() end
            end
        end
    end)

    ui.card(T("Телеметрия", "Live Telemetry"), nil, function()
        if st.has_car then
            local front_sa = st.front_slip_deg or 0.0
            ui.stat_row({
                { label = T("Скорость", "Speed"), value = string.format(T("%.0f км/ч", "%.0f km/h"), st.speed_kmh or 0.0), tone = "accent" },
                { label = T("Руль", "Steer"), value = string.format("%.1f° / %.0f°", st.steer_deg or 0.0, st.max_steer_deg or 65.0) },
                { label = T("Снос", "Front Slip"), value = string.format("%.1f°", front_sa), tone = front_sa > 15.6 and "yellow" or "green" },
            })
        else
            ui.text_dim(T("Выйдите на трассу для телеметрии.", "Enter a track for live telemetry."))
        end
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "steer_assist",
        title = T("Помощник руля", "Steering Assist"),
        icon = "wheel",
        order = 35,
        default_open = false,
        default_pinned = false,
        x = 716,
        y = 24,
        width = 408,
        settings = sa_settings,
        render = render_steer_assist_window,
    })
end

log.info("URUSHI Steering Assistant module loaded.")
