# План разработки: Replay & Ghost Mod для CarX Drift Racing Online 2

## 1. Архитектурный контекст CarX2

В ходе реверс-инжиниринга выяснилось, что игра разделена на три слоя, и **в ней уже существует полноценный нативный движок призраков и реплеев**:

1. **Слой чистой физики (`NativeCarX.dll`)**:
   - Написан на чистом C++ (unmanaged).
   - Отвечает исключительно за математику: сцепление шин, дифференциал, обороты, физику твердого тела.
   - `CX_CreateCar` создаёт расчётный контекст машины.

2. **Слой симуляции и траекторий Unity DOTS ECS (`lib_burst_generated.dll`)**:
   - Симуляция и расчёт кинематики подвески/колёс выполняется скомпилированным Burst-кодом.
   - Именно Burst-система вызывает `CX_CarSetRenderMatrix` каждый кадр по RVA `0x326094`.
   - Здесь работает встроенная система `SyncTimelineGhostCarTransformSystem` и `GhostDots`.

3. **Слой графики и игрового мира (`GameAssembly.dll` + `UnityPlayer.dll`)**:
   - Unity IL2CPP движок.
   - В игре **уже есть полноценный встроенный движок призраков и реплеев**:
     - `FreerideGhostsBehaviourController` (активен прямо сейчас на карте фрирайда).
     - `GhostDots`, `GhostCommon`, `RecordableDotsRaceCar`, `CARXCar`.
     - `GhostPlaybackInfo` (время воспроизведения `currentTime`, общая длина заезда `totalDuration`).
     - `DebugScreens.GhostRecorder.DebugGhostRecorderScreen` (служебный экран записи/воспроизведения разработчиков с кнопками `Record`, `Replay`, `OnReset`, `m_loadedGhost`, `m_ghostToSave`).
     - База из 865 записанных заездов в `StreamingAssets/aa/StandaloneWindows64/ghosts_assets_all_*.bundle` с форматом `GhostVersion` + JSON тюнинга + `CarVisualData` (`{"parent_slot":""}`).

---

## 2. Поэтапный план реализации

### Этап 1. Визуальная 3D-болванка на Unity Main Thread [ГОТОВО]
- **Решение**: Вместо небезопасного клонирования из рендер-потока установлен перехват `UnityPlayer.dll!ExecutePlayerLoop` (`+0x5AC580`) на главном потоке Unity (`events.on_main_update`).
- **Реализовано**:
  1. Автономный IPC-мост `runner.lua` v3 и C++-сканер/инжектор без крашей.
  2. Поиск живого `Model` машины игрока на трассе и безопасное клонирование через `UnityEngine.Object.Internal_CloneSingle` (`Model(Clone)`, без физики).
  3. Отключение `LiveryCollider` и перевод всей иерархии в слой `2` (`Ignore Raycast`), чтобы не сталкиваться с игроком.
  4. Хоткеи `G` (создать болванку) и `H` (удалить болванку) + панель в `ghost_test.lua` `[F8]`.

---

### Этап 2. Анимация колес, суппортов, подвески и руля [РЕАЛИЗОВАНО, НА ПРОВЕРКЕ]
- **Цель**: Сделать так, чтобы болванка полностью повторяла работу подвески, кастер/шкворень, вращение ступиц/колес и угол поворота руля.
- **Реализовано**:
  1. Верифицированы column-major матрицы `NativeCarX.dll` (`wheelMatrix` для `Wheel*_hub` и `caliperMatrix` из `CX_WheelGetCaliperMatrix` для некрутящихся кулаков/суппортов `Wheel*_root` и костей `FL_Wheel..RR_Wheel`, с fallback через `ReconstructSpinFreeKnuckle`).
  2. Кеширование узлов рига болванки в `UnityBridge::ConfigureClonedGhost` + локальный Y-поворот `SteeringWheel_*` (`~0.78 * steerAngle`).
  3. Нативный C++ режим **Живого Зеркала (`[M]` / slider смещения)** в `UnityBridge::SetGhostMirrorMode` для визуальной проверки всей кинематики рядом с машиной игрока.

---

