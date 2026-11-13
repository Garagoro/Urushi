-- scripts/config.lua
-- Universal Configuration & Persistence Engine for CarX2 URUSHI
-- Stores all settings in urushi_data/config.json with atomic writes and autosave.
--
-- Features:
--   1. Zero-Boilerplate for Scripts:
--      - Scripts can declare `settings = { ... }` or `state = { ... }`
--      - Scripts can pass `settings = { ... }` to `urushi.register_window`
--      - Scripts can return `{ settings = { ... } }` from the script file
--      - Scripts can call `local cfg = config.bind([id], defaults)`
--      - Core automatically detects settings, restores saved values, and persists all changes!
--   2. Direct API:
--      - config.bind(id, defaults)              bind table to config section, returns reactive proxy
--      - config.auto(id, defaults)              alias for config.bind
--      - config.get(section, [key], [default])  read value or entire section
--      - config.set(section, key, value)        write value
--      - config.save()                          immediate atomic flush to disk
--      - config.reload()                        re-read from disk and notify all providers
--      - config.for_script(id)                  scoped config helper for isolated scripts
--      - config.get_all()                       returns entire config snapshot
--
-- Safety: keeps previous copy in config.bak.json. Debounced periodic flush (0.5s).

local FILE = "urushi_data/config.json"
local BACKUP = "urushi_data/config.bak.json"
local SAVE_DELAY = 0.5          -- seconds after the last change to flush to disk

local config = {}
local data = { version = 1 }
local dirty = false
local dirty_since = 0

config.registry = {}

-- ==================== tiny JSON ====================

local function esc(s)
    return (s:gsub('[%c"\\]', function(c)
        local map = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
        return map[c] or string.format("\\u%04x", c:byte())
    end))
end

