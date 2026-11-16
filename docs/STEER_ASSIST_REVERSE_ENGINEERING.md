# Реверс-инжиниринг помощника руления для клавиатуры (`SteeringMode.Buttons`)

Документ фиксирует результаты анализа цепочки обработки руля с клавиатуры в **CarX Drift Racing Online 2** (`GameAssembly.dll` + `NativeCarX.dll`, vanilla).

---

## 1. Цепочка в `GameAssembly.dll` (C#)

### Чтение ввода и запись в модель
- `InputSystemCarController::ReadInputHandler` (`GA+0x14E71E0`):
  - Читает `m_steeringAction` (`[rbx+0x20]`) через `ReadValue<float>()` (`GA+0x25BA980`).
  - Записывает значение без C#-сглаживания и без кривых напрямую в `CarControlModel.m_steering` (`+0x30`, `GA+0x1421310`) — в отличие от газа/тормоза, где есть гистерезис (`0x14E72D2..0x14E7307`).

### Режим руления и флаг `m_isButtonSteerInput`
- `CarControlController::UpdateButtonSteerInput` (`GA+0x14C6DA0`, вызывается из `0x14C39EB` и `SteeringModeParameterUpdateHandler` `0x14C69E5`):
  - Определяет `SteeringMode` (`GA+0x1905210`, `SteeringMode.Buttons == 2`).
  - Если `effectiveAssistantMode == 1`, запрашивает `InputSettingsAdapter.GetSteeringModeFloat(mode, 5)` (`UseAlternativeSteering`).
  - Ставит `m_isButtonSteerInput` (`[rbx+0xA0]`) = `(mode == 2) ? !UseAlternativeSteering : false`.
- Значения по умолчанию (`CreateDefaultSettingsHolder`, `GA+0x1495CD0` и `buttons.settings`):
  - `1 = 1.0`
  - `2 = 1.0` (`AssistantMode`)
  - `3 = 1.0`
  - `5 = 0.0` (`UseAlternativeSteering`)
  - `6 = 1.0`
  - `7 = 1.0`
- `InputSimulatorSettingsManager::ApplyStaticParameters` (`GA+0x1824590`):
  - Если `mode == 2 && assistantMode == 1 && UseAlternativeSteering(5) > 0`, режим принудительно подменяется на `3` (`GamePad`).
  - Иначе `Sub_17DA560` маппит: `2 -> "keyboard"`, `!2 -> "gamepad"`, `4 -> "steering_wheel"`.
- `InputSimulatorSettingsManager::ApplyAssistSlipAngle` (`GA+0x1823C20`):
  - Передаёт параметр `0x74` (`116` -> `NCX+0x6C0FE0`).

### Передача в `NativeCarX.dll`
- `CarControlController` (`0x14C3990 -> 0x14C4320`):
  - Для `mode != 4` вызывает `CX_CarControllerSetSteerFFmode` (`GA+0x21A4670`) с `edx = (sbyte)effectiveAssistantMode` (`1`). В рантайме `[player+0x5BC] == 1`.
- `CarControlController::GetStabilizedSteeringAndAssisting` (`GA+0x14C4D90`) и `ApplySteering` (`GA+0x14C4750`):
  - Инвертирует знак `m_steering`, читает `maxSteerRad = [rcx+0x38]->[+0x60]->[+0x17C] * 0.0174533` (`GA+0x2187980`) и вызывает `CX_CarSteerAssistent2` (`GA+0x21AC340`).

---

## 2. Логика в `NativeCarX.dll`

### Запись входного состояния (`CX_CarSteerAssistent2`, `NCX+0x4A50`)
- `[car+0x4E8] = isBtn` (`1` для клавиатуры)
- `[car+0x4EC] = assisting` (`1`)
- `[car+0x4F0] = abs` (`0`)
- `[car+0x4F4] = steer`
- `[car+0x838] = maxSteerRad`
- `[car+0x5F0] = 1`

### Диспетчер (`Sub_27140` `NCX+0x27140` -> `SteerDispatch_87A0` `NCX+0x87A0`)
- `0x87B0..0x87B6` (`cmp edx, 1; cmovne r11d, r8d`):
  - Для клавиатуры (`edx == 1`) выставляет `r11d = 0`.
  - Для геймпада выставляет `r11d = 1`.
- `0x87BA..0x87C3` проверяет `[r10+0xD4]` (`car+0x5BC`, `steerFFMode`):
  - При `steerFFMode == 1` (`1 > 0`) переходит по `0x880B` напрямую в `SteerAssistMain_8DA0` (`NCX+0x8DA0`) с `esi = 0` (`Sub_9450` используется только при `steerFFMode == 0 && [0x6C0204] != ±30`).