### Этап 3. Запись заезда (Telemetry Recorder v3 — Multi-Car Player List) [РЕАЛИЗОВАНО]
- **Цель**: Записывать полный заезд игрока и других машин на трассе (Player List) в бинарный формат `.cxr2`.
- **Реализовано**:
  1. Формат `.cxr2` `version = 3` с поддержкой нескольких независимых треков (`ReplayTrackHeader` + `ReplayFrame[]` с `caliperMatrices[4][16]` на каждую машину) и обратной совместимостью `v1`/`v2`.
  2. Кешированное сопоставление `NativeCarX::GetAllCarsTelemetry()` <-> корни машин Unity (`Kyoto S90_0`, `BNM M92_1` и др.) через `UnityBridge::GetLiveCarList()`.
  3. Во вкладке `[REC]` (`replay_mod.lua`) добавлен **Player List** с чекбоксами, расстоянием, скоростью и кнопками выбора машин перед записью `[F6]`.

---

### Этап 4. Воспроизведение 3D-Призраков и коллизия [РЕАЛИЗОВАНО, НА ПРОВЕРКЕ]
- **Цель**: Плавное воспроизведение 1 или нескольких записанных машин на 3D-болванках с опциональной физической коллизией.
- **Реализовано**:
  1. Математическая интерполяция кадров в `ReplayEngine::GetInterpolatedTrackFrame`: Quaternion SLERP + LERP координат.
  2. Пул слотов `GhostSlot` в `UnityBridge`: каждая машина из реплея клонирует подходящую модель (`Model(Clone)`) со своими колёсами, суппортами и рулём (`replay.apply_all_to_ghosts`).
  3. Переключатель **Коллизии призраков (`[K]` / чекбокс в `[PLAY]`)**: переключает слои и `LiveryCollider` + решает OBB-столкновения с `NativeCarX::GetCarRigidBody` / `SetCarRigidBody` (отскок, трение боками и угловой импульс).

---

### Этап 5. Полировка и визуальные эффекты [СЛЕДУЮЩИЙ]
- **Задачи**:
  1. Свет и стоп-сигналы (`TimelineGhostCarLightsBehaviour` / эмиттеры фар и тормоза по `brake`/`gear`).
  2. Дым из-под шин по записанному каналу `wheelSmoke[4]` и `ParticleSystem[]`.
  3. Поддержка полупрозрачного ghost-шейдера и гоночных камер (`ReplayCameras`, `ReplayTvCameraController`).
  4. Анти-встречка (селективная прозрачность/отключение коллизии для игроков, едущих навстречу).

---

### Этап 6. `CarFX` (`CarX-WFX`): кастомные HLSL-шейдеры на кузов и ливрею игрока [РЕАЛИЗОВАНО, НА ПРОВЕРКЕ]
- **Техническая база**: см. [`CARX_RENDERING.md`](CARX_RENDERING.md) — в CarX DR2 ливрея не рисуется отдельным мешем, а запекается `VinylBaker`'ом в 2048×2048 атласы `u_sTexture` / `u_sPackedMap` и рендерится одним `Forward`-проходом `RenderCore/Standard` (`MAT_CAR_PAINT_3_0`, `SV_Target0`).
- **Подэтап 6.1 — Резолвер ливреи и перехват `DrawIndexed*` [ГОТОВО]**:
  1. На Unity Main Thread (`UnityBridge`) через `UpdateCarFXPlayerTexturesOnMainThread` извлекаются нативные `ID3D11ShaderResourceView*` текстур `u_sTexture` (`0x17C`) и `u_sPackedMap` (`0x55A`) локальной машины игрока (`BodyTypeID = 0x8C255AA8`) через `GetNativeTexturePtr_Injected`.
  2. В `D3D11Hook` добавлены хуки `ID3D11DeviceContext::DrawIndexed` (`vtbl[12]`) и `DrawIndexedInstanced` (`vtbl[20]`) через `HookManager` с recursion `Guard` и авто-дохукиванием Win11 runtime vtable switching в `Hooked_Present`.
  3. Детекция draw-вызовов кузова игрока по паре SRV `(srvAlbedo, srvPacked)` в слотах `t12..t31` + проверке цветного RenderTarget (+ диагностика в ImGui/лог).
