# CarX2ScriptHook: Extensible Lua Scripting & Replay Mod for CarX Drift Racing Online 2

**CarX2ScriptHook** — это открытый (MIT) модульный фреймворк и Replay/Ghost мод для симулятора **CarX Drift Racing Online 2**. 

Фреймворк разделен на две части:
1. **Высокопроизводительное С++ Ядро (Core)** с внедрением через прокси `version.dll`, DirectX 11 оверлеем на базе Dear ImGui и перехватом физики/телеметрии через `NativeCarX.dll`.
2. **Скриптовый слой (Lua 5.4)** с **мгновенным Hot-Reload без перезапуска игры**. Любые изменения в файлах `.lua` вступают в силу на лету за 1 миллисекунду!

---

## Возможности

- **Replay & Ghost система**:
  - Запись заездов с частотой тиков физики (позиция, кватернион вращения кузова, 4 колеса, выворот, дым, обороты, свет).
  - Сжатое бинарное сохранение в формате `.cxr2` в папку `replays/`.
  - Воспроизведение на локалке со сглаженной интерполяцией (Lerp/Slerp).
  - Воспроизведение в качестве **локального призрака (Ghost) прямо во время нахождения на сетевом сервере** для тренировки парного дрифта со своей копией или реплеем друга.
  - Таймлайн со скраббером, регулировка скорости (0.25x - 2.0x), зацикливание (Loop) и обратный отсчет перед стартом.
- **Полноценный Lua API**:
  - В скрипты вынесена вся доступная телеметрия (`speed_kmh`, `rpm`, `gear`, `steer_angle`, `wheels`, `smoke`, `accel`, `brake`, `handbrake`, матрицы колес).
  - Управление позицией и матрицей рендера машин на лету.
- **Встроенный GUI (Dear ImGui)**:
  - Красивое современное темное меню с вкладками, кнопками, слайдерами и инпутами.
  - Управление оверлеем через горячую клавишу **F4** или **Insert**.
- **Живой Hot-Reload**:
  - `FileWatcher` отслеживает изменения в `scripts/*.lua`. Как только вы нажимаете Ctrl+S в VSCode или Блокноте, мод тут же перезапускает скрипты без вылета или перезапуска игры.
  - Клавиша **F9** позволяет принудительно пересобрать и перезагрузить само C++ ядро (`CarX2ScriptHook.dll`).

---

## Горячие клавиши

| Клавиша | Действие |
| :--- | :--- |
| **F4** / **Insert** | Открыть / закрыть меню Replay & Ghost мода |
| **F6** | Быстрый старт / остановка записи заезда |
| **F7** | Быстрый старт / остановка воспроизведения реплея |
| **F9** | Полная горячая перезагрузка DLL ядра (`FreeLibrary` / `LoadLibrary`) |

---

## Полная документация Lua API

### 1. Модуль `carx`
- `carx.get_player_car()` — возвращает таблицу с параметрами машины игрока:
  - `x`, `y`, `z` — мировые координаты
  - `speed_kmh`, `speed_mph` — текущая скорость
  - `rpm` — обороты двигателя
  - `gear` — передача (0 = R, 1..N = передачи)
  - `steer_angle` — текущий угол поворота колес (радианы)
  - `accel`, `brake`, `handbrake`, `clutch` — положение педалей (0.0 .. 1.0)
  - `matrix` — массив из 16 float (4x4 Transform Matrix)
  - `wheels` — массив из 4 колес:
    - `matrix` (матрица колеса 4x4)
    - `smoke` / `smoke_intensity` (интенсивность дыма)
    - `camber` (развал)
    - `toe_in` (схождение)
- `carx.get_all_cars()` — массив всех обнаруженных машин в лобби/сессии.
- `carx.set_car_render_matrix(car_ptr, matrix_table)` — прямое выставление матрицы рендера кузова машины.
- `carx.is_in_session()` — находится ли игрок в активном заезде.
- `carx.get_time()` — текущее точное время работы в секундах.
- `carx.get_delta_time()` — время предыдущего кадра (dt).

### 2. Модуль `replay`
- `replay.start_record([track_name], [car_name])` — начать запись.
- `replay.record_frame()` — записать текущее состояние машины.
- `replay.stop_record([filename])` — сохранить реплей в файл `.cxr2`.
- `replay.is_recording()` — статус записи.
- `replay.get_record_duration()` — длительность текущей записи в сек.
- `replay.get_record_frame_count()` — число записанных кадров.
- `replay.load(filename)` — загрузить реплей из папки `replays/`.
- `replay.unload()` — выгрузить текущий реплей.
- `replay.is_loaded()` — загружен ли реплей.
- `replay.get_duration()` — общая длительность загруженного реплея.
- `replay.get_frame_count()` — количество кадров загруженного реплея.
- `replay.get_frame(time)` — интерполированный кадр на отметке `time` со всеми матрицами кузова и колес.
- `replay.list_saved()` — список всех файлов реплеев в папке с метаданными.

### 3. Модуль `imgui`
- `imgui.begin_window(title, [open])` / `imgui.end_window()`
- `imgui.text(str)` / `imgui.text_colored(r, g, b, a, str)`
- `imgui.button(label, [w, h])`
- `imgui.checkbox(label, current_value)`
- `imgui.slider_float(label, current_val, min, max)`
- `imgui.slider_int(label, current_val, min, max)`
- `imgui.input_text(label, current_str)`
- `imgui.combo(label, current_idx, items_table)`
- `imgui.progress_bar(fraction, [text])`
- `imgui.same_line()` / `imgui.separator()` / `imgui.spacing()`
- `imgui.begin_tab_bar(id)` / `imgui.end_tab_bar()`
- `imgui.begin_tab_item(label)` / `imgui.end_tab_item()`
- `imgui.is_menu_open()` / `imgui.set_menu_open(bool)` / `imgui.toggle_menu()`

### 4. Модуль `events`
- `events.on_update(function(dt) ... end)` — вызывается каждый кадр рендера.
- `events.on_fixed_update(function(dt) ... end)` — вызывается с фиксированным шагом физики.
- `events.on_render(function() ... end)` — отрисовка интерфейса ImGui.
- `events.on_key(function(vkey, is_down) ... end)` — перехват клавиш клавиатуры.

### 5. Модуль `log`
- `log.info(str)` / `log.warn(str)` / `log.error(str)` — запись в `CarX2ScriptHook.log`.

---

## Сборка и установка

Проект собирается с помощью CMake и MSVC 2022 (x64):

```powershell
# Сборка проекта
cd C:\Users\garagoro\.gemini\antigravity\scratch\CarX2ScriptHook
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release

# Установка мода в папку игры
powershell -ExecutionPolicy Bypass -File ..\install.ps1
```

---

## Лицензия

Проект распространяется под свободной лицензией **MIT License**. См. файл [LICENSE](LICENSE).
