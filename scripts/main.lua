-- URUSHI: Main Entry Point
log.info("URUSHI main.lua initialized")

dofile("scripts/i18n.lua")
dofile("scripts/keybinds.lua")
dofile("scripts/telemetry_hud.lua")
dofile("scripts/ghost_test.lua")
dofile("scripts/teleport_mod.lua")
dofile("scripts/replay_mod.lua")
dofile("scripts/carfx_mod.lua")
dofile("scripts/steer_assist_mod.lua")
dofile("scripts/tires_mod.lua")
dofile("scripts/perf_optimizer.lua")
dofile("scripts/system_mod.lua")
dofile("scripts/runner.lua")

if i18n and i18n.sync_titles then
    i18n.sync_titles()
end

log.info("All modules loaded successfully")
