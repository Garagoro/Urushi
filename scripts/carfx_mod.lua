-- CarFX (CarX-WFX): Custom HLSL Livery & Body Shader Control Panel [F9]
-- Uses WeaponFX-style @param / @color / @tex / @mode directives & hot-reloading.

local VK_F9 = 0x78

local state = {
    shaders = {},
    selected_idx = 1, -- 1-based index for ui.combo
    custom_path = "",
    tex_paths = { [0] = "", [1] = "", [2] = "", [3] = "" },
    status_msg = "Готово. Выберите .hlsl шейдер из папки shaders/.",
}

local function refresh_shader_list()
    if not carfx or not carfx.list_shaders then return end
    state.shaders = carfx.list_shaders() or {}
    if #state.shaders > 0 then
        if state.selected_idx < 1 or state.selected_idx > #state.shaders then
            state.selected_idx = 1
        end
        if state.custom_path == "" then
            state.custom_path = state.shaders[state.selected_idx] or ""
        end
    end
end

refresh_shader_list()

events.on_key(function(vk, is_down)
    if vk == VK_F9 and is_down then
        if imgui and imgui.is_any_item_active and imgui.is_any_item_active() then return end
        if urushi and urushi.toggle_window then
            urushi.toggle_window("carfx")
        end
    end
end)

events.on_update(function(dt)
    if not carfx or not carfx.set_game_input then return end
    local p = carx.get_player_car and carx.get_player_car() or nil
    if p then
        local speed = p.speed_kmh or 0.0
        local steer_rad = math.abs(p.steer_angle or 0.0)
        local steer_deg = steer_rad * 57.2958
        local speed_factor = math.min(math.max(speed / 35.0, 0.0), 1.0)
        local drift_norm = math.min(
            math.max(steer_rad * 2.2 * speed_factor + ((p.handbrake or 0.0) > 0.5 and 0.4 * speed_factor or 0.0), 0.0),
            1.0
        )

        carfx.set_game_input(0, speed)
        carfx.set_game_input(1, p.rpm or 0.0)
        carfx.set_game_input(2, steer_deg)
        carfx.set_game_input(3, drift_norm)
        carfx.set_game_input(4, p.accel or 0.0)
        carfx.set_game_input(5, p.brake or 0.0)
        carfx.set_game_input(6, p.gear or 0)
    end
end)

