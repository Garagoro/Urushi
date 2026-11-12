# URUSHI — Architecture & Code Map

Reference for humans **and AI assistants**: read this first instead of grepping the whole tree.
Companion docs: [LUA_API.md](LUA_API.md) (everything callable from Lua), [SHADERS.md](SHADERS.md) (CarFX HLSL).

Target: **CarX Drift Racing Online 2** (Unity 6000.3.19f1, IL2CPP, DX11, x64 Windows). URUSHI is injected as a proxy
`version.dll` and runs *inside the game process*. Everything is native C++ plus an embedded Lua 5.4 VM.

---

## 1. Components and files

| Layer | Files | Role |
|---|---|---|
| Proxy / loader | `proxy/version_proxy.cpp` (+`.def`) | Forwards the real `version.dll` exports, loads `urushi.dll` from a **shadow copy** (so the file is never locked), hot-reinject on Ctrl+F11 / event `Local\UrushiReload_<pid>`, unload on Ctrl+F10 |
| Entry | `core/dllmain.cpp` | `InitThread`, `StartFeatures()` (phase B), `ScriptHook_Unload()` |
| Safe mode | `core/safe_mode.*` | Startup mode decision, crash prompt, Safe-mode window |
| Crash capture | `core/crash_handler.*` | SEH + `std::terminate` filter → `urushi_crash/` |
| Settings | `core/settings.*` | `urushi_settings.ini` (crash policy, per-script enabled flags) |
| Hooking | `core/hook_manager.*` | MinHook wrapper, in-flight counter for clean unload |
| Overlay | `core/d3d11_hook.*` | Hooks `IDXGISwapChain::Present/ResizeBuffers`, `DrawIndexed(Instanced)`; WndProc subclass; ImGui init & frame loop |
| UI | `core/urushi_ui.*` | Lacquered ImGui theme, dock/window manager, widgets (`BeginCard`, `Toggle`, `Segmented`, …) |
| Declarative UI | `core/ui_schema.*` | Lua tables → ImGui panels (`on_action` / `on_change`) |
| Lua | `core/lua_engine.*`, `core/lua_sandbox.*`, `core/bindings/lua_*.cpp` | VM lifecycle, callbacks, sandbox; one binding file per Lua module |
| Game access | `core/native_carx.*` | Hooks in `NativeCarX.dll` (car create/destroy, render matrices, tyres, telemetry) |
| Unity access | `core/unity_bridge.*` (4.7k lines) | Hooks `ExecutePlayerLoop` in `UnityPlayer.dll` (**hard-coded offsets**), transforms, raycasts, spawn/teleport, ghost cars, main-thread task queue |
| ECS / DOTS | `core/ecs_collision.*` | Selective ghosting (anti wrong-way), DOTS raycast/spherecast |
| Physics tweaks | `core/steer_assist.*` | Code patches for the 155° steering-flip fix |
| FPS optimizer | `core/car_sim_optimizer.*` | Hooks `DistanceBasedPhysicsSwitchingSubController::Update/IsFarCar`, `ApplyForceToDeformationSkeletonSystem::OnUpdate`, `BreakPhysicsJointSystem::OnUpdate`; damage/tyre-wear immunity |
| Shaders | `core/carfx.*`, `core/carfx_shader_decl.*`, `shaders/*.hlsl` | Per-car pixel-shader replacement/overlay, sensors (wall, tandem, drift angle) |
| Replays | `core/replay_engine.*` | `.cxr2` multi-car recording/playback, embeds CarFX config |
| Input | `core/input_manager.*` | Keyboard + XInput gamepad, rebindable bindings |
| Dev tooling | `core/dev_pipe.*`, `tools/urushi_dev_mcp.py` | Named pipe `\\.\pipe\urushi_dev`: backbuffer screenshots, UI inspect, Lua eval, reload |
| Updater | `core/updater.*` | Syncs `scripts/`, `shaders/`, `dist/*.dll` from GitHub `Garagoro/Urushi` branch `main` |
| Lua mods | `scripts/*.lua` | Everything user-visible (windows, HUD, tuning) — see §6 |

