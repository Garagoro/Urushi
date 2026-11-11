-- scripts/teleport_mod.lua
-- Urushi Teleport System: Per-track persistence, custom naming, unlimited checkpoints list
local T = i18n.t
local VK_SAVE = 0xDB -- '['
local VK_TP   = 0xDD -- ']'

local DATA_FILE = "scripts/teleports_data.json"

local KNOWN_TRACKS = {
    { key = "fuji", name = "Fuji Speedway" },
    { key = "kami", name = "Kami Road" },
    { key = "nurburg", name = "Nurburgring" },
    { key = "adm", name = "ADM Raceway" },
    { key = "dominion", name = "Dominion Raceway" },
    { key = "south_port", name = "South Port" },
    { key = "southport", name = "South Port" },
    { key = "springstone", name = "Springstone" },
    { key = "palezzo", name = "San Palezzo" },
    { key = "milton", name = "Milton Drive" },
    { key = "navarra", name = "Circuit de Navarra" },
    { key = "atron", name = "Atron" },
    { key = "red_rock", name = "Red Rock" },
    { key = "redrock", name = "Red Rock" },
    { key = "castle", name = "Castle Road" },
    { key = "touge", name = "East Touge" },
    { key = "mondello", name = "Mondello Park" },
    { key = "petersburg", name = "Saint Petersburg" },
    { key = "ebisu", name = "Ebisu Minami" },
    { key = "parking", name = "Parking" },
    { key = "airfield", name = "Airfield" },
    { key = "winter", name = "Winter City" },
    { key = "la_block", name = "LA Block" },
}

local state = {
    current_track = "Fuji Speedway",
    auto_track = true,
    tracks = {},
    active_idx = 1,
    new_point_name = "",
    editing_idx = nil,
    edit_name_buf = "",
    vel_mode = 1, -- 1=Stop, 2=Saved, 3=Keep
    hotkeys_enabled = true,
    last_track_check = 0,
    status_msg = "",
    status_tone = "muted",
    status_time = 0,
}

local function set_status(msg, tone)
    state.status_msg = msg
    state.status_tone = tone or "muted"
    state.status_time = os.clock()
end

local function is_typing()
    return imgui and imgui.is_any_item_active and imgui.is_any_item_active()
end

