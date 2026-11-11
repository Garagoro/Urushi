-- scripts/steer_assist_mod.lua
-- URUSHI: Мод помощника управления (фикс 90°/баквардов с ограничением разворота на 155° + снятие бага ±15.6°)

local function is_recommended_preset(cfg)
    return cfg.enabled == true
        and math.abs((cfg.flip_angle_deg or 0.0) - 155.0) < 0.5
        and cfg.disable_early_steer_flip == true
        and cfg.fix_slip_gate == true
        and math.abs((cfg.center_scale or 0.0) - 0.30) < 0.01
end

local function render_steer_assist_window()
    if not steer_assist or not steer_assist.get_config then
        ui.card("Помощник руля недоступен", "Модуль C++ steer_assist", function()
            ui.text_wrapped("Модуль C++ steer_assist не загружен (Ctrl+F11 или перезапустите игру).", "red")
        end)
        return
    end

    local cfg = steer_assist.get_config()
    local st = steer_assist.get_stats()
    local dirty = false

    local patch_txt = "Сток"
    local patch_tone = "muted"
    if st.patches_applied then
        patch_txt = string.format("Активен (%.0f°)", cfg.flip_angle_deg or 155.0)
        patch_tone = "green"
    elseif not st.native_found then
        patch_txt = "Ожидание"
        patch_tone = "yellow"
    elseif not st.bytes_verified then
        patch_txt = "Не совпал"
        patch_tone = "red"
    end

    local slip_txt = (cfg.enabled and cfg.fix_slip_gate) and "Снят" or "Сток"
    local slip_tone = (cfg.enabled and cfg.fix_slip_gate) and "green" or "yellow"

    ui.card("Режим помощника DR2", "NativeCarX Assist2", function()
        local ch_en, val_en = ui.toggle("Включить фикс баквардов и 90°##sa_en", cfg.enabled == true)
        if ch_en then
            cfg.enabled = val_en
            dirty = true
        end

        ui.spacing(4)
        local is_rec = is_recommended_preset(cfg)
        local is_van = (cfg.enabled == false)
        local bw = (ui.avail_width() - 6) * 0.5

        if ui.button("155° Баквард + Фикс##sa_p_rec", bw, 28, is_rec and "primary" or "default") then
            steer_assist.set_recommended()
            cfg = steer_assist.get_config()
            dirty = false
        end
        ui.same_line(0, 6)
        if ui.button("Сток DR2 (90°/140°)##sa_p_van", bw, 28, is_van and "primary" or "default") then
            steer_assist.set_vanilla()
            cfg = steer_assist.get_config()
            dirty = false
        end

        ui.spacing(6)
        ui.stat_row({
            { label = "Патч", value = patch_txt, tone = patch_tone },
            { label = "Блок 15.6°", value = slip_txt, tone = slip_tone },
            {
                label = "90° рулём",
                value = (cfg.enabled and cfg.disable_early_steer_flip) and "Выкл" or (cfg.enabled and string.format("%.0f°", cfg.early_steer_flip_deg or 135.0) or "Сток 90°"),
                tone = (cfg.enabled and cfg.disable_early_steer_flip) and "accent" or "muted",
            },
        })
    end)

    ui.card("Порог разворота и снос", "Настройка на лету", function()
        local ch_flip, val_flip = ui.slider_float(
            "Порог переброса 360° / задний ход##sa_flip",
            cfg.flip_angle_deg or 155.0, 90.0, 180.0, "%.0f°"
        )
        if ch_flip then
            cfg.flip_angle_deg = val_flip
            if (cfg.early_steer_flip_deg or 135.0) > val_flip then
                cfg.early_steer_flip_deg = val_flip
            end
            dirty = true
        end

        local ch_early, val_early = ui.toggle(
            "Убрать ранний переброс на 90° (полный руль)##sa_no90",
            cfg.disable_early_steer_flip == true
        )
        if ch_early then
            cfg.disable_early_steer_flip = val_early
            dirty = true
        end

        if not cfg.disable_early_steer_flip then
            local ch_edeg, val_edeg = ui.slider_float(
                "Порог при полном руле (в спин)##sa_edeg",
                cfg.early_steer_flip_deg or 135.0, 90.0, cfg.flip_angle_deg or 155.0, "%.0f°"
            )
            if ch_edeg then
                cfg.early_steer_flip_deg = val_edeg
                dirty = true
            end
        end

        local ch_slip, val_slip = ui.toggle(
            "Снять односторонний блок сноса (>15.6°)##sa_slip",
            cfg.fix_slip_gate == true
        )
        if ch_slip then
            cfg.fix_slip_gate = val_slip
            dirty = true
        end

        if ui.collapsing("Тонкая настройка (Assist2)", false) then
            local ch_cs, val_cs = ui.slider_float(
                "Целевой возврат руля (сток 0.30x)##sa_cs",
                cfg.center_scale or 0.30, 0.15, 0.85, "%.2fx"
            )
            if ch_cs then
                cfg.center_scale = val_cs
                dirty = true
            end
            ui.text_wrapped(
                "0.30x — стандарт DR2. Выше 0.40x сильнее докручивает колёса в контрруление при отпущенном руле.",
                "muted"
            )
        end

        if dirty then
            steer_assist.set_config(cfg)
        end

        ui.spacing(2)
        ui.text_wrapped(
            string.format(
                "До %.0f° помощник не переворачивает вектор на 180°, после %.0f° сразу помогает докрутить 360° и стабилизирует задний ход. Снятие бага ±15.6° возвращает симметричное управление.",
                cfg.flip_angle_deg or 155.0,
                cfg.flip_angle_deg or 155.0
            ),
            "muted"
        )
    end)

    ui.card("Телеметрия руля (Live)", st.has_car and string.format("Assist mode=%d | FF=%d", st.steer_mode or 0, st.steer_ff_mode or 0) or "Ожидание заезда", function()
        if st.has_car then
            local front_sa = st.front_slip_deg or 0.0
            ui.stat_row({
                { label = "Скорость", value = string.format("%.0f км/ч", st.speed_kmh or 0.0), tone = "accent" },
                { label = "Руль", value = string.format("%.1f° / %.0f°", st.steer_deg or 0.0, st.max_steer_deg or 65.0) },
                {
                    label = "Снос передка",
                    value = string.format("%.1f°", front_sa),
                    tone = front_sa > 15.6 and "yellow" or "green",
                },
            })
            ui.spacing(4)
            ui.text_dim(string.format("FFB момент: %.3f | Ассист активен: %s", st.ffb_torque or 0.0, (st.assist_active or 0) ~= 0 and "Да" or "Нет"))
        else
            ui.text_dim("Выйдите на трассу для отображения углов и FFB.")
        end
    end)
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "steer_assist",
        title = "Помощник руля",
        icon = "wheel",
        order = 35,
        default_open = false,
        default_pinned = false,
        x = 264,
        y = 24,
        width = 420,
        render = render_steer_assist_window,
    })
end

log.info("URUSHI Steering Assistant module (scripts/steer_assist_mod.lua) loaded.")