local function render_carfx_window()
    if not carfx or not carfx.get_status then
        ui.card("CarFX недоступен", "Модуль C++ carfx", function()
            ui.text_wrapped("Модуль carfx C++ не инициализирован.", "red")
        end)
        return
    end

    local st = carfx.get_status(0)
    local init_ok = st.initialized and st.hooked_draw

    ui.card("DX11 и кузов игрока", "Перехват DrawIndexed*", function()
        ui.stat_row({
            {
                label = "Хук DX11",
                value = init_ok and "Активен" or "Ожидание",
                tone = init_ok and "green" or "yellow",
            },
            {
                label = "Совпадений",
                value = string.format("%d / %d", st.matched_last_frame or 0, st.total_draws or 0),
                tone = (st.matched_last_frame or 0) > 0 and "accent" or "muted",
            },
            {
                label = "Кузов",
                value = st.body_valid and "Найден" or "Не на трассе",
                tone = st.body_valid and "green" or "muted",
            },
        })

        ui.spacing(4)
        local en_ch, new_en = ui.toggle("Включить CarFX на кузов игрока##cfx_en", st.enabled == true)
        if en_ch then
            carfx.set_enabled(new_en)
        end

        if st.body_valid then
            local warn_str = (st.body_albedo_default and st.body_packed_default)
                and " (дефолт: нет наклеек!)" or ""
            ui.text_wrapped(
                string.format(
                    "Albedo=%s (0x%X) | Packed=%s (0x%X)%s",
                    st.body_albedo_srv or "?", st.body_albedo_id or 0,
                    st.body_packed_srv or "?", st.body_packed_id or 0,
                    warn_str
                ),
                "muted"
            )
        end
        if (st.last_rt_w or 0) > 0 then
            ui.text_dim(string.format(
                "RT %dx%d fmt=%d | slots=0x%X | PS=%s",
                st.last_rt_w or 0, st.last_rt_h or 0, st.last_rt_fmt or 0,
                st.last_slot_mask or 0, st.last_game_ps or "0"
            ))
        end
    end)

    ui.card("Шейдер (shaders/*.hlsl)", st.compiled and "Скомпилирован" or "Не загружен", function()
        if #state.shaders == 0 then
            refresh_shader_list()
        end

        if #state.shaders > 0 then
            local c_ch, new_idx = ui.combo("Выбор .hlsl##cfx_combo", state.selected_idx, state.shaders)
            if c_ch then
                state.selected_idx = new_idx
                local picked = state.shaders[new_idx]
                if picked then
                    state.custom_path = picked
                    local ok, err = carfx.load(picked, 0)
                    state.status_msg = ok and ("Загружен: " .. picked) or ("Ошибка: " .. tostring(err))
                end
            end
        end

        local p_ch, new_p = ui.input_text("Путь к .hlsl##cfx_path", state.custom_path, 256)
        if p_ch then
            state.custom_path = new_p
        end

        ui.spacing(4)
        local bw = (ui.avail_width() - 8) * 0.5
        if ui.button("Загрузить##cfx_load", bw, 28, "primary") then
            if state.custom_path ~= "" then
                local ok, err = carfx.load(state.custom_path, 0)
                state.status_msg = ok and ("Загружен: " .. state.custom_path) or ("Ошибка: " .. tostring(err))
            end
        end
        ui.same_line(0, 8)
        if ui.button("Перезагрузить##cfx_rel", bw, 28) then
            local ok, err = carfx.reload(0)
            state.status_msg = ok and "Шейдер перекомпилирован" or ("Ошибка: " .. tostring(err))
        end

        if ui.button("Снять шейдер##cfx_unl", bw, 28, "danger") then
            carfx.unload(0)
            state.status_msg = "Шейдер отключён."
        end
        ui.same_line(0, 8)
        if ui.button("Обновить список##cfx_ref", bw, 28, "ghost") then
            refresh_shader_list()
            state.status_msg = string.format("Найдено шейдеров: %d", #state.shaders)
        end

        ui.spacing(6)
        ui.text_dim("Режим наложения (@mode)")
        local cur_m = (st.active_mode or 0) + 1
        local m_ch, new_m = ui.segmented("cfx_mode", cur_m, { "Авто", "Replace", "Overlay" })
        if m_ch then
            carfx.set_mode(new_m - 1, 0)
        end

        if carfx.set_stealth then
            ui.spacing(2)
            ui.text_dim("Эффект прозрачности (@stealth)")
            local cur_s = (st.active_stealth or 0) + 1
            local s_ch, new_s = ui.segmented("cfx_stealth", cur_s, { "Авто", "Стекло", "Невидимка" })
            if s_ch then
                carfx.set_stealth(new_s - 1, 0)
            end
        end

        ui.spacing(4)
        if st.compiled then
            local eff_m = (st.resolved_mode == 2) and "OVERLAY" or "REPLACE"
            local short_name = (st.shader_path or ""):match("([^\\/]+)$") or (st.shader_path or "")
            ui.badge(eff_m, "accent")
            ui.same_line(0, 8)
            ui.text_wrapped(string.format("%s (#%d)", short_name, st.compiles or 0))
            if (st.scene_copies or 0) > 0 or (st.depth_copies or 0) > 0 then
                ui.text_dim(string.format("Копии фона: %d | глубины: %d", st.scene_copies or 0, st.depth_copies or 0))
            end
        end

        if st.last_error and st.last_error ~= "" then
            ui.text_wrapped("Ошибка компиляции: " .. st.last_error, "red")
        else
            ui.text_wrapped(state.status_msg, "muted")
        end
    end)

    if carfx.get_spatial then
        local sp = carfx.get_spatial()
        if sp and sp.car_fwd and (sp.car_fwd.valid or 0) > 0.5 then
            ui.card("Сенсоры близости (cBuffer b13)", "Перед / Дверь / Зад / Авто (м)", function()
                local fmt_d = function(v)
                    return (v and v < 900.0) and string.format("%.2fм", v) or "—"
                end
                ui.text(string.format(
                    "Бамперы: Перед %s | Зад %s",
                    fmt_d(sp.wall_front), fmt_d(sp.wall_rear)
                ))
                ui.text(string.format(
                    "Лево  (Пер / Дверь / Зад): %s / %s / %s",
                    fmt_d(sp.wall_front_left), fmt_d(sp.wall_door_left), fmt_d(sp.wall_rear_left)
                ))
                ui.text(string.format(
                    "Право (Пер / Дверь / Зад): %s / %s / %s",
                    fmt_d(sp.wall_front_right), fmt_d(sp.wall_door_right), fmt_d(sp.wall_rear_right)
                ))
                local deg = sp.drift_deg or 0.0
                local p65 = math.min((deg / 65.0) * 100.0, 100.0)
                ui.text(string.format("Угол заноса: %.1f° (%.0f%% от 65°)", deg, p65))
                local near_gap = sp.near_car_pos and sp.near_car_pos.gap or 999.0
                if near_gap < 900.0 then
                    ui.text_colored(0.25, 0.90, 1.0, 1.0, string.format("Ближайшее авто / болванка: %.2f м", near_gap))
                else
                    ui.text_dim("Ближайшее авто / болванка: нет в радиусе 40 м")
                end
            end)
        end
    end

    if st.params and #st.params > 0 then
        ui.card("Параметры шейдера", "@param / @color (" .. tostring(#st.params) .. ")", function()
            for idx = 1, #st.params do
                local pm = st.params[idx]
                local vals = pm.values or {}
                local lo = pm.lo or 0.0
                local hi = pm.hi or 1.0
                if hi <= lo then
                    lo = -10.0
                    hi = 10.0
                end

                if pm.desc and pm.desc ~= "" then
                    ui.text(string.format("%s — %s", pm.name, pm.desc))
                else
                    ui.text(pm.name)
                end

                if pm.color and pm.count >= 3 then
                    local r = vals[1] or 1.0
                    local g = vals[2] or 1.0
                    local b = vals[3] or 1.0
                    local a = vals[4] or 1.0
                    ui.same_line(0, 8)
                    ui.text_colored(r, g, b, 1.0, "[ЦВЕТ]")

                    local c1, nr = ui.slider_float(pm.name .. ".R##cfx_pr_" .. idx, r, lo, hi)
                    local c2, ng = ui.slider_float(pm.name .. ".G##cfx_pg_" .. idx, g, lo, hi)
                    local c3, nb = ui.slider_float(pm.name .. ".B##cfx_pb_" .. idx, b, lo, hi)
                    local changed = c1 or c2 or c3
                    local na = a
                    if pm.count == 4 then
                        local c4
                        c4, na = ui.slider_float(pm.name .. ".A##cfx_pa_" .. idx, a, lo, hi)
                        changed = changed or c4
                    end
                    if changed then
                        if pm.count == 4 then
                            carfx.set_param(pm.name, nr, ng, nb, na)
                        else
                            carfx.set_param(pm.name, nr, ng, nb)
                        end
                    end
                else
                    local comp_names = { "x", "y", "z", "w" }
                    local out_v = { vals[1] or 0, vals[2] or 0, vals[3] or 0, vals[4] or 0 }
                    local any_changed = false
                    for c = 1, (pm.count or 1) do
                        local lbl = (pm.count == 1) and pm.name or (pm.name .. "." .. comp_names[c])
                        local ch, nv = ui.slider_float(lbl .. "##cfx_pv_" .. idx .. "_" .. c, out_v[c], lo, hi)
                        if ch then
                            out_v[c] = nv
                            any_changed = true
                        end
                    end
                    if any_changed then
                        if pm.count == 1 then carfx.set_param(pm.name, out_v[1])
                        elseif pm.count == 2 then carfx.set_param(pm.name, out_v[1], out_v[2])
                        elseif pm.count == 3 then carfx.set_param(pm.name, out_v[1], out_v[2], out_v[3])
                        else carfx.set_param(pm.name, out_v[1], out_v[2], out_v[3], out_v[4])
                        end
                    end
                end
                if idx < #st.params then ui.spacing(4) end
            end
        end)
    end

    if st.textures and #st.textures > 0 then
        ui.card("Текстуры шейдера", "@tex 0..3", function()
            local bw = (ui.avail_width() - 8) * 0.5
            for idx = 1, #st.textures do
                local tx = st.textures[idx]
                local slot = tx.slot or 0
                ui.text(string.format("t%d [%s] %s", 33 + slot, tx.name, tx.desc or ""))
                local tch, np = ui.input_text("Файл##cfx_tex_in_" .. slot, state.tex_paths[slot] or "", 256)
                if tch then state.tex_paths[slot] = np end
                if ui.button("Задать##cfx_tex_set_" .. slot, bw, 26, "primary") then
                    local ok, err = carfx.set_tex(slot, state.tex_paths[slot] or "")
                    state.status_msg = ok and ("Текстура загружена в slot " .. slot) or ("Ошибка текстуры: " .. tostring(err))
                end
                ui.same_line(0, 8)
                if ui.button("Очистить##cfx_tex_clr_" .. slot, bw, 26, "ghost") then
                    carfx.clear_tex(slot)
                    state.status_msg = "Текстура сброшена для slot " .. slot
                end
                if idx < #st.textures then ui.separator() end
            end
        end)
    end

    ui.text_wrapped("Hot-reload: сохраните любой .hlsl в shaders/ — обновится автоматически. [F9]", "muted")
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "carfx",
        title = "CarFX Шейдеры",
        icon = "shader",
        order = 20,
        default_open = false,
        default_pinned = false,
        x = 696,
        y = 24,
        width = 436,
        render = render_carfx_window,
    })
end

log.info("CarFX module (scripts/carfx_mod.lua) loaded.")
