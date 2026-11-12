-- URUSHI: Main Entry Point
log.info("URUSHI main.lua initialized")

-- Core modules: always loaded (UI language, keybinds)
dofile("scripts/i18n.lua")
dofile("scripts/keybinds.lua")

-- Script list. Each entry is loaded in the sandbox (own time budget, errors are isolated):
-- a broken or hanging script is reported in System -> Scripts instead of taking everything down.
-- Enabled flags are stored in urushi_settings.ini ([scripts] section).
urushi_scripts = { list = {}, dirty = false }

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
local RESERVED = { main = true, i18n = true, keybinds = true, system_mod = true, runner = true }
for _, b in ipairs(BUILTIN) do RESERVED[b.id] = true end

local function enabled_flag(id, default)
    local v = urushi.get_setting("scripts", id, default and "1" or "0")
    return v ~= "0"
end

local function load_entry(e)
    if not e.enabled then
        e.status = "disabled"
        return
    end
    local ok, err = urushi.load_script(e.id)
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

-- User scripts dropped into scripts/: listed, but OFF until the user enables them
for _, name in ipairs(urushi.list_scripts()) do
    if not RESERVED[name] then
        local e = { id = name, user = true, enabled = enabled_flag(name, false) }
        table.insert(urushi_scripts.list, e)
        load_entry(e)
    end
end

-- System window (hosts the Scripts / Stability cards) and the dev runner
for _, id in ipairs({ "system_mod", "runner" }) do
    local ok, err = urushi.load_script(id)
    if not ok then log.error("Core script '" .. id .. "' failed: " .. tostring(err)) end
end

if i18n and i18n.sync_titles then
    i18n.sync_titles()
end

log.info("All modules loaded successfully")
