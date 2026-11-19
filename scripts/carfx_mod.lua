-- scripts/carfx_mod.lua
-- URUSHI CarFX Module: Independent Body (Target 0) & Internals/Cabin (Target 1) Shader Controls
local T = i18n.t
local VK_F9 = 0x78

local PARTS = {
    { "interior",   "Салон",       "Interior" },
    { "wheels",     "Диски",       "Rims / Wheels" },
    { "tires",      "Шины",        "Tires" },
    { "engine",     "Двигатель",   "Engine" },
    { "brakes",     "Тормоза",     "Brakes" },
    { "suspension", "Подвеска",    "Suspension" },
    { "glass",      "Стёкла",      "Glass" },
}

local cfx_settings = {
    enabled = true,
    body_enabled = true,
    internals_enabled = true,
    mode = 0,
    internals_mode = 0,
    selected_shader = "livery_holo.hlsl",
    internals_shader = "internals_neon.hlsl",
    body_extras = {
        spoiler = true,
        lights = true,
    },
    parts = {
        engine = true,
        suspension = true,
        brakes = true,
        wheels = true,
        tires = false,
        lights = false,
        glass = false,
        interior = true,
        spoiler = false,
    },
    tex_paths = {},
    internals_tex_paths = {},
    body_params = {},
    internals_params = {},
}

if config and config.bind then
    cfx_settings = config.bind("carfx", cfx_settings)
end

-- Normalize legacy settings keys
if cfx_settings.body_enabled == nil then
    cfx_settings.body_enabled = (cfx_settings.enabled ~= false)
end
if cfx_settings.internals_enabled == nil then
    cfx_settings.internals_enabled = true
end
if type(cfx_settings.body_extras) ~= "table" then
    cfx_settings.body_extras = { spoiler = true, lights = true }
else
    if cfx_settings.body_extras.spoiler == nil then cfx_settings.body_extras.spoiler = true end
    if cfx_settings.body_extras.lights == nil then cfx_settings.body_extras.lights = true end
end
if type(cfx_settings.parts) ~= "table" then
    cfx_settings.parts = {
        engine = true, suspension = true, brakes = true, wheels = true,
        tires = false, lights = false, glass = false, interior = true, spoiler = false,
    }
end
if type(cfx_settings.body_params) ~= "table" then cfx_settings.body_params = {} end
if type(cfx_settings.internals_params) ~= "table" then cfx_settings.internals_params = {} end
if type(cfx_settings.tex_paths) ~= "table" then cfx_settings.tex_paths = {} end
if type(cfx_settings.internals_tex_paths) ~= "table" then cfx_settings.internals_tex_paths = {} end

local state = {
    tab = 1, -- 1: Body, 2: Internals
    shaders = {},
    available_textures = {},
    tex_inputs = { [0] = { [0] = "", [1] = "", [2] = "", [3] = "" }, [1] = { [0] = "", [1] = "", [2] = "", [3] = "" } },
    status_msg = { [0] = "", [1] = "" },
    status_tone = { [0] = "muted", [1] = "muted" },
}

local function short_shader_name(p)
    return (p and p:match("([^\\/]+)$")) or ""
end

local function refresh_shader_list()
    if carfx and carfx.list_shaders then
        state.shaders = carfx.list_shaders() or {}
    end
    if carfx and carfx.list_textures then
        state.available_textures = carfx.list_textures() or {}
    end
end
refresh_shader_list()

local function get_param_store(target)
    return (target == 1) and cfx_settings.internals_params or cfx_settings.body_params
end

local function get_tex_store(target)
    return (target == 1) and cfx_settings.internals_tex_paths or cfx_settings.tex_paths
end

local function apply_master_enable()
    local any_on = (cfx_settings.body_enabled ~= false) or (cfx_settings.internals_enabled ~= false)
    cfx_settings.enabled = any_on
    if carfx and carfx.set_enabled then
        carfx.set_enabled(any_on)
    end
    if carfx and carfx.set_target_enabled then
        carfx.set_target_enabled(0, cfx_settings.body_enabled ~= false)
        carfx.set_target_enabled(1, cfx_settings.internals_enabled ~= false)
    end
