# URUSHI

[![Discord](https://img.shields.io/badge/Discord-URUSHI-5865F2?logo=discord&logoColor=white)](https://discord.gg/jS6cc49EbT)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

[English](#features-v020) · [Русский](#-urushi-ru)

Open-source **C++ / Lua 5.4 / DirectX 11** modding & telemetry framework for **CarX Drift Racing Online 2**.

> **License:** Distributed under the **MIT License** — see [`LICENSE`](LICENSE). Free for personal and commercial use, with required attribution to original author.

---

## Features (v0.2.0)

- **Steering Assist (`155°` Fix):**
  - Raises the 180° vector-flip threshold to `155°` (disables premature `90°` auto-flip and `140°` clamping — deep backwards entries and ~`90°` angles hold cleanly without twitching, while spins/360s above `155°` snap through smoothly).
  - Removes the asymmetric `±15.6°` front-slip lockup in `NativeCarX Assist2`.
  - Live preset toggle (`155° Backwards` / `Stock DR2`), threshold slider, and live steering/slip telemetry.
- **CarFX Shaders (DX11 HLSL Engine):**
  - Replace or overlay custom pixel shaders strictly on the player car body (`MAT_CAR_PAINT_3_0` Forward pass) with instant hot-reload.
  - Bundled shaders in [`shaders/`](shaders/): `livery_wall_tap.hlsl`, `livery_proximity_reactive.hlsl`, `livery_tandem_angle.hlsl`, `livery_cyber_pulse.hlsl`, `livery_aurora_fluid.hlsl`, `livery_glass.hlsl`, `livery_chrome.hlsl`, `livery_holo.hlsl`, `livery_refract.hlsl`, `livery_stealth.hlsl`, `livery_glow_overlay.hlsl`.
  - Auto-generated UI directly from HLSL comments (`@param`, `@color` with SV/Hue/Hex/Alpha color pickers, `@tex`, `@mode`, `@stealth`), plus real-time wall/tandem/drift sensors.
- **Multiplayer Optimizer:**
  - Keeps only the nearest `Top-N` remote cars in full physics simulation and switches distant cars to smooth server snapshots with hysteresis, bone-sleep tuning, and LOD controls.
- **3D Replays, Dummy, Teleport & Anti-Wrong-Way:**
  - Record and replay multi-car `.cxr2` runs with ghost collision (`K`), playback speed, lateral offset, and loop controls.
  - Clone a live 3D dummy of your car (`G` / `H`) with real-time mirror mode (`M`).
  - Save/restore multi-slot track checkpoints with velocity preservation (`[` / `]`).
  - Automatic wrong-way detection, selective collision ghosting (`J`), and on-screen directional radar.
- **URUSHI ImGui UI & Updater:**
  - Bilingual interface (**English / Русский**) auto-detected from Windows locale with instant switcher in **System**.
  - Theme accents (`Amber`, `Crimson`, `Silver`, `Indigo`, plus **Custom** RGB/Hue picker), DPI/manual UI scaling, pin-to-overlay HUD windows (**Telemetry HUD**, **FPS & Network**), Lua 5.4 hot-reload (`scripts/*.lua`), and built-in GitHub auto-updater (`dist/`).

---

## Quick Install

1. Copy from this repository into your **CarX Drift Racing Online 2** root directory (next to `CarX Drift Racing Online 2.exe`):
   - `dist/version.dll`
   - `dist/urushi.dll`
   - `scripts/`
   - `shaders/`
2. Launch the game via Steam and press **`F4`** or **`Insert`** to open URUSHI.

---

## Hotkeys

| Key | Action |
| :--- | :--- |
| **F4** / **Insert** | Toggle URUSHI main dock |
| **F5** | Toggle Optimizer window |
| **F6** | Start / stop `.cxr2` replay recording |
| **F7** | Start / stop replay ghost playback |
| **F8** | Toggle 3D Dummy & Scene window |
| **F9** | Toggle CarFX Shaders window |
| **G** / **H** | Spawn / remove 3D dummy car |
| **M** | Toggle Live Mirror (`+3.8 m`) |
| **J** | Toggle Anti-Wrong-Way ghosting |
| **K** | Toggle 3D ghost / dummy collision |
| **[** / **]** | Save checkpoint / Teleport |
| **Ctrl + F11** | Hot-reload `urushi.dll` |

---

## Custom CarFX Shaders

Place any `.hlsl` file inside `shaders/` starting with `#include "carfx.hlsli"`. UI widgets are automatically built from header annotations:

```hlsl
// @mode replace
// @param glowPower  p0.w 0.2..6.0 = 3.0  : HDR proximity glow intensity
// @color accentCol  p1.xyz = 1.0 0.34 0.03 : Accent neon colour
#include "carfx.hlsli"
```

---

## Build from Source

Requires **CMake 3.20+** and **MSVC x64** (Visual Studio 2022 Build Tools):

```powershell
git clone https://github.com/Garagoro/Urushi.git
cd Urushi
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Deploy into your CarX Drift Racing Online 2 folder
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

---

<a id="-urushi-ru"></a>
## URUSHI (Русский)

Открытый **C++ / Lua 5.4 / DirectX 11** мод-фреймворк для **CarX Drift Racing Online 2**.

### Основные возможности
- **Помощник руля (`155°` Fix):** перенос порога переворота вектора на `155°` (глубокие бакварды и углы около `90°` держатся стабильно, `360°`/развороты свыше `155°` докручиваются сразу) и снятие асимметричного клина передних колёс `±15.6°` в `NativeCarX Assist2`. Пресеты `155° Баквард` / `Сток DR2` и живая телеметрия.
- **CarFX Шейдеры:** кастомные DX11 HLSL шейдеры для кузова игрока (`Replace` / `Overlay`, hot-reload), реактивные шейдеры (`livery_wall_tap.hlsl`, `livery_proximity_reactive.hlsl`, `livery_tandem_angle.hlsl` и др.), авто-интерфейс из `@param`, `@color` (с квадратным SV/Hue/Hex/Alpha колорпикером) и `@tex`.
- **Оптимизатор FPS:** ограничение полной физики мультиплеера до `Top-N` ближайших машин с переключением остальных на серверные снапшоты.
- **Реплеи 3D, Болванка, Телепорт и Анти-встречка:** запись/воспроизведение `.cxr2` реплеев (`F6`/`F7`), 3D-клон машины (`G`/`H`) с зеркалом (`M`) и коллизией (`K`), точки телепорта (`[`/`]`), анти-встречка с радаром (`J`).
- **Интерфейс:** выбор языка **Русский / English** (по умолчанию подхватывается язык Windows), 5 цветовых тем (включая свой RGB/Hue-акцент), масштаб UI, закрепление HUD (`Телеметрия HUD`, `FPS и Сеть`) и авто-обновление с GitHub (`dist/`).

### Быстрая установка
1. Скопируйте в папку с игрой (`CarX Drift Racing Online 2`): `dist/version.dll`, `dist/urushi.dll`, папки `scripts/` и `shaders/`.
2. Запустите игру. Меню открывается на **`F4`** или **`Insert`** (горячие клавиши те же, что в таблице выше).

---

## License & Third-Party Notices

- **URUSHI**: [MIT License](LICENSE)
- **Dear ImGui** (`third_party/imgui`): MIT License
- **Lua 5.4** (`third_party/lua`): MIT License
- **MinHook** (`third_party/minhook`): BSD 2-Clause License

---

## Credits & Acknowledgments

- **Lead Developer:** [Garagoro](https://github.com/Garagoro)
- **Pair Programming & Automation:** [Antigravity](https://deepmind.google/technologies/gemini/) (Google DeepMind)