local function distance_3d(a, b)
    if not a or not b then return 0.0 end
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    local dz = (a.z or 0) - (b.z or 0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

-- ==================== JSON Persistence ====================

local function escape_str(s)
    local str = tostring(s):gsub("\\", "\\\\"):gsub("\"", "\\\""):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    return "\"" .. str .. "\""
end

local function clean_num(n)
    if n ~= n then return "0" end
    local s = string.format("%.4f", n)
    local trimmed = (s:gsub("%.?0+$", ""))
    if trimmed == "" or trimmed == "-" then trimmed = "0" end
    return trimmed
end

local function json_encode(val)
    local t = type(val)
    if t == "number" then return clean_num(val)
    elseif t == "string" then return escape_str(val)
    elseif t == "boolean" then return val and "true" or "false"
    elseif t == "table" then
        local is_arr = (#val > 0)
        local parts = {}
        if is_arr then
            for i = 1, #val do
                table.insert(parts, json_encode(val[i]))
            end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            for k, subv in pairs(val) do
                table.insert(parts, escape_str(k) .. ":" .. json_encode(subv))
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    end
    return "null"
end

local function json_decode(str)
    if not str or str == "" then return nil end
    local pos = 1
    local len = #str

    local function skip_ws()
        while pos <= len do
            local b = str:byte(pos)
            if b == 32 or b == 9 or b == 10 or b == 13 then
                pos = pos + 1
            else
                break
            end
        end
    end

    local parse_value

    local function parse_string()
        pos = pos + 1
        local start = pos
        local buf = {}
        while pos <= len do
            local c = str:sub(pos, pos)
            if c == "\"" then
                table.insert(buf, str:sub(start, pos - 1))
                pos = pos + 1
                return table.concat(buf)
            elseif c == "\\" then
                table.insert(buf, str:sub(start, pos - 1))
                pos = pos + 1
                local esc = str:sub(pos, pos)
                if esc == "n" then table.insert(buf, "\n")
                elseif esc == "r" then table.insert(buf, "\r")
                elseif esc == "t" then table.insert(buf, "\t")
                elseif esc == "\\" then table.insert(buf, "\\")
                elseif esc == "\"" then table.insert(buf, "\"")
                elseif esc == "/" then table.insert(buf, "/")
                else table.insert(buf, esc) end
                pos = pos + 1
                start = pos
            else
                pos = pos + 1
            end
        end
        return table.concat(buf)
    end

    local function parse_number()
        local s, e = str:find("^-?%d+%.?%d*[eE]?[+-]?%d*", pos)
        if s then
            pos = e + 1
            return tonumber(str:sub(s, e)) or 0
        end
        pos = pos + 1
        return 0
    end

    local function parse_array()
        pos = pos + 1
        local arr = {}
        skip_ws()
        if pos <= len and str:sub(pos, pos) == "]" then
            pos = pos + 1
            return arr
        end
        while pos <= len do
            skip_ws()
            table.insert(arr, parse_value())
            skip_ws()
            local c = str:sub(pos, pos)
            if c == "," then
                pos = pos + 1
            elseif c == "]" then
                pos = pos + 1
                break
            else
                pos = pos + 1
            end
        end
        return arr
    end

    local function parse_object()
        pos = pos + 1
        local obj = {}
        skip_ws()
        if pos <= len and str:sub(pos, pos) == "}" then
            pos = pos + 1
            return obj
        end
        while pos <= len do
            skip_ws()
            if str:sub(pos, pos) ~= "\"" then break end
            local key = parse_string()
            skip_ws()
            if str:sub(pos, pos) == ":" then pos = pos + 1 end
            skip_ws()
            local val = parse_value()
            obj[key] = val
            skip_ws()
            local c = str:sub(pos, pos)
            if c == "," then
                pos = pos + 1
            elseif c == "}" then
                pos = pos + 1
                break
            else
                pos = pos + 1
            end
        end
        return obj
    end

    parse_value = function()
        skip_ws()
        if pos > len then return nil end
        local c = str:sub(pos, pos)
        if c == "\"" then return parse_string()
        elseif c == "{" then return parse_object()
        elseif c == "[" then return parse_array()
        elseif c == "t" and str:sub(pos, pos + 3) == "true" then pos = pos + 4; return true
        elseif c == "f" and str:sub(pos, pos + 4) == "false" then pos = pos + 5; return false
        elseif c == "n" and str:sub(pos, pos + 3) == "null" then pos = pos + 4; return nil
        else return parse_number() end
    end

    return parse_value()
end

local function load_from_disk()
    local f = io.open(DATA_FILE, "r")
    if not f then return end
    local content = f:read("*a")
    f:close()
    if not content or content == "" then return end

    local ok, data = pcall(json_decode, content)
    if ok and type(data) == "table" then
        if type(data.tracks) == "table" then
            state.tracks = data.tracks
        end
        if data.current_track and type(data.current_track) == "string" then
            state.current_track = data.current_track
        end
        if data.vel_mode and type(data.vel_mode) == "number" then
            state.vel_mode = math.max(1, math.min(3, math.floor(data.vel_mode)))
        end
        if data.hotkeys_enabled ~= nil then
            state.hotkeys_enabled = (data.hotkeys_enabled == true)
        end
    end
end

local function save_to_disk()
    local payload = {
        version = 1,
        current_track = state.current_track,
        vel_mode = state.vel_mode,
        hotkeys_enabled = state.hotkeys_enabled,
        tracks = state.tracks,
    }
    local text = json_encode(payload)
    local f = io.open(DATA_FILE, "w")
    if f then
        f:write(text)
        f:close()
    end
end

-- ==================== Track Detection ====================

local function detect_current_track()
    if not unity or not unity.get_all_objects then return "Fuji Speedway" end
    local objs = unity.get_all_objects()
    for _, o in ipairs(objs) do
        local n = (o.name or ""):lower()
        if n ~= "" then
            for _, tr in ipairs(KNOWN_TRACKS) do
                if n:find(tr.key) then
                    return tr.name
                end
            end
        end
    end
    -- Fallback generic patterns
    for _, o in ipairs(objs) do
        local n = o.name or ""
        local raw = n:match("^dr3_([%w_]+)") or n:match("^([%w_]+)GarageContent")
        if raw then
            return raw:gsub("_", " "):gsub("(%a)([%w_]*)", function(first, rest)
                return first:upper() .. rest:lower()
            end)
        end
    end
    return "Fuji Speedway"
end

local function get_current_track_points()
    local track = state.current_track or "Fuji Speedway"
    if not state.tracks[track] then
        state.tracks[track] = {}
    end
    return state.tracks[track]
end

local function check_track_update()
    if not state.auto_track then return end
    local now = os.clock()
    if now - state.last_track_check > 2.0 then
        state.last_track_check = now
        local detected = detect_current_track()
        if detected and detected ~= "" and detected ~= state.current_track then
            state.current_track = detected
            state.active_idx = 1
            state.editing_idx = nil
            set_status(string.format(T("Трасса: %s", "Track: %s"), detected), "accent")
        end
    end
end

-- ==================== Actions ====================

local function save_checkpoint(custom_name)
    if not carx or not carx.get_rigid_body then return false end
    local snap = carx.get_rigid_body()
    if not snap or (not snap.rb and not snap.x) then
        set_status(T("Выйдите на трассу на автомобиле.", "Enter a track first."), "yellow")
        return false
    end

    local points = get_current_track_points()
    local name = custom_name
    if not name or name:match("^%s*$") then
        name = string.format(T("Точка %d", "Point %d"), #points + 1)
    end

    local new_pt = {
        id = os.time(),
        name = name,
        x = snap.x or 0.0,
        y = snap.y or 0.0,
        z = snap.z or 0.0,
        qx = snap.qx or 0.0,
        qy = snap.qy or 0.0,
        qz = snap.qz or 0.0,
        qw = snap.qw or 1.0,
        speed_kmh = snap.speed_kmh or 0.0,
        gear = snap.gear or 0,
        saved_at = os.date("%H:%M:%S"),
        rb = snap.rb,
        vel = snap.vel,
        ang_vel = snap.ang_vel,
    }

    table.insert(points, new_pt)
    state.active_idx = #points
    state.new_point_name = ""
    save_to_disk()

    set_status(string.format(T("Сохранено: «%s»", "Saved: «%s»"), name), "green")
    return true
end

local function teleport_to_point(idx)
    local points = get_current_track_points()
    local pt = points[idx]
    if not pt or (not pt.rb and not pt.x) then
        set_status(T("Точка не найдена.", "Checkpoint not found."), "yellow")
        return false
    end
    if not carx or not carx.teleport_rigid_body then return false end

    local ok = carx.teleport_rigid_body(pt, (state.vel_mode or 1) - 1)
    if ok then
        state.active_idx = idx
        set_status(string.format(T("Телепорт в «%s»", "Teleported to «%s»"), pt.name), "green")
    else
        set_status(T("Ошибка телепортации.", "Teleport failed."), "red")
    end
    return ok
end

local function delete_point(idx)
    local points = get_current_track_points()
    if idx >= 1 and idx <= #points then
        local removed = table.remove(points, idx)
        if state.active_idx > #points then
            state.active_idx = math.max(1, #points)
        end
        if state.editing_idx == idx then
            state.editing_idx = nil
        end
        save_to_disk()
        set_status(string.format(T("Удалено: «%s»", "Deleted: «%s»"), removed.name or tostring(idx)), "muted")
    end
end

-- ==================== Hotkeys ====================

events.on_key(function(vk, is_down)
    if not is_down or not state.hotkeys_enabled or is_typing() then return end
    if vk == VK_SAVE then
        save_checkpoint(state.new_point_name)
    elseif vk == VK_TP then
        teleport_to_point(state.active_idx)
    end
end)

-- ==================== UI Rendering ====================

local function render_teleport_window()
    check_track_update()

    local p = carx and carx.get_player_car and carx.get_player_car() or nil
    local points = get_current_track_points()
    if state.active_idx > #points and #points > 0 then
        state.active_idx = #points
    end
    local active_pt = points[state.active_idx]

    -- Card 1: Track Header & Info
    ui.card(T("Текущая трасса", "Current Track"), nil, function()
        ui.badge(state.current_track, "accent")
        ui.same_line(0, 8)
        ui.badge(string.format(T("%d чекпоинтов", "%d checkpoints"), #points), #points > 0 and "green" or "muted")

        if not state.auto_track then
            ui.same_line(0, 8)
            if ui.button(T("Авто##tp_reset_auto", "Auto##tp_reset_auto"), 45, 20, "ghost") then
                state.auto_track = true
                state.current_track = detect_current_track()
                state.active_idx = 1
            end
        end

        ui.spacing(4)
        if p then
            local active_dist = active_pt and string.format("%.1f м", distance_3d(p, active_pt)) or "--"
            ui.stat_row({
                { label = T("Скорость", "Speed"), value = string.format(T("%.0f км/ч", "%.0f km/h"), p.speed_kmh or 0.0), tone = "default" },
                { label = T("До активной", "To Active"), value = active_dist, tone = active_pt and "accent" or "muted" },
            })
            ui.spacing(4)
            ui.text_dim(string.format("XYZ: %.1f · %.1f · %.1f", p.x or 0.0, p.y or 0.0, p.z or 0.0))
        else
            ui.text_dim(T("Выйдите на трассу на автомобиле.", "Enter a track first."))
        end

        ui.spacing(4)
        ui.collapsing(T("Выбрать другую трассу вручную", "Select track manually"), false, function()
            local list = {}
            for k, _ in pairs(state.tracks) do
                table.insert(list, k)
            end
            for _, tr in ipairs(KNOWN_TRACKS) do
                local found = false
                for _, ex in ipairs(list) do
                    if ex == tr.name then found = true; break end
                end
                if not found then table.insert(list, tr.name) end
            end
            table.sort(list)

            local cur_idx = 1
            for idx, tn in ipairs(list) do
                if tn == state.current_track then cur_idx = idx; break end
            end

            local ch_tr, sel_tr = ui.combo("##tp_man_tr", cur_idx, list)
            if ch_tr and list[sel_tr] then
                state.current_track = list[sel_tr]
                state.auto_track = false
                state.active_idx = 1
                state.editing_idx = nil
            end
        end)
    end)

    -- Card 2: Add New Checkpoint
    ui.card(T("Новая точка", "Add Checkpoint"), nil, function()
        local ch, new_val = ui.input_text(T("Название точки##tp_new_name", "Point Name##tp_new_name"), state.new_point_name, 64)
        if ch then state.new_point_name = new_val end

        ui.spacing(4)
        if ui.button(T("Запомнить позицию [##tp_add_btn", "Save Pose [##tp_add_btn"), ui.avail_width(), 32, "primary") then
            save_checkpoint(state.new_point_name)
        end
    end)

    -- Card 3: Checkpoints List (Player-list style)
    ui.card(T("Точки телепорта", "Checkpoints"), tostring(#points), function()
        if #points == 0 then
            ui.text_dim(T("На этой трассе пока нет сохранённых точек.", "No checkpoints saved for this track."))
            ui.spacing(2)
            ui.text_dim(T("Нажмите кнопку выше или '[' на трассе.", "Press button above or '[' to save current pose."))
        else
            -- Quick button: Teleport to active
            local act_name = active_pt and active_pt.name or "?"
            local tp_lbl = string.format(T("Телепорт на «%s» ]##tp_quick", "Teleport to «%s» ]##tp_quick"), act_name)
            if ui.button(tp_lbl, ui.avail_width(), 32, "primary") then
                teleport_to_point(state.active_idx)
            end

            ui.spacing(6)
            ui.separator()
            ui.spacing(6)

            -- Render list of checkpoints
            for i, pt in ipairs(points) do
                local is_active = (state.active_idx == i)
                local is_editing = (state.editing_idx == i)

                -- Line 1: Badge + Name / Rename Input
                if is_active then
                    ui.badge(T("АКТИВ", "ACTIVE"), "accent")
                else
                    ui.badge(string.format("#%d", i), "muted")
                end
                ui.same_line(0, 8)

                if is_editing then
                    local ch_edit, edit_val = ui.input_text("##tp_edit_txt_" .. i, state.edit_name_buf, 64)
                    if ch_edit then state.edit_name_buf = edit_val end

                    ui.same_line(0, 4)
                    if ui.button(T("OK##tp_rn_ok_" .. i, "OK##tp_rn_ok_" .. i), 36, 24, "primary") then
                        if state.edit_name_buf and state.edit_name_buf ~= "" then
                            pt.name = state.edit_name_buf
                            save_to_disk()
                        end
                        state.editing_idx = nil
                    end
                    ui.same_line(0, 4)
                    if ui.button(T("Отмена##tp_rn_cnc_" .. i, "Cancel##tp_rn_cnc_" .. i), 56, 24, "ghost") then
                        state.editing_idx = nil
                    end
                else
                    ui.text(pt.name or string.format("Point %d", i))
                end

                -- Line 2: Telemetry + Action buttons
                if not is_editing then
                    local dist_str = p and string.format("%.1f м", distance_3d(p, pt)) or "--"
                    local spd_str = string.format("%.0f %s", pt.speed_kmh or 0, T("км/ч", "km/h"))
                    local time_str = pt.saved_at or "--:--:--"
                    ui.text_dim(string.format("%s · %s · %s", dist_str, spd_str, time_str))

                    ui.spacing(2)
                    local btn_tp_tone = is_active and "primary" or "default"
                    if ui.button(T("ТП##tp_go_" .. i, "TP##tp_go_" .. i), 50, 24, btn_tp_tone) then
                        teleport_to_point(i)
                    end

                    ui.same_line(0, 4)
                    if is_active then
                        ui.button(T("Активна##tp_act_" .. i, "Active##tp_act_" .. i), 68, 24, "ghost")
                    else
                        if ui.button(T("Выбрать##tp_sel_" .. i, "Select##tp_sel_" .. i), 68, 24, "default") then
                            state.active_idx = i
                        end
                    end

                    ui.same_line(0, 4)
                    if ui.button(T("Имя##tp_rn_" .. i, "Rename##tp_rn_" .. i), 48, 24, "ghost") then
                        state.editing_idx = i
                        state.edit_name_buf = pt.name or ""
                    end

                    ui.same_line(0, 4)
                    if ui.button("X##tp_del_" .. i, 28, 24, "danger") then
                        delete_point(i)
                        break
                    end
                end

                ui.spacing(4)
                if i < #points then
                    ui.separator()
                    ui.spacing(4)
                end
            end
        end
    end)

    -- Card 4: Options
    ui.card(T("Параметры", "Options"), nil, function()
        ui.text_dim(T("Скорость при телепорте", "Velocity on Teleport"))
        local vel_labels = T({ "Стоп (0)", "Как в точке", "Текущая" }, { "Stop (0)", "Saved", "Keep" })
        local ch_vel, new_vel = ui.segmented("tp_vel_seg", state.vel_mode, vel_labels)
        if ch_vel then
            state.vel_mode = new_vel
            save_to_disk()
        end

        ui.spacing(4)
        local hk_ch, hk_val = ui.toggle(T("Горячие клавиши [ / ]##tp_hk", "Hotkeys [ / ]##tp_hk"), state.hotkeys_enabled)
        if hk_ch then
            state.hotkeys_enabled = hk_val
            save_to_disk()
        end

        if #points > 0 then
            ui.spacing(4)
            if ui.button(T("Удалить все точки этой трассы##tp_clear_all", "Clear All Points on This Track##tp_clear_all"), ui.avail_width(), 26, "ghost") then
                state.tracks[state.current_track] = {}
                state.active_idx = 1
                state.editing_idx = nil
                save_to_disk()
                set_status(T("Все точки очищены.", "All checkpoints cleared."), "muted")
            end
        end
    end)

    -- Card 5: Status Notification
    if state.status_msg ~= "" then
        if os.clock() - state.status_time > 5.0 then
            state.status_msg = ""
        else
            ui.text_wrapped(state.status_msg, state.status_tone or "muted")
        end
    end
end

-- Initialize from disk
load_from_disk()
state.current_track = detect_current_track()

teleport_mod = {
    state = state,
    save = save_checkpoint,
    tp = teleport_to_point,
    delete = delete_point,
    detect_track = detect_current_track,
    save_to_disk = save_to_disk,
    load_from_disk = load_from_disk,
}

if urushi and urushi.register_window then
    urushi.register_window({
        id = "teleport",
        title = T("Телепорт", "Teleport"),
        icon = "target",
        order = 55,
        default_open = false,
        default_pinned = false,
        x = 710,
        y = 54,
        width = 380,
        render = render_teleport_window,
    })
end

log.info("URUSHI Teleport module loaded (unlimited, per-track persistence).")