end

local function apply_saved_params_for_target(target, shader_name)
    if not carfx or not carfx.set_target_param then return end
    local name = short_shader_name(shader_name)
    if name == "" then return end
    local store = get_param_store(target)
    local saved = store[name]
    if type(saved) ~= "table" then return end
    for pm_name, vals in pairs(saved) do
        if type(vals) == "table" and #vals >= 1 then
            if #vals == 1 then carfx.set_target_param(target, pm_name, vals[1])
            elseif #vals == 2 then carfx.set_target_param(target, pm_name, vals[1], vals[2])
            elseif #vals == 3 then carfx.set_target_param(target, pm_name, vals[1], vals[2], vals[3])
            else carfx.set_target_param(target, pm_name, vals[1], vals[2], vals[3], vals[4])
            end
        end
    end
end

local function apply_saved_textures_for_target(target)
    if not carfx or not carfx.set_tex then return end
    local store = get_tex_store(target)
    for i = 0, 3 do
        local p = store[i] or store[tostring(i)]
        if p and p ~= "" then
            state.tex_inputs[target][i] = p
            carfx.set_tex(i, p, target)
        end
    end
end

local function save_param_override(target, shader_name, pm_name, ...)
    local name = short_shader_name(shader_name)
    if name == "" then return end
    local store = get_param_store(target)
    if type(store[name]) ~= "table" then
        store[name] = {}
    end
    store[name][pm_name] = { ... }
    if config then config.mark_dirty() end
end

local function load_shader_for_target(target, filename, skip_save_config)
    if not carfx or not carfx.load then return false end
    local short = short_shader_name(filename)
    if short == "" then
        if carfx.unload then carfx.unload(target) end
        state.status_msg[target] = ""
        if not skip_save_config then
            if target == 0 then cfx_settings.selected_shader = "" else cfx_settings.internals_shader = "" end
            if config then config.mark_dirty() end
        end
        return true
    end

    local ok, err = carfx.load(filename, target)
    state.status_msg[target] = ok and "" or tostring(err)
    state.status_tone[target] = ok and "muted" or "red"
    if ok then
        apply_saved_params_for_target(target, short)
        apply_saved_textures_for_target(target)
        if not skip_save_config then
            if target == 0 then
                cfx_settings.selected_shader = filename
            else
                cfx_settings.internals_shader = filename
            end
            if config then config.mark_dirty() end
        end
    end
    return ok
end

local pending_restore = true
local function try_restore_carfx()
    if not pending_restore or not carfx or not carfx.get_status then return end
    local st = carfx.get_status(0)
    if not st or not st.initialized then return end
    pending_restore = false

    refresh_shader_list()
    apply_master_enable()

    if carfx.set_mode then
        carfx.set_mode(cfx_settings.mode or 0, 0)
        carfx.set_mode(cfx_settings.internals_mode or 0, 1)
    end
    if carfx.set_parts and type(cfx_settings.parts) == "table" then
        carfx.set_parts(cfx_settings.parts)
    end
    if carfx.set_body_extras and type(cfx_settings.body_extras) == "table" then
        carfx.set_body_extras(cfx_settings.body_extras)
    end

    if cfx_settings.selected_shader and cfx_settings.selected_shader ~= "" then
        local ok, err = load_shader_for_target(0, cfx_settings.selected_shader, true)
        if not ok then
            log.warn("CarFX body shader auto-load failed: " .. tostring(err))
        end
    end
    if cfx_settings.internals_shader and cfx_settings.internals_shader ~= "" then
        local ok, err = load_shader_for_target(1, cfx_settings.internals_shader, true)
        if not ok then
            log.warn("CarFX internals shader auto-load failed: " .. tostring(err))
        end
    end
end

