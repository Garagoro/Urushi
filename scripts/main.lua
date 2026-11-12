-- URUSHI: Main Entry Point
--
-- Trust tiers (see docs/SECURITY.md):
--   TRUSTED  : core + built-in modules shipped in the repository. Share the real globals.
--   ISOLATED : user scripts dropped into scripts/ and everything installed from the cloud library. They run in their
--              own environment: no updater/cloud/settings access, private copies of the API tables, private data folder.
-- The VM itself has no os.execute / io.popen / package / debug / bytecode loading, and io.open only reaches
-- urushi_data/ (see core/lua_hardening.cpp), so even trusted code cannot run programs or create code files.
log.info("URUSHI main.lua initialized")

local function copy(t)
    local r = {}
    for k, v in pairs(t) do r[k] = v end
    return r
end

-- Core modules: always loaded (UI language, config, keybinds)
for _, id in ipairs({ "i18n", "config", "keybinds" }) do
    local ok, err = urushi.load_script(id)
    if not ok then log.error("Core script '" .. id .. "' failed: " .. tostring(err)) end
end

-- ---------------------------------------------------------------------------------------------
-- Isolated environment for untrusted scripts
-- ---------------------------------------------------------------------------------------------
local PRIVILEGED = {
    set_setting = true, get_setting = true, list_scripts = true, load_script = true,
    reload_scripts = true, open_crash_dir = true, get_last_crash = true,
}
local LUA_FUNCS = { "assert", "error", "ipairs", "pairs", "next", "pcall", "xpcall", "select", "tonumber", "tostring",
                    "type", "rawequal", "rawget", "rawset", "rawlen", "setmetatable", "print" }
local LUA_LIBS = { "string", "table", "math", "utf8", "coroutine", "os", "debug" }
local API_MODULES = { "carx", "steer_assist", "carfx", "replay", "memory", "unity", "optimizer", "ui", "input", "imgui", "gui",
                      "events", "log", "i18n", "keybinds" }

local function make_env(script_id, cloud_id)
    local env = {}
    for _, n in ipairs(LUA_FUNCS) do env[n] = _G[n] end
    for _, n in ipairs(LUA_LIBS) do env[n] = copy(_G[n]) end
    for _, n in ipairs(API_MODULES) do
        if _G[n] then env[n] = copy(_G[n]) end
    end
    local pub = {}
    for k, v in pairs(urushi) do
        if not PRIVILEGED[k] then pub[k] = v end
    end
    env.urushi = pub
    env._G = env
    if config and config.for_script then env.config = config.for_script(script_id) end   -- own section "script:<id>"
    if cloud_id then
        -- multi-file cloud scripts: load a sibling file from the script's own folder
        env.include = function(rel) return cloud.include(cloud_id, rel, env) end
    end

    -- strings share one metatable for the whole VM: never hand it out
    env.getmetatable = function(x)
        if type(x) == "table" then return getmetatable(x) end
        return nil
    end
    -- load() would default to the REAL globals: always bind it to this environment
    env.load = function(chunk, name, _, e) return load(chunk, name, "t", e or env) end

    -- private data folder: urushi_data/<script>/<file>.(json|txt|cfg|csv)
    local folder = tostring(script_id):gsub("[^%w_%-]", "_")
    env.io = {
        type = io.type,
        open = function(path, mode)
            path = tostring(path)
            if path:find("[:\\]") or path:find("%.%.") or path:sub(1, 1) == "/" then
                return nil, path .. ": access denied", 13
            end
            return io.open("urushi_data/" .. folder .. "/" .. path, mode)
        end,
    }
    return env
end

-- ---------------------------------------------------------------------------------------------
-- Script list
-- ---------------------------------------------------------------------------------------------
-- Each entry is loaded in the sandbox (own time budget, errors are isolated):
-- a broken or hanging script is reported in System -> Scripts instead of taking everything down.
-- Enabled flags are stored in urushi_settings.ini ([scripts] section).
urushi_scripts = { list = {}, dirty = false, cloud_loaded = {} }