### Основной солвер (`SteerAssistMain_8DA0`, `NCX+0x8DA0`)
1. **Средний боковой увод передних колёс (`avg_slip`, `xmm10`)**:
   - Для передних колёс берётся `wheel+0xF6C * 57.2958 / [0x6C0FDC]` (`12.0`), взвешенный по вертикальной нагрузке `[+0x1F4]`.
2. **Угол заноса (`SlipSolver_9DD0`, `NCX+0x9DD0`)**:
   - Если `|beta| > 90°` (`1.5708`, `0xA5C5`) и `|steer| > 0.99` (`0xA5E5`), выставляется ранний флаг разворота `bl = 1`.
   - Если `|beta| > 140°` (`2.44346`, `0xA5F3`), безусловный flip.
   - Выход: `car_slip_factor`.
3. **Центрирование (`0x9085..0x910A`)**:
   - `speed_gate = clamp(speed * 0.1 - 0.5, 0, 1)`
   - `center = clamp((avg_front_steer - car_slip_factor * 0.3) * (-40.3), -15, 15) * speed_gate`
4. **Накопление пользовательского смещения (`bias`, `[car+0x5E0]`)**:
   - `step = dt * 250.0 * [0x6C0FF4]`
   - `max_bias = [0x6C0FEC] * 60.0` (`0.6 * 60 = 36.0`)
   - Для **клавиатуры (`esi == 0`, `0x910B..0x91AD`)**:
     - `steer == +1`: сбрасывает `bias < 0 -> 0`, наращивает `+step`.
     - `steer == -1`: сбрасывает `bias > -0 -> -0`, уменьшает `-step`.
     - `steer == 0`: **мгновенно сбрасывает `bias = 0` за 1 тик**.
   - Для **геймпада (`esi == 1`, `0x91B2..0x923A`)**:
     - При `|avg_slip| > 0.8` дополнительно ослабляет `bias` множителем `max((1.3 - |avg_slip|) * 2.0, 0)`. Клавиатура этот ослабитель пропускает.
5. **Ограничитель по уводу (`0x92AC..0x9305`)**:
   - `corr = (center * [0x6C0FF0] + bias) * 0.02`
   - Прибавляется к `xmm9` (`0.6 * [car+0xB0C]`) **только** при выполнении:
     - `avg_slip > -1.3 && steer_vel < 2.0 && corr > 0`, либо
     - `avg_slip < +1.3 && steer_vel > -2.0 && corr < 0`.
6. **Интегратор рулевой рейки (`0x9306..0x9400 -> ApplyRackSteer_333B0`, `NCX+0x333B0`)**:
   - `inv_inertia = 1.35 / [0x6C0FE8]`
   - `steer_vel += (xmm9 * 50.0) * dt * inv_inertia`
   - `steer_out += (0.8849558 * [rbx+0xD8]) * steer_vel * dt`

---

## 3. Проверенные глобалы `NativeCarX.dll` (live)

| Смещение | Значение | Назначение (из анализа) |
|---|---|---|
| `+0x6C0204` | `1` | Режим/флаг диспетчера (`Sub_9450` vs `SteerAssistMain_8DA0`) |
| `+0x6C0208` | `0` | — |
| `+0x6C020C` | `1` | — |
| `+0x6C0230` | `1` | — |
| `+0x6C0EE0` | `3` | — |
| `+0x6C0F38` | `1` | — |
| `+0x6C0FAC` | `0` | — |
| `+0x6C0FD0` | `0.35` | — |
| `+0x6C0FD4` | `0.3` | — |
| `+0x6C0FD8` | `8.25` | — |
| `+0x6C0FDC` | `12` | Нормировка переднего увода (град) |
| `+0x6C0FE0` | `12` | `ApplyAssistSlipAngle` (параметр `0x74` / `116`) |
| `+0x6C0FE8` | `1` | Делитель инерции рулевой рейки (`1.35 / [0x6C0FE8]`) |
| `+0x6C0FEC` | `0.6` | Множитель `max_bias` (`* 60.0 = 36.0`) |
| `+0x6C0FF0` | `1` | Вес `center` в `corr` |
| `+0x6C0FF4` | `1` | Множитель скорости набора `bias` (`dt * 250.0 * [0x6C0FF4]`) |

---

## 4. Практическая заметка
Если помощник на клавиатуре ощущается «мерзким» (дёрганым / слишком агрессивным), в настройках игры стоит проверить переключатель альтернативного помощника (`UseAlternativeSteering`, параметр `5`), который переключает клавиатурный путь в ветку геймпадовского ассиста (`SteeringMode::GamePad`), либо при дальнейшей доработке корректировать накопление/сброс `bias` (`0x910B..0x91AD`) и параметры `+0x6C0FE8..+0x6C0FF4`.