if keybinds and keybinds.register then
    keybinds.register("carfx_menu", { label = T("CarFX шейдеры", "CarFX Shaders"), mod_name = T("CarFX Шейдеры", "CarFX Shaders"), order = 1 })
end

events.on_key(function(vk, is_down)
    if not keybinds and vk == VK_F9 and is_down then
        if imgui and imgui.is_any_item_active and imgui.is_any_item_active() then return end
        if urushi and urushi.toggle_window then
            urushi.toggle_window("carfx")
        end
    end
end)

local refresh_timer = 0
events.on_update(function(dt)
    if pending_restore then
        try_restore_carfx()
    end
    refresh_timer = refresh_timer + (dt or 0.016)
    if refresh_timer >= 2.0 then
        refresh_timer = 0
        refresh_shader_list()
    end
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

local function render_shader_params(target, st, short_name)
    if not (st.params and #st.params > 0) then return end
    ui.card(T("Параметры шейдера", "Shader Parameters"), short_name ~= "" and short_name or nil, function()
        local id_sfx = string.format("t%d", target)
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
                    local ch, nr, ng, nb, na = ui.color_edit4(nice_name .. "##cfx_c4_" .. id_sfx .. "_" .. idx, r, g, b, a)
                    if ch then
                        carfx.set_target_param(target, pm.name, nr, ng, nb, na)
                        save_param_override(target, short_name, pm.name, nr, ng, nb, na)
                    end
                elseif ui.color_edit3 then
                    local ch, nr, ng, nb = ui.color_edit3(nice_name .. "##cfx_c3_" .. id_sfx .. "_" .. idx, r, g, b)
                    if ch then
                        carfx.set_target_param(target, pm.name, nr, ng, nb)
                        save_param_override(target, short_name, pm.name, nr, ng, nb)
                    end
                end
            else
                if pm.count == 1 then
                    local ch, nv = ui.slider_float(nice_name .. "##cfx_pv_" .. id_sfx .. "_" .. idx, vals[1] or 0.0, lo, hi)
                    if ch then
                        carfx.set_target_param(target, pm.name, nv)
                        save_param_override(target, short_name, pm.name, nv)
                    end
                else
                    ui.text_dim(nice_name)
                    local comp_names = { "x", "y", "z", "w" }
                    local out_v = { vals[1] or 0, vals[2] or 0, vals[3] or 0, vals[4] or 0 }
                    local any_changed = false
                    for c = 1, (pm.count or 1) do
                        local lbl = pm.name .. "." .. comp_names[c]
                        local ch, nv = ui.slider_float(lbl .. "##cfx_pv_" .. id_sfx .. "_" .. idx .. "_" .. c, out_v[c], lo, hi)
                        if ch then
                            out_v[c] = nv
                            any_changed = true
                        end
                    end
                    if any_changed then
                        if pm.count == 2 then
                            carfx.set_target_param(target, pm.name, out_v[1], out_v[2])
                            save_param_override(target, short_name, pm.name, out_v[1], out_v[2])
                        elseif pm.count == 3 then
                            carfx.set_target_param(target, pm.name, out_v[1], out_v[2], out_v[3])
                            save_param_override(target, short_name, pm.name, out_v[1], out_v[2], out_v[3])
                        else
                            carfx.set_target_param(target, pm.name, out_v[1], out_v[2], out_v[3], out_v[4])
                            save_param_override(target, short_name, pm.name, out_v[1], out_v[2], out_v[3], out_v[4])
                        end
                    end
                end
            end
        end

        ui.spacing(4)
        if ui.button(T("Сбросить параметры##cfx_rst_", "Reset Parameters##cfx_rst_") .. id_sfx, ui.avail_width(), 24, "ghost") then
            local store = get_param_store(target)
            if short_name ~= "" then store[short_name] = nil end
            if config then config.mark_dirty() end
            carfx.reload(target)
        end
    end)
end

local function render_shader_textures(target, st)
    if not (st.textures and #st.textures > 0) then return end
    ui.card(T("Текстуры шейдера", "Shader Textures"), nil, function()
        local bw = (ui.avail_width() - 8) * 0.5
        local id_sfx = string.format("t%d", target)
        local tex_store = get_tex_store(target)
        for idx = 1, #st.textures do
            local tx = st.textures[idx]
            local slot = tx.slot or 0
            local lbl = (tx.desc and tx.desc ~= "") and tx.desc or tx.name
            local cur_path = state.tex_inputs[target][slot] or ""

            if state.available_textures and #state.available_textures > 0 then
                local combo_items = { T("<Выбрать из shaders/>", "<Select from shaders/>") }
                local cur_sel = 1
                for _, fn in ipairs(state.available_textures) do
                    combo_items[#combo_items + 1] = fn
                    if cur_path == fn then cur_sel = #combo_items end
                end
                local c_ch, new_sel = ui.combo(string.format("##cfx_tx_cb_%s_%d", id_sfx, slot), cur_sel, combo_items)
                if c_ch and new_sel > 1 then
                    local picked = combo_items[new_sel]
                    state.tex_inputs[target][slot] = picked
                    local ok, err = carfx.set_tex(slot, picked, target)
                    if not ok then
                        state.status_msg[target] = tostring(err)
                        state.status_tone[target] = "red"
                    else
                        state.status_msg[target] = ""
                        tex_store[slot] = picked
                        if config then config.mark_dirty() end
                    end
                end
            end

            local tch, np = ui.input_text(string.format("%s##cfx_tx_%s_%d", lbl, id_sfx, slot), state.tex_inputs[target][slot] or "", 256)
            if tch then state.tex_inputs[target][slot] = np end
            if ui.button(T("Применить##cfx_tx_s_", "Apply##cfx_tx_s_") .. id_sfx .. "_" .. slot, bw, 24, "primary") then
                local p = state.tex_inputs[target][slot] or ""
                local ok, err = carfx.set_tex(slot, p, target)
                if not ok then
                    state.status_msg[target] = tostring(err)
                    state.status_tone[target] = "red"
                else
                    state.status_msg[target] = ""
                    tex_store[slot] = p
                    if config then config.mark_dirty() end
                end
            end
            ui.same_line(0, 8)
            if ui.button(T("Очистить##cfx_tx_c_", "Clear##cfx_tx_c_") .. id_sfx .. "_" .. slot, bw, 24, "ghost") then
                carfx.clear_tex(slot, target)
                state.tex_inputs[target][slot] = ""
                tex_store[slot] = ""
                if config then config.mark_dirty() end
            end

            if carfx and carfx.get_tex_info and carfx.set_tex_playback then
                local info = carfx.get_tex_info(slot, target)
                if info and (info.animated or info.state == "loading" or info.state == "error") then
                    ui.spacing(3)
                    if info.state == "loading" then
                        ui.badge(T("Загрузка анимации...", "Loading animation..."), "yellow")
                    elseif info.state == "error" then
                        ui.badge(T("Ошибка анимации", "Animation error"), "red")
                        if info.error and info.error ~= "" then
                            ui.text_wrapped(info.error, "red")
                        end
                    elseif info.state == "ready" then
                        ui.badge(string.format(T("Анимация: %d к., %.1f FPS", "Anim: %d fr, %.1f FPS"), info.frames or 0, info.fps or 0), "accent")
                        ui.spacing(2)
                        local p_lbl = info.playing and T("Пауза##cfx_ap_", "Pause##cfx_ap_") or T("Старт##cfx_ap_", "Play##cfx_ap_")
                        if ui.button(p_lbl .. id_sfx .. "_" .. slot, 80, 24, info.playing and "ghost" or "primary") then
                            carfx.set_tex_playback(slot, { playing = not info.playing }, target)
                        end
                        ui.same_line(0, 6)
                        local mode_opts = { T("Цикл", "Loop"), T("Пинг-понг", "Ping-Pong"), T("1 раз", "Once") }
                        local mode_map = { "loop", "pingpong", "once" }
                        local cur_mi = (info.mode == "pingpong" and 2) or (info.mode == "once" and 3) or 1
                        local seg_ch, new_mi = ui.segmented(string.format("cfx_am_%s_%d", id_sfx, slot), cur_mi, mode_opts)
                        if seg_ch and new_mi ~= cur_mi then
                            carfx.set_tex_playback(slot, { mode = mode_map[new_mi] }, target)
                        end
                        local spd_ch, new_spd = ui.slider_float(
                            string.format(T("Скорость##cfx_aspd_%s_%d", "Speed##cfx_aspd_%s_%d"), id_sfx, slot),
                            info.speed or 1.0, 0.1, 3.0, "%.2fx")
                        if spd_ch and math.abs(new_spd - (info.speed or 1.0)) > 0.01 then
                            carfx.set_tex_playback(slot, { speed = new_spd }, target)
                        end
                        local max_f = math.max(0, (info.frames or 1) - 1)
                        local frm_ch, new_frm = ui.slider_int(
                            string.format(T("Кадр##cfx_afrm_%s_%d", "Frame##cfx_afrm_%s_%d"), id_sfx, slot),
                            info.frame or 0, 0, max_f)
                        if frm_ch and new_frm ~= (info.frame or 0) then
                            carfx.set_tex_playback(slot, { frame = new_frm }, target)
                        end
                    end
                end
            end

            if idx < #st.textures then ui.spacing(4) end
        end
    end)
end

local function render_shader_picker_and_modes(target, st, is_body, car_ready)
    local short_name = short_shader_name(st.shader_path)
    local pill_name = short_name:gsub("%.hlsl$", "")
    local eff_mode = st.compiled and ((st.resolved_mode == 2) and "Overlay" or "Replace") or T("Выкл", "None")
    local id_sfx = is_body and "body" or "int"
    local target_on = is_body and (cfx_settings.body_enabled ~= false) or (cfx_settings.internals_enabled ~= false)

    ui.card(is_body and T("Шейдер кузова", "Body Shader") or T("Шейдер салона и деталей", "Internals & Parts Shader"), nil, function()
        local toggle_lbl = is_body
            and T("Включить шейдер кузова##cfx_en_0", "Enable Body Shader##cfx_en_0")
            or  T("Включить шейдер салона/деталей##cfx_en_1", "Enable Internals Shader##cfx_en_1")
        local ch_en, new_en = ui.toggle(toggle_lbl, target_on)
        if ch_en then
            if is_body then cfx_settings.body_enabled = new_en else cfx_settings.internals_enabled = new_en end
            apply_master_enable()
            if config then config.mark_dirty() end
        end

        ui.spacing(4)
        ui.stat_row({
            {
                label = is_body and T("Кузов", "Body") or T("Авто", "Car"),
                value = car_ready and T("Готов", "Ready") or T("Вне трассы", "Off Track"),
                tone = car_ready and "green" or "muted",
            },
            {
                label = T("Шейдер", "Shader"),
                value = (st.compiled and target_on) and (pill_name ~= "" and pill_name or T("Активен", "Active")) or T("Выкл", "Off"),
                tone = (st.compiled and target_on) and "accent" or "muted",
            },
            {
                label = T("Режим", "Mode"),
                value = eff_mode,
                tone = st.compiled and "default" or "muted",
            },
        })

        ui.spacing(4)
        if #state.shaders == 0 then refresh_shader_list() end

        local names = { T("(не выбран)", "(none)") }
        local cur_idx = 1
        for _, name in ipairs(state.shaders) do
            names[#names + 1] = name
            if st.compiled and name == short_name then
                cur_idx = #names
            end
        end
        local c_ch, new_idx = ui.combo(T("Выбор шейдера##cfx_cmb_", "Shader Preset##cfx_cmb_") .. id_sfx, cur_idx, names)
        if c_ch then
            if new_idx == 1 then
                load_shader_for_target(target, "")
            elseif names[new_idx] then
                load_shader_for_target(target, names[new_idx])
                if not target_on then
                    if is_body then cfx_settings.body_enabled = true else cfx_settings.internals_enabled = true end
                    apply_master_enable()
                end
            end
        end

        ui.spacing(2)
        local bw = (ui.avail_width() - 12) / 3.0
        if ui.button(T("Перезагрузить##cfx_rel_", "Reload##cfx_rel_") .. id_sfx, bw, 26) then
            local ok, err = carfx.reload(target)
            state.status_msg[target] = ok and "" or tostring(err)
            state.status_tone[target] = ok and "muted" or "red"
            if ok then apply_saved_params_for_target(target, short_name) end
        end
        ui.same_line(0, 6)
        if ui.button(T("Обновить список##cfx_ref_", "Refresh List##cfx_ref_") .. id_sfx, bw, 26, "ghost") then
            refresh_shader_list()
        end
        ui.same_line(0, 6)
        if ui.button(T("Снять##cfx_clr_", "Clear##cfx_clr_") .. id_sfx, bw, 26, "danger") then
            load_shader_for_target(target, "")
        end

        ui.spacing(6)
        ui.text_dim(T("Режим наложения", "Blend Mode"))
        local cur_m = (st.active_mode or 0) + 1
        local m_ch, new_m = ui.segmented("cfx_mode_" .. id_sfx, cur_m, { T("Авто", "Auto"), "Replace", "Overlay" })
        if m_ch then
            carfx.set_mode(new_m - 1, target)
            if is_body then cfx_settings.mode = new_m - 1 else cfx_settings.internals_mode = new_m - 1 end
            if config then config.mark_dirty() end
        end

        if short_name == "livery_music.hlsl" and audio_in and audio_in.is_enabled then
            ui.spacing(6)
            local audio_on = audio_in.is_enabled()
            local a_ch, new_a = ui.toggle(T("Захват музыки для шейдера##cfx_m_q_" .. id_sfx, "Capture Music Audio##cfx_m_q_" .. id_sfx), audio_on)
            if a_ch then
                audio_in.set_enabled(new_a)
                if config then config.set("music", "enabled", new_a) end
            end
            if urushi and urushi.set_window_open then
                if ui.button(T("Открыть настройки музыки##cfx_m_o_" .. id_sfx, "Open Music Settings##cfx_m_o_" .. id_sfx), ui.avail_width(), 24, "ghost") then
                    urushi.set_window_open("music", true)
                end
            end
        end

        if st.last_error and st.last_error ~= "" then
            ui.spacing(4)
            ui.text_wrapped(st.last_error, "red")
        elseif state.status_msg[target] ~= "" then
            ui.spacing(4)
            ui.text_wrapped(state.status_msg[target], state.status_tone[target])
        end
    end)
end

local function render_body_extras_card()
    if not (carfx and carfx.get_body_extras and carfx.set_body_extras) then return end
    ui.card(T("Доп. элементы кузова", "Body Attachments"), T("Шейдер кузова применится и к ним", "Body shader applies to these too"), function()
        local ex = carfx.get_body_extras()
        local avail = ui.avail_width()
        local col_w = (avail - 6.0) * 0.5
        local sp_on = (ex.spoiler == true)
        local lt_on = (ex.lights == true)
        if ui.button((sp_on and "● " or "○ ") .. T("Спойлер##cfx_bex_sp", "Spoiler##cfx_bex_sp"), col_w, 26, sp_on and "primary" or "ghost") then
            carfx.set_body_extras({ spoiler = not sp_on })
            cfx_settings.body_extras = carfx.get_body_extras()
            if config then config.mark_dirty() end
        end
        ui.same_line(0, 6)
        if ui.button((lt_on and "● " or "○ ") .. T("Фары##cfx_bex_lt", "Lights##cfx_bex_lt"), col_w, 26, lt_on and "primary" or "ghost") then
            carfx.set_body_extras({ lights = not lt_on })
            cfx_settings.body_extras = carfx.get_body_extras()
            if config then config.mark_dirty() end
        end
    end)
end

local function render_internals_parts_card()
    if not (carfx and carfx.get_parts and carfx.set_parts) then return end
    ui.card(T("К каким частям применять", "Target Parts Filter"), T("Работает только на вашем авто", "Applies only to your car"), function()
        local parts = carfx.get_parts()
        local avail = ui.avail_width()
        local col_w = (avail - 6.0) * 0.5

        local pw = (avail - 12.0) / 3.0
        if ui.button(T("Только салон##cfx_pr_cab", "Cabin Only##cfx_pr_cab"), pw, 24, "ghost") then
            local p = { engine = false, suspension = false, brakes = false, wheels = false, tires = false, lights = false, glass = false, interior = true, spoiler = false }
            carfx.set_parts(p)
            cfx_settings.parts = carfx.get_parts()
            if config then config.mark_dirty() end
        end
        ui.same_line(0, 6)
        if ui.button(T("Стандарт##cfx_pr_def", "Default##cfx_pr_def"), pw, 24, "ghost") then
            local p = { engine = true, suspension = true, brakes = true, wheels = true, tires = false, lights = false, glass = false, interior = true, spoiler = false }
            carfx.set_parts(p)
            cfx_settings.parts = carfx.get_parts()
            if config then config.mark_dirty() end
        end
        ui.same_line(0, 6)
        if ui.button(T("Снять все##cfx_pr_clr", "Clear All##cfx_pr_clr"), pw, 24, "ghost") then
            local p = { engine = false, suspension = false, brakes = false, wheels = false, tires = false, lights = false, glass = false, interior = false, spoiler = false }
            carfx.set_parts(p)
            cfx_settings.parts = carfx.get_parts()
            if config then config.mark_dirty() end
        end

        ui.spacing(4)
        for i = 1, #PARTS, 2 do
            local p1 = PARTS[i]
            local p2 = PARTS[i + 1]
            local on1 = (parts[p1[1]] == true)
            if ui.button((on1 and "● " or "○ ") .. T(p1[2], p1[3]) .. "##cfx_pt_" .. p1[1], p2 and col_w or avail, 26, on1 and "primary" or "ghost") then
                carfx.set_parts({ [p1[1]] = not on1 })
                cfx_settings.parts = carfx.get_parts()
                if config then config.mark_dirty() end
            end
            if p2 then
                ui.same_line(0, 6)
                local on2 = (parts[p2[1]] == true)
                if ui.button((on2 and "● " or "○ ") .. T(p2[2], p2[3]) .. "##cfx_pt_" .. p2[1], col_w, 26, on2 and "primary" or "ghost") then
                    carfx.set_parts({ [p2[1]] = not on2 })
                    cfx_settings.parts = carfx.get_parts()
                    if config then config.mark_dirty() end
                end
            end
        end
    end)
end

local function render_carfx_window()
    if not carfx or not carfx.get_status then
        ui.card("CarFX", nil, function()
            ui.text_wrapped(T("Модуль CarFX не инициализирован.", "CarFX module not initialized."), "red")
        end)
        return
    end

    local st0 = carfx.get_status(0)
    local st1 = carfx.get_status(1)
    local short0 = short_shader_name(st0.shader_path)
    local short1 = short_shader_name(st1.shader_path)

    local tab_ch, new_tab = ui.segmented("cfx_nav_tabs", state.tab, T({ "Кузов", "Салон и детали" }, { "Body", "Internals" }))
    if tab_ch then
        state.tab = new_tab
    end
    ui.spacing(4)

    if state.tab == 1 then
        render_shader_picker_and_modes(0, st0, true, st0.body_valid)
        render_body_extras_card()
        render_shader_params(0, st0, short0)
        render_shader_textures(0, st0)
    else
        render_shader_picker_and_modes(1, st1, false, st0.body_valid)
        render_internals_parts_card()
        render_shader_params(1, st1, short1)
        render_shader_textures(1, st1)
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
        width = 432,
        settings = cfx_settings,
        render = render_carfx_window,
    })
end

log.info("CarFX module loaded.")
