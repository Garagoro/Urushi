-- scripts/carfx_mod.lua
local T = i18n.t
local VK_F9 = 0x78

local state = {
    shaders = {},
    selected_idx = 1,
    custom_path = "",
    tex_paths = { [0] = "", [1] = "", [2] = "", [3] = "" },
    status_msg = "",
    status_tone = "muted",
}

local function refresh_shader_list()
    if carfx and carfx.list_shaders then
        state.shaders = carfx.list_shaders() or {}
        if state.selected_idx > #state.shaders then
            state.selected_idx = 1
        end
        if state.custom_path == "" and state.shaders[state.selected_idx] then
            state.custom_path = state.shaders[state.selected_idx]
        end
    end
end

refresh_shader_list()

events.on_key(function(vk, is_down)
    if not keybinds and vk == VK_F9 and is_down then
        if imgui and imgui.is_any_item_active and imgui.is_any_item_active() then return end
        if urushi and urushi.toggle_window then
            urushi.toggle_window("carfx")
        end
    end
end)

events.on_update(function(dt)
    if keybinds and keybinds.is_pressed and keybinds.is_pressed("carfx_menu") then
        if urushi and urushi.toggle_window then
            urushi.toggle_window("carfx")
        end
    end
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
        ui.card("CarFX", nil, function()
            ui.text_wrapped(T("Модуль CarFX не инициализирован.", "CarFX module not initialized."), "red")
        end)
        return
    end

    local st = carfx.get_status(0)
    local init_ok = st.initialized and st.hooked_draw
    local short_name = (st.shader_path or ""):match("([^\\/]+)$") or ""
    local eff_mode = st.compiled and ((st.resolved_mode == 2) and "Overlay" or "Replace") or T("Выкл", "None")

    ui.card(T("Внутренние части", "Internals"), T("Двигатель, подвеска, салон (свой шейдер)", "Engine, suspension, interior (own shader)"), function()
        local ist = carfx.get_status(1)
        local iname = (ist.shader_path or ""):match("([^\\/]+)$") or ""
        ui.stat_row({
            {
                label = T("Шейдер", "Shader"),
                value = ist.compiled and iname or T("Не выбран", "None"),
                tone = ist.compiled and "accent" or "muted",
            },
            {
                label = T("Деталей в кадре", "Draws / frame"),
                value = tostring(ist.matched_last_frame or 0),
                tone = (ist.matched_last_frame or 0) > 0 and "green" or "muted",
            },
        })
        if #state.shaders > 0 then
            local names = { T("(нет)", "(none)") }
            local cur = 1
            for i, name in ipairs(state.shaders) do
                names[#names + 1] = name
                if ist.compiled and name == iname then cur = i + 1 end
            end
            local ch, idx = ui.combo(T("Шейдер внутренностей##cfx_int", "Internals shader##cfx_int"), cur, names)
            if ch then
                if idx == 1 then
                    carfx.unload(1)
                else
                    local ok, err = carfx.load(names[idx], 1)
                    state.status_msg = ok and "" or tostring(err)
                    state.status_tone = ok and "muted" or "red"
                end
            end
        end
    end)

    ui.card(T("Шейдер кузова", "Body Shader"), nil, function()
        local en_ch, new_en = ui.toggle(T("Включить CarFX##cfx_en", "Enable CarFX##cfx_en"), st.enabled == true)
        if en_ch then
            carfx.set_enabled(new_en)
        end

        ui.spacing(4)
        ui.stat_row({
            {
                label = T("Кузов", "Body"),
                value = st.body_valid and T("Готов", "Ready") or T("Вне трассы", "Off Track"),
                tone = st.body_valid and "green" or "muted",
            },
            {
                label = T("Шейдер", "Shader"),
                value = st.compiled and T("Активен", "Active") or T("Не выбран", "None"),
                tone = st.compiled and "accent" or "muted",
            },
            {
                label = T("Режим", "Mode"),
                value = eff_mode,
                tone = st.compiled and "default" or "muted",
            },
        })

        if #state.shaders == 0 then
            refresh_shader_list()
        end
        if short_name ~= "" and state.shaders[state.selected_idx] ~= short_name then
            for i, name in ipairs(state.shaders) do
                if name == short_name then
                    state.selected_idx = i
                    state.custom_path = name
                    break
                end
            end
        end

        ui.spacing(4)
        if #state.shaders > 0 then
            local c_ch, new_idx = ui.combo(T("Выбор .hlsl##cfx_combo", "Shader (.hlsl)##cfx_combo"), state.selected_idx, state.shaders)
            if c_ch then
                state.selected_idx = new_idx
                local picked = state.shaders[new_idx]
                if picked then
                    state.custom_path = picked
                    local ok, err = carfx.load(picked, 0)
                    state.status_msg = ok and "" or tostring(err)
                    state.status_tone = ok and "muted" or "red"
                end
            end
        end

        ui.spacing(2)
        local bw3 = (ui.avail_width() - 12) / 3.0
        if ui.button(T("Загрузить##cfx_load", "Load##cfx_load"), bw3, 28, "primary") then
            if state.custom_path ~= "" then
                local ok, err = carfx.load(state.custom_path, 0)
                state.status_msg = ok and "" or tostring(err)
                state.status_tone = ok and "muted" or "red"
            end
        end
        ui.same_line(0, 6)
        if ui.button(T("Релоад##cfx_rel", "Reload##cfx_rel"), bw3, 28) then
            local ok, err = carfx.reload(0)
            state.status_msg = ok and "" or tostring(err)
            state.status_tone = ok and "muted" or "red"
        end
        ui.same_line(0, 6)
        if ui.button(T("Снять##cfx_unl", "Clear##cfx_unl"), bw3, 28, "danger") then
            carfx.unload(0)
            state.status_msg = ""
        end

        ui.spacing(6)
        ui.text_dim(T("Режим наложения", "Blend Mode"))
        local cur_m = (st.active_mode or 0) + 1
        local m_ch, new_m = ui.segmented("cfx_mode", cur_m, { T("Авто", "Auto"), "Replace", "Overlay" })
        if m_ch then
            carfx.set_mode(new_m - 1, 0)
        end

        if carfx.set_stealth then
            ui.spacing(2)
            ui.text_dim(T("Прозрачность (@stealth)", "Stealth Mode"))
            local cur_s = (st.active_stealth or 0) + 1
            local s_ch, new_s = ui.segmented("cfx_stealth", cur_s, T({ "Авто", "Стекло", "Невидимка" }, { "Auto", "Glass", "Stealth" }))
            if s_ch then
                carfx.set_stealth(new_s - 1, 0)
            end
        end

        if st.last_error and st.last_error ~= "" then
            ui.spacing(4)
            ui.text_wrapped(st.last_error, "red")
        elseif state.status_msg ~= "" then
            ui.spacing(4)
            ui.text_wrapped(state.status_msg, state.status_tone)
        end
    end)

    if st.params and #st.params > 0 then
        ui.card(T("Параметры шейдера", "Shader Parameters"), short_name ~= "" and short_name or nil, function()
            for idx = 1, #st.params do
                local pm = st.params[idx]
                local vals = pm.values or {}
                local lo = pm.lo or 0.0
                local hi = pm.hi or 1.0
                if hi <= lo then
                    lo = -10.0
                    hi = 10.0
                end
                local nice_name = (pm.desc and pm.desc ~= "") and pm.desc or pm.name

                if pm.color and pm.count >= 3 then
                    local r = vals[1] or 1.0
                    local g = vals[2] or 1.0
                    local b = vals[3] or 1.0
                    local a = vals[4] or 1.0

                    if pm.count == 4 and ui.color_edit4 then
                        local ch, nr, ng, nb, na = ui.color_edit4(nice_name .. "##cfx_c4_" .. idx, r, g, b, a)
                        if ch then
                            carfx.set_param(pm.name, nr, ng, nb, na)
                        end
                    elseif ui.color_edit3 then
                        local ch, nr, ng, nb = ui.color_edit3(nice_name .. "##cfx_c3_" .. idx, r, g, b)
                        if ch then
                            carfx.set_param(pm.name, nr, ng, nb)
                        end
                    end
                else
                    if pm.count == 1 then
                        local ch, nv = ui.slider_float(nice_name .. "##cfx_pv_" .. idx, vals[1] or 0.0, lo, hi)
                        if ch then
                            carfx.set_param(pm.name, nv)
                        end
                    else
                        ui.text_dim(nice_name)
                        local comp_names = { "x", "y", "z", "w" }
                        local out_v = { vals[1] or 0, vals[2] or 0, vals[3] or 0, vals[4] or 0 }
                        local any_changed = false
                        for c = 1, (pm.count or 1) do
                            local lbl = pm.name .. "." .. comp_names[c]
                            local ch, nv = ui.slider_float(lbl .. "##cfx_pv_" .. idx .. "_" .. c, out_v[c], lo, hi)
                            if ch then
                                out_v[c] = nv
                                any_changed = true
                            end
                        end
                        if any_changed then
                            if pm.count == 2 then carfx.set_param(pm.name, out_v[1], out_v[2])
                            elseif pm.count == 3 then carfx.set_param(pm.name, out_v[1], out_v[2], out_v[3])
                            else carfx.set_param(pm.name, out_v[1], out_v[2], out_v[3], out_v[4])
                            end
                        end
                    end
                end
            end
        end)
    end

    if st.textures and #st.textures > 0 then
        ui.card(T("Текстуры (@tex)", "Textures (@tex)"), nil, function()
            local bw = (ui.avail_width() - 8) * 0.5
            for idx = 1, #st.textures do
                local tx = st.textures[idx]
                local slot = tx.slot or 0
                local lbl = (tx.desc and tx.desc ~= "") and tx.desc or tx.name
                local tch, np = ui.input_text(string.format("%s (t%d)##cfx_tx_in_%d", lbl, 33 + slot, slot), state.tex_paths[slot] or "", 256)
                if tch then state.tex_paths[slot] = np end
                if ui.button(T("Задать##cfx_tx_s_", "Apply##cfx_tx_s_") .. slot, bw, 26, "primary") then
                    local ok, err = carfx.set_tex(slot, state.tex_paths[slot] or "")
                    if not ok then
                        state.status_msg = tostring(err)
                        state.status_tone = "red"
                    end
                end
                ui.same_line(0, 8)
                if ui.button(T("Очистить##cfx_tx_c_", "Clear##cfx_tx_c_") .. slot, bw, 26, "ghost") then
                    carfx.clear_tex(slot)
                end
                if idx < #st.textures then ui.spacing(4) end
            end
        end)
    end

    if ui.collapsing(T("Датчики и диагностика##cfx_diag", "Sensors & Diagnostics##cfx_diag"), false) then
        ui.card(nil, nil, function()
            local p_ch, new_p = ui.input_text(T("Путь .hlsl##cfx_path", "Custom .hlsl Path##cfx_path"), state.custom_path, 256)
            if p_ch then state.custom_path = new_p end
            if ui.button(T("Обновить список шейдеров##cfx_ref", "Refresh Shader List##cfx_ref"), ui.avail_width(), 26, "ghost") then
                refresh_shader_list()
            end

            if carfx.get_spatial then
                local sp = carfx.get_spatial()
                if sp and sp.car_fwd and (sp.car_fwd.valid or 0) > 0.5 then
                    ui.separator()
                    local fmt_d = function(v)
                        return (v and v < 900.0) and string.format("%.2fm", v) or "—"
                    end
                    ui.text(string.format(T("Стены Пер/Зад: %s / %s | Угол: %.1f°", "Walls F/R: %s / %s | Drift: %.1f°"),
                        fmt_d(sp.wall_front), fmt_d(sp.wall_rear), sp.drift_deg or 0.0))
                    ui.text(string.format(T("Лево: %s / %s / %s | Право: %s / %s / %s", "Left: %s / %s / %s | Right: %s / %s / %s"),
                        fmt_d(sp.wall_front_left), fmt_d(sp.wall_door_left), fmt_d(sp.wall_rear_left),
                        fmt_d(sp.wall_front_right), fmt_d(sp.wall_door_right), fmt_d(sp.wall_rear_right)))
                    local near_gap = sp.near_car_pos and sp.near_car_pos.gap or 999.0
                    if near_gap < 900.0 then
                        ui.text_dim(string.format(T("Ближайшее авто: %.2f м", "Nearest car: %.2f m"), near_gap))
                    end
                end
            end

            ui.separator()
            ui.text_dim(string.format("DX11=%s | Match=%d/%d | RT=%dx%d",
                init_ok and "OK" or "WAIT", st.matched_last_frame or 0, st.total_draws or 0,
                st.last_rt_w or 0, st.last_rt_h or 0))
            if st.body_valid and st.body_albedo_default and st.body_packed_default then
                ui.text_wrapped(T("Внимание: на машине нет винилов (дефолтная текстура).", "Note: car has no decals (default texture)."), "yellow")
            end
        end)
    end
end

if urushi and urushi.register_window then
    urushi.register_window({
        id = "carfx",
        title = T("CarFX Шейдеры", "CarFX Shaders"),
        icon = "shader",
        order = 20,
        default_open = false,
        default_pinned = false,
        x = 696,
        y = 24,
        width = 418,
        render = render_carfx_window,
    })
end

log.info("CarFX module loaded.")