Third-party: `third_party/{imgui,minhook,lua}`. Release binaries live in `dist/` (tracked, used by the updater).

---

## 2. Lifecycle

```
game starts → loads proxy version.dll → loads urushi.dll (shadow copy) → DllMain spawns InitThread

InitThread (dllmain.cpp)
  1. Settings::Init, Logger::Init (previous log kept as urushi.prev.log), CrashHandler::Init
  2. wait for NativeCarX.dll (≤20 s)
  3. HookManager::Init (MinHook)
  4. D3D11Hook::Init            ← overlay is ALWAYS installed (hosts the menu + Safe mode UI)
  5. SafeMode::Decide()         → true  : StartFeatures() then SafeMode::MarkFull()
                                → false : stay featureless (Safe / waiting for the user's answer)

StartFeatures()  ("phase B" — everything that touches the game or runs scripts)
  NativeCarX → SteerAssist → UnityBridge → CarSimOptimizer → ReplayEngine
  → LuaEngine::Init (runs scripts/main.lua) → FileWatcher → DevPipe → Updater
```

**Startup mode** (`SafeMode::Decide`), in priority order:
1. `[stability] force_safe_next=1` → Safe (flag is consumed).
2. No crash marker from the previous run → Full.
3. Crash marker present → `[stability] crash_policy`: `safe` → Safe, `normal` → Full, `ask` (default) → `AwaitingChoice`
   (native ImGui prompt "Start in Safe mode?", Y/N, "remember my choice").

**Safe mode** = overlay + Safe window only. No hooks into the game, no Lua, no CarFX, optimizer, replays, updater, dev pipe,
`InputManager::Update`. Draw hooks stay installed but pass through (`SafeMode::IsFull()` check). Leaving Safe mode runs
`StartFeatures()` on a worker thread (`SafeMode::RequestFull`), CarFX initialises lazily on the next `Present`.

**Unload** (`ScriptHook_Unload`, Ctrl+F10 or reinject): restore CarFX/optimizer state → detach WndProc → `HookManager::BeginUnload`
+ wait in-flight hooks → shut modules down in reverse order. Phase-B modules are shut down only if `SafeMode::FeaturesStarted()`.

---

## 3. Threads — who runs what

| Thread | Runs | Notes |
|---|---|---|
| **InitThread** | startup sequence | short-lived |
| **Unity main thread** | `Hooked_ExecutePlayerLoop` → `ProcessPendingMainThreadTasks`, `LuaEngine::OnMainUpdate` (Lua `events.on_main_update`), ghost collision resolve | The only thread where Unity API calls are safe. Anything that touches Unity objects from elsewhere must be queued to it (`UnityBridge` pending-task machinery) |
| **Render thread** (whoever calls `Present`) | `Hooked_Present`: `InputManager::Update`, `CarFX::OnNewFrame`, ImGui frame, `LuaEngine::OnRender` (→ `on_update`, `on_fixed_update`, `on_render`), `UiSchema::RenderAll`, `Updater::RenderUI`, `SafeMode::RenderUI`; also **applies pending Lua reloads** | May differ from the Unity main thread |
| **Window thread** | `Hooked_WndProc`: key events → `InputManager`, `LuaEngine::OnKeyEvent` (`events.on_key`), menu toggle (F4/Insert) | Ignores auto-repeat; no script hotkeys while ImGui wants text input |
| **FileWatcher** | polls `scripts/*.lua` mtimes every 500 ms | Only *schedules* a reload (debounced 300 ms) |
| **DevPipe** | named pipe server | Commands are executed on the render thread in `BeforeNewFrame` |
| **Updater worker** | GitHub HTTP, download, stage files | |
| **SafeMode StartThread** | `StartFeatures()` when leaving Safe mode | |
| D3D draw hooks | `CarFX::TryHandleDraw` per draw call | Hot path — keep cheap; re-entrancy guarded by `t_inDrawHook` |

### Locks (acquire in this order)
`LuaEngine::Mutex()` (recursive) → `UiSchema::s_mutex` / `UrushiUI::s_mutex`. `UiSchema::RenderAll` takes the Lua lock **first**
because panels call into Lua. `InputManager` and `NativeCarX` have their own mutexes and never call Lua while holding them.

