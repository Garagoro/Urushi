-- CarFX (CarX-WFX): Custom HLSL Livery & Body Shader Control Panel [F9]
-- Uses WeaponFX-style @param / @color / @tex / @mode directives & hot-reloading.

local VK_F9 = 0x78

local state = {
    is_open = true,
    shaders = {},
    selected_idx = 1,       -- 1-based index for imgui.combo
    custom_path = "",
    mode_idx = 0,           -- 0: Auto, 1: Replace, 2: Overlay
    tex_paths = { [0] = "", [1] = "", [2] = "", [3] = "" },
    status_msg = "Готово. Нажмите кнопку ниже или выберите .hlsl шейдер из shaders/.",
    last_scan_ms = 0,
    prev_f9 = false
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
        state.is_open = not state.is_open
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
        local drift_norm = math.min(math.max(steer_rad * 2.2 * speed_factor + ((p.handbrake or 0.0) > 0.5 and 0.4 * speed_factor or 0.0), 0.0), 1.0)

        carfx.set_game_input(0, speed)
        carfx.set_game_input(1, p.rpm or 0.0)
        carfx.set_game_input(2, steer_deg)
        carfx.set_game_input(3, drift_norm)
        carfx.set_game_input(4, p.accel or 0.0)
        carfx.set_game_input(5, p.brake or 0.0)
        carfx.set_game_input(6, p.gear or 0)
    end
end)