- **Подэтап 6.2 — Шейдерное ядро в стиле `WeaponFX` и hot-reload [ГОТОВО]**:
  1. Реализовано ядро `CarFX` (`carfx_shader_decl.*`, `carfx.*`) с компиляцией HLSL через `d3dcompiler_47.dll` (`D3DCompile` + `D3DReflect`), парсингом `@param` / `@color` / `@tex` / `@mode replace|overlay`, выделенными сэмплерами `s14`/`s15` и безопасным отложенным освобождением COM-ресурсов.
  2. Создан `shaders/carfx.hlsli` с точным `PSIn` под `MAT_CAR_PAINT_3_0`, `cbuffer CarFx : register(b13)` (`g_fxTime`, `g_fxScreen`, `g_fxParam[8]`, `g_fxGame[4]`), `g_fxLivery` (`t38`), `g_fxTex0..3` (`t33..t36`), `g_fxScene` (`t32`) и `g_fxDepth` (`t37`), с поддержкой автоперезагрузки (`PollHotReload`) каждые 250 мс.
  3. Реализованы режимы `replace` и `overlay` (`ONE / INV_SRC_ALPHA`, `DepthFunc = LESS_EQUAL`, `DepthWrite = ZERO`) + захват сцены и глубины.
  4. Дальнейшее расширение (при необходимости): фоллбэк-маркер для стоковых машин без наклеек (если `whiteTexture`/`blackTexture`) и второй таргет `CarGlass` (`0xD9F8C9BF`).
- **Подэтап 6.3 — Интеграция в Lua/ImGui и пресеты [ГОТОВО]**:
  1. Lua-модуль `carfx.*` (`core/bindings/lua_carfx.*`) + автодеплой `shaders/` в `CMakeLists.txt` и `install.ps1`.
  2. Панель в меню (`scripts/carfx_mod.lua`, хоткей `[F9]`) со списком шейдеров, слайдерами `@param` / `@color`, загрузкой `@tex` (через WIC), телеметрией машины в `g_fxGame[4]` и стартовыми пресетами (`livery_glow_overlay.hlsl`, `livery_chrome.hlsl`, `livery_holo.hlsl`, `livery_refract.hlsl`).

---

### Этап 7. Задел на будущее: DLSS 5 Neural Rendering (DX11 → D3D12 NGX Bridge) [ИДЕЯ / R&D]
- **Контекст**: В `CarX Drift Racing Online 2` уже работает нативный DX11 DLSS через `NVUnityPlugin.dll` (`NVSDK_NGX_D3D11_CreateFeature` / `EvaluateFeature`), который получает чистые буферы `Color`, `Depth` и `MotionVectors` до отрисовки HUD. При этом нейрорендер DLSS 5 (`nvngx_dlssnr.dll`, NGX Feature 18 / `NVSDK_NGX_Feature_Reserved18`) исполняется на `D3D12` / `Vulkan` (и на RTX 4060 / Ada требует соответствующей версии библиотеки и тюнинга `WorkingScale`).
- **Лицензирование и правила игры**: Проприетарный бинарник `nvngx_dlssnr.dll` **не включается** в open-source репозиторий или релиз мода — код остаётся чистым MIT-мостом, а DLL пользователь кладёт в папку игры сам.
- **План реализации (когда будет готов тестовый бинарник)**:
  1. Ориентироваться на открытый протокол `DLSSNR.*` (`Color`, `Output`, `MVec`, `Depth`, `Style`, `Intensity`, `LocalToneStrength`, `LocalStructureStrength`, `UseAutoMask` из `NIGos/dlss5-bridge`, `jlrouzies-fr/DLSS5-Feeder`, `Dagherbou/OptiScaler_DLSSNR`), не таща ReShade.
  2. Перехватывать `NVSDK_NGX_D3D11_*` от `NVUnityPlugin.dll`, шарить текстуры через `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` в скрытый `ID3D12Device` и вызывать `NVSDK_NGX_D3D12` Feature 18 (либо обеспечить чистую совместимость `d3d11_hook.cpp` / `version.dll` с внешним `OptiScaler_DLSSNR`).
  3. Вынести переключатель и ползунки параметров в ImGui / Lua меню мода.
