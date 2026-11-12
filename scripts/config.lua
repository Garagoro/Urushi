-- scripts/config.lua
-- The one and only config system: a single JSON file (urushi_data/config.json) with autosave.
-- Deliberately tiny. Want profiles / presets / per-car configs / import-export? Build them on top of this module
-- (or replace this file): everything goes through config.get / config.set / config.save.
--
--   config.get(section, key, default)      read a value (default if missing)
--   config.set(section, key, value)        write a value (nil removes it); autosaved ~2 s later
--   config.section(name)                   table with all keys of a section (read-only copy)
--   config.save()                          write now (also happens automatically)
--   config.reload()                        re-read the file from disk
--   config.for_script(id)                  { get(key, default), set(key, value) } fixed to section "script:<id>"
--                                          (this is what isolated / cloud scripts get as `config`)
--
-- File format: { "version": 1, "<section>": { "<key>": <json value> } }
-- Only values that were actually changed are stored, so new settings in updates need no migration.
-- Safety: the previous good file is kept as config.bak.json and used if config.json is missing or corrupt.

local FILE = "urushi_data/config.json"
local BACKUP = "urushi_data/config.bak.json"
local SAVE_DELAY = 2.0          -- seconds after the last change

local config = {}
local data = { version = 1 }
local dirty = false
local dirty_since = 0

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
        local parts = {}
        if next(v) == nil or is_array(v) then
            for i = 1, #v do parts[i] = encode(v[i], depth + 1) end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = tostring(k) end
        table.sort(keys)
        for _, k in ipairs(keys) do
            local val = v[k]
            if val == nil then val = v[tonumber(k)] end
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

local function write_file(path, text)
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

function config.reload()
    local t = try_load(FILE)
    if not t then
        t = try_load(BACKUP)
        if t then log.warn("config.json missing or corrupt, restored from config.bak.json") end
    end
    data = t or { version = 1 }
    data.version = data.version or 1
    dirty = false
end

function config.save()
    local text = encode(data)
    -- keep the last good copy first; if we crash while writing config.json, the backup is used on next start
    local old = read_file(FILE)
    if old and old ~= "" and pcall(decode, old) then write_file(BACKUP, old) end
    if write_file(FILE, text) then
        dirty = false
        return true
    end
    log.warn("config: cannot write " .. FILE)
    return false
end

-- ==================== API ====================

function config.get(section, key, default)
    local s = data[section]
    if type(s) == "table" then
        local v = s[key]
        if v ~= nil then return v end
    end
    return default
end

function config.set(section, key, value)
    if section == "version" then return end
    local s = data[section]
    if type(s) ~= "table" then
        if value == nil then return end
        s = {}
        data[section] = s
    end
    if s[key] == value then return end          -- no-op writes do not touch the disk
    s[key] = value
    if next(s) == nil then data[section] = nil end
    if not dirty then dirty_since = os.clock() end
    dirty = true
end

function config.section(name)
    local out = {}
    local s = data[name]
    if type(s) == "table" then
        for k, v in pairs(s) do out[k] = v end
    end
    return out
end

function config.for_script(id)
    local section = "script:" .. tostring(id)
    return {
        get = function(key, default) return config.get(section, key, default) end,
        set = function(key, value) config.set(section, key, value) end,
        save = config.save,
    }
end

-- ==================== autosave ====================

config.reload()

if events and events.on_update then
    events.on_update(function()
        if dirty and os.clock() - dirty_since >= SAVE_DELAY then
            config.save()
        end
    end)
end

_G.config = config
