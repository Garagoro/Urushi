# URUSHI Lua API Documentation

URUSHI embeds a **Lua 5.4** runtime inside **CarX Drift Racing Online 2**. It exposes game telemetry, physics controls, custom UI widgets, gamepad/keyboard polling, and CarFX shader parameters directly to Lua scripts.

All scripts live inside the `scripts/` directory. The main entry point is [`scripts/main.lua`](../scripts/main.lua), which loads individual modules via `dofile()`.

> **Hot-Reload:** You can reload all Lua scripts live in-game by pressing **`Ctrl + F11`** or clicking **Reload Lua** in the URUSHI **System** window.

---

## Table of Contents
1. [Core Architecture & Module Loading](#1-core-architecture--module-loading)
2. [`carx` Module — Physics & Telemetry](#2-carx-module--physics--telemetry)
3. [`urushi` Module — Window & Environment](#3-urushi-module--window--environment)
4. [`ui` Module — Component Library](#4-ui-module--component-library)
5. [`input` Module — Keys & Gamepad](#5-input-module--keys--gamepad)
6. [`carfx` Module — Shader Control](#6-carfx-module--shader-control)
7. [`optimizer` Module — Multiplayer Performance](#7-optimizer-module--multiplayer-performance)
8. [`steer_assist` Module — Steering Fixes](#8-steer_assist-module--steering-fixes)
9. [`replay` Module — 3D Replay & Ghost](#9-replay-module--3d-replay--ghost)
10. [Example: Creating a Custom Mod](#10-example-creating-a-custom-mod)

---

## 1. Core Architecture & Module Loading

To add your own script:
1. Create a new `.lua` file inside `scripts/`, e.g. `scripts/my_mod.lua`.
2. Add `dofile("scripts/my_mod.lua")` to `scripts/main.lua`.
3. Press `Ctrl + F11` in-game to load your changes instantly without restarting the game.

Logging helpers:
```lua
log.info("Message")
log.warn("Warning")
log.error("Error")
```

---

## 2. `carx` Module — Physics & Telemetry

The `carx` global table exposes real-time vehicle dynamics and game states.

### Player & Vehicle Data
* `carx.get_player_car()`: Returns a table representing the local player car, or `nil` if not on track.
  ```lua
  local car = carx.get_player_car()
  if car then
      print(car.addr)         -- integer: NativeCarX::Car pointer address
      print(car.speed_kmh)    -- float: Speed in km/h
      print(car.speed_mph)    -- float: Speed in mph
      print(car.rpm)          -- float: Current engine RPM
      print(car.gear)         -- int: Current gear (0 = Reverse, >0 = Gear)
      print(car.steer_angle)  -- float: Wheel steer angle (radians)
      print(car.accel)        -- float: Throttle input [0..1]
      print(car.brake)        -- float: Brake input [0..1]
      print(car.handbrake)    -- float: Handbrake input [0..1]
      print(car.clutch)       -- float: Clutch input [0..1]
      print(car.x, car.y, car.z) -- float: World position
      print(car.velocity.x, car.velocity.y, car.velocity.z) -- World velocity

      -- Wheel telemetry (1: Front-Left, 2: Front-Right, 3: Rear-Left, 4: Rear-Right)
      for i, wheel in ipairs(car.wheels) do
          print(wheel.wear)          -- float: Wear percentage [0..100]
          print(wheel.life_percent)  -- float: Remaining life [0..100]
          print(wheel.camber)        -- float: Wheel camber
          print(wheel.smoke)         -- float: Smoke level
      end
  end
  ```
* `carx.get_all_cars()`: Returns an array of telemetry tables for all active cars in the session (player + multiplayer).
* `carx.get_player_list()`: Returns an array of connected multiplayer racers with names and pointers.
* `carx.is_in_session()`: Returns `boolean` whether the player is currently on a track in a driving session.

### Physics, Teleport & Movement
* `carx.teleport(x, y, z [, yaw, pitch, roll])`: Instantly teleports the local player's car to the given coordinates.
* `carx.get_velocity()`: Returns `{x, y, z}` velocity vector in m/s.
* `carx.set_velocity(x, y, z)`: Overrides linear velocity vector.
* `carx.get_angular_velocity()`: Returns `{x, y, z}` angular velocity.
* `carx.set_angular_velocity(x, y, z)`: Overrides angular velocity.
* `carx.reset_car()`: Restores the car to the nearest spawn/reset point.

### Damage & Tyres
* `carx.set_no_damage(enabled)`: Enables or disables body/mechanical collision damage immunity.
* `carx.get_no_damage()`: Returns `boolean`.
* `carx.set_no_tire_wear(enabled)`: Prevents tire degradation over time.
* `carx.get_no_tire_wear()`: Returns `boolean`.
* `carx.repair_car()`: Restores vehicle body damage, deforms, and mechanical health.
* `carx.reset_tire_wear()`: Resets all 4 tires to 100% life immediately.
* `carx.get_tyre_config()`: Returns tyre pressures, friction multipliers, and rim dimensions.
* `carx.set_tyre_config(table)`: Applies custom tyre pressures (kPa) and settings.
* `carx.get_tyre_live()`: Returns live tire temperature, friction coefficient, and ballistic grip.

### Drift & Proximity Sensors
* `carx.get_drift_info()`: Returns `{ angle_deg, signed_angle_deg, speed_kmh, score }`.
* `carx.get_proximity()`: Returns proximity distances (in meters) to surrounding walls, obstacles, and nearest tandem cars.
* `carx.raycast(origin, direction, max_dist)`: Performs a physics raycast against track geometry.
* `carx.get_camera()`: Returns camera position and forward vectors.

---

## 3. `urushi` Module — Window & Environment

Handles window registration, styling, and framework settings.

### Window Registration
```lua
urushi.register_window({
    id = "my_custom_window",      -- Unique identifier
    title = "My Custom Mod",      -- Window title
    icon = "⚡",                  -- Sidebar icon
    order = 50,                   -- Order in sidebar
    default_open = false,         -- Open by default
    default_pinned = false,       -- Pinned to overlay HUD
    hud_no_inputs = false,        -- Transparent to inputs when pinned
    width = 320,                  -- Default width
    render = function()
        ui.text("Hello from my mod!")
    end
})
```

### Window & UI State
* `urushi.toggle_window(id)`: Toggles visibility of a registered window.
* `urushi.set_window_open(id, boolean)`: Sets window open/closed state.
* `urushi.is_window_open(id)`: Returns `boolean`.
* `urushi.toggle_menu()`: Opens or closes the main URUSHI dock.
* `urushi.is_menu_open()`: Returns `boolean`.
* `urushi.set_accent(preset_name)`: Changes accent theme (`"Amber"`, `"Crimson"`, `"Silver"`, `"Indigo"`).
* `urushi.get_accent()`: Returns current accent name.
* `urushi.set_custom_accent(r, g, b)`: Sets custom RGB accent color.
* `urushi.set_language("en" | "ru")`: Switches UI language.
* `urushi.set_ui_scale(scale)`: Sets interface scaling factor.

---

> **Security (see [SECURITY.md](SECURITY.md)):** the Lua VM has no `os.execute`, `io.popen`, `package`/`require`, `dofile`/`loadfile`, `debug` (only `debug.traceback`),
> `string.dump` or bytecode loading; `os` is `time/clock/date/difftime`; `io` is `open/type` and `io.open` only reaches `urushi_data/` (`.json .txt .cfg .csv`).
> User and cloud scripts additionally run in an isolated environment: `urushi.set_setting/get_setting/list_scripts/load_script/reload_scripts/open_crash_dir`,
> `updater` and `cloud` are not available to them, and `io.open("x.json")` maps to `urushi_data/<script>/x.json`.
> Native objects (cars, GameObjects, Transforms) are opaque handles; only handles returned by the API are accepted.

### Config (`config`, `scripts/config.lua`)

One JSON file, `urushi_data/config.json`, with autosave (~2 s after the last change). Intentionally minimal: presets, profiles, per-car configs or
import/export are meant to be built on top of this module (or by replacing the file).

| Function | Description |
| :--- | :--- |
| `config.get(section, key, default)` | Read a value; `default` if missing. |
| `config.set(section, key, value)` | Write a value (`nil` removes it). Values are JSON types: number, string, boolean, array/object tables. |
| `config.section(name)` | Copy of all keys of a section. |
| `config.save()` / `config.reload()` | Write now / re-read from disk. |
| `config.for_script(id)` | `{ get(key, default), set(key, value), save() }` bound to section `"script:<id>"`. |

Isolated and cloud scripts get `config` already bound to their own section: `config.get("volume", 0.5)`, `config.set("volume", 0.8)`.
File format: `{ "version": 1, "<section>": { "<key>": value } }`; the last good file is kept as `config.bak.json` and used if `config.json` is corrupt.
Currently persisted by the core UI: language, accent (+custom colour), UI scale. Pending changes are flushed by *Reload scripts*; a file-watcher reload can lose the last ~2 s.

### Settings, Scripts & Stability (`urushi.*`)

| Function | Description |
| :--- | :--- |
| `urushi.get_setting(section, key, default)` | Read a string from `urushi_settings.ini`. |
| `urushi.set_setting(section, key, value)` | Write a string (booleans are stored as `"1"`/`"0"`). |
| `urushi.list_scripts()` | Names (without `.lua`) of all files in `scripts/`. |
| `urushi.load_script(name)` | Load `scripts/<name>.lua` inside the sandbox. Returns `true` or `false, error`. |
| `urushi.reload_scripts()` | Request a full Lua reload (applied on the next render frame; safe to call from callbacks). |
| `urushi.get_last_crash()` | `{ crashed, in_urushi, summary, report }` describing the previous session. |
| `urushi.open_crash_dir()` | Open the `urushi_crash` folder in Explorer. |
| `urushi.is_safe_mode()` | `true` while features are off (scripts normally never see this: Lua does not run in Safe mode). |

Every callback is executed under a wall-clock budget (250 ms; script load 8 s). Endless loops raise `script timeout`; a callback that errors 25 times in a row is unregistered. See [ARCHITECTURE.md](ARCHITECTURE.md).

## 4. `ui` Module — Component Library

Provides modern, cohesive ImGui components with automatic layout and accent coloring.

* `ui.card(title, render_fn)`: Wraps content into a modern rounded card panel.
* `ui.toggle(label, value)`: Switch toggle; returns updated `boolean`.
* `ui.slider_float(label, value, min, max [, format])`: Float slider; returns updated `float`.
* `ui.slider_int(label, value, min, max)`: Integer slider; returns updated `int`.
* `ui.segmented(label, options, selected_idx)`: Segmented radio-style switcher; returns selected index.
* `ui.button(label [, style])`: Button. Style can be `"default"`, `"primary"`, `"danger"`, or `"ghost"`. Returns `true` on click.
* `ui.stat_row(items)`: Multi-column stat display:
  ```lua
  ui.stat_row({
      { label = "SPEED", value = "120 KM/H", tone = "accent" },
      { label = "GEAR",  value = "3",        tone = "default" },
      { label = "TEMP",  value = "85°C",     tone = "red" }
  })
  ```
* `ui.progress_bar(fraction, width, height)`: Renders a themed progress bar.
* `ui.badge(text, tone)`: Renders a colored status badge (`"accent"`, `"green"`, `"red"`, `"yellow"`, `"muted"`).
* `ui.keycap(key_text)`: Renders a keyboard key indicator.
* `ui.color_edit3(label, rgb_table)` / `ui.color_edit4(label, rgba_table)`: Color pickers.
* `ui.spacing(px)`: Vertical spacing.
* `ui.separator()`: Subtle divider line.
* `ui.text(str)` / `ui.text_dim(str)` / `ui.text_colored(str, r, g, b, a)`: Formatted text rendering.

---

## 5. `input` Module — Keys & Gamepad

* `input.is_key_down(vk_code)`: Returns `boolean` while keyboard key is held down.
* `input.is_key_pressed(vk_code)`: Returns `boolean` on the initial key press frame.
* `input.is_gamepad_connected(slot)`: Returns `boolean` for XInput controller (0..3).
* `input.get_gamepad_state(slot)`: Returns table with sticks, triggers, and pressed buttons.
* `input.is_bound_pressed(binding_table)`: Checks if a configured key or gamepad binding was triggered.
* `input.get_key_name(vk_code)`: Returns human-readable key name (e.g. `"F4"`, `"Insert"`).

---

## 6. `carfx` Module — Shader Control

* `carfx.get_shader_list()`: Returns names of all compiled `.hlsl` shaders in `shaders/`.
* `carfx.set_active_shader(name)`: Activates a shader on the player's car.
* `carfx.get_active_shader()`: Returns current shader name.
* `carfx.set_param(param_index, x, y, z, w)`: Overrides `p0..p7` parameters directly from Lua.
* `carfx.reload_shaders()`: Hot-reloads all shaders from disk.
* `carfx.load(file, target)` / `carfx.unload(target)`: `target` 0 = body (always one piece: bumpers, doors... are not split), 1 = internals (see below).
* `carfx.get_parts()` -> `{ engine, suspension, brakes, wheels, interior }` booleans: which groups of non-body parts the **internals** target (1) applies to.
  Defaults: everything on except `wheels`. `carfx.set_parts{ wheels = true }` changes only the listed groups and returns the new table.
  `interior` means every other non-body, non-glass part (seats, dashboard, cage, driver...).

---

## 7. `optimizer` Module — Multiplayer Performance

Controls the physics simulation scaling for high-population multiplayer lobbies.

* `optimizer.set_enabled(boolean)`: Enables or disables the distant car physics optimizer.
* `optimizer.set_max_full_sim_cars(count)`: Maximum count of nearest cars kept at full 60Hz physics (default: 4..8).
* `optimizer.get_stats()`: Returns current active cars, bone-sleep count, and frame savings.

---

## 8. `steer_assist` Module — Steering Fixes (Pure Lua: `scripts/steer_assist_mod.lua`)

* `steer_assist.set_enabled(boolean)`: Toggles the `155°` backwards entry threshold fix (uses `memory.alloc_near` + `memory.patch_bytes` trampolines).
* `steer_assist.set_threshold(degrees)`: Adjusts vector-flip threshold (default: `155.0°`).
* `steer_assist.get_stats()` / `steer_assist.get_telemetry()`: Returns patch status (`bytes_verified`, `near_alloc_ok`, `patches_applied`), live front slip angles, assist multipliers, and lock status.

---

## 9. `replay` Module — 3D Replay & Ghost

* `replay.start_recording(filename)`: Starts recording car transform stream to `.cxr2` file.
* `replay.stop_recording()`: Stops and saves recording.
* `replay.start_playback(filename)`: Spawns ghost car playing back the recorded run.
* `replay.stop_playback()`: Removes replay ghost.
* `replay.set_mirror_mode(boolean)`: Mirrors the ghost laterally for tandem chase practice.
* `replay.set_ghost_collision(boolean)`: Toggles physical collision against ghost cars.

---

## 10. `il2cpp` & `unity` — Dynamic Reflection & AI Inspection

URUSHI provides full reflection over Unity GameObjects and IL2CPP C# components at runtime, without needing C++ core modifications.

### Unity Scene Primitives (`unity` Module)
* `unity.find(name)`: Finds a native `GameObject` pointer by name in the active scene.
* `unity.get_all_objects([max])`: Returns array of all active GameObjects `{ ptr, name, transform, x, y, z }`.
* `unity.get_components(go)`: Returns all components attached to a GameObject:
  ```lua
  local comps = unity.get_components(go)
  -- returns: { { cid = 66, name = "CarBehaviour", namespace = "Framework.Views.Cars", ptr = <managed>, class_addr = 0x... }, ... }
  ```
* `unity.get_position(transform)` / `unity.set_position(transform, x, y, z)`: Fast position access.
* `unity.get_rotation(transform)` / `unity.set_rotation(transform, x, y, z, w)`: Quaternion rotation access.

### IL2CPP Component Inspection (`il2cpp` Library)
* `il2cpp.get_component(go, name)`: Finds a component by name or partial name on a GameObject.
* `il2cpp.inspect(component_or_ptr)`: Inspects class metadata and returns:
  * `info.name`, `info.namespace`, `info.full_name`
  * `info.field_count`, `info.method_count`
  * `info.fields`: Array of `{ name, offset, is_static, val_float, val_int, val_ptr }`
  * `info.methods`: Array of `{ name, fn_ptr, params_count }`
* `il2cpp.get_field(comp, field_name)`: Returns live float, int, and pointer values for a named field.
* `il2cpp.set_field_float(comp, field_name, value)`: Modifies a float field directly in memory.
* `il2cpp.set_field_int(comp, field_name, value)`: Modifies an integer field directly in memory.
* `il2cpp.dump_class(comp)`: Generates Markdown documentation of the component.
* `il2cpp.dump_object_for_ai(go)`: Formats a complete structured prompt for an AI assistant (ChatGPT, Claude) detailing all components, fields, and methods.
* `il2cpp.export_ai_dump(go, filename)`: Writes the AI dump to `urushi_data/<filename>` and `urushi.log`.

### Memory Primitives (`memory` Module)
* `memory.read_u8 / u16 / u32 / i32 / u64 / ptr(addr)`
* `memory.read_float(addr)`
* `memory.read_bytes(addr, count)`
* `memory.read_string(addr, [maxLen])`
* `memory.write_u8 / u16 / u32 / i32 / u64 / ptr(addr, val)`
* `memory.write_float(addr, val)`
* `memory.write_bytes(addr, bytes_or_hex_string)`
* `memory.patch_bytes(addr, bytes_or_hex_string)`: Safe patch with rollback tracking
* `memory.restore_patch(addr)`: Rolls back patch at address
* `memory.restore_all_patches()`: Rolls back all registered patches and releases near-allocated trampolines
* `memory.alloc_near(module_name, [size])`: *(trusted built-in scripts only)* Allocates RWX executable memory within ±2 GB of a module for RIP-relative trampolines
* `memory.free_alloc(addr)`: *(trusted built-in scripts only)* Releases a trampoline allocated by `memory.alloc_near`
* `memory.scan(pattern, [module_name])`: Fast cached AOB signature scanner
* `memory.resolve_rip(instr_addr, op_len, rip_offset)`: Resolves x64 RIP-relative pointer
* `memory.call(fn_ptr, [arg1...arg8])`: Fastcall invocation with SEH protection (returns integer, ok)
* `memory.call_float(fn_ptr, [arg1...arg8])`: Fastcall invocation with float return
* `memory.call_main(fn_ptr, [arg1...arg8])`: Fastcall invocation on Unity main thread

---

## 11. Example: Creating a Custom Mod

Save as `scripts/drift_sound_mod.lua`:

```lua
local is_active = false
local angle_alert = 70.0

urushi.register_window({
    id = "drift_alert",
    title = "Drift Angle Alert",
    icon = "🏎️",
    order = 100,
    render = function()
        ui.card("Settings", function()
            is_active = ui.toggle("Enable Alert", is_active)
            angle_alert = ui.slider_float("Target Angle", angle_alert, 45.0, 120.0, "%.0f°")
        end)

        local car = carx.get_player_car()
        if car and is_active then
            local drift = carx.get_drift_info()
            local cur_angle = drift and drift.angle_deg or 0.0

            ui.card("Live Angle", function()
                ui.stat_row({
                    { label = "CURRENT", value = string.format("%.1f°", cur_angle), tone = "accent" },
                    { label = "TARGET",  value = string.format("%.0f°", angle_alert), tone = "default" }
                })

                if cur_angle >= angle_alert then
                    ui.spacing(4)
                    ui.badge("DEEP ANGLE ACHIEVED!", "green")
                end
            end)
        end
    end
})
```

Drop the file into `scripts/` (e.g. `scripts/drift_sound_mod.lua`). It shows up in *the *Library* window*, disabled by default; enable it and press "Reload scripts" (or `Ctrl + F11`). Built-in modules are listed in `BUILTIN` inside `scripts/main.lua`.

---

## 8. Embedded Reverse-Engineering & Disassembly (`disasm`)

URUSHI embeds the high-performance **Zydis x86_64 Disassembler**, allowing modders and AI agents to disassemble native functions, generate IDA signatures, resolve RIP targets, and patch instructions without external tools.

### `disasm.decode(address, [count = 10])`
Disassembles up to `count` instructions starting from `address`.
- **Arguments:**
  - `address`: Virtual address (`number`, `string` e.g. `"0x7FF8466C1000"`, or pointer)
  - `count`: Maximum number of instructions (default `10`, max `256`)
- **Returns:** Table of instruction records:
  ```lua
  {
      {
          address = 140704322173952,
          address_hex = "0x7FF8466C1000",
          rva = 4096,
          rva_hex = "0x1000",
          length = 7,
          bytes = "48 8D 0D C9 2B B8 00",
          mnemonic = "lea",
          text = "lea rcx, [0x00007FF847243BD0]",
          is_relative = true,
          target = 140704327957456,
          target_hex = "0x7FF847243BD0",
      },
      ...
  }
  ```

### `disasm.make_signature(address, [module = "GameAssembly.dll"], [min_len = 14], [max_len = 64])`
Automatically scans instructions at `address`, masks out relative jumps, calls, and RIP displacements with `?` wildcards, and ensures the signature is unique in the module.
- **Returns:** `pattern_string, match_count, byte_count`
  - If `match_count == 1`, the pattern is 100% unique!
  ```lua
  local sig, matches, len = disasm.make_signature(method_addr, "GameAssembly.dll")
  print("IDA Pattern:", sig) -- "48 8D 0D ? ? ? ? E9 ? ? ? ? CC CC"
  ```

### `disasm.nop(address, [instruction_count = 1])`
Safely overwrites `instruction_count` instructions with `0x90` (`NOP`) using `VirtualProtect` and flushes the instruction cache.
- **Returns:** `ok, total_bytes_nopped`

### `disasm.resolve_rip(address)`
Decodes 1 instruction and calculates its absolute RIP-relative target.
- **Returns:** `target_address, target_hex_string`

---

## 9. Memory Pattern Scanner (`pattern`)

Scans all executable PE sections (`.text`, `il2cpp`, etc.) across game modules.

### `pattern.find(module_name, ida_pattern, [name])`
- Scans `module_name` (default: `"GameAssembly.dll"`) for an IDA-style hex pattern (e.g. `"48 89 5C 24 ? 48 83 EC"`).
- Returns virtual address `va, hex_string` (or `nil` if not found).

### `pattern.find_rip(module_name, ida_pattern, instr_len, rip_offset, [instr_offset = 0])`
- Finds pattern and resolves the 32-bit RIP-relative displacement in one call.

### `pattern.find_entry(module_name, body_pattern, [max_backtrack = 256])`
- Finds unique signature inside a function body and automatically walks backwards to function prologue (`0xCC` / `0xC3`).

### `pattern.count(module_name, ida_pattern)`
- Returns number of occurrences in the module.

### `pattern.get_module(module_name)`
- Returns `{ base, base_hex, size, path }`.

### `pattern.to_rva(module_name, address)` / `pattern.to_va(module_name, rva)`
- Converts between virtual addresses and module RVAs.

---

## 10. In-Memory HLSL Shader Compilation (`carfx.compile`)

Compile custom DirectX 11 Pixel Shaders directly from Lua strings at runtime without writing files to disk.

### `carfx.compile(hlsl_source, [target = 0], [virtual_name = "dynamic.hlsl"])`
- **Arguments:**
  - `hlsl_source`: Full HLSL shader code (with `@mode`, `@param`, `@stealth` headers).
  - `target`: `0` for Car Body, `1` for Internals, `2..9` for Ghosts.
- **Returns:** `ok, error_message`
- **Example:**
  ```lua
  local hlsl = [[
  // @name GoldenGlow
  // @mode replace
  float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
      return float4(1.0, 0.8, 0.2, 1.0);
  }
  ]]
  local ok, err = carfx.compile(hlsl, 0)
  if ok then carfx.set_enabled(true) end
  ```

---

## 11. Memory Patching & Unity IL2CPP Reflection

### `memory.write_bytes(address, hex_string_or_table)`
Writes raw bytes to memory with automatic `VirtualProtect` (PAGE_EXECUTE_READWRITE) and instruction cache flush.
```lua
memory.write_bytes(addr, "90 90 90 90 90")
memory.write_bytes(addr, { 0x90, 0x90, 0x90 })
```

### `il2cpp` Reflection & AI Dump
- `il2cpp.get_components(gameObject)`
- `il2cpp.inspect(component)` -> fields with offsets and types, methods with native function pointers
- `il2cpp.get_field(comp, "fieldName")` / `set_field_float(comp, "fieldName", val)`
- `il2cpp.get_method(comp, "methodName")` -> `{ name, fn_ptr, params_count }`
- `il2cpp.call(comp, "methodName", ...)` -> invokes IL2CPP method with SEH protection
- `il2cpp.repair_car()` -> requests full body mesh & detached parts repair on main thread
- `il2cpp.disasm_method(comp, "methodName", count)` -> disassembled instructions via Zydis
- `il2cpp.make_method_signature(comp, "methodName")` -> auto-generated unique IDA signature
- `il2cpp.export_ai_dump(gameObject, filename)` -> complete class & field dump formatted for AI models

