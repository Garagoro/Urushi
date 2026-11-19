# URUSHI — Script security model

URUSHI runs inside the game process, so a Lua script is code running next to the user's session. Scripts can arrive from
anywhere (a Discord zip, a fork, the cloud library). The Lua layer is therefore built so that **a script cannot run programs,
cannot download anything, cannot create runnable files, and cannot reach other parts of the machine**.

**Out of scope by design:** native code. Anyone who edits the C++ core ships a fork; forks are not supported or reviewed.

## What a Lua script can NOT do

| Attack | Defence (file) |
|---|---|
| Run programs / shell (`os.execute`, `io.popen`) | Removed. `os` = `time clock date difftime` only (`core/lua_hardening.cpp`) |
| Load native code (`package.loadlib`, `require` of DLLs) | `package`/`require` are not opened at all |
| Run pre-compiled / malicious bytecode | `load` is text-only, `loadfile`/`dofile` removed for scripts, `string.dump` removed; the C++ loaders use text mode (`LuaSandbox::LoadFileText`) |
| Download files, open sockets | No network API exists in Lua. `updater.*` and `cloud.*` are invisible to isolated scripts |
| Create files that can be run or loaded as code | `io.open` only reaches `urushi_data/` (write) and `scripts/` (read), only `.json .txt .cfg .csv`. Names are validated (no `..`, no `:` / alternate data streams, no UNC / `\\?\`, no device names, no hidden files, no reparse points), data folder capped at 64 MB |
| Overwrite scripts / DLLs / settings | `scripts/` is read-only for scripts; `urushi_settings.ini` is outside `urushi_data/` |
| Tamper with the sandbox (`debug.sethook`, `debug.getregistry`, `setupvalue`) | `debug` = `traceback` only |
| Freeze the game with an endless loop | Wall-clock budget per call, enforced even through `pcall` (`core/lua_sandbox.cpp`) |
| Aim native calls at forged pointers (memory write primitive) | Cars/Unity objects are light userdata that C++ handed out; every native function validates them against `PtrRegistry` (`core/ptr_registry.h`). `memory.read_ptr` returns a plain integer |
| Write replay files outside `replays/` or with a runnable extension | Replay names are reduced to `<plain name>.cxr2` (`SanitizeReplayFileName`) |
| Read / compile arbitrary files through CarFX | `carfx.load` / `carfx.set_tex` only accept files/directories inside `shaders/` (both static images and animated GIF / sequence folders are strictly verified via `ResolvePathW`); HLSL `#include` is confined to `shaders/` too |
| Read / compile arbitrary files through PostFX | `postfx.load` only accepts files inside `shaders/post/` (`.hlsl` only); path validation rejects `..`, absolute, or UNC paths |
| Read arbitrary files / path traversal in audio | `audio.play` strictly confined to `urushi_data/audio/` (and isolated scripts confined to `urushi_data/<script>/` or `urushi_data/audio/`). Validated against `..`, `:`, UNC, device names, and extension whitelist (`.wav .mp3 .ogg .flac .aac .m4a .wma`). Decoding runs off the render thread on a worker thread. Max 8 voices |
| Eavesdrop on sound / record microphone | `audio_in` only exposes smoothed frequency band levels (0..1) and 32-band spectrum energies — never raw PCM audio samples or waveforms (`core/bindings/lua_audio_in.cpp`) |
| Hijack other scripts | Isolated scripts run in their own environment with private copies of the API tables; the string metatable is hidden; `load` is bound to the private environment |

### Trust tiers (`scripts/main.lua`)
* **Trusted** — core + official modules defined in `scripts/manifest.json`: shared globals, but still the hardened VM. When downloaded in-game, official modules are verified against their SHA-256 hashes in `scripts/manifest.json`.
* **Isolated** — everything else (user scripts in `scripts/`, cloud scripts): private environment, no `updater`/`cloud`/settings
  functions, `io.open` redirected to `urushi_data/<script>/`. New user scripts are **off** until enabled in the *Library* window.
* **Unified start/stop execution**: Starting or stopping any script from the UI (`system_mod.lua`) only modifies the `[scripts]` configuration flag and requests `urushi.reload_scripts()`. The loading logic is centralized in `main.lua`'s `load_entry`, ensuring sandbox isolation and security tiers are always strictly enforced without bypass.

### Other switches
* `runner.lua` (loads code from a file) and the **dev pipe** (`\\.\pipe\urushi_dev`, remote Lua eval) are **off by default**.
  Developers enable them in `urushi_settings.ini`: `[dev] runner=1`, `[dev] pipe=1`.
* Cloud library & Official pack: nothing is downloaded automatically; every file is verified against the SHA-256 hash in `catalog.json` (community scripts) or `scripts/manifest.json` (official pack) before it is atomically installed. See [CLOUD_SCRIPTS.md](CLOUD_SCRIPTS.md).

## Known residual risks (honest list)
* `memory.read_*` can read process memory (read-only, SEH-guarded). Without network/file exfiltration this stays inside the process.
* Scripts can draw arbitrary UI (e.g. a fake prompt). The Scripts list shows what is loaded; only enable scripts you trust.
* Hot-path hostile CPU use is bounded by the time budget, memory use is not capped.
* Isolated scripts share the `urushi_data/` disk quota (64 MB).
* If a script finds a Lua VM bug (e.g. in the bundled Lua 5.4) it could escape; keep `third_party/lua` current.

## For maintainers: reviewing a script for the `verified` branch
Reject on sight: attempts to obfuscate (`load` of strings/`string.char` chains), huge minified blobs, anything that tries to reach
`_G`, `getmetatable`, `debug`, builds paths with `..`, or touches `memory.*` without a gameplay reason.

## Tests
The sandbox is covered by an attack suite (76 checks: removed APIs, bytecode, path/ADS/device/UNC/traversal tricks, isolation)
that was run against `OpenSafeLibs` and the `make_env` function taken verbatim from `scripts/main.lua`.
