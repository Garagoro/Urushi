# URUSHI (`CarX2ScriptHook`)

Открытый С++ / Lua 5.4 / DirectX 11 мод-фреймворк для **CarX Drift Racing Online 2**.

> **Лицензия:** проект распространяется под **GNU General Public License v3.0 (GPL-3.0)** — см. файл [`LICENSE`](LICENSE). Любые производные работы и модификации при распространении должны также оставаться открытыми под GPL-3.0.

---

## Возможности (v0.2.0)

- **Помощник управления (Steering Assistant `155°` Fix):**
  - Переносит порог переворота вектора на `155°` (убирает преждевременный `90°` переброс и ранний `140°` клип — глубокие бакварды и углы около `90°` проходят без дерготни, а `360°`/развороты свыше `155°` сразу докручиваются).
  - Снимает асимметричную блокировку руля при сносе передних колёс `> 15.6°` (`NativeCarX Assist2`).
  - Настройка параметров, пресеты (`155°` / `Сток DR2`) и живая телеметрия руля прямо в меню.
- **CarFX (HLSL-шейдеры кузова и ливреи):**
  - Замена или оверлей кастомных DX11 Pixel Shaders только на машину игрока (`MAT_CAR_PAINT_3_0` Forward pass) с hot-reload.
  - Реактивные шейдеры (`livery_wall_tap.hlsl`, `livery_proximity_reactive.hlsl`, `livery_tandem_angle.hlsl`, `livery_cyber_pulse.hlsl`, `livery_aurora_fluid.hlsl`, `livery_glass.hlsl`, `livery_chrome.hlsl` и др.) с авто-генерацией UI из `@param` / `@color` / `@tex`.
- **Оптимизатор физики и FPS (Multiplayer):**
  - Переключение удалённых сетевых машин на серверные снапшоты вне Top-N ближайших с гистерезисом против мерцания, настройкой сна скелета деформации и LOD-политики.
- **3D Реплеи, Призрак, Телепорт и Анти-встречка:**
  - Запись и воспроизведение мульти-кар реплеев `.cxr2`, клонирование живой 3D-модели машины на Unity Main Thread, живое зеркало (`M`), опциональная коллизия (`K`), сохранение точек телепорта (`[` / `]`) и анти-встречка (`J`).
- **URUSHI UI & Авто-обновление:**
  - Масштабируемый интерфейс на Dear ImGui (`F4` / `Insert`), смена акцентов, Lua 5.4 с hot-reload (`scripts/*.lua`) и встроенный клиент авто-обновления через публичный GitHub API (`dist/`).

---

## Быстрая установка (для игроков)

1. Скопируйте из репозитория в корневую папку игры (`CarX Drift Racing Online 2`):
   - `dist/version.dll`
   - `dist/CarX2ScriptHook.dll`
   - папку `scripts/`
   - папку `shaders/`
2. Запустите игру. Меню URUSHI открывается клавишей **`F4`** или **`Insert`**.

---

## Горячие клавиши

| Клавиша | Действие |
| :--- | :--- |
| **F4** / **Insert** | Открыть / скрыть панель URUSHI |
| **F5** | Окно оптимизатора FPS |
| **F6** | Старт / стоп записи реплея |
| **F7** | Старт / стоп 3D-призрака реплея |
| **F8** | Окно 3D-болванки и сцены Unity |
| **F9** | Окно CarFX шейдеров |
| **G** / **H** | Создать / удалить 3D-болванку |
| **M** | Живое зеркало (`+3.8 м`) |
| **J** | Вкл / выкл Анти-встречку |
| **K** | Коллизия 3D-призраков и болванки |
| **[** / **]** | Запомнить точку / Телепорт |
| **Ctrl + F11** | Горячая перезагрузка `CarX2ScriptHook.dll` |

---

## Сборка из исходников

Требуется CMake 3.20+ и MSVC (Visual Studio 2022 Build Tools, x64):

```powershell
git clone https://github.com/Garagoro/Urushi.git
cd Urushi
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Копирование в папку игры
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

---

## Лицензия и сторонние компоненты

- **URUSHI (`CarX2ScriptHook`)**: [GNU General Public License v3.0](LICENSE)
- **Dear ImGui** (`third_party/imgui`): MIT License
- **Lua 5.4** (`third_party/lua`): MIT License
- **MinHook** (`third_party/minhook`): BSD 2-Clause License
