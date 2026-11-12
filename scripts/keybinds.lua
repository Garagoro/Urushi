-- scripts/keybinds.lua
-- Centralized Keybind & Gamepad Management for CarX2 URUSHI
keybinds = keybinds or {}

local DATA_FILE = "urushi_data/keybinds.json"          -- scripts may only write inside urushi_data/
local LEGACY_FILE = "scripts/keybinds.json"            -- read-only fallback for older installs

local DEFAULT_BINDS = {
    menu_toggle    = { source = "keyboard", code = 0x73, mod = 0, name = "F4" },       -- F4
    replay_record  = { source = "keyboard", code = 0x75, mod = 0, name = "F6" },       -- F6
    replay_ghost   = { source = "keyboard", code = 0x76, mod = 0, name = "F7" },       -- F7
    anticollision  = { source = "keyboard", code = 0x4A, mod = 0, name = "J" },        -- J
    perf_menu      = { source = "keyboard", code = 0x74, mod = 0, name = "F5" },       -- F5
    carfx_menu     = { source = "keyboard", code = 0x78, mod = 0, name = "F9" },       -- F9
    dummy_ghost    = { source = "keyboard", code = 0x77, mod = 0, name = "F8" },       -- F8
    tp_save        = { source = "keyboard", code = 0xDB, mod = 0, name = "[" },        -- [
    tp_teleport    = { source = "keyboard", code = 0xDD, mod = 0, name = "]" },        -- ]
}

local current_binds = {}

-- ==================== Minimal JSON Helper ====================

local function escape_str(s)
    local str = tostring(s):gsub("\\", "\\\\"):gsub("\"", "\\\""):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    return "\"" .. str .. "\""
end

local function json_encode(val)
    local t = type(val)
    if t == "number" then
        return string.format("%d", math.floor(val))
    elseif t == "string" then
        return escape_str(val)
    elseif t == "boolean" then
        return val and "true" or "false"
    elseif t == "table" then
        local parts = {}
        for k, v in pairs(val) do
            table.insert(parts, escape_str(k) .. ":" .. json_encode(v))
        end
        return "{" .. table.concat(parts, ",") .. "}"
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
                elseif esc == "\"" then table.insert(buf, "\"")
                elseif esc == "\\" then table.insert(buf, "\\")
                else table.insert(buf, esc)
                end
                pos = pos + 1
                start = pos
            else
                pos = pos + 1
            end
        end
        return table.concat(buf)
    end

    local function parse_number()
        local start = pos
        while pos <= len do
            local b = str:byte(pos)
            if (b >= 48 and b <= 57) or b == 45 or b == 43 or b == 46 or b == 101 or b == 69 then
                pos = pos + 1
            else
                break
            end
        end
        return tonumber(str:sub(start, pos - 1)) or 0
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
            local sep = str:sub(pos, pos)
            if sep == "," then
                pos = pos + 1
            elseif sep == "}" then
                pos = pos + 1
                break
            else
                break
            end
        end
        return obj
    end

    function parse_value()
        skip_ws()
        if pos > len then return nil end
        local c = str:sub(pos, pos)
        if c == "{" then return parse_object()
        elseif c == "\"" then return parse_string()
        elseif (c >= "0" and c <= "9") or c == "-" or c == "+" then return parse_number()
        elseif str:sub(pos, pos + 3) == "true" then pos = pos + 4; return true
        elseif str:sub(pos, pos + 4) == "false" then pos = pos + 5; return false
        elseif str:sub(pos, pos + 3) == "null" then pos = pos + 4; return nil
        else pos = pos + 1; return nil
        end
    end

    return parse_value()
end

-- ==================== Persistence ====================

function keybinds.save()
    local payload = {}
    for k, v in pairs(current_binds) do
        payload[k] = {
            source = v.source or "none",
            code = v.code or 0,
            mod = v.mod or 0,
            name = v.name or "None",
        }
    end

    local text = json_encode(payload)
    local f = io.open(DATA_FILE, "w")
    if f then
        f:write(text)
        f:close()
    end
end

function keybinds.load()
    -- Initialize with defaults
    current_binds = {}
    for k, v in pairs(DEFAULT_BINDS) do
        current_binds[k] = {
            source = v.source,
            code = v.code,
            mod = v.mod,
            name = v.name,
        }
    end

    local f = io.open(DATA_FILE, "r") or io.open(LEGACY_FILE, "r")
    if f then
        local content = f:read("*a")
        f:close()
        local ok, data = pcall(json_decode, content)
        if ok and type(data) == "table" then
            for k, b in pairs(data) do
                if type(b) == "table" and b.source then
                    current_binds[k] = {
                        source = tostring(b.source),
                        code = tonumber(b.code) or 0,
                        mod = tonumber(b.mod) or 0,
                        name = tostring(b.name or "None"),
                    }
                end
            end
        end
    end

    -- Sync menu toggle to C++ InputManager
    if input and input.set_menu_toggle and current_binds.menu_toggle then
        input.set_menu_toggle(current_binds.menu_toggle)
    end
end

-- ==================== API ====================

function keybinds.get(action_id)
    if not current_binds[action_id] then
        if DEFAULT_BINDS[action_id] then
            current_binds[action_id] = {
                source = DEFAULT_BINDS[action_id].source,
                code = DEFAULT_BINDS[action_id].code,
                mod = DEFAULT_BINDS[action_id].mod,
                name = DEFAULT_BINDS[action_id].name,
            }
        else
            current_binds[action_id] = { source = "none", code = 0, mod = 0, name = "None" }
        end
    end
    return current_binds[action_id]
end

function keybinds.set(action_id, bind)
    if not bind then return end
    current_binds[action_id] = {
        source = bind.source or "none",
        code = bind.code or 0,
        mod = bind.mod or 0,
        name = bind.name or "None",
    }
    if action_id == "menu_toggle" and input and input.set_menu_toggle then
        input.set_menu_toggle(current_binds[action_id])
    end
    keybinds.save()
end

function keybinds.is_pressed(action_id)
    local b = keybinds.get(action_id)
    if not b or b.source == "none" or b.code == 0 then return false end
    if not input or not input.is_bound_pressed then return false end

    -- Check if ImGui is capturing keyboard text
    if b.source == "keyboard" and imgui and imgui.is_any_item_active and imgui.is_any_item_active() then
        return false
    end

    return input.is_bound_pressed(b)
end

function keybinds.render(widget_id, label, action_id)
    local cur = keybinds.get(action_id)
    if not ui or not ui.keybind then
        -- Fallback if UI widget unavailable
        ui.keycap(cur.name or "None")
        ui.same_line(0, 8)
        ui.text(label, "muted")
        return false
    end

    local changed, new_bind = ui.keybind(widget_id, label, cur)
    if changed then
        keybinds.set(action_id, new_bind)
        return true
    end
    return false
end

-- Load bindings on module startup
keybinds.load()

log.info("URUSHI Keybinds Manager loaded with Gamepad and Keyboard support.")