local function is_array(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n == #t
end

local function encode(v, depth)
    depth = depth or 0
    local t = type(v)
    if depth > 12 then return "null" end
    if t == "nil" then return "null"
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        if math.type(v) == "integer" then return tostring(v) end
        return string.format("%.9g", v)
    elseif t == "string" then return '"' .. esc(v) .. '"'
    elseif t == "table" then
        local raw = v._raw or v
        local parts = {}
        if next(raw) == nil or is_array(raw) then
            for i = 1, #raw do
                local val = raw[i]
                local vt = type(val)
                if vt ~= "function" and vt ~= "userdata" and vt ~= "thread" then
                    parts[#parts + 1] = encode(val, depth + 1)
                else
                    parts[#parts + 1] = "null"
                end
            end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local keys = {}
        for k in pairs(raw) do keys[#keys + 1] = tostring(k) end
        table.sort(keys)
        for _, k in ipairs(keys) do
            local val = raw[k]
            if val == nil then val = raw[tonumber(k)] end
            local vt = type(val)
            if vt ~= "function" and vt ~= "userdata" and vt ~= "thread" and vt ~= "nil" then
                parts[#parts + 1] = '"' .. esc(k) .. '":' .. encode(val, depth + 1)
            end
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return "null"
end

local function decode(s)
    local i = 1
    local function ws() i = s:find("%S", i) or #s + 1 end
    local value

    local function str()
        i = i + 1
        local out = {}
        while true do
            local c = s:sub(i, i)
            if c == "" then error("unterminated string") end
            if c == '"' then i = i + 1; return table.concat(out) end
            if c == "\\" then
                local n = s:sub(i + 1, i + 1)
                local map = { b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
                if n == "u" then
                    local cp = tonumber(s:sub(i + 2, i + 5), 16)
                    if not cp then error("bad escape") end
                    out[#out + 1] = utf8.char(cp)
                    i = i + 6
                elseif map[n] then
                    out[#out + 1] = map[n]
                    i = i + 2
                else error("bad escape") end
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
    end

    function value(depth)
        if depth > 12 then error("too deep") end
        ws()
        local c = s:sub(i, i)
        if c == "{" then
            i = i + 1
            local t = {}
            ws()
            if s:sub(i, i) == "}" then i = i + 1; return t end
            while true do
                ws()
                if s:sub(i, i) ~= '"' then error("object key expected") end
                local k = str()
                ws()
                if s:sub(i, i) ~= ":" then error("':' expected") end
                i = i + 1
                t[k] = value(depth + 1)
                ws()
                local d = s:sub(i, i)
                i = i + 1
                if d == "}" then return t end
                if d ~= "," then error("',' expected") end
            end
        elseif c == "[" then
            i = i + 1
            local t = {}
            ws()
            if s:sub(i, i) == "]" then i = i + 1; return t end
            while true do
                t[#t + 1] = value(depth + 1)
                ws()
                local d = s:sub(i, i)
                i = i + 1
                if d == "]" then return t end
                if d ~= "," then error("',' expected") end
            end
        elseif c == '"' then return str()
        elseif s:sub(i, i + 3) == "true" then i = i + 4; return true
        elseif s:sub(i, i + 4) == "false" then i = i + 5; return false
        elseif s:sub(i, i + 3) == "null" then i = i + 4; return nil
        else
            local num = s:match("^-?%d+%.?%d*[eE]?[+-]?%d*", i)
            if not num or num == "" then error("unexpected character at " .. i) end
            i = i + #num
            return tonumber(num)
        end
    end

    local v = value(0)
    ws()
    if i <= #s then error("trailing data") end
    return v
end

-- ==================== file I/O ====================

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local text = f:read("*a")
    f:close()
    return text
end

local function write_file_content(path, text)
    local f = io.open(path, "w")
    if not f then return false end
    f:write(text)
    f:close()
    return true
end

local function try_load(path)
    local text = read_file(path)
    if not text or text == "" then return nil end
    local ok, t = pcall(decode, text)
    if ok and type(t) == "table" and not (next(t) ~= nil and #t > 0) then return t end
    return nil
end

-- ==================== Table Helpers ====================

local function deep_copy(v)
    if type(v) ~= "table" then return v end
    local raw = v._raw or v
    local r = {}
    for k, sub in pairs(raw) do
        local st = type(sub)
        if st ~= "function" and st ~= "userdata" and st ~= "thread" then
            r[k] = deep_copy(sub)
        end
    end
    return r
end

local function tables_equal(a, b)
    if a == b then return true end
    if type(a) ~= "table" or type(b) ~= "table" then return false end
    local raw_a = a._raw or a
    local raw_b = b._raw or b
    for k, v in pairs(raw_a) do
        local vt = type(v)
        if vt ~= "function" and vt ~= "userdata" and vt ~= "thread" then
            if not tables_equal(v, raw_b[k]) then return false end
        end
    end
    for k, v in pairs(raw_b) do
        local vt = type(v)
        if vt ~= "function" and vt ~= "userdata" and vt ~= "thread" then
            if raw_a[k] == nil then return false end
        end
    end
    return true
end

local function detect_caller_id(level)
    level = level or 2
    local info = debug and debug.getinfo and debug.getinfo(level, "S")
    if info and info.short_src then
        local src = info.short_src
        local cloud_id = src:match("scripts[/\\]cloud[/\\]([^/\\]+)")
        if cloud_id then return cloud_id end
        local script_name = src:match("([^/\\]+)%.lua$")
        if script_name then return script_name end
    end
    return "general"
end

local function resolve_section(id)
    if not id then return "general" end
    local s = tostring(id)
    if s:sub(1, 7) == "script:" then s = s:sub(8) end
    if s:sub(1, 6) == "cloud:" then s = s:sub(7) end
    return s
end

-- ==================== Reactive Proxy ====================

local function make_reactive(section, store, on_change, prefix)
    prefix = prefix or ""
    local proxy = {}
    local sub_proxies = {}

    local mt = {
        __index = function(_, k)
            if k == "_raw" then return store end
            local v = store[k]
            if type(v) == "table" then
                if not sub_proxies[k] then
                    sub_proxies[k] = make_reactive(section, v, on_change, prefix .. tostring(k) .. ".")
                end
                return sub_proxies[k]
            end
            return v
        end,

        __newindex = function(_, k, v)
            local raw_val = (type(v) == "table" and v._raw) and v._raw or v
            if store[k] == raw_val and type(raw_val) ~= "table" then return end
            sub_proxies[k] = nil
            if raw_val == nil then
                store[k] = nil
            else
                store[k] = deep_copy(raw_val)
            end
            if on_change then
                on_change(k, raw_val)
            end
        end,

        __pairs = function(_)
            return pairs(store)
        end,

        __ipairs = function(_)
            return ipairs(store)
        end,

        __len = function(_)
            return #store
        end,

        __tostring = function(_)
            return "config_proxy(" .. section .. (prefix ~= "" and (":" .. prefix) or "") .. ")"
        end,
    }

    return setmetatable(proxy, mt)
end

-- ==================== Persistence API ====================

function config.mark_dirty()
    if not dirty then dirty_since = os.clock() end
    dirty = true
end

function config.get(a1, a2, a3)
    local section, key, default
    if a3 ~= nil then
        section = resolve_section(a1)
        key = a2
        default = a3
    elseif a2 ~= nil then
        local sec = resolve_section(a1)
        if data[sec] ~= nil or data["script:" .. sec] ~= nil then
            section = sec
            key = a2
            default = nil
        else
            section = "general"
            key = a1
            default = a2
        end
    else
        section = resolve_section(a1)
        local s = data[section] or data["script:" .. section]
        return type(s) == "table" and deep_copy(s) or nil
    end

    local s = data[section] or data["script:" .. section]
    if type(s) == "table" then
        if key == nil then return deep_copy(s) end
        local v = s[key]
        if v ~= nil then return deep_copy(v) end
    end
    return deep_copy(default)
end

function config.set(a1, a2, a3)
    local section, key, value
    if a3 ~= nil then
        section = resolve_section(a1)
        key = a2
        value = a3
    else
        section = "general"
        key = a1
        value = a2
    end
    if section == "version" then return end

    local s = data[section]
    if type(s) ~= "table" then
        if value == nil then return end
        s = {}
        data[section] = s
    end
    local raw_val = (type(value) == "table" and value._raw) and value._raw or value
    if raw_val == nil then
        s[key] = nil
    else
        s[key] = deep_copy(raw_val)
    end

    -- Synchronize registered live store table if present
    local reg = config.registry[section] or config.registry["script:" .. section]
    if reg and type(reg.store) == "table" then
        if raw_val == nil then
            reg.store[key] = nil
        else
            reg.store[key] = deep_copy(raw_val)
        end
    end

    config.mark_dirty()
end

function config.flush()
    if dirty then
        return config.save()
    end
    return true
end

function config.section(name)
    local sec = resolve_section(name)
    local out = {}
    local s = data[sec] or data["script:" .. sec]
    if type(s) == "table" then
        for k, v in pairs(s) do out[k] = deep_copy(v) end
    end
    return out
end

function config.sync()
    for section, reg in pairs(config.registry) do
        local sec = resolve_section(section)
        if reg.store then
            if not tables_equal(reg.store, data[sec]) then
                data[sec] = deep_copy(reg.store)
                config.mark_dirty()
            end
        end
        if reg.on_save then
            local ok, res = pcall(reg.on_save)
            if ok and type(res) == "table" and not tables_equal(res, data[sec]) then
                data[sec] = deep_copy(res)
                config.mark_dirty()
            end
        end
    end

    -- Automatic introspection of all live C++ UI elements across all scripts & windows
    if urushi and urushi.get_ui_elements then
        local ok, ui_elems = pcall(urushi.get_ui_elements)
        if ok and type(ui_elems) == "table" then
            for win_id, elems in pairs(ui_elems) do
                local sec = resolve_section(win_id)
                if type(elems) == "table" then
                    for elem_id, el in pairs(elems) do
                        if type(el) == "table" and el.value ~= nil then
                            if type(data[sec]) ~= "table" then data[sec] = {} end
                            if el.changed or data[sec][elem_id] ~= el.value then
                                data[sec][elem_id] = deep_copy(el.value)
                                config.mark_dirty()
                            end
                        end
                    end
                end
            end
        end
    end
end

function config.get_all()
    config.sync()
    return deep_copy(data)
end

-- ==================== Provider Registration ====================

function config.register(opts)
    if type(opts) ~= "table" then return end
    local id = opts.id or detect_caller_id(3)
    local section = opts.section or resolve_section(id)

    local reg = {
        id = id,
        section = section,
        store = opts.store or opts.settings or opts.config,
        on_save = opts.on_save,
        on_load = opts.on_load,
    }

    local saved = data[section]
    if saved ~= nil then
        if reg.on_load then
            pcall(reg.on_load, deep_copy(saved))
        elseif type(reg.store) == "table" and type(saved) == "table" then
            for k, v in pairs(saved) do
                reg.store[k] = deep_copy(v)
            end
        end
    elseif type(opts.defaults) == "table" and type(reg.store) == "table" then
        for k, v in pairs(opts.defaults) do
            if reg.store[k] == nil then reg.store[k] = deep_copy(v) end
        end
        data[section] = deep_copy(reg.store)
    end

    config.registry[section] = reg
    return reg
end

-- ==================== Reactive Auto-Config (bind) ====================

function config.bind(arg1, arg2)
    local id, defaults
    if type(arg1) == "string" then
        id = arg1
        defaults = type(arg2) == "table" and arg2 or {}
    else
        defaults = type(arg1) == "table" and arg1 or {}
        id = detect_caller_id(3)
    end

    local section = resolve_section(id)

    -- If defaults is already a reactive proxy, return its store
    local store = (type(defaults) == "table" and defaults._raw) and defaults._raw or defaults

    local saved = data[section] or data["script:" .. section]

    -- Automatic migration of legacy nested configurations:
    if type(saved) == "table" then
        if section == "optimizer" and type(saved.config) == "table" then
            for k, v in pairs(saved.config) do saved[k] = v end
            saved.config = nil
        elseif section == "steer_assist" and type(saved.config) == "table" then
            for k, v in pairs(saved.config) do saved[k] = v end
            saved.config = nil
        elseif section == "anticollision" and type(saved.ghost_cfg) == "table" then
            for k, v in pairs(saved.ghost_cfg) do saved[k] = v end
            saved.ghost_cfg = nil
        end

        -- Merge saved values into store in-place so defaults for any missing keys are preserved
        for k, v in pairs(saved) do
            store[k] = deep_copy(v)
        end
    end

    -- Update authoritative data section
    data[section] = deep_copy(store)

    local proxy = make_reactive(section, store, function(k, v)
        data[section] = deep_copy(store)
        config.mark_dirty()
    end)

    config.registry[section] = {
        id = id,
        section = section,
        store = store,
        proxy = proxy,
    }

    return proxy
end
config.auto = config.bind
config.bind_script_table = config.bind

function config.bind_script_env(script_id, env)
    if type(env) ~= "table" then return false end
    local candidates = { "settings", "SETTINGS", "config_data", "CONFIG", "state", "STATE" }
    for _, name in ipairs(candidates) do
        local t = rawget(env, name)
        if type(t) == "table" and not t._raw then
            local proxy = config.bind(script_id, t)
            rawset(env, name, proxy)
            log.info("config: auto-bound '" .. name .. "' for script '" .. tostring(script_id) .. "'")
            return true
        end
    end
    return false
end

-- ==================== Scoped Environment Binding ====================

function config.for_script(id)
    local section = resolve_section(id)
    return {
        get = function(a1, a2, a3)
            if a3 ~= nil or (a2 ~= nil and type(a1) == "string" and data[a1]) then
                return config.get(a1, a2, a3)
            elseif a2 ~= nil then
                return config.get(section, a1, a2)
            else
                return config.get(section, a1, nil)
            end
        end,
        set = function(a1, a2, a3)
            if a3 ~= nil then
                return config.set(a1, a2, a3)
            else
                return config.set(section, a1, a2)
            end
        end,
        auto = function(defaults) return config.bind(section, defaults) end,
        bind = function(defaults) return config.bind(section, defaults) end,
        register = function(opts)
            opts = opts or {}
            opts.id = opts.id or id
            opts.section = opts.section or section
            return config.register(opts)
        end,
        save = config.save,
        reload = config.reload,
        flush = config.flush,
        section = function() return config.section(section) end,
        mark_dirty = config.mark_dirty,
    }
end

-- ==================== Save & Reload Engine ====================

function config.save()
    -- Pull authoritative state from all registered providers
    for section, reg in pairs(config.registry) do
        local sec = resolve_section(section)
        if reg.on_save then
            local ok, res = pcall(reg.on_save)
            if ok and type(res) == "table" then
                if type(data[sec]) ~= "table" then data[sec] = {} end
                for k, v in pairs(res) do
                    data[sec][k] = deep_copy(v)
                end
            end
        elseif reg.store then
            data[sec] = deep_copy(reg.store)
        end
    end

    local text = encode(data)
    local old = read_file(FILE)
    if old and old ~= "" and pcall(decode, old) then write_file_content(BACKUP, old) end
    if write_file_content(FILE, text) then
        dirty = false
        return true
    end
    log.warn("config: cannot write " .. FILE)
    return false
end

function config.reload()
    local t = try_load(FILE)
    if not t then
        t = try_load(BACKUP)
        if t then log.warn("config.json missing or corrupt, restored from config.bak.json") end
    end
    data = t or { version = 1 }
    data.version = data.version or 1
    dirty = false

    -- Normalize legacy "script:foo" to "foo"
    for k, v in pairs(data) do
        if type(k) == "string" and k:sub(1, 7) == "script:" then
            local clean = k:sub(8)
            if data[clean] == nil then
                data[clean] = v
            end
        end
    end

    -- Sync back to all live registered providers
    for section, reg in pairs(config.registry) do
        local sec = resolve_section(section)
        local saved = data[sec]
        if saved ~= nil then
            if reg.on_load then
                pcall(reg.on_load, deep_copy(saved))
            elseif type(reg.store) == "table" and type(saved) == "table" then
                for k, v in pairs(saved) do
                    reg.store[k] = deep_copy(v)
                end
            end
        end
    end

    -- Feed stored values into C++ UI introspection registry
    if urushi and urushi.set_ui_element then
        for sec, section_data in pairs(data) do
            if type(section_data) == "table" and sec ~= "version" then
                for elem_id, val in pairs(section_data) do
                    local vt = type(val)
                    if vt == "boolean" or vt == "number" or vt == "string" then
                        pcall(urushi.set_ui_element, sec, elem_id, val)
                    end
                end
            end
        end
    end
end

-- ==================== Autosave & Deep State Scanner ====================

config.reload()

local last_table_check = 0
if events and events.on_update then
    events.on_update(function(dt)
        local now = os.clock()

        -- Scan all registered script stores every 250ms for in-place table mutations
        if now - last_table_check >= 0.25 then
            last_table_check = now
            config.sync()
        end

        -- Debounced disk persistence
        if dirty and now - dirty_since >= SAVE_DELAY then
            config.save()
        end
    end)
end

_G.config = config
