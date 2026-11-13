<div align="center">

# URUSHI

[![Discord](https://img.shields.io/badge/Discord-URUSHI-5865F2?logo=discord&logoColor=white)](https://discord.gg/jS6cc49EbT)
[![Discussions](https://img.shields.io/badge/GitHub-Discussions-orange?logo=github)](https://github.com/Garagoro/Urushi/discussions)
[![Stars](https://img.shields.io/github/stars/Garagoro/Urushi?style=flat&logo=github&color=gold)](https://github.com/Garagoro/Urushi/stargazers)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

<br/>

[English](#about-urushi) · [Русский](#о-проекте-urushi)

<br/>

Open-source **C++ / Lua 5.4 / DirectX 11** modding framework for **CarX Drift Racing Online 2**.

</div>

---

<a id="about-urushi"></a>
## About URUSHI

The primary goal of URUSHI was to build a clean, solid **Lua scripting foundation** for CarX DR2 with instant hot-reloading, a modern ImGui UI, and a real-time DirectX 11 shader pipeline (**CarFX**).

The repository includes several sample scripts in `scripts/` demonstrating telemetry HUDs, checkpoint teleports, physics overrides, and shader automation.

### Community & Maintenance
* ⭐ **Star the Project:** If you find URUSHI useful or enjoy using it, please drop a star on GitHub — it's the best motivation and really helps the project grow!
* **Discussions & Scripts:** Share custom Lua scripts, shaders, or ask questions in [GitHub Discussions](https://github.com/Garagoro/Urushi/discussions)!
* **Forks & PRs:** You are very welcome to fork the repository, experiment, and submit Pull Requests! New Lua mods, custom shaders, and fixes are always appreciated.
* **Maintenance:** I maintain this project minimally — ensuring it remains working with game updates. I do not promise frequent feature releases or a heavy roadmap, which is why the code is open and easy to extend.

---

## Documentation

* 📜 **[Lua Scripting API (docs/LUA_API.md)](docs/LUA_API.md)** — Complete reference for `carx`, `urushi`, `ui`, `input`, and telemetry bindings.
* 🎨 **[CarFX Shaders Guide (docs/SHADERS.md)](docs/SHADERS.md)** — Creating custom HLSL pixel shaders with automatic UI controls, wall proximity, and drift angle sensors.

---

## Bundled Features & Examples

* **Lua 5.4 Environment (`scripts/`):** Real-time telemetry, tire wear/temperature tuning, damage toggles, and live UI (`Ctrl + F11` hot-reload).
* **CarFX Shader Engine (`shaders/`):** Reactive liveries (wall tap, tandem proximity, neon drift angle glow, refractive glass, chameleon chrome).
* **Steering Assist (`155°` Fix):** Extends the vector-flip threshold to 155° for deep backwards entries without twitching.
* **Multiplayer Optimizer:** Limits full 60Hz physics calculations to nearby cars, boosting FPS in populated lobbies.
* **Tools & Utilities:** Checkpoint teleport, multi-car 3D replay recording (`.cxr2`), live mirror/dummy car, anti-wrong-way ghosting.

---

## Quick Install

1. Download the latest release from [Releases](https://github.com/Garagoro/Urushi/releases).
2. Copy files into your **CarX Drift Racing Online 2** game folder:
   * `dist/version.dll`
   * `dist/urushi.dll`
   * `scripts/`
   * `shaders/`
3. Launch the game via Steam and press **`F4`** or **`Insert`** to open the menu.

---

## Hotkeys

| Key | Action |
| :--- | :--- |
| **F4** / **Insert** | Toggle URUSHI main dock |
| **F5** | Toggle Optimizer window |
| **F6** / **F7** | Record / Play 3D `.cxr2` replay |
| **F8** | Toggle 3D Dummy & Scene window |
| **F9** | Toggle CarFX Shaders window |
| **G** / **H** | Spawn / Remove 3D dummy car |
| **M** | Toggle Live Mirror |
| **J** | Toggle Anti-Wrong-Way ghosting |
| **[** / **]** | Save checkpoint / Teleport |
| **Ctrl + F11** | Hot-reload all Lua scripts & DLL |

---

<a id="о-проекте-urushi"></a>
## О проекте URUSHI (Русский)

Главная цель URUSHI — создать надежную и гибкую **базу для Lua-скриптов** в CarX Drift Racing Online 2 с поддержкой горячей перезагрузки, удобного интерфейса на ImGui и инжектора шейдеров DirectX 11 (**CarFX**).

В репозитории уже есть примеры скриптов в папке `scripts/`: HUD телеметрии, телепорт по чекпоинтам, управление износом шин и физикой.

### Сообщество и поддержка
* ⭐ **Звёздочка на GitHub:** Если проект вам понравился или оказался полезен — буду искренне рад вашей звёздочке на GitHub! Это лучшая поддержка и мотивация для развития проекта.
* **Дискуссии и скрипты:** Делитесь своими Lua-скриптами, шейдерами или задавайте вопросы в [GitHub Discussions](https://github.com/Garagoro/Urushi/discussions)!
* **Форки и Pull Request'ы:** Буду рад форкам, идеям и PR! Если вы написали свой крутой скрипт или шейдер — присылайте.
* **Поддержка:** Мод поддерживается в минимальном режиме — чтобы оставаться в рабочем состоянии при обновлениях игры. Частых контентных апдейтов и жесткого роадмапа не обещаю, поэтому проект полностью открыт для сообщества.

Подробная документация:
* 📜 **[Документация Lua API](docs/LUA_API.md)**
* 🎨 **[Руководство по шейдерам CarFX](docs/SHADERS.md)**

---

## License & Credits

* **License:** [MIT License](LICENSE)
* **Lead Developer:** [Garagoro](https://github.com/Garagoro)
* **Pair Programming & Automation:** [Antigravity](https://deepmind.google/technologies/gemini/) (Google DeepMind)
* **Third-Party:** Dear ImGui (MIT), Lua 5.4 (MIT), MinHook (BSD-2)
