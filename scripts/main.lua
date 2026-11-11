-- CarX2ScriptHook: Main Entry Point
log.info("CarX2ScriptHook main.lua initialized")

-- Load modules
dofile("scripts/telemetry_hud.lua")
dofile("scripts/ghost_test.lua")
dofile("scripts/teleport_mod.lua")
dofile("scripts/replay_mod.lua")
dofile("scripts/carfx_mod.lua")
dofile("scripts/steer_assist_mod.lua")
dofile("scripts/perf_optimizer.lua")
dofile("scripts/system_mod.lua")
dofile("scripts/runner.lua")

log.info("All modules loaded successfully")