local BUILTIN = {
    { id = "telemetry_hud",    ru = "Телеметрия (HUD)",          en = "Telemetry HUD" },
    { id = "ghost_test",       ru = "Призрак и 3D-болванка",     en = "Ghost & 3D Dummy" },
    { id = "teleport_mod",     ru = "Телепорт по точкам",        en = "Checkpoint Teleport" },
    { id = "replay_mod",       ru = "Реплеи",                    en = "Replays" },
    { id = "carfx_mod",        ru = "CarFX шейдеры",             en = "CarFX Shaders" },
    { id = "steer_assist_mod", ru = "Помощь руля (155°)",        en = "Steering Assist (155°)" },
    { id = "tires_mod",        ru = "Кузов и шины",              en = "Body & Tires" },
    { id = "perf_optimizer",   ru = "Оптимизатор FPS",           en = "FPS Optimizer" },
}

-- Files that are part of the framework and never listed as user scripts
local RESERVED = { main = true, i18n = true, config = true, keybinds = true, system_mod = true, runner = true }
for _, b in ipairs(BUILTIN) do RESERVED[b.id] = true end

local function enabled_flag(id, default)
    local v = urushi.get_setting("scripts", id, default and "1" or "0")
    return v ~= "0"
end

local function load_entry(e)
    if e.cloud then urushi_scripts.cloud_loaded[e.cloud_id] = e.version end
    if not e.enabled then
        e.status = "disabled"
        return
    end
    if e.shader_only then e.status = "ok"; return end        -- shader pack: files are in shaders/cloud/<id>/, nothing to run
    local ok, err
    if e.cloud then
        ok, err = cloud.load(e.cloud_id, make_env(e.id, e.cloud_id))
    elseif e.user then
        ok, err = urushi.load_script(e.id, make_env(e.id))
    else
        ok, err = urushi.load_script(e.id)           -- trusted built-in
    end
    if ok then
        e.status = "ok"
    else
        e.status = "error"
        e.err = tostring(err)
        log.error("Script '" .. e.id .. "' failed to load: " .. e.err)
    end
end

for _, b in ipairs(BUILTIN) do
    local e = { id = b.id, ru = b.ru, en = b.en, user = false, enabled = enabled_flag(b.id, true) }
    table.insert(urushi_scripts.list, e)
    load_entry(e)
end

-- User scripts dropped into scripts/: listed, but OFF until the user enables them. Run isolated.
for _, name in ipairs(urushi.list_scripts()) do
    if not RESERVED[name] then
        local e = { id = name, user = true, isolated = true, enabled = enabled_flag(name, false) }
        table.insert(urushi_scripts.list, e)
        load_entry(e)
    end
end

-- Scripts installed from the cloud library (scripts/cloud/<id>/): ON right after install, user can switch off. Isolated.
if cloud and cloud.installed then
    for _, c in ipairs(cloud.installed()) do
        local key = "cloud:" .. c.id
        local e = {
            id = key, cloud = true, isolated = true, cloud_id = c.id, version = c.version, entry = c.entry, shader_only = (c.entry == ""),
            en = c.name, ru = (c.name_ru ~= "" and c.name_ru or c.name),
            enabled = enabled_flag(key, true),
        }
        table.insert(urushi_scripts.list, e)
        load_entry(e)
    end
end

-- System window (hosts the Scripts / Stability cards). The dev runner is a code-execution channel: opt-in only.
local core = { "system_mod" }
if urushi.get_setting("dev", "runner", "0") == "1" then table.insert(core, "runner") end
for _, id in ipairs(core) do
    local ok, err = urushi.load_script(id)
    if not ok then log.error("Core script '" .. id .. "' failed: " .. tostring(err)) end
end

if i18n and i18n.sync_titles then
    i18n.sync_titles()
end

log.info("All modules loaded successfully")