---

## 4. The Lua boundary (rules that keep it stable)

* **One Lua state**, created in `LuaEngine::Init`, closed in `LuaEngine::Shutdown`. All access must hold `LuaEngine::Mutex()`.
* **Never close/re-create the state from inside Lua or from another thread.** Use `LuaEngine::RequestReload()` (exposed as
  `urushi.reload_scripts()`); the render thread performs the reload at the start of `OnRender`. FileWatcher does the same.
* **Every C++→Lua call goes through `LuaSandbox::PCall(L, nargs, nres, label, budgetMs)`** (never raw `lua_pcall`):
  wall-clock budget (callbacks/UI 250 ms, script load 8 s, `main.lua` 60 s, eval 5 s) enforced by an instruction-count hook —
  an endless loop becomes the error "script timeout" even if the script wraps it in `pcall`; errors carry a traceback; `label`
  is stored for crash reports.
* Callbacks registered via `events.*` live in `LuaEngine` vectors as registry refs. A callback failing **25 times in a row is
  removed**; error logging is throttled to once per 2 s per callback.
* `events.on_fixed_update` is a true 60 Hz fixed step (max 4 catch-up steps per frame); `dt` is clamped to 0.25 s.
* Adding a binding: write `static int Lua_X(lua_State*)` in `core/bindings/lua_<module>.cpp`, add to that file's `luaL_Reg`
  table, document it in `docs/LUA_API.md`. A new module needs `Register(L)` called from `LuaEngine::Init` and a source entry in
  `core/CMakeLists.txt`.

### Lua globals (registered in `LuaEngine::Init`)
`carx` (cars, telemetry, tyres, physics, raycast, camera — `bindings/lua_carx.cpp`), `steer`, `carfx`, `replay`, `memory`,
`updater`, `optimizer`, `urushi` + `ui` + `input` (`bindings/lua_urushi.cpp`), `imgui` (`lua_imgui.cpp`), `events`, `log`.
Stability-related `urushi.*`: `get_setting/set_setting(section,key,value)`, `list_scripts()`, `load_script(name)` (sandboxed),
`reload_scripts()`, `get_last_crash()`, `open_crash_dir()`, `is_safe_mode()`.

---

## 5. Scripts: loading and the script list

`scripts/main.lua` is the only entry point:
1. `dofile` core modules **always**: `i18n.lua`, `keybinds.lua`.
2. Builds the global `urushi_scripts = { list = {...}, dirty = false }`.
3. For each **built-in** module (`telemetry_hud, ghost_test, teleport_mod, replay_mod, carfx_mod, steer_assist_mod, tires_mod,
   perf_optimizer`) → `urushi.load_script(id)` if enabled (default **on**), errors captured in `entry.status/err`.
4. Any other `scripts/*.lua` (not reserved) is a **user script**: listed, default **off**.
5. Loads `system_mod.lua` (System window: language/theme/scale, updater, **Stability**, **Scripts**, keybinds) and `runner.lua`.

Enabled flags: `urushi_settings.ini` → `[scripts]` `<id>=0|1`. Toggling in *System → Scripts* sets `urushi_scripts.dirty`; the
"Reload scripts" button applies it.

A script registers UI with `urushi.register_window{ id, title, render=function() ... end }` (imperative `ui.*` widgets) or returns a
schema table (declarative, `ui_schema`). It uses `T(ru, en)` from `i18n.lua` for bilingual text. Per-script file ↔ feature:

| Script | Feature | Main C++ counterpart |
|---|---|---|
| `telemetry_hud.lua` | speed/RPM/gear HUD | `native_carx` |
| `ghost_test.lua` | ghost/dummy car, anti-wrong-way | `unity_bridge`, `ecs_collision` |
| `teleport_mod.lua` | per-track checkpoints, teleport | `unity_bridge` (TeleportPose) |
| `replay_mod.lua` | record/playback `.cxr2` | `replay_engine` |
| `carfx_mod.lua` | shader picker/params | `carfx` |
| `steer_assist_mod.lua` | 155° steering fix UI | `steer_assist` |
| `tires_mod.lua` | tyre wear/temp/pressure, damage toggle | `native_carx` TyreConfig, `car_sim_optimizer` |
| `perf_optimizer.lua` | multiplayer physics LOD | `car_sim_optimizer` |
| `keybinds.lua` | bindings (persist `scripts/keybinds.json`) | `input_manager` |
| `system_mod.lua` | settings, updater, stability, script list | `urushi_ui`, `updater`, `settings` |
| `runner.lua` | dev IPC helper for tests | `dev_pipe` |

