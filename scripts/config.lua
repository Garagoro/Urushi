-- scripts/config.lua
-- The universal configuration & persistence engine: single JSON file (urushi_data/config.json) with autosave.
--
-- Features:
--   1. Zero-Boilerplate for Scripts:
--      - Scripts can declare `settings = { ... }` or `SETTINGS = { ... }` in their environment:
--        The core automatically detects it, restores saved values from config.json, and auto-saves changes!
--      - Scripts can pass `settings = { ... }` to `urushi.register_window`:
--        The core automatically detects it and persists it.
--      - Scripts can call `local cfg = config.auto({ speed = 100, enabled = true })`:
--        Returns a reactive proxy table that auto-saves on any property mutation!
--      - Scripts can use lifecycle hooks: `config.register({ id = "my_mod", on_save = fn, on_load = fn })`.
--   2. Direct API:
--      - config.get(section, key, default)      read a value
--      - config.set(section, key, value)        write a value; debounced autosave ~2s
--      - config.auto([id], defaults)            reactive table proxy bound to config.json
--      - config.register(opts)                  register custom state provider (on_save, on_load, store)
--      - config.save()                          write to disk immediately
--      - config.reload()                        re-read from disk and notify all registered providers
--      - config.for_script(id)                  scoped config object for isolated script environments
--
-- Safety: keeps previous good copy in config.bak.json. Atomic debounced flushing.

local FILE = "urushi_data/config.json"
local BACKUP = "urushi_data/config.bak.json"
local SAVE_DELAY = 2.0          -- seconds after the last change

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
            for i = 1, #raw do parts[i] = encode(raw[i], depth + 1) end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local keys = {}
        for k in pairs(raw) do keys[#keys + 1] = tostring(k) end
        table.sort(keys)
        for _, k in ipairs(keys) do
            local val = raw[k]
            if val == nil then val = raw[tonumber(k)] end
            parts[#parts + 1] = '"' .. esc(k) .. '":' .. encode(val, depth + 1)
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
        r[k] = deep_copy(sub)
    end
    return r
end

local function tables_equal(a, b)
    if a == b then return true end
    if type(a) ~= "table" or type(b) ~= "table" then return false end
    local raw_a = a._raw or a
    local raw_b = b._raw or b
    for k, v in pairs(raw_a) do
        if not tables_equal(v, raw_b[k]) then return false end
    end
    for k in pairs(raw_b) do
        if raw_a[k] == nil then return false end
    end
    return true
end

local function detect_caller_id(level)
    level = level or 2
    local info = debug and debug.getinfo and debug.getinfo(level, "S")
    if info and info.short_src then
        local src = info.short_src
        local cloud_id = src:match("scripts[/\\]cloud[/\\]([^/\\]+)")
        if cloud_id then return "cloud:" .. cloud_id end
        local script_name = src:match("([^/\\]+)%.lua$")
        if script_name then return script_name end
    end
    return "general"
end

local function resolve_section(id)
    id = tostring(id)
    if id:find("^script:") or id:find("^cloud:") then return id end
    local known = { ui = true, carfx = true, steer_assist = true, optimizer = true, tires = true, anticollision = true }
    if known[id] then return id end
    return "script:" .. id
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

function config.get(section, key, default)
    local s = data[section]
    if type(s) == "table" then
        local v = s[key]
        if v ~= nil then return deep_copy(v) end
    end
    return deep_copy(default)
end

function config.set(section, key, value)
    if section == "version" then return end
    local s = data[section]
    if type(s) ~= "table" then
        if value == nil then return end
        s = {}
        data[section] = s
    end
    local raw_val = (type(value) == "table" and value._raw) and value._raw or value
    if type(raw_val) ~= "table" and s[key] == raw_val then return end
    if raw_val == nil then
        s[key] = nil
    else
        s[key] = deep_copy(raw_val)
    end
    if next(s) == nil then data[section] = nil end
    config.mark_dirty()
end

function config.flush()
    if dirty then
        return config.save()
    end
    return true
end

function config.section(name)
    local out = {}
    local s = data[name]
    if type(s) == "table" then
        for k, v in pairs(s) do out[k] = deep_copy(v) end
    end
    return out
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

-- ==================== Reactive Auto-Config ====================

function config.auto(arg1, arg2)
    local id, defaults
    if type(arg1) == "string" then
        id = arg1
        defaults = type(arg2) == "table" and arg2 or {}
    else
        defaults = type(arg1) == "table" and arg1 or {}
        id = detect_caller_id(3)
    end

    local section = resolve_section(id)

    -- If this is already a reactive proxy for this section, return it
    if type(defaults) == "table" and defaults._raw then
        return defaults
    end

    -- Mutate defaults directly in place so references stay synchronized
    local store = defaults
    local saved = data[section]
    if type(saved) == "table" then
        for k, v in pairs(saved) do
            store[k] = deep_copy(v)
        end
    else
        data[section] = deep_copy(store)
    end

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
config.bind = config.auto

-- ==================== Automatic Script Introspection ====================

function config.bind_script_table(script_id, tbl)
    if type(tbl) ~= "table" then return tbl end
    return config.auto(script_id, tbl)
end

function config.bind_script_env(script_id, env)
    if type(env) ~= "table" then return false end
    local candidates = { "settings", "SETTINGS", "config_data", "CONFIG", "state", "STATE" }
    for _, name in ipairs(candidates) do
        local t = rawget(env, name)
        if type(t) == "table" and not t._raw then
            local proxy = config.auto(script_id, t)
            rawset(env, name, proxy)
            log.info("config: auto-bound '" .. name .. "' (reactive) for script '" .. tostring(script_id) .. "'")
            return true
        end
    end
    return false
end

-- ==================== Scoped Environment Binding ====================

function config.for_script(id)
    local section = resolve_section(id)
    return {
        get = function(key, default) return config.get(section, key, default) end,
        set = function(key, value) config.set(section, key, value) end,
        auto = function(defaults) return config.auto(section, defaults) end,
        bind = function(defaults) return config.auto(section, defaults) end,
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
        if reg.on_save then
            local ok, res = pcall(reg.on_save)
            if ok and type(res) == "table" then
                data[section] = deep_copy(res)
            end
        elseif reg.store then
            data[section] = deep_copy(reg.store)
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

    -- Sync back to all live registered providers
    for section, reg in pairs(config.registry) do
        local saved = data[section]
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
end

-- ==================== Autosave & Dirty Scanner ====================

config.reload()

local last_table_check = 0
if events and events.on_update then
    events.on_update(function(dt)
        local now = os.clock()

        -- Check registered plain tables for in-place mutations (e.g. settings.val = 5)
        if now - last_table_check >= 1.0 then
            last_table_check = now
            for section, reg in pairs(config.registry) do
                if reg.store and not reg.proxy and not reg.on_save then
                    if not tables_equal(reg.store, data[section]) then
                        data[section] = deep_copy(reg.store)
                        config.mark_dirty()
                    end
                end
            end
        end

        if dirty and now - dirty_since >= SAVE_DELAY then
            config.save()
        end
    end)
end

_G.config = config