events.on_render(function()
    if not state.is_open then return end
    if imgui.is_menu_open and not imgui.is_menu_open() then return end

    if imgui.set_next_window_size then
        imgui.set_next_window_size(540, 620, 2)
    end

    local is_visible, should_open = imgui.begin_window("🎨 CarFX (WFX Livery Shaders) [F9]", true)
    if not is_visible then
        imgui.end_window()
        state.is_open = should_open
        return
    end

    if not carfx or not carfx.get_status then
        imgui.text_colored(1.0, 0.35, 0.35, 1.0, "Модуль carfx C++ не инициализирован.")
        imgui.end_window()
        state.is_open = should_open
        return
    end

    local st = carfx.get_status(0)

    -- Status Header
    imgui.text_colored(0.4, 0.85, 1.0, 1.0, "Статус перехвата DX11 и ливреи игрока:")
    imgui.separator()

    local init_ok = st.initialized and st.hooked_draw
    if init_ok then
        imgui.text_colored(0.25, 1.0, 0.45, 1.0,
            string.format("[DX11 OK] DrawIndexed* хукнут | Matched draws/frame: %d (из %d)",
            st.matched_last_frame or 0, st.total_draws or 0))
    else
        imgui.text_colored(1.0, 0.45, 0.35, 1.0, "[ОЖИДАНИЕ] DX11 / DrawIndexed хук ещё не активен")
    end

    if st.body_valid then
        local warn_str = (st.body_albedo_default and st.body_packed_default)
            and " [ВНИМАНИЕ: дефолтные текстуры, нет наклеек!]" or ""
        imgui.text_colored(0.25, 1.0, 0.45, 1.0,
            string.format("Кузов (Body): Albedo=%s (id=0x%X, def=%s) | Packed=%s (id=0x%X)%s",
            st.body_albedo_srv, st.body_albedo_id or 0, tostring(st.body_albedo_default),
            st.body_packed_srv, st.body_packed_id or 0, warn_str))
    else
        imgui.text_colored(0.9, 0.75, 0.3, 1.0, "Кузов (Body): зайдите на трассу на своей машине...")
    end

    if (st.last_rt_w or 0) > 0 then
        imgui.text(string.format("Последний draw: slots=0x%X | RT=%dx%d fmt=%d | GamePS=%s",
            st.last_slot_mask or 0, st.last_rt_w or 0, st.last_rt_h or 0, st.last_rt_fmt or 0, st.last_game_ps or "0"))
    end

    imgui.spacing()
    imgui.separator()
    imgui.text_colored(1.0, 0.85, 0.3, 1.0, "Управление шейдером (shaders/*.hlsl):")

    local en_changed, new_en = imgui.checkbox("Включить CarFX на кузов игрока", st.enabled == true)
    if en_changed then
        carfx.set_enabled(new_en)
    end

    -- Shader Combo
    if #state.shaders == 0 then
        refresh_shader_list()
    end

    if #state.shaders > 0 and imgui.combo then
        local c_changed, new_idx = imgui.combo("Выбор .hlsl", state.selected_idx, state.shaders)
        if c_changed then
            state.selected_idx = new_idx
            local picked = state.shaders[new_idx]
            if picked then
                state.custom_path = picked
                local ok, err = carfx.load(picked, 0)
                state.status_msg = ok and ("Загружен: " .. picked) or ("Ошибка: " .. tostring(err))
            end
        end
    end

    if imgui.input_text then
        local p_changed, new_p = imgui.input_text("Путь к .hlsl", state.custom_path, 256)
        if p_changed then
            state.custom_path = new_p
        end
    end

    if imgui.button("Загрузить", 120, 28) then
        if state.custom_path ~= "" then
            local ok, err = carfx.load(state.custom_path, 0)
            state.status_msg = ok and ("Загружен: " .. state.custom_path) or ("Ошибка: " .. tostring(err))
        end
    end
    imgui.same_line()
    if imgui.button("Перезагрузить", 120, 28) then
        local ok, err = carfx.reload(0)
        state.status_msg = ok and "Шейдер перекомпилирован" or ("Ошибка: " .. tostring(err))
    end
    imgui.same_line()
    if imgui.button("Снять шейдер", 120, 28) then
        carfx.unload(0)
        state.status_msg = "Шейдер отключён."
    end
    imgui.same_line()
    if imgui.button("Обновить список", 130, 28) then
        refresh_shader_list()
        state.status_msg = string.format("Найдено шейдеров: %d", #state.shaders)
    end

    -- Mode & Stealth Overrides
    if imgui.combo then
        local cur_m = (st.active_mode or 0) + 1
        local m_changed, new_m = imgui.combo("Режим (@mode)", cur_m, { "Авто (из @mode)", "Replace (замена PS)", "Overlay (поверх ливреи)" })
        if m_changed then
            carfx.set_mode(new_m - 1, 0)
        end

        local cur_s = (st.active_stealth or 0) + 1
        local s_changed, new_s = imgui.combo("Эффект (@stealth)", cur_s, { "Авто (из @stealth)", "Стекло (просвет внутренностей)", "Невидимка (только кузов)" })
        if s_changed and carfx.set_stealth then
            carfx.set_stealth(new_s - 1, 0)
        end
    end

    if st.compiled then
        local eff_m = (st.resolved_mode == 2) and "OVERLAY" or "REPLACE"
        imgui.text_colored(0.3, 1.0, 0.6, 1.0,
            string.format("Активен: %s [%s] | Stealth=%s Scene=%s Livery=%s (compiles=%d)",
            st.shader_path or "", eff_m,
            tostring(st.resolved_stealth), tostring(st.uses_scene), tostring(st.uses_livery), st.compiles or 0))
        if (st.scene_copies or 0) > 0 or (st.depth_copies or 0) > 0 then
            imgui.text(string.format("Копии фона: %d | Копии глубины: %d", st.scene_copies or 0, st.depth_copies or 0))
        end
    end

    if st.last_error and st.last_error ~= "" then
        imgui.text_colored(1.0, 0.3, 0.3, 1.0, "Ошибка компиляции:")
        if imgui.text_wrapped then
            imgui.text_wrapped(st.last_error)
        else
            imgui.text(st.last_error)
        end
    else
        imgui.text_colored(0.75, 0.8, 0.9, 1.0, state.status_msg)
    end

    -- Dynamic @param & @color UI
    if st.params and #st.params > 0 then
        imgui.spacing()
        imgui.separator()
        imgui.text_colored(0.6, 0.9, 1.0, 1.0, "Параметры шейдера (@param / @color):")
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
                imgui.text(string.format("%s — %s", pm.name, pm.desc))
            else
                imgui.text(pm.name)
            end

            if pm.color and pm.count >= 3 then
                local r = vals[1] or 1.0
                local g = vals[2] or 1.0
                local b = vals[3] or 1.0
                local a = vals[4] or 1.0
                imgui.same_line()
                imgui.text_colored(r, g, b, 1.0, "[ЦВЕТ]")

                local c1, nr = imgui.slider_float(pm.name .. ".R", r, lo, hi)
                local c2, ng = imgui.slider_float(pm.name .. ".G", g, lo, hi)
                local c3, nb = imgui.slider_float(pm.name .. ".B", b, lo, hi)
                local changed = c1 or c2 or c3
                local na = a
                if pm.count == 4 then
                    local c4
                    c4, na = imgui.slider_float(pm.name .. ".A", a, lo, hi)
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
                    local ch, nv = imgui.slider_float(lbl, out_v[c], lo, hi)
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
        end
    end

    -- Dynamic @tex UI
    if st.textures and #st.textures > 0 then
        imgui.spacing()
        imgui.separator()
        imgui.text_colored(0.9, 0.7, 1.0, 1.0, "Пользовательские текстуры (@tex 0..3):")
        for idx = 1, #st.textures do
            local tx = st.textures[idx]
            local slot = tx.slot or 0
            imgui.text(string.format("t%d [%s] %s", 33 + slot, tx.name, tx.desc or ""))
            if imgui.input_text then
                local tch, np = imgui.input_text("Файл##tex" .. slot, state.tex_paths[slot] or "", 256)
                if tch then state.tex_paths[slot] = np end
            end
            if imgui.button("Задать##tex" .. slot, 110, 24) then
                local ok, err = carfx.set_tex(slot, state.tex_paths[slot] or "")
                state.status_msg = ok and ("Текстура загружена в slot " .. slot) or ("Ошибка текстуры: " .. tostring(err))
            end
            imgui.same_line()
            if imgui.button("Очистить##tex" .. slot, 110, 24) then
                carfx.clear_tex(slot)
                state.status_msg = "Текстура сброшена для slot " .. slot
            end
        end
    end

    imgui.spacing()
    imgui.text_colored(0.5, 0.5, 0.55, 1.0, "Hot-reload: сохраните любой .hlsl в shaders/ и он обновится автоматически.")
    imgui.end_window()
    state.is_open = should_open
end)

log.info("CarFX module (scripts/carfx_mod.lua) loaded.")