---

## 6. Stability subsystem

* **Crash handler** (`crash_handler.cpp`): `SetUnhandledExceptionFilter` (re-asserted every ~2 s in `Hooked_Present` because Unity may
  replace it; chains to the previous filter) + `std::set_terminate`. On crash writes, in `<gameDir>\urushi_crash\`:
  `crash_<stamp>.txt` (exception, fault module+offset, registers, x64-unwound stack, *Lua context* = last sandbox entry point,
  "crash in URUSHI code: YES/no"), `.dmp` (MiniDump, dbghelp loaded dynamically), `.log` (copy of `urushi.log`), and
  `last_crash.marker` (read & deleted by the next launch → drives Safe mode). Marker is written **before** the dump.
* **Settings file**: `<gameDir>\urushi_settings.ini` — `[stability] crash_policy=ask|safe|normal`, `force_safe_next=0|1`,
  `[scripts] <id>=0|1`. API: `Settings::GetString/SetString/GetBool/SetBool`.
* Logs: `urushi.log` (current), `urushi.prev.log` (previous session), crash copies in `urushi_crash/`.

---

## 7. How-to recipes

**Add a Lua-visible feature**: C++ logic in the owning module → binding in `core/bindings/lua_*.cpp` → doc in `LUA_API.md` → UI in a
`scripts/*.lua` window. Touching Unity objects? Do it on the main thread via UnityBridge's main-thread task queue.

**Add a script/mod**: create `scripts/<id>.lua`, add `{id, ru, en}` to `BUILTIN` in `main.lua` (or just drop the file for a user script).

**Add a native hook**: `HookManager::CreateHook(target, detour, &orig, "name")`; wrap detours with `HookManager::ScopedHookGuard` and
return early when `HookManager::IsUnloading()`; resolve offsets/exports defensively (game updates break hard-coded offsets).

**Build** (VS 2022 BuildTools; MSBuild is not on PATH):
```powershell
$cm = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cm -S . -B <out-dir> -G "Visual Studio 17 2022" -A x64
& $cm --build <out-dir> --config Release --target urushi --parallel
```
Do **not** reuse the repo's `build/` folder: its CMake cache points at an old copy of the sources. New `.cpp` files must be listed in
`core/CMakeLists.txt`. `deploy` target copies DLLs/scripts/shaders into the game folder.

**Debug a live game**: `tools/urushi_dev_mcp.py` (pipe `\\.\pipe\urushi_dev`: screenshots, UI layout, `lua_eval`, `reload_lua`,
hot-swap DLL). Crash analysis: `urushi_crash/*.txt` first, then `.dmp` (`tools/analyze_dump.py`, `resolve_symbols.py`).

---

## 8. Fragile points / gotchas

* `unity_bridge.cpp` and parts of `car_sim_optimizer.cpp` / `steer_assist.cpp` use **hard-coded Unity/NativeCarX offsets and byte
  patterns** for Unity 6000.3.19f1 — a game update breaks them; bytes are verified before patching (`Stats.bytesVerified`).
* Unity objects are not thread-safe: the render thread must not call into Unity (see §3). The garage-scene transition freed
  GraphicsBuffers while CarFX still referenced them (fixed in `c1b419e`) — clear texture/buffer references on scene changes.
* `Updater` overwrites everything under `scripts/` and `shaders/` from `main` (except `scripts/runner_state.json`); it never
  deletes extra local files. Do not keep personal data in `scripts/` files that exist in the repo.
* Don't call `lua_pcall` directly, don't hold the Lua lock while waiting on other threads, don't reload Lua synchronously.
* Language/accent are not persisted; only the settings listed in §6 are.
